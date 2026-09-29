"""Acceptance evidence regression tests use synthetic PNGs and a fake native bridge."""

import base64
import struct
import zlib
from copy import deepcopy
from types import SimpleNamespace
from unittest.mock import AsyncMock

import pytest
from mcp.server.fastmcp import FastMCP
from mcp.server.fastmcp.exceptions import ToolError

from jev_unreal.acceptance_workflow import (
    MAX_CAPTURES,
    RETENTION_SECONDS,
    AcceptanceWorkflows,
    VisualReview,
    _rgba,
    register_acceptance_tools,
)
from jev_unreal.capture import capture_content
from jev_unreal.errors import JevError
from jev_unreal.verification import LabelEquals
from jev_unreal.workflows import ExpectedState

IDENTITY = {
    "project_file": "C:/Public/Game.uproject",
    "session_id": "session-a",
    "world_path": "/Game/Map.Map",
    "revision": "revision-a",
}
STATE = ExpectedState(**{key: IDENTITY[key] for key in ("session_id", "world_path", "revision")})
CAMERA = {
    "location": [100, 100, 100],
    "rotation": [-20, 45, 0],
    "fov_degrees": 90,
    "realtime": False,
    "realtime_override": False,
    "fixed_exposure": True,
    "fixed_ev100": 0,
    "view_mode_index": 3,
    "motion_blur": False,
}
CHECK = LabelEquals(kind="label", actor_path="/Game/Map.Map:PersistentLevel.Cube", expected="Cube")
REVIEW = VisualReview(
    reviewer="human",
    verdict="passed",
    criteria="Cube remains visible",
    observations="Both images reviewed; cube is visible.",
)


def chunk(kind, payload):
    return (
        struct.pack(">I", len(payload))
        + kind
        + payload
        + struct.pack(">I", zlib.crc32(kind + payload))
    )


def png(pixels=b"\x10\x20\x30\xff", *, compressed=None, color=6, filtering=0):
    channels = 3 if color == 2 else 4
    width = len(pixels) // channels
    header = struct.pack(">IIBBBBB", width, 1, 8, color, 0, 0, 0)
    data = zlib.compress(bytes([filtering]) + pixels) if compressed is None else compressed
    return (
        b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", data) + chunk(b"IEND", b"")
    )


class FakeBridge:
    def __init__(self):
        self.settings = SimpleNamespace(expected_project=IDENTITY["project_file"])
        self.identity = deepcopy(IDENTITY)
        self.camera = deepcopy(CAMERA)
        self.image = png()
        self.calls = []
        self.discovery = {
            "enabled": True,
            "configuration_valid": True,
            "standalone_pie_ready": True,
            "tests": [{"id": "approved", "editor_actor_available": True, "test_enabled": True}],
        }
        self.job = {
            **IDENTITY,
            "job_id": "native-job",
            "test_id": "approved",
            "state": "queued",
            "started": True,
            "cleanup_attempted": True,
            "native_result": "Succeeded",
            "observed_error_count": 0,
            "truncated": False,
        }
        self.hook = None

    async def call(self, action, params=None):
        self.calls.append((action, deepcopy(params)))
        if self.hook:
            self.hook(action, params)
        if action == "status":
            return deepcopy(self.identity)
        if action == "workflow_inspect":
            return {**deepcopy(self.identity), **deepcopy(self.camera)}
        if action == "capture":
            width, height = struct.unpack_from(">II", self.image, 16)
            return {
                "data": base64.b64encode(self.image).decode(),
                "mime_type": "image/png",
                "width": width,
                "height": height,
                "source": "editor_viewport",
                "world_path": self.identity["world_path"],
                "revision": self.identity["revision"],
                "current_camera_location": deepcopy(self.camera["location"]),
                "current_camera_rotation": deepcopy(self.camera["rotation"]),
            }
        if action == "functional_tests":
            return {**deepcopy(self.identity), **deepcopy(self.discovery)}
        if action in {"functional_start", "functional_job"}:
            return deepcopy(self.job)
        raise AssertionError(action)


async def record(workflow, *, checks=None, protocol="test", expected=STATE):
    return await workflow.capture(protocol, expected, checks or [], 512)


async def start_run():
    bridge = FakeBridge()
    workflow = AcceptanceWorkflows(bridge)
    baseline = await record(workflow)
    run = await workflow.playtest_start(baseline["receipt_id"], "approved", STATE)
    return bridge, workflow, baseline, run


async def test_capture_compares_pixels_but_requires_explicit_visual_review(monkeypatch):
    bridge = FakeBridge()
    workflow = AcceptanceWorkflows(bridge)
    verify = AsyncMock(return_value={"status": "passed", "checks": [{"status": "passed"}]})
    monkeypatch.setattr("jev_unreal.acceptance_workflow.verify_fresh", verify)
    first = await record(workflow, checks=[CHECK])
    bridge.image = png(b"\x15\x20\x30\xff")
    bridge.identity["revision"] = "revision-b"
    second = await record(
        workflow, checks=[CHECK], expected=STATE.model_copy(update={"revision": "revision-b"})
    )
    assert first["visual_acceptance"] == "review_required"
    assert verify.await_args.kwargs["expected_revision"] == "revision-b"
    assert verify.await_args.kwargs["expected_identity"]["world_path"] == IDENTITY["world_path"]
    result = workflow.compare(first["receipt_id"], second["receipt_id"])
    assert result["status"] == "inconclusive"
    assert result["pixel_difference"]["changed_percent"] == 100
    assert result["pixel_difference"]["max_channel_difference"] == 5
    result = workflow.compare(first["receipt_id"], second["receipt_id"], 5, REVIEW)
    assert result["pixel_difference"]["changed_pixels"] == 0
    assert result["status"] == "passed_with_declared_visual_review"
    assert result["visual_review_source"] == "caller_attestation"
    assert result["automated_visual_quality_proven"] is False
    assert not any(action in {"frame", "apply", "functional_start"} for action, _ in bridge.calls)


@pytest.mark.parametrize("changed", ["world_path", "session_id", "revision", "camera"])
async def test_capture_rejects_state_change_and_never_retains_partial_receipt(changed):
    bridge = FakeBridge()
    workflow = AcceptanceWorkflows(bridge)

    def change(action, _params):
        if action == "capture":
            if changed == "camera":
                bridge.camera["fov_degrees"] = 70
            else:
                bridge.identity[changed] += "-changed"

    bridge.hook = change
    with pytest.raises(JevError, match="state|Camera|Capture|camera"):
        await record(workflow)
    assert not workflow.captures


async def test_capture_requires_explicit_project_and_rejects_stale_expected_state():
    bridge = FakeBridge()
    workflow = AcceptanceWorkflows(bridge)
    bridge.settings.expected_project = ""
    with pytest.raises(JevError) as failure:
        await record(workflow)
    assert failure.value.code == "project_required"
    assert not bridge.calls
    bridge.settings.expected_project = "C:/Other/Game.uproject"
    with pytest.raises(JevError) as failure:
        await record(workflow)
    assert failure.value.code == "wrong_project"
    bridge.settings.expected_project = IDENTITY["project_file"]
    with pytest.raises(JevError) as failure:
        await record(workflow, expected=STATE.model_copy(update={"revision": "stale"}))
    assert failure.value.code == "stale_plan"
    assert all(action == "status" for action, _ in bridge.calls)


@pytest.mark.parametrize("changed", ["protocol_id", "camera", "max_dimension", "world", "checks"])
async def test_incompatible_captures_are_inconclusive_even_with_declared_pass(changed):
    bridge = FakeBridge()
    workflow = AcceptanceWorkflows(bridge)
    first, second = await record(workflow), await record(workflow)
    row = workflow.captures[second["receipt_id"]]
    if changed == "world":
        row["receipt"]["identity"]["world_path"] = "/Game/Other.Other"
    elif changed == "checks":
        row["checks"] = [CHECK]
    else:
        row["receipt"][changed] = "changed"
    result = workflow.compare(first["receipt_id"], second["receipt_id"], visual_review=REVIEW)
    assert not result["comparable"]
    assert result["status"] == "inconclusive"
    assert result["pixel_difference"] == {"available": False}


async def test_failed_or_unverifiable_requirements_cannot_be_overridden_by_visual_review():
    workflow = AcceptanceWorkflows(FakeBridge())
    first, second = await record(workflow), await record(workflow)
    row = workflow.captures[second["receipt_id"]]
    row["receipt"]["verification"]["status"] = "failed"
    assert (
        workflow.compare(first["receipt_id"], second["receipt_id"], visual_review=REVIEW)["status"]
        == "failed"
    )
    row["receipt"]["verification"]["status"] = "unverifiable"
    assert (
        workflow.compare(first["receipt_id"], second["receipt_id"], visual_review=REVIEW)["status"]
        == "inconclusive"
    )


def test_png_decoder_handles_native_rgb_rgba_filters_and_skips_unsupported_formats():
    assert _rgba(png(b"\x10\x20\x30", color=2)) == b"\x10\x20\x30\xff"
    assert _rgba(png()) == b"\x10\x20\x30\xff"
    pixels = b"\x10\x20\x30\xff\x10\x10\x10\x00"
    # Two pixels: Sub/Paeth subtract the prior pixel, Up is zero on this first row.
    for filtering in (1, 4):
        assert _rgba(png(pixels, filtering=filtering)) == b"\x10\x20\x30\xff\x20\x30\x40\xff"
    assert _rgba(png(filtering=2)) == b"\x10\x20\x30\xff"
    assert (
        _rgba(png(b"\x10\x20\x30\xff\x18\x20\x28\x80", filtering=3))
        == b"\x10\x20\x30\xff\x20\x30\x40\xff"
    )
    assert _rgba(png(color=0)) is None


@pytest.mark.parametrize(
    "compressed",
    [
        zlib.compress(b"\x00" * 10000),
        zlib.compress(b"\x00"),
        b"invalid",
        zlib.compress(b"\x00\x10\x20\x30\xff")[:-1],
        zlib.compress(b"\x00\x10\x20\x30\xff") + b"trailing",
        zlib.compress(b"\x05\x10\x20\x30\xff"),
    ],
)
async def test_structurally_valid_png_with_invalid_pixel_stream_is_rejected(compressed):
    bridge = FakeBridge()
    bridge.image = png(compressed=compressed)
    # Structural checks alone accept these CRC-correct payloads; bounded decoding must not.
    capture_content(await bridge.call("capture"))
    workflow = AcceptanceWorkflows(bridge)
    with pytest.raises(JevError) as failure:
        await record(workflow)
    assert failure.value.code == "capture_failed"
    assert not workflow.captures


async def test_receipts_are_bounded_expire_and_require_distinct_captures():
    now = [0]
    workflow = AcceptanceWorkflows(FakeBridge(), clock=lambda: now[0])
    first = await record(workflow)
    result = workflow.compare(first["receipt_id"], first["receipt_id"])
    assert result["inconclusive_reasons"] == ["distinct_captures_required"]
    for _ in range(MAX_CAPTURES):
        await record(workflow)
    assert len(workflow.captures) == MAX_CAPTURES
    with pytest.raises(JevError, match="expired"):
        workflow._get(workflow.captures, first["receipt_id"])
    last_id = next(reversed(workflow.captures))
    now[0] = RETENTION_SECONDS
    with pytest.raises(JevError, match="expired"):
        workflow._get(workflow.captures, last_id)


@pytest.mark.parametrize(
    "field,value,code",
    [
        ("enabled", False, "functional_disabled"),
        ("configuration_valid", False, "functional_disabled"),
        ("standalone_pie_ready", False, "pie_required"),
        ("tests", [], "test_not_allowed"),
    ],
)
async def test_playtest_preflight_never_starts_disallowed_or_unready_tests(field, value, code):
    bridge = FakeBridge()
    workflow = AcceptanceWorkflows(bridge)
    baseline = await record(workflow)
    bridge.discovery[field] = value
    with pytest.raises(JevError) as failure:
        await workflow.playtest_start(baseline["receipt_id"], "approved", STATE)
    assert failure.value.code == code
    assert all(action != "functional_start" for action, _ in bridge.calls)


async def test_successful_playtest_polls_without_wait_and_captures_after_once():
    bridge, workflow, baseline, run = await start_run()
    assert run["status"] == "pending"
    pending = await workflow.playtest_job(run["run_id"])
    assert pending["status"] == "pending"
    assert len(workflow.captures) == 1
    bridge.job["state"] = "passed"
    terminal = await workflow.playtest_job(run["run_id"])
    assert terminal["status"] == "gameplay_passed_visual_review_required"
    assert terminal["gameplay_acceptance"] == "passed"
    assert terminal["comparison"]["baseline_id"] == baseline["receipt_id"]
    assert terminal["comparison"]["visual_status"] == "review_required"
    assert terminal["after_capture_id"] in workflow.captures
    calls = len(bridge.calls)
    assert await workflow.playtest_job(run["run_id"]) == terminal
    assert len(bridge.calls) == calls
    assert sum(action == "functional_start" for action, _ in bridge.calls) == 1


@pytest.mark.parametrize(
    "overrides",
    [
        {"state": "failed"},
        {"state": "error"},
        {"state": "timeout"},
        {"state": "timed_out"},
        {"state": "cancelled"},
        {"state": "interrupted"},
        {"state": "unknown"},
        {"state": "passed", "cleanup_attempted": False},
        {"state": "passed", "started": False},
        {"state": "passed", "native_result": "Failed"},
        {"state": "passed", "observed_error_count": 1},
        {"state": "passed", "truncated": True},
    ],
)
async def test_nonpassing_or_incomplete_playtest_evidence_never_passes(overrides):
    bridge, workflow, _, run = await start_run()
    bridge.job.update(overrides)
    result = await workflow.playtest_job(run["run_id"])
    assert result["status"] in {"failed", "inconclusive"}
    assert result["gameplay_acceptance"] != "passed"


async def test_missing_job_and_changed_world_are_not_success_and_never_restart():
    bridge, workflow, _, run = await start_run()

    def missing(action, _params):
        if action == "functional_job":
            raise JevError("unknown_job", "Expired")

    bridge.hook = missing
    result = await workflow.playtest_job(run["run_id"])
    assert result["status"] == "inconclusive"
    assert result["reason"] == "unknown_job"
    bridge.hook = None
    bridge.identity["world_path"] = "/Game/Other.Other"
    with pytest.raises(JevError) as failure:
        await workflow.playtest_job(run["run_id"])
    assert failure.value.code == "state_changed"
    assert sum(action == "functional_start" for action, _ in bridge.calls) == 1


async def test_after_capture_failure_retains_gameplay_evidence_without_overall_acceptance():
    bridge, workflow, _, run = await start_run()
    bridge.job["state"] = "passed"

    def unavailable(action, _params):
        if action == "capture":
            raise JevError("viewport_unavailable", "No viewport")

    bridge.hook = unavailable
    result = await workflow.playtest_job(run["run_id"])
    assert result["status"] == "inconclusive"
    assert result["gameplay_acceptance"] == "passed"
    assert result["after_capture_error"] == "viewport_unavailable"


async def test_terminal_after_capture_does_not_evict_its_own_oldest_baseline():
    bridge, workflow, baseline, run = await start_run()
    for _ in range(MAX_CAPTURES - 1):
        await record(workflow)
    assert next(iter(workflow.captures)) == baseline["receipt_id"]
    bridge.job["state"] = "passed"
    result = await workflow.playtest_job(run["run_id"])
    assert result["status"] == "gameplay_passed_visual_review_required"
    assert baseline["receipt_id"] in workflow.captures
    assert len(workflow.captures) == MAX_CAPTURES


async def test_mcp_tools_emit_image_pairs_and_validate_bounds_before_native_calls():
    bridge = FakeBridge()
    app = FastMCP("acceptance")
    workflow = register_acceptance_tools(app, bridge)
    tools = {tool.name: tool for tool in await app.list_tools()}
    assert tools["unreal_acceptance_playtest_start"].annotations.readOnlyHint is False
    assert tools["unreal_acceptance_compare"].annotations.readOnlyHint is True
    with pytest.raises(ToolError):
        await app.call_tool(
            "unreal_acceptance_capture",
            {
                "protocol_id": "test",
                "expected_state": STATE.model_dump(),
                "max_dimension": 1025,
            },
        )
    assert not bridge.calls
    first = await record(workflow)
    second = await record(workflow)
    content = await app.call_tool(
        "unreal_acceptance_compare",
        {
            "baseline_id": first["receipt_id"],
            "candidate_id": second["receipt_id"],
            "include_images": True,
        },
    )
    assert [item.type for item in content.content] == ["text", "image", "image"]
