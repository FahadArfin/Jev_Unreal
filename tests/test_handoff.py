import hashlib
from copy import deepcopy
from unittest.mock import AsyncMock

import pytest
from mcp.server.fastmcp import FastMCP
from pydantic import ValidationError

from jev_unreal.errors import JevError
from jev_unreal.handoff import (
    HandoffAsset,
    HandoffManifest,
    compare_asset,
    compare_manifest,
    inspect_bundle,
    register_handoff_tools,
)

ASSET = {
    "asset_id": "prop",
    "unreal_asset_path": "/Game/Props/Prop.Prop",
    "mesh_file": "prop.fbx",
    "bounds_size_cm": [200.0, 100.0, 50.0],
    "bounds_center_cm": [0.0, 0.0, 25.0],
    "material_slots": ["Surface"],
}
OBSERVED = {
    "asset_path": ASSET["unreal_asset_path"],
    "bounds_size_cm": ASSET["bounds_size_cm"],
    "bounds_center_cm": ASSET["bounds_center_cm"],
    "material_slot_names": ["Surface"],
}


def manifest(tmp_path):
    files = []
    for name, role in (
        ("prop.blend", "editable_source"),
        ("prop.fbx", "mesh"),
        ("surface.png", "texture"),
    ):
        content = ("fixture-" + name).encode()
        (tmp_path / name).write_bytes(content)
        files.append(
            {
                "path": name,
                "role": role,
                "bytes": len(content),
                "sha256": hashlib.sha256(content).hexdigest(),
            }
        )
    return HandoffManifest.model_validate(
        {
            "version": 1,
            "coordinate_contract": "unreal_local_centimeters_z_up",
            "producer": "test",
            "source_license": "MIT",
            "provenance": "Original fixture",
            "files": files,
            "assets": [ASSET],
        }
    )


def test_file_verification_detects_changed_source_and_missing_texture(tmp_path):
    data = manifest(tmp_path)
    assert inspect_bundle(data, tmp_path)["status"] == "passed"
    source = tmp_path / "prop.blend"
    source.write_bytes(b"x" * source.stat().st_size)
    (tmp_path / "surface.png").unlink()
    report = inspect_bundle(data, tmp_path)
    assert report["status"] == "failed"
    assert [r["status"] for r in report["files"]] == [
        "hash_mismatch",
        "verified",
        "missing_or_size_mismatch",
    ]


@pytest.mark.parametrize(
    "path",
    [
        "../private.blend",
        "/private.blend",
        "C:/private.blend",
        "x/../prop.blend",
        "x\\prop.blend",
        "a./prop.blend",
    ],
)
def test_paths_cannot_escape_bundle(tmp_path, path):
    data = manifest(tmp_path).model_dump()
    data["files"][0]["path"] = path
    with pytest.raises(ValidationError):
        HandoffManifest.model_validate(data)


def test_duplicate_paths_and_missing_sources_rejected(tmp_path):
    data = manifest(tmp_path).model_dump()
    data["files"][1]["path"] = "PROP.BLEND"
    with pytest.raises(ValidationError):
        HandoffManifest.model_validate(data)
    data = manifest(tmp_path).model_dump()
    data["files"].pop(0)
    with pytest.raises(ValidationError):
        HandoffManifest.model_validate(data)


@pytest.mark.parametrize(
    "field,value",
    [
        ("bounds_size_cm", [2.0, 1.0, 0.5]),
        ("bounds_size_cm", [100.0, 200.0, 50.0]),
        ("bounds_center_cm", [0.0, 0.0, 0.0]),
        ("material_slot_names", ["Wrong"]),
    ],
)
def test_actual_unit_axis_pivot_and_material_regressions_fail(field, value):
    observation = deepcopy(OBSERVED)
    observation[field] = value
    assert compare_asset(HandoffAsset(**ASSET), observation)["status"] == "failed"


def test_missing_evidence_is_not_a_pass(tmp_path):
    assert (
        compare_asset(HandoffAsset(**ASSET), {"asset_path": ASSET["unreal_asset_path"]})["status"]
        == "unverifiable"
    )
    assert compare_manifest(manifest(tmp_path), [])["status"] == "failed"


def test_identity_and_nonfinite_evidence_rejected():
    with pytest.raises(JevError, match="exact"):
        compare_asset(HandoffAsset(**ASSET), {**OBSERVED, "asset_path": "/Game/Other.Other"})
    result = compare_asset(
        HandoffAsset(**ASSET), {**OBSERVED, "bounds_size_cm": [float("nan")] * 3}
    )
    assert result["status"] == "unverifiable"
    with pytest.raises(JevError):
        compare_asset(HandoffAsset(**ASSET), OBSERVED, float("nan"))


async def test_mcp_reads_fresh_exact_asset_without_filesystem_access():
    bridge = AsyncMock()
    bridge.call.return_value = {
        **OBSERVED,
        "project_file": "C:/Fixture/Fixture.uproject",
        "session_id": "fixture-session",
        "world_path": "/Game/Fixture.Fixture",
        "revision": "fresh-revision",
    }
    server = FastMCP("handoff-test")
    register_handoff_tools(server, bridge)
    response = await server.call_tool("unreal_handoff_verify", {"asset": ASSET})
    assert "passed" in str(response)
    assert "fixture-session" in str(response) and "native_bridge" in str(response)
    bridge.call.assert_awaited_once_with(
        "workflow_inspect",
        {
            "kind": "asset_diagnosis",
            "target_path": ASSET["unreal_asset_path"],
            "dependency_depth": 1,
        },
    )


async def test_mcp_handoff_refuses_measurements_without_native_identity():
    bridge = AsyncMock()
    bridge.call.return_value = OBSERVED
    server = FastMCP("handoff-identity-test")
    register_handoff_tools(server, bridge)
    response = await server.call_tool("unreal_handoff_verify", {"asset": ASSET})
    assert "bridge_error" in str(response)
