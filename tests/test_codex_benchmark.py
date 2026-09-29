from types import SimpleNamespace

import pytest

from jev_unreal.benchmarks import fingerprint
from jev_unreal.codex_benchmark import (
    AppServer,
    AssetCase,
    CodexDriver,
    decode_object,
    normalize_answer,
    read_state,
)
from jev_unreal.errors import JevError


def test_answer_canonicalization_preserves_numeric_equality_and_rejects_fake_evidence():
    first = {"path": "/Engine/BasicShapes/Cube.Cube", "size_cm": [100, 100, 100], "lod_count": 1}
    second = {**first, "size_cm": [100.0, 100.0, 100.0]}
    assert fingerprint(normalize_answer(first)) == fingerprint(normalize_answer(second))
    for bad in ({**first, "size_cm": [True, 100, 100]},
                {**first, "size_cm": [float("nan"), 100, 100]},
                {**first, "lod_count": True}, {**first, "explanation": "not evidence"}):
        with pytest.raises(ValueError):
            normalize_answer(bad)
    for raw in ('{"value":1,"value":2}', '{"value":NaN}', '[]'):
        with pytest.raises(ValueError):
            decode_object(raw)


async def test_agent_dispatch_refuses_unknown_tool_and_records_builtin_use():
    broker = AppServer("unused", "unused")
    broker.thread_id = "thread"
    called = []

    async def dispatch(*args):
        called.append(args)
        return {"ok": True}

    broker.tool_handler = dispatch
    with pytest.raises(JevError, match="out-of-scope"):
        await broker.event({"id": 1, "method": "item/tool/call", "params": {
            "threadId": "thread", "tool": "unreal_apply", "arguments": {}
        }})
    assert called == []
    await broker.event({"method": "item/started", "params": {
        "threadId": "thread", "item": {"type": "commandExecution"}
    }})
    assert broker.unexpected_tool


async def test_thread_requires_confirmed_read_only_ephemeral_policy(monkeypatch):
    broker = AppServer("unused", "unused")
    response = {"thread": {"id": "thread", "ephemeral": True}, "model": "test-model",
                "modelProvider": "test-provider", "approvalPolicy": "never",
                "sandbox": {"type": "readOnly"}}

    async def rpc(method, params):
        assert params["sandbox"] == "read-only" and params["ephemeral"]
        return response

    monkeypatch.setattr(broker, "rpc", rpc)
    assert await broker.prepare([]) == "thread"
    assert broker.environment["served_model_revision"] is None
    response["sandbox"] = {"type": "dangerFullAccess"}
    with pytest.raises(JevError, match="read-only isolation"):
        await broker.prepare([])


async def test_live_evidence_rejects_state_drift_and_pie():
    case = AssetCase(id="Cube", asset_path="/Engine/BasicShapes/Cube.Cube")
    state = {"project_file": "/Sandbox.uproject", "session_id": "s", "world_path": "w",
             "revision": "r", "bridge_version": "1", "engine_version": "5.8"}

    class Session:
        def __init__(self, drift=False, pie=False):
            self.calls = 0
            self.drift, self.pie = drift, pie

        async def call_tool(self, name, arguments):
            self.calls += 1
            result = ({**state, "revision": "changed" if self.drift and self.calls == 3 else "r",
                       "play_in_editor": self.pie}
                      if name == "unreal_status" else {
                          "path": case.asset_path,
                          "static_mesh": {"bounds_cm": {"size": [100, 100, 100]}, "lod_count": 1}
                      })
            return SimpleNamespace(structuredContent={"ok": True, "result": result})

    _, answer = await read_state(Session(), case)
    assert answer["size_cm"] == [100.0, 100.0, 100.0]
    for session in (Session(drift=True), Session(pie=True)):
        with pytest.raises(JevError, match="changed during"):
            await read_state(session, case)


async def test_router_uses_only_sanitized_current_request_cost_and_auth_failure():
    result = {"selected": "unreal_asset_details", "outcome": "recommend", "cached": False,
              "executed": False, "usage": {"cost": 0.0001}}

    class Session:
        async def call_tool(self, name, args):
            assert name == "jev_route"
            return SimpleNamespace(structuredContent={"ok": True, "result": result})

    driver = CodexDriver(None, None, Session(), [], {})
    task = SimpleNamespace(goal="Inspect public cube bounds")
    fresh = await driver.route(task)
    assert fresh.provider_requests == 1 and fresh.provider_cost_usd == 0.0001
    result["cached"] = True
    cached = await driver.route(task)
    assert cached.provider_requests == 0 and cached.provider_cost_usd is None
    result["selected"] = "unreal_apply"
    with pytest.raises(JevError, match="Invalid sanitized"):
        await driver.route(task)

    class RejectedSession:
        async def call_tool(self, name, args):
            return SimpleNamespace(structuredContent={
                "ok": False, "error": {"message": "Provider returned HTTP 401"}
            })

    driver.session = RejectedSession()
    rejected = await driver.route(task)
    assert rejected.outcome == "authentication_failed" and rejected.provider_cost_usd is None
