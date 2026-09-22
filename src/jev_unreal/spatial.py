"""Measured, deterministic actor translations; calculation never applies an edit."""

import math
from decimal import ROUND_HALF_UP, Context, Decimal, localcontext
from typing import Annotated, Literal

from pydantic import (
    BaseModel,
    ConfigDict,
    Field,
    TypeAdapter,
    ValidationError,
    field_validator,
    model_validator,
)

from .bridge import UnrealBridge, project_identity
from .errors import JevError
from .layouts import PreviewTracker

Coordinate = Annotated[float, Field(ge=-1_000_000, le=1_000_000, strict=True)]
GridSize = Annotated[float, Field(ge=0.001, le=1_000_000, strict=True)]
Vector = Annotated[list[Coordinate], Field(min_length=3, max_length=3)]
GridVector = Annotated[list[GridSize], Field(min_length=3, max_length=3)]
ActorPath = Annotated[str, Field(min_length=1, max_length=1024, strict=True)]
Axis = Literal["x", "y", "z"]


class SpatialBase(BaseModel):
    model_config = ConfigDict(
        extra="forbid", strict=True, allow_inf_nan=False, revalidate_instances="always"
    )
    actor_paths: list[ActorPath] = Field(min_length=1, max_length=20)

    @field_validator("actor_paths")
    @classmethod
    def exact_unique_paths(cls, paths: list[str]) -> list[str]:
        if len(set(paths)) != len(paths):
            raise ValueError("Actor paths must be unique.")
        for path in paths:
            if (
                not path.startswith("/")
                or "." not in path
                or any(character in path for character in ("..", "\\", "*", "?"))
                or any(character.isspace() or ord(character) < 32 for character in path)
                or "\x7f" in path
            ):
                raise ValueError("Use exact actor object paths returned by the editor.")
            try:
                units = len(path.encode("utf-16-le")) // 2
            except UnicodeEncodeError:
                raise ValueError("Actor paths must contain valid Unicode.") from None
            if units > 1024:
                raise ValueError("Actor paths exceed 1024 Unreal UTF-16 code units.")
        return paths


class AlignRecipe(SpatialBase):
    kind: Literal["align"]
    axis: Axis
    anchor: Literal["min", "center", "max"] = "center"
    target_cm: Coordinate


class DistributeRecipe(SpatialBase):
    kind: Literal["distribute"]
    axis: Axis
    mode: Literal["centers", "equal_gaps"] = "centers"

    @model_validator(mode="after")
    def enough_actors(self):
        if len(self.actor_paths) < 3:
            raise ValueError("Distribution requires at least three actors.")
        return self


class SnapGridRecipe(SpatialBase):
    kind: Literal["snap_grid"]
    grid_cm: GridSize | GridVector
    origin_cm: Vector = Field(default_factory=lambda: [0.0, 0.0, 0.0])


class GroundRecipe(SpatialBase):
    kind: Literal["ground"]
    z_cm: Coordinate


SpatialRecipe = Annotated[
    AlignRecipe | DistributeRecipe | SnapGridRecipe | GroundRecipe, Field(discriminator="kind")
]
_RECIPE = TypeAdapter(SpatialRecipe)

Bound = Annotated[float, Field(ge=-1_000_000_000, le=1_000_000_000, strict=True)]
BoundVector = Annotated[list[Bound], Field(min_length=3, max_length=3)]
Size = Annotated[float, Field(ge=0, le=2_000_000_000, strict=True)]
Rotation = Annotated[float, Field(ge=-36000, le=36000, strict=True)]
Scale = Annotated[float, Field(ge=0.001, le=1000, strict=True)]


class _Bounds(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True, allow_inf_nan=False)
    min: BoundVector
    max: BoundVector
    center: BoundVector
    size: Annotated[list[Size], Field(min_length=3, max_length=3)]

    @model_validator(mode="after")
    def consistent_geometry(self):
        for index in range(3):
            low, high = self.min[index], self.max[index]
            if low > high or not all(
                math.isclose(actual, expected, rel_tol=1e-12, abs_tol=1e-4)
                for actual, expected in (
                    (self.center[index], (low + high) / 2),
                    (self.size[index], high - low),
                )
            ):
                raise ValueError("World bounds must have consistent min, max, center and size.")
        return self


class _Actor(BaseModel):
    # Native inspection includes unrelated mesh/material metadata. Only these fields
    # influence translations; additional inspection fields cannot authorize an edit.
    model_config = ConfigDict(extra="ignore", strict=True, allow_inf_nan=False)
    path: ActorPath
    instance_id: str = Field(min_length=1, max_length=128)
    location: Vector
    rotation: Annotated[list[Rotation], Field(min_length=3, max_length=3)]
    scale: Annotated[list[Scale], Field(min_length=3, max_length=3)]
    bounds_available: Literal[True]
    bounds_cm: _Bounds
    editable: Literal[True]
    edit_blockers: list[Annotated[str, Field(max_length=256)]] = Field(max_length=0)

    @field_validator("instance_id")
    @classmethod
    def nonblank_instance(cls, value: str) -> str:
        if not value.strip() or any(
            ord(character) < 32 or ord(character) == 127 for character in value
        ):
            raise ValueError("Actor instance identity must be nonblank and contain no controls.")
        try:
            value.encode("utf-8")
        except UnicodeEncodeError:
            raise ValueError("Actor instance identity must contain valid Unicode.") from None
        return value

    @field_validator("bounds_available", "editable", mode="before")
    @classmethod
    def explicit_true(cls, value: object) -> object:
        if value is not True:
            raise ValueError("Editable actors with available bounds are required.")
        return value


class _Details(BaseModel):
    model_config = ConfigDict(extra="ignore", strict=True, allow_inf_nan=False)
    project_file: str = Field(min_length=1, max_length=4096)
    session_id: str = Field(min_length=1, max_length=128)
    world_path: str = Field(min_length=1, max_length=1024)
    current_level: str = Field(min_length=1, max_length=1024)
    revision: str = Field(min_length=1, max_length=128)
    truncated: Literal[False]
    actors: list[_Actor] = Field(min_length=1, max_length=20)

    @field_validator("truncated", mode="before")
    @classmethod
    def explicitly_complete(cls, value: object) -> object:
        if value is not False:
            raise ValueError("A complete actor inspection is required.")
        return value

    @field_validator("project_file", "session_id", "world_path", "current_level", "revision")
    @classmethod
    def nonblank_identity(cls, value: str) -> str:
        if not value.strip() or any(
            ord(character) < 32 or ord(character) == 127 for character in value
        ):
            raise ValueError("Inspection identities must be nonblank and contain no controls.")
        try:
            value.encode("utf-8")
        except UnicodeEncodeError:
            raise ValueError("Inspection identities must contain valid Unicode.") from None
        return value


def _snap(value: float, origin: float, grid: float) -> float:
    # Decimal strings make positive and negative decimal half-grid ties explicit,
    # independent of Python's bankers' rounding and the caller's Decimal context.
    with localcontext(Context(prec=50)):
        anchor, step = Decimal(str(origin)), Decimal(str(grid))
        multiple = ((Decimal(str(value)) - anchor) / step).to_integral_value(rounding=ROUND_HALF_UP)
        return float(anchor + multiple * step)


def compile_spatial(recipe: SpatialRecipe, actor_details: object) -> dict:
    """Calculate translations from one native snapshot, without contacting an editor."""
    recipe = _RECIPE.validate_python(recipe)
    try:
        details = _Details.model_validate(actor_details)
        if any(
            actor_details[flag] is not False
            for flag in ("actors_truncated", "scan_incomplete")
            if flag in actor_details
        ):
            raise ValueError("Incomplete actor inspection.")
    except (ValidationError, ValueError):
        raise JevError(
            "invalid_actor_details",
            "Spatial recipes require complete, finite bounds and transforms for editable actors.",
        ) from None
    actors = details.actors
    if [actor.path for actor in actors] != recipe.actor_paths:
        raise JevError(
            "invalid_actor_details",
            "Actor inspection must match the exact requested paths and order.",
        )
    locations = {actor.path: list(actor.location) for actor in actors}
    measurements: dict = {}
    if isinstance(recipe, AlignRecipe):
        axis = "xyz".index(recipe.axis)
        for actor in actors:
            anchor = getattr(actor.bounds_cm, recipe.anchor)[axis]
            locations[actor.path][axis] += recipe.target_cm - anchor
    elif isinstance(recipe, GroundRecipe):
        for actor in actors:
            locations[actor.path][2] += recipe.z_cm - actor.bounds_cm.min[2]
    elif isinstance(recipe, SnapGridRecipe):
        grid = recipe.grid_cm if isinstance(recipe.grid_cm, list) else [recipe.grid_cm] * 3
        for actor in actors:
            locations[actor.path] = [
                _snap(value, origin, step)
                for value, origin, step in zip(actor.location, recipe.origin_cm, grid, strict=True)
            ]
        measurements = {"anchor": "actor_pivot", "half_grid_ties": "away_from_origin"}
    else:
        axis = "xyz".index(recipe.axis)
        ordered = sorted(actors, key=lambda actor: (actor.bounds_cm.center[axis], actor.path))
        measurements["axis_order"] = [actor.path for actor in ordered]
        if recipe.mode == "centers":
            first, last = ordered[0].bounds_cm.center[axis], ordered[-1].bounds_cm.center[axis]
            step = (last - first) / (len(ordered) - 1)
            measurements["center_spacing_cm"] = step
            for index, actor in enumerate(ordered[1:-1], start=1):
                locations[actor.path][axis] += first + index * step - actor.bounds_cm.center[axis]
        else:
            low = min(actor.bounds_cm.min[axis] for actor in ordered)
            high = max(actor.bounds_cm.max[axis] for actor in ordered)
            widths = [actor.bounds_cm.max[axis] - actor.bounds_cm.min[axis] for actor in ordered]
            free = high - low - math.fsum(widths)
            if free < 0:
                raise JevError(
                    "invalid_spatial_recipe",
                    "Actor widths cannot fit nonnegative equal gaps "
                    "inside the existing outer bounds.",
                )
            gap = free / (len(ordered) - 1)
            measurements.update(gap_cm=gap, outer_min_cm=low, outer_max_cm=high)
            for index, actor in enumerate(ordered):
                target_min = low + math.fsum(widths[:index]) + index * gap
                if index == len(ordered) - 1:
                    target_min = high - widths[index]
                locations[actor.path][axis] += target_min - actor.bounds_cm.min[axis]

    operations, comparisons, targets, checks = [], [], [], []
    for actor in actors:
        location = locations[actor.path]
        if any(not math.isfinite(value) or abs(value) > 1_000_000 for value in location):
            raise JevError("invalid_spatial_recipe", "Calculated locations exceed editor limits.")
        delta = [new - old for new, old in zip(location, actor.location, strict=True)]
        before = actor.model_dump(include={"location", "rotation", "scale", "bounds_cm"})
        after_bounds = {
            key: [value + delta[index] for index, value in enumerate(getattr(actor.bounds_cm, key))]
            for key in ("min", "max", "center")
        }
        after_bounds["size"] = list(actor.bounds_cm.size)
        try:
            _Bounds.model_validate(after_bounds)
        except ValidationError:
            raise JevError(
                "invalid_spatial_recipe", "Calculated world bounds exceed limits."
            ) from None
        transform = {
            "location": location,
            "rotation": list(actor.rotation),
            "scale": list(actor.scale),
        }
        operations.append({"op": "set_transform", "actor_path": actor.path, **transform})
        after = {**transform, "bounds_cm": after_bounds}
        comparisons.append(
            {"path": actor.path, "delta_cm": delta, "before": before, "after": after}
        )
        targets.append({"path": actor.path, **after})
        checks.extend(
            [
                {
                    "kind": "transform_equals",
                    "actor_path": actor.path,
                    "expected_instance_id": actor.instance_id,
                    **transform,
                },
                {
                    "kind": "bounds_size",
                    "actor_path": actor.path,
                    "expected_instance_id": actor.instance_id,
                    "expected_cm": list(actor.bounds_cm.size),
                },
            ]
        )
        if isinstance(recipe, GroundRecipe):
            checks.append(
                {
                    "kind": "bottom_z",
                    "actor_path": actor.path,
                    "expected_instance_id": actor.instance_id,
                    "expected_cm": recipe.z_cm,
                }
            )
        elif isinstance(recipe, (AlignRecipe, DistributeRecipe)):
            anchor = (
                recipe.anchor
                if isinstance(recipe, AlignRecipe)
                else ("center" if recipe.mode == "centers" else "min")
            )
            checks.append(
                {
                    "kind": "bounds_anchor",
                    "actor_path": actor.path,
                    "expected_instance_id": actor.instance_id,
                    "axis": recipe.axis,
                    "anchor": anchor,
                    "expected_cm": after_bounds[anchor]["xyz".index(recipe.axis)],
                }
            )
    return {
        "kind": recipe.kind,
        "recipe": recipe.model_dump(mode="json"),
        "units": "centimeters",
        "measurement_state": details.model_dump(exclude={"actors", "truncated"}),
        "measurements": measurements,
        "operations": operations,
        "operation_count": len(operations),
        "actors": comparisons,
        "verification_targets": targets,
        "verification_checks": checks,
        "applied": False,
        "cloud_used": False,
        "note": (
            "Predicted translations from world-axis-aligned bounds; "
            "these are not collision or terrain tests. "
            "Apply the preview separately, inspect fresh bounds, and review the rendered scene."
        ),
    }


async def preview_spatial(
    bridge: UnrealBridge, previews: PreviewTracker, recipe: SpatialRecipe
) -> dict:
    """Inspect once, calculate locally, and request a preview bound to that exact state."""
    recipe = _RECIPE.validate_python(recipe)
    details = await bridge.call("actor_details", {"actor_paths": recipe.actor_paths})
    plan = compile_spatial(recipe, details)
    state = plan["measurement_state"]
    expected_project = getattr(getattr(bridge, "settings", None), "expected_project", None)
    if (
        isinstance(expected_project, str)
        and expected_project
        and project_identity(state["project_file"]) != project_identity(expected_project)
    ):
        raise JevError("wrong_project", "Actor details do not match the configured project.")
    preview = await previews.preview(
        plan["operations"],
        expected_state={key: state[key] for key in ("session_id", "world_path", "revision")},
    )
    return {**plan, "preview": preview}
