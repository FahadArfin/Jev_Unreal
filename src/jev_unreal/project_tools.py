"""Explicit, bounded bindings for native project inspection and selected validation."""

from typing import Annotated, Any, Literal

from mcp.server.fastmcp import FastMCP
from mcp.types import ToolAnnotations
from pydantic import Field

from .bridge import UnrealBridge
from .errors import JevError
from .workflows import ExpectedState

AssetPath = Annotated[str, Field(min_length=1, max_length=1024)]
JobId = Annotated[str, Field(min_length=1, max_length=64)]


def register_project_tools(server: FastMCP, bridge: UnrealBridge) -> None:
    read = ToolAnnotations(readOnlyHint=True, destructiveHint=False, openWorldHint=False)
    run = ToolAnnotations(
        readOnlyHint=False, destructiveHint=True, idempotentHint=False, openWorldHint=False
    )

    async def call(action: str, params: dict | None = None) -> dict[str, Any]:
        try:
            return {"ok": True, "result": await bridge.call(action, params)}
        except JevError as exc:
            return exc.as_dict()

    @server.tool(annotations=read)
    async def unreal_pending_plans(
        limit: Annotated[int, Field(ge=1, le=64, strict=True)] = 20,
    ) -> dict[str, Any]:
        """List native one-shot previews shared with Window > Jev Review; never apply them.

        Native receipts survive MCP reconnects while this editor stays open. They remain
        bounded session memory, expire, and are not evidence of the current scene.
        """
        return await call("pending_plans", {"limit": limit})

    @server.tool(annotations=read)
    async def unreal_blueprint_inspect(
        asset_path: AssetPath,
        graph_limit: Annotated[int, Field(ge=1, le=32, strict=True)] = 16,
        node_limit: Annotated[int, Field(ge=1, le=256, strict=True)] = 128,
        pin_limit: Annotated[int, Field(ge=1, le=1024, strict=True)] = 512,
    ) -> dict[str, Any]:
        """Read an already-open exact native Blueprint's graph IDs, pins, variables and diagnostics.

        No compile, implicit asset load or graph edit. Node/pin limits are totals across graphs;
        inspect truncation flags. Stored compiler messages may be stale; they are not a fresh
        successful compile. Open the chosen Blueprint in Unreal if asset_not_loaded is returned.
        """
        return await call(
            "blueprint_inspect",
            {
                "asset_path": asset_path,
                "graph_limit": graph_limit,
                "node_limit": node_limit,
                "pin_limit": pin_limit,
            },
        )

    @server.tool(annotations=read)
    async def unreal_asset_dependencies(
        asset_path: AssetPath,
        direction: Literal["dependencies", "referencers"] = "dependencies",
        category: Literal["package", "manage", "searchable_name"] = "package",
        offset: Annotated[int, Field(ge=0, le=100000, strict=True)] = 0,
        limit: Annotated[int, Field(ge=1, le=200, strict=True)] = 100,
    ) -> dict[str, Any]:
        """Inspect one-hop asset registry references with pagination; no recursive loads or edits.

        Registry dependencies are evidence of references, not proof that cooking or loading
        will succeed. Read truncation and registry-loading flags before drawing conclusions.
        """
        return await call(
            "asset_dependencies",
            {
                "asset_path": asset_path,
                "direction": direction,
                "category": category,
                "offset": offset,
                "limit": limit,
            },
        )

    @server.tool(annotations=read)
    async def unreal_asset_import_info(asset_path: AssetPath) -> dict[str, Any]:
        """Read registry import provenance with source basenames only; never inspect source files.

        Missing metadata is reported explicitly. This does not establish source availability,
        original DCC units, import quality or texture completeness.
        """
        return await call("asset_import_info", {"asset_path": asset_path})

    @server.tool(annotations=read)
    async def unreal_validation_rules() -> dict[str, Any]:
        """List named project-approved native Data Validation rules and their availability.

        Disabled unless explicitly configured in the project's [JevEditor.Validation] policy.
        Only loaded native validator classes are eligible; no Blueprint/Python rule discovery.
        """
        return await call("validation_rules")

    @server.tool(annotations=run)
    async def unreal_validation_start(
        rule_ids: Annotated[
            list[Annotated[str, Field(min_length=1, max_length=64)]],
            Field(min_length=1, max_length=8),
        ],
        asset_paths: Annotated[list[AssetPath], Field(min_length=1, max_length=20)],
    ) -> dict[str, Any]:
        """Run only selected approved project validators on exact assets as a bounded native job.

        Validators execute trusted project code and can have side effects; listing is separate
        from authorization to run them. Requires expected project binding; no PIE or automatic
        retries. Cancellation/timeouts are cooperative between validator callbacks. Poll the
        returned job_id; not_validated is never a pass. Nothing is saved by this adapter.
        The bridge automatically binds the queued job to its authenticated project,
        session, world and revision preflight; an editor change rejects the request.
        """
        return await call("validation_start", {"rule_ids": rule_ids, "asset_paths": asset_paths})

    @server.tool(annotations=read)
    async def unreal_validation_job(job_id: JobId) -> dict[str, Any]:
        """Read a native validation job and its authoritative bounded errors; never restart it."""
        return await call("validation_job", {"job_id": job_id})

    @server.tool(annotations=run)
    async def unreal_validation_cancel(job_id: JobId) -> dict[str, Any]:
        """Cancel pending validation between callbacks. Does not undo validator side effects."""
        return await call("validation_cancel", {"job_id": job_id})

    @server.tool(annotations=read)
    async def unreal_functional_tests() -> dict[str, Any]:
        """List named project-approved functional tests and the current single PIE eligibility.

        Configuration lives in [JevEditor.FunctionalTesting]; execution is disabled by default.
        No tests run, actors load or PIE sessions start during discovery.
        """
        return await call("functional_tests")

    @server.tool(annotations=run)
    async def unreal_functional_start(
        test_id: Annotated[str, Field(min_length=1, max_length=64)],
        expected_state: ExpectedState,
    ) -> dict[str, Any]:
        """Run one approved placed AFunctionalTest in an existing standalone PIE session.

        Inspect context and list tests first. Requires exact project/session/world/revision.
        Trusted test callbacks can change gameplay state. This does not start or stop PIE,
        run arbitrary functions, retry failures, save maps or certify the whole game.
        Poll the job for passed/failed/error/timeout/cancelled/interrupted evidence. Cancellation
        and timeout use the owned test's FinishTest/CleanUp; callbacks are not sandboxed.
        """
        return await call(
            "functional_start",
            {
                "test_id": test_id,
                "expected_state": expected_state.model_dump(),
            },
        )

    @server.tool(annotations=read)
    async def unreal_functional_job(job_id: JobId) -> dict[str, Any]:
        """Read a functional test's native result and cleanup status without retrying."""
        return await call("functional_job", {"job_id": job_id})

    @server.tool(annotations=run)
    async def unreal_functional_cancel(job_id: JobId) -> dict[str, Any]:
        """Finish/clean up this bridge-owned test. Leaves the user's PIE session running."""
        return await call("functional_cancel", {"job_id": job_id})
