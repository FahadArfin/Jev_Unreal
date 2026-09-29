# Material and viewport editing extensions

These extensions use the existing inspect → preview → apply → readback workflow.
They preserve exact project/session/world identity, 120-second one-shot plans,
native object identity checks and bounded receipts. Neither Jev confidence nor a
successful provider response grants permission to edit.

## Material textures, switches and existing layer parameters

Inspect a loaded native Material Instance Constant using
`unreal_workflow_inspect(kind="material", target_path=...)`. Each exposed
parameter reports its kind, name, association, index and effective value. The
`association_name` field is `global`, `layer` or `blend`. The numeric association
field remains available for existing clients. A material with more than 128
parameters can be inspected with a truncation warning, but cannot be edited by
this workflow because its whole bounded review would be incomplete.

Approve exact materials in the project's `Config/DefaultGame.ini`:

```ini
[JevEditor.Workflows]
bEnableMaterialEdits=true
+EditableMaterials=/Game/Materials/MI_Prop.MI_Prop
bEnableStaticSwitchEdits=true
+AllowedMaterialTextures=/Game/Textures/T_Prop.T_Prop
```

Both path lists accept at most 64 unique exact paths. Materials must be `/Game`
assets. Approved textures may be `/Game` or `/Engine` assets. Duplicate/malformed
entries fail closed. Texture paths are references to already-loaded assets;
they are never filesystem access or implicit asset-load requests.

Use `unreal_workflow_preview` with one of these `change` objects, then review the
returned before/requested state before `unreal_workflow_apply(plan_id)`:

```json
{
  "kind": "material_texture",
  "target_path": "/Game/Materials/MI_Prop.MI_Prop",
  "parameter": "Albedo",
  "association": "global",
  "index": -1,
  "value": "/Game/Textures/T_Prop.T_Prop"
}
```

`material_static_switch` uses a boolean `value` and requires the separate
`bEnableStaticSwitchEdits` opt-in. Scalar/vector/texture/static-switch operations
all accept optional `association` and `index`. Defaults are global/-1; layer or
blend indices must be in 0..63. The exact exposed name, association and index must
exist. This edits parameters within an existing layer stack; it does not add,
remove, reorder or replace layers or blends.

Texture assignment requires native `UTexture2D` objects for both the existing
effective value and proposed reference, matching virtual-texture sampling mode,
and the exact loaded proposed object at both preview and apply. Texture cubes,
media textures, volumes, unresolved defaults and implicit sampler conversions are
refused. Sampler suitability, color management and appearance still require a
rendered comparison. Static switch changes can enqueue native shader work; a
verified stored boolean is not shader completion or visual acceptance.

Material changes use native Undo. No asset save is requested. Read
`status`, `readback_verified` and `after`: a transport success alone is not a
successful edit. Policy revocation, changed object identity, notified edits and
changed material snapshots invalidate a pending plan.

## Repeatable viewport settings

`unreal_workflow_inspect(kind="camera")` now reports fixed-exposure state,
EV100, view-mode index, motion blur and realtime override status alongside pose
and FOV. Existing `camera` changes retain their pose-only behavior.

The new `camera_render` change controls a closed set of native viewport settings:

```json
{
  "kind": "camera_render",
  "exposure_mode": "fixed",
  "fixed_ev100": 8,
  "view_mode": "lit",
  "realtime": false,
  "motion_blur": false
}
```

Exposure mode is `fixed` or `automatic`, fixed EV100 is -16..32, and view mode is
`lit` or `unlit`. All fields are required. Another subsystem's realtime or
show-flags override blocks this edit. The same exact unlocked perspective level
viewport must still be active, with unchanged captured state, at apply.
The initial mode must also be lit/unlit and initial EV100 within the supported
range, so the reviewed baseline can be restored using the same typed workflow.

Before changing settings, retain the inspected values. Restore them through a
new reviewed plan. Viewport changes do not use asset Undo, and realtime is an
editor preference that Unreal may retain between sessions. Setting realtime
false does **not** freeze the entire world's clock: other viewports, editor
systems, requested realtime frames, simulation, temporal history and time-of-day
systems can still affect the scene. These controls improve capture consistency;
they do not guarantee deterministic pixels or packaged rendering. Settle shader
and streaming work and use repeated captures when comparing visuals.

## Native verification coverage

`Jev.Editor.BlueprintGraphWorkflow` exercises native math node creation, typed
links, disconnect/removal, cycle/type refusal, stale graph detection, one-shot
receipts, compilation and Undo. `Jev.Editor.MaterialTextureSwitch` uses real
engine textures and generated unsaved material fixtures to check texture/switch
readback, Undo and permission revocation. `Jev.Rendered.CameraRenderSettings`
checks actual native viewport settings and competing override refusal in a
rendered editor. These tests are not a substitute for real project visual,
semantic or user acceptance; current execution evidence belongs in
[VALIDATION.md](VALIDATION.md).
