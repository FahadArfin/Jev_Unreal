"""Durable, bounded recipe checkpoints. Interrupted execution is never replayed."""

import asyncio
import json
import math
import uuid
from typing import Annotated, Any

from mcp.server.fastmcp import FastMCP
from mcp.types import ToolAnnotations
from pydantic import Field, TypeAdapter

from .errors import JevError
from .layouts import (
    GridLayout,
    PreviewTracker,
    RoomLayout,
    StairLayout,
    compile_layout,
    verify_readback,
)
from .spatial import AlignRecipe, DistributeRecipe, GroundRecipe, SnapGridRecipe, compile_spatial
from .team_policy import digest, project_identity
from .verification import Check, SessionIdentity, verify
from .workflows import ExpectedState

Recipe = Annotated[
    AlignRecipe
    | DistributeRecipe
    | GroundRecipe
    | SnapGridRecipe
    | GridLayout
    | RoomLayout
    | StairLayout,
    Field(discriminator="kind"),
]
RECIPES = TypeAdapter(Recipe)
CHECKS = TypeAdapter(list[Check])
Id = Annotated[str, Field(pattern=r"^[a-f0-9]{32}$", strict=True)]
Hash = Annotated[str, Field(pattern=r"^[a-f0-9]{64}$", strict=True)]
SCOPE = "Local recipe history; no save, rollback, cloud, automatic resume or replay."


def identity(value: dict) -> dict:
    try:
        project = SessionIdentity.model_validate(
            {key: value[key] for key in ("project_file", "session_id", "world_path")}
        ).model_dump()
        state = ExpectedState.model_validate(
            {key: value[key] for key in ("session_id", "world_path", "revision")}
        ).model_dump()
        return {**project, **state}
    except (KeyError, ValueError, TypeError):
        raise JevError("state_identity", "Complete workflow editor identity is required.") from None


class WorkflowRuns:
    def __init__(self, bridge, previews: PreviewTracker):
        self.bridge, self.previews = bridge, previews
        self.lock = asyncio.Lock()

    def _store(self):
        store = self.bridge.infrastructure.require()
        with store.connection() as db:
            db.execute(
                "CREATE TABLE IF NOT EXISTS workflow_runs "
                "(id TEXT PRIMARY KEY, updated REAL NOT NULL, body TEXT NOT NULL)"
            )
            db.execute(
                "DELETE FROM workflow_runs WHERE updated < ?",
                (store.clock() - store.config.receipt_retention_days * 86400,),
            )
        return store

    def _get(self, run_id):
        store = self._store()
        with store.connection() as db:
            row = db.execute("SELECT body FROM workflow_runs WHERE id=?", (run_id,)).fetchone()
        if not row:
            raise JevError("workflow_missing", "Workflow checkpoint expired or is unknown.")
        try:
            value = json.loads(row[0])
            if value["project_file"] != store.project or value["run_id"] != run_id:
                raise ValueError
            return value
        except (ValueError, KeyError, TypeError):
            raise JevError("state_identity", "Workflow checkpoint identity is invalid.") from None

    def _save(self, row, *, expected_status=None, create=False):
        store = self._store()
        encoded = json.dumps(row, allow_nan=False, sort_keys=True)
        if len(encoded.encode()) > 65536:
            raise JevError("workflow_too_large", "Workflow checkpoint exceeds 64 KiB.")
        with store.connection() as db:
            if create:
                count = db.execute("SELECT COUNT(*) FROM workflow_runs").fetchone()[0]
                if count >= min(128, store.config.max_receipts):
                    raise JevError("workflow_capacity", "Workflow history capacity reached.")
                db.execute(
                    "INSERT INTO workflow_runs VALUES (?, ?, ?)",
                    (
                        row["run_id"],
                        store.clock(),
                        encoded,
                    ),
                )
            else:
                previous = db.execute(
                    "SELECT body FROM workflow_runs WHERE id=?", (row["run_id"],)
                ).fetchone()
                if not previous or (
                    expected_status is not None
                    and json.loads(previous[0])["status"] != expected_status
                ):
                    raise JevError("workflow_consumed", "Workflow changed or was already consumed.")
                db.execute(
                    "UPDATE workflow_runs SET body=?, updated=? WHERE id=?",
                    (
                        encoded,
                        store.clock(),
                        row["run_id"],
                    ),
                )

    async def _status(self):
        store = self._store()
        current = identity(await self.bridge.call("status"))
        if project_identity(current["project_file"]) != store.project:
            raise JevError("wrong_project", "Workflow requires the explicitly configured project.")
        return current

    @staticmethod
    def _public(row):
        return {
            key: value
            for key, value in row.items()
            if key
            not in {
                "operations",
                "checks",
                "recipe",
            }
        } | {"scope": SCOPE, "saved": False, "replay_allowed": False}

    def list(self, limit=20):
        store = self._store()
        if type(limit) is not int or not 1 <= limit <= 20:
            raise JevError("invalid_request", "Workflow list limit must be 1..20.")
        with store.connection() as db:
            rows = db.execute(
                "SELECT body FROM workflow_runs ORDER BY updated DESC LIMIT ?", (limit,)
            ).fetchall()
        return {"runs": [self._public(json.loads(row[0])) for row in rows], "scope": SCOPE}

    async def preview(self, recipe):
        recipe = RECIPES.validate_python(recipe)
        current = await self._status()
        spatial = hasattr(recipe, "actor_paths")
        if spatial:
            details = await self.bridge.call("actor_details", {"actor_paths": recipe.actor_paths})
            plan = compile_spatial(recipe, details)
            if identity(plan["measurement_state"]) != current:
                raise JevError("state_changed", "Actor measurements changed during inspection.")
        else:
            plan = compile_layout(recipe)
        state = {key: current[key] for key in ("session_id", "world_path", "revision")}
        row = {
            "run_id": uuid.uuid4().hex,
            "project_file": self._store().project,
            "identity": current,
            "recipe": recipe.model_dump(mode="json"),
            "operations": plan["operations"],
            "checks": plan.get("verification_checks", []),
            "recipe_kind": recipe.kind,
            "status": "preparing",
            "apply_attempted": False,
        }
        self._save(row, create=True)  # Refuse exhausted storage before creating a native plan.
        try:
            preview = await self.previews.preview(plan["operations"], expected_state=state)
            plan_id = preview.get("plan_id")
            expiry = preview.get("expires_in_seconds")
            if (
                not isinstance(plan_id, str)
                or not 1 <= len(plan_id) <= 64
                or type(expiry) not in (int, float)
                or not math.isfinite(expiry)
                or not 0 < expiry <= 120
            ):
                raise JevError("invalid_plan", "Native preview has invalid identity or expiry.")
            # Native preview can expand operation defaults; retain its digest, not raw scene data.
            if digest(preview.get("operations")) != digest(plan["operations"]):
                # Native preview expands some operations; the returned review is authoritative
                # for display, while application requirements remain caller-derived.
                row["native_review_sha256"] = digest(preview.get("operations"))
            row.update(
                plan_id=plan_id, expires_at=self._store().clock() + expiry, status="awaiting_review"
            )
            row["review_sha256"] = digest(
                {
                    "identity": current,
                    "plan_id": plan_id,
                    "operations": plan["operations"],
                    "checks": row["checks"],
                    "native_review": preview,
                }
            )
            self._save(row, expected_status="preparing")
        except BaseException:
            row["status"] = "preview_interrupted"
            self._save(row, expected_status="preparing")
            raise
        return {
            **self._public(row),
            "preview": preview,
            "requirements": row["checks"],
            "next_step": "Review the preview, then explicitly apply this run and review hash.",
        }

    async def apply(self, run_id, review_sha256):
        row = self._get(run_id)
        if (
            row["status"] != "awaiting_review"
            or row["apply_attempted"]
            or review_sha256 != row.get("review_sha256")
        ):
            raise JevError(
                "workflow_consumed", "Review this run's exact digest; never repeat apply."
            )
        current = await self._status()
        if current != row["identity"] or self._store().clock() >= row["expires_at"]:
            raise JevError("stale_plan", "Workflow changed or expired; create a fresh preview.")
        row.update(status="outcome_uncertain", apply_attempted=True)
        self._save(row, expected_status="awaiting_review")  # Atomic before the only dispatch.
        try:
            applied = await self.previews.apply(row["plan_id"])
            row["apply_response_sha256"] = digest(applied)
            if applied.get("applied") is not True:
                row["apply_observed"] = False
                self._save(row)
                return self._public(row)
            row["apply_observed"] = True
            if not row["checks"]:
                readback = verify_readback(row["operations"], applied.get("actors"))
                if readback["status"] != "passed":
                    row["status"] = "applied_unverified"
                    self._save(row)
                    return self._public(row)
                for operation, actor in zip(row["operations"], applied["actors"], strict=True):
                    row["checks"].extend(
                        [
                            {
                                "kind": "transform_equals",
                                "actor_path": actor["path"],
                                "expected_instance_id": actor["instance_id"],
                                **{k: operation[k] for k in ("location", "rotation", "scale")},
                            },
                            {
                                "kind": "label",
                                "actor_path": actor["path"],
                                "expected_instance_id": actor["instance_id"],
                                "expected": operation["label"],
                            },
                        ]
                    )
            row["status"] = "applied_unverified"
            self._save(row)  # Survive interruption before fresh verification.
            verification = await self._verify(row)
            row["status"] = (
                "verified"
                if verification["status"] == "passed"
                else "verification_failed"
                if verification["status"] == "failed"
                else "applied_unverified"
            )
            row["verification"] = {"status": verification["status"], "sha256": digest(verification)}
            self._save(row)
            return {**self._public(row), "verification": verification}
        except BaseException as exc:
            row["error_code"] = exc.code if isinstance(exc, JevError) else "client_interrupted"
            self._save(row)
            raise

    async def _verify(self, row):
        checks = CHECKS.validate_python(row["checks"])
        paths = list(dict.fromkeys(check.actor_path for check in checks))
        details = await self.bridge.call("actor_details", {"actor_paths": paths})
        result = verify(
            checks,
            details,
            expected_identity={
                k: row["identity"][k]
                for k in (
                    "project_file",
                    "session_id",
                    "world_path",
                )
            },
        )
        # Layout shape, label and transform expectations also use this same fresh native read.
        if row["recipe_kind"] in {"grid", "stairs", "room"}:
            readback = verify_readback(row["operations"], details.get("actors"))
            result["recipe_readback"] = readback
            if result["status"] == "passed" and readback["status"] != "passed":
                result["status"] = "failed"
        result["evidence"] = "Fresh exact-actor native inspection, separate from apply response."
        return result

    async def status(self, run_id, reconcile=False):
        row = self._get(run_id)
        if not reconcile:
            return self._public(row)
        current = await self._status()
        if any(current[k] != row["identity"][k] for k in ("session_id", "world_path")):
            return {**self._public(row), "reconciliation": "different_editor_session_or_world"}
        try:
            native = await self.bridge.call("plan_status", {"plan_id": row.get("plan_id", "")})
        except JevError as exc:
            return {**self._public(row), "reconciliation": exc.code}
        if (
            native.get("plan_id") != row.get("plan_id")
            or native.get("session_id") != row["identity"]["session_id"]
        ):
            raise JevError("state_identity", "Native history is not this workflow's plan/session.")
        outcome = native.get("status")
        observation = {"native_status": outcome, "native_evidence_sha256": digest(native)}
        if row["apply_attempted"] and outcome == "applied" and row["checks"]:
            verification = await self._verify(row)
            observation["fresh_verification"] = verification
        # Reconciliation is an observation, never resets dispatch eligibility or proves who applied.
        return {**self._public(row), "reconciliation": observation}

    def cancel(self, run_id):
        row = self._get(run_id)
        if row["status"] != "awaiting_review" or row["apply_attempted"]:
            raise JevError(
                "workflow_consumed", "Dispatched work cannot be cancelled or rolled back."
            )
        row["status"] = "cancelled"
        self._save(row, expected_status="awaiting_review")
        return {
            **self._public(row),
            "note": "Local run abandoned; native preview expires normally.",
        }


def register_workflow_run_tools(server: FastMCP, bridge, previews: PreviewTracker):
    workflows = WorkflowRuns(bridge, previews)
    local = ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=False)
    mutate = ToolAnnotations(readOnlyHint=False, destructiveHint=True, openWorldHint=False)
    read = ToolAnnotations(readOnlyHint=True, destructiveHint=False, openWorldHint=False)

    async def guarded(operation):
        try:
            async with workflows.lock:
                return {"ok": True, "result": await operation()}
        except JevError as exc:
            return exc.as_dict()

    @server.tool(annotations=local)
    async def unreal_workflow_run_preview(recipe: Recipe) -> dict[str, Any]:
        """Inspect and preview a spatial/layout recipe; retain a private durable review checkpoint.

        Requires reviewed JEV_RUNTIME_CONFIG. Returns the exact preview and approval digest.
        No apply, save or cloud; explicit apply and fresh verification are separate steps.
        """
        return await guarded(lambda: workflows.preview(recipe))

    @server.tool(annotations=mutate)
    async def unreal_workflow_run_apply(run_id: Id, review_sha256: Hash) -> dict[str, Any]:
        """Apply a reviewed recipe once and verify fresh requirements; never replay uncertainty."""
        return await guarded(lambda: workflows.apply(run_id, review_sha256))

    @server.tool(annotations=read)
    async def unreal_workflow_run(
        run_id: Id | None = None,
        reconcile: bool = False,
        limit: Annotated[int, Field(ge=1, le=20, strict=True)] = 20,
    ) -> dict[str, Any]:
        """List durable runs or read one; explicit reconciliation never applies/replays work."""

        async def operation():
            if run_id is None:
                if reconcile:
                    raise JevError("invalid_request", "Select a run before reconciling.")
                return workflows.list(limit)
            return await workflows.status(run_id, reconcile)

        return await guarded(operation)

    @server.tool(annotations=local)
    async def unreal_workflow_run_cancel(run_id: Id) -> dict[str, Any]:
        """Abandon an undispatched workflow. Never cancel native apply, undo or restore files."""

        async def operation():
            return workflows.cancel(run_id)

        return await guarded(operation)

    return workflows
