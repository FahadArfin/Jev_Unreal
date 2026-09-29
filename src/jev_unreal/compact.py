"""Bounded projections, frozen pages and change-only reads of native inspections."""

import asyncio
import copy
import hashlib
import json
import time
import uuid
from dataclasses import dataclass
from typing import Any, Literal

from mcp.server.fastmcp import FastMCP
from mcp.types import ToolAnnotations
from pydantic import BaseModel, ConfigDict, Field, model_validator

from .bridge import UnrealBridge
from .errors import JevError

Source = Literal["context", "actors", "actor_details", "assets", "asset_details"]
ACTOR_FIELDS = {
    "label",
    "class",
    "folder",
    "location",
    "rotation",
    "scale",
    "static_mesh_path",
    "bounds_cm",
    "materials",
    "material_slot_count",
    "material_override_count",
    "mesh_settings",
    "collision_enabled",
    "attachment_parent_path",
    "attachment_parent_instance_id",
    "attachment_relative_transform",
    "attachment_socket",
}
ASSET_FIELDS = {
    "name",
    "class",
    "loaded",
    "static_mesh.bounds_cm",
    "static_mesh.lods",
    "static_mesh.lod_count",
    "static_mesh.material_slots",
    "static_mesh.material_slot_count",
    "static_mesh.collision",
}
IDENTITY = ("project_file", "session_id", "world_path", "revision")
MANDATORY = {"path", "instance_id", "editable", "edit_blockers", "bounds_available"}


class ReadRequest(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)
    source: Source
    query: str = Field(default="", max_length=200)
    path: str | None = Field(default=None, min_length=1, max_length=512)
    actor_paths: list[str] = Field(default_factory=list, max_length=20)
    fields: list[str] | None = Field(default=None, min_length=1, max_length=24)
    page_size: int = Field(default=20, ge=1, le=100)
    cursor: str | None = Field(default=None, min_length=1, max_length=80)
    since: str | None = Field(default=None, min_length=1, max_length=64)

    @model_validator(mode="after")
    def valid_scope(self):
        if self.cursor and self.since:
            raise ValueError("Use cursor or since, not both.")
        if self.source == "actor_details":
            if not self.actor_paths or len(set(self.actor_paths)) != len(self.actor_paths):
                raise ValueError("Supply 1–20 unique actor paths for actor_details.")
        elif self.actor_paths:
            raise ValueError("actor_paths only applies to actor_details.")
        if any(
            not p.strip() or len(p) > 1024 or any(ord(c) < 32 for c in p) for p in self.actor_paths
        ):
            raise ValueError("Invalid actor path.")
        if self.source in {"actor_details", "asset_details"} and self.query:
            raise ValueError("Exact details do not accept query.")
        if self.source == "asset_details" and self.path is None:
            raise ValueError("An exact asset path is required.")
        if self.path is not None and self.source not in {"assets", "asset_details"}:
            raise ValueError("path only applies to asset inspection.")
        if self.source == "assets" and self.path is not None and len(self.path) > 200:
            raise ValueError("Asset search path exceeds 200 characters.")
        allowed = ASSET_FIELDS if self.source.startswith("asset") else ACTOR_FIELDS
        if self.fields is not None and (
            len(set(self.fields)) != len(self.fields) or not set(self.fields) <= allowed | MANDATORY
        ):
            raise ValueError("Select unique documented fields for this source.")
        return self

    def selected_fields(self) -> list[str]:
        if self.fields is not None:
            return sorted(self.fields)
        if self.source.startswith("asset"):
            return (
                [
                    "name",
                    "class",
                    "static_mesh.bounds_cm",
                    "static_mesh.lod_count",
                    "static_mesh.material_slot_count",
                ]
                if self.source == "asset_details"
                else ["name", "class"]
            )
        return ["label", "class", "location", "static_mesh_path", "bounds_cm"]

    def signature(self) -> str:
        scope = self.model_dump(exclude={"cursor", "since"})
        scope["fields"] = self.selected_fields()
        return hashlib.sha256(json.dumps(scope, sort_keys=True).encode()).hexdigest()


def _safety(data: dict) -> dict:
    """Retain blockers/availability/truncation even when callers request fewer fields."""
    result = {}
    for key, value in data.items():
        if (
            key in MANDATORY
            or key in {"registry_loading", "warnings", "scan_incomplete"}
            or (key.endswith(("_truncated", "_available", "_incomplete")) or key == "truncated")
        ):
            result[key] = copy.deepcopy(value)
        elif isinstance(value, dict):
            nested = _safety(value)
            if nested:
                result[key] = nested
    return result


def _project(row: dict, fields: list[str]) -> dict:
    result, unavailable = _safety(row), []
    for name in fields:
        parts = name.split(".")
        source, target = row, result
        for part in parts[:-1]:
            if not isinstance(source.get(part), dict):
                break
            source = source[part]
            target = target.setdefault(part, {})
        else:
            if parts[-1] in source:
                target[parts[-1]] = copy.deepcopy(source[parts[-1]])
                continue
        unavailable.append(name)
    if unavailable:
        result["unavailable_fields"] = unavailable
    return result


@dataclass
class _Read:
    expires: float
    signature: str
    identity: dict
    metadata: dict
    rows: list[dict]
    output_rows: list[dict]
    delta: dict | None
    byte_count: int


class CompactReads:
    def __init__(self, bridge: UnrealBridge, *, clock=time.monotonic):
        self.bridge, self.clock = bridge, clock
        self._reads: dict[str, _Read] = {}
        self._lock = asyncio.Lock()

    def _get(self, key: str) -> _Read:
        for stale in [k for k, v in self._reads.items() if v.expires <= self.clock()]:
            del self._reads[stale]
        if key not in self._reads:
            raise JevError(
                "read_expired", "Read expired, was evicted, or belongs to another server."
            )
        return self._reads[key]

    @staticmethod
    def _identity(status: dict) -> dict:
        if any(not isinstance(status.get(k), str) or not status[k] for k in IDENTITY):
            raise JevError("bridge_error", "Inspection lacks complete editor identity.")
        return {k: status[k] for k in IDENTITY}

    async def read(self, request: ReadRequest) -> dict:
        async with self._lock:
            return await self._read(request)

    async def _native_capture(self, request: ReadRequest, identity: dict) -> tuple[list, dict]:
        params = request.model_dump(exclude={"cursor", "since", "page_size"}, exclude_none=True)
        params.update(fields=request.selected_fields(), page_size=100)
        rows, cursor, seen = [], None, set()
        metadata, captured_count, read_id = None, None, None
        # A full bounded projection is retained for honest deltas. Native transport
        # pages never contain unselected mesh settings/material/LOD arrays.
        for _ in range(2):
            if cursor:
                params["cursor"] = cursor
            page = await self.bridge.call("compact_read", params)
            items = page.get("items")
            if (
                page.get("identity") != identity
                or not isinstance(items, list)
                or len(items) > 100
                or page.get("returned_count") != len(items)
                or type(page.get("captured_count")) is not int
                or not 0 <= page["captured_count"] <= 200
                or not isinstance(page.get("metadata"), dict)
                or not isinstance(page.get("native_read_id"), str)
                or not page["native_read_id"]
            ):
                raise JevError("bridge_error", "Invalid native compact page or identity.")
            if metadata is None:
                metadata = page["metadata"]
                captured_count = page["captured_count"]
                read_id = page["native_read_id"]
            elif (metadata != page["metadata"] or captured_count != page["captured_count"]
                  or read_id != page["native_read_id"]):
                raise JevError("bridge_error", "Native compact pages have inconsistent snapshots.")
            rows.extend(items)
            cursor = page.get("next_cursor")
            if cursor is None:
                if len(rows) != captured_count:
                    raise JevError("bridge_error", "Native compact capture ended prematurely.")
                return rows, {**metadata, "projection_location": "native",
                              "native_transport_pages": len(seen) + 1}
            if (not isinstance(cursor, str) or not 1 <= len(cursor) <= 80
                    or cursor in seen or not items):
                raise JevError("bridge_error", "Invalid native compact continuation.")
            seen.add(cursor)
        raise JevError("bridge_error", "Native compact pagination exceeded its bounded capture.")

    async def _read(self, request: ReadRequest) -> dict:
        if request.cursor:
            key, separator, offset = request.cursor.partition(":")
            if not separator or not offset.isascii() or not offset.isdecimal():
                raise JevError("invalid_cursor", "Use the returned continuation cursor.")
            cached = self._get(key)
            index = int(offset)
            if cached.signature != request.signature() or (
                not 0 < index < len(cached.output_rows) or index % request.page_size
            ):
                raise JevError("invalid_cursor", "Cursor does not match this request/page.")
            current = self._identity(await self.bridge.call("status"))
            if current != cached.identity:
                raise JevError(
                    "stale_cursor", "Editor identity/revision changed; start a fresh read."
                )
            return self._page(key, cached, request, index)

        previous = self._get(request.since) if request.since else None
        if previous and previous.signature != request.signature():
            raise JevError(
                "read_scope_changed", "Delta requires the same source, fields and scope."
            )
        status = await self.bridge.call("status")
        before = self._identity(status)
        if previous and any(previous.identity[k] != before[k] for k in IDENTITY[:-1]):
            raise JevError(
                "read_identity_changed", "Delta belongs to another project/session/world."
            )
        params = {"query": request.query, "limit": 200}
        if request.source == "assets":
            params["path"] = request.path or "/Game"
        elif request.source == "asset_details":
            params = {"path": request.path}
        elif request.source == "actor_details":
            params = {"actor_paths": request.actor_paths}
        native = "compact_read" in status.get("capabilities", [])
        if native:
            rows, metadata = await self._native_capture(request, before)
            raw = {}
        else:
            raw = await self.bridge.call(request.source, params)
        after = self._identity(await self.bridge.call("status"))
        if before != after or any(k in raw and raw[k] != after[k] for k in IDENTITY):
            raise JevError(
                "read_state_changed", "Scene changed during inspection; no baseline stored."
            )
        collection = "assets" if request.source == "assets" else "actors"
        if not native:
            rows = [raw] if request.source == "asset_details" else raw.get(collection)
        if (
            not isinstance(rows, list)
            or len(rows) > 200
            or any(
                not isinstance(row, dict) or not isinstance(row.get("path"), str) or not row["path"]
                for row in rows
            )
            or len({row["path"] for row in rows}) != len(rows)
        ):
            raise JevError(
                "bridge_error", "Inspection returned invalid or duplicate row identities."
            )
        if not native:
            metadata = (
                {}
                if request.source == "asset_details"
                else {k: v for k, v in raw.items() if k != collection and k not in IDENTITY}
            )
            metadata.update(_safety(raw) if request.source == "asset_details" else {})
            metadata["projection_location"] = "mcp_legacy_fallback"
        projected = [_project(row, request.selected_fields()) for row in rows]
        output, delta = projected, None
        if previous:
            old = {r["path"]: r for r in previous.rows}
            current = {r["path"]: r for r in projected}
            output = [r for r in projected if old.get(r["path"]) != r]
            delta = {
                "baseline_id": request.since,
                "added_paths": sorted(current.keys() - old.keys()),
                "changed_paths": sorted(
                    k for k in old.keys() & current.keys() if old[k] != current[k]
                ),
                "removed_from_result": sorted(old.keys() - current.keys()),
                "metadata_changed": previous.metadata != metadata,
                "absence_proves_deletion": False,
                "scope": "Selected fields in bounded results; omitted fields were not compared.",
            }
        size = len(json.dumps([projected, metadata, delta], ensure_ascii=False).encode("utf-8"))
        if size > 1048576:
            raise JevError(
                "read_too_large", "Select fewer fields/actors; retained read exceeds 1 MiB."
            )
        key = uuid.uuid4().hex
        cached = _Read(
            self.clock() + 120, request.signature(), after, metadata, projected, output, delta, size
        )
        while self._reads and (
            len(self._reads) >= 32
            or sum(r.byte_count for r in self._reads.values()) + size > 8388608
        ):
            del self._reads[next(iter(self._reads))]
        self._reads[key] = cached
        return self._page(key, cached, request, 0)

    def _page(self, key: str, value: _Read, request: ReadRequest, index: int) -> dict:
        end = index + request.page_size
        return copy.deepcopy(
            {
                "read_id": key,
                "source": request.source,
                "identity": value.identity,
                "metadata": value.metadata,
                "fields": request.selected_fields(),
                "items": value.output_rows[index:end],
                "delta": value.delta,
                "captured_count": len(value.rows),
                "returned_count": len(value.output_rows[index:end]),
                "matching_change_count": len(value.output_rows)
                if value.delta is not None
                else None,
                "next_cursor": f"{key}:{end}" if end < len(value.output_rows) else None,
                "expires_in_seconds": max(0, round(value.expires - self.clock(), 2)),
                "limits": {
                    "native_scan_cap": {"actor_details": 20, "asset_details": 1}.get(
                        request.source, 200
                    ),
                    "pages_cover": "Bounded captured result only; inspect native truncation flags.",
                    "freshness": "Actor revision does not fingerprint asset contents or selection.",
                    "purpose": "Inspection only; verify edits with fresh native measurements.",
                },
            }
        )


def register_compact_tools(server: FastMCP, bridge: UnrealBridge) -> None:
    reads = CompactReads(bridge)

    @server.tool(
        annotations=ToolAnnotations(readOnlyHint=True, destructiveHint=False, openWorldHint=False)
    )
    async def unreal_read(request: ReadRequest) -> dict[str, Any]:
        """Read selected fields, frozen pages, or changes since read_id. Local; no edits/cloud.

        Sources: context, actors, actor_details, assets, asset_details. Default compact fields;
        explicit fields use top-level names or static_mesh.bounds_cm/lod_count/etc for assets.
        Identity, blockers and truncation survive projection. Retains 32 reads/8 MiB for 120s.
        Repeat the same request with next_cursor to page; with since=read_id for a fresh delta.
        Uses native compact_read when supported, otherwise legacy projection in the MCP process.
        Native result stays bounded at 200; pages do not reveal beyond a truncated scan.
        """
        try:
            return {"ok": True, "result": await reads.read(request)}
        except JevError as exc:
            return exc.as_dict()
