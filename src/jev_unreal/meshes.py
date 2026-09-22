"""Freshly inspected, bounded mesh replacement and explicit native mesh copies."""

from copy import deepcopy
from typing import Annotated, Literal

from pydantic import Field, TypeAdapter, ValidationError, field_validator, model_validator

from .bridge import UnrealBridge
from .errors import JevError
from .layouts import PreviewTracker, verify_readback
from .mesh_state import mesh_checks, mesh_state
from .spatial import SpatialBase, Vector
from .verification import _check_project, _normalize
from .workflows import Operation


class ReplaceMeshRecipe(SpatialBase):
    kind: Literal["replace"]
    asset_path: str = Field(min_length=1, max_length=512)
    material_policy: Literal["preserve_slots", "mesh_defaults"]

    @field_validator("asset_path")
    @classmethod
    def exact_asset(cls, value: str) -> str:
        return Operation.exact_asset_path(value)


class DuplicateMeshRecipe(SpatialBase):
    kind: Literal["duplicate"]
    offset_cm: Vector = Field(default_factory=lambda: [0.0, 0.0, 0.0])
    label_prefix: str = Field(default="", max_length=40)
    label_suffix: str = Field(default="_Copy", max_length=40)

    @field_validator("label_prefix", "label_suffix")
    @classmethod
    def valid_fragment(cls, value: str) -> str:
        if any(ord(character) < 32 or ord(character) == 127 for character in value):
            raise ValueError("Label fragments must not contain controls.")
        if len(value.encode("utf-16-le")) // 2 > 40:
            raise ValueError("Label fragments are limited to 40 Unreal UTF-16 code units.")
        return value

    @model_validator(mode="after")
    def named_copy(self):
        if not (self.label_prefix.strip() or self.label_suffix.strip()):
            raise ValueError("Supply a nonblank label prefix or suffix for the copy.")
        return self


MeshRecipe = Annotated[ReplaceMeshRecipe | DuplicateMeshRecipe, Field(discriminator="kind")]
_RECIPE = TypeAdapter(MeshRecipe)


def compile_mesh(recipe: MeshRecipe, actor_details: object) -> dict:
    """Prepare mutation inputs from exact source identities; native preview resolves assets."""
    recipe = _RECIPE.validate_python(recipe)
    capability = "replace_mesh" if recipe.kind == "replace" else "duplicate_mesh"
    capabilities = actor_details.get("capabilities") if isinstance(actor_details, dict) else None
    if not isinstance(capabilities, list) or capability not in capabilities:
        raise JevError(
            "capability_unavailable",
            "Mesh workflows require the matching JevEditor 0.5 plugin. Rebuild and relaunch it.",
        )
    details = _normalize(actor_details, recipe.actor_paths)
    operations, measurements = [], []
    try:
        for actor in details["actors"]:
            if (
                actor["class"] != "/Script/Engine.StaticMeshActor"
                or actor["editable"] is not True
                or actor["edit_blockers"]
                or not actor["mesh_state_available"]
            ):
                raise ValueError("Only complete editable native static mesh actors are supported.")
            source = mesh_state(actor, actor=True).model_dump(mode="json")
            if isinstance(recipe, ReplaceMeshRecipe):
                operation = {
                    "op": "replace_mesh",
                    "actor_path": actor["path"],
                    "asset_path": recipe.asset_path,
                    "material_policy": recipe.material_policy,
                }
            else:
                operation = {
                    "op": "duplicate_mesh",
                    "actor_path": actor["path"],
                    "label": recipe.label_prefix + actor["label"] + recipe.label_suffix,
                    "location": [
                        old + offset
                        for old, offset in zip(actor["location"], recipe.offset_cm, strict=True)
                    ],
                    "rotation": actor["rotation"],
                    "scale": actor["scale"],
                }
            # Validate calculated locations and complete generated labels, without truncation.
            operation = Operation.model_validate(operation).model_dump(
                mode="json", exclude_none=True
            )
            operations.append(operation)
            measurements.append(
                {
                    "actor_path": actor["path"],
                    "source_instance_id": actor["instance_id"],
                    "before": {
                        **source,
                        **{key: actor[key] for key in ("location", "rotation", "scale")},
                    },
                    "bounds_available": actor["bounds_available"],
                    "bounds_cm": actor["bounds_cm"],
                }
            )
    except (ValueError, TypeError, ValidationError):
        raise JevError(
            "invalid_mesh_recipe",
            "Mesh recipes require complete supported native actors, bounded transforms, "
            "and labels of at most 80 Unreal UTF-16 code units. Inspect edit blockers.",
        ) from None
    return {
        "kind": recipe.kind,
        "recipe": recipe.model_dump(mode="json"),
        "units": "centimeters; rotation [pitch,yaw,roll] degrees",
        "measurement_state": {
            key: details[key]
            for key in ("project_file", "session_id", "world_path", "current_level", "revision")
        },
        "measurements": measurements,
        "operations": operations,
        "operation_count": len(operations),
        "applied": False,
        "cloud_used": False,
        "verification_checks": [],
        "note": (
            "Review the native normalized targets and material policy before a separate apply. "
            "Duplicate creates one native mesh actor per source using only declared settings. "
            "Fresh checks and visual review are separate; no map is saved."
        ),
    }


async def preview_mesh(bridge: UnrealBridge, previews: PreviewTracker, recipe: MeshRecipe) -> dict:
    recipe = _RECIPE.validate_python(recipe)
    details = await bridge.call("actor_details", {"actor_paths": recipe.actor_paths})
    _check_project(bridge, details)
    proposal = compile_mesh(recipe, details)
    state = proposal["measurement_state"]
    preview = await previews.preview(
        proposal["operations"],
        expected_state={key: state[key] for key in ("session_id", "world_path", "revision")},
    )
    targets, checks = [], []
    normalized = preview.get("operations")
    try:
        if not isinstance(normalized, list) or len(normalized) != len(proposal["operations"]):
            raise ValueError("Incomplete normalized operations.")
        for requested, measured, operation in zip(
            proposal["operations"], proposal["measurements"], normalized, strict=True
        ):
            if (
                not isinstance(operation, dict)
                or operation.get("op") != requested["op"]
                or operation.get("actor_path") != requested["actor_path"]
                or operation.get("source_instance_id") != measured["source_instance_id"]
            ):
                raise ValueError("Native preview source identity differs from inspection.")
            target = mesh_state(operation).model_dump(mode="json")
            source = measured["before"]
            desired = {
                **source,
                "path": requested["actor_path"],
                "instance_id": measured["source_instance_id"],
                "class": "/Script/Engine.StaticMeshActor",
                "static_mesh_path": source["asset_path"],
                "materials_truncated": False,
            }
            if recipe.kind == "replace":
                if (
                    operation.get("asset_path") != requested["asset_path"]
                    or operation.get("material_policy") != requested["material_policy"]
                ):
                    raise ValueError("Native preview differs from the requested material policy.")
                desired["static_mesh_path"] = requested["asset_path"]
                if requested["material_policy"] == "preserve_slots":
                    if target["material_slot_count"] != source["material_slot_count"]:
                        raise ValueError("Preserving slots requires equal slot counts.")
                    desired["materials"] = [
                        {**item, "override_path": item["path"]} for item in source["materials"]
                    ]
                    desired["material_override_count"] = source["material_slot_count"]
                else:
                    if target["material_override_count"] != 0 or any(
                        item["override_path"] is not None for item in target["materials"]
                    ):
                        raise ValueError("Mesh defaults must clear every material override.")
                    # Effective defaults are resolved by native asset inspection, not invented.
                    desired["materials"] = target["materials"]
                    desired["material_slot_count"] = target["material_slot_count"]
                    desired["material_override_count"] = 0
                checks.extend(
                    mesh_checks(operation, requested["actor_path"], measured["source_instance_id"])
                )
            else:
                if operation.get("source_actor_path") != requested["actor_path"]:
                    raise ValueError("Native duplicate source does not match inspection.")
                desired.update(
                    {key: requested[key] for key in ("label", "location", "rotation", "scale")}
                )
                # An internal candidate identity is used only for comparing the reviewed state.
                # Real duplicate paths/IDs are learned from the separate apply result.
                desired["path"] = requested["actor_path"] + "_ReviewCandidate"
                desired["instance_id"] = "review-candidate-" + str(len(targets))
            if verify_readback([operation], [desired])["status"] != "passed":
                raise ValueError("Native preview differs from the requested source state.")
            targets.append(
                {
                    "source_actor_path": requested["actor_path"],
                    "source_instance_id": measured["source_instance_id"],
                    "after": {
                        **target,
                        **{key: operation[key] for key in ("location", "rotation", "scale")},
                    },
                }
            )
    except (ValueError, TypeError, KeyError):
        raise JevError(
            "invalid_mesh_preview",
            "Native mesh preview is incomplete or disagrees with the measured source. "
            "Do not apply it; inspect the scene and create a new preview.",
        ) from None
    return {
        **proposal,
        "preview": preview,
        "verification_targets": deepcopy(targets),
        "verification_checks": checks,
        "verification_note": (
            "After apply, run its verification_checks with fresh unreal_verify. "
            "Duplicate checks require the newly confirmed actor paths and instance IDs; "
            "they are returned by unreal_apply only when native readback verification passes."
        ),
    }
