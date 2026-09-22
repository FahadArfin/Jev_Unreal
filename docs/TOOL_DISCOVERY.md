# Discover local Unreal MCP tools

The discovery catalog helps an assistant find a relevant tool without loading every
external tool schema into its context. Discovery uses the official MCP client and
only lists metadata. It never calls external tools, launches programs, scans ports,
or grants permission to execute a recommendation. An outbound request allowlist
also rejects `tools/call` and endpoint changes at the transport boundary. Protocol
initialization, listing, ping/cancellation responses, and HTTP session cleanup are
allowed; these do not execute an advertised editor tool.

## Configure explicit endpoints

Create a local JSON file outside the repository, then set `JEV_CATALOG_FILE` to its
absolute path in the Jev_Unreal server's environment. For example:

```json
{
  "servers": [
    {"id": "epic", "url": "http://127.0.0.1:9846/mcp"},
    {
      "id": "community",
      "url": "http://127.0.0.1:9847/mcp",
      "bearer_env": "MY_LOCAL_MCP_TOKEN"
    }
  ]
}
```

These ports are examples; use each installed server's actual endpoint. Discovery
supports **Streamable HTTP** MCP, not legacy HTTP+SSE-only or stdio servers.
Only literal `127.0.0.1` or `[::1]` HTTP(S) addresses with an explicit port are
accepted. Host names, remote addresses, URL credentials, query strings, fragments,
redirects, and arbitrary command settings are rejected. HTTP proxy environment
settings are ignored. TLS certificate verification remains enabled for HTTPS.

`bearer_env` is optional. It names an environment variable containing that server's
credential; it is not the credential itself. Keep credentials in the existing local
secret setup, and never put their values into catalog JSON, repository files, or
chat messages. Discovery does not reuse your Jev provider key as an MCP credential.
No configured catalog is a valid state and produces an empty result.

If Epic's `tools/list` returns only `list_toolsets`, `describe_toolset`, and
`call_tool`, Epic's own tool-search gateway is enabled. The catalog reports a
warning; those three entries do not represent full discovery of its underlying
tools. To expose the full catalog, disable `bEnableToolSearch` in the target
Unreal MCP settings and refresh. This catalog deliberately does not invoke even
apparently read-only external meta-tools. Check the settings in the installed
engine version; their location may change between releases.

## Recommended assistant flow

1. Call `jev_catalog_refresh` after starting or changing an external MCP server.
   This initializes each configured connection, follows bounded `tools/list`
   pagination, closes it, and caches one complete snapshot in memory.
2. Call `jev_catalog_search` with the task, relevant tool name, or action name.
   Search runs locally and returns short descriptions, namespaced IDs, scores,
   and a `catalog_version`. It does not contact Jev or an editor.
3. Optionally call `jev_catalog_rerank` with the goal, 1–16 shortlisted IDs, and
   that version. This sends their bounded descriptions to the configured Jev
   provider. It returns a recommendation or deferral and `executed: false`.
4. Call `jev_catalog_get` with a chosen ID and version to obtain its exact
   advertised input/output schemas, external server/tool names, annotations,
   and any suggested action argument.
5. Use the existing external MCP connection to perform an authorized operation,
   following its own project identity, validation, and execution requirements.
   Jev_Unreal is not a generic tool-execution proxy.

Descriptions and annotations are untrusted external metadata. A read-only hint or
Jev confidence score is not a security boundary. A catalog listing also does not
verify which editor/project an external server will modify. Discovering a tool
does not automatically make it callable by the host assistant: its external MCP
connection must already be installed and available to that assistant.

Search uses deterministic BM25-style lexical scoring with camel-case/underscore
tokenization and strong exact-name/action boosts. It is not an embedding search
or a claim of semantic understanding. No lexical match returns an empty list.
Optional Jev reranking can reorder the decision among supplied candidates or
defer; it cannot recover a relevant tool omitted from that shortlist. No complete
catalog or schemas are automatically sent to Jev. Only an explicit rerank sends
the supplied goal and selected descriptions to the configured provider.

## Names, actions, and versions

IDs are namespaced, for example `epic:find_assets` and `community:find_assets`.
Long or unusual names get a stable readable prefix plus a hash; the original
external tool name is preserved separately. Duplicate tool names within one
server or duplicate configured server IDs are rejected.

Multiplexed tools advertising a top-level `action` enum, or direct `oneOf`/`anyOf`
branches with `action` enums/constants, also create searchable action entries.
For example, `community:manage_assets:inspect_collision` points to the original
`manage_assets` tool with `argument_preset: {"action": "inspect_collision"}`.
Its returned schema is the exact original schema, not an invented narrowed schema.
The preset does not imply all required arguments are present. Arbitrary nested
schema references are not expanded and descriptions are not mined for actions.

A SHA-256 catalog version covers endpoint identity and canonical tool metadata.
It remains stable when only refresh time or tool enumeration order changes.
Passing an old version after metadata or endpoint changes returns `catalog_stale`.
A refresh during reranking also rejects an outdated recommendation. Version checks
guard this local snapshot; they cannot prevent an external server changing after
discovery, so execution still needs that server's normal checks.

Refresh is atomic across all configured servers. A failure, invalid page, repeated
pagination cursor, malformed metadata, or exceeded budget preserves the entire
previous snapshot and reports a credential-free error. `jev_status` reports the
last refresh failure so cached results are not mistaken for a successful refresh.
There is no background polling, disk cache, or automatic catalog invalidation.
Explicitly refresh when server capabilities change.

## Resource limits

Defaults are also hard upper bounds, except that the Python API may increase the
request deadline to 30 seconds and total refresh deadline to 60 seconds:

| Limit | Default |
|---|---:|
| Config file | 64 KiB |
| Explicit servers | 8 |
| Pages per server | 128 |
| Entries including action variants | 8,192 |
| Action variants per tool | 256 |
| Each input/output schema | 256 KiB |
| Each HTTP response and parsed page | 2 MiB |
| Aggregate response bytes, stored metadata, and indexed text | 16 MiB each |
| Each tool description | 16,384 characters |
| Request deadline | 10 seconds |
| Total refresh deadline | 45 seconds |
| Search results | 1–20 through MCP; 1–32 through Python (default 8) |
| Jev shortlist | 1–16 |

Byte limits are checked before parsing HTTP bodies. Compressed responses are
refused so decompression cannot bypass them. The aggregate wire budget includes
initialization and stream traffic. Action variants share their parent schema in
memory. A server with a single oversized page must provide smaller pages; the
catalog does not silently truncate schemas or claim complete coverage.

## Python integration and validation

```python
from pathlib import Path
from jev_unreal.catalog import ToolCatalog

catalog = ToolCatalog(Path("local-tool-catalog.json"))
snapshot = await catalog.refresh()  # Compact CatalogSnapshot, no complete schemas
hits = catalog.search("inspect collision", limit=5, server_id="community")
if hits:
    exact = catalog.get(hits[0].id, version=snapshot.version)
    print(exact.name, exact.argument_preset)  # No execution occurs
```

`status()` returns a plain dictionary. `refresh()`, `search()`, and `get()` return
typed Pydantic models supporting `model_dump(mode="json")`. `rerank(client, goal,
tool_ids, version=None)` returns the normal routing dictionary with a catalog
version. The instance serializes refreshes; existing searches continue to use the
prior complete snapshot until the new one is ready.

The offline regression suite uses the actual MCP client against synthetic HTTP
responses. Two additional integration cases start a metadata-only official MCP
server on a dynamically allocated literal loopback port and use real TCP sockets,
with both JSON and SSE response modes. Each advertises 200 synthetic tools over
two pages, requires a synthetic bearer credential, has no `tools/call` handler,
and verifies GET/POST/DELETE session traffic through the standalone smoke. This
is protocol integration evidence, not live Unreal or third-party plugin acceptance.

The suite covers initialization, pagination, duplicate names, action variants,
version invalidation, partial failures, deadlines, body/schema/aggregate budgets,
redirect refusal, credential handling, sanitized errors, and recommendation-only
reranking. These tests do not demonstrate a particular third-party editor's live
compatibility or measure end-to-end productivity. Consult [VALIDATION.md](VALIDATION.md)
for separately recorded live integration evidence.

With your external server already running and `JEV_CATALOG_FILE` set, run:

```powershell
uv run python scripts/smoke_catalog.py --query actor
```

The bounded smoke performs two refreshes, lexical search, exact-schema retrieval,
and version rejection checks. It prints only counts, a catalog hash, and timing;
it makes no provider requests or external tool calls. Use a query appropriate to
your server's advertised catalog. An empty catalog or search fails the smoke.

Protocol reference: [MCP tools and pagination](https://modelcontextprotocol.io/specification/2025-11-25/server/tools).
Client implementation: [official MCP Python SDK](https://github.com/modelcontextprotocol/python-sdk).
