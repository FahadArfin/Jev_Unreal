"""Authenticated loopback connection to one explicitly identified Unreal editor."""

import asyncio
import json
import os
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
            async with self._http.stream(
                "POST",
                f"{self.settings.bridge_url}/jev/v1/call",
                headers={"Authorization": f"Bearer {self.settings.bridge_token}"},
                json={"action": action, "params": params},
            ) as response:
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
        if action not in {"status", "actors", "assets", "preview", "apply"}:
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
            if action in {"preview", "apply"} and not expected:
                raise JevError(
                    "project_required", "Set JEV_EXPECTED_PROJECT before editing a scene."
                )
            if action == "status":
                return status
            return await self._call(action, params or {})
