"""Deterministic blockout recipes and verification of native editor readbacks.

Recipes only produce existing allowlisted operations. Preview/apply remains the
same project-bound, single-use editor transaction, including its 20-operation cap.
"""

import math
import time
from copy import deepcopy
from typing import Annotated, Literal

from pydantic import BaseModel, ConfigDict, Field, field_validator, model_validator

from .bridge import UnrealBridge
from .errors import JevError
from .journal import PlanJournal

Coordinate = Annotated[float, Field(ge=-900000, le=900000, strict=True)]
Dimension = Annotated[float, Field(ge=0.1, le=50000, strict=True)]
Vector = tuple[Coordinate, Coordinate, Coordinate]


class LayoutBase(BaseModel):
    model_config = ConfigDict(extra="forbid", allow_inf_nan=False)
    label_prefix: str = Field(default="Jev", min_length=1, max_length=48, pattern=r"^[\w -]+$")
    origin: Vector = (0.0, 0.0, 0.0)
    yaw_degrees: float = Field(default=0, ge=-360, le=360, strict=True)

    @field_validator("label_prefix")
    @classmethod
    def bounded_label_prefix(cls, value: str) -> str:
        if not value.strip() or len(value.encode("utf-16-le")) // 2 > 48:
            raise ValueError("Label prefix needs 1 to 48 Unreal UTF-16 code units.")
        return value


class GridLayout(LayoutBase):
    """Boxes in rows along local Y and columns along local X; origin is floor corner."""

    kind: Literal["grid"]
    rows: int = Field(default=2, ge=1, le=20, strict=True)
    columns: int = Field(default=3, ge=1, le=20, strict=True)
    size_cm: tuple[Dimension, Dimension, Dimension] = (100.0, 100.0, 100.0)
    gap_cm: tuple[Annotated[float, Field(ge=0, le=50000, strict=True)], ...] = (100.0, 100.0)

    @model_validator(mode="after")
    def bounded_grid(self):
        if self.rows * self.columns > 20 or len(self.gap_cm) != 2:
            raise ValueError("Grid needs at most 20 boxes and exactly two gap dimensions.")
        return self


class StairLayout(LayoutBase):
    """Solid ascending treads along local X; origin is the first tread's bottom corner."""

    kind: Literal["stairs"]
    steps: int = Field(default=8, ge=1, le=20, strict=True)
    tread_depth_cm: Dimension = 30.0
    rise_cm: Dimension = 18.0
    width_cm: Dimension = 150.0


class RoomLayout(LayoutBase):
    """Floor and four walls around a clear inner volume; ceiling is optional."""

    kind: Literal["room"]
    inner_size_cm: tuple[Dimension, Dimension, Dimension] = (800.0, 600.0, 300.0)
    wall_thickness_cm: Dimension = 20.0
    floor_thickness_cm: Dimension = 20.0
    ceiling: bool = Field(default=False, strict=True)


Layout = Annotated[GridLayout | StairLayout | RoomLayout, Field(discriminator="kind")]

LAYOUT_CATALOG = {
    "grid": {
        "description": "Place a regular array of boxes with explicit dimensions and clear gaps.",
        "example": {"kind": "grid", "rows": 2, "columns": 3, "size_cm": [100, 100, 150]},
    },
    "stairs": {
        "description": "Build a solid staircase with explicit rise, tread depth, and width.",
        "example": {"kind": "stairs", "steps": 8, "rise_cm": 18, "tread_depth_cm": 30},
    },
    "room": {
        "description": "Build a floor and four walls around the requested clear inner volume.",
        "example": {"kind": "room", "inner_size_cm": [800, 600, 300], "ceiling": False},
    },
}


def compile_layout(layout: Layout) -> dict:
    """Compile measured geometry locally; never asks a model to calculate transforms."""
    boxes: list[tuple[str, tuple, tuple]] = []
    if isinstance(layout, GridLayout):
        width, depth, height = layout.size_cm
        for row in range(layout.rows):
            for column in range(layout.columns):
                boxes.append(
                    (
                        f"R{row + 1}_C{column + 1}",
                        (
                            column * (width + layout.gap_cm[0]) + width / 2,
                            row * (depth + layout.gap_cm[1]) + depth / 2,
                            height / 2,
                        ),
                        (width, depth, height),
                    )
                )
    elif isinstance(layout, StairLayout):
        for step in range(layout.steps):
            height = layout.rise_cm * (step + 1)
            boxes.append(
                (
                    f"Step_{step + 1:02d}",
                    ((step + 0.5) * layout.tread_depth_cm, layout.width_cm / 2, height / 2),
                    (layout.tread_depth_cm, layout.width_cm, height),
                )
            )
    else:
        width, depth, height = layout.inner_size_cm
        wall, floor = layout.wall_thickness_cm, layout.floor_thickness_cm
        boxes.extend(
            [
                (
                    "Floor",
                    (width / 2, depth / 2, -floor / 2),
                    (width + 2 * wall, depth + 2 * wall, floor),
                ),
                ("West", (-wall / 2, depth / 2, height / 2), (wall, depth, height)),
                ("East", (width + wall / 2, depth / 2, height / 2), (wall, depth, height)),
                ("South", (width / 2, -wall / 2, height / 2), (width + 2 * wall, wall, height)),
                (
                    "North",
                    (width / 2, depth + wall / 2, height / 2),
                    (width + 2 * wall, wall, height),
                ),
            ]
        )
        if layout.ceiling:
            boxes.append(
                (
                    "Ceiling",
                    (width / 2, depth / 2, height + floor / 2),
                    (width + 2 * wall, depth + 2 * wall, floor),
                )
            )
    yaw = math.radians(layout.yaw_degrees)
    cos, sin = math.cos(yaw), math.sin(yaw)
    operations = []
    for label, center, size in boxes:
        x, y, z = center
        location = [
            layout.origin[0] + x * cos - y * sin,
            layout.origin[1] + x * sin + y * cos,
            layout.origin[2] + z,
        ]
        scale = [dimension / 100 for dimension in size]  # Engine Cube is 100 cm on each axis.
        if any(abs(value) > 1000000 for value in location) or any(
            not 0.001 <= value <= 1000 for value in scale
        ):
            raise JevError("invalid_layout", "Generated geometry exceeds editor transform limits.")
        operations.append(
            {
                "op": "spawn_primitive",
                "shape": "Cube",
                "label": f"{layout.label_prefix}_{label}",
                "location": location,
                "rotation": [0, layout.yaw_degrees, 0],
                "scale": scale,
            }
        )
    return {
        "kind": layout.kind,
        "units": "centimeters",
        "operations": operations,
        "operation_count": len(operations),
        "cloud_used": False,
        "note": "Blockout geometry only; gameplay traversal and visual quality require validation.",
    }


def _quat(rotation: list[float]) -> tuple[float, ...]:
    pitch, yaw, roll = [math.radians(value) / 2 for value in rotation]
    sp, sy, sr = math.sin(pitch), math.sin(yaw), math.sin(roll)
    cp, cy, cr = math.cos(pitch), math.cos(yaw), math.cos(roll)
    # Unreal FRotator (pitch, yaw, roll) to quaternion; q and -q represent the same orientation.
    return (
        cr * sp * sy - sr * cp * cy,
        -cr * sp * cy - sr * cp * sy,
        cr * cp * sy - sr * sp * cy,
        cr * cp * cy + sr * sp * sy,
    )


def _finite_vector(value: object) -> bool:
    if not isinstance(value, (list, tuple)) or len(value) != 3:
        return False
    try:
        return all(
            type(component) in (int, float) and math.isfinite(component) for component in value
        )
    except (OverflowError, ValueError):
        return False


def verify_readback(expected: object, actors: object) -> dict:
    """Check returned actor identity and transforms, including equivalent Euler rotations."""
    issues = []
    if (
        not isinstance(actors, list)
        or not isinstance(expected, list)
        or not 1 <= len(expected) <= 20
        or len(actors) > 20
    ):
        return {
            "status": "mismatch",
            "issues": [
                {
                    "code": "invalid_actor_list"
                    if not isinstance(actors, list) or len(actors) > 20
                    else "invalid_expected_operations"
                }
            ],
            "checked_actors": 0,
            "scope": "Native apply readback: actor identities, positions, orientations, scales.",
            "visual_acceptance": False,
            "saved": False,
        }
    if len(expected) != len(actors):
        issues.append({"code": "actor_count_mismatch"})
    seen_paths = set()
    for index, (operation, actor) in enumerate(zip(expected, actors, strict=False)):
        if (
            not isinstance(operation, dict)
            or not isinstance(operation.get("op"), str)
            or operation["op"]
            not in {
                "spawn_primitive",
                "spawn_static_mesh",
                "set_transform",
                "set_material",
                "set_metadata",
            }
        ):
            issues.append({"index": index, "code": "invalid_expected_operation"})
            continue
        if not isinstance(actor, dict):
            issues.append({"index": index, "code": "invalid_actor_readback"})
            continue
        if operation["op"] in {"spawn_primitive", "spawn_static_mesh"}:
            label = operation.get("label")
            if not isinstance(label, str) or not label.strip():
                issues.append({"index": index, "code": "invalid_expected_operation"})
                continue
            if actor.get("label") != label:
                issues.append({"index": index, "code": "metadata_mismatch", "field": "label"})
        if operation["op"] == "spawn_primitive":
            shape = operation.get("shape")
            if not isinstance(shape, str) or shape not in {"Cube", "Sphere", "Cylinder", "Plane"}:
                issues.append({"index": index, "code": "invalid_expected_operation"})
                continue
            if actor.get("static_mesh_path") != f"/Engine/BasicShapes/{shape}.{shape}":
                issues.append({"index": index, "code": "mesh_identity_mismatch"})
        elif operation["op"] == "spawn_static_mesh":
            asset_path = operation.get("asset_path")
            if not isinstance(asset_path, str) or not asset_path:
                issues.append({"index": index, "code": "invalid_expected_operation"})
                continue
        elif operation["op"] == "set_metadata" and not any(
            isinstance(operation.get(field), str) for field in ("label", "folder")
        ):
            issues.append({"index": index, "code": "invalid_expected_operation"})
            continue
        path = actor.get("path")
        if not isinstance(path, str) or not path or path in seen_paths:
            issues.append({"index": index, "code": "invalid_actor_identity"})
        elif operation.get("op") in {
            "set_transform",
            "set_material",
            "set_metadata",
        } and path != operation.get("actor_path"):
            issues.append({"index": index, "code": "actor_identity_mismatch"})
        seen_paths.add(path if isinstance(path, str) else None)
        if operation.get("op") == "spawn_static_mesh" and actor.get(
            "static_mesh_path"
        ) != operation.get("asset_path"):
            issues.append({"index": index, "code": "mesh_identity_mismatch"})
        if operation.get("op") == "set_material":
            materials = actor.get("materials")
            matches = (
                [
                    item
                    for item in materials
                    if isinstance(item, dict)
                    and type(item.get("slot")) is int
                    and item["slot"] == operation.get("slot")
                ]
                if isinstance(materials, list)
                else []
            )
            if len(matches) != 1 or matches[0].get("path") != operation.get("material_path"):
                issues.append({"index": index, "code": "material_mismatch"})
        if operation.get("op") == "set_metadata":
            for field in ("label", "folder"):
                if field in operation and actor.get(field) != operation[field]:
                    issues.append({"index": index, "code": "metadata_mismatch", "field": field})
        for field in ("location", "scale", "rotation"):
            actual, target = actor.get(field), operation.get(field)
            if not _finite_vector(actual):
                issues.append({"index": index, "code": "invalid_transform", "field": field})
                continue
            if not _finite_vector(target):
                issues.append(
                    {"index": index, "code": "invalid_expected_transform", "field": field}
                )
                continue
            if field == "rotation":
                a, b = _quat(actual), _quat(target)
                matches = (
                    min(
                        sum((x - y) ** 2 for x, y in zip(a, b, strict=True)),
                        sum((x + y) ** 2 for x, y in zip(a, b, strict=True)),
                    )
                    < 1e-10
                )
            else:
                matches = all(
                    math.isclose(x, y, rel_tol=1e-6, abs_tol=1e-3)
                    for x, y in zip(actual, target, strict=True)
                )
            if not matches:
                issues.append({"index": index, "code": "transform_mismatch", "field": field})
    return {
        "status": "passed" if not issues else "mismatch",
        "issues": issues,
        "checked_actors": min(len(expected), len(actors)),
        "scope": "Native readback: actor identities, transforms and requested metadata/materials.",
        "visual_acceptance": False,
        "saved": False,
    }


class PreviewTracker:
    """Bounded in-memory expectations; never retries or rolls back an ambiguous apply."""

    def __init__(self, bridge: UnrealBridge):
        self.bridge = bridge
        self._plans: dict[str, tuple[float, list[dict]]] = {}
        self._attempted: dict[str, float] = {}
        self.journal = PlanJournal()

    def _remember(self, plan_id: str, **fields):
        try:
            self.journal.put(plan_id, **fields)
        except Exception:
            # A receipt is supporting evidence. Malformed optional metadata must
            # never hide the outcome of an already-applied native transaction.
            try:
                self.journal.put(
                    plan_id,
                    status=fields["status"],
                    executed=fields.get("executed"),
                    saved=False,
                    details_omitted=True,
                )
            except Exception:
                # Receipt persistence is best effort, including the minimal fallback.
                # The independent attempt guard still prevents immediate replay.
                pass

    async def preview(self, operations: list[dict], expected_state: dict | None = None) -> dict:
        params = {"operations": operations}
        if expected_state is not None:
            params["expected_state"] = expected_state
        result = await self.bridge.call("preview", params)
        now = time.monotonic()
        self._plans = {key: value for key, value in self._plans.items() if value[0] > now}
        if len(self._plans) >= 64:
            self._plans.pop(next(iter(self._plans)))
        expiry = result.get("expires_in_seconds", 120)
        if type(expiry) not in (int, float) or not 0 <= expiry <= 120 or not math.isfinite(expiry):
            expiry = 120
        self._plans[result["plan_id"]] = (now + expiry, deepcopy(result["operations"]))
        self._remember(
            result["plan_id"],
            status="previewed",
            operations=result["operations"],
            expires_in_seconds=result.get("expires_in_seconds", 120),
            expected_state=expected_state,
            revision=result.get("revision"),
            executed=False,
            saved=False,
        )
        return result

    async def apply(self, plan_id: str) -> dict:
        now = time.monotonic()
        self._attempted = {
            key: deadline for key, deadline in self._attempted.items() if deadline > now
        }
        if plan_id in self._attempted:
            raise JevError(
                "unknown_plan",
                "This process already attempted the plan. Inspect its record and scene; "
                "do not retry.",
            )
        if len(self._attempted) >= 64:
            self._attempted.pop(next(iter(self._attempted)))
        self._attempted[plan_id] = now + 900
        expectation = self._plans.pop(plan_id, None)
        if expectation and expectation[0] <= now:
            expectation = None
        self._remember(plan_id, status="applying", executed=None, saved=False)
        try:
            result = await self.bridge.call("apply", {"plan_id": plan_id})
        except BaseException as exc:
            # Cancellation/transport loss may happen after native application. Preserve
            # that uncertainty and never issue another apply on behalf of the caller.
            code = exc.code if isinstance(exc, JevError) else "interrupted"
            rejected = code in {
                "wrong_project",
                "project_required",
                "missing_bridge_token",
                "stale_plan",
                "expired_plan",
                "play_mode",
                "level_locked",
                "actor_locked",
                "actor_not_found",
                "actor_unsupported",
                "bad_request",
                "editor_busy",
                "asset_not_found",
                "asset_unsupported",
                "asset_unavailable",
                "material_slot_invalid",
                "rate_limited",
                "unauthorized",
                "forbidden",
                "capability_unavailable",
            }
            self._remember(
                plan_id,
                status="rejected" if rejected else "unknown",
                error_code=code,
                executed=False if rejected else None,
                next_step="Inspect the record/current actors; create a fresh preview if needed.",
            )
            raise
        if not isinstance(result, dict):
            result = {
                "applied": None,
                "verification": {
                    "status": "unavailable",
                    "reason": "Invalid native apply result; inspect the scene.",
                    "visual_acceptance": False,
                    "saved": False,
                },
            }
        if result.get("applied") is not True:
            result["verification"] = {
                "status": "unavailable",
                "reason": "Native result did not confirm apply success; inspect the scene.",
                "visual_acceptance": False,
                "saved": False,
            }
        elif expectation:
            result["verification"] = verify_readback(expectation[1], result.get("actors", []))
        else:
            result["verification"] = {
                "status": "unavailable",
                "reason": "No unexpired preview expectation in this MCP process.",
                "visual_acceptance": False,
                "saved": False,
            }
        paths = (
            [
                actor["path"]
                for actor in result.get("actors", [])
                if isinstance(actor, dict) and isinstance(actor.get("path"), str)
            ]
            if isinstance(result.get("actors"), list)
            else []
        )
        self._remember(
            plan_id,
            status="applied" if result.get("applied") is True else "unknown",
            executed=True if result.get("applied") is True else None,
            actor_paths=paths[:20],
            verification=result["verification"],
            revision_after=result.get("revision"),
            next_step="Use fresh unreal_verify/unreal_diff and capture for independent checks.",
        )
        return result
