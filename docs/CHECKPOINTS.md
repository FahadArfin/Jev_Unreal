# Project file checkpoints and source-control awareness

These tools inspect exact approved disk files. They do not save editor buffers,
stage Git changes, submit Perforce changelists, check out files, restore a project
or run caller-supplied commands. A checkpoint is a **hash manifest, not a backup**.

Set `JEV_CHECKPOINT_CONFIG` to a private reviewed JSON file. It requires the same
explicit project as `JEV_EXPECTED_PROJECT` and `JEV_RUNTIME_CONFIG`; durable
checkpoint manifests use the existing private runtime receipt storage.

```json
{
  "version": 1,
  "project_file": "C:/Projects/Example/Example.uproject",
  "approved_files": [
    "Example.uproject",
    "Config/DefaultEngine.ini",
    "Content/Maps/Example.umap"
  ],
  "maximum_file_bytes": 8388608
}
```

The allowlist contains at most 32 exact relative files. Traversal, wildcard or
Perforce revision syntax, linked paths, directories and generated `Saved`,
`Intermediate`, `Binaries` or `.git` content are refused. Files are hashed only
when explicitly selected in a tool call. Missing files are recorded as absent;
changed-during-read files are refused. The default per-file bound is 8 MiB,
configurable up to 64 MiB. Larger assets need a different backup/version-control
workflow, not an unbounded read through these tools.

- `unreal_project_checkpoint(files)` records size/hash/existence plus the editor's
  dirty-package count and an explicit unsaved-buffer warning. It stores no file
  contents. It verifies the connected editor project before creating evidence.
- `unreal_project_checkpoint_compare(checkpoint_id)` compares current approved
  disk hashes with that historical manifest. The exact configuration must still
  match. It reports changed paths without modifying them.
- `unreal_project_vcs_status(files)` reports Git porcelain state and optional
  Perforce opened-file counts for the explicitly selected files. It does not
  need or edit a running editor.

## Optional Git and Perforce status

Configure `git_executable` with its absolute local path and `git_sha256` with
that installed executable's SHA-256. Perforce uses `p4_executable`, `p4_sha256`
and an explicit reviewed `p4_port`, such as `ssl:perforce.example.com:1666`.
Only TCP endpoints are accepted; inherited or registry endpoints cannot select
command transports. Perforce recursive `...` syntax is refused in file paths.
Both pairs are optional; unconfigured utilities are reported honestly. After a
trusted utility upgrade, review and update its pinned executable identity.

Commands use a fixed argument list without a shell, an exact configured project
working directory, ten-second timeout per command and 64 KiB output cap. Git first
reads its project prefix to map repository-root porcelain paths when the Unreal
project is in a subdirectory. Prefix output has a separate 4 KiB cap. Git receives literal
pathspecs after `--`; external fsmonitor and optional index writes are disabled,
and injected Git environment configuration is excluded. Perforce uses fixed
`opened` arguments with exact absolute files and explicit `-p` endpoint. Repository
P4CONFIG files and client SSO commands are disabled. It can contact the configured
Perforce server, uses the user's existing client/ticket settings, and performs no
automatic login or credential update. Missing configuration, authentication,
timeouts or command failures return `unavailable`; raw stderr is not returned.
Malformed/truncated Git records are unavailable evidence, never a clean result.
The fixed `-p` override follows [Perforce's global options](https://help.perforce.com/helix-core/server-apps/cmdref/current/Content/CmdRef/global.options.html);
Git path mapping follows the [porcelain v1 contract](https://git-scm.com/docs/git-status).

Source-control status is read-only evidence, not an exclusive file lock. Files can
change after the read, and an unchanged disk hash says nothing about unsaved
Unreal changes. Checkpoint comparison never claims an automatic rollback path.

Tests use temporary approved files, SQLite and mocked Git/Perforce responses.
Bounded child-process tests use a synthetic Python process solely to verify
output-limit/error handling. They do not establish real Perforce compatibility
or live editor behavior.
