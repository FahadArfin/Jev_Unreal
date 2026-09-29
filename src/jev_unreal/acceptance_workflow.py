"""Bounded before/after evidence and explicitly started project-owned playtests.

Images and receipts stay in process memory. Pixel changes are measurements, never a
visual-quality verdict. No tool changes the camera, starts PIE, loads files or saves maps.
"""

import asyncio
import base64
import hashlib
import json
import math
import struct
import time
import uuid
import zlib
from collections import OrderedDict
from copy import deepcopy
from typing import Annotated, Literal

from mcp.server.fastmcp import FastMCP
from mcp.types import CallToolResult, ImageContent, TextContent, ToolAnnotations
from pydantic import BaseModel, ConfigDict, Field

from .bridge import UnrealBridge, project_identity
from .capture import capture_content
from .errors import JevError
from .verification import Check, SessionIdentity, verify_fresh
from .workflows import ExpectedState

ReceiptId = Annotated[str, Field(min_length=1, max_length=64, strict=True)]
ProtocolId = Annotated[str, Field(pattern=r"^[A-Za-z0-9_.-]{1,64}$", strict=True)]
Checks = Annotated[list[Check], Field(max_length=20)]
RETENTION_SECONDS = 1800
MAX_CAPTURES = 12
MAX_RUNS = 32
MAX_STORED_BYTES = 8 * 1024 * 1024
SCOPE = (
    "Historical editor-viewport evidence and selected actor checks only. Camera matching does "
    "not freeze temporal rendering, shader compilation, simulation or lighting. Pixel changes "
    "do not establish visual quality. No cloud call, scene edit, PIE launch/stop or save."
)
CAMERA_FIELDS = (
    "location",
    "rotation",
    "fov_degrees",
    "realtime",
    "realtime_override",
    "fixed_exposure",
    "fixed_ev100",
    "view_mode_index",
    "motion_blur",
)


class VisualReview(BaseModel):
    """A declared human/agent assessment, explicitly distinct from measured evidence."""

    model_config = ConfigDict(extra="forbid", strict=True)
    reviewer: str = Field(min_length=1, max_length=120)
    verdict: Literal["passed", "failed", "inconclusive"]
    criteria: str = Field(min_length=1, max_length=1000)
    observations: str = Field(min_length=1, max_length=2000)


def _rgba(raw: bytes) -> bytes | None:
    """Decode only native-compatible noninterlaced 8-bit RGB(A), with a hard output cap.

    capture_content has already checked chunks, CRCs, dimensions and compressed size.
    Unsupported formats have no pixel metric; malformed supported streams are errors.
    """
    width, height, depth, color, _, _, interlace = struct.unpack_from(">IIBBBBB", raw, 16)
    if depth != 8 or color not in (2, 6) or interlace != 0:
        return None
    channels = 3 if color == 2 else 4
    stride = width * channels
    compressed = bytearray()
    offset = 8
    while offset < len(raw):
        size = struct.unpack_from(">I", raw, offset)[0]
        if raw[offset + 4 : offset + 8] == b"IDAT":
            compressed.extend(raw[offset + 8 : offset + 8 + size])
        offset += size + 12
    expected = (stride + 1) * height
    decoder = zlib.decompressobj()
    try:
        rows = decoder.decompress(compressed, expected + 1)
    except zlib.error:
        raise JevError("capture_failed", "Invalid native PNG compressed pixels.") from None
    if len(rows) != expected or not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
        raise JevError("capture_failed", "PNG pixel stream is incomplete or exceeds its bounds.")
    rgba = bytearray()
    previous = bytearray(stride)
    for y in range(height):
        start = y * (stride + 1)
        filtering = rows[start]
        if filtering > 4:
            raise JevError("capture_failed", "Unsupported PNG row filter.")
        row = bytearray(rows[start + 1 : start + 1 + stride])
        for x in range(stride):
            left = row[x - channels] if x >= channels else 0
            up = previous[x]
            upper_left = previous[x - channels] if x >= channels else 0
            if filtering == 1:
                value = left
            elif filtering == 2:
                value = up
            elif filtering == 3:
                value = (left + up) // 2
            elif filtering == 4:
                prediction = left + up - upper_left
                distances = (
                    abs(prediction - left),
                    abs(prediction - up),
                    abs(prediction - upper_left),
                )
                value = (left, up, upper_left)[distances.index(min(distances))]
            else:
                value = 0
            row[x] = (row[x] + value) & 255
        if channels == 4:
            rgba.extend(row)
        else:
            for x in range(0, stride, 3):
                rgba.extend(row[x : x + 3])
                rgba.append(255)
        previous = row
    return bytes(rgba)


def _identity(result: dict) -> dict:
    try:
        identity = SessionIdentity.model_validate(
            {key: result[key] for key in ("project_file", "session_id", "world_path")}
        ).model_dump(exclude_none=True)
        state = ExpectedState.model_validate(
            {key: result[key] for key in ("session_id", "world_path", "revision")}
        ).model_dump()
    except (KeyError, ValueError, TypeError):
        raise JevError("invalid_evidence", "Editor evidence lacks complete identity.") from None
    return {**identity, "revision": state["revision"]}


def _same(left: dict, right: dict, *, revision: bool = True) -> bool:
    return (
        project_identity(left["project_file"]) == project_identity(right["project_file"])
        and all(left[key] == right[key] for key in ("session_id", "world_path"))
        and (not revision or left["revision"] == right["revision"])
    )


def _camera(result: dict) -> dict:
    values = {key: result.get(key) for key in CAMERA_FIELDS}
    vectors = [values["location"], values["rotation"]]
    valid_vectors = all(
        isinstance(vector, list)
        and len(vector) == 3
        and all(type(x) in (int, float) and math.isfinite(x) for x in vector)
        for vector in vectors
    )
    valid_numbers = all(
        type(values[key]) in (int, float) and math.isfinite(values[key])
        for key in ("fov_degrees", "fixed_ev100", "view_mode_index")
    )
    valid_flags = all(
        type(values[key]) is bool
        for key in ("realtime", "realtime_override", "fixed_exposure", "motion_blur")
    )
    if not valid_vectors or not valid_numbers or not valid_flags:
        raise JevError("invalid_evidence", "Native camera settings are incomplete.")
    return values


def _content(result: dict, images: list[str] | None = None) -> CallToolResult:
    payload = {"ok": True, "result": result}
    return CallToolResult(
        content=[TextContent(type="text", text=json.dumps(payload))]
        + [ImageContent(type="image", mimeType="image/png", data=data) for data in images or []],
        structuredContent=payload,
    )


class AcceptanceWorkflows:
    def __init__(self, bridge: UnrealBridge, *, clock=time.monotonic):
        self.bridge, self.clock = bridge, clock
        self.captures: OrderedDict[str, dict] = OrderedDict()
        self.runs: OrderedDict[str, dict] = OrderedDict()
        self.lock = asyncio.Lock()

    def _prune(self):
        now = self.clock()
        for collection in (self.captures, self.runs):
            for key in list(collection):
                if now - collection[key]["created"] >= RETENTION_SECONDS:
                    del collection[key]

    def _get(self, collection: dict, key: str) -> dict:
        self._prune()
        if key not in collection:
            raise JevError("unknown_receipt", "Receipt is unknown, evicted or expired; recapture.")
        return collection[key]

    async def _status(self) -> dict:
        expected = self.bridge.settings.expected_project
        if not expected:
            raise JevError("project_required", "Set JEV_EXPECTED_PROJECT for acceptance evidence.")
        identity = _identity(await self.bridge.call("status"))
        if project_identity(expected) != project_identity(identity["project_file"]):
            raise JevError("wrong_project", "Acceptance evidence belongs to another project.")
        return identity

    async def capture(
        self,
        protocol_id: str,
        expected_state: ExpectedState,
        checks: list[Check],
        max_dimension: int,
        *,
        preserve_id: str | None = None,
    ) -> dict:
        identity = await self._status()
        if any(identity[key] != value for key, value in expected_state.model_dump().items()):
            raise JevError("stale_plan", "Inspect current status before capturing evidence.")
        camera_read = await self.bridge.call("workflow_inspect", {"kind": "camera"})
        if not _same(identity, _identity(camera_read)):
            raise JevError(
                "state_changed", "Camera inspection belongs to a different editor state."
            )
        camera_before = _camera(camera_read)
        native = await self.bridge.call("capture", {"max_dimension": max_dimension})
        capture_content(native)  # Validate image structure, CRCs and the existing size limits.
        raw = base64.b64decode(native["data"], validate=True)
        _rgba(raw)  # Reject malformed supported streams before retaining the receipt.
        if (
            native.get("source") != "editor_viewport"
            or native.get("world_path") != identity["world_path"]
            or native.get("revision") != identity["revision"]
            or native.get("current_camera_location") != camera_before["location"]
            or native.get("current_camera_rotation") != camera_before["rotation"]
            or max(native["width"], native["height"]) > max_dimension
        ):
            raise JevError("state_changed", "Capture no longer matches inspected editor state.")
        verification = (
            await verify_fresh(
                self.bridge,
                checks,
                expected_identity={
                    key: value for key, value in identity.items() if key != "revision"
                },
                expected_revision=identity["revision"],
            )
            if checks
            else {"status": "not_requested", "checks": []}
        )
        camera_read = await self.bridge.call("workflow_inspect", {"kind": "camera"})
        if not _same(identity, _identity(camera_read)):
            raise JevError("state_changed", "Editor state changed during camera inspection.")
        camera_after = _camera(camera_read)
        after = await self._status()
        if not _same(identity, after) or camera_before != camera_after:
            raise JevError(
                "state_changed", "Editor identity, revision or camera changed during capture."
            )
        self._prune()
        while self.captures and (
            len(self.captures) >= MAX_CAPTURES
            or sum(len(row["png"]) for row in self.captures.values()) + len(raw) > MAX_STORED_BYTES
        ):
            victim = next(key for key in self.captures if key != preserve_id)
            del self.captures[victim]
        identifier = str(uuid.uuid4())
        receipt = {
            "receipt_id": identifier,
            "protocol_id": protocol_id,
            "identity": identity,
            "camera": camera_before,
            "max_dimension": max_dimension,
            "width": int(native["width"]),
            "height": int(native["height"]),
            "png_sha256": hashlib.sha256(raw).hexdigest(),
            "byte_count": len(raw),
            "verification": verification,
            "visual_acceptance": "review_required",
            "retention_seconds": RETENTION_SECONDS,
            "scope": SCOPE,
        }
        self.captures[identifier] = {
            "created": self.clock(),
            "receipt": receipt,
            "png": raw,
            "checks": deepcopy(checks),
        }
        return deepcopy(receipt)

    def compare(
        self,
        baseline_id: str,
        candidate_id: str,
        pixel_tolerance: int = 0,
        visual_review: VisualReview | None = None,
    ) -> dict:
        baseline = self._get(self.captures, baseline_id)
        candidate = self._get(self.captures, candidate_id)
        first, second = baseline["receipt"], candidate["receipt"]
        reasons = []
        if baseline_id == candidate_id:
            reasons.append("distinct_captures_required")
        if not _same(first["identity"], second["identity"], revision=False):
            reasons.append("different_project_session_or_world")
        for key in ("protocol_id", "camera", "max_dimension", "width", "height"):
            if first[key] != second[key]:
                reasons.append(f"different_{key}")
        if baseline["checks"] != candidate["checks"]:
            reasons.append("different_checks")
        pixels = {"available": False}
        if not reasons:
            left, right = _rgba(baseline["png"]), _rgba(candidate["png"])
            if left is not None and right is not None:
                differences = [
                    max(abs(a - b) for a, b in zip(left[i : i + 4], right[i : i + 4], strict=True))
                    for i in range(0, len(left), 4)
                ]
                changed = sum(value > pixel_tolerance for value in differences)
                pixels = {
                    "available": True,
                    "metric": "maximum_absolute_rgba_channel_difference",
                    "tolerance": pixel_tolerance,
                    "changed_pixels": changed,
                    "total_pixels": len(differences),
                    "changed_percent": changed / len(differences) * 100,
                    "max_channel_difference": max(differences),
                }
            else:
                pixels["reason"] = "unsupported_png_pixel_format"
        checks_status = second["verification"]["status"]
        visual_status = visual_review.verdict if visual_review else "review_required"
        status = "inconclusive"
        if not reasons:
            if checks_status == "failed" or visual_status == "failed":
                status = "failed"
            elif checks_status in {"passed", "not_requested"} and visual_status == "passed":
                status = "passed_with_declared_visual_review"
        return {
            "baseline_id": baseline_id,
            "candidate_id": candidate_id,
            "status": status,
            "comparable": not reasons,
            "inconclusive_reasons": reasons,
            "pixel_difference": pixels,
            "candidate_checks": second["verification"],
            "baseline_checks_status": first["verification"]["status"],
            "visual_status": visual_status,
            "visual_review": visual_review.model_dump() if visual_review else None,
            "visual_review_source": "caller_attestation" if visual_review else None,
            "automated_visual_quality_proven": False,
            "scope": SCOPE,
        }

    async def playtest_start(
        self, baseline_id: str, test_id: str, expected_state: ExpectedState
    ) -> dict:
        baseline = self._get(self.captures, baseline_id)
        identity = await self._status()
        if not _same(identity, baseline["receipt"]["identity"]) or any(
            identity[key] != value for key, value in expected_state.model_dump().items()
        ):
            raise JevError("stale_plan", "Capture a fresh baseline in this exact editor state.")
        discovery = await self.bridge.call("functional_tests")
        if not _same(identity, _identity(discovery)):
            raise JevError("state_changed", "Editor state changed during test discovery.")
        if discovery.get("enabled") is not True or discovery.get("configuration_valid") is not True:
            raise JevError("functional_disabled", "Project functional testing is disabled.")
        if discovery.get("standalone_pie_ready") is not True:
            raise JevError("pie_required", "An existing standalone PIE session is required.")
        approved = [
            row
            for row in discovery.get("tests", [])
            if isinstance(row, dict)
            and row.get("id") == test_id
            and row.get("editor_actor_available") is True
            and row.get("test_enabled") is True
        ]
        if len(approved) != 1:
            raise JevError("test_not_allowed", "Choose one available project-approved test ID.")
        job = await self.bridge.call(
            "functional_start",
            {
                "test_id": test_id,
                "expected_state": expected_state.model_dump(),
            },
        )
        if not _same(identity, _identity(job)) or job.get("test_id") != test_id:
            raise JevError("invalid_evidence", "Started test returned mismatched identity.")
        job_id = job.get("job_id")
        if not isinstance(job_id, str) or not 1 <= len(job_id) <= 64:
            raise JevError("invalid_evidence", "Started test did not return a bounded job ID.")
        run_id = str(uuid.uuid4())
        self._prune()
        if len(self.runs) >= MAX_RUNS:
            self.runs.popitem(last=False)
        self.runs[run_id] = {
            "created": self.clock(),
            "identity": identity,
            "baseline_id": baseline_id,
            "test_id": test_id,
            "job_id": job_id,
            "final": None,
        }
        return {
            "run_id": run_id,
            "job_id": job_id,
            "test_id": test_id,
            "baseline_id": baseline_id,
            "status": "pending",
            "native_state": job.get("state"),
            "scope": "Explicitly started approved project code; poll once per call. No PIE launch.",
        }

    async def playtest_job(self, run_id: str) -> dict:
        run = self._get(self.runs, run_id)
        if run["final"] is not None:
            return deepcopy(run["final"])
        status = await self._status()
        if not _same(status, run["identity"], revision=False):
            raise JevError("state_changed", "Playtest belongs to another project/session/world.")
        try:
            job = await self.bridge.call("functional_job", {"job_id": run["job_id"]})
        except JevError as exc:
            return {
                "run_id": run_id,
                "job_id": run["job_id"],
                "status": "inconclusive",
                "reason": exc.code,
                "gameplay_acceptance": "unproven",
            }
        if (
            not _same(_identity(job), run["identity"])
            or job.get("job_id") != run["job_id"]
            or job.get("test_id") != run["test_id"]
        ):
            raise JevError("invalid_evidence", "Playtest receipt identity does not match this run.")
        state = job.get("state")
        if state in {"queued", "running"}:
            return {
                "run_id": run_id,
                "job_id": run["job_id"],
                "status": "pending",
                "native_state": state,
                "gameplay_acceptance": "pending",
            }
        passed = (
            state == "passed"
            and job.get("started") is True
            and job.get("cleanup_attempted") is True
            and job.get("native_result") == "Succeeded"
            and type(job.get("observed_error_count")) in (int, float)
            and job["observed_error_count"] == 0
            and job.get("truncated") is False
        )
        result = {
            "run_id": run_id,
            "job_id": run["job_id"],
            "test_id": run["test_id"],
            "status": "inconclusive",
            "native_state": state,
            "gameplay_acceptance": "passed" if passed else "unproven",
            "cleanup_attempted": job.get("cleanup_attempted") is True,
            "native_result": job.get("native_result"),
            "after_capture_id": None,
            "scope": "One approved project test only; editor capture is not a PIE frame. "
            "Terminal receipts are historical. Cleanup attempted does not prove reset.",
        }
        if state in {"failed", "error", "timeout", "timed_out", "cancelled", "interrupted"}:
            result.update(status="failed", gameplay_acceptance="failed")
        try:
            baseline = self._get(self.captures, run["baseline_id"])
            current = await self._status()
            if not _same(current, run["identity"], revision=False):
                raise JevError("state_changed", "World changed after test completion.")
            after = await self.capture(
                baseline["receipt"]["protocol_id"],
                ExpectedState(
                    **{key: current[key] for key in ("session_id", "world_path", "revision")}
                ),
                baseline["checks"],
                baseline["receipt"]["max_dimension"],
                preserve_id=run["baseline_id"],
            )
            comparison = self.compare(run["baseline_id"], after["receipt_id"])
            result.update(after_capture_id=after["receipt_id"], comparison=comparison)
            if (
                passed
                and comparison["comparable"]
                and after["verification"]["status"] in {"passed", "not_requested"}
            ):
                result["status"] = "gameplay_passed_visual_review_required"
            elif after["verification"]["status"] == "failed":
                result["status"] = "failed"
        except JevError as exc:
            result["after_capture_error"] = exc.code
        run["final"] = deepcopy(result)
        return result


def register_acceptance_tools(server: FastMCP, bridge: UnrealBridge) -> AcceptanceWorkflows:
    workflows = AcceptanceWorkflows(bridge)
    read = ToolAnnotations(readOnlyHint=True, destructiveHint=False, openWorldHint=False)
    run = ToolAnnotations(
        readOnlyHint=False, destructiveHint=True, idempotentHint=False, openWorldHint=False
    )

    async def guarded(operation):
        try:
            async with workflows.lock:
                return await operation()
        except JevError as exc:
            error = exc.as_dict()
            return CallToolResult(
                content=[TextContent(type="text", text=json.dumps(error))],
                structuredContent=error,
                isError=True,
            )

    @server.tool(annotations=read, structured_output=False)
    async def unreal_acceptance_capture(
        protocol_id: ProtocolId,
        expected_state: ExpectedState,
        checks: Checks | None = None,
        max_dimension: Annotated[int, Field(ge=64, le=1024, strict=True)] = 512,
    ) -> CallToolResult:
        """Capture a bounded repeatable editor baseline/candidate plus fresh selected checks.

        Returns the native image and an expiring receipt. Requires explicit project and current
        state; observes camera twice, never changes it. Reuse protocol/checks/dimensions for the
        candidate. Images stay in local server memory, but the MCP client can forward to vision.
        """

        async def operation():
            receipt = await workflows.capture(
                protocol_id, expected_state, checks or [], max_dimension
            )
            row = workflows._get(workflows.captures, receipt["receipt_id"])
            return _content(receipt, [base64.b64encode(row["png"]).decode()])

        return await guarded(operation)

    @server.tool(annotations=read, structured_output=False)
    async def unreal_acceptance_compare(
        baseline_id: ReceiptId,
        candidate_id: ReceiptId,
        pixel_tolerance: Annotated[int, Field(ge=0, le=255, strict=True)] = 0,
        visual_review: VisualReview | None = None,
        include_images: bool = False,
    ) -> CallToolResult:
        """Compare two stored captures under matching conditions and fresh-check requirements.

        Optional images are ordered baseline then candidate. RGBA pixel differences flag changes,
        never quality; visual_review is an explicit caller attestation, not automated evidence.
        Different worlds, cameras or protocols are inconclusive. No live editor read occurs.
        """

        async def operation():
            result = workflows.compare(baseline_id, candidate_id, pixel_tolerance, visual_review)
            images = (
                [
                    base64.b64encode(workflows._get(workflows.captures, key)["png"]).decode()
                    for key in (baseline_id, candidate_id)
                ]
                if include_images
                else []
            )
            return _content(result, images)

        return await guarded(operation)

    @server.tool(annotations=run, structured_output=False)
    async def unreal_acceptance_playtest_start(
        baseline_id: ReceiptId,
        test_id: ProtocolId,
        expected_state: ExpectedState,
    ) -> CallToolResult:
        """Explicitly run one named project-approved test against a fresh capture baseline.

        Requires an existing standalone PIE session, native project allowlist and exact current
        state. Test/cleanup callbacks can change project state. Never starts/stops PIE or retries.
        Poll the run ID; native job ID also works with the existing functional cancellation tool.
        """

        async def operation():
            return _content(await workflows.playtest_start(baseline_id, test_id, expected_state))

        return await guarded(operation)

    @server.tool(annotations=read, structured_output=False)
    async def unreal_acceptance_playtest_job(run_id: ReceiptId) -> CallToolResult:
        """Poll an owned test once; terminal evidence includes fresh after-capture/checks.

        Pending, missing, timed-out, failed and uncleaned tests never pass. A successful test still
        needs visual review. After-capture IDs can be compared with include_images=true. Does not
        wait, retry, clean up independently or restart a test; repeated terminal reads are cached.
        """

        async def operation():
            return _content(await workflows.playtest_job(run_id))

        return await guarded(operation)

    return workflows
