# Local team policy, durable receipts and named jobs

These optional Python client features require an explicit, reviewed
`JEV_RUNTIME_CONFIG` JSON file. They do not add shell, console, Python, filesystem
or C++ execution to the authenticated native editor bridge. Configuring the file
does not launch an engine process or authorize a build.

Use one private local state directory for every cooperating client of the same
project. Keep this configuration, the SQLite database and generated outputs out
of source control. State and output directories must be outside the project
source tree. Paths must be absolute local paths, without links or reparse points.
The startup configuration must name the exact `JEV_EXPECTED_PROJECT` or selected
connection-profile project. Changing policy requires restarting the MCP process.

## Configure a policy

For example, a project owner can save a private JSON file like this, replacing
the example paths with the intended disposable project and private directories:

```json
{
  "version": 1,
  "project_file": "C:/UnrealProjects/Example/Example.uproject",
  "state_directory": "C:/Users/YourName/AppData/Local/JevUnreal/Example",
  "allowed_actions": ["preview", "apply", "frame", "workflow_preview", "workflow_apply"],
  "asset_roots": ["/Game/Maps", "/Game/Props", "/Engine/BasicShapes"],
  "allowed_operations": ["spawn_primitive", "set_transform", "set_metadata", "light"],
  "blueprint_targets": [],
  "functional_tests": [],
  "validation_rules": [],
  "receipt_retention_days": 7,
  "max_receipts": 256,
  "jobs": []
}
```

Set `JEV_RUNTIME_CONFIG` to that file's absolute path in the local MCP environment.
`unreal_team_status` reports whether it is active, its policy hash, allowed actions,
package roots, retention limits, lease status and named jobs. No configuration
means these additional features are disabled; existing native identity,
authentication, permissions and stale-plan checks still apply.

The action allowlist covers preview/apply, viewport framing, selected native
validation, functional tests, Blueprint compile/edit previews and compilation,
domain previews/applies, and performance start/cancel. The operation allowlist
contains exact `op` and `kind` values inside requests, such as `set_transform`,
`material_scalar`, or `camera`. Include each needed value explicitly. Both the
current world and explicit asset/actor references must be under approved roots.
Root matching respects folder boundaries: `/Game/Props` does not approve
`/Game/PropsOther`. Primitive spawning also needs its `/Engine/BasicShapes` root.
Opt into an explicit `/Temp` root for unsaved editor worlds; it is never added
automatically and does not force a map save. Graph edits use their exact
`graph_edit.operation` value, such as `add_math_node`, in `allowed_operations`.
Normalized previews are checked recursively before authorization, including
implicit copied meshes, material overrides and attachment references.
Blueprint target, validator and functional-test aliases need their respective
explicit lists as well as the existing native project policies.

An apply needs a preview observed under the same policy and native editor session.
Authorization is consumed locally before dispatch, even if transport later fails.
A plan created outside this policy cannot be applied through this configured
client. Native one-shot consumption, revision checks, object checks and human
review still apply independently. Policy does not derive permissions from Jev
confidence, model text or a tool annotation. This is a cooperating-client policy,
not an operating-system boundary: another client without this configuration and
human editor actions are not governed by the Python policy. Trusted project
validators and compiler callbacks retain their documented side effects.

## Cooperating-client leases

Each configured mutation acquires the project's SQLite lease atomically before
dispatch and releases its automatic lease afterward. Concurrent clients using
the same directory receive `project_leased`; no operation is queued or retried.
Active requests renew their 60-second lease every ten seconds. A crashed process
stops renewing; expiry permits recovery. Clock changes and clients using another
state directory are outside this cooperative coordination guarantee. Native
stale-scene checks remain necessary to protect independent human edits.

Use `unreal_project_lease(seconds)` for a longer inspect/preview/review sequence
(one to 300 seconds) and `unreal_project_lease_release(lease_id)` to end an idle
lease early. Only its owning MCP process can release it. Active mutations extend
the lease while work is in progress. Active jobs cannot be unlocked through the
lease tool; cancel the job through its owned lifecycle instead. A manual lease
must be released before starting a named job. Discovery and read-only inspection
remain available while another client holds the lease.

Leases do not reserve Unreal against the user, other software or unrelated
clients. Cancellation of an HTTP request cannot undo an already dispatched
native operation; its durable receipt remains uncertain and requires fresh
inspection.

## Durable historical receipts

`unreal_durable_receipts(limit)` lists up to 100 recent records;
`unreal_durable_receipt(receipt_id)` reads one. Metadata is committed with SQLite
`synchronous=FULL` before dispatch. It contains exact project/session/world/revision,
policy and request hashes, bounded native plan/job IDs and observed outcome
digests. Request values, provider payloads, token values, captures and raw logs are
not retained. Native result metadata includes only bounded IDs/status and selected
boolean outcome fields.

Possible observations include `dispatched_uncertain`, `response_observed`,
`rejection_observed` and `outcome_uncertain`. A process that dies after dispatch but
before recording a response leaves `dispatched_uncertain`. Reopening the database
does not convert that into success or replay the operation. These records survive
MCP/editor restarts independently of native in-memory receipts, but cannot restore
the scene, establish whether a lost native response applied, or prove current state.
An observed response is not automatically a successful compile, save or verification.

Retention is one to thirty days and 16–1024 records, with at most 16 KiB per record
and a bounded database page budget. Expired/old records are pruned on access.
`unreal_durable_receipt_forget` deletes one record for privacy without changing
scene state or reauthorizing a consumed plan. Directory permissions are inherited
from the chosen location on Windows; choose a private directory. Receipts are not
encrypted, and logical deletion is not secure erasure of disk blocks/backups.
SQLite durability does not claim protection against every power-loss, filesystem
or hardware failure.

## Named local build, cook and package jobs

Jobs are opt-in Windows/Win64 workflows, with exactly three typed kinds:
`build`, `cook` and `package`. Add `engine_root`, `output_directory`, a configured
`editor_target` for builds, and explicit `jobs` entries to the private configuration:

```json
{
  "engine_root": "C:/Program Files/Epic Games/UE_5.8",
  "dotnet_relative_root": "Engine/Binaries/ThirdParty/DotNet/10.0/win-x64",
  "dotnet_host_sha256": "REPLACE_WITH_EXACT_64_LOWERCASE_HEX_DIGEST",
  "output_directory": "C:/UnrealBuildOutputs/Example",
  "editor_target": "ExampleEditor",
  "jobs": [
    {
      "name": "editor-build",
      "kind": "build",
      "executable_sha256": "REPLACE_WITH_EXACT_64_LOWERCASE_HEX_DIGEST",
      "timeout_seconds": 900,
      "max_output_bytes": 1048576,
      "max_artifact_bytes": 1073741824,
      "max_artifact_files": 4096,
      "minimum_free_bytes": 1073741824
    }
  ]
}
```

This second fragment extends the complete policy above; it is not a standalone
configuration. The placeholder digest deliberately fails validation. Obtain and
review the SHA-256 of the installed, licensed entrypoint selected by the job kind:

| Kind | Fixed executable under the configured engine root | Fixed workflow |
| --- | --- | --- |
| `build` | `Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.exe` | Configured editor target, Win64 Development, exact project, wait mutex and no hot reload |
| `cook` | `Engine/Binaries/Win64/UnrealEditor-Cmd.exe` | Exact project, `-run=cook`, Windows target, unattended, no Perforce and stdout |
| `package` | `Engine/Binaries/DotNET/AutomationTool/AutomationTool.exe` | `BuildCookRun`, exact project, Win64 Development, build/cook/stage/pak/package/archive |

Missing entrypoints fail closed. No fallback installs an SDK, invokes a `.bat`,
chooses a different executable, or accepts custom flags. The exact project file,
entrypoint and `Engine/Build/Build.version` are hashed in the preview and checked
again before launch. Build/package also require an explicitly selected bundled
.NET root under `Engine/Binaries/ThirdParty/DotNet/<numeric-version>/win-x64` and
an exact host SHA-256 pin. The managed assembly and runtime configuration are
bound in the preview. Generated `DOTNET_ROOT`/`DOTNET_ROOT_X64` point only to that
bundle; ambient .NET injection settings are not inherited.
The entrypoint pin is not a hash of the entire engine or of
every project source, plugin, DLL, SDK or dependency. These workflows execute
trusted engine/project build code and can modify project build outputs. The
Windows process job object contains process lifetime, not filesystem/network
permissions. Running any named job needs user authorization for that project and
workflow. Close the target editor before builds/cooks/packages and use disposable
projects for acceptance.

1. Call `unreal_named_job_preview(name)`. Review the exact identities, fixed command,
   output directory and budgets. It launches nothing and expires after 120 seconds.
2. Call `unreal_named_job_start(plan_id)` once within the authorized scope. The plan
   is consumed even if preflight fails. Jobs acquire the same project lease used by
   editor mutations; only one named job per client can run at a time.
3. Poll `unreal_named_job(job_id)`. It reports lifecycle, exit code, stdout/stderr
   byte count and hash, changed artifact counts and bounded artifact hash samples.
4. Use `unreal_named_job_cancel(job_id)` to terminate this client's process tree.
   Cancellation, timeout and budget failure preserve a partial receipt and leave
   outputs in place for review; they never delete or roll back project files.

Processes start suspended, join a Windows job object with kill-on-close, then
resume. Cancellation, server shutdown or owner-process termination closes that
boundary and terminates descendants. Failure to establish containment fails
closed. Jobs recheck their lease after preflight and input hashing before process
creation. A separate heartbeat renews the lease every ten seconds even during a
slow output scan; losing it terminates the owned process tree. Process-local job
records and their lifecycle metadata retain at most 32 completed/current jobs.
The child environment omits bridge/provider secrets; output is drained
with a byte limit and retained only as a hash/count. A timeout is one to 7200 seconds.

The monitor checks free disk space and file-count/byte-growth budgets for the new
job output directory and the project's `Binaries`, `Intermediate` and `Saved`
trees every 250 ms. This is an observed budget with possible overshoot, not an OS
disk quota; engine caches, SDK files and other locations written by trusted build
code are not covered. Preexisting file counts count toward the configured tree
limit. Artifact receipts list at most 32 samples and hash at most 64 MiB in total;
omissions are explicit. Build/cook output stays in Unreal's usual project folders;
packages archive under a unique configured output directory.

`succeeded` requires a zero exit code and observed changed nonempty output of the
expected kind; otherwise zero-exit jobs report `output_unverified`. Neither state
proves the whole game works or that a package launches on a clean machine. A
previous process's unfinished job record becomes `interrupted_uncertain` on read,
never a completed job or an automatic relaunch.

The fixed cooking/package workflow follows Epic's
[cooking command-line documentation](https://dev.epicgames.com/documentation/unreal-engine/cooking-content-in-unreal-engine)
and [build operations documentation](https://dev.epicgames.com/documentation/unreal-engine/build-operations-cooking-packaging-deploying-and-running-projects-in-unreal-engine).
Actual licensed-engine job acceptance is separate from the controlled Python
subprocess tests, which exercise success/failure, timeout, output/disk limits,
credential filtering, cancellation and descendant termination without Unreal.

## Restricted integration smoke

`uv run python scripts/smoke_infrastructure.py` requires the repository's exact
`examples/JevSandbox/JevSandbox.uproject` as the configured expected project. It
creates fresh private AppData state and uses two official MCP clients to exercise
lease conflict/release, one unsaved Cube preview/apply, durable receipt reconnect,
replay refusal, fresh measurement and read-only recipe resources. It never opens
or closes an editor, saves the map, or requests a provider decision.

With that sandbox editor closed, explicitly select a real engine job:

```powershell
uv run python scripts/smoke_infrastructure.py --jobs-only --editor-closed --job build --engine-root 'C:/Program Files/UE_5.8'
```

Optional `--job cook` and `--job package` are separate explicit selections. This
mode refuses a listening configured bridge endpoint, requires the operator's
closed-editor attestation, pins the selected UE 5.8 installation and its bundled
.NET 10.0 host, and creates no editor session. Reports go under ignored
`artifacts/infrastructure-<id>.json`; no-op builds can report `output_unverified`
with exit code zero and remain labeled that way. DCC round trips, packaged game
launches, human review and clean-machine acceptance are separate runs.
