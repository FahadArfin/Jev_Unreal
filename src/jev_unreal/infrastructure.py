"""Opt-in local policy/lease enforcement and privacy-bounded persistent outcome evidence."""

import asyncio
from typing import TYPE_CHECKING, Annotated, Any

from mcp.server.fastmcp import FastMCP
from mcp.types import ToolAnnotations
from pydantic import Field

from .errors import JevError
from .jobs import NamedJobs
from .runtime_state import RuntimeStore
from .team_policy import (
    APPLY_ACTIONS,
    MUTATING_ACTIONS,
    PREVIEW_APPLY,
    TeamPolicy,
    read_runtime_config,
)

if TYPE_CHECKING:
    from .bridge import UnrealBridge
    from .config import Settings

Id = Annotated[str, Field(min_length=1, max_length=64, pattern=r"^[A-Za-z0-9_-]+$")]


class ProjectInfrastructure:
    def __init__(self, settings: "Settings"):
        self.settings = settings
        self.config = None
        self.store = None
        self.policy = None
        self.jobs = None
        self._active = False
        if settings.runtime_config_file:
            self.config = read_runtime_config(
                settings.runtime_config_file, settings.expected_project
            )
            self.policy = TeamPolicy(self.config)
            self.store = RuntimeStore(self.config)
            self.jobs = NamedJobs(self.config, self.store, self.policy.hash)

    def require(self):
        if self.store is None:
            raise JevError(
                "runtime_configuration_required",
                "Set JEV_RUNTIME_CONFIG to a reviewed "
                "local project policy to enable persistent receipts, leases and jobs.",
            )
        return self.store

    def status(self) -> dict:
        if self.config is None:
            return {
                "configured": False,
                "durable_receipts": False,
                "shared_leases": False,
                "scope": "Native identity, authentication and stale-plan checks remain active.",
            }
        return {
            "configured": True,
            "project_file": self.store.project,
            "policy_sha256": self.policy.hash,
            "allowed_actions": self.config.allowed_actions,
            "asset_roots": self.config.asset_roots,
            "durable_receipts": True,
            "retention_days": self.config.receipt_retention_days,
            "max_receipts": self.config.max_receipts,
            "lease": self.store.lease_status(),
            "jobs": self.jobs.definitions(),
            "scope": "Cooperating local clients sharing this state directory; not an OS sandbox "
            "or a replacement for native permissions and stale-scene checks.",
        }

    async def execute(self, action: str, params: dict, status: dict, dispatch):
        if self.config is None or action not in MUTATING_ACTIONS:
            return await dispatch(action, params)
        self.policy.check(action, params, status)
        session = status.get("session_id")
        if not isinstance(session, str) or not session:
            raise JevError("state_identity", "Team mutations require the exact editor session.")
        lease_id, acquired = self.store.acquire()
        receipt_id = None
        heartbeat = None
        self._active = True

        async def renew():
            while True:
                self.store.renew(lease_id)
                await asyncio.sleep(10)

        try:
            # Renew even a short manual lease before dispatching the bounded network operation.
            self.store.renew(lease_id)
            if action in APPLY_ACTIONS:
                self.store.consume_plan(
                    session, params.get("plan_id", ""), action, self.policy.hash
                )
            receipt_id = self.store.begin(action, params, status, self.policy.hash)
            heartbeat = asyncio.create_task(renew())
            request = asyncio.create_task(dispatch(action, params))
            try:
                done, _ = await asyncio.wait(
                    {request, heartbeat}, return_when=asyncio.FIRST_COMPLETED
                )
                if heartbeat in done:
                    await heartbeat
                result = await request
            finally:
                if not request.done():
                    request.cancel()
                await asyncio.gather(request, return_exceptions=True)
            if action in PREVIEW_APPLY:
                # Returned asset identities are checked too, before any apply authorization.
                self.policy.check_data(result, result=True)
                self.store.authorize_plan(
                    session, result.get("plan_id"), PREVIEW_APPLY[action], self.policy.hash
                )
            self.store.finish(receipt_id, "response_observed", result=result)
            return result
        except asyncio.CancelledError:
            if receipt_id:
                self.store.finish(receipt_id, "outcome_uncertain", error_code="client_cancelled")
            raise
        except JevError as exc:
            if receipt_id:
                # An HTTP failure, callback failure or lost lease cannot establish rollback.
                certain_rejections = {
                    "stale_plan",
                    "unknown_plan",
                    "expired_plan",
                    "wrong_project",
                    "forbidden",
                    "unauthorized",
                    "play_mode",
                    "plan_consumed",
                    "actor_locked",
                    "level_locked",
                    "target_not_allowed",
                }
                state = (
                    "rejection_observed" if exc.code in certain_rejections else "outcome_uncertain"
                )
                self.store.finish(receipt_id, state, error_code=exc.code)
            raise
        except BaseException:
            if receipt_id:
                self.store.finish(receipt_id, "outcome_uncertain", error_code="client_interrupted")
            raise
        finally:
            if heartbeat:
                heartbeat.cancel()
                await asyncio.gather(heartbeat, return_exceptions=True)
            self._active = False
            if acquired:
                self.store.release(lease_id)

    def release_lease(self, lease_id: str) -> dict:
        store = self.require()
        state = store.lease_status()
        if self._active or (state.get("owned_by_this_client") and state.get("kind") == "job"):
            raise JevError(
                "lease_busy", "Cancel or finish the active operation before releasing its lease."
            )
        return {"released": store.release(lease_id)}

    async def close(self):
        if self.jobs is not None:
            await self.jobs.close()
        if self.store is not None:
            state = self.store.lease_status()
            if state.get("owned_by_this_client"):
                self.store.release(state["lease_id"])


def register_infrastructure_tools(server: FastMCP, bridge: "UnrealBridge") -> None:
    runtime = bridge.infrastructure
    read = ToolAnnotations(readOnlyHint=True, destructiveHint=False, openWorldHint=False)
    local = ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=False)
    launch = ToolAnnotations(
        readOnlyHint=False, destructiveHint=True, idempotentHint=False, openWorldHint=False
    )

    def safe(function, *args):
        try:
            return {"ok": True, "result": function(*args)}
        except JevError as exc:
            return exc.as_dict()

    @server.tool(annotations=read, structured_output=True)
    def unreal_team_status() -> dict[str, Any]:
        """Read configured project policy, local retention limits, lease state and named jobs."""
        return safe(runtime.status)

    @server.tool(annotations=read, structured_output=True)
    def unreal_durable_receipts(
        limit: Annotated[int, Field(ge=1, le=100, strict=True)] = 20,
    ) -> dict[str, Any]:
        """Read bounded historical local receipts after reconnect/crash. Never retry from one."""
        return safe(lambda: runtime.require().list_receipts(limit))

    @server.tool(annotations=read, structured_output=True)
    def unreal_durable_receipt(receipt_id: Id) -> dict[str, Any]:
        """Read exact project/session/digests and observed or uncertain outcome, without replay."""
        return safe(lambda: runtime.require().get(receipt_id))

    @server.tool(annotations=local, structured_output=True)
    def unreal_durable_receipt_forget(receipt_id: Id) -> dict[str, Any]:
        """Delete one private local receipt. Does not change scene state or permit plan replay."""
        return safe(lambda: {"forgotten": runtime.require().forget(receipt_id)})

    @server.tool(annotations=local, structured_output=True)
    def unreal_project_lease(
        seconds: Annotated[int, Field(ge=1, le=300, strict=True)] = 120,
    ) -> dict[str, Any]:
        """Acquire/retain this client's cooperative project lease for a reviewed workflow."""

        def acquire():
            store = runtime.require()
            lease_id, _ = store.acquire(seconds, kind="manual")
            return {"lease_id": lease_id, **store.lease_status()}

        return safe(acquire)

    @server.tool(annotations=local, structured_output=True)
    def unreal_project_lease_release(lease_id: Id) -> dict[str, Any]:
        """Release only this client's idle lease; other owners and active jobs stay locked."""
        return safe(runtime.release_lease, lease_id)

    @server.tool(annotations=local, structured_output=True)
    def unreal_named_job_preview(
        name: Annotated[str, Field(pattern=r"^[a-z][a-z0-9_-]{0,47}$")],
    ) -> dict[str, Any]:
        """Review exact pinned project/engine, fixed Win64 command and budgets. Launches nothing."""

        def preview():
            runtime.require()
            return runtime.jobs.preview(name)

        return safe(preview)

    @server.tool(annotations=launch, structured_output=True)
    async def unreal_named_job_start(plan_id: Id) -> dict[str, Any]:
        """Consume a reviewed named build/cook/package plan once, outside the editor bridge.

        Runs trusted project build code. Requires separate user authorization, local config and
        pinned engine. No custom executable, flags, shell, automatic launch or automatic retry.
        """
        try:
            runtime.require()
            return {"ok": True, "result": await runtime.jobs.start(plan_id)}
        except JevError as exc:
            return exc.as_dict()

    @server.tool(annotations=read, structured_output=True)
    def unreal_named_job(job_id: Id) -> dict[str, Any]:
        """Read lifecycle, exit status, output digests and artifact evidence; no raw logs."""

        def status():
            runtime.require()
            return runtime.jobs.status(job_id)

        return safe(status)

    @server.tool(annotations=launch, structured_output=True)
    async def unreal_named_job_cancel(job_id: Id) -> dict[str, Any]:
        """Terminate this client's contained job process tree and retain its partial receipt."""
        try:
            runtime.require()
            return {"ok": True, "result": await runtime.jobs.cancel(job_id)}
        except JevError as exc:
            return exc.as_dict()
