"""Host-side fixture refusal checks; these do not execute or mock-prove an Unreal import."""

import hashlib
import importlib.util
import json
import sys
from copy import deepcopy
from pathlib import Path
from types import SimpleNamespace

import pytest

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/unreal_handoff_fixture.py"
SPEC = importlib.util.spec_from_file_location("jev_fixture_validation", SCRIPT)
fixture = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(fixture)


def bundle(repository):
    root = repository / "artifacts/handoff-v09"
    root.mkdir(parents=True)
    files = []
    for name, role in fixture.FIXED_FILES.items():
        data = ("unit fixture bytes: " + name).encode()
        (root / name).write_bytes(data)
        files.append({
            "path": name, "role": role, "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
        })
    manifest = {
        "version": 1,
        "coordinate_contract": "unreal_local_centimeters_z_up",
        "producer": "Blender unit-test fixture",
        "source_license": "MIT",
        "provenance": "Original Jev_Unreal calibration fixture",
        "files": files,
        "assets": [deepcopy(fixture.FIXED_ASSET)],
    }
    (root / "handoff.json").write_text(json.dumps(manifest), encoding="utf-8")
    return root, manifest


def test_exact_bundle_checks_all_three_hashes(tmp_path):
    root, expected = bundle(tmp_path)
    assert fixture.read_calibration_bundle(tmp_path) == (root, expected)
    source = root / "calibration.blend"
    source.write_bytes(b"x" * source.stat().st_size)
    with pytest.raises(RuntimeError, match="hash differs"):
        fixture.read_calibration_bundle(tmp_path)


@pytest.mark.parametrize("mutation", [
    "other_asset", "other_path", "duplicate_file", "missing_source", "wrong_role",
    "bool_size", "oversize", "wrong_pivot", "extra_field", "bool_version", "bool_vector",
])
def test_changed_contract_is_refused_before_unreal_import(tmp_path, mutation):
    root, manifest = bundle(tmp_path)
    if mutation == "other_asset":
        manifest["assets"][0]["unreal_asset_path"] = "/Game/Other.Other"
    elif mutation == "other_path":
        manifest["files"][0]["path"] = "../../secret.blend"
    elif mutation == "duplicate_file":
        manifest["files"][1] = manifest["files"][0]
    elif mutation == "missing_source":
        manifest["files"].pop(0)
    elif mutation == "wrong_role":
        manifest["files"][0]["role"] = "mesh"
    elif mutation == "bool_size":
        manifest["files"][0]["bytes"] = True
    elif mutation == "oversize":
        manifest["files"][0]["bytes"] = 32 * 1024 * 1024 + 1
    elif mutation == "wrong_pivot":
        manifest["assets"][0]["bounds_center_cm"][2] = 50
    elif mutation == "extra_field":
        manifest["execute"] = "anything"
    elif mutation == "bool_version":
        manifest["version"] = True
    elif mutation == "bool_vector":
        manifest["assets"][0]["bounds_center_cm"][0] = False
    (root / "handoff.json").write_text(json.dumps(manifest), encoding="utf-8")
    with pytest.raises(RuntimeError):
        fixture.read_calibration_bundle(tmp_path)


def test_duplicate_json_keys_are_refused(tmp_path):
    root, _ = bundle(tmp_path)
    path = root / "handoff.json"
    path.write_text(path.read_text(encoding="utf-8").replace(
        '"version": 1', '"version": 1, "version": 1'
    ), encoding="utf-8")
    with pytest.raises(RuntimeError, match="Duplicate"):
        fixture.read_calibration_bundle(tmp_path)


@pytest.mark.parametrize("existing", ["disk", "registry"])
def test_existing_namespace_refused_before_import(tmp_path, monkeypatch, existing):
    bundle(tmp_path)
    project = tmp_path / "examples/JevSandbox/JevSandbox.uproject"
    project.parent.mkdir(parents=True)
    if existing == "disk":
        (project.parent / "Content/JevHandoff").mkdir(parents=True)
    # No import/save APIs are provided: reaching either would fail this refusal test.
    native = SimpleNamespace(
        Paths=SimpleNamespace(get_project_file_path=lambda: str(project)),
        SystemLibrary=SimpleNamespace(
            get_command_line=lambda: "-JevHandoffSaveFixture",
            parse_command_line=lambda _: ([], ["JevHandoffSaveFixture"], {}),
        ),
        EditorAssetLibrary=SimpleNamespace(
            does_directory_exist=lambda _: existing == "registry",
            does_asset_exist=lambda _: False,
        ),
    )
    monkeypatch.setitem(sys.modules, "unreal", native)
    monkeypatch.setattr(fixture, "__file__", str(tmp_path / "scripts/unreal_handoff_fixture.py"))
    with pytest.raises(RuntimeError, match="already exist"):
        fixture.main()


def test_wrong_project_refused_before_reading_bundle(tmp_path, monkeypatch):
    monkeypatch.setitem(sys.modules, "unreal", SimpleNamespace(
        Paths=SimpleNamespace(get_project_file_path=lambda: str(tmp_path / "Other.uproject"))
    ))
    monkeypatch.setattr(fixture, "__file__", str(tmp_path / "scripts/unreal_handoff_fixture.py"))
    with pytest.raises(RuntimeError, match="non-sandbox"):
        fixture.main()
