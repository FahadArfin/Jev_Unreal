# Diagnose the selected connection

Run `uv run jev-unreal doctor` from the same configured environment used to start
the MCP server. Doctor checks only the explicitly selected project/profile and
loopback endpoint. It never scans ports, switches projects, starts an editor,
changes credentials or sends a provider request.

The report separates:

- Editor authentication and exact project identity.
- Native capabilities and plugin/server version metadata.
- Optional provider-key presence and credential-source diagnostics.
- Actionable `issues`, each with a fixed code, severity and next action.

`ready` describes local editor-tool readiness, not Jev authentication or gameplay
acceptance. A missing provider key does not prevent local Unreal tools from
working. Native capability support does not enable project policies or approve
execution of project tests.

## Which credential did the process load?

`provider.diagnostics` names the expected environment variable and reports
presence only for process/User/Machine environment scopes. Windows persistent
environment checks query registry value metadata without retrieving secret
value bytes. Unsupported or unreadable scopes are `null`, not falsely absent.

The report also records whether the known saved DPAPI file exists and its UTC
modification timestamp. It never opens/decrypts that file, prints a credential,
reports a key prefix or length, or computes a credential hash.

`selected_source` is one of:

- `not_loaded`: this process has no provider key, even if a saved file exists.
- `process_environment`: a key was present in the launching environment.
- `saved_dpapi`: the launcher explicitly reported loading the saved key.
- `settings_supplied`: a caller supplied settings directly without a recognized
  launcher marker or matching process variable.

A validated `JEV_CREDENTIAL_SOURCE` marker is labeled `launcher_reported`; it is
provenance metadata, not authentication evidence. Without such a marker, doctor
does not guess that an environment value originally came from DPAPI. The existing
Windows launcher prefers an already-set process environment key over the saved
file, so replacing only the saved file may not affect a running process.

When a key has been updated, compare the reported file timestamp with the intended
update, check which source was selected, and reconnect the MCP process to reload
it. Use the secure local helper `scripts/Set-OpenRouterKey.ps1`; never paste a key
into chat. Direct CLI invocation does not itself decrypt the saved Windows file.

Doctor always reports provider authentication as `unknown`. A separate explicit
`jev_provider_health(probe=true)` performs one bounded cloud check when wanted;
its live result is different evidence from local source inspection. HTTP 401
does not reveal why a provider rejected a key, and local presence cannot explain
or override that rejection.

## Common editor issues

For `wrong_project`, choose the intended explicit profile. Do not change the
expected identity simply to accept another game's open editor. For
`unauthorized`, initialize the same local bridge token for this project's editor
and launcher. For `editor_unavailable`, verify that the intended project is built
and running with JevEditor at its configured loopback port. Version or capability
mismatches require the matching plugin build and a reconnect, not credential
changes.

The setup inspector also includes modification timestamps in its existing
credential-file metadata. Its port bind probe proves only that a port was
available or unavailable at that instant; it does not authenticate a bridge.
