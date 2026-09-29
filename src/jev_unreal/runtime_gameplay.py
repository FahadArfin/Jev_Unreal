"""Project-approved standalone PIE lifecycle and captures; no arbitrary runtime execution."""

from typing import Annotated, Any, Literal

from mcp.server.fastmcp import FastMCP
from mcp.types import ToolAnnotations
from pydantic import Field

from .bridge import UnrealBridge
from .capture import capture_content
from .errors import JevError
from .workflows import ExpectedState

RuntimeId = Annotated[str, Field(pattern=r"^[A-Fa-f0-9]{8}(?:-[A-Fa-f0-9]{4}){3}-[A-Fa-f0-9]{12}$")]


def register_runtime_gameplay_tools(server: FastMCP, bridge: UnrealBridge) -> None:
    read = ToolAnnotations(readOnlyHint=True, destructiveHint=False, openWorldHint=False)
    preview = ToolAnnotations(readOnlyHint=False, destructiveHint=False, idempotentHint=False)
    execute = ToolAnnotations(readOnlyHint=False, destructiveHint=True, idempotentHint=False)

    async def call(action: str, params: dict | None = None) -> dict[str, Any]:
        try:
            return {"ok": True, "result": await bridge.call(action, params)}
        except JevError as exc:
            return exc.as_dict()

    @server.tool(annotations=read)
    async def unreal_runtime_status() -> dict[str, Any]:
        """Inspect project PIE policy, current map approval and this bridge's owned session.

        Default disabled. Configure [JevEditor.RuntimeGameplay] bEnabled and exact Maps locally.
        A user-started session is never adopted. No provider calls or project code execution.
        """
        return await call("runtime_status")

    @server.tool(annotations=preview)
    async def unreal_runtime_preview(
        operation: Literal["start", "stop"],
        expected_state: ExpectedState,
        owned_session_id: RuntimeId | None = None,
    ) -> dict[str, Any]:
        """Review starting approved standalone PIE or stopping our exact owned session.

        Start accepts no session ID. Stop requires the ID returned by this bridge's start.
        Exact editor state and project binding are required. Start runs trusted project
        BeginPlay code; stop runs EndPlay/cleanup. Callbacks may have external side effects.
        Review the one-shot plan before applying; expiry is 120 seconds. No arbitrary commands.
        """
        if (operation == "stop") != (owned_session_id is not None):
            return JevError("bad_request", "Only stop requires owned_session_id.").as_dict()
        params = {"operation": operation, "expected_state": expected_state.model_dump()}
        if owned_session_id is not None:
            params["owned_session_id"] = owned_session_id
        return await call("runtime_preview", params)

    @server.tool(annotations=execute)
    async def unreal_runtime_apply(plan_id: RuntimeId) -> dict[str, Any]:
        """Consume one reviewed start/stop plan. Poll its receipt to observe the transition.

        Never blindly retry after timeout. A stale or rejected attempt also consumes the plan.
        Native callbacks are not interruptible or sandboxed. Stop never targets another PIE
        session. No map loads, saves requested, online login, input injection or multiplayer.
        """
        return await call("runtime_apply", {"plan_id": plan_id})

    @server.tool(annotations=read)
    async def unreal_runtime_receipt(plan_id: RuntimeId) -> dict[str, Any]:
        """Read lifecycle outcome and owned_session_id after starting; no repeated execution.

        Receipts survive MCP reconnects for 15 minutes within this native editor session.
        'running' only establishes PIE startup. Use approved functional tests and captures
        separately to establish gameplay and visual acceptance.
        """
        return await call("runtime_receipt", {"plan_id": plan_id})

    @server.tool(annotations=read)
    async def unreal_runtime_capture(
        owned_session_id: RuntimeId,
        max_dimension: Annotated[int, Field(ge=64, le=1024)] = 1024,
    ) -> Any:
        """Read one bounded PNG from our exact live standalone PIE game viewport.

        This is a runtime frame, with world time, not the level editor camera. Requires the
        surviving bridge-owned session. No filesystem path, recording or visual-pass claim.
        """
        try:
            result = await bridge.call(
                "runtime_capture",
                {"owned_session_id": owned_session_id, "max_dimension": max_dimension},
            )
            return capture_content(result)
        except JevError as exc:
            return exc.as_dict()
