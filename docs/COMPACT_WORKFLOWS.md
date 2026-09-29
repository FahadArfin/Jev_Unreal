# Compact inspection and scoped tool catalogs

Compact reads negotiate the native `compact_read` capability. Older bridges use
the existing MCP projection fallback. Neither changes native edit permissions or
replaces preview, one-shot application and fresh verification. The full catalog
remains the default.

## Read only the fields needed

`unreal_read` accepts a `request` object. It supports `context`, `actors`,
`actor_details`, `assets` and `asset_details` using existing authenticated native
reads. For example:

```json
{
  "request": {
    "source": "actor_details",
    "actor_paths": ["/Temp/YourWorld.YourWorld:PersistentLevel.StaticMeshActor_0"],
    "fields": ["label", "bounds_cm"],
    "page_size": 10
  }
}
```

Replace that placeholder with a path from current inspection. The response keeps
project/session/world/revision identity and row paths/instance IDs. Edit blockers,
availability flags, truncation flags and warnings cannot be suppressed through
field selection. Unavailable requested fields are listed explicitly.

Actor fields include `label`, `class`, `folder`, `location`, `rotation`, `scale`,
`static_mesh_path`, `bounds_cm`, `materials`, `material_slot_count`,
`material_override_count`, `mesh_settings`, `collision_enabled`,
`attachment_parent_path`, `attachment_parent_instance_id`,
`attachment_relative_transform` and `attachment_socket`.

Asset fields include `name`, `class`, `loaded`, `static_mesh.bounds_cm`,
`static_mesh.lods`, `static_mesh.lod_count`, `static_mesh.material_slots`,
`static_mesh.material_slot_count` and `static_mesh.collision`.
For an exact asset, use `source: "asset_details"` and `path`.
Registry search remains restricted to the native `/Game` root; exact asset
inspection also supports native `/Engine` assets.

Omitting `fields` chooses a compact default. With the native capability, the
editor skips unrequested actor material arrays and mesh settings, and skips
unrequested asset material/LOD arrays and collision details. A name/class-only
exact asset read does not load the mesh. Native identity/revision checks still
run and can dominate inspection time; no speedup is claimed without measurement.
`metadata.projection_location` reports `native` or `mcp_legacy_fallback`.
The legacy path still fetches full bounded metadata before projection.

## Pages and changes

Repeat exactly the same request with `cursor: <next_cursor>` to read another
page. Pages come from one frozen capture. A changed editor identity/revision
refuses continuation; start a new read. Native captured results are capped at 200 entries
(20 for exact actor details). Pagination does **not** extend that cap. Inspect
native truncation flags and narrow the query when incomplete. Native discovery
examines at most 5,000 candidates before reporting `scan_incomplete`; a filtered
query can therefore remain incomplete. Native transport pages contain at most
100 projected rows. The MCP process gathers the bounded capture to preserve
complete change comparisons across all pages, then serves caller-sized pages.

Repeat the request with `since: <read_id>` for a fresh change-only response.
Changed/added rows contain the selected fields; `removed_from_result` means a row
left that bounded result, not that the object was deleted. Changing fields,
query, page size or source requires a new baseline. Deltas can themselves have
pages. Unselected properties are not compared. Asset content and selection may
change without changing the native actor revision; frozen pages remain historical.

Reads expire after 120 seconds, at most 32 reads / 8 MiB per MCP process, with a
1 MiB per-read budget. Native captures have their own 32-read / 8 MiB budget,
120-second expiry and 900,000-byte per-capture cap. Old reads can be evicted early.
Restart clears all IDs.
Concurrent calls are serialized. A detected state change during capture refuses
the read rather than storing a mixed baseline. Use `unreal_verify` for actual
edit acceptance; these projections are an inspection convenience.

## Advertise a smaller tool catalog

Set `JEV_TOOL_GROUPS` in the MCP server's environment before connecting:

```text
JEV_TOOL_GROUPS=core,scene,acceptance
```

Supported groups: `core`, `inspection`, `scene`, `blueprints`, `domains`,
`validation`, `gameplay`, `performance`, `team`, `jobs`, `decisions`, `discovery`,
`handoff`, `acceptance`, or `all` by itself. Core is always included. Domain tools
share a typed dispatcher for materials, cameras, lighting, rigs, UI and navigation;
this grouping does not narrow the dispatcher's native allowlists.

`jev_tool_groups` returns group membership, active count, exact catalog hash and
serialized schema bytes. It does not estimate tokens. `jev_tool_schema(name)`
returns one exact schema, including whether the tool is active, without enabling
or executing it. To activate another group in a compatible client:

```json
{"groups": ["core", "scene", "acceptance"], "client_supports_list_changed": true}
```

Call `jev_tool_groups_activate` with this object. Activation replaces this MCP
session's groups and sends the real `notifications/tools/list_changed` message;
core always stays available. Other clients sharing a server retain their own
catalogs. Reconnecting restores startup configuration. Calls already in progress
are not cancelled. A transport notification failure restores the previous groups.

MCP defines the server's `tools.listChanged` capability, but does not define a
client capability promising refresh. Set the explicit opt-in only when your
client supports that notification. The server cannot verify the client's UI
refresh. Without opt-in or a live session it changes nothing and returns
reconnect instructions. Clients that cache schemas should use `JEV_TOOL_GROUPS`
and reconnect instead. See the [MCP tools specification](https://modelcontextprotocol.io/specification/2025-11-25/server/tools).

Inactive tools are absent from `tools/list` and refused by `tools/call`. Groups
are a context-management feature, not a security boundary: native project policy,
authentication, leases and preview/apply rules still govern execution. Existing
workflow resources describe the complete product and can mention inactive tools.

Use [selective routing and health](ROUTING_HEALTH.md) for local decisions and
provider diagnostics, [acceptance workflows](ACCEPTANCE_WORKFLOWS.md) for
before/after evidence and [paired benchmarks](PAIRED_BENCHMARKS.md) to measure
workflow outcomes. Smaller response/schema bytes alone do not establish lower
billed tokens, faster completion or better gameplay.
