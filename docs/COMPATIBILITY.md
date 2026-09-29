# Compatibility and adoption evidence

0.10 is an alpha source release. Installation into an existing development host,
a native compile, automation, a live editor handshake and human acceptance are
different checks. There is no certification for every project or engine version.

| Surface | Target | Acceptance boundary |
| --- | --- | --- |
| Native editor | Windows, licensed Unreal 5.8.2 | Local source build and isolated JevSandbox tests; see dated [validation](VALIDATION.md). Other native engines/platforms are untested. |
| MCP transport | Official Python MCP SDK, stdio | Initialization, schemas, dispatch, reconnects and two-client group isolation. A client must explicitly support list-change notifications to switch groups in-session. |
| Python service | Python 3.12+ | Locked dependencies and CI tests; Python-only success does not certify native Unreal support. |
| Codex benchmark | Installed Codex app-server experimental dynamic-tool API | One bounded actual-agent read baseline; API drift fails closed. Model and usage are measured, not hardcoded. No Jev benefit established. |
| OpenRouter | Alpha Decisions endpoint, typed Jev model | Provider authentication and alpha contracts require separate live evidence. Offline fixtures are not provider acceptance. |
| Blender handoff | Binary FBX static meshes with retained `.blend` and hash manifest | Reviewed import/reimport plus dimensions, pivot-relative bounds and material-slot checks. Does not certify visuals, animation or arbitrary DCC assets. |
| Source control | Pinned local Git; optional explicitly configured Perforce | Hash checkpoints do not back up or restore files. Real Perforce server compatibility remains untested. |
| Gameplay | One project-approved standalone PIE session | Owned lifecycle, bounded viewport capture and project-authored tests. Packaged, multiplayer, remote devices and Gauntlet remain future work. |
| Community | Source-only public sandbox and recipes | Independent clean-host installation, accessibility, translation review and representative project adoption remain open. |

Start with the [connection doctor](DOCTOR.md), then follow the
[clean-host acceptance procedure](COMMUNITY_ACCEPTANCE.md). Use the same release
for Python and JevEditor; capability checks reject unsupported new native actions.
Never copy a machine-specific policy or credential into a public example.
