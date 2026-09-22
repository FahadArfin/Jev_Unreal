"""The bounded mesh state explicitly copied/reviewed by the native editor bridge."""

from typing import Annotated, Literal

from pydantic import BaseModel, ConfigDict, Field, field_validator, model_validator

from .workflows import Operation

Text = Annotated[str, Field(max_length=1024, strict=True)]
Tag = Annotated[str, Field(max_length=128, strict=True)]


def valid_mesh_identity(value: object, *, actor_path: bool = False) -> bool:
    if (
        not isinstance(value, str)
        or not value.strip()
        or len(value) > (1024 if actor_path else 128)
        or any(ord(character) < 32 or ord(character) == 127 for character in value)
    ):
        return False
    try:
        value.encode("utf-8")
        if actor_path:
            return (
                value.startswith("/")
                and "." in value
                and len(value.encode("utf-16-le")) // 2 <= 1024
                and not any(character.isspace() for character in value)
                and not any(part in value for part in ("..", "\\", "*", "?"))
            )
    except UnicodeEncodeError:
        return False
    return True


class MeshSettings(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True, revalidate_instances="always")
    mobility: Literal["Static", "Stationary", "Movable"]
    collision_mode: int = Field(ge=0, le=5)
    collision_profile: str = Field(min_length=1, max_length=256)
    use_mesh_default_collision: bool
    collision_object_type: int = Field(ge=0, le=31)
    collision_responses: list[Annotated[int, Field(ge=0, le=2)]] = Field(
        min_length=32, max_length=32
    )
    actor_collision_enabled: bool
    cast_shadow: bool
    visible: bool
    hidden_in_game: bool
    actor_hidden_in_game: bool
    actor_hidden_in_editor: bool
    actor_tags: list[Tag] = Field(max_length=32)
    component_tags: list[Tag] = Field(max_length=32)
    tags_truncated: bool


class MeshMaterial(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True, revalidate_instances="always")
    slot: int = Field(ge=0, le=63)
    path: Text | None
    override_path: Text | None


class MeshState(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True, revalidate_instances="always")
    asset_path: str = Field(min_length=1, max_length=512)
    label: str = Field(min_length=1, max_length=80)
    folder: str = Field(max_length=256)
    materials: list[MeshMaterial] = Field(max_length=64)
    material_slot_count: int = Field(ge=0, le=64)
    material_override_count: int = Field(ge=0, le=64)
    mesh_settings: MeshSettings

    @field_validator("asset_path")
    @classmethod
    def exact_asset(cls, value: str) -> str:
        return Operation.exact_asset_path(value)

    @field_validator("label")
    @classmethod
    def label_value(cls, value: str) -> str:
        return Operation.valid_label(value)

    @field_validator("folder")
    @classmethod
    def folder_value(cls, value: str) -> str:
        return Operation.valid_folder(value)

    @model_validator(mode="after")
    def complete_materials(self):
        if self.mesh_settings.tags_truncated:
            raise ValueError("Complete actor/component tag records are required.")
        if sorted(item.slot for item in self.materials) != list(range(self.material_slot_count)):
            raise ValueError("Mesh material slots must be complete, contiguous and unique.")
        if self.material_override_count > self.material_slot_count or any(
            item.override_path is not None and item.slot >= self.material_override_count
            for item in self.materials
        ):
            raise ValueError("Override count must agree with reported override slots.")
        return self


def mesh_state(value: dict, *, actor: bool = False) -> MeshState:
    """Require every reviewed field; absence never becomes a matching default."""
    selected = {key: value[key] for key in MeshState.model_fields if key in value}
    if actor:
        selected["asset_path"] = value.get("static_mesh_path")
        if value.get("materials_truncated") is not False:
            raise ValueError("A complete material readback is required.")
    return MeshState.model_validate(selected)


def mesh_checks(operation: dict, actor_path: str, instance_id: str) -> list[dict]:
    """Generate explicit requirements, with actor identity supplied by reviewed evidence."""
    state = mesh_state(operation).model_dump(mode="json")
    return [
        {
            "kind": "mesh",
            "actor_path": actor_path,
            "expected_instance_id": instance_id,
            "expected": state,
        },
        {
            "kind": "transform_equals",
            "actor_path": actor_path,
            "expected_instance_id": instance_id,
            **{key: operation[key] for key in ("location", "rotation", "scale")},
        },
    ]
