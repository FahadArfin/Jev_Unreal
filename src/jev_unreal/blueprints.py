"""Reviewed, project-approved native Blueprint compilation; no generated code execution."""

from typing import Annotated, Any

from mcp.server.fastmcp import FastMCP
from mcp.types import ToolAnnotations
from pydantic import Field

from .bridge import UnrealBridge
from .errors import JevError
from .workflows import ExpectedState

TargetId = Annotated[str, Field(min_length=1, max_length=64, pattern=r"^[A-Za-z0-9_.-]+$")]
PlanId = Annotated[str, Field(min_length=1, max_length=64)]


def register_blueprint_tools(server: FastMCP, bridge: UnrealBridge) -> None:
    read = ToolAnnotations(readOnlyHint=True, destructiveHint=False, openWorldHint=False)
    preview = ToolAnnotations(
        readOnlyHint=False, destructiveHint=False, idempotentHint=False, openWorldHint=False
    )
    compile_action = ToolAnnotations(
        readOnlyHint=False, destructiveHint=True, idempotentHint=False, openWorldHint=False
    )

    async def call(action: str, params: dict | None = None) -> dict[str, Any]:
        try:
            return {"ok": True, "result": await bridge.call(action, params)}
        except JevError as exc:
            return exc.as_dict()

    @server.tool(annotations=read)
    async def unreal_blueprint_compile_targets() -> dict[str, Any]:
        """List project-approved Blueprint compile aliases without loading or compiling assets.

        Disabled by default; configure [JevEditor.BlueprintCompilation] locally. Discovery
        does not authorize execution. Exact native Blueprint, WidgetBlueprint and AnimBlueprint
        assets are eligible only when already loaded and listed by their project owner.
        """
        return await call("blueprint_compile_targets")

    @server.tool(annotations=preview)
    async def unreal_blueprint_compile_preview(
        target_id: TargetId, expected_state: ExpectedState
    ) -> dict[str, Any]:
        """Preview one explicit compiler run for an approved alias using inspected editor state.

        Does not compile. Review the exact asset and callback side effects in the returned
        plan. It expires in 120 seconds; scene or notified object edits invalidate it. Native
        compiler callbacks are trusted project code, can affect other assets or instances,
        and cannot be sandboxed or rolled back by Jev. Requires exact project binding.
        """
        return await call(
            "blueprint_compile_preview",
            {"target_id": target_id, "expected_state": expected_state.model_dump()},
        )

    @server.tool(annotations=compile_action)
    async def unreal_blueprint_compile(plan_id: PlanId) -> dict[str, Any]:
        """Consume one reviewed native compile plan and return fresh bounded compiler diagnostics.

        Runs synchronously with SkipSave; trusted callbacks may still change or save content.
        No hard timeout, cancellation, automatic retry, rollback or blanket save guarantee.
        A failed or stale attempt consumes the plan. If the client times out, read the same
        plan's receipt before taking further action. A successful compile is not gameplay or
        visual acceptance. Requires explicit expected project configuration; no PIE/simulation.
        """
        return await call("blueprint_compile", {"plan_id": plan_id})

    @server.tool(annotations=read)
    async def unreal_blueprint_compile_receipt(plan_id: PlanId) -> dict[str, Any]:
        """Read an existing compile plan/result; never retry compilation.

        Receipts are retained for at most 15 minutes in this editor session, including across
        MCP reconnects. Diagnostics may contain private project text; review before sharing.
        """
        return await call("blueprint_compile_receipt", {"plan_id": plan_id})
