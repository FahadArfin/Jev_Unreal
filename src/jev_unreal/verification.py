"""Selected-actor snapshots and deterministic checks; no files, cloud, or scene edits."""

import json
import math
import time
import uuid
from collections import OrderedDict
from collections.abc import Callable
from copy import deepcopy
from dataclasses import dataclass
from typing import Annotated, Literal

from pydantic import (
    AfterValidator,
    BaseModel,
    ConfigDict,
    Field,
    TypeAdapter,
    ValidationError,
    model_validator,
)

from .bridge import UnrealBridge, project_identity
from .errors import JevError
from .mesh_state import MeshSettings, MeshState, mesh_state

MAX_SNAPSHOT_BYTES = 2 * 1024 * 1024
SCOPE = (
    "Only the explicitly selected, currently loaded actors and reported fields are inspected. "
    "No scene-wide additions/removals, asset contents, gameplay, physics, "
    "or visual quality are proved."
)
Finite = Annotated[float, Field(strict=True, allow_inf_nan=False)]
Vector = Annotated[list[Finite], Field(min_length=3, max_length=3)]
Text = Annotated[str, Field(max_length=2048, strict=True)]


def _identity_value(value: str) -> str:
    if not value.strip() or any(
        ord(character) < 32 or ord(character) == 127 for character in value
    ):
        raise ValueError("Identity values must be nonblank and contain no control characters.")
    try:
        value.encode("utf-8")
    except UnicodeEncodeError:
        raise ValueError("Identity values must contain valid Unicode.") from None
    return value


IdentityText = Annotated[
    str, Field(min_length=1, max_length=2048, strict=True), AfterValidator(_identity_value)
]
ActorPath = Annotated[
    str, Field(min_length=1, max_length=1024, strict=True), AfterValidator(_identity_value)
]
InstanceID = Annotated[
    str, Field(min_length=1, max_length=128, strict=True), AfterValidator(_identity_value)
]
ToleranceCM = Annotated[float, Field(ge=0, le=100, strict=True, allow_inf_nan=False)]
Coordinate = Annotated[float, Field(ge=-1e12, le=1e12, strict=True, allow_inf_nan=False)]
Size = Annotated[float, Field(ge=0, le=1e12, strict=True, allow_inf_nan=False)]


class SessionIdentity(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True, revalidate_instances="always")
    project_file: IdentityText
    session_id: IdentityText
    world_path: IdentityText
    current_level: IdentityText | None = None


class _Bounds(BaseModel):
    model_config = ConfigDict(extra="ignore", strict=True, allow_inf_nan=False)
    min: Vector
    max: Vector
    center: Vector
    size: Vector

    @model_validator(mode="after")
    def consistent(self):
        for low, high, center, size in zip(self.min, self.max, self.center, self.size, strict=True):
            extent, midpoint = high - low, low / 2 + high / 2
            if (
                low > high
                or size < 0
                or not math.isfinite(extent)
                or not math.isclose(size, extent, rel_tol=1e-7, abs_tol=1e-4)
                or not math.isclose(center, midpoint, rel_tol=1e-7, abs_tol=1e-4)
            ):
                raise ValueError("Bounds must contain consistent world-axis dimensions.")
        return self


class _Material(BaseModel):
    model_config = ConfigDict(extra="ignore", strict=True)
    slot: int = Field(ge=0, le=63)
    path: Text | None
    override_path: Text | None = None


class _Actor(BaseModel):
    model_config = ConfigDict(extra="ignore", strict=True, allow_inf_nan=False)
    path: ActorPath
    instance_id: InstanceID
    class_name: IdentityText = Field(alias="class")
    label: Text
    folder: Text
    location: Vector
    rotation: Vector
    scale: Vector
    static_mesh_path: Text | None
    collision_enabled: bool | None
    materials: list[_Material] = Field(max_length=64)
    material_slot_count: int | None = Field(default=None, ge=0, le=2_147_483_647)
    material_override_count: int | None = Field(default=None, ge=0, le=2_147_483_647)
    mesh_settings: MeshSettings | None = None
    materials_truncated: bool
    bounds_available: bool
    bounds_cm: _Bounds | None
    editable: bool
    edit_blockers: list[Text] = Field(max_length=32)

    @model_validator(mode="after")
    def complete(self):
        if len({item.slot for item in self.materials}) != len(self.materials):
            raise ValueError("Material slots must be unique.")
        if self.material_slot_count is not None:
            count = self.material_slot_count
            if (
                any(item.slot >= count for item in self.materials)
                or (self.materials_truncated and count <= len(self.materials))
                or (not self.materials_truncated and count != len(self.materials))
            ):
                raise ValueError("Material slot count must agree with observed/truncated slots.")
        if self.bounds_available != (self.bounds_cm is not None):
            raise ValueError("Bounds availability must match the supplied bounds.")
        return self


class _Details(BaseModel):
    model_config = ConfigDict(extra="ignore", strict=True)
    project_file: IdentityText
    session_id: IdentityText
    world_path: IdentityText
    current_level: IdentityText
    revision: IdentityText
    truncated: bool
    actors: list[_Actor] = Field(min_length=1, max_length=20)


def _paths(actor_paths: list[str]) -> list[str]:
    if not isinstance(actor_paths, list) or not 1 <= len(actor_paths) <= 20:
        raise JevError("invalid_request", "Supply 1 to 20 unique exact actor paths.")
    try:
        parsed = TypeAdapter(list[ActorPath]).validate_python(actor_paths, strict=True)
    except (ValueError, TypeError):
        raise JevError("invalid_request", "Supply 1 to 20 unique exact actor paths.") from None
    if (
        not 1 <= len(parsed) <= 20
        or len(set(parsed)) != len(parsed)
        or any(not path.strip() for path in parsed)
    ):
        raise JevError("invalid_request", "Supply 1 to 20 unique exact actor paths.")
    return parsed


def _normalize(
    result: object, paths: list[str], *, require_complete_materials: bool = True
) -> dict:
    try:
        details = _Details.model_validate(result)
        if details.truncated or any(
            result.get(flag) is not False
            for flag in ("actors_truncated", "scan_incomplete")
            if flag in result
        ):
            raise ValueError("Incomplete actor inspection")
        if [actor.path for actor in details.actors] != paths:
            raise ValueError("Actor inspection must match the exact requested order and selection")
        if require_complete_materials and any(
            actor.materials_truncated for actor in details.actors
        ):
            raise ValueError("Snapshots and diffs require complete material records.")
        normalized = details.model_dump(by_alias=True, mode="json")
        # Material order is not semantic; slot identity is. Blocker ordering is also not semantic.
        for actor, parsed_actor in zip(normalized["actors"], details.actors, strict=True):
            actor["mesh_state_available"] = (
                parsed_actor.mesh_settings is not None
                and parsed_actor.material_override_count is not None
                and parsed_actor.material_slot_count is not None
                and all("override_path" in item.model_fields_set for item in parsed_actor.materials)
            )
            actor["materials"].sort(key=lambda item: item["slot"])
            actor["edit_blockers"] = sorted(set(actor["edit_blockers"]))
        encoded = json.dumps(normalized, allow_nan=False, ensure_ascii=False).encode("utf-8")
        if len(encoded) > MAX_SNAPSHOT_BYTES:
            raise ValueError("Actor inspection exceeds the bounded data budget")
        return normalized
    except (ValidationError, ValueError, TypeError, AttributeError, OverflowError):
        raise JevError(
            "invalid_actor_details", "Actor details are incomplete or inconsistent."
        ) from None


def _identity(details: dict) -> dict:
    return {
        key: details[key] for key in ("project_file", "session_id", "world_path", "current_level")
    }


def _check_project(bridge: UnrealBridge, details: dict):
    expected = getattr(getattr(bridge, "settings", None), "expected_project", None)
    if isinstance(expected, str) and expected and isinstance(details, dict):
        actual = details.get("project_file")
        if isinstance(actual, str) and project_identity(actual) != project_identity(expected):
            raise JevError("wrong_project", "Actor details do not match the configured project.")


def _same_session(left: dict, right: dict) -> bool:
    return (
        project_identity(left["project_file"]) == project_identity(right["project_file"])
        and left["session_id"] == right["session_id"]
        and left["world_path"] == right["world_path"]
    )


def _error_code(exc: JevError) -> str:
    known = {
        "actor_not_found",
        "wrong_project",
        "editor_unavailable",
        "bridge_error",
        "rate_limited",
        "unauthorized",
        "forbidden",
        "missing_bridge_token",
        "play_mode",
        "invalid_actor_details",
        "snapshot_expired",
        "snapshot_not_found",
        "project_required",
        "capability_unavailable",
        "response_too_large",
        "actor_bounds_unavailable",
    }
    return exc.code if exc.code in known else "inspection_unavailable"


def _error_guidance(reason: str) -> dict:
    if reason == "capability_unavailable":
        return {
            "next_step": "Rebuild and relaunch the matching JevEditor plugin for actor inspection."
        }
    return {}


@dataclass
class _Snapshot:
    expires_at: float
    details: dict
    size: int


class SceneSnapshots:
    """Process-local selected-actor baselines with hard count, lifetime and byte limits."""

    def __init__(
        self,
        bridge: UnrealBridge,
        *,
        max_snapshots: int = 32,
        ttl_seconds: float = 900,
        max_bytes: int = MAX_SNAPSHOT_BYTES,
        clock: Callable[[], float] = time.monotonic,
    ):
        if (
            type(max_snapshots) is not int
            or not 1 <= max_snapshots <= 32
            or type(max_bytes) is not int
            or not 1 <= max_bytes <= MAX_SNAPSHOT_BYTES
            or type(ttl_seconds) not in (int, float)
            or not math.isfinite(ttl_seconds)
            or not 0 < ttl_seconds <= 900
        ):
            raise ValueError("Snapshot limits cannot exceed 32 entries, 900 seconds or 2 MiB.")
        self.bridge, self.max_snapshots = bridge, max_snapshots
        self.ttl_seconds, self.max_bytes, self.clock = ttl_seconds, max_bytes, clock
        self._snapshots: OrderedDict[str, _Snapshot] = OrderedDict()
        self._bytes = 0

    def _remove(self, snapshot_id: str):
        value = self._snapshots.pop(snapshot_id)
        self._bytes -= value.size

    def _purge(self):
        now = self.clock()
        for key in [key for key, value in self._snapshots.items() if value.expires_at <= now]:
            self._remove(key)

    def _get(self, snapshot_id: str) -> _Snapshot:
        if not isinstance(snapshot_id, str) or not 1 <= len(snapshot_id) <= 64:
            raise JevError("snapshot_not_found", "Snapshot is not present in this process.")
        value = self._snapshots.get(snapshot_id)
        if value is None:
            raise JevError("snapshot_not_found", "Snapshot is not present in this process.")
        if value.expires_at <= self.clock():
            self._remove(snapshot_id)
            raise JevError("snapshot_expired", "Snapshot expired; capture a new baseline.")
        return value

    def status(self) -> dict:
        self._purge()
        return {
            "count": len(self._snapshots),
            "stored_bytes": self._bytes,
            "max_snapshots": self.max_snapshots,
            "max_bytes": self.max_bytes,
            "ttl_seconds": self.ttl_seconds,
        }

    async def capture(self, actor_paths: list[str]) -> dict:
        paths = _paths(actor_paths)
        readback = await self.bridge.call("actor_details", {"actor_paths": paths})
        _check_project(self.bridge, readback)
        details = _normalize(readback, paths)
        size = len(json.dumps(details, allow_nan=False, ensure_ascii=False).encode("utf-8"))
        if size > self.max_bytes:
            raise JevError("snapshot_too_large", "Selected actor data exceeds the snapshot budget.")
        self._purge()
        evicted = 0
        while len(self._snapshots) >= self.max_snapshots or self._bytes + size > self.max_bytes:
            self._remove(next(iter(self._snapshots)))
            evicted += 1
        snapshot_id = uuid.uuid4().hex
        self._snapshots[snapshot_id] = _Snapshot(
            self.clock() + self.ttl_seconds, deepcopy(details), size
        )
        self._bytes += size
        return {
            "snapshot_id": snapshot_id,
            "identity": _identity(details),
            "revision": details["revision"],
            "actor_paths": paths,
            "actors": deepcopy(details["actors"]),
            "expires_in_seconds": self.ttl_seconds,
            "evicted_snapshots": evicted,
            "stored_bytes": size,
            "scope": SCOPE,
            "cloud_used": False,
            "scene_modified": False,
        }

    def _unverifiable(self, snapshot_id: str, reason: str) -> dict:
        return {
            "snapshot_id": snapshot_id,
            "status": "unverifiable",
            "reason": reason,
            **_error_guidance(reason),
            "added": [],
            "removed": [],
            "changed": [],
            "scope": SCOPE,
            "cloud_used": False,
            "scene_modified": False,
        }

    def compare(self, snapshot_id: str, actor_details_result: dict) -> dict:
        try:
            baseline = self._get(snapshot_id).details
            paths = [actor["path"] for actor in baseline["actors"]]
            current = _normalize(actor_details_result, paths)
        except JevError as exc:
            return self._unverifiable(snapshot_id, _error_code(exc))
        if not _same_session(baseline, current):
            return self._unverifiable(snapshot_id, "identity_changed")
        replaced = [
            {
                "actor_path": before["path"],
                "before_instance_id": before["instance_id"],
                "after_instance_id": after["instance_id"],
            }
            for before, after in zip(baseline["actors"], current["actors"], strict=True)
            if before["instance_id"] != after["instance_id"]
        ]
        if replaced:
            return {**self._unverifiable(snapshot_id, "actor_replaced"), "replaced": replaced}
        added, removed, changed = [], [], []
        for before, after in zip(baseline["actors"], current["actors"], strict=True):
            for field in before:
                if field == "materials":
                    previous = {item["slot"]: item["path"] for item in before[field]}
                    now = {item["slot"]: item["path"] for item in after[field]}
                    for slot in sorted(previous.keys() | now.keys()):
                        item = {
                            "actor_path": before["path"],
                            "field": "materials",
                            "slot": slot,
                            "before": previous.get(slot),
                            "after": now.get(slot),
                            "before_exists": slot in previous,
                            "after_exists": slot in now,
                        }
                        if slot not in previous:
                            added.append(item)
                        elif slot not in now:
                            removed.append(item)
                        elif previous[slot] != now[slot]:
                            changed.append(item)
                    previous_overrides = {
                        item["slot"]: item["override_path"] for item in before[field]
                    }
                    now_overrides = {item["slot"]: item["override_path"] for item in after[field]}
                    for slot in sorted(previous_overrides.keys() & now_overrides.keys()):
                        if previous_overrides[slot] != now_overrides[slot]:
                            changed.append(
                                {
                                    "actor_path": before["path"],
                                    "field": "material_overrides",
                                    "slot": slot,
                                    "before": previous_overrides[slot],
                                    "after": now_overrides[slot],
                                }
                            )
                elif before[field] != after[field]:
                    changed.append(
                        {
                            "actor_path": before["path"],
                            "field": field,
                            "before": deepcopy(before[field]),
                            "after": deepcopy(after[field]),
                        }
                    )
        context_changes = []
        if baseline["current_level"] != current["current_level"]:
            context_changes.append(
                {
                    "field": "current_level",
                    "before": baseline["current_level"],
                    "after": current["current_level"],
                }
            )
        return {
            "snapshot_id": snapshot_id,
            "status": "changed" if added or removed or changed or context_changes else "unchanged",
            "identity": _identity(current),
            "baseline_revision": baseline["revision"],
            "current_revision": current["revision"],
            "added": added,
            "removed": removed,
            "changed": changed,
            "context_changes": context_changes,
            "scope": SCOPE,
            "cloud_used": False,
            "scene_modified": False,
            "note": (
                "Added/removed entries describe selected actors' material slots, "
                "not scene-wide actors."
            ),
        }

    async def diff(self, snapshot_id: str) -> dict:
        try:
            baseline = self._get(snapshot_id).details
            paths = [actor["path"] for actor in baseline["actors"]]
            current = await self.bridge.call("actor_details", {"actor_paths": paths})
            _check_project(self.bridge, current)
        except JevError as exc:
            return self._unverifiable(snapshot_id, _error_code(exc))
        return self.compare(snapshot_id, current)


class _CheckBase(BaseModel):
    model_config = ConfigDict(
        extra="forbid", strict=True, allow_inf_nan=False, revalidate_instances="always"
    )
    id: str | None = Field(default=None, min_length=1, max_length=64)


class _ActorCheck(_CheckBase):
    actor_path: ActorPath
    expected_instance_id: InstanceID | None = None


class TransformEquals(_ActorCheck):
    kind: Literal["transform_equals"]
    location: Annotated[list[Coordinate], Field(min_length=3, max_length=3)] | None = None
    rotation: Vector | None = None
    scale: Vector | None = None
    location_tolerance_cm: ToleranceCM = 0.1
    rotation_tolerance_degrees: float = Field(default=0.1, ge=0, le=5, strict=True)
    scale_tolerance: float = Field(default=0.001, ge=0, le=0.1, strict=True)

    @model_validator(mode="after")
    def at_least_one(self):
        if self.location is None and self.rotation is None and self.scale is None:
            raise ValueError("Specify at least one transform component.")
        return self


class BoundsSize(_ActorCheck):
    kind: Literal["bounds_size"]
    expected_cm: Annotated[list[Size], Field(min_length=3, max_length=3)]
    tolerance_cm: ToleranceCM = 0.1


class BoundsAnchor(_ActorCheck):
    kind: Literal["bounds_anchor"]
    axis: Literal["x", "y", "z"]
    anchor: Literal["min", "center", "max"]
    expected_cm: Coordinate
    tolerance_cm: ToleranceCM = 0.1


class BottomZ(_ActorCheck):
    kind: Literal["bottom_z"]
    expected_cm: Coordinate
    tolerance_cm: ToleranceCM = 0.1


class MaterialSlot(_ActorCheck):
    kind: Literal["material_slot"]
    slot: int = Field(ge=0, le=63)
    expected_path: Text | None


class LabelEquals(_ActorCheck):
    kind: Literal["label"]
    expected: Text


class FolderEquals(_ActorCheck):
    kind: Literal["folder"]
    expected: Text


class MeshEquals(_ActorCheck):
    """Exact reviewed mesh, effective/override slots, declared settings and metadata."""

    kind: Literal["mesh"]
    expected: MeshState


class MinimumGap(_CheckBase):
    kind: Literal["min_gap"]
    first_actor_path: ActorPath
    second_actor_path: ActorPath
    first_expected_instance_id: InstanceID | None = None
    second_expected_instance_id: InstanceID | None = None
    axis: Literal["x", "y", "z"]
    minimum_cm: Size
    tolerance_cm: ToleranceCM = 0.1

    @model_validator(mode="after")
    def distinct(self):
        if self.first_actor_path == self.second_actor_path:
            raise ValueError("A gap check requires two different actors.")
        return self


Check = Annotated[
    TransformEquals
    | BoundsSize
    | BoundsAnchor
    | BottomZ
    | MaterialSlot
    | LabelEquals
    | FolderEquals
    | MeshEquals
    | MinimumGap,
    Field(discriminator="kind"),
]
_CHECKS = TypeAdapter(list[Check])


def _checks(checks: list[Check]) -> tuple[list[Check], list[str]]:
    if not isinstance(checks, list) or not 1 <= len(checks) <= 64:
        raise JevError("invalid_request", "Supply 1 to 64 typed verification checks.")
    try:
        parsed = _CHECKS.validate_python(checks)
    except (ValidationError, ValueError, TypeError):
        raise JevError(
            "invalid_request", "Verification checks violate the bounded schema."
        ) from None
    ids = [check.id or f"check_{index + 1}" for index, check in enumerate(parsed)]
    if len(set(ids)) != len(ids):
        raise JevError("invalid_request", "Verification check IDs must be unique.")
    paths = []
    for check in parsed:
        selected = (
            [check.first_actor_path, check.second_actor_path]
            if isinstance(check, MinimumGap)
            else [check.actor_path]
        )
        paths.extend(path for path in selected if path not in paths)
    return parsed, _paths(paths)


def _orientation_delta(actual: list[float], expected: list[float]) -> float:
    def quaternion(angles):
        pitch, yaw, roll = [math.radians(value) / 2 for value in angles]
        sp, sy, sr = math.sin(pitch), math.sin(yaw), math.sin(roll)
        cp, cy, cr = math.cos(pitch), math.cos(yaw), math.cos(roll)
        return (
            cr * sp * sy - sr * cp * cy,
            -cr * sp * cy - sr * cp * sy,
            cr * cp * sy - sr * sp * cy,
            cr * cp * cy + sr * sp * sy,
        )

    a, b = quaternion(actual), quaternion(expected)
    dot = abs(sum(x * y for x, y in zip(a, b, strict=True)))
    norm = math.sqrt(sum(x * x for x in a) * sum(x * x for x in b))
    return math.degrees(2 * math.acos(min(1.0, dot / norm)))


def _check_one(check: Check, actors: dict[str, dict]) -> dict:
    expected, actual, passed = None, None, False
    if isinstance(check, MinimumGap):
        first, second = actors[check.first_actor_path], actors[check.second_actor_path]
        for actor, expected_id in (
            (first, check.first_expected_instance_id),
            (second, check.second_expected_instance_id),
        ):
            if expected_id is not None and actor["instance_id"] != expected_id:
                return {
                    "status": "unverifiable",
                    "reason": "actor_replaced",
                    "actor_path": actor["path"],
                    "expected": expected_id,
                    "actual": actor["instance_id"],
                }
        expected = {
            "minimum_cm": check.minimum_cm,
            "axis": check.axis,
            "tolerance_cm": check.tolerance_cm,
        }
        if not first["bounds_available"] or not second["bounds_available"]:
            return {
                "status": "unverifiable",
                "reason": "bounds_unavailable",
                "expected": expected,
                "actual": None,
            }
        axis = "xyz".index(check.axis)
        a, b = first["bounds_cm"], second["bounds_cm"]
        gap = max(b["min"][axis] - a["max"][axis], a["min"][axis] - b["max"][axis])
        if not math.isfinite(gap):
            return {
                "status": "unverifiable",
                "reason": "measurement_overflow",
                "expected": expected,
                "actual": None,
            }
        actual = {
            "gap_cm": gap,
            "axis": check.axis,
            "first_interval_cm": [a["min"][axis], a["max"][axis]],
            "second_interval_cm": [b["min"][axis], b["max"][axis]],
        }
        passed = gap + check.tolerance_cm >= check.minimum_cm
        return {
            "status": "passed" if passed else "failed",
            "expected": expected,
            "actual": actual,
            "scope": (
                "Separation of world-axis-aligned bounds along one axis, "
                "not exact geometry or collision clearance."
            ),
        }
    actor = actors[check.actor_path]
    if (
        check.expected_instance_id is not None
        and actor["instance_id"] != check.expected_instance_id
    ):
        return {
            "status": "unverifiable",
            "reason": "actor_replaced",
            "actor_path": actor["path"],
            "expected": check.expected_instance_id,
            "actual": actor["instance_id"],
        }
    if isinstance(check, TransformEquals):
        expected = {
            field: getattr(check, field)
            for field in ("location", "rotation", "scale")
            if getattr(check, field) is not None
        }
        actual = {field: actor[field] for field in expected}
        matches = []
        for field, target in expected.items():
            if field == "rotation":
                matches.append(
                    _orientation_delta(actual[field], target)
                    <= check.rotation_tolerance_degrees + 1e-9
                )
            else:
                tolerance = (
                    check.location_tolerance_cm if field == "location" else check.scale_tolerance
                )
                matches.append(
                    all(abs(a - b) <= tolerance for a, b in zip(actual[field], target, strict=True))
                )
        passed = all(matches)
    elif isinstance(check, (BoundsSize, BoundsAnchor, BottomZ)):
        expected = check.expected_cm
        if not actor["bounds_available"]:
            return {
                "status": "unverifiable",
                "reason": "bounds_unavailable",
                "expected": expected,
                "actual": None,
            }
        if isinstance(check, BoundsSize):
            actual = actor["bounds_cm"]["size"]
            passed = all(
                abs(a - b) <= check.tolerance_cm for a, b in zip(actual, expected, strict=True)
            )
        elif isinstance(check, BoundsAnchor):
            actual = actor["bounds_cm"][check.anchor]["xyz".index(check.axis)]
            passed = abs(actual - expected) <= check.tolerance_cm
        else:
            actual = actor["bounds_cm"]["min"][2]
            passed = abs(actual - expected) <= check.tolerance_cm
    elif isinstance(check, MeshEquals):
        expected = check.expected.model_dump(mode="json")
        if not actor["mesh_state_available"]:
            return {
                "status": "unverifiable",
                "reason": "mesh_state_unavailable",
                "expected": expected,
                "actual": None,
            }
        try:
            actual = mesh_state(actor, actor=True).model_dump(mode="json")
        except (ValueError, TypeError):
            return {
                "status": "unverifiable",
                "reason": "mesh_state_incomplete",
                "expected": expected,
                "actual": None,
            }
        expected["materials"].sort(key=lambda item: item["slot"])
        passed = actual == expected
    elif isinstance(check, MaterialSlot):
        expected = {"slot": check.slot, "path": check.expected_path}
        slot = next((item for item in actor["materials"] if item["slot"] == check.slot), None)
        if slot is None:
            return {
                "status": "unverifiable",
                "reason": "material_slot_unavailable",
                "expected": expected,
                "actual": {"slot": check.slot, "exists": False},
            }
        actual = {key: slot[key] for key in ("slot", "path")}
        passed = slot["path"] == check.expected_path
    else:
        expected, actual = check.expected, actor[check.kind]
        passed = expected == actual
    return {
        "status": "passed" if passed else "failed",
        "expected": deepcopy(expected),
        "actual": deepcopy(actual),
    }


def _unverifiable_checks(checks: list[Check], reason: str) -> dict:
    return {
        "status": "unverifiable",
        "reason": reason,
        **_error_guidance(reason),
        "checks": [
            {
                "id": check.id or f"check_{index + 1}",
                "kind": check.kind,
                "status": "unverifiable",
                "reason": reason,
                "expected": check.model_dump(mode="json", exclude_none=False),
                "actual": None,
            }
            for index, check in enumerate(checks)
        ],
        "scope": SCOPE,
        "cloud_used": False,
        "scene_modified": False,
    }


def verify(
    checks: list[Check],
    actor_details_result: dict,
    *,
    expected_identity: SessionIdentity | dict | None = None,
    expected_revision: str | None = None,
) -> dict:
    """Check complete supplied details; use verify_fresh for a current authenticated read."""
    parsed, paths = _checks(checks)
    try:
        details = _normalize(actor_details_result, paths, require_complete_materials=False)
    except JevError as exc:
        return _unverifiable_checks(parsed, _error_code(exc))
    if expected_identity is not None:
        try:
            identity = SessionIdentity.model_validate(expected_identity).model_dump()
        except (ValueError, TypeError):
            raise JevError(
                "invalid_request", "Expected identity requires project, session and world."
            ) from None
        if not _same_session(details, identity):
            return _unverifiable_checks(parsed, "identity_changed")
    if expected_revision is not None:
        try:
            TypeAdapter(IdentityText).validate_python(expected_revision)
        except (ValueError, TypeError):
            raise JevError(
                "invalid_request", "Expected revision must be a bounded nonempty string."
            ) from None
        if expected_revision != details["revision"]:
            return _unverifiable_checks(parsed, "revision_changed")
    actors = {actor["path"]: actor for actor in details["actors"]}
    results = [
        {"id": check.id or f"check_{index + 1}", "kind": check.kind, **_check_one(check, actors)}
        for index, check in enumerate(parsed)
    ]
    statuses = {result["status"] for result in results}
    status = (
        "failed"
        if "failed" in statuses
        else "unverifiable"
        if "unverifiable" in statuses
        else "passed"
    )
    return {
        "status": status,
        "identity": _identity(details),
        "revision": details["revision"],
        "checks": results,
        "actor_paths": paths,
        "scope": SCOPE,
        "cloud_used": False,
        "scene_modified": False,
        "evidence": (
            "Caller-supplied actor_details; freshness requires verify_fresh "
            "or an explicit revision check."
        ),
    }


async def verify_fresh(
    bridge: UnrealBridge,
    checks: list[Check],
    *,
    expected_identity: SessionIdentity | dict | None = None,
    expected_revision: str | None = None,
) -> dict:
    parsed, paths = _checks(checks)
    try:
        details = await bridge.call("actor_details", {"actor_paths": paths})
        _check_project(bridge, details)
    except JevError as exc:
        return _unverifiable_checks(parsed, _error_code(exc))
    result = verify(
        parsed, details, expected_identity=expected_identity, expected_revision=expected_revision
    )
    result["evidence"] = (
        "Fresh authenticated actor_details read for the exact selected actor paths."
    )
    return result
