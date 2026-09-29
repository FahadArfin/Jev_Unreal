"""Reviewed, project-approved native Blueprint compilation; no generated code execution."""

from typing import Annotated, Any, Literal

from mcp.server.fastmcp import FastMCP
from mcp.types import ToolAnnotations
from pydantic import Field

from .bridge import UnrealBridge
from .domain_workflows import Strict
from .errors import JevError
from .workflows import ExpectedState

TargetId = Annotated[str, Field(min_length=1, max_length=64, pattern=r"^[A-Za-z0-9_.-]+$")]
PlanId = Annotated[str, Field(min_length=1, max_length=64)]


class PinEdit(Strict):
    node_id: Annotated[str, Field(min_length=32, max_length=36, pattern=r"^[A-Fa-f0-9-]+$")]
    pin_id: Annotated[str, Field(min_length=32, max_length=36, pattern=r"^[A-Fa-f0-9-]+$")]
    value: Annotated[str, Field(min_length=1, max_length=32)]


Guid = Annotated[str, Field(min_length=32, max_length=36, pattern=r"^[A-Fa-f0-9-]+$")]


class AddMathNode(Strict):
    operation: Literal["add_math_node"]
    graph_id: Guid
    function: Literal[
        "Add_IntInt", "Multiply_IntInt", "Add_DoubleDouble", "Multiply_DoubleDouble", "Not_PreBool"
    ]
    x: int = Field(ge=-100_000, le=100_000)
    y: int = Field(ge=-100_000, le=100_000)


class RemoveMathNode(Strict):
    operation: Literal["remove_math_node"]
    graph_id: Guid
    node_id: Guid


class MathLink(Strict):
    operation: Literal["connect", "disconnect"]
    graph_id: Guid
    output_node_id: Guid
    output_pin_id: Guid
    input_node_id: Guid
    input_pin_id: Guid


class GraphPosition(Strict):
    graph_id: Guid
    x: int = Field(ge=-100_000, le=100_000)
    y: int = Field(ge=-100_000, le=100_000)


class AddEvent(GraphPosition):
    operation: Literal["add_event"]
    event: Literal["ReceiveBeginPlay", "ReceiveActorBeginOverlap"]


class AddBranch(GraphPosition):
    operation: Literal["add_branch"]


VariableName = Annotated[
    str, Field(min_length=1, max_length=64, pattern=r"^[A-Za-z_][A-Za-z0-9_]*$")
]


class AddVariable(Strict):
    operation: Literal["add_variable"]
    graph_id: Guid
    name: VariableName
    type: Literal["bool", "int", "double"]


class AddVariableNode(GraphPosition):
    operation: Literal["add_variable_get", "add_variable_set"]
    name: VariableName


class AddActorCall(GraphPosition):
    operation: Literal["add_actor_call"]
    function: Literal[
        "K2_SetActorRelativeLocation", "SetActorEnableCollision", "SetActorHiddenInGame"
    ]
    location: Annotated[
        list[Annotated[float, Field(ge=-1_000_000, le=1_000_000)]],
        Field(min_length=3, max_length=3),
    ] | None = None


GraphEdit = Annotated[
    AddMathNode | RemoveMathNode | MathLink | AddEvent | AddBranch | AddVariable
    | AddVariableNode | AddActorCall,
    Field(discriminator="operation"),
]


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

    @server.tool(annotations=preview)
    async def unreal_blueprint_pin_preview(
        target_id: TargetId, pin_edit: PinEdit, expected_state: ExpectedState
    ) -> dict[str, Any]:
        """Preview one unconnected bool/int/real literal on an approved native graph node.

        Supports Add_IntInt, Multiply_IntInt, Add_DoubleDouble, Multiply_DoubleDouble and
        Not_PreBool in a loaded ordinary Blueprint approved for compilation. Additional
        project GameplayTargets permit primitive literals on approved branch, variable and
        actor-call nodes. Inspect node/pin GUIDs first. No object defaults or expressions.
        Commit using unreal_blueprint_compile: it edits in an Undo transaction and compiles.
        Compiler failure retains the edit for explicit Undo/correction. Callbacks are trusted
        project code; complete rollback and runtime/semantic success are not guaranteed.
        """
        return await call(
            "blueprint_pin_preview",
            {
                "target_id": target_id,
                "pin_edit": pin_edit.model_dump(),
                "expected_state": expected_state.model_dump(),
            },
        )

    @server.tool(annotations=read)
    async def unreal_blueprint_compile_receipt(plan_id: PlanId) -> dict[str, Any]:
        """Read an existing compile plan/result; never retry compilation.

        Receipts are retained for at most 15 minutes in this editor session, including across
        MCP reconnects. Diagnostics may contain private project text; review before sharing.
        """
        return await call("blueprint_compile_receipt", {"plan_id": plan_id})

    @server.tool(annotations=preview)
    async def unreal_blueprint_graph_preview(
        target_id: TargetId, graph_edit: GraphEdit, expected_state: ExpectedState
    ) -> dict[str, Any]:
        """Preview one approved graph edit, then compile the reviewed one-shot plan.

        Inspect GUIDs first. Requires project graph-edit policy; no coercion, implicit link
        breaks or cycles. Remove only unlinked supported math nodes. Additional project
        bEnableGameplayGraphEdits and GameplayTargets approval permits actor BeginPlay/overlap
        events, branches, local bool/int/double variables and their get/set nodes, and three
        fixed self-actor calls. Relative location requires three bounded centimetre values.
        No arbitrary functions, object links, new classes, timelines or generated code.
        Compiler callbacks are trusted project code; compilation is not gameplay verification.
        """
        return await call(
            "blueprint_graph_preview",
            {
                "target_id": target_id,
                "graph_edit": graph_edit.model_dump(mode="json", exclude_none=True),
                "expected_state": expected_state.model_dump(),
            },
        )
