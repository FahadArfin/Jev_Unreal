# Measured Blender and Unreal handoffs

Version 0.9 adds a versioned, hash-bound handoff contract and a local Blender
exporter. The authenticated editor bridge does not accept file paths to import,
Python scripts, arbitrary commands or Blender execution. Export/import remain
explicit local operations; inspection checks their measurable results.

## Export editable sources

Run the utility with installed Blender and a trusted local source:

```powershell
& 'C:\Program Files\Blender Foundation\Blender 5.2\blender.exe' `
  --background --factory-startup --disable-autoexec `
  --python scripts/export_blender_handoff.py -- `
  --source 'D:\Art\Prop.blend' --output 'D:\Exports\Prop-Review' `
  --asset-root /Game/Props --license MIT --provenance 'Original authored prop'
uv run jev-unreal handoff inspect 'D:\Exports\Prop-Review\handoff.json' `
  --root 'D:\Exports\Prop-Review'
```

The destination must be empty. The exporter retains a separate editable `.blend`
copy, exports one FBX per static mesh, copies used texture sources, and writes
SHA-256 and byte counts plus dimensions, local pivot offsets and ordered material
slots. It leaves the original source unchanged. License/provenance values are
author declarations; a manifest does not independently establish usage rights.

The initial exporter accepts 1–32 unparented static meshes with unique ASCII
names, metre scene units and applied rotation/scale. It refuses linked libraries,
armatures, constraints, missing image files and ambiguous transforms. Supported
image sources include ordinary files, packed images and bounded generated images.
Editable source retention does not guarantee every Blender material or modifier
has an equivalent Unreal representation. Inspect partial output after a failed
export and choose a new empty directory for a retry.

## Inspect, import, verify

1. Inspect the bundle before import. Hash verification is streaming and bounded;
   absolute/traversing paths, reparse points, missing files, size changes and hash
   mismatches fail. The manifest supports at most 128 files, 32 assets and 2 GiB
   total declared input. It must retain an editable `.blend` source.
2. Import reviewed files through Unreal's normal FBX workflow into the exact
   manifest `/Game/...` destinations. Review units, pivots, materials and collision.
3. Use `unreal_handoff_verify(asset, tolerance_cm=0.1)` for each manifest asset.
   The MCP tool obtains fresh native mesh bounds and material slot names from the
   connected editor and retains the project/session/world/revision identity with
   `observation_source: native_bridge`. Missing identity is refused. It does not
   trust imported observations as live evidence.
4. Review the rendered asset and project gameplay/collision requirements separately.

The coordinate contract is `unreal_local_centimeters_z_up`: mesh-local axis-aligned
size and bounds centre relative to the mesh origin, in centimetres. A fresh read
checks exact asset identity, finite positive dimensions, local pivot offset and
ordered slot names. Missing measurements are **unverifiable**, mismatches **failed**,
and matching supported measurements **passed**. None of those proves texture
appearance, topology, rigging, physics, runtime performance or license ownership.

Offline comparison is also available:

```powershell
uv run jev-unreal handoff verify handoff.json --observations observations.json
```

That command labels inputs as caller-provided measurements. It cannot certify
that measurements came from Unreal. Use live MCP for fresh bridge readback and
retain the explicit project/session identity alongside results.

## Reproducible calibration evidence

Public fixture scripts create a new 2 m × 1 m × 0.5 m prop with a bottom pivot,
an original material/image and retained `.blend` source:

- `blender_handoff_fixture.py` creates the bundle in ignored local artifacts.
- `unreal_handoff_fixture.py` refuses every project except this repository's
  JevSandbox, verifies all three fixed source hashes and the calibration contract,
  imports the known fixture without saving a package, measures it and
  exports a round-trip FBX. This is a local licensed test script, never a bridge tool.
- `blender_handoff_roundtrip.py` checks the resulting dimensions and pivot again.

For separate live MCP verification, explicitly pass `-JevHandoffSaveFixture` to
the isolated Unreal Python commandlet running `scripts/unreal_handoff_fixture.py`.
This preparation mode refuses any existing `/Game/JevHandoff` folder or assets,
imports the same hash-verified public fixture and saves only its newly created
native mesh/material/texture packages in the exact ignored sandbox content folder.
It saves no map and skips round-trip export, including when `roundtrip.fbx` already
exists. Inspect partial output if a save fails; the script never deletes or
overwrites existing fixture content on a retry.

After restarting the exact sandbox editor with the configured authenticated bridge,
run `uv run python scripts/smoke_handoff.py`. The official stdio SDK loads the saved
mesh through the existing bounded asset inspection, calls `unreal_handoff_verify`
for fresh measurements, and requires an intentionally shifted pivot contract to
fail while dimensions/material slots still pass. Its local JSON report retains
project/session identity, results and zero provider requests. Preparation logs and
`unreal-prepared.json` identify saved assets explicitly; MCP requests perform no
import or save. Preparation is separate from a successful live smoke result;
record executed results in validation evidence.

The fixture passed Blender 5.2.1 → Unreal 5.8.2 → Blender 5.2.1 dimensional and
pivot checks on the current Windows host. The generic exporter also exported
that retained source and passed bundle hash inspection. FBX import reported a
missing-smoothing-group warning for the calibration fixture. This is one static
prop; material appearance, animation, arbitrary DCC assets and production-project
acceptance remain separate work.

References: [Blender FBX exporter](https://docs.blender.org/api/main/bpy.ops.export_scene.html),
[Unreal FBX pipeline](https://dev.epicgames.com/documentation/unreal-engine/fbx-content-pipeline).
See [validation evidence](VALIDATION.md) for final release checks.
