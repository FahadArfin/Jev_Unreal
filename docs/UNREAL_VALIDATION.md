# Unreal validation

The original Jev Editor C++ bridge was built and exercised locally on Unreal Engine **5.8.2**, Windows 11, on 2026-09-22 UTC. No upstream JevUnreal implementation was copied into this plugin.

## Verified native checks

`scripts/Build-Unreal.ps1` completed successfully with the installed UnrealBuildTool and Windows SDK. Visual Studio compiler 14.51 emitted Unreal's warning that its preferred compiler is 14.50; the plugin compiled and linked successfully.

`scripts/Launch-Unreal.ps1 -AutomationTests` launched only the repository's dedicated `examples/JevSandbox` project. The final report was checked, rather than treating process exit as proof:

| Test | Result | Coverage |
| --- | --- | --- |
| `Jev.Editor.PlanLifecycle` | Passed | Preview causes no scene mutation; primitive spawn; transform of the resulting actor; single-use plan rejection; native editor undo restores the transform and removes the spawn. |
| `Jev.Editor.PlanSafety` | Passed | Changed scenes, expiry, different bridge sessions, replacement actors with reused object paths, Unicode label changes, and replacement editor worlds reject old plans. |
| `Jev.Editor.SchemaSafety` | Passed | Unknown actions and fields; excessive or incorrectly typed limits; path traversal; invalid operation elements; disallowed shapes; incorrect scalar/vector types; negative scales; excessive coordinates; mixed valid/invalid plans cause no mutation. |

Final report timestamp: `2026.09.22-01.09.34`. Totals: **3 succeeded, 0 failed, 0 warnings, 0 not run, 0 in progress**. Machine-specific raw artifacts remain local under `artifacts/unreal-automation/` and `examples/JevSandbox/Saved/Logs/`.

The launcher checks the report's modification time, pass count, and failure count. Headless execution uses `UnrealEditor-Cmd.exe -NullRHI`; `-NoShaderCompile` is intentionally omitted because that combination crashed the installed engine during startup before test execution. A replacement-identity test fixture was also corrected to preserve the actor's level during rename. Both issues were followed by successful full reruns.

## Bridge guarantees and scope

- The bridge starts only with a printable non-space ASCII `JEV_BRIDGE_TOKEN` of 32–256 characters. A token should be randomly generated. The plugin never prints it.
- The listener explicitly binds `127.0.0.1:9845`; an occupied port fails closed. Requests require the matching loopback Host header and a bearer token, and requests with an Origin header are rejected.
- Unreal's HTTPServer dispatches the action handler on the game thread. Off-thread invocation is rejected. The plugin only unbinds its own route on shutdown and never stops shared HTTP listeners.
- Only editor inspection, allowed primitive spawning, and previews followed by explicit plan application are exposed. Play/Simulate blocks actions. There is no Python, console, deletion, arbitrary file writing, map saving, or arbitrary class construction endpoint.
- `set_transform` is intentionally restricted to the exact native `AStaticMeshActor` class with no attached parent, attached children, or child-actor ownership. Blueprint-derived actors are rejected because editor movement may run construction scripts with side effects.
- A plan is bound to the editor session, project, weak world identity, weak target identities, and original scene actor identities. Its revision covers actor identity, object path, class, root path, label, transforms, attachment, hidden/locked state, and the current level. UTF-8 hashing preserves Unicode label changes. It does **not** hash full material, mesh, Blueprint, or asset contents.
- Plans expire after 120 seconds, contain at most 20 operations, and are consumed on an apply attempt. At most 64 live plans are retained. Preview validation runs before an editor undo transaction is opened. Failures restore the supported transforms and remove newly spawned actors; arbitrary third-party editor callback side effects are outside this guarantee.
- Scene and asset responses contain at most 200 entries. Request bodies are limited to 64 KiB at the handler, and authenticated requests are limited to 30 per second. Unreal's HTTPServer parses the HTTP body before the handler runs, so this is not a transport-level defense against a hostile local process.

## Remaining acceptance boundaries

Native headless tests prove the supported scene operations and undo behavior. They do not prove visual appearance, gameplay quality, large production-map performance, multiplayer behavior, other Unreal versions, or compatibility with every third-party editor plugin. Network and MCP end-to-end evidence should be recorded separately after those checks complete.
