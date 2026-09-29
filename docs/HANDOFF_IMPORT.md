# Reviewed Blender imports and reimports

The existing Blender exporter retains the `.blend` source and writes an explicit
`handoff.json` contract for source/export/texture hashes, local dimensions,
pivot-relative bounds, material slot order, and declared provenance. The reviewed
import workflow binds that contract to one locally configured native alias. The
MCP tool accepts aliases, never filesystem paths or arbitrary importer settings.

Set `JEV_HANDOFF_CONFIG` to a local JSON file outside shared source control:

```json
{
  "version": 1,
  "project_file": "C:/Projects/MyGame/MyGame.uproject",
  "bundles": [
    {"id": "chair", "directory": "C:/Art/Exports/chair", "asset_id": "chair"}
  ]
}
```

The directory must contain `handoff.json`, its recorded editable `.blend` source,
the exported FBX, and every other recorded file. Up to 32 aliases are accepted.
Absolute local paths are required; symbolic links, junctions and network shares
are rejected. The config's project must match `JEV_EXPECTED_PROJECT` or the active
project profile. Configuring the MCP layer alone does not authorize native import.

Also approve the exact source bytes and target in the project's local
`Config/DefaultGame.ini`:

```ini
[JevEditor.Handoff]
bEnabled=true
+Bundles=chair|C:/Art/Exports/chair/chair.fbx|<64-character-lowercase-sha256>|/Game/Props/Chair.Chair
```

Use the exported mesh's SHA-256 from the manifest in place of the placeholder.
An intentionally updated export requires reviewing its new manifest and hash,
then updating native approval before a new preview. Do not commit private source
paths, unreleased game data, credentials or Unreal binary assets to this project.

1. Inspect the editor identity and call `unreal_handoff_bundles` to discover aliases.
2. Call `unreal_handoff_import_preview(alias="chair", operation="import",
   expected_state=...)`. Use `reimport` only for the approved existing loaded,
   clean static mesh.
3. Review target path, mesh hash, retained-source verification, declared provenance
   and the dimension/pivot/slot contract. License text is an author declaration,
   not independent rights verification.
4. Apply the returned one-shot `plan_id` with `unreal_handoff_import_apply`.
   Every recorded bundle file is hashed again before native import. Changed local
   configuration, contract, bytes or editor state rejects the attempt.
5. Read the returned fresh native asset measurements. `verified` means dimension,
   pivot-relative bounds and ordered material slot names match within 0.1 cm.
   Any mismatch or failed observation becomes `needs_review` even when native
   import succeeded. Inspect appearance, textures, collision and gameplay
   separately, then save explicitly through the editor when accepted.

The native path is a bounded fixed static-mesh FBX importer, with no material or
texture import and no requested save. Mesh input is limited to 64 MiB. The bridge
stages verified source bytes rather than forwarding tool-provided paths. Trusted
import callbacks may have side effects; full rollback is not promised.

Plans expire after 120 seconds and are consumed before apply validation, including
failed attempts. A native timeout can leave an uncertain result: inspect the exact
target instead of retrying. Python-side bundle plans do not survive an MCP server
restart; create a new reviewed preview after reconnecting. A passed byte/metadata
contract is not visual acceptance or production readiness. See `VALIDATION.md`
for current mock, native build, actual import and visual evidence.

## Reproduce the guarded reimport check

The public [`smoke_handoff_reimport.py`](../scripts/smoke_handoff_reimport.py)
script applies one reimport to the saved public calibration mesh in this
repository's JevSandbox. It refuses another configured or connected project and
another preview target. This is an explicit mutation test, not a read-only smoke.

1. Prepare the public bundle and saved fixture using the separate
   [calibration preparation procedure](DCC_HANDOFF.md#reproducible-calibration-evidence).
   Review its explicit `-JevHandoffSaveFixture` preparation step: it saves only new
   owned calibration packages. Do not run preparation over an existing fixture.
2. Add `calibration-existing` to your private `JEV_HANDOFF_CONFIG`. Its directory
   is the absolute path to this checkout's `artifacts/handoff-v09`, its `asset_id`
   is `calibration-prop`, and `project_file` is this checkout's exact
   `examples/JevSandbox/JevSandbox.uproject`. For example, the bundle entry is:

   ```json
   {
     "id": "calibration-existing",
     "directory": "C:/Projects/Jev_Unreal/artifacts/handoff-v09",
     "asset_id": "calibration-prop"
   }
   ```

3. Review and add the matching native alias in the sandbox's local project policy:

   ```ini
   [JevEditor.Handoff]
   bEnabled=true
   +Bundles=calibration-existing|C:/Projects/Jev_Unreal/artifacts/handoff-v09/calibration.fbx|<manifest-mesh-sha256>|/Game/JevHandoff/SM_Calibration.SM_Calibration
   ```

   Replace the example checkout path and hash with your reviewed local bundle.
   This alias deliberately targets the **existing** saved calibration mesh. An
   import-only alias targeting a new asset is a separate approval; it cannot be
   substituted for this reimport test. Keep these machine-specific approvals local.
4. Start the exact sandbox with the normal authenticated profile and a clean
   saved calibration mesh. Resolve any pre-existing unsaved changes deliberately
   before restarting; the smoke does not save or discard them. It loads the mesh,
   obtains fresh editor identity, requests a bounded preview and applies once.
5. Run from the repository root with the reviewed profile and handoff config:

   ```powershell
   uv run python scripts/smoke_handoff_reimport.py
   ```

Success requires fresh native dimensions, pivot-relative bounds and ordered
material slots to match, plus an unchanged SHA-256 of the saved calibration
`.uasset`. The script requests no save and makes no provider calls. It writes only
a bounded summary to ignored `artifacts/roadmap-reimport-smoke.json`; it does not
print raw responses, configuration or credentials. It leaves the reimported mesh
dirty in memory. Run it only once after a fresh clean load. After a failure,
inspect the exact fixture and any uncertain result before deciding how to recover;
the script never retries or performs automatic cleanup. This check does not prove
material appearance, collision, arbitrary FBX compatibility or production readiness.
