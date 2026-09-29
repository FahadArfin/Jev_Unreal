"""Create an original public calibration prop in an isolated --factory-startup Blender.

Run: blender --background --factory-startup --disable-autoexec --python THIS -- OUTPUT
Preserves an editable .blend plus FBX, texture and explicit Unreal-local geometry contract.
This test script is not an MCP/native bridge execution surface.
"""

import hashlib
import json
import sys
from pathlib import Path

import bpy

root = Path(sys.argv[sys.argv.index("--") + 1]).resolve()
root.mkdir(parents=True, exist_ok=True)
if any(root.iterdir()):
    raise RuntimeError("Choose an empty output folder; never overwrite an existing asset bundle.")
bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete(use_global=False)
bpy.context.scene.unit_settings.system = "METRIC"
bpy.context.scene.unit_settings.scale_length = 1.0
bpy.ops.mesh.primitive_cube_add(size=1, location=(0, 0, 0))
prop = bpy.context.object
prop.name = "JevCalibrationProp"
# Asymmetric footprint reveals swapped axes; bottom-origin pivot reveals pivot drift.
for vertex in prop.data.vertices:
    vertex.co.x *= 2
    vertex.co.y *= 1
    vertex.co.z = vertex.co.z * 0.5 + 0.25
prop["jev_asset_id"] = "calibration-prop"
material = bpy.data.materials.new("JevCalibrationMaterial")
material.use_nodes = True
material.diffuse_color = (0.1, 0.5, 0.9, 1)
image = bpy.data.images.new("JevCalibrationTexture", width=16, height=16)
image.generated_color = (0.1, 0.5, 0.9, 1)
image.filepath_raw = str(root / "calibration.png")
image.file_format = "PNG"
image.save()
texture = material.node_tree.nodes.new("ShaderNodeTexImage")
texture.image = image
material.node_tree.links.new(
    texture.outputs["Color"], material.node_tree.nodes.get("Principled BSDF").inputs["Base Color"]
)
prop.data.materials.append(material)
bpy.ops.wm.save_as_mainfile(filepath=str(root / "calibration.blend"))
bpy.ops.export_scene.fbx(
    filepath=str(root / "calibration.fbx"),
    check_existing=True,
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
)
files = []
for name, role in (
    ("calibration.blend", "editable_source"),
    ("calibration.fbx", "mesh"),
    ("calibration.png", "texture"),
):
    content = (root / name).read_bytes()
    files.append(
        {
            "path": name,
            "bytes": len(content),
            "sha256": hashlib.sha256(content).hexdigest(),
            "role": role,
        }
    )
manifest = {
    "version": 1,
    "coordinate_contract": "unreal_local_centimeters_z_up",
    "producer": "Blender " + bpy.app.version_string,
    "source_license": "MIT",
    "provenance": "Original Jev_Unreal calibration fixture",
    "files": files,
    "assets": [
        {
            "asset_id": "calibration-prop",
            "mesh_file": "calibration.fbx",
            "unreal_asset_path": "/Game/JevHandoff/SM_Calibration.SM_Calibration",
            "bounds_size_cm": [200.0, 100.0, 50.0],
            "bounds_center_cm": [0.0, 0.0, 25.0],
            "material_slots": ["JevCalibrationMaterial"],
        }
    ],
}
(root / "handoff.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
print("JEV_HANDOFF_EXPORT_OK")
