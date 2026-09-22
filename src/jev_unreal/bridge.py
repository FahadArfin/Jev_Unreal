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
            delay = self._next_request - time.monotonic()
            if delay > 0:
                await asyncio.sleep(delay)
            self._next_request = time.monotonic() + 0.05
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
            if action in {"preview", "apply", "frame"} and not expected:
                raise JevError(
                    "project_required", "Set JEV_EXPECTED_PROJECT before editing a scene."
                )
            if action == "status":
                return status
            required_capabilities = set()
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
            return await self._call(action, params or {})
