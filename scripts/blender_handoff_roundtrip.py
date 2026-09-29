"""Check the calibration FBX exported back from Unreal; isolated Blender only."""

import json
import sys
from pathlib import Path

import bpy

root = Path(sys.argv[sys.argv.index("--") + 1]).resolve()
bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete(use_global=False)
bpy.ops.import_scene.fbx(filepath=str(root / "roundtrip.fbx"))
objects = [obj for obj in bpy.context.scene.objects if obj.type == "MESH"]
if len(objects) != 1:
    raise RuntimeError("Expected exactly one returned mesh.")
mesh = objects[0]
points = [mesh.matrix_world @ vertex.co for vertex in mesh.data.vertices]
low = [min(p[i] for p in points) for i in range(3)]
high = [max(p[i] for p in points) for i in range(3)]
size = [(high[i] - low[i]) * 100 for i in range(3)]
center = [(high[i] + low[i]) * 50 for i in range(3)]
report = {
    "bounds_size_cm": size,
    "bounds_center_cm": center,
    "vertices": len(mesh.data.vertices),
    "material_slots": [slot.name for slot in mesh.material_slots],
    "blender": bpy.app.version_string,
}
if any(abs(a - b) > 0.1 for a, b in zip(size, [200, 100, 50], strict=True)):
    raise RuntimeError("Roundtrip dimensions changed: " + repr(size))
if any(abs(a - b) > 0.1 for a, b in zip(center, [0, 0, 25], strict=True)):
    raise RuntimeError("Roundtrip pivot changed: " + repr(center))
report["geometry_status"] = "passed"
(root / "roundtrip-observation.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
print("JEV_HANDOFF_ROUNDTRIP_OK")
