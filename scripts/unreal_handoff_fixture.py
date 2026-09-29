"""Fixed calibration import, run only as a local Unreal Python commandlet.

Default: unsaved import and round-trip FBX export. Explicit -JevHandoffSaveFixture:
save only newly imported owned calibration assets for a subsequent live MCP test,
without exporting again or saving maps. Neither mode is a bridge operation.
"""

import hashlib
import json
import math
import os
import re
import stat
from pathlib import Path

ASSET_FOLDER = "/Game/JevHandoff"
ASSET_PATH = ASSET_FOLDER + "/SM_Calibration.SM_Calibration"
FIXED_ASSET = {
    "asset_id": "calibration-prop",
    "mesh_file": "calibration.fbx",
    "unreal_asset_path": ASSET_PATH,
    "bounds_size_cm": [200.0, 100.0, 50.0],
    "bounds_center_cm": [0.0, 0.0, 25.0],
    "material_slots": ["JevCalibrationMaterial"],
}
FIXED_FILES = {
    "calibration.blend": "editable_source",
    "calibration.fbx": "mesh",
    "calibration.png": "texture",
}


def _ordinary(path):
    metadata = path.lstat()
    if stat.S_ISLNK(metadata.st_mode) or getattr(metadata, "st_file_attributes", 0) & 0x400:
        raise RuntimeError("Calibration paths must not contain links or reparse points.")


def _unique_object(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise RuntimeError("Duplicate calibration manifest key.")
        value[key] = item
    return value


def _write_report(path, value):
    if os.path.lexists(path):
        _ordinary(path)
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def read_calibration_bundle(repository):
    """Validate only the three fixed local public fixture files, without Unreal imports."""
    root = repository / "artifacts/handoff-v09"
    for path in (repository / "artifacts", root, root / "handoff.json"):
        _ordinary(path)
    manifest_path = root / "handoff.json"
    if not manifest_path.is_file() or not 0 < manifest_path.stat().st_size <= 65536:
        raise RuntimeError("Missing or oversized calibration manifest.")
    with manifest_path.open("rb") as handle:
        content = handle.read(65537)
    if len(content) > 65536:
        raise RuntimeError("Calibration manifest grew beyond its read budget.")
    manifest = json.loads(content, object_pairs_hook=_unique_object)
    if (
        not isinstance(manifest, dict)
        or set(manifest) != {
            "version", "coordinate_contract", "producer", "source_license",
            "provenance", "files", "assets",
        }
        or type(manifest["version"]) is not int
        or manifest["version"] != 1
        or manifest["coordinate_contract"] != "unreal_local_centimeters_z_up"
        or manifest["source_license"] != "MIT"
        or manifest["provenance"] != "Original Jev_Unreal calibration fixture"
        or not isinstance(manifest["producer"], str)
        or not manifest["producer"].startswith("Blender ")
        or len(manifest["producer"]) > 128
        or manifest["assets"] != [FIXED_ASSET]
    ):
        raise RuntimeError("Expected the exact generated public calibration contract.")
    for field in ("bounds_size_cm", "bounds_center_cm"):
        if any(type(value) not in (int, float) for value in manifest["assets"][0][field]):
            raise RuntimeError("Calibration vectors require numeric measurements.")
    entries = manifest["files"]
    if not isinstance(entries, list) or len(entries) != 3:
        raise RuntimeError("Expected all three fixed calibration files.")
    seen = set()
    for entry in entries:
        if (
            not isinstance(entry, dict)
            or set(entry) != {"path", "role", "bytes", "sha256"}
            or not isinstance(entry["path"], str)
            or entry["path"] not in FIXED_FILES
            or entry["path"] in seen
            or entry["role"] != FIXED_FILES[entry["path"]]
            or type(entry["bytes"]) is not int
            or not 0 < entry["bytes"] <= 32 * 1024 * 1024
            or not isinstance(entry["sha256"], str)
            or not re.fullmatch(r"[a-f0-9]{64}", entry["sha256"])
        ):
            raise RuntimeError("Invalid fixed calibration file declaration.")
        seen.add(entry["path"])
        path = root / entry["path"]
        _ordinary(path)
        if not path.is_file() or path.stat().st_size != entry["bytes"]:
            raise RuntimeError("Calibration file missing or size differs: " + entry["path"])
        with path.open("rb") as handle:
            data = handle.read(entry["bytes"] + 1)
        if len(data) != entry["bytes"] or hashlib.sha256(data).hexdigest() != entry["sha256"]:
            raise RuntimeError("Calibration file hash differs: " + entry["path"])
    return root, manifest


def _save_owned_assets(unreal, root, repository):
    paths = list(unreal.EditorAssetLibrary.list_assets(ASSET_FOLDER, True, False))
    if not 1 <= len(paths) <= 8 or ASSET_PATH not in paths:
        raise RuntimeError("Unexpected calibration asset set; nothing has been saved.")
    assets = []
    for path in paths:
        # Import creates this previously absent namespace. Save only direct native assets.
        if not re.fullmatch(r"/Game/JevHandoff/[A-Za-z0-9_]+\.[A-Za-z0-9_]+", path):
            raise RuntimeError("Import created an unexpected calibration path.")
        asset = unreal.load_asset(path)
        if asset is None or asset.get_class() not in (
            unreal.StaticMesh.static_class(), unreal.Material.static_class(),
            unreal.Texture2D.static_class(),
        ):
            raise RuntimeError("Refusing to save a non-native calibration mesh/material/texture.")
        if isinstance(asset, unreal.StaticMesh) and path != ASSET_PATH:
            raise RuntimeError("Unexpected additional static mesh in calibration import.")
        assets.append(asset)
    saved = []
    for asset in assets:
        if not unreal.EditorAssetLibrary.save_loaded_asset(asset, False):
            raise RuntimeError("Calibration package save failed; inspect partial owned output.")
        saved.append(asset.get_path_name())
    report = {
        "project_file": str(repository / "examples/JevSandbox/JevSandbox.uproject"),
        "saved_assets": saved,
        "maps_saved": False,
        "roundtrip_export_requested": False,
        "scope": "Explicit local fixture preparation; no general import or save bridge action.",
    }
    _write_report(root / "unreal-prepared.json", report)
    unreal.log("JEV_HANDOFF_PREPARED_OK: owned assets saved; maps not saved.")


def main():
    import unreal

    repository = Path(__file__).resolve().parents[1]
    expected = repository / "examples/JevSandbox/JevSandbox.uproject"
    actual = Path(unreal.Paths.get_project_file_path()).resolve()
    if actual != expected.resolve():
        raise RuntimeError("Refusing non-sandbox project.")
    _, switches, _ = unreal.SystemLibrary.parse_command_line(
        unreal.SystemLibrary.get_command_line()
    )
    save_fixture = "jevhandoffsavefixture" in {str(flag).casefold() for flag in switches}
    root, manifest = read_calibration_bundle(repository)
    content = repository / "examples/JevSandbox/Content/JevHandoff"
    for parent in (repository / "examples", expected.parent, content.parent):
        if os.path.lexists(parent):
            _ordinary(parent)
    if (
        os.path.lexists(content)
        or unreal.EditorAssetLibrary.does_directory_exist(ASSET_FOLDER)
        or unreal.EditorAssetLibrary.does_asset_exist(ASSET_PATH)
    ):
        raise RuntimeError("Fixture folder or assets already exist; refusing overwrite or reuse.")
    if not save_fixture and (root / "roundtrip.fbx").exists():
        raise RuntimeError("Refusing to overwrite existing roundtrip output.")
    options = unreal.FbxImportUI()
    options.import_mesh = True
    options.import_as_skeletal = False
    options.import_materials = True
    options.import_textures = True
    options.import_animations = False
    options.automated_import_should_detect_type = False
    options.mesh_type_to_import = unreal.FBXImportType.FBXIT_STATIC_MESH
    options.static_mesh_import_data.convert_scene = True
    options.static_mesh_import_data.convert_scene_unit = True
    options.static_mesh_import_data.force_front_x_axis = False
    options.static_mesh_import_data.combine_meshes = True
    options.static_mesh_import_data.auto_generate_collision = True
    task = unreal.AssetImportTask()
    task.filename = str(root / "calibration.fbx")
    task.destination_path = ASSET_FOLDER
    task.destination_name = "SM_Calibration"
    task.automated = True
    task.save = False
    task.replace_existing = False
    task.options = options
    task.factory = unreal.FbxFactory()
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    mesh = unreal.load_asset(ASSET_PATH)
    if not isinstance(mesh, unreal.StaticMesh) or mesh.get_path_name() != ASSET_PATH:
        raise RuntimeError("Import did not produce the exact expected static mesh.")
    box = mesh.get_bounding_box()
    size = box.max - box.min
    center = (box.max + box.min) * 0.5
    observation = {
        "asset_path": ASSET_PATH,
        "bounds_size_cm": [size.x, size.y, size.z],
        "bounds_center_cm": [center.x, center.y, center.z],
        "material_slot_names": [str(slot.material_slot_name) for slot in mesh.static_materials],
        "lod_count": mesh.get_num_lods(),
        "imported_object_paths": list(task.imported_object_paths),
    }
    _write_report(root / "unreal-observation.json", [observation])
    contract = manifest["assets"][0]
    for field in ("bounds_size_cm", "bounds_center_cm"):
        if any(
            not math.isfinite(a) or abs(a - b) > 0.1
            for a, b in zip(observation[field], contract[field], strict=True)
        ):
            raise RuntimeError("DCC geometry mismatch: " + field)
    if observation["material_slot_names"] != contract["material_slots"]:
        raise RuntimeError("DCC material identity mismatch.")
    if save_fixture:
        _save_owned_assets(unreal, root, repository)
        return
    export = unreal.AssetExportTask()
    export.object = mesh
    export.filename = str(root / "roundtrip.fbx")
    export.automated = True
    export.prompt = False
    export.replace_identical = False
    export.exporter = unreal.StaticMeshExporterFBX()
    export.options = unreal.FbxExportOption()
    export.options.collision = False
    export.options.level_of_detail = False
    if not unreal.Exporter.run_asset_export_task(export):
        raise RuntimeError("Unreal failed to export the imported calibration prop.")
    unreal.log("JEV_HANDOFF_IMPORT_OK: packages and maps not saved.")


if __name__ == "__main__":
    main()
