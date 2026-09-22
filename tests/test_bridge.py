"""Exercise authentication, project binding and failures without a live editor."""

import asyncio
import json
import time

import httpx
import pytest

from jev_unreal.bridge import UnrealBridge, project_identity
from jev_unreal.config import Settings
from jev_unreal.errors import JevError

TOKEN = "test-only-bridge-token-01234567890123456789"
PROJECT = r"C:\Projects\Disposable\Disposable.uproject"


@pytest.mark.parametrize("action", ["blueprint_compile_preview", "blueprint_compile"])
async def test_compile_requests_bind_authenticated_project_and_preserve_reviewed_state(action):
    seen = []
    state = {"session_id": "reviewed-session", "world_path": "/Game/Map", "revision": "old"}

    def handler(request):
        body = json.loads(request.content)
        seen.append(body)
        if body["action"] == "status":
            return response({"project_file": PROJECT, "capabilities": [action]})
        return response({"compiled": False})

    bridge = UnrealBridge(
        Settings(bridge_token=TOKEN, expected_project=PROJECT), httpx.MockTransport(handler)
    )
    try:
        await bridge.call(action, {"expected_state": state, "expected_project": "forged"})
    finally:
        await bridge.close()
    assert seen[1]["params"]["expected_project"] == PROJECT
    assert seen[1]["params"]["expected_state"] == state


@pytest.mark.parametrize("action", ["blueprint_compile_preview", "blueprint_compile"])
async def test_compile_requires_explicit_project_and_supported_plugin(action):
    seen = []

    def handler(request):
        seen.append(json.loads(request.content)["action"])
        return status_response()

    for expected, code in [("", "project_required"), (PROJECT, "capability_unavailable")]:
        bridge = UnrealBridge(
            Settings(bridge_token=TOKEN, expected_project=expected), httpx.MockTransport(handler)
        )
        try:
            with pytest.raises(JevError) as error:
                await bridge.call(action, {"plan_id": "never-dispatched"})
            assert error.value.code == code
        finally:
            await bridge.close()
    assert seen == ["status", "status"]


def response(result=None):
    return httpx.Response(200, json={"ok": True, "result": result or {}})


def status_response(project=PROJECT):
    return response({"project_file": project, "session": "test-session"})


@pytest.mark.parametrize(
    "action",
    [
        "status",
        "actors",
        "assets",
        "preview",
        "apply",
        "context",
        "asset_details",
        "validate",
        "capture",
        "frame",
    ],
)
async def test_authentication_and_project_identity_checked_before_each_operation(action):
    seen = []

    def handler(request):
        assert request.headers["Authorization"] == f"Bearer {TOKEN}"
        assert request.url == "http://127.0.0.1:9845/jev/v1/call"
        body = json.loads(request.content)
        seen.append(body)
        return status_response() if body["action"] == "status" else response({"received": action})

    bridge = UnrealBridge(
        Settings(bridge_token=TOKEN, expected_project=PROJECT), httpx.MockTransport(handler)
    )
    try:
        await bridge.call(action, {"test_parameter": "value"})
    finally:
        await bridge.close()
    assert seen[0] == {"action": "status", "params": {}}
    if action == "status":
        assert len(seen) == 1
    else:
        assert seen[1] == {"action": action, "params": {"test_parameter": "value"}}
        assert len(seen) == 2


async def test_fast_requests_are_paced_including_identity_reads():
    observed = []

    def handler(request):
        observed.append(time.monotonic())
        return status_response()

    bridge = UnrealBridge(Settings(bridge_token=TOKEN), httpx.MockTransport(handler))
    try:
        await bridge.call("actors")
        await bridge.call("context")
        assert len(observed) == 4
        assert all(b - a >= 0.045 for a, b in zip(observed, observed[1:], strict=False))
    finally:
        await bridge.close()


class PacingClock:
    """Advance only this bridge's clock, leaving pytest's event-loop clock alone."""

    def __init__(self, early_wakeups=()):
        self.now = 100.0
        self.early_wakeups = iter(early_wakeups)
        self.sleeps = []

    def monotonic(self):
        return self.now

    def advance(self, seconds):
        self.now += seconds

    async def sleep(self, delay):
        self.sleeps.append(delay)
        self.advance(min(delay, next(self.early_wakeups, delay)))


def bridge_with_clock(clock, handler):
    bridge = UnrealBridge(Settings(bridge_token=TOKEN), httpx.MockTransport(handler))
    bridge._clock = clock.monotonic
    bridge._sleep = clock.sleep
    return bridge


async def test_pacing_rechecks_early_wakeups_before_identity_reads_and_operations():
    clock = PacingClock(early_wakeups=(0.01, 0.02))
    observed = []

    def handler(request):
        observed.append((json.loads(request.content)["action"], clock.monotonic()))
        return status_response()

    bridge = bridge_with_clock(clock, handler)
    try:
        await bridge.call("actors")
        await bridge.call("context")
        assert [action for action, _ in observed] == ["status", "actors", "status", "context"]
        assert [instant for _, instant in observed] == pytest.approx(
            [100.0, 100.05, 100.10, 100.15]
        )
        assert clock.sleeps == pytest.approx([0.05, 0.04, 0.02, 0.05, 0.05])
    finally:
        await bridge.close()


async def test_pacing_interval_starts_after_transport_body_and_close_complete():
    clock = PacingClock()
    started, completed = [], []

    class SlowResponse(httpx.AsyncByteStream):
        async def __aiter__(self):
            clock.advance(0.07)
            yield json.dumps({"ok": True, "result": {"project_file": PROJECT}}).encode()

        async def aclose(self):
            clock.advance(0.04)
            completed.append(clock.monotonic())

    def handler(request):
        started.append(clock.monotonic())
        clock.advance(0.03)
        return httpx.Response(200, stream=SlowResponse())

    bridge = bridge_with_clock(clock, handler)
    try:
        await bridge.call("status")
        await bridge.call("status")
        assert len(started) == len(completed) == 2
        assert completed[0] - started[0] == pytest.approx(0.14)
        assert started[1] - completed[0] == pytest.approx(0.05)
        assert clock.sleeps == pytest.approx([0.05])
    finally:
        await bridge.close()


@pytest.mark.parametrize("failure", ["transport", "status", "body"])
async def test_failed_http_attempts_still_require_a_full_interval_without_retry(failure):
    clock = PacingClock()
    started, completed = [], []

    class FailedResponse(httpx.AsyncByteStream):
        async def __aiter__(self):
            clock.advance(0.08)
            raise httpx.ReadError("synthetic-private-diagnostic")
            yield b""  # Make this failing body an async iterator.

        async def aclose(self):
            clock.advance(0.03)
            completed.append(clock.monotonic())

    def handler(request):
        started.append(clock.monotonic())
        if len(started) > 1:
            return status_response()
        clock.advance(0.08)
        if failure == "transport":
            completed.append(clock.monotonic())
            raise httpx.ConnectError("synthetic-private-diagnostic", request=request)
        return httpx.Response(429 if failure == "status" else 200, stream=FailedResponse())

    bridge = bridge_with_clock(clock, handler)
    try:
        with pytest.raises(JevError) as caught:
            await bridge.call("status")
        assert caught.value.code == (
            "rate_limited" if failure == "status" else "editor_unavailable"
        )
        assert "synthetic-private-diagnostic" not in str(caught.value)
        assert len(started) == len(completed) == 1
        await bridge.call("status")
        assert len(started) == 2
        assert started[1] - completed[0] == pytest.approx(0.05)
    finally:
        await bridge.close()


async def test_cancellation_while_pacing_preserves_deadline_without_dispatching():
    clock = PacingClock()
    started = []
    waiting, never = asyncio.Event(), asyncio.Event()

    def handler(request):
        started.append(clock.monotonic())
        return status_response()

    async def cancellable_sleep(delay):
        clock.sleeps.append(delay)
        clock.advance(0.01)
        waiting.set()
        await never.wait()

    bridge = bridge_with_clock(clock, handler)
    await bridge.call("status")
    bridge._sleep = cancellable_sleep
    pending = asyncio.create_task(bridge.call("actors"))
    try:
        await asyncio.wait_for(waiting.wait(), timeout=5)
        pending.cancel()
        with pytest.raises(asyncio.CancelledError):
            await pending
        assert started == [100.0]
        assert bridge._next_request == pytest.approx(100.05)
        bridge._sleep = clock.sleep
        await bridge.call("status")
        assert started == pytest.approx([100.0, 100.05])
        assert clock.sleeps == pytest.approx([0.05, 0.04])
    finally:
        pending.cancel()
        await asyncio.gather(pending, return_exceptions=True)
        await bridge.close()


@pytest.mark.parametrize("phase", ["entry", "body", "close"])
async def test_cancellation_during_http_attempt_paces_next_request_after_cleanup(phase):
    clock = PacingClock()
    started, completed = [], []
    waiting, never = asyncio.Event(), asyncio.Event()

    async def pause():
        clock.advance(0.08)
        waiting.set()
        await never.wait()

    class CancelledResponse(httpx.AsyncByteStream):
        async def __aiter__(self):
            if phase == "body":
                await pause()
            yield json.dumps({"ok": True, "result": {"project_file": PROJECT}}).encode()

        async def aclose(self):
            try:
                if phase == "close":
                    await pause()
            finally:
                clock.advance(0.03)
                completed.append(clock.monotonic())

    async def handler(request):
        started.append(clock.monotonic())
        if len(started) > 1:
            return status_response()
        if phase == "entry":
            try:
                await pause()
            finally:
                clock.advance(0.03)
                completed.append(clock.monotonic())
        return httpx.Response(200, stream=CancelledResponse())

    bridge = bridge_with_clock(clock, handler)
    pending = asyncio.create_task(bridge.call("status"))
    try:
        await asyncio.wait_for(waiting.wait(), timeout=5)
        pending.cancel()
        with pytest.raises(asyncio.CancelledError):
            await pending
        assert len(started) == len(completed) == 1
        await bridge.call("status")
        assert len(started) == 2
        assert started[1] - completed[0] == pytest.approx(0.05)
    finally:
        pending.cancel()
        await asyncio.gather(pending, return_exceptions=True)
        await bridge.close()


@pytest.mark.parametrize(
    "http_status,code", [(429, "rate_limited"), (401, "unauthorized"), (403, "forbidden")]
)
async def test_native_http_rejections_are_distinct_and_never_retried(http_status, code):
    calls = []

    def handler(request):
        calls.append(request)
        return httpx.Response(http_status, text="synthetic-private-diagnostic")

    bridge = UnrealBridge(Settings(bridge_token=TOKEN), httpx.MockTransport(handler))
    try:
        with pytest.raises(JevError) as caught:
            await bridge.call("apply", {"plan_id": "once"})
        assert caught.value.code == code
        assert "synthetic-private-diagnostic" not in str(caught.value)
        assert len(calls) == 1
    finally:
        await bridge.close()


@pytest.mark.parametrize("action", ["preview", "apply", "frame"])
async def test_scene_edits_require_explicit_project_configuration(action):
    calls = []

    def handler(request):
        calls.append(json.loads(request.content)["action"])
        return status_response()

    bridge = UnrealBridge(Settings(bridge_token=TOKEN), httpx.MockTransport(handler))
    try:
        with pytest.raises(JevError) as caught:
            await bridge.call(action)
        assert caught.value.code == "project_required"
        assert calls == ["status"]
    finally:
        await bridge.close()


@pytest.mark.parametrize(
    "action",
    [
        "status",
        "actors",
        "assets",
        "preview",
        "apply",
        "context",
        "asset_details",
        "validate",
        "capture",
        "frame",
    ],
)
async def test_wrong_project_blocks_reads_and_mutations(action):
    calls = []

    def handler(request):
        calls.append(json.loads(request.content)["action"])
        return status_response(r"C:\Projects\Production\Production.uproject")

    bridge = UnrealBridge(
        Settings(bridge_token=TOKEN, expected_project=PROJECT), httpx.MockTransport(handler)
    )
    try:
        with pytest.raises(JevError) as caught:
            await bridge.call(action)
        assert caught.value.code == "wrong_project"
        assert calls == ["status"]
    finally:
        await bridge.close()


def test_windows_project_identity_handles_case_and_separator_differences():
    other_spelling = "c:/projects/disposable/Disposable.uproject"
    assert project_identity(PROJECT) == project_identity(other_spelling)


@pytest.mark.parametrize("identity", [None, "", 17, {"path": PROJECT}])
async def test_missing_or_malformed_editor_identity_blocks_operations(identity):
    bridge = UnrealBridge(
        Settings(bridge_token=TOKEN, expected_project=PROJECT),
        httpx.MockTransport(lambda request: status_response(identity)),
    )
    try:
        with pytest.raises(JevError) as caught:
            await bridge.call("actors")
        assert caught.value.code == "bridge_error"
    finally:
        await bridge.close()


@pytest.mark.parametrize("action", ["execute_python", "delete_assets", "shell", "save", ""])
async def test_allowlist_rejects_arbitrary_actions_without_network_requests(action):
    def handler(request):
        pytest.fail("Disallowed operation reached the network")

    bridge = UnrealBridge(Settings(bridge_token=TOKEN), httpx.MockTransport(handler))
    try:
        with pytest.raises(JevError) as caught:
            await bridge.call(action)
        assert caught.value.code == "unknown_action"
    finally:
        await bridge.close()


@pytest.mark.parametrize("token", ["", "too-short"])
async def test_missing_token_is_rejected_before_network_request(token):
    def handler(request):
        pytest.fail("Unauthenticated request reached the network")

    bridge = UnrealBridge(Settings(bridge_token=token), httpx.MockTransport(handler))
    try:
        with pytest.raises(JevError) as caught:
            await bridge.call("status")
        assert caught.value.code == "missing_bridge_token"
    finally:
        await bridge.close()


@pytest.mark.parametrize("failure", ["timeout", "http_error"])
async def test_apply_is_never_retried_after_an_ambiguous_failure(failure):
    calls = []

    def handler(request):
        action = json.loads(request.content)["action"]
        calls.append(action)
        if action == "status":
            return status_response()
        if failure == "timeout":
            raise httpx.ReadTimeout(
                "possibly applied; synthetic private diagnostic", request=request
            )
        return httpx.Response(503, text="synthetic private diagnostic")

    bridge = UnrealBridge(
        Settings(bridge_token=TOKEN, expected_project=PROJECT), httpx.MockTransport(handler)
    )
    try:
        with pytest.raises(JevError) as caught:
            await bridge.call("apply", {"plan_id": "single-use-plan"})
        assert caught.value.code in {"editor_unavailable", "bridge_error"}
        assert "synthetic private diagnostic" not in str(caught.value)
        assert calls == ["status", "apply"]
    finally:
        await bridge.close()


@pytest.mark.parametrize(
    "payload",
    [
        [],
        {"ok": True, "result": []},
        {"ok": True},
        {"ok": 1, "result": {}},
        {"ok": False, "error": []},
        {"ok": False, "error": {"code": ["unexpected"]}},
        {"ok": False, "error": {"code": "synthetic-private-error"}},
    ],
)
async def test_malformed_response_and_unknown_errors_are_sanitized(payload):
    bridge = UnrealBridge(
        Settings(bridge_token=TOKEN),
        httpx.MockTransport(lambda request: httpx.Response(200, json=payload)),
    )
    try:
        with pytest.raises(JevError) as caught:
            await bridge.call("status")
        assert caught.value.code == "bridge_error"
        assert "synthetic-private-error" not in str(caught.value)
    finally:
        await bridge.close()


@pytest.mark.parametrize(
    "error_code",
    ["stale_plan", "expired_plan", "play_mode", "apply_failed", "material_slot_invalid"],
)
async def test_known_editor_failure_codes_are_preserved_without_raw_messages(error_code):
    bridge = UnrealBridge(
        Settings(bridge_token=TOKEN),
        httpx.MockTransport(
            lambda request: httpx.Response(
                200,
                json={
                    "ok": False,
                    "error": {"code": error_code, "message": "synthetic-private-diagnostic"},
                },
            )
        ),
    )
    try:
        with pytest.raises(JevError) as caught:
            await bridge.call("status")
        assert caught.value.code == error_code
        assert "synthetic-private-diagnostic" not in str(caught.value)
    finally:
        await bridge.close()


@pytest.mark.parametrize(
    "content", [b"not-json", b"x" * (1048576 + 1)], ids=["invalid-json", "over-1-mib"]
)
async def test_invalid_json_and_oversized_response_fail_safely(content):
    bridge = UnrealBridge(
        Settings(bridge_token=TOKEN),
        httpx.MockTransport(lambda request: httpx.Response(200, content=content)),
    )
    try:
        with pytest.raises(JevError) as caught:
            await bridge.call("status")
        assert caught.value.code == "bridge_error"
    finally:
        await bridge.close()


async def test_redirects_do_not_forward_bridge_credentials():
    calls = []

    def handler(request):
        calls.append(request.url)
        return httpx.Response(307, headers={"Location": "https://external.example/steal-token"})

    bridge = UnrealBridge(Settings(bridge_token=TOKEN), httpx.MockTransport(handler))
    try:
        with pytest.raises(JevError) as caught:
            await bridge.call("status")
        assert caught.value.code == "bridge_error"
        assert len(calls) == 1
    finally:
        await bridge.close()


@pytest.mark.parametrize(
    "action,params,required",
    [
        ("actor_details", {"actor_paths": ["/Temp/Map.Actor"]}, ["actor_details"]),
        (
            "preview",
            {
                "operations": [],
                "expected_state": {"session_id": "s", "world_path": "w", "revision": "r"},
            },
            ["preview_expected_state"],
        ),
        ("preview", {"operations": [{"op": "set_material"}]}, ["set_material"]),
        ("preview", {"operations": [{"op": "set_metadata"}]}, ["set_metadata"]),
        ("preview", {"operations": [{"op": "replace_mesh"}]}, ["replace_mesh"]),
        ("preview", {"operations": [{"op": "duplicate_mesh"}]}, ["duplicate_mesh"]),
        (
            "preview",
            {
                "operations": [{"op": "replace_mesh"}, {"op": "duplicate_mesh"}],
                "expected_state": {"session_id": "s", "world_path": "w", "revision": "r"},
            },
            ["preview_expected_state", "replace_mesh", "duplicate_mesh"],
        ),
        (
            "frame",
            {"actor_paths": ["/Temp/Map.Map:PersistentLevel.Cube"], "view": "top"},
            ["frame_views"],
        ),
        (
            "preview",
            {
                "operations": [{"op": "set_material"}, {"op": "set_metadata"}],
                "expected_state": {"session_id": "s", "world_path": "w", "revision": "r"},
            },
            ["preview_expected_state", "set_material", "set_metadata"],
        ),
    ],
)
async def test_required_native_capabilities_are_checked_as_one_set_before_dispatch(
    action, params, required
):
    advertised = []
    seen = []

    def handler(request):
        body = json.loads(request.content)
        seen.append(body)
        if body["action"] == "status":
            return response({"project_file": PROJECT, "capabilities": advertised})
        return response({"received": body["params"]})

    bridge = UnrealBridge(
        Settings(bridge_token=TOKEN, expected_project=PROJECT), httpx.MockTransport(handler)
    )
    try:
        # Every missing capability fails before native dispatch, including a
        # partial advertisement where all the other required flags are present.
        for missing in required:
            advertised = [capability for capability in required if capability != missing]
            previous = len(seen)
            with pytest.raises(JevError) as caught:
                await bridge.call(action, params)
            assert caught.value.code == "capability_unavailable"
            assert seen[previous:] == [{"action": "status", "params": {}}]
        advertised = [*required, "future_extension", None, {"not": "a capability"}]
        result = await bridge.call(action, params)
        assert result == {"received": params}
        assert seen[-2:] == [
            {"action": "status", "params": {}},
            {"action": action, "params": params},
        ]
    finally:
        await bridge.close()


@pytest.mark.parametrize(
    "capabilities",
    [None, "actor_details", {"actor_details": True}, True, 1, [], ["Actor_Details"], [1, None, {}]],
)
async def test_malformed_or_nonmatching_capabilities_cannot_authorize_new_native_actions(
    capabilities,
):
    seen = []

    def handler(request):
        seen.append(json.loads(request.content)["action"])
        return response({"project_file": PROJECT, "capabilities": capabilities})

    bridge = UnrealBridge(
        Settings(bridge_token=TOKEN, expected_project=PROJECT), httpx.MockTransport(handler)
    )
    try:
        with pytest.raises(JevError) as caught:
            await bridge.call("actor_details", {"actor_paths": ["/Temp/Map.Actor"]})
        assert caught.value.code == "capability_unavailable"
        assert seen == ["status"]
    finally:
        await bridge.close()


async def test_capabilities_are_refreshed_each_call_and_do_not_bypass_project_binding():
    advertised = ["actor_details"]
    project = PROJECT
    seen = []

    def handler(request):
        action = json.loads(request.content)["action"]
        seen.append(action)
        return response({"project_file": project, "capabilities": advertised})

    bridge = UnrealBridge(
        Settings(bridge_token=TOKEN, expected_project=PROJECT), httpx.MockTransport(handler)
    )
    try:
        await bridge.call("actor_details", {"actor_paths": ["/Temp/Map.Actor"]})
        advertised = []
        with pytest.raises(JevError) as caught:
            await bridge.call("actor_details", {"actor_paths": ["/Temp/Map.Actor"]})
        assert caught.value.code == "capability_unavailable"
        advertised = ["actor_details"]
        project = r"C:\Projects\Other\Other.uproject"
        with pytest.raises(JevError) as caught:
            await bridge.call("actor_details", {"actor_paths": ["/Temp/Map.Actor"]})
        assert caught.value.code == "wrong_project"
        assert seen == ["status", "actor_details", "status", "status"]
    finally:
        await bridge.close()


@pytest.mark.parametrize(
    "action,params",
    [
        ("actors", {}),
        ("frame", {"actor_paths": ["/Temp/Map.Map:PersistentLevel.Cube"], "padding": 1.2}),
        ("preview", {"operations": [{"op": "set_transform"}]}),
        ("preview", {"operations": [{"op": "spawn_primitive"}], "expected_state": None}),
        ("apply", {"plan_id": "legacy-native-plan"}),
    ],
)
async def test_legacy_native_operations_do_not_require_new_capability_advertisements(
    action, params
):
    seen = []

    def handler(request):
        body = json.loads(request.content)
        seen.append(body)
        return status_response() if body["action"] == "status" else response({"received": action})

    bridge = UnrealBridge(
        Settings(bridge_token=TOKEN, expected_project=PROJECT), httpx.MockTransport(handler)
    )
    try:
        assert await bridge.call(action, params) == {"received": action}
        assert seen == [{"action": "status", "params": {}}, {"action": action, "params": params}]
    finally:
        await bridge.close()
