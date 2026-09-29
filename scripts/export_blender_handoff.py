"""Export a local static-mesh bundle from Blender without changing the original .blend.

blender --background --factory-startup --disable-autoexec --python THIS --
  --source SOURCE.blend --output EMPTY_FOLDER --asset-root /Game/Handoff
  --license MIT --provenance "Original work"

Loads no linked libraries or scripts. Rejects unapplied rotation/scale, parenting,
rigs and missing textures; the original editable scene is retained in a new copy.
"""

import argparse
import hashlib
import json
import re
import shutil
import sys
from pathlib import Path

import bpy
from mathutils import Vector


def regular_local(path):
    path = Path(path).absolute()
    if str(path).startswith(("\\\\", "//")) or ".." in path.parts:
        raise ValueError("Use explicit local paths without traversal.")
    for part in (*reversed(path.parents), path):
        if part.is_symlink() or (
            part.exists() and getattr(part.lstat(), "st_file_attributes", 0) & 0x400
        ):
            raise ValueError("Linked paths/reparse points are not supported.")
    return path


parser = argparse.ArgumentParser()
parser.add_argument("--source", required=True)
parser.add_argument("--output", required=True)
parser.add_argument("--asset-root", required=True)
parser.add_argument("--license", required=True)
parser.add_argument("--provenance", required=True)
args = parser.parse_args(sys.argv[sys.argv.index("--") + 1 :])
source, output = regular_local(args.source), regular_local(args.output)
if (
    source.suffix.lower() != ".blend"
    or not source.is_file()
    or source.stat().st_size > 512 * 1024**2
):
    raise ValueError("Source must be a saved editable Blender file.")
if not re.fullmatch(r"/Game/[A-Za-z0-9_/]+", args.asset_root) or args.asset_root.endswith("/"):
    raise ValueError("Use an exact /Game asset folder without a trailing slash.")
if not 1 <= len(args.license) <= 256 or not 1 <= len(args.provenance) <= 2048:
    raise ValueError("License and provenance declarations are required and bounded.")
if output.exists() and (not output.is_dir() or any(output.iterdir())):
    raise ValueError("Output must be a new or empty folder.")
bpy.ops.wm.open_mainfile(filepath=str(source), load_ui=False, use_scripts=False)
if bpy.data.libraries:
    raise ValueError("Make linked content local before exporting this self-contained bundle.")
meshes = [obj for obj in bpy.context.scene.objects if obj.type == "MESH"]
if not 1 <= len(meshes) <= 32:
    raise ValueError("Export between one and 32 static meshes.")
if bpy.context.scene.unit_settings.scale_length != 1.0:
    raise ValueError("This exporter requires Blender meters (unit scale 1.0).")
names = set()
for obj in meshes:
    if not re.fullmatch(r"[A-Za-z0-9_]{1,100}", obj.name) or obj.name.casefold() in names:
        raise ValueError("Mesh names must be unique ASCII asset names of at most 100 characters.")
    names.add(obj.name.casefold())
    if (
        obj.parent
        or any(abs(v) > 1e-7 for v in obj.rotation_euler)
        or any(abs(v - 1) > 1e-7 for v in obj.scale)
        or obj.constraints
        or any(mod.type == "ARMATURE" for mod in obj.modifiers)
    ):
        raise ValueError("Apply rotation/scale, remove constraints/parents and use static meshes.")
    if len(obj.material_slots) > 64 or any(
        not slot.material or not re.fullmatch(r"[A-Za-z0-9_.-]{1,128}", slot.material.name)
        for slot in obj.material_slots
    ):
        raise ValueError("Use at most 64 assigned material slots with stable simple names.")
images = []
trees = [
    slot.material.node_tree
    for obj in meshes
    for slot in obj.material_slots
    if slot.material.use_nodes and slot.material.node_tree
]
seen = set()
while trees:
    tree = trees.pop()
    if tree in seen:
        continue
    if len(seen) >= 256:
        raise ValueError("Material graph traversal exceeds 256 node trees.")
    seen.add(tree)
    for node in tree.nodes:
        if node.type == "GROUP" and node.node_tree:
            trees.append(node.node_tree)
        if node.type == "TEX_IMAGE" and node.image and node.image not in images:
            images.append(node.image)
if len(images) > 64:
    raise ValueError("At most 64 texture sources are supported.")
texture_sources = []
for i, image in enumerate(images):
    if image.packed_file:
        suffix = Path(image.filepath).suffix.lower()
        if suffix not in {".png", ".jpg", ".jpeg", ".tga", ".tif", ".tiff", ".exr"}:
            raise ValueError("Packed texture must retain a supported file extension.")
        if len(image.packed_file.data) > 128 * 1024**2:
            raise ValueError("Packed texture exceeds 128 MiB.")
        texture_sources.append(
            (image, bytes(image.packed_file.data), f"textures/texture_{i:02d}{suffix}")
        )
        continue
    if image.source == "GENERATED" and image.size[0] * image.size[1] <= 4096**2:
        texture_sources.append((image, None, f"textures/texture_{i:02d}.png"))
        continue
    if image.source != "FILE" or image.packed_file:
        raise ValueError("Save/unpack oversized, packed or non-file textures before bundling.")
    path = regular_local(bpy.path.abspath(image.filepath))
    if not path.is_file() or path.stat().st_size > 128 * 1024**2:
        raise ValueError("Missing or oversized texture source: " + image.name)
    if path.suffix.lower() not in {".png", ".jpg", ".jpeg", ".tga", ".tif", ".tiff", ".exr"}:
        raise ValueError("Unsupported texture format.")
    texture_sources.append((image, path, f"textures/texture_{i:02d}{path.suffix.lower()}"))
output.mkdir(parents=True, exist_ok=True)
files = []


def record(path, role):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for part in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(part)
    files.append(
        {
            "path": path.relative_to(output).as_posix(),
            "role": role,
            "bytes": path.stat().st_size,
            "sha256": digest.hexdigest(),
        }
    )


for image, path, relative in texture_sources:
    target = output / relative
    target.parent.mkdir(exist_ok=True)
    if isinstance(path, bytes):
        target.write_bytes(path)
    elif path is None:
        image.filepath_raw = str(target)
        image.file_format = "PNG"
        image.save()
        image.source = "FILE"
    else:
        shutil.copyfile(path, target)
    image.filepath = "//" + relative
    record(target, "texture")
for obj in meshes:
    obj["jev_asset_id"] = obj.name
bpy.ops.wm.save_as_mainfile(filepath=str(output / "source.blend"), relative_remap=False)
record(output / "source.blend", "editable_source")
assets = []
depsgraph = bpy.context.evaluated_depsgraph_get()
for obj in meshes:
    evaluated = obj.evaluated_get(depsgraph)
    corners = [Vector(v) for v in evaluated.bound_box]
    low = [min(v[i] for v in corners) for i in range(3)]
    high = [max(v[i] for v in corners) for i in range(3)]
    if any(high[i] <= low[i] for i in range(3)):
        raise ValueError("Flat/empty meshes require a separate dimension contract.")
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    location = obj.location.copy()
    obj.location = (0, 0, 0)
    target = output / (obj.name + ".fbx")
    try:
        bpy.ops.export_scene.fbx(
            filepath=str(target),
            use_selection=True,
            object_types={"MESH"},
            global_scale=1.0,
            apply_unit_scale=True,
            apply_scale_options="FBX_SCALE_NONE",
            use_mesh_modifiers=True,
            bake_anim=False,
            axis_forward="-Y",
            axis_up="Z",
            path_mode="RELATIVE",
            use_custom_props=True,
            mesh_smooth_type="FACE",
        )
    finally:
        obj.location = location
    record(target, "mesh")
    asset_path = args.asset_root + "/" + obj.name + "." + obj.name
    assets.append(
        {
            "asset_id": obj.name,
            "unreal_asset_path": asset_path,
            "mesh_file": target.name,
            "bounds_size_cm": [(high[i] - low[i]) * 100 for i in range(3)],
            "bounds_center_cm": [(high[i] + low[i]) * 50 for i in range(3)],
            "material_slots": [slot.material.name for slot in obj.material_slots],
        }
    )
manifest = {
    "version": 1,
    "coordinate_contract": "unreal_local_centimeters_z_up",
    "producer": "Blender " + bpy.app.version_string,
    "source_license": args.license,
    "provenance": args.provenance,
    "files": files,
    "assets": assets,
}
(output / "handoff.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
print("JEV_HANDOFF_BUNDLE_READY")
