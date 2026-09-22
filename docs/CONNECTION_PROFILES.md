# Multiple editor connections

Use one named connection profile per Unreal editor project and one MCP server
process per selected profile. A profile selects its project, loopback URL and
token file together at startup. Existing environment-only configurations continue
to work. There is no endpoint scan, automatic project switching or shared edit queue.

Create a private local JSON file, for example `.local/editor-profiles.json`:

```json
{
  "version": 1,
  "profiles": [
    {
      "id": "sandbox",
      "project_file": "D:/Games/JevSandbox/JevSandbox.uproject",
      "bridge_url": "http://127.0.0.1:9845",
      "bridge_token_file": "C:/Users/You/AppData/Local/JevUnreal/sandbox.token"
    },
    {
      "id": "mygame",
      "project_file": "D:/Games/MyGame/MyGame.uproject",
      "bridge_url": "http://127.0.0.1:9846",
      "bridge_token_file": "C:/Users/You/AppData/Local/JevUnreal/mygame.token"
    }
  ]
}
```

Use your actual paths and securely generated bridge token files. Profiles contain
file paths only; inline tokens and provider API keys are rejected. Keep profiles
and token files out of public repositories. Credentials are not copied or created
by the profile reader.

Inspect names and bindings without opening any token files:

```powershell
uv run jev-unreal profiles list '.local\editor-profiles.json'
```

For a Codex MCP entry that launches PowerShell, pass the matching launcher arguments:

```text
-NoProfile -File C:/Path/To/Jev_Unreal/scripts/Start-Mcp.ps1 -ProfilesFile C:/Private/editor-profiles.json -Profile mygame
```

Do not also pass `-ExpectedProject`, `-BridgeUrl` or `-BridgeTokenFile`. The launcher
rejects explicit conflicting arguments and clears inherited legacy connection
variables while the selected server runs, then restores the parent environment.

For another MCP client or a direct Python invocation:

```powershell
$env:JEV_PROFILES_FILE = 'C:\Private\editor-profiles.json'
$env:JEV_PROFILE = 'mygame'
uv run jev-unreal doctor
uv run jev-unreal serve
```

Both variables are required together. A missing profile or unreadable selected
token file fails before startup; the server never falls back to an inherited
project, URL or token. Only the selected profile's token is read, at most 4 KiB.
Provider credentials remain a separate setting and are unnecessary for local
Unreal tools. Restart that MCP connection after changing profiles: running servers
retain their original immutable settings.

## Launch the matching editor

Each editor must be launched with the same token and port as its profile. Set
`JEV_BRIDGE_PORT` in the environment of that editor before launching it; valid
ports are 1024–65535 and the default is 9845. Configure the native bridge token as
described in the main quick start. The selected `.uproject` must contain and enable
the matching JevEditor plugin. Profiles do not launch or retarget editor processes.

For legacy configurations without profiles, `JEV_BRIDGE_URL` still selects the
client URL. When it is absent, `JEV_BRIDGE_PORT` supplies the port for
`http://127.0.0.1`; otherwise the client uses port 9845.

Run `doctor` for each selected profile. A port responding is insufficient: the
client checks the authenticated editor's exact project identity before every read
or edit. If another project starts on that port, the request fails with
`wrong_project` before an edit is sent. The two-editor test fixtures verify this
behavior with separate synthetic tokens and endpoints.

## Limits

Files are bounded to 64 KiB and one to eight profiles. IDs use lowercase letters,
digits, hyphens and underscores, start with a letter, and are at most 48 characters.
Each profile must have a unique ID, project path and endpoint. Bridge URLs require
literal `http://127.0.0.1` with an explicit allowed port; DNS names, remote addresses,
credentials, paths, queries and fragments are rejected. Project and token paths
must be absolute local paths without parent traversal; network shares, symbolic
links and Windows reparse points are not used to read configuration or tokens.

These profiles isolate connections, not edits from multiple clients connected to
the same editor. Native stale-plan checks and one-shot plans still apply. Human
review, shared team permissions and multi-agent coordination are separate features.
