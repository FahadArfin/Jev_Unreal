# Unreal validation

Jev Editor **0.2.0a1** was rebuilt and exercised locally on Unreal Engine **5.8.2**, Windows 11, on 2026-09-22 UTC. This is an original independent bridge; no upstream JevUnreal implementation was copied into this plugin.

## Verified native checks

`scripts/Build-Unreal.ps1` completed successfully with the installed UnrealBuildTool and Windows SDK. Visual Studio compiler 14.51 emitted Unreal's warning that its preferred compiler is 14.50; the plugin compiled and linked successfully.

`scripts/Launch-Unreal.ps1 -AutomationTests` launched only the repository's dedicated `examples/JevSandbox` project. The final report was checked, rather than treating process exit as proof:

| Test | Result | Coverage |
| --- | --- | --- |
| `Jev.Editor.PlanLifecycle` | Passed | Preview causes no scene mutation; primitive spawn; transform of the resulting actor; single-use plan rejection; native editor undo restores the transform and removes the spawn. |
| `Jev.Editor.PlanSafety` | Passed | Changed scenes, expiry, different bridge sessions, replacement actors with reused object paths, Unicode label changes, and replacement editor worlds reject old plans. |
| `Jev.Editor.SchemaSafety` | Passed | Unknown actions and fields; excessive or incorrectly typed limits; path traversal; invalid operation elements; disallowed shapes; incorrect scalar/vector types; negative scales; excessive coordinates; mixed valid/invalid plans cause no mutation. |
| `Jev.Editor.ContextInspection` | Passed | Combined bounded actors/selection/context, truncation, strict parameters, and status/context availability under the simulated play-mode policy. |
| `Jev.Editor.SceneValidation` | Passed | Missing mesh, negative/zero scale and disabled-collision warnings tied to exact actor paths; bounded scans and incomplete-result flags. |
| `Jev.Editor.AssetInspection` | Passed | Exact engine cube dimensions, material/LOD/collision metadata, bounded results, missing assets, and rejection of filesystem/traversal/subobject paths. |
| `Jev.Editor.CaptureSafety` | Passed | Bounded dimensions, strict types, no arbitrary output path, and explicit missing-viewport failure. |
| `Jev.Editor.FrameSafety` | Passed | Unique bounded exact actor paths, padding limits, missing targets/viewports, and preservation of actor transforms/count/selection after rejected requests. |
| `Jev.Editor.StaticMeshPlacement` | Passed | Exact registry mesh placement and native Undo; missing/wrong-class assets, redirectors, malformed paths, and replacement assets that reuse reviewed paths are rejected. |

Final report timestamp: **`2026.09.22-01.53.08`**. Totals: **9 succeeded, 0 failed, 0 warnings, 0 not run, 0 in progress**. Schema tests also verify that oversized responses are rejected by UTF-8 byte size. Machine-specific raw artifacts remain local under `artifacts/unreal-automation/` and `examples/JevSandbox/Saved/Logs/`.

The launcher checks report freshness, completion counts, and each of the nine expected test names. Headless execution uses `UnrealEditor-Cmd.exe -NullRHI`; this does not establish rendered capture success. `-NoShaderCompile` is intentionally omitted because combining it with the installed headless engine previously caused a startup crash before tests ran.

## Live editor and visual evidence

The real stdio MCP server discovered **21 tools** and passed `scripts/smoke_editor.py` against the authenticated rendered sandbox editor. `artifacts/editor-smoke-v0.2.json` records engine `5.8.2-56702186+++UE5+Release-5.8` and these checks:

- Authentication/origin rejection and exact project identity.
- Primitive preview without mutation, spawn/transform, consumed-plan rejection and stale-plan rejection.
- Actor/asset bounds, compact context, exact native mesh measurements, existing static-mesh placement and bounded scene warnings.
- Preview/application of grid, stairs and room layouts, with **4, 4 and 5 actors** respectively; all three readback verifications passed.
- Native camera framing followed by a **1014 × 479**, **520,449-byte** PNG from the editor viewport.

Initial visual inspection found that viewport invalidation alone could leave old pixels in a background editor capture. The bridge now submits pending world/component updates, explicitly draws that viewport, and flushes rendering commands before reading pixels. After rebuilding and repeating native/live checks, the [saved capture](images/verified-stair-blockout.png) was viewed and confirmed to show the intended four-step staircase in the current frame. Camera metadata also matched the framing result.

This is narrow visual acceptance of one blockout and the capture path. It is not gameplay, character traversal, collision simulation, an art review, or confirmation that every scene renders correctly. All smoke-test scene changes remain unsaved; no other game project was modified.

## Bridge guarantees and scope

- The bridge starts only with a printable non-space ASCII `JEV_BRIDGE_TOKEN` of 32–256 characters. A token should be randomly generated. The plugin never prints it.
- The listener explicitly binds `127.0.0.1:9845`; an occupied port fails closed. Requests require the matching loopback Host header and a bearer token, and requests with an Origin header are rejected.
- Unreal's HTTPServer dispatches the action handler on the game thread. Off-thread invocation is rejected. The plugin only unbinds its own route on shutdown and never stops shared HTTP listeners. Python serializes calls and paces all HTTP requests, including identity checks, to at most **20 per second per client**, below the native **30-per-second** authenticated limit. Throttled operations are not automatically retried.
- Supported operations are bounded inspection, native viewport framing/capture, allowed primitive spawning, exact existing static-mesh placement, and supported actor transforms. Play/Simulate permits only status and editor-world context. There is no Python, console, deletion, arbitrary file write, map save, asset import, or arbitrary class-construction endpoint.
- `set_transform` is intentionally restricted to the exact native `AStaticMeshActor` class with no attached parent, attached children, or child-actor ownership. Blueprint-derived actors are rejected because editor movement may run construction scripts with side effects.
- `spawn_static_mesh` resolves an exact `/Game` or `/Engine` registry object path and accepts only native StaticMesh assets. Redirectors/subobjects are rejected. Preview retains the mesh's weak UObject identity; apply rejects a missing/replaced object even when a replacement reuses its path, and holds resolved meshes strongly during the transaction. This does not fingerprint in-place changes to mesh contents.
- A plan is bound to the editor session, project, weak world identity, weak target identities, and original scene actor identities. Its revision covers actor identity, object path, class, root path, label, transforms, attachment, hidden/locked state, and the current level. UTF-8 hashing preserves Unicode label changes. It does **not** hash full material, mesh, Blueprint, or asset contents.
- Plans expire after 120 seconds, contain at most 20 operations, and are consumed on an apply attempt. At most 64 live plans are retained. Preview validation runs before an editor undo transaction is opened. Failures restore the supported transforms and remove newly spawned actors; arbitrary third-party editor callback side effects are outside this guarantee.
- The MCP layer compares successful apply readback against normalized previews, including actor paths, transforms and applicable mesh identity. Equivalent Euler rotations are accepted. Malformed readback is reported as failed verification rather than hiding an already-applied result. Readback does not certify visual/gameplay behavior; never blindly retry an ambiguous apply.
- Scene/asset queries return at most **200 entries**. Context/validation report independent truncation/incomplete flags. Extended inspection scans at most 5,000 candidate actors, 64 components/material slots per applicable item, and 400 warnings; asset LOD details are capped at 16.
- Native capture uses the existing editor viewport, without desktop capture or arbitrary output paths. Requested maximum dimensions are 64–1024; source area is capped at 16 million pixels and compressed PNG at 720 KiB. Existing screenshot/movie operations block bridge capture. Python checks bounded PNG structure/checksums and dimensions before emitting MCP image content; this does not assess visual quality.
- Handler request bodies are capped at **64 KiB**, and serialized responses at **1 MiB of UTF-8**. Unreal's HTTPServer parses the body before the handler, so these application limits are not a transport-level defense against a hostile local process.

## Remaining acceptance boundaries

There is no evidence yet for production-map performance, sustained multi-client load, other Unreal versions/platforms, multiplayer, broad gameplay/visual quality, or every third-party editor plugin. External MCP discovery is metadata-only and separate from this authenticated editor bridge. Actual discovery of the installed Epic server's full catalog remains unverified; real SDK/network tests use synthetic local servers. [Python, provider and catalog evidence](VALIDATION.md) is reported separately.
