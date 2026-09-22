"""Authenticated loopback connection to one explicitly identified Unreal editor."""

import asyncio
import json
import os
import time
from pathlib import PureWindowsPath

import httpx

from .config import Settings
from .errors import JevError


def project_identity(path: str) -> str:
    if "\\" in path or (len(path) > 1 and path[1] == ":"):
        return str(PureWindowsPath(path)).casefold()
    return os.path.normcase(os.path.abspath(path))


class UnrealBridge:
    def __init__(self, settings: Settings, transport: httpx.AsyncBaseTransport | None = None):
        self.settings = settings
        self._lock = asyncio.Lock()
        self._clock = time.monotonic
        self._sleep = asyncio.sleep
        self._next_request = 0.0
        self._http = httpx.AsyncClient(
            transport=transport, timeout=15, trust_env=False, follow_redirects=False
        )

    async def close(self):
        await self._http.aclose()

    async def _call(self, action: str, params: dict) -> dict:
        if len(self.settings.bridge_token) < 32:
            raise JevError(
                "missing_bridge_token", "Configure a bridge token of at least 32 characters."
            )
        try:
            # Native bridge accepts 30 authenticated requests/second, including identity reads.
            # Keep this client's bursts below that ceiling without retrying any operation.
            while (delay := self._next_request - self._clock()) > 0:
                await self._sleep(delay)
            try:
                async with self._http.stream(
                    "POST",
                    f"{self.settings.bridge_url}/jev/v1/call",
                    headers={"Authorization": f"Bearer {self.settings.bridge_token}"},
                    json={"action": action, "params": params},
                ) as response:
                    if response.status_code == 429:
                        raise JevError(
                            "rate_limited", "Editor request limit reached; no retry was made."
                        )
                    if response.status_code in {401, 403}:
                        code = "unauthorized" if response.status_code == 401 else "forbidden"
                        raise JevError(code, "The editor rejected bridge authentication or origin.")
                    if response.status_code != 200:
                        raise JevError(
                            "bridge_error", f"Editor bridge returned HTTP {response.status_code}."
                        )
                    content = bytearray()
                    async for chunk in response.aiter_bytes():
                        content.extend(chunk)
                        if len(content) > 1048576:
                            raise JevError("bridge_error", "Editor response exceeds 1 MiB.")
            finally:
                # Include transport/body/close time and failed attempts in the interval.
                # Cancellation during the preceding wait makes no new HTTP attempt.
                self._next_request = self._clock() + 0.05
            payload = json.loads(content)
            if not isinstance(payload, dict):
                raise ValueError
            if payload.get("ok") is not True:
                code = payload.get("error", {}).get("code", "bridge_error")
                known = {
                    "bad_request",
                    "unauthorized",
                    "forbidden",
                    "stale_plan",
                    "unknown_plan",
                    "expired_plan",
                    "play_mode",
                    "editor_unavailable",
                    "apply_failed",
                    "actor_not_found",
                    "actor_locked",
                    "asset_unavailable",
                    "too_many_plans",
                    "unknown_action",
                    "rate_limited",
                    "level_locked",
                    "actor_unsupported",
                    "asset_not_found",
                    "asset_unsupported",
                    "viewport_unavailable",
                    "capture_failed",
                    "capture_too_large",
                    "response_too_large",
                    "actor_bounds_unavailable",
                    "viewport_locked",
                    "editor_busy",
                    "rollback_failed",
                    "material_slot_invalid",
                    "asset_not_loaded",
                    "unsupported_asset",
                    "validation_disabled",
                    "validation_rule_unavailable",
                    "rule_not_allowed",
                    "rule_unavailable",
                    "wrong_project",
                    "validation_busy",
                    "unknown_job",
                    "job_busy",
                    "policy_invalid",
                    "functional_disabled",
                    "test_not_allowed",
                    "pie_required",
                    "test_unavailable",
                    "blueprint_compile_disabled",
                    "target_not_allowed",
                    "compile_failed",
                    "plan_consumed",
                }
                if code not in known:
                    code = "bridge_error"
                raise JevError(code, f"Editor rejected the operation ({code}).")
            if not isinstance(payload.get("result"), dict):
                raise ValueError
            return payload["result"]
        except httpx.HTTPError:
            raise JevError(
                "editor_unavailable", "Cannot reach the authenticated Unreal bridge."
            ) from None
        except (ValueError, TypeError, AttributeError, RecursionError):
            raise JevError("bridge_error", "Editor returned an invalid response.") from None

    async def call(self, action: str, params: dict | None = None) -> dict:
        if action not in {
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
            "actor_details",
            "pending_plans",
            "plan_status",
            "blueprint_inspect",
            "blueprint_compile_targets",
            "blueprint_compile_preview",
            "blueprint_compile",
            "blueprint_compile_receipt",
            "asset_dependencies",
            "asset_import_info",
            "validation_rules",
            "validation_start",
            "validation_job",
            "validation_cancel",
            "functional_tests",
            "functional_start",
            "functional_job",
            "functional_cancel",
        }:
            raise JevError("unknown_action", "Operation is not part of the editor allowlist.")
        async with self._lock:
            status = await self._call("status", {})
            expected = self.settings.expected_project
            actual = status.get("project_file")
            if not isinstance(actual, str) or not actual:
                raise JevError("bridge_error", "Editor did not return its project identity.")
            if expected and project_identity(expected) != project_identity(actual):
                raise JevError(
                    "wrong_project", "Connected editor does not match JEV_EXPECTED_PROJECT."
                )
            if (
                action
                in {
                    "preview",
                    "apply",
                    "frame",
                    "validation_start",
                    "validation_cancel",
                    "functional_start",
                    "functional_cancel",
                    "blueprint_compile_preview",
                    "blueprint_compile",
                }
                and not expected
            ):
                raise JevError(
                    "project_required", "Set JEV_EXPECTED_PROJECT before editing a scene."
                )
            if action == "status":
                return status
            required_capabilities = set()
            if action in {
                "pending_plans",
                "plan_status",
                "blueprint_inspect",
                "blueprint_compile_targets",
                "blueprint_compile_preview",
                "blueprint_compile",
                "blueprint_compile_receipt",
                "asset_dependencies",
                "asset_import_info",
                "validation_rules",
                "validation_start",
                "validation_job",
                "validation_cancel",
                "functional_tests",
                "functional_start",
                "functional_job",
                "functional_cancel",
            }:
                required_capabilities.add(action)
            if action == "actor_details":
                required_capabilities.add("actor_details")
            if action == "frame" and params and "view" in params:
                required_capabilities.add("frame_views")
            if action == "preview" and params:
                if params.get("expected_state") is not None:
                    required_capabilities.add("preview_expected_state")
                for operation in params.get("operations", []):
                    if isinstance(operation, dict) and operation.get("op") in {
                        "set_material",
                        "set_metadata",
                        "replace_mesh",
                        "duplicate_mesh",
                    }:
                        required_capabilities.add(operation["op"])
            capabilities = status.get("capabilities", [])
            if required_capabilities and (
                not isinstance(capabilities, list)
                or not required_capabilities
                <= {item for item in capabilities if isinstance(item, str)}
            ):
                raise JevError(
                    "capability_unavailable",
                    "The connected editor lacks this capability. "
                    "Rebuild/relaunch the matching JevEditor plugin.",
                )
            if action == "validation_start":
                # Bind the native mutation to this authenticated status response,
                # not just an earlier client-side project check. A restarted editor
                # can reuse an endpoint/token between these two HTTP requests.
                state = {}
                for name, maximum in (("session_id", 64), ("world_path", 1024), ("revision", 128)):
                    value = status.get(name)
                    if (
                        not isinstance(value, str)
                        or not 1 <= len(value) <= maximum
                        or any(ord(character) < 32 for character in value)
                    ):
                        raise JevError(
                            "bridge_error",
                            "Editor did not return a valid state for project validation.",
                        )
                    state[name] = value
                if len(actual) > 2048 or any(ord(character) < 32 for character in actual):
                    raise JevError("bridge_error", "Editor returned an invalid project identity.")
                params = {
                    **(params or {}),
                    "expected_project": actual,
                    "expected_state": state,
                }
            if action in {"blueprint_compile_preview", "blueprint_compile"}:
                if len(actual) > 2048 or any(ord(character) < 32 for character in actual):
                    raise JevError("bridge_error", "Editor returned an invalid project identity.")
                params = {**(params or {}), "expected_project": actual}
            return await self._call(action, params or {})
