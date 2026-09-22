"""Typed domain inspections, reviewed edits and measured editor timing workflows."""

import math
from typing import Annotated, Any, Literal

from mcp.server.fastmcp import FastMCP
from mcp.types import ToolAnnotations
from pydantic import BaseModel, ConfigDict, Field, field_validator

from .bridge import UnrealBridge
from .errors import JevError
from .layouts import PreviewTracker
from .workflows import ExpectedState, Operation

Path = Annotated[str, Field(min_length=1, max_length=1024, strict=True)]
Name = Annotated[str, Field(min_length=1, max_length=128, strict=True)]
JobId = Annotated[str, Field(min_length=1, max_length=64)]
Coordinate = Annotated[float, Field(ge=-10_000_000, le=10_000_000, allow_inf_nan=False)]
Vec3 = Annotated[list[Coordinate], Field(min_length=3, max_length=3)]
Degrees = Annotated[float, Field(ge=-360, le=360, allow_inf_nan=False)]
Rotation = Annotated[list[Degrees], Field(min_length=3, max_length=3)]
Rgb = Annotated[
    list[Annotated[float, Field(ge=0, le=1, allow_inf_nan=False)]],
    Field(min_length=3, max_length=3),
]


class Strict(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True, revalidate_instances="always")

    @field_validator("*")
    @classmethod
    def no_control_text(cls, value):
        if isinstance(value, str) and any(ord(c) < 32 or ord(c) == 127 for c in value):
            raise ValueError("Control characters are not allowed.")
        return value


class AssetInspection(Strict):
    kind: Literal["material", "light", "widgets"]
    target_path: Path


class CameraInspection(Strict):
    kind: Literal["camera"]


class AssetDiagnosis(Strict):
    kind: Literal["asset_diagnosis"]
    target_path: Path
    dependency_depth: int = Field(default=1, ge=1, le=3)


class RigInspection(Strict):
    kind: Literal["rig"]
    target_path: Path
    animation_path: Path | None = None
    required_bones: list[Name] = Field(default_factory=list, max_length=64)


class SurfaceInspection(Strict):
    kind: Literal["surface"]
    actor_path: Path
    surface_paths: list[Path] = Field(min_length=1, max_length=32)
    trace_channel: Literal["visibility", "camera"] = "visibility"
    trace_up_cm: float = Field(default=1000, ge=0, le=100_000, allow_inf_nan=False)
    trace_down_cm: float = Field(default=10_000, ge=1, le=100_000, allow_inf_nan=False)
    max_slope_degrees: float = Field(default=30, ge=0, le=60, allow_inf_nan=False)
    clearance_cm: float = Field(default=1, ge=0, le=1000, allow_inf_nan=False)
    align_to_normal: bool = False


class NavigationInspection(Strict):
    kind: Literal["navigation"]
    nav_data_path: Path
    start: Vec3
    end: Vec3
    projection_extent_cm: Annotated[
        list[Annotated[float, Field(gt=0, le=1000)]], Field(min_length=3, max_length=3)
    ]
    required_width_cm: float = Field(ge=1, le=10_000, allow_inf_nan=False)
    maximum_step_cm: float = Field(ge=0, le=1000, allow_inf_nan=False)


Inspection = Annotated[
    AssetInspection
    | CameraInspection
    | AssetDiagnosis
    | RigInspection
    | SurfaceInspection
    | NavigationInspection,
    Field(discriminator="kind"),
]


class ScalarChange(Strict):
    kind: Literal["material_scalar"]
    target_path: Path
    parameter: Name
    value: float = Field(ge=-1_000_000, le=1_000_000, allow_inf_nan=False)


class VectorChange(Strict):
    kind: Literal["material_vector"]
    target_path: Path
    parameter: Name
    value: Annotated[
        list[Annotated[float, Field(ge=0, le=16, allow_inf_nan=False)]],
        Field(min_length=4, max_length=4),
    ]


class LightChange(Strict):
    kind: Literal["light"]
    target_path: Path
    intensity: float = Field(ge=0, le=1_000_000, allow_inf_nan=False)
    color_rgb: Rgb


class CameraChange(Strict):
    kind: Literal["camera"]
    location: Vec3
    rotation: Rotation
    fov_degrees: float = Field(ge=5, le=170, allow_inf_nan=False)


Change = Annotated[
    ScalarChange | VectorChange | LightChange | CameraChange, Field(discriminator="kind")
]


def compare_performance(baseline: dict, candidate: dict, maximum_regression_percent: float) -> dict:
    """Compare complete native receipts; threshold is a review rule, not statistical proof."""
    if not math.isfinite(maximum_regression_percent) or not 0 <= maximum_regression_percent <= 1000:
        raise JevError("invalid_comparison", "Regression tolerance must be finite in 0..1000.")
    for key in (
        "project_file",
        "session_id",
        "world_path",
        "protocol_id",
        "metric",
        "requested_samples",
    ):
        if not baseline.get(key) or baseline.get(key) != candidate.get(key):
            raise JevError(
                "incomparable_measurements",
                "Use the same project/session, world, protocol, metric and sample count.",
            )
    if not baseline.get("job_id") or baseline.get("job_id") == candidate.get("job_id"):
        raise JevError("incomparable_measurements", "Two distinct completed captures are required.")
    measurements = []
    for receipt in (baseline, candidate):
        timing = receipt.get("editor_tick_interval", {})
        count = receipt.get("requested_samples")
        if (
            receipt.get("status") != "completed"
            or type(count) is not int
            or not 10 <= count <= 600
            or not isinstance(timing, dict)
            or timing.get("count") != count
        ):
            raise JevError(
                "incomplete_measurement",
                "Failed, cancelled, partial or changed-state captures cannot be compared.",
            )
        row = {}
        for metric in ("mean_ms", "p50_ms", "p95_ms"):
            value = timing.get(metric)
            if (
                type(value) not in (int, float)
                or not math.isfinite(value)
                or not 0 < value <= 60_000
            ):
                raise JevError(
                    "invalid_measurement",
                    "Native timing values must be positive, finite milliseconds.",
                )
            row[metric] = value
        measurements.append(row)
    rows = []
    for key in measurements[0]:
        old, new = measurements[0][key], measurements[1][key]
        delta = (new - old) / old * 100
        rows.append(
            {
                "metric": key,
                "baseline": old,
                "candidate": new,
                "delta_ms": new - old,
                "delta_percent": delta,
                "threshold_exceeded": delta > maximum_regression_percent,
            }
        )
    return {
        "baseline_job_id": baseline["job_id"],
        "candidate_job_id": candidate["job_id"],
        "protocol_id": baseline["protocol_id"],
        "maximum_regression_percent": maximum_regression_percent,
        "comparisons": rows,
        "regression_flagged": any(row["threshold_exceeded"] for row in rows),
        "statistical_significance_established": False,
        "scope": (
            "Editor ticker intervals include idle/throttle, UI, polling and background work. "
            "Matching protocol IDs are operator declarations, not verified workload equivalence. "
            "Repeat captures and investigate with Unreal Insights; these timings do not isolate "
            "a GPU/gameplay bottleneck."
        ),
        "cloud_used": False,
    }


def register_domain_tools(server: FastMCP, bridge: UnrealBridge, previews: PreviewTracker) -> None:
    read = ToolAnnotations(readOnlyHint=True, destructiveHint=False, openWorldHint=False)
    preview = ToolAnnotations(
        readOnlyHint=False, destructiveHint=False, idempotentHint=False, openWorldHint=False
    )
    edit = ToolAnnotations(
        readOnlyHint=False, destructiveHint=True, idempotentHint=False, openWorldHint=False
    )

    async def call(action: str, params: dict) -> dict[str, Any]:
        try:
            return {"ok": True, "result": await bridge.call(action, params)}
        except JevError as exc:
            return exc.as_dict()

    @server.tool(annotations=read)
    async def unreal_workflow_inspect(query: Inspection) -> dict[str, Any]:
        """Inspect material parameters, native lights, viewport camera, mesh/import dependencies,
        skeleton/animation compatibility, stored Widget Blueprint trees, approved surfaces,
        or native Recast routes. Explicit bounded fields; never arbitrary execution or loading.
        Results declare evidence limits. No compile, save, tracing into DCC files or gameplay proof.
        """
        return await call("workflow_inspect", query.model_dump(mode="json", exclude_none=True))

    @server.tool(annotations=preview)
    async def unreal_workflow_preview(
        change: Change, expected_state: ExpectedState
    ) -> dict[str, Any]:
        """Preview one material scalar/vector, light intensity/color or editor camera pose change.
        Inspect first. Global exposed material parameters require exact project approval in
        [JevEditor.Workflows] bEnableMaterialEdits and EditableMaterials. The returned native
        plan expires after 120 seconds. Review before using unreal_workflow_apply once.
        """
        return await call(
            "workflow_preview",
            {
                "change": change.model_dump(mode="json"),
                "expected_state": expected_state.model_dump(),
            },
        )

    @server.tool(annotations=edit)
    async def unreal_workflow_apply(plan_id: JobId) -> dict[str, Any]:
        """Apply one reviewed domain plan once; return native before/after state.
        No automatic retry. Asset/light edits use Undo, camera changes do not. No save request.
        A timed-out attempt may have applied: inspect unreal_workflow_receipt and fresh state.
        Material/light changes need rendered capture after settling for visual acceptance.
        """
        return await call("workflow_apply", {"plan_id": plan_id})

    @server.tool(annotations=read)
    async def unreal_workflow_receipt(plan_id: JobId) -> dict[str, Any]:
        """Read a domain edit receipt; retained 15 minutes in this editor session."""
        return await call("workflow_receipt", {"plan_id": plan_id})

    @server.tool(annotations=preview)
    async def unreal_surface_preview(query: SurfaceInspection) -> dict[str, Any]:
        """Trace to approved collision surfaces, reject excessive slopes/overlap, and preview
        the measured transform. Requires a native mesh actor. Does not apply. Review the trace
        and native scene plan, then use unreal_apply and fresh actor/visual checks.
        """
        try:
            measured = await bridge.call("workflow_inspect", query.model_dump(mode="json"))
            if (
                measured.get("placement_valid") is not True
                or measured.get("actor_path") != query.actor_path
            ):
                return {
                    "ok": False,
                    "error": {
                        "code": "placement_refused",
                        "message": "Surface/overlap requirements were not met.",
                    },
                    "measurement": measured,
                }
            operation = Operation.model_validate(
                {
                    "op": "set_transform",
                    "actor_path": query.actor_path,
                    **{key: measured[key] for key in ("location", "rotation", "scale")},
                }
            )
            state = ExpectedState.model_validate(
                {key: measured[key] for key in ("session_id", "world_path", "revision")}
            )
            result = await previews.preview(
                [operation.model_dump(mode="json", exclude_none=True)],
                expected_state=state.model_dump(),
            )
            result["surface_measurement"] = measured
            return {"ok": True, "result": result}
        except JevError as exc:
            return exc.as_dict()
        except (KeyError, TypeError, ValueError):
            return JevError(
                "invalid_measurement", "The editor returned incomplete placement evidence."
            ).as_dict()

    @server.tool(annotations=preview)
    async def unreal_performance_start(
        protocol_id: Annotated[
            str, Field(min_length=1, max_length=64, pattern=r"^[A-Za-z0-9_-]+$")
        ],
        expected_state: ExpectedState,
        sample_count: Annotated[int, Field(ge=10, le=600)] = 120,
    ) -> dict[str, Any]:
        """Start a bounded editor ticker timing/memory capture (10..600 samples, 60-second limit).
        Hold workload, viewport, polling, throttle and rendering settings constant in the named
        protocol. Does not freeze the scene or measure packaged gameplay FPS. Poll by job id.
        """
        return await call(
            "performance_start",
            {
                "protocol_id": protocol_id,
                "expected_state": expected_state.model_dump(),
                "sample_count": sample_count,
            },
        )

    @server.tool(annotations=read)
    async def unreal_performance_job(job_id: JobId) -> dict[str, Any]:
        """Read timing progress and scope, including incomplete or changed-state outcomes."""
        return await call("performance_job", {"job_id": job_id})

    @server.tool(annotations=preview)
    async def unreal_performance_cancel(job_id: JobId) -> dict[str, Any]:
        """Cancel an owned timing capture and preserve its partial result."""
        return await call("performance_cancel", {"job_id": job_id})

    @server.tool(annotations=read)
    async def unreal_performance_compare(
        baseline_job_id: JobId,
        candidate_job_id: JobId,
        maximum_regression_percent: Annotated[float, Field(ge=0, le=1000, allow_inf_nan=False)] = 5,
    ) -> dict[str, Any]:
        """Compare two complete native timing receipts from the same protocol, project/session
        and world. Report absolute/percent changes and configured threshold crossings. A pair
        of captures cannot establish significance or identify a rendering/gameplay bottleneck.
        """
        try:
            baseline = await bridge.call("performance_job", {"job_id": baseline_job_id})
            candidate = await bridge.call("performance_job", {"job_id": candidate_job_id})
            return {
                "ok": True,
                "result": compare_performance(baseline, candidate, maximum_regression_percent),
            }
        except JevError as exc:
            return exc.as_dict()
