"""Offline discovery tests exercise the real MCP client over a synthetic HTTP transport."""

import asyncio
import importlib.util
import json
import socket
import sys
from pathlib import Path
from unittest.mock import AsyncMock

import httpx
import pytest
import uvicorn
from mcp import types
from mcp.server.lowlevel import Server
from mcp.server.streamable_http_manager import StreamableHTTPSessionManager
from mcp.server.transport_security import TransportSecuritySettings
from starlette.applications import Starlette
from starlette.responses import Response
from starlette.routing import Route

from jev_unreal.catalog import (
    CatalogLimits,
    CatalogServer,
    ToolCatalog,
    _BoundedTransport,
    _ByteBudget,
)
from jev_unreal.errors import JevError

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "jev_smoke_catalog", ROOT / "scripts/smoke_catalog.py"
)
smoke_catalog = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = smoke_catalog
SPEC.loader.exec_module(smoke_catalog)


def tool(name="inspect_actors", description="Read actor transforms and selection", **kwargs):
    return {
        "name": name,
        "description": description,
        "inputSchema": {"type": "object", "properties": {"query": {"type": "string"}}},
        **kwargs,
    }


class FakeMCP:
    """A JSON Streamable HTTP server: real initialize/notification/list protocol."""

    def __init__(self, pages=None):
        self.pages = pages or {None: {"tools": [tool()]}}
        self.methods = []
        self.requests = []
        self.failure = None

    async def handle(self, request):
        self.requests.append(request)
        if request.method == "GET":
            return httpx.Response(405)
        if request.method == "DELETE":
            return httpx.Response(204)
        message = json.loads(request.content)
        method = message.get("method")
        self.methods.append(method)
        if method == "initialize":
            result = {
                "protocolVersion": message["params"]["protocolVersion"],
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "synthetic-discovery", "version": "1.0"},
            }
        elif method == "notifications/initialized":
            return httpx.Response(202)
        elif method == "tools/list":
            if self.failure:
                return await self.failure(request)
            cursor = message.get("params", {}).get("cursor")
            result = self.pages[cursor]
        else:
            raise AssertionError(f"Unexpected protocol method: {method}")
        return httpx.Response(
            200,
            json={"jsonrpc": "2.0", "id": message["id"], "result": result},
        )


def configured(tmp_path, fake, *, servers=None, limits=None):
    path = tmp_path / "catalog.json"
    path.write_text(
        json.dumps({"servers": servers or [{"id": "epic", "url": "http://127.0.0.1:7777/mcp"}]}),
        encoding="utf-8",
    )
    return ToolCatalog(
        path, limits=limits, transport_factory=lambda: httpx.MockTransport(fake.handle)
    )


async def test_unconfigured_is_valid_and_network_free():
    catalog = ToolCatalog()
    assert catalog.status()["configured"] is False
    snapshot = await catalog.refresh()
    assert snapshot.entry_count == 0
    assert catalog.search("actors") == []


async def test_real_sdk_pagination_search_schema_and_no_execution(tmp_path):
    fake = FakeMCP(
        {
            None: {"tools": [tool()], "nextCursor": "next"},
            "next": {"tools": [tool("find_assets", "Search material and mesh assets")]},
        }
    )
    catalog = configured(tmp_path, fake)
    snapshot = await catalog.refresh()
    assert snapshot.tool_count == snapshot.entry_count == 2
    assert fake.methods == ["initialize", "notifications/initialized", "tools/list", "tools/list"]
    hits = catalog.search("mesh assets")
    assert hits[0].id == "epic:find_assets"
    assert hits[0].catalog_version == snapshot.version
    assert catalog.get(hits[0].id, snapshot.version).input_schema == tool()["inputSchema"]
    requests_before = len(fake.requests)
    assert catalog.search("inspect_actors")[0].id == "epic:inspect_actors"
    assert catalog.search("doesnotexist") == []
    assert len(fake.requests) == requests_before


@pytest.mark.parametrize(
    "url",
    [
        "https://example.com/mcp",
        "http://localhost:7777/mcp",
        "http://127.1:7777/mcp",
        "http://127.0.0.1/mcp",
        "http://127.0.0.1:0/mcp",
        "http://0.0.0.0:7777/mcp",
        "http://user:secret@127.0.0.1:7777/mcp",
        "http://127.0.0.1:7777/mcp?token=secret",
        "http://127.0.0.1:7777/mcp#secret",
        "file:///etc/secret",
        "http://[::ffff:127.0.0.1]:7777/mcp",
        "http://127.0.0.1:7777/\nsecret",
        "http://127.0.0.1:7777\\remote/mcp",
    ],
)
def test_only_explicit_literal_loopback_endpoints(url):
    with pytest.raises(ValueError):
        CatalogServer(id="local", url=url)


def test_ipv6_literal_allowed():
    assert CatalogServer(id="ipv6", url="http://[::1]:7777/mcp").id == "ipv6"


async def test_config_rejects_commands_and_duplicate_server_ids_before_network(tmp_path):
    fake = FakeMCP()
    catalog = configured(tmp_path, fake)
    catalog.config_path.write_text('{"servers":[{"id":"a","command":"python"}]}')
    with pytest.raises(JevError, match="Invalid tool catalog"):
        await catalog.refresh()
    catalog.config_path.write_text(
        json.dumps(
            {
                "servers": [
                    {"id": "a", "url": "http://127.0.0.1:7777/mcp"},
                    {"id": "a", "url": "http://127.0.0.1:8888/mcp"},
                ]
            }
        )
    )
    with pytest.raises(JevError, match="Invalid tool catalog"):
        await catalog.refresh()
    assert fake.requests == []


async def test_namespaces_duplicates_between_servers(tmp_path):
    fake = FakeMCP()
    catalog = configured(
        tmp_path,
        fake,
        servers=[
            {"id": "epic", "url": "http://127.0.0.1:7777/mcp"},
            {"id": "community", "url": "http://127.0.0.1:8888/mcp"},
        ],
    )
    snapshot = await catalog.refresh()
    assert snapshot.tool_count == 2
    assert {hit.id for hit in catalog.search("inspect_actors")} == {
        "epic:inspect_actors",
        "community:inspect_actors",
    }
    assert len(catalog.search("actors", server_id="epic")) == 1


async def test_action_variants_preserve_exact_schema_and_arguments(tmp_path):
    schema = {
        "type": "object",
        "properties": {
            "action": {"type": "string", "enum": ["create_material", "inspect_collision"]},
            "options": {"type": "object"},
        },
        "required": ["action"],
    }
    fake = FakeMCP({None: {"tools": [tool("manage_assets", inputSchema=schema)]}})
    catalog = configured(tmp_path, fake)
    snapshot = await catalog.refresh()
    assert snapshot.tool_count == 1 and snapshot.entry_count == 3
    hit = catalog.search("inspect_collision")[0]
    assert hit.argument_preset == {"action": "inspect_collision"}
    assert catalog.get(hit.id).input_schema == schema
    entry = catalog.get(hit.id)
    entry.input_schema["properties"].clear()
    entry.argument_preset.clear()
    assert catalog.get(hit.id).input_schema == schema
    assert catalog.get(hit.id).argument_preset == {"action": "inspect_collision"}


async def test_oneof_action_const_variants_and_unusual_names(tmp_path):
    schema = {
        "oneOf": [
            {"properties": {"action": {"const": "read material"}}},
            {"properties": {"action": {"const": "write material"}}},
        ]
    }
    fake = FakeMCP({None: {"tools": [tool("tools with spaces", inputSchema=schema)]}})
    catalog = configured(tmp_path, fake)
    await catalog.refresh()
    entries = catalog.search("read material")
    assert entries[0].argument_preset == {"action": "read material"}
    assert len(entries[0].id) <= 128 and " " not in entries[0].id


async def test_version_stable_across_order_and_changes_on_metadata(tmp_path):
    fake = FakeMCP({None: {"tools": [tool("one"), tool("two")]}})
    catalog = configured(tmp_path, fake)
    first = await catalog.refresh()
    fake.pages[None]["tools"].reverse()
    second = await catalog.refresh()
    assert first.version == second.version
    fake.pages[None]["tools"][0]["description"] = "Changed meaning"
    third = await catalog.refresh()
    assert third.version != first.version
    with pytest.raises(JevError) as caught:
        catalog.get("epic:one", version=first.version)
    assert caught.value.code == "catalog_stale"


async def test_version_changes_when_endpoint_changes_with_identical_metadata(tmp_path):
    fake = FakeMCP()
    catalog = configured(tmp_path, fake)
    first = await catalog.refresh()
    catalog.config_path.write_text(
        json.dumps({"servers": [{"id": "epic", "url": "http://127.0.0.1:8888/mcp"}]})
    )
    assert (await catalog.refresh()).version != first.version


async def test_epic_gateway_only_warns_about_incomplete_underlying_catalog(tmp_path):
    fake = FakeMCP(
        {
            None: {
                "tools": [tool(name) for name in ("list_toolsets", "describe_toolset", "call_tool")]
            }
        }
    )
    catalog = configured(tmp_path, fake)
    snapshot = await catalog.refresh()
    assert snapshot.tool_count == 3
    assert len(snapshot.warnings) == 1
    assert "bEnableToolSearch" in snapshot.warnings[0]
    assert "tools/call" not in fake.methods


async def test_large_catalog_and_schema_sharing_across_action_variants(tmp_path):
    schema = {
        "type": "object",
        "description": "x" * 60000,
        "properties": {"action": {"enum": [f"operation_{index}" for index in range(200)]}},
    }
    fake = FakeMCP(
        {
            None: {
                "tools": [tool(f"utility_{i}") for i in range(2000)]
                + [tool("manage_many_actions", inputSchema=schema)]
            }
        }
    )
    catalog = configured(tmp_path, fake)
    snapshot = await catalog.refresh()
    assert snapshot.tool_count == 2001
    assert snapshot.entry_count == 2201
    assert catalog.search("operation_199")[0].argument_preset == {"action": "operation_199"}
    assert catalog.get("epic:manage_many_actions:operation_199").input_schema == schema


async def test_repeated_property_names_are_bounded_before_building_large_indexes(tmp_path):
    schema = {
        "properties": {
            **{f"parameter_name_{index:03}": {} for index in range(50)},
            "action": {"enum": [f"op_{index}" for index in range(6)]},
        }
    }
    catalog = configured(
        tmp_path,
        FakeMCP({None: {"tools": [tool("many_parameters", inputSchema=schema)]}}),
        limits=CatalogLimits(max_total_bytes=4000),
    )
    with pytest.raises(JevError) as caught:
        await catalog.refresh()
    assert caught.value.code == "catalog_budget_exceeded"
    assert catalog.status()["entry_count"] == 0


async def test_concurrent_refreshes_are_serialized(tmp_path):
    fake = FakeMCP()
    catalog = configured(tmp_path, fake)
    snapshots = await asyncio.gather(catalog.refresh(), catalog.refresh())
    assert snapshots[0].version == snapshots[1].version
    assert fake.methods == [
        "initialize",
        "notifications/initialized",
        "tools/list",
        "initialize",
        "notifications/initialized",
        "tools/list",
    ]


async def test_cancelled_refresh_keeps_previous_snapshot(tmp_path):
    fake = FakeMCP()
    catalog = configured(tmp_path, fake)
    previous = await catalog.refresh()
    entered = asyncio.Event()

    async def wait_indefinitely(request):
        entered.set()
        await asyncio.Event().wait()

    fake.failure = wait_indefinitely
    task = asyncio.create_task(catalog.refresh())
    await asyncio.wait_for(entered.wait(), timeout=1)
    task.cancel()
    with pytest.raises(asyncio.CancelledError):
        await task
    assert catalog.status()["version"] == previous.version
    assert catalog.get("epic:inspect_actors").name == "inspect_actors"


async def test_atomic_partial_failure_retains_all_old_entries(tmp_path):
    fake = FakeMCP()
    catalog = configured(
        tmp_path,
        fake,
        servers=[
            {"id": "first", "url": "http://127.0.0.1:7777/mcp"},
            {"id": "second", "url": "http://127.0.0.1:8888/mcp"},
        ],
    )
    snapshot = await catalog.refresh()
    fake.pages = {None: {"tools": [tool("replacement")]}}

    async def fail_second(request):
        if request.url.port == 8888:
            raise httpx.ConnectError("SYNTHETIC_PRIVATE_TOKEN")
        message = json.loads(request.content)
        return httpx.Response(
            200, json={"jsonrpc": "2.0", "id": message["id"], "result": fake.pages[None]}
        )

    fake.failure = fail_second
    with pytest.raises(JevError) as caught:
        await catalog.refresh()
    assert "SYNTHETIC_PRIVATE_TOKEN" not in str(caught.value)
    assert catalog.status()["version"] == snapshot.version
    assert len(catalog.search("inspect_actors")) == 2
    assert catalog.search("replacement") == []
    assert catalog.status()["last_refresh_error"]


@pytest.mark.parametrize(
    "pages",
    [
        {None: {"tools": [tool(), tool()]}},
        {
            None: {"tools": [tool()], "nextCursor": "loop"},
            "loop": {"tools": [], "nextCursor": "loop"},
        },
        {None: {"tools": [], "nextCursor": ""}},
    ],
)
async def test_duplicate_names_and_invalid_pagination_rejected(tmp_path, pages):
    catalog = configured(tmp_path, FakeMCP(pages))
    with pytest.raises(JevError) as caught:
        await catalog.refresh()
    assert caught.value.code == "catalog_protocol_error"
    assert catalog.status()["entry_count"] == 0


@pytest.mark.parametrize(
    "limits,pages",
    [
        ({"max_entries": 1}, {None: {"tools": [tool("one"), tool("two")]}}),
        (
            {"max_schema_bytes": 128},
            {None: {"tools": [tool(inputSchema={"description": "x" * 200})]}},
        ),
        ({"max_pages_per_server": 1}, {None: {"tools": [], "nextCursor": "another"}}),
        (
            {"max_actions_per_tool": 1},
            {
                None: {
                    "tools": [
                        tool(inputSchema={"properties": {"action": {"enum": ["one", "two"]}}})
                    ]
                }
            },
        ),
    ],
)
async def test_semantic_budgets_fail_atomically(tmp_path, limits, pages):
    catalog = configured(tmp_path, FakeMCP(pages), limits=CatalogLimits(**limits))
    with pytest.raises(JevError) as caught:
        await catalog.refresh()
    assert caught.value.code == "catalog_budget_exceeded"
    assert catalog.status()["entry_count"] == 0


async def test_http_declared_and_streamed_body_budget(tmp_path):
    fake = FakeMCP()
    catalog = configured(tmp_path, fake, limits=CatalogLimits(max_response_bytes=512))

    async def declared(request):
        return httpx.Response(200, headers={"Content-Length": "999999999"})

    fake.failure = declared
    with pytest.raises(JevError) as caught:
        await catalog.refresh()
    assert caught.value.code == "catalog_budget_exceeded"

    class Stream(httpx.AsyncByteStream):
        closed = False

        async def __aiter__(self):
            yield b"x" * 300
            yield b"x" * 300
            raise AssertionError("Must stop before an unbounded stream")

        async def aclose(self):
            self.closed = True

    stream = Stream()

    async def streamed(request):
        return httpx.Response(200, headers={"Content-Type": "application/json"}, stream=stream)

    fake.failure = streamed
    with pytest.raises(JevError) as caught:
        await catalog.refresh()
    assert caught.value.code == "catalog_budget_exceeded"
    assert stream.closed


async def test_aggregate_http_budget_and_config_budget(tmp_path):
    fake = FakeMCP(
        {
            None: {"tools": [tool("one")], "nextCursor": "two"},
            "two": {"tools": [tool("two")]},
        }
    )
    catalog = configured(tmp_path, fake, limits=CatalogLimits(max_total_bytes=400))
    with pytest.raises(JevError) as caught:
        await catalog.refresh()
    assert caught.value.code == "catalog_budget_exceeded"
    catalog = configured(tmp_path, fake, limits=CatalogLimits(max_config_bytes=128))
    catalog.config_path.write_text(" " * 129)
    with pytest.raises(JevError) as caught:
        await catalog.refresh()
    assert caught.value.code == "catalog_budget_exceeded"


@pytest.mark.parametrize(
    "status,headers",
    [
        (307, {"Location": "http://127.0.0.1:7777/another"}),
        (302, {"Location": "https://external.example/private"}),
        (200, {"Content-Encoding": "gzip"}),
    ],
)
async def test_redirects_and_compression_are_rejected(tmp_path, status, headers):
    fake = FakeMCP()
    catalog = configured(tmp_path, fake)

    async def response(request):
        return httpx.Response(status, headers=headers)

    fake.failure = response
    with pytest.raises(JevError) as caught:
        await catalog.refresh()
    assert caught.value.code == "catalog_protocol_error"
    assert all(str(request.url) == "http://127.0.0.1:7777/mcp" for request in fake.requests)


@pytest.mark.parametrize(
    "method,url,payload",
    [
        (
            "POST",
            "http://127.0.0.1:7777/mcp",
            {"method": "tools/call", "params": {"name": "delete"}},
        ),
        ("POST", "http://127.0.0.1:7777/mcp", {"method": "resources/read"}),
        ("POST", "http://127.0.0.1:8888/mcp", {"method": "tools/list"}),
        ("POST", "http://127.0.0.1:7777/other", {"method": "tools/list"}),
        ("PUT", "http://127.0.0.1:7777/mcp", {}),
    ],
)
async def test_outbound_boundary_forbids_execution_and_endpoint_changes(method, url, payload):
    reached_transport = []
    wrapped = httpx.MockTransport(lambda request: reached_transport.append(request))
    transport = _BoundedTransport(
        wrapped, CatalogLimits(), _ByteBudget(1048576), "http://127.0.0.1:7777/mcp"
    )
    async with httpx.AsyncClient(transport=transport) as client:
        with pytest.raises(JevError) as caught:
            await client.request(method, url, json=payload)
    assert caught.value.code == "catalog_protocol_error"
    assert reached_transport == []


async def test_total_deadline_is_bounded_and_retains_previous(tmp_path):
    fake = FakeMCP()
    catalog = configured(tmp_path, fake, limits=CatalogLimits(refresh_timeout_seconds=0.1))
    previous = await catalog.refresh()

    async def blocked(request):
        await asyncio.Event().wait()

    fake.failure = blocked
    with pytest.raises(JevError) as caught:
        await asyncio.wait_for(catalog.refresh(), timeout=1)
    assert caught.value.code == "catalog_timeout"
    assert catalog.status()["version"] == previous.version


async def test_total_refresh_deadline_includes_lock_wait_and_preserves_snapshot(tmp_path):
    fake = FakeMCP()
    catalog = configured(tmp_path, fake, limits=CatalogLimits(refresh_timeout_seconds=0.1))
    previous = await catalog.refresh()
    original_entry = catalog.get("epic:inspect_actors", version=previous.version)
    request_count = len(fake.requests)
    await catalog._lock.acquire()
    try:
        # Model another refresh holding the lock. The catalog's own deadline must
        # expire before this outer test timeout, even without entering discovery.
        with pytest.raises(JevError) as caught:
            await asyncio.wait_for(catalog.refresh(), timeout=1)
        assert caught.value.code == "catalog_timeout"
        assert len(fake.requests) == request_count
        assert catalog.status()["version"] == previous.version
        assert catalog.status()["last_refresh_error"] == "catalog_timeout"
        assert catalog.get(original_entry.id, version=previous.version) == original_entry
    finally:
        catalog._lock.release()
    # Timing out in the queue neither consumes the lock nor poisons later refreshes.
    assert (await catalog.refresh()).version == previous.version
    assert catalog.status()["last_refresh_error"] is None


async def test_bearer_credentials_only_from_environment_and_never_in_catalog(tmp_path, monkeypatch):
    fake = FakeMCP()
    catalog = configured(
        tmp_path,
        fake,
        servers=[
            {"id": "epic", "url": "http://127.0.0.1:7777/mcp", "bearer_env": "SYNTHETIC_MCP_TOKEN"}
        ],
    )
    monkeypatch.delenv("SYNTHETIC_MCP_TOKEN", raising=False)
    with pytest.raises(JevError) as caught:
        await catalog.refresh()
    assert caught.value.code == "catalog_auth_missing"
    assert fake.requests == []
    monkeypatch.setenv("SYNTHETIC_MCP_TOKEN", "synthetic-private-token")
    snapshot = await catalog.refresh()
    assert all(
        request.headers["authorization"] == "Bearer synthetic-private-token"
        for request in fake.requests
    )
    assert "synthetic-private-token" not in snapshot.model_dump_json()
    assert "synthetic-private-token" not in catalog.get("epic:inspect_actors").model_dump_json()


async def test_sdk_error_logs_do_not_dump_server_payload(tmp_path, caplog):
    fake = FakeMCP()
    catalog = configured(tmp_path, fake)

    async def malformed(request):
        return httpx.Response(200, json={"jsonrpc": "PRIVATE_METADATA_SENTINEL"})

    fake.failure = malformed
    with pytest.raises(JevError):
        await catalog.refresh()
    assert "PRIVATE_METADATA_SENTINEL" not in caplog.text


async def test_optional_jev_rerank_shortlist_never_executes(tmp_path):
    fake = FakeMCP()
    catalog = configured(tmp_path, fake)
    snapshot = await catalog.refresh()
    decision = AsyncMock()
    decision.decide.return_value = {
        "answers": {
            "route": {
                "choice": "epic:inspect_actors",
                "confidence": 0.99,
                "probabilities": {"epic:inspect_actors": 0.99, "__defer__": 0.01},
            }
        }
    }
    count = len(fake.requests)
    result = await catalog.rerank(
        decision, "Inspect actors", ["epic:inspect_actors"], snapshot.version
    )
    assert result["selected"] == "epic:inspect_actors"
    assert result["executed"] is False
    assert result["catalog_version"] == snapshot.version
    assert len(fake.requests) == count
    assert "input_schema" not in str(decision.decide.call_args)


async def test_rerank_rejects_unknown_duplicate_and_stale_before_provider(tmp_path):
    catalog = configured(tmp_path, FakeMCP())
    await catalog.refresh()
    decision = AsyncMock()
    for ids in ([], ["unknown"], ["epic:inspect_actors"] * 2):
        with pytest.raises(JevError):
            await catalog.rerank(decision, "Inspect actors", ids)
    with pytest.raises(JevError):
        await catalog.rerank(decision, "Inspect actors", ["epic:inspect_actors"], "outdated")
    decision.decide.assert_not_called()


async def test_refresh_during_rerank_rejects_outdated_result(tmp_path):
    fake = FakeMCP()
    catalog = configured(tmp_path, fake)
    await catalog.refresh()
    decision = AsyncMock()

    async def change_catalog(*args):
        fake.pages[None]["tools"][0]["description"] = "New metadata"
        await catalog.refresh()
        return {"answers": {"route": {"choice": "epic:inspect_actors"}}}

    decision.decide.side_effect = change_catalog
    with pytest.raises(JevError) as caught:
        await catalog.rerank(decision, "Inspect actors", ["epic:inspect_actors"])
    assert caught.value.code == "catalog_stale"


@pytest.mark.parametrize(
    "query,limit", [("", 8), (" " * 20, 8), ("x" * 2001, 8), ("x", 0), ("x", True)]
)
def test_invalid_search_input(query, limit):
    with pytest.raises(JevError):
        ToolCatalog().search(query, limit=limit)


async def test_smoke_script_uses_real_discovery_api_without_exposing_metadata(
    tmp_path, monkeypatch
):
    fake = FakeMCP()
    catalog = configured(tmp_path, fake)
    monkeypatch.setattr(smoke_catalog, "ToolCatalog", lambda path: catalog)
    report = await smoke_catalog.run(str(catalog.config_path))
    assert report["ok"] is True
    assert report["provider_requests"] == report["external_tool_calls"] == 0
    assert report["schema_retrievals"] == 1
    assert report["stale_version_rejected"] is True
    assert report["unchanged_between_refreshes"] is True
    assert "127.0.0.1" not in str(report)
    assert "inspect_actors" not in str(report)
    assert "tools/call" not in fake.methods


async def test_smoke_requires_configuration_and_search_hits(tmp_path, monkeypatch):
    with pytest.raises(JevError) as caught:
        await smoke_catalog.run("")
    assert caught.value.code == "catalog_config_missing"
    catalog = configured(tmp_path, FakeMCP())
    monkeypatch.setattr(smoke_catalog, "ToolCatalog", lambda path: catalog)
    with pytest.raises(JevError) as caught:
        await smoke_catalog.run(str(catalog.config_path), "no_matching_capability")
    assert caught.value.code == "catalog_query_empty"


@pytest.fixture
def isolated_sse_shutdown_state(request):
    """Give each synthetic server the shutdown state of a fresh server process.

    sse-starlette's Uvicorn watcher sets a process-global should_exit flag. It may
    catch the previous fixture shutting down just before pytest closes its loop;
    the next loop then inherits that flag and terminates every SSE response.
    Match the upstream test lifecycle reset, without changing client deadlines:
    https://github.com/sysid/sse-starlette/blob/main/tests/conftest.py
    """
    from sse_starlette.sse import AppStatus, _thread_state

    if request.param:
        # Deterministically cover the state left by a previous server's shutdown.
        AppStatus.should_exit = True
    AppStatus.should_exit = False
    AppStatus.enable_automatic_graceful_drain = True
    if hasattr(_thread_state, "shutdown_state"):
        del _thread_state.shutdown_state
    try:
        yield
    finally:
        # Let watchers stop; pytest also closes this test's event loop. Clear its
        # thread-local event references before another server creates a new loop.
        AppStatus.should_exit = True
        AppStatus.enable_automatic_graceful_drain = True
        if hasattr(_thread_state, "shutdown_state"):
            del _thread_state.shutdown_state


@pytest.mark.parametrize(
    "isolated_sse_shutdown_state",
    [False, True],
    indirect=True,
    ids=["fresh-state", "previous-server-shutdown"],
)
@pytest.mark.parametrize("json_response", [True, False], ids=["http-json", "http-sse"])
async def test_real_loopback_metadata_server_smoke_and_protocol_cleanup(
    tmp_path, monkeypatch, json_response, isolated_sse_shutdown_state
):
    """Real sockets and SDK server, synthetic metadata only; no tools/call handler exists."""
    metadata_server = Server("synthetic-metadata-only")
    method_log = []
    http_methods = []

    @metadata_server.list_tools()
    async def list_metadata(request: types.ListToolsRequest) -> types.ListToolsResult:
        cursor = request.params.cursor if request.params else None
        start = 100 if cursor == "page-two" else 0
        return types.ListToolsResult(
            tools=[
                types.Tool.model_validate(tool(f"inspect_actor_{index}"))
                for index in range(start, start + 100)
            ],
            nextCursor="page-two" if cursor is None else None,
        )

    assert types.CallToolRequest not in metadata_server.request_handlers
    manager = StreamableHTTPSessionManager(
        metadata_server,
        json_response=json_response,
        stateless=False,
        max_request_body_size=65536,
        max_sessions=2,
        security_settings=TransportSecuritySettings(allowed_hosts=["127.0.0.1:*"]),
    )

    class MetadataEndpoint:
        async def __call__(self, scope, receive, send):
            if dict(scope["headers"]).get(b"authorization") != b"Bearer synthetic-socket-token":
                await Response(status_code=401)(scope, receive, send)
                return
            http_methods.append(scope["method"])
            body = bytearray()

            async def tracked_receive():
                message = await receive()
                if scope["method"] == "POST" and message["type"] == "http.request":
                    body.extend(message.get("body", b""))
                    if not message.get("more_body", False):
                        method_log.append(json.loads(body).get("method"))
                return message

            await manager.handle_request(scope, tracked_receive, send)

    app = Starlette(routes=[Route("/mcp", MetadataEndpoint())], lifespan=lambda app: manager.run())
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.bind(("127.0.0.1", 0))
    listener.listen(8)
    port = listener.getsockname()[1]
    server = uvicorn.Server(uvicorn.Config(app, log_level="error", access_log=False))
    task = asyncio.create_task(server.serve(sockets=[listener]))
    try:
        async with asyncio.timeout(5):
            while not server.started:
                if task.done():
                    await task
                await asyncio.sleep(0.01)
        monkeypatch.setenv("SYNTHETIC_SOCKET_MCP_TOKEN", "synthetic-socket-token")
        path = tmp_path / "socket-catalog.json"
        path.write_text(
            json.dumps(
                {
                    "servers": [
                        {
                            "id": "socket_fixture",
                            "url": f"http://127.0.0.1:{port}/mcp",
                            "bearer_env": "SYNTHETIC_SOCKET_MCP_TOKEN",
                        }
                    ]
                }
            )
        )
        report = await asyncio.wait_for(smoke_catalog.run(str(path), "actor"), timeout=10)
        assert report["ok"] is True
        assert report["tool_count"] == report["entry_count"] == 200
        assert report["schema_retrievals"] == 5
        assert report["unchanged_between_refreshes"] is True
        assert report["stale_version_rejected"] is True
        assert {"GET", "POST", "DELETE"}.issubset(set(http_methods))
        assert method_log.count("tools/list") == 4
        assert set(method_log) == {"initialize", "notifications/initialized", "tools/list"}
    finally:
        server.should_exit = True
        try:
            await asyncio.wait_for(task, timeout=5)
        finally:
            listener.close()
