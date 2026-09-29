"""Explicit Blender/Unreal contracts; local file checks never go through the editor bridge."""

import hashlib
import math
from pathlib import Path, PurePosixPath
from typing import Annotated, Any, Literal

from mcp.server.fastmcp import FastMCP
from mcp.types import ToolAnnotations
from pydantic import Field, field_validator, model_validator

from .domain_workflows import Strict
from .errors import JevError
from .setup import _absolute, _read

Number = Annotated[float, Field(ge=-10_000_000, le=10_000_000, allow_inf_nan=False)]
Vector = Annotated[list[Number], Field(min_length=3, max_length=3)]
Identifier = Annotated[str, Field(min_length=1, max_length=128, pattern=r"^[A-Za-z0-9_.-]+$")]


class HandoffFile(Strict):
    path: str = Field(min_length=1, max_length=512)
    sha256: str = Field(pattern=r"^[a-f0-9]{64}$")
    bytes: int = Field(ge=1, le=512 * 1024 * 1024)
    role: Literal["editable_source", "mesh", "texture"]

    @field_validator("path")
    @classmethod
    def relative(cls, value):
        parts = PurePosixPath(value).parts
        if (
            not parts
            or value != "/".join(parts)
            or value.startswith("/")
            or "\\" in value
            or ":" in value
            or any(p in {".", ".."} or p.endswith((" ", ".")) for p in parts)
        ):
            raise ValueError("Use a bundle-relative ordinary path without traversal.")
        return value


class HandoffAsset(Strict):
    asset_id: Identifier
    unreal_asset_path: str = Field(pattern=r"^/Game/[A-Za-z0-9_/]+\.[A-Za-z0-9_]+$", max_length=512)
    mesh_file: str = Field(min_length=1, max_length=512)
    bounds_size_cm: Vector
    bounds_center_cm: Vector
    material_slots: list[Identifier] = Field(max_length=64)

    @field_validator("bounds_size_cm")
    @classmethod
    def positive_bounds(cls, value):
        if any(v <= 0 for v in value):
            raise ValueError("Expected dimensions must be positive.")
        return value


class HandoffManifest(Strict):
    version: Literal[1] = 1
    coordinate_contract: Literal["unreal_local_centimeters_z_up"]
    producer: str = Field(min_length=1, max_length=128)
    source_license: str = Field(min_length=1, max_length=256)
    provenance: str = Field(min_length=1, max_length=2048)
    files: list[HandoffFile] = Field(min_length=2, max_length=128)
    assets: list[HandoffAsset] = Field(min_length=1, max_length=32)

    @model_validator(mode="after")
    def references(self):
        paths = [f.path.casefold() for f in self.files]
        if len(set(paths)) != len(paths):
            raise ValueError("Bundle file paths must be unique even on Windows.")
        if not any(f.role == "editable_source" for f in self.files):
            raise ValueError("An editable source must be retained.")
        if any(
            not f.path.lower().endswith(".blend") for f in self.files if f.role == "editable_source"
        ):
            raise ValueError("Editable Blender sources require .blend files.")
        meshes = {f.path for f in self.files if f.role == "mesh"}
        if any(a.mesh_file not in meshes for a in self.assets):
            raise ValueError("Each asset must reference a recorded mesh file.")
        for values in (
            [a.asset_id for a in self.assets],
            [a.unreal_asset_path.casefold() for a in self.assets],
        ):
            if len(set(values)) != len(values):
                raise ValueError("Asset IDs and target paths must be unique.")
        if sum(f.bytes for f in self.files) > 2 * 1024**3:
            raise ValueError("Bundle exceeds the two GiB verification budget.")
        return self


def read_manifest(path: str | Path) -> HandoffManifest:
    return HandoffManifest.model_validate_json(_read(_absolute(path), 1024 * 1024))


def inspect_bundle(manifest: HandoffManifest, root: str | Path) -> dict:
    """Hash only explicitly enumerated regular local files, with bounded streaming reads."""
    root = _absolute(root)
    if not root.is_dir():
        raise JevError("invalid_bundle", "Select an existing local bundle directory.")
    rows = []
    for entry in manifest.files:
        path = _absolute(root / entry.path)
        try:
            path.relative_to(root)
            if not path.is_file() or path.stat().st_size != entry.bytes:
                rows.append({"path": entry.path, "status": "missing_or_size_mismatch"})
                continue
            digest = hashlib.sha256()
            remaining = entry.bytes
            with path.open("rb") as handle:
                while remaining:
                    part = handle.read(min(1024 * 1024, remaining))
                    if not part:
                        break
                    digest.update(part)
                    remaining -= len(part)
                grew = bool(handle.read(1))
            matches = not remaining and not grew and digest.hexdigest() == entry.sha256
            rows.append({"path": entry.path, "status": "verified" if matches else "hash_mismatch"})
        except OSError:
            rows.append({"path": entry.path, "status": "unreadable"})
    return {
        "status": "passed" if all(r["status"] == "verified" for r in rows) else "failed",
        "files": rows,
        "editable_source_recorded": True,
        "license": manifest.source_license,
        "scope": "Byte identity and file availability only; license is an author declaration.",
    }


def compare_asset(contract: HandoffAsset, observed: dict, tolerance_cm: float = 0.1) -> dict:
    if (
        type(tolerance_cm) not in (int, float)
        or not math.isfinite(tolerance_cm)
        or not 0 <= tolerance_cm <= 10
    ):
        raise JevError("invalid_tolerance", "Use a finite tolerance between zero and ten cm.")
    if observed.get("asset_path") != contract.unreal_asset_path:
        raise JevError("asset_mismatch", "Observation must identify the exact contracted asset.")
    checks = []
    for field in ("bounds_size_cm", "bounds_center_cm"):
        value = observed.get(field)
        valid = (
            isinstance(value, list)
            and len(value) == 3
            and all(type(v) in (int, float) and math.isfinite(v) for v in value)
        )
        expected = getattr(contract, field)
        checks.append(
            {
                "check": field,
                "expected": expected,
                "observed": value if valid else None,
                "status": "unverifiable"
                if not valid
                else "passed"
                if all(abs(a - b) <= tolerance_cm for a, b in zip(expected, value, strict=True))
                else "failed",
            }
        )
    slots = observed.get("material_slot_names")
    checks.append(
        {
            "check": "material_slots",
            "expected": contract.material_slots,
            "observed": slots,
            "status": "unverifiable"
            if not isinstance(slots, list)
            else "passed"
            if slots == contract.material_slots
            else "failed",
        }
    )
    return {
        "asset_id": contract.asset_id,
        "asset_path": contract.unreal_asset_path,
        "status": "failed"
        if any(c["status"] == "failed" for c in checks)
        else "unverifiable"
        if any(c["status"] == "unverifiable" for c in checks)
        else "passed",
        "checks": checks,
        "tolerance_cm": tolerance_cm,
        "scope": "Local mesh dimensions, pivot-relative bounds and ordered slot names. "
        "Does not establish texture appearance, collision or gameplay quality.",
    }


def register_handoff_tools(server: FastMCP, bridge) -> None:
    @server.tool(
        annotations=ToolAnnotations(readOnlyHint=True, destructiveHint=False, openWorldHint=False)
    )
    async def unreal_handoff_verify(
        asset: HandoffAsset,
        tolerance_cm: Annotated[float, Field(ge=0, le=10, allow_inf_nan=False)] = 0.1,
    ) -> dict[str, Any]:
        """Compare a Blender handoff contract with a fresh exact loaded Unreal mesh inspection.

        No file reads, imports or writes. Use local CLI handoff inspect to verify retained
        editable source/export/texture hashes separately. Contract measurements use Unreal
        local centimeters and Z-up; exporter conversion must be established independently.
        """
        try:
            observed = await bridge.call(
                "workflow_inspect",
                {
                    "kind": "asset_diagnosis",
                    "target_path": asset.unreal_asset_path,
                    "dependency_depth": 1,
                },
            )
            identity_keys = ("project_file", "session_id", "world_path", "revision")
            if any(
                not isinstance(observed.get(key), str) or not 1 <= len(observed[key]) <= 4096
                for key in identity_keys
            ):
                raise JevError("bridge_error", "Fresh handoff measurements lack editor identity.")
            result = compare_asset(asset, observed, tolerance_cm)
            result["identity"] = {key: observed[key] for key in identity_keys}
            result["observation_source"] = "native_bridge"
            return {"ok": True, "result": result}
        except JevError as exc:
            return exc.as_dict()


def compare_manifest(manifest: HandoffManifest, observations: list[dict], tolerance=0.1):
    if not isinstance(observations, list) or len(observations) > 32:
        raise JevError("invalid_observation", "Supply at most 32 exact asset observations.")
    by_path = {}
    for row in observations:
        if (
            not isinstance(row, dict)
            or not isinstance(row.get("asset_path"), str)
            or row["asset_path"] in by_path
        ):
            raise JevError("invalid_observation", "Unique exact asset observations are required.")
        by_path[row["asset_path"]] = row
    rows = [
        compare_asset(
            a, by_path.get(a.unreal_asset_path, {"asset_path": a.unreal_asset_path}), tolerance
        )
        for a in manifest.assets
    ]
    return {
        "status": "passed" if all(r["status"] == "passed" for r in rows) else "failed",
        "assets": rows,
        "observation_source": "caller_supplied",
    }
