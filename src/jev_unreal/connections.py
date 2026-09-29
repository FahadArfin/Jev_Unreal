"""Read-only checks of explicit profiles, without endpoint scanning or editor switching."""

from dataclasses import replace

from . import __version__
from .bridge import UnrealBridge
from .config import Settings
from .errors import JevError
from .profiles import read_profiles, read_token_file


async def check_profiles(path, bridge_factory=UnrealBridge) -> dict:
    rows = []
    for profile in read_profiles(path):
        bridge = None
        try:
            config = replace(
                Settings(),
                expected_project=profile.project_file,
                bridge_url=profile.bridge_url,
                profile_id=profile.id,
                bridge_token=read_token_file(profile.bridge_token_file),
            )
            bridge = bridge_factory(config)
            result = await bridge.call("status")
            version = result.get("bridge_version", "")
            expected = __version__.split("a")[0]
            compatible = version == expected
            rows.append(
                {
                    "profile": profile.id,
                    "status": "connected",
                    "expected_project": profile.project_file,
                    "session_id": result.get("session_id"),
                    "bridge_version": version,
                    "server_version": __version__,
                    "versions_match": compatible,
                    "next_step": "Ready for project capability checks."
                    if compatible
                    else "Build/relaunch the matching plugin, then reconnect this MCP profile.",
                }
            )
        except JevError as exc:
            rows.append(
                {
                    "profile": profile.id,
                    "status": "unavailable",
                    "error_code": exc.code,
                    "next_step": "Check the selected project, endpoint and local token; "
                    "never switch projects automatically.",
                }
            )
        finally:
            if bridge:
                await bridge.close()
    return {
        "profiles": rows,
        "all_connected": all(r["status"] == "connected" for r in rows),
        "scope": "Authenticated explicit profiles only; no edits, provider calls or scans.",
    }
