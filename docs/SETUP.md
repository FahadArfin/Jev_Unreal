# Install, inspect, update and repair

The source installer places JevEditor in one explicitly selected project's
`Plugins/JevEditor` directory and enables its `JevEditor` entry in the `.uproject`.
It works from a trusted local source checkout or source archive. It does not
download a plugin, obtain Unreal, install compilers, close editors, build code or
load credentials. A successful copy is not evidence of a working editor connection.

The wheel supplies the Python MCP server and installer, but does not bundle the
Unreal plugin source. Pass `--source-plugin` from the matching source release.
This is an independent MIT community project, not an official Epic or TypeSafe installer.

## Review a plan, then apply it

Close the selected project's editor before applying an install, update or removal.
Keep plan files and backup receipts local: they contain absolute paths and may
contain the selected project's configuration. They must not be committed publicly.

From the Jev_Unreal checkout in PowerShell:

```powershell
uv sync --locked --all-extras
.\scripts\Install-JevEditor.ps1 -Action Inspect `
  -ProjectFile 'D:\Games\MyGame\MyGame.uproject' `
  -EngineRoot 'C:\Program Files\UE_5.8' -Port 9845

.\scripts\Install-JevEditor.ps1 -Action Plan `
  -ProjectFile 'D:\Games\MyGame\MyGame.uproject' `
  -PlanFile '.local\mygame-install-plan.json'
Get-Content -LiteralPath '.local\mygame-install-plan.json'

.\scripts\Install-JevEditor.ps1 -Action Apply `
  -PlanFile '.local\mygame-install-plan.json'
```

The cross-platform core CLI has the same separation:

```text
jev-unreal setup inspect --project /work/MyGame/MyGame.uproject --source-plugin /work/Jev_Unreal/Plugins/JevEditor --port 9845
jev-unreal setup plan --project /work/MyGame/MyGame.uproject --source-plugin /work/Jev_Unreal/Plugins/JevEditor --output /work/private/install-plan.json
jev-unreal setup apply /work/private/install-plan.json
```

Review `can_apply`, `conflicts`, `actions`, `project_entry_before` and
`project_entry_after`. Installation changes only listed plugin source files, the
local ownership manifest, and the project's JevEditor entry. Other project fields
and plugin entries retain their values. Reformatting the project JSON is expected;
its original bytes are backed up. An existing explicit `Enabled: false` is shown
as an intentional enable change in the plan.

After applying, build the selected project with its matching licensed Unreal
installation and required C++ toolchain, then launch it with the local bridge
credentials. Blueprint-only projects also need a compiled native plugin; copying
source does not bypass that requirement. Use `JEV_EXPECTED_PROJECT` for the exact
absolute project path and a matching `JEV_BRIDGE_URL`, then run `jev-unreal doctor`
to authenticate and verify the running editor. Native bridge ports use
`JEV_BRIDGE_PORT` and must be between 1024 and 65535. The default is 9845.

Doctor's overall readiness requires both core inspect/edit/verify capabilities
and the project-tool capabilities, including 0.5 mesh editing. An older connected editor remains visible
under `editor` and `workflow_compatibility`, while `project_workflow_compatibility`
lists exactly which feature groups need an upgrade. Rebuild and relaunch the
matching source plugin, then rerun doctor. A ready capability report does not
enable or execute project-approved validators or functional tests.

The repository's JevSandbox already uses `AdditionalPluginDirectories` to discover
the source checkout. Do not install a duplicate copy into that sandbox. Setup
reports this conflict instead of creating ambiguous plugin discovery.

## Updates and repair

Generate a new install plan against the new trusted source release. The installer
compares SHA-256 hashes with its previous manifest:

- Unchanged owned files may be updated or removed when a release removes them.
- Missing owned files may be restored from source.
- Modified files and unknown pre-existing files block an update, even when an
  unknown file happens to match the release. The installer never silently adopts it.
- Project, source or destination changes after review make the plan stale. Generate
  a new plan and review the changed state.
- A second setup operation cannot enter while the per-project installer lock exists.
  Installation and recovery also share a kernel-held lease. The operating system
  releases that lease when a process exits; recovery never guesses from lock age
  or a possibly reused process ID. Its one-byte `.JevEditor.setup.lease` file stays
  in `Plugins` so all operations continue locking the same file.

Resolve a conflict by inspecting and preserving your local edits, then merging
them with the new source or restoring the intended known version yourself. Do not
edit manifest hashes to force an upgrade. The manifest records local ownership;
it is not a signature or proof that downloaded code is trustworthy.

## Removal and recovery

```powershell
.\scripts\Install-JevEditor.ps1 -Action UninstallPlan `
  -ProjectFile 'D:\Games\MyGame\MyGame.uproject' `
  -PlanFile '.local\mygame-uninstall-plan.json'
Get-Content -LiteralPath '.local\mygame-uninstall-plan.json'
.\scripts\Install-JevEditor.ps1 -Action Apply `
  -PlanFile '.local\mygame-uninstall-plan.json'
```

Removal deletes only unchanged installer-owned source files and the ownership
manifest. Modified source, untracked files, generated `Binaries`/`Intermediate`,
and backups remain. A retained modified source produces `partially_uninstalled`.
The original JevEditor project entry is restored, or the installer-created entry
is removed. If someone changes that entry after installation, removal is blocked
until the conflict is reviewed. Other later project edits are preserved.

Every operation that writes files retains original bytes and a `receipt.json`
under the selected project's `Plugins/.JevEditorBackups/<operation-id>` directory.
Each new receipt maps an exact target path to its numbered original file, before
hash and intended after hash. All originals and the prepared receipt are written
before changing any target. Receipts use schema 2; older schema-1 receipts remain
readable for manual recovery but lack the after hashes needed for automatic recovery.
There is no automatic recursive cleanup. Keep these private and remove backups
manually only after reviewing the receipt and confirming they are no longer needed.

Ordinary failed writes trigger rollback of files changed by that operation. If a
file changes again during failure, rollback preserves that external edit and
reports `setup_rollback_failed` while retaining its operation lock. A successfully
rolled-back operation is closed and cannot later undo a new installation. Recovery
requires the selected receipt's matching retained lock. After a process crash or
rollback failure, close
the selected editor, list receipts and review an explicit recovery plan:

```text
jev-unreal setup recovery --project /work/MyGame/MyGame.uproject
jev-unreal setup recovery-plan --project /work/MyGame/MyGame.uproject --operation OPERATION_ID --output /work/private/recovery-plan.json
jev-unreal setup recover /work/private/recovery-plan.json
```

Use the exact 32-character `operation_id` from the local receipt listing. Review
`mode`, `actions`, `unchanged`, `conflicts` and `can_apply`. Recovery rechecks the
receipt, target hashes, backup hashes and matching lock under the exclusive lease.
An active installer blocks recovery. Unrelated edits block the entire operation;
they are never overwritten. Only files still matching the interrupted installer's
intended output can be restored or removed. Files already at their original hash
are left alone. Every restoration rechecks its target and backup immediately
before writing. A failed recovery retains its lock and receipt so a newly reviewed
plan can continue from the remaining hashes.

If the successful install's final receipt was written before its process exited,
recovery uses `finish_committed_install`: it verifies every completed target and
clears the matching stale lock without undoing the successful installation. An
already completed operation with no retained lock cannot be replayed as recovery.

Legacy receipts, missing prepared receipts, damaged backups and unsafe paths need
manual inspection. Do not remove a retained lock until no setup operation is
running. The manifest and receipt establish local bookkeeping, not source trust or
cryptographic authenticity. Power-loss durability and hostile concurrent filesystem
replacement are not promised.

## What inspection proves

`setup inspect` reports the explicit project path, plugin enablement, managed source
version and modified files, selected engine build metadata, standard-location
Windows MSVC/SDK presence, Python and installed dependency version compatibility,
bounded installer receipt metadata, a loopback port bind probe, credential file
presence and the existence of Unreal's autosave recovery metadata. It never reads
key/token or Unreal autosave-file contents, clears autosaves, or sends a provider request.

An unavailable port may belong to the intended running editor or another process;
the bind probe cannot distinguish them. `doctor` performs the authenticated project
check. Toolchain files being present is not compilation proof. Custom compiler
installations may not be found, and a missing default credential file does not
invalidate a deliberately configured alternative credential path.

Installer fixtures test disposable projects, updates, repair, removal, stale plans,
project entry restoration, rollback, modified/untracked protection, unsafe paths,
real subprocess exits during install/upgrade/removal, resumed recovery, active
lease rejection and Windows file-sharing locks. These tests establish behavior on
the tested host. They do not establish fresh-machine installation success or
macOS/Linux Unreal runtime acceptance.

For clean-host acceptance, record the host's provenance (fresh VM or an existing
development machine), OS, Python version, source release hash and licensed Unreal
version. Run locked dependency installation and `setup inspect`; then install in
a disposable project, build it, launch it and run authenticated `doctor` plus a
bounded inspect/preview/apply/verify workflow. Repeat with an upgrade and a retained
crash receipt. Keep the local reports alongside the engine build and bridge evidence.
Mark any omitted step as not run. Dependency metadata, passing disposable fixture
tests and a successful source copy alone do not establish a working clean host.

To repeat the source lifecycle using two real local releases, select the older
trusted plugin and the current plugin explicitly:

```powershell
uv run python scripts/smoke_setup.py --baseline-plugin D:\Releases\v0.6\Plugins\JevEditor --source-plugin Plugins\JevEditor --workspace artifacts\setup-lifecycle-new --report artifacts\setup-lifecycle-new.json
```

The workspace and report must be new. This helper freezes bounded source snapshots,
then checks install, changed-source protection, a real process exit during upgrade,
reviewed recovery, successful upgrade, repair and removal in Blueprint-style and
C++-style disposable project descriptors. It retains backups for inspection and
emits a report without local paths. It does not build either project descriptor or
launch Unreal, and labels its evidence as an existing-host source lifecycle.
