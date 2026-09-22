"""Bounded, read-only discovery of explicitly configured local MCP tools.

Discovery never invokes a tool. Advertised descriptions, schemas and annotations
are untrusted metadata, not permissions or proof of editor/project identity.
"""

from __future__ import annotations

import asyncio
import contextvars
import hashlib
import json
import logging
import math
import os
import re
from collections import Counter
from collections.abc import Callable
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit

import httpx
from mcp import ClientSession, types
from mcp.client.streamable_http import streamable_http_client
from mcp.shared.exceptions import McpError
from pydantic import BaseModel, ConfigDict, Field, ValidationError, field_validator

from .decision import DecisionClient
from .errors import JevError
from .workflows import Candidate, route

_DISCOVERING = contextvars.ContextVar("jev_catalog_discovering", default=False)


class _MetadataLogFilter(logging.Filter):
    """Prevent SDK parsing exceptions/debug logs from dumping server metadata."""

    def filter(self, record: logging.LogRecord) -> bool:
        if _DISCOVERING.get():
            record.msg = "External MCP discovery diagnostic (payload withheld)."
            record.args = ()
            record.exc_info = None
            record.exc_text = None
            record.stack_info = None
        return True


for _logger_name in ("mcp.client.streamable_http", "mcp.client.session", "mcp.shared.session"):
    logging.getLogger(_logger_name).addFilter(_MetadataLogFilter())


class CatalogLimits(BaseModel):
    model_config = ConfigDict(extra="forbid", frozen=True, strict=True)
    max_config_bytes: int = Field(default=65536, ge=128, le=65536)
    max_servers: int = Field(default=8, ge=1, le=8)
    max_pages_per_server: int = Field(default=128, ge=1, le=128)
    max_entries: int = Field(default=8192, ge=1, le=8192)
    max_schema_bytes: int = Field(default=262144, ge=128, le=262144)
    max_response_bytes: int = Field(default=2097152, ge=128, le=2097152)
    max_total_bytes: int = Field(default=16777216, ge=128, le=16777216)
    max_actions_per_tool: int = Field(default=256, ge=0, le=256)
    request_timeout_seconds: float = Field(default=10.0, gt=0, le=30)
    refresh_timeout_seconds: float = Field(default=45.0, gt=0, le=60)


class CatalogServer(BaseModel):
    model_config = ConfigDict(extra="forbid", frozen=True, strict=True)
    id: str = Field(min_length=1, max_length=32, pattern=r"^[A-Za-z0-9_-]+$")
    url: str = Field(min_length=1, max_length=512)
    bearer_env: str | None = Field(default=None, pattern=r"^[A-Za-z_][A-Za-z0-9_]{0,127}$")

    @field_validator("url")
    @classmethod
    def literal_loopback_only(cls, value: str) -> str:
        try:
            parsed = urlsplit(value)
            port = parsed.port
            if (
                parsed.scheme not in {"http", "https"}
                or parsed.hostname not in {"127.0.0.1", "::1"}
                or parsed.username is not None
                or parsed.password is not None
                or parsed.query
                or parsed.fragment
                or port is None
                or not 1 <= port <= 65535
                or any(ord(char) < 33 for char in value)
                or "\\" in value
                or "%" in parsed.netloc
            ):
                raise ValueError
        except ValueError:
            raise ValueError(
                "Use an explicit loopback HTTP(S) URL and port without credentials."
            ) from None
        return value


class _CatalogConfig(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)
    servers: list[CatalogServer] = Field(default_factory=list, max_length=8)


class CatalogEntry(BaseModel):
    model_config = ConfigDict(extra="forbid", frozen=True)
    id: str
    server_id: str
    name: str
    description: str
    input_schema: dict[str, Any]
    output_schema: dict[str, Any] | None = None
    annotations: dict[str, Any] = Field(default_factory=dict)
    # A preset describes an enum/const action; the schema remains the EXACT tool schema.
    argument_preset: dict[str, str] = Field(default_factory=dict)


class CatalogHit(BaseModel):
    model_config = ConfigDict(extra="forbid", frozen=True)
    id: str
    server_id: str
    name: str
    description: str
    score: float
    argument_preset: dict[str, str]
    catalog_version: str


class CatalogSnapshot(BaseModel):
    model_config = ConfigDict(extra="forbid", frozen=True)
    version: str
    refreshed_at: str | None
    server_ids: tuple[str, ...]
    tool_count: int
    entry_count: int
    warnings: tuple[str, ...] = ()


def _json_bytes(value: Any) -> bytes:
    return json.dumps(
        value, sort_keys=True, ensure_ascii=False, separators=(",", ":"), allow_nan=False
    ).encode("utf-8")


def _budget_error() -> JevError:
    return JevError("catalog_budget_exceeded", "Discovery exceeded its configured metadata budget.")


class _ByteBudget:
    def __init__(self, limit: int):
        self.limit = limit
        self.used = 0
        self.failure: JevError | None = None

    def add(self, count: int) -> None:
        self.used += count
        if self.used > self.limit:
            self.failure = _budget_error()
            raise self.failure


class _BoundedStream(httpx.AsyncByteStream):
    def __init__(self, stream: httpx.AsyncByteStream, limit: int, total: _ByteBudget):
        self.stream, self.limit, self.total = stream, limit, total

    async def __aiter__(self):
        received = 0
        async for chunk in self.stream:
            received += len(chunk)
            self.total.add(len(chunk))
            if received > self.limit:
                self.total.failure = _budget_error()
                raise self.total.failure
            yield chunk

    async def aclose(self):
        await self.stream.aclose()


class _BoundedTransport(httpx.AsyncBaseTransport):
    def __init__(
        self,
        wrapped: httpx.AsyncBaseTransport,
        limits: CatalogLimits,
        total: _ByteBudget,
        endpoint: str,
    ):
        self.wrapped, self.limits, self.total = wrapped, limits, total
        self.endpoint = httpx.URL(endpoint)

    async def handle_async_request(self, request: httpx.Request) -> httpx.Response:
        # Enforce discovery-only operations at the final outbound boundary as well
        # as in the client API. Session cleanup and protocol error responses are allowed.
        if request.url != self.endpoint or request.method not in {"GET", "POST", "DELETE"}:
            raise JevError("catalog_protocol_error", "An unexpected discovery request was refused.")
        if request.method == "POST":
            if len(request.content) > 65536:
                raise _budget_error()
            payload = json.loads(request.content)
            if not isinstance(payload, dict) or payload.get("method") not in {
                None,
                "initialize",
                "notifications/initialized",
                "tools/list",
                "ping",
                "notifications/cancelled",
            }:
                raise JevError("catalog_protocol_error", "Non-discovery MCP requests are refused.")
        response = await self.wrapped.handle_async_request(request)
        try:
            # Refuse compression so wire byte limits also bound decoded JSON/SSE bytes.
            if response.headers.get("content-encoding", "identity").lower() != "identity":
                raise JevError(
                    "catalog_protocol_error", "Compressed discovery responses are refused."
                )
            if 300 <= response.status_code < 400:
                raise JevError("catalog_protocol_error", "Discovery redirects are refused.")
            declared = response.headers.get("content-length")
            if declared is not None and (
                not declared.isdecimal() or int(declared) > self.limits.max_response_bytes
            ):
                raise _budget_error()
            if response.is_stream_consumed:
                self.total.add(len(response.content))
                if len(response.content) > self.limits.max_response_bytes:
                    raise _budget_error()
            else:
                response.stream = _BoundedStream(
                    response.stream, self.limits.max_response_bytes, self.total
                )
            return response
        except BaseException as exc:
            if isinstance(exc, JevError):
                self.total.failure = exc
            await response.aclose()
            raise

    async def aclose(self):
        await self.wrapped.aclose()


def _tokens(value: str) -> list[str]:
    value = re.sub(r"([a-z0-9])([A-Z])", r"\1 \2", value)
    return re.findall(r"[^\W_]+", value.casefold(), flags=re.UNICODE)


def _entry_id(server_id: str, name: str, action: str | None = None) -> str:
    raw = f"{server_id}:{name}" + (f":{action}" if action is not None else "")
    if len(raw) <= 128 and re.fullmatch(r"[A-Za-z0-9_.:-]+", raw):
        return raw
    readable = re.sub(r"[^A-Za-z0-9_.-]", "_", name)[:56]
    return f"{server_id}:{readable}:{hashlib.sha256(raw.encode()).hexdigest()[:24]}"


def _actions(schema: dict, maximum: int) -> list[str]:
    """Only advertised top-level action enums and direct oneOf/anyOf branches."""
    found: set[str] = set()
    nodes = [schema]
    for key in ("oneOf", "anyOf"):
        branches = schema.get(key, [])
        if isinstance(branches, list):
            nodes.extend(branches)
    for node in nodes:
        if not isinstance(node, dict):
            continue
        properties = node.get("properties", {})
        action = properties.get("action", {}) if isinstance(properties, dict) else {}
        if not isinstance(action, dict):
            continue
        values = action.get("enum", [])
        if not isinstance(values, list):
            values = []
        if "const" in action:
            values = [*values, action["const"]]
        for value in values:
            if isinstance(value, str) and 1 <= len(value) <= 128 and value.isprintable():
                found.add(value)
                if len(found) > maximum:
                    raise _budget_error()
    return sorted(found)


class ToolCatalog:
    """In-memory catalog. An explicit refresh is required; searches make no network calls."""

    def __init__(
        self,
        config_path: Path | str | None = None,
        *,
        limits: CatalogLimits | None = None,
        transport_factory: Callable[[], httpx.AsyncBaseTransport] | None = None,
    ):
        self.config_path = Path(config_path) if config_path is not None else None
        self.limits = limits or CatalogLimits()
        self._transport_factory = transport_factory
        self._entries: dict[str, CatalogEntry] = {}
        self._documents: dict[str, Counter] = {}
        self._frequency: Counter = Counter()
        self._average_length = 1.0
        self._snapshot = CatalogSnapshot(
            version=hashlib.sha256(b"[]").hexdigest(),
            refreshed_at=None,
            server_ids=(),
            tool_count=0,
            entry_count=0,
        )
        self._lock = asyncio.Lock()
        self._last_error: str | None = None

    def status(self) -> dict:
        return {
            **self._snapshot.model_dump(mode="json"),
            "configured": self.config_path is not None,
            "last_refresh_error": self._last_error,
            "discovery_only": True,
        }

    def _config(self) -> _CatalogConfig:
        if self.config_path is None:
            return _CatalogConfig()
        try:
            with self.config_path.open("rb") as stream:
                data = stream.read(self.limits.max_config_bytes + 1)
            if len(data) > self.limits.max_config_bytes:
                raise _budget_error()
            config = _CatalogConfig.model_validate_json(data)
            if len(config.servers) > self.limits.max_servers:
                raise _budget_error()
            if len({item.id for item in config.servers}) != len(config.servers):
                raise ValueError
            return config
        except JevError:
            raise
        except (OSError, ValueError, ValidationError, RecursionError):
            raise JevError(
                "catalog_config_error",
                "Invalid tool catalog configuration; no discovery was started.",
            ) from None

    async def _server_tools(self, server: CatalogServer, total: _ByteBudget) -> list[types.Tool]:
        headers = {"Accept-Encoding": "identity"}
        if server.bearer_env:
            token = os.environ.get(server.bearer_env, "")
            if (
                not token
                or len(token) > 4096
                or not token.isascii()
                or any(ord(char) < 33 or ord(char) > 126 for char in token)
            ):
                raise JevError(
                    "catalog_auth_missing", "A configured discovery credential is unavailable."
                )
            headers["Authorization"] = f"Bearer {token}"
        wrapped = (
            self._transport_factory()
            if self._transport_factory
            else httpx.AsyncHTTPTransport(retries=0, trust_env=False)
        )
        transport = _BoundedTransport(wrapped, self.limits, total, server.url)
        async with httpx.AsyncClient(
            transport=transport,
            headers=headers,
            timeout=self.limits.request_timeout_seconds,
            follow_redirects=False,
            trust_env=False,
        ) as client:
            async with streamable_http_client(server.url, http_client=client) as streams:
                async with ClientSession(
                    streams[0],
                    streams[1],
                    read_timeout_seconds=timedelta(seconds=self.limits.request_timeout_seconds),
                ) as session:
                    initialized = await session.initialize()
                    if initialized.capabilities.tools is None:
                        raise JevError(
                            "catalog_protocol_error", "A server does not advertise tools."
                        )
                    items: list[types.Tool] = []
                    cursors: set[str] = set()
                    names: set[str] = set()
                    cursor = None
                    for _ in range(self.limits.max_pages_per_server):
                        page = await session.list_tools(
                            params=types.PaginatedRequestParams(cursor=cursor) if cursor else None
                        )
                        # SDK parsing is preceded by the HTTP byte limit, then semantic budgets.
                        if (
                            len(_json_bytes(page.model_dump(mode="json")))
                            > self.limits.max_response_bytes
                        ):
                            raise _budget_error()
                        for tool in page.tools:
                            if tool.name in names:
                                raise JevError(
                                    "catalog_protocol_error", "A server repeated a tool name."
                                )
                            names.add(tool.name)
                            items.append(tool)
                            if len(items) > self.limits.max_entries:
                                raise _budget_error()
                        cursor = page.nextCursor
                        if cursor is None:
                            return items
                        if not cursor or len(cursor) > 4096 or cursor in cursors:
                            raise JevError(
                                "catalog_protocol_error", "Invalid or repeated pagination cursor."
                            )
                        cursors.add(cursor)
                    raise _budget_error()

    def _tool_entries(self, server_id: str, tool: types.Tool) -> list[CatalogEntry]:
        if not tool.name or len(tool.name) > 256 or not tool.name.isprintable():
            raise JevError("catalog_protocol_error", "A tool has an invalid name.")
        description = tool.description or tool.title or tool.name
        if len(description) > 16384:
            raise _budget_error()
        for schema in (tool.inputSchema, tool.outputSchema):
            if schema is not None and len(_json_bytes(schema)) > self.limits.max_schema_bytes:
                raise _budget_error()
        entry = CatalogEntry(
            id=_entry_id(server_id, tool.name),
            server_id=server_id,
            name=tool.name,
            description=description,
            input_schema=tool.inputSchema,
            output_schema=tool.outputSchema,
            annotations=tool.annotations.model_dump(exclude_none=True) if tool.annotations else {},
        )
        entries = [entry]
        for action in _actions(tool.inputSchema, self.limits.max_actions_per_tool):
            entries.append(
                entry.model_copy(
                    update={
                        "id": _entry_id(server_id, tool.name, action),
                        "description": f"Action {action}: {description}",
                        "argument_preset": {"action": action},
                    }
                )
            )
        return entries

    async def refresh(self) -> CatalogSnapshot:
        """Refresh all servers sequentially; any failure preserves the previous snapshot."""
        marker = _DISCOVERING.set(True)
        total = _ByteBudget(self.limits.max_total_bytes)
        try:
            # The total deadline includes waiting for another refresh to finish.
            async with asyncio.timeout(self.limits.refresh_timeout_seconds):
                async with self._lock:
                    config = await asyncio.to_thread(self._config)
                    entries: dict[str, CatalogEntry] = {}
                    canonical_tools: dict[str, dict] = {}
                    warnings: list[str] = []
                    metadata_bytes = 0
                    tool_count = 0
                    for server in config.servers:
                        tools = await self._server_tools(server, total)
                        names = {tool.name for tool in tools}
                        if names == {"list_toolsets", "describe_toolset", "call_tool"}:
                            warnings.append(
                                f"{server.id}: tools/list exposes only the Epic search gateway. "
                                "Disable bEnableToolSearch there to expose its full catalog."
                            )
                        tool_count += len(tools)
                        for tool in tools:
                            base_entries = self._tool_entries(server.id, tool)
                            base = base_entries[0]
                            serialized = base.model_dump()
                            canonical_tools[base.id] = serialized
                            # Virtual action entries share the exact base schema in memory.
                            metadata_bytes += len(_json_bytes(serialized))
                            for entry in base_entries:
                                if entry.id in entries:
                                    raise JevError(
                                        "catalog_protocol_error", "A catalog ID collided."
                                    )
                                entries[entry.id] = entry
                                metadata_bytes += len(
                                    _json_bytes(
                                        {
                                            "id": entry.id,
                                            "description": entry.description,
                                            "argument_preset": entry.argument_preset,
                                        }
                                    )
                                )
                                if (
                                    len(entries) > self.limits.max_entries
                                    or metadata_bytes > self.limits.max_total_bytes
                                ):
                                    raise _budget_error()
                    ordered = sorted(entries)
                    version = hashlib.sha256(
                        _json_bytes(
                            {
                                "servers": sorted(
                                    (server.id, server.url) for server in config.servers
                                ),
                                "tools": [canonical_tools[key] for key in sorted(canonical_tools)],
                            }
                        )
                    ).hexdigest()
                    documents: dict[str, Counter] = {}
                    indexed_bytes = 0
                    for key in ordered:
                        entry = entries[key]
                        document = " ".join(
                            (
                                entry.name,
                                entry.name,
                                " ".join(entry.argument_preset.values()),
                                entry.description,
                                " ".join(entry.input_schema.get("properties", {})),
                            )
                        )
                        indexed_bytes += len(document.encode("utf-8"))
                        if indexed_bytes > self.limits.max_total_bytes:
                            raise _budget_error()
                        documents[key] = Counter(_tokens(document))
                    frequency: Counter = Counter()
                    for document in documents.values():
                        frequency.update(document.keys())
                    snapshot = CatalogSnapshot(
                        version=version,
                        refreshed_at=datetime.now(UTC).isoformat(),
                        server_ids=tuple(sorted(server.id for server in config.servers)),
                        tool_count=tool_count,
                        entry_count=len(entries),
                        warnings=tuple(warnings),
                    )
                    # No awaits below: readers observe one complete version in this event loop.
                    self._entries, self._documents, self._frequency = entries, documents, frequency
                    self._average_length = (
                        sum(sum(doc.values()) for doc in documents.values()) / len(documents)
                        if documents
                        else 1.0
                    )
                    self._snapshot = snapshot
                    self._last_error = None
                    return snapshot
        except Exception as exc:
            error = total.failure or self._public_error(exc)
            self._last_error = error.code
            raise error from None
        finally:
            _DISCOVERING.reset(marker)

    @staticmethod
    def _public_error(exc: Exception) -> JevError:
        if isinstance(exc, JevError):
            return exc
        if isinstance(exc, BaseExceptionGroup):
            for nested in exc.exceptions:
                if isinstance(nested, Exception):
                    candidate = ToolCatalog._public_error(nested)
                    if candidate.code != "catalog_unavailable":
                        return candidate
        if isinstance(exc, TimeoutError | httpx.TimeoutException) or (
            isinstance(exc, McpError) and exc.error.code == httpx.codes.REQUEST_TIMEOUT
        ):
            return JevError(
                "catalog_timeout", "Tool discovery timed out; the prior catalog is retained."
            )
        return JevError(
            "catalog_unavailable", "Tool discovery failed; the prior catalog is retained."
        )

    def _require_version(self, version: str | None) -> None:
        if version is not None and version != self._snapshot.version:
            raise JevError(
                "catalog_stale", "The catalog changed; search again before retrieving a schema."
            )

    def get(self, tool_id: str, version: str | None = None) -> CatalogEntry:
        self._require_version(version)
        entry = self._entries.get(tool_id)
        if entry is None:
            raise JevError("catalog_not_found", "No catalog entry has that ID.")
        # Frozen Pydantic models do not recursively freeze dictionaries.
        return entry.model_copy(deep=True)

    def search(self, query: str, limit: int = 8, server_id: str | None = None) -> list[CatalogHit]:
        if not isinstance(query, str) or not 1 <= len(query.strip()) <= 2000:
            raise JevError("invalid_request", "Provide a search query of 1 to 2000 characters.")
        if type(limit) is not int or not 1 <= limit <= 32:
            raise JevError("invalid_request", "Search limit must be between 1 and 32.")
        terms = set(_tokens(query))
        document_count = len(self._documents)
        scores: list[tuple[float, str]] = []
        for key, document in self._documents.items():
            entry = self._entries[key]
            if server_id is not None and server_id != entry.server_id:
                continue
            length = sum(document.values())
            score = 0.0
            for term in terms:
                count = document[term]
                if count:
                    inverse = math.log1p(
                        (document_count - self._frequency[term] + 0.5)
                        / (self._frequency[term] + 0.5)
                    )
                    denominator = count + 1.2 * (0.25 + 0.75 * length / self._average_length)
                    score += inverse * count * 2.2 / denominator
            exact = query.strip().casefold()
            if exact in {
                entry.id.casefold(),
                entry.name.casefold(),
                *[item.casefold() for item in entry.argument_preset.values()],
            }:
                score += 1000
            if score > 0:
                scores.append((score, key))
        scores.sort(key=lambda item: (-item[0], item[1]))
        return [
            CatalogHit(
                id=key,
                server_id=self._entries[key].server_id,
                name=self._entries[key].name,
                description=self._entries[key].description[:1000],
                score=round(score, 6),
                argument_preset=dict(self._entries[key].argument_preset),
                catalog_version=self._snapshot.version,
            )
            for score, key in scores[:limit]
        ]

    async def rerank(
        self,
        client: DecisionClient,
        goal: str,
        tool_ids: list[str],
        version: str | None = None,
    ) -> dict:
        """One optional provider request using descriptions of caller-selected entries only."""
        self._require_version(version)
        if not isinstance(goal, str) or not 1 <= len(goal.strip()) <= 4000:
            raise JevError("invalid_request", "Provide a routing goal of 1 to 4000 characters.")
        if not 1 <= len(tool_ids) <= 16 or len(set(tool_ids)) != len(tool_ids):
            raise JevError("invalid_request", "Provide 1 to 16 unique catalog IDs.")
        candidates = [
            Candidate(id=entry.id, description=entry.description[:2000])
            for entry in (self.get(key) for key in tool_ids)
        ]
        selected_version = self._snapshot.version
        result = await route(client, goal, candidates)
        # A refresh can occur while the provider is working; don't return an outdated winner.
        self._require_version(selected_version)
        return {**result, "catalog_version": selected_version, "executed": False}
