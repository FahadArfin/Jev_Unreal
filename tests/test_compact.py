import copy
import json
from contextlib import contextmanager
from types import SimpleNamespace

import pytest
from pydantic import ValidationError

from jev_unreal.compact import CompactReads, ReadRequest
from jev_unreal.errors import JevError


@contextmanager
def error_code(code):
    with pytest.raises(JevError) as caught:
        yield
    assert caught.value.code == code


class Bridge:
    def __init__(self):
        self.status = dict(project_file="/P.uproject", session_id="s", world_path="w", revision="r")
        self.rows = [
            dict(path=f"/Actor{i}", instance_id=f"i{i}", label=f"Actor {i}", editable=False,
                 edit_blockers=["locked"], materials_truncated=True, location=[i, 0, 0],
                 mesh_settings={"large_detail": "x" * 1000})
            for i in range(3)
        ]
        self.truncated = True
        self.calls = []
        self.change_during_read = False

    async def call(self, action, params=None):
        self.calls.append((action, params))
        if action == "status":
            return copy.deepcopy(self.status)
        if self.change_during_read:
            self.status["revision"] = "changed"
        if action == "asset_details":
            return {"path": "/Game/M.M", "class": "StaticMesh", "registry_loading": True,
                    "static_mesh": {"lod_count": 2, "lods": [1, 2], "lods_truncated": False,
                                    "material_slots": ["large"] * 30, "materials_truncated": True}}
        return {"actors": copy.deepcopy(self.rows), "revision": self.status["revision"],
                "truncated": self.truncated}


class NativeBridge(Bridge):
    def __init__(self):
        super().__init__()
        self.status["capabilities"] = ["compact_read"]
        self.rows = [{"path": f"/Actor{i}", "label": f"Actor {i}", "editable": False,
                      "edit_blockers": ["locked"]} for i in range(105)]
        self.tamper = None

    async def call(self, action, params=None):
        if action == "status":
            return await super().call(action, params)
        assert action == "compact_read"
        self.calls.append((action, copy.deepcopy(params)))
        offset = 100 if params.get("cursor") else 0
        rows = copy.deepcopy(self.rows[offset:offset + 100])
        page = {"identity": {k: self.status[k] for k in
                              ("project_file", "session_id", "world_path", "revision")},
                "items": rows, "metadata": {"truncated": False},
                "captured_count": len(self.rows), "returned_count": len(rows),
                "native_read_id": "native", "next_cursor": "native:100" if not offset else None}
        if self.tamper:
            self.tamper(page, offset)
        return page


async def test_native_projection_negotiates_once_and_collects_bounded_frozen_pages_for_delta():
    bridge = NativeBridge()
    reads = CompactReads(bridge)
    req = ReadRequest(source="actors", fields=["label"], page_size=2)
    first = await reads.read(req)
    assert first["metadata"]["projection_location"] == "native"
    assert first["metadata"]["native_transport_pages"] == 2
    assert first["captured_count"] == 105 and first["returned_count"] == 2
    native_calls = [p for a, p in bridge.calls if a == "compact_read"]
    assert native_calls[0]["fields"] == ["label"]
    assert native_calls[1]["cursor"] == "native:100"
    assert first["items"][0]["edit_blockers"] == ["locked"]
    bridge.rows[100]["label"] = "Changed beyond first native page"
    delta = await reads.read(req.model_copy(update={"since": first["read_id"]}))
    assert delta["delta"]["changed_paths"] == ["/Actor100"]
    assert delta["items"][0]["label"] == "Changed beyond first native page"


@pytest.mark.parametrize("defect", ["identity", "count", "metadata", "snapshot", "loop"])
async def test_native_page_corruption_fails_closed_without_legacy_retry(defect):
    bridge = NativeBridge()

    def tamper(page, offset):
        if defect == "identity":
            page["identity"]["session_id"] = "other"
        elif defect == "count":
            page["captured_count"] = 106
        elif offset:
            if defect == "metadata":
                page["metadata"]["truncated"] = True
            elif defect == "snapshot":
                page["native_read_id"] = "other"
            else:
                page["next_cursor"] = "native:100"

    bridge.tamper = tamper
    with error_code("bridge_error"):
        await CompactReads(bridge).read(ReadRequest(source="actors", fields=["label"]))
    assert all(action in {"status", "compact_read"} for action, _ in bridge.calls)


async def test_projection_preserves_safety_and_pages_are_frozen():
    bridge = Bridge()
    reads = CompactReads(bridge)
    request = ReadRequest(source="actors", fields=["label"], page_size=2)
    result = await reads.read(request)
    assert len(result["items"]) == 2 and result["captured_count"] == 3
    assert result["metadata"]["truncated"]
    row = result["items"][0]
    assert row["path"] == "/Actor0" and row["instance_id"] == "i0"
    assert row["edit_blockers"] == ["locked"] and row["materials_truncated"]
    assert "mesh_settings" not in row and "location" not in row
    # Caller mutations must not modify the retained baseline/page.
    row["label"] = "tamper"
    page = await reads.read(request.model_copy(update={"cursor": result["next_cursor"]}))
    assert [r["path"] for r in page["items"]] == ["/Actor2"]
    assert page["next_cursor"] is None
    assert [a for a, _ in bridge.calls] == ["status", "actors", "status", "status"]
    delta = await reads.read(request.model_copy(update={"since": result["read_id"]}))
    assert delta["items"] == []


async def test_delta_reports_changed_selected_fields_and_absence_without_deletion_claim():
    bridge = Bridge()
    reads = CompactReads(bridge)
    req = ReadRequest(source="actors", fields=["label"])
    first = await reads.read(req)
    bridge.rows[0]["label"] = "Changed"
    bridge.rows[1]["mesh_settings"] = {"unselected": "not compared"}
    bridge.rows.pop()
    bridge.rows.append({"path": "/New", "label": "Added"})
    bridge.status["revision"] = "new"
    second = await reads.read(req.model_copy(update={"since": first["read_id"]}))
    assert [r["path"] for r in second["items"]] == ["/Actor0", "/New"]
    assert second["delta"]["changed_paths"] == ["/Actor0"]
    assert second["delta"]["removed_from_result"] == ["/Actor2"]
    assert second["delta"]["absence_proves_deletion"] is False


async def test_nested_asset_projection_retains_availability_and_marks_missing_fields():
    result = await CompactReads(Bridge()).read(ReadRequest(
        source="asset_details", path="/Game/M.M", fields=["static_mesh.lod_count", "name"]
    ))
    row = result["items"][0]
    assert row["registry_loading"] and row["static_mesh"]["materials_truncated"]
    assert row["static_mesh"]["lod_count"] == 2
    assert "material_slots" not in row["static_mesh"]
    assert row["unavailable_fields"] == ["name"]


async def test_stale_cursor_scope_session_and_mid_read_drift_are_refused():
    bridge = Bridge()
    reads = CompactReads(bridge)
    req = ReadRequest(source="actors", page_size=1)
    first = await reads.read(req)
    with error_code("invalid_cursor"):
        await reads.read(req.model_copy(update={"cursor": first["next_cursor"], "query": "new"}))
    bridge.status["revision"] = "r2"
    with error_code("stale_cursor"):
        await reads.read(req.model_copy(update={"cursor": first["next_cursor"]}))
    with error_code("read_scope_changed"):
        await reads.read(req.model_copy(update={"since": first["read_id"], "fields": ["label"]}))
    bridge.status["session_id"] = "different"
    with error_code("read_identity_changed"):
        await reads.read(req.model_copy(update={"since": first["read_id"]}))
    bridge.change_during_read = True
    with error_code("read_state_changed"):
        await reads.read(req)


async def test_expiry_eviction_duplicate_rows_and_byte_budget():
    bridge = Bridge()
    now = SimpleNamespace(time=0)
    reads = CompactReads(bridge, clock=lambda: now.time)
    req = ReadRequest(source="actors")
    initial = await reads.read(req)
    now.time = 121
    with error_code("read_expired"):
        await reads.read(req.model_copy(update={"since": initial["read_id"]}))
    retained = await reads.read(req)
    for _ in range(32):
        await reads.read(req)
    assert len(reads._reads) == 32
    with error_code("read_expired"):
        await reads.read(req.model_copy(update={"since": retained["read_id"]}))
    bridge.rows.append(bridge.rows[0])
    with error_code("bridge_error"):
        await reads.read(req)
    bridge.rows = [{"path": "/Huge", "label": "x" * 1048576}]
    with error_code("read_too_large"):
        await reads.read(req)


@pytest.mark.parametrize("params", [
    {"source": "shell"}, {"source": "actors", "page_size": True},
    {"source": "actors", "fields": ["unknown"]},
    {"source": "actors", "cursor": "x", "since": "y"},
    {"source": "actor_details", "actor_paths": []},
    {"source": "actor_details", "actor_paths": ["/a", "/a"]},
    {"source": "actor_details", "actor_paths": ["a" * 1025]},
    {"source": "assets", "actor_paths": ["/a"]},
    {"source": "asset_details"}, {"source": "actors", "path": "/Game"},
])
def test_invalid_read_never_reaches_bridge(params):
    with pytest.raises(ValidationError):
        ReadRequest(**params)


async def test_projection_has_measured_byte_savings_on_large_fixture():
    bridge = Bridge()
    result = await CompactReads(bridge).read(ReadRequest(source="actors", fields=["label"]))
    raw_bytes = len(json.dumps({"actors": bridge.rows, "truncated": True}).encode())
    compact_bytes = len(json.dumps(result).encode())
    assert compact_bytes < raw_bytes
