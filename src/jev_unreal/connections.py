"""Read-only checks of explicit profiles, without endpoint scanning or editor switching."""

import os
import stat
from dataclasses import replace
from datetime import UTC, datetime
from pathlib import Path

from . import __version__
from .bridge import UnrealBridge
from .config import Settings
from .errors import JevError
from .profiles import read_profiles, read_token_file
from .team_policy import local_path


def _windows_environment_presence(name: str) -> dict:
    """Query registry value metadata only: never request its credential value bytes."""
    result = {"user": None, "machine": None}
    if os.name != "nt":
        return result
    import ctypes
    import winreg

    for label, hive, path in (
        ("user", winreg.HKEY_CURRENT_USER, "Environment"),
        (
            "machine",
            winreg.HKEY_LOCAL_MACHINE,
            r"SYSTEM\CurrentControlSet\Control\Session Manager\Environment",
        ),
    ):
        try:
            with winreg.OpenKey(hive, path, 0, winreg.KEY_QUERY_VALUE) as key:
                value_type, size = ctypes.c_ulong(), ctypes.c_ulong()
                code = ctypes.windll.advapi32.RegQueryValueExW(
                    ctypes.c_void_p(int(key)),
                    ctypes.c_wchar_p(name),
                    None,
                    ctypes.byref(value_type),
                    None,
                    ctypes.byref(size),
                )
                if code == 0:
                    result[label] = size.value > 2
                elif code == 2:
                    result[label] = False
        except OSError:
            pass
    return result


def credential_diagnostics(
    settings: Settings, *, credential_directory=None, environment=None
) -> dict:
    """Explain source selection without decrypting, hashing, echoing or testing credentials."""
    env = os.environ if environment is None else environment
    variable = "OPENROUTER_API_KEY" if settings.provider == "openrouter" else "TYPESAFE_API_KEY"
    name = "openrouter.dpapi" if settings.provider == "openrouter" else "typesafe.dpapi"
    present = bool(env.get(variable))
    saved = {
        "present": None, "modified_at_utc": None, "contents_read": False,
        "logical_path": None, "resolved_path": None, "redirected": None,
    }
    directory = credential_directory
    if directory is None and env.get("LOCALAPPDATA"):
        directory = Path(env["LOCALAPPDATA"]) / "JevUnreal"
    if directory is not None:
        try:
            path = local_path(str(Path(directory) / name))
            saved["logical_path"] = str(path)
            metadata = path.stat()
            saved.update(
                present=stat.S_ISREG(metadata.st_mode),
                modified_at_utc=datetime.fromtimestamp(metadata.st_mtime, UTC).isoformat(),
            )
            if saved["present"]:
                try:
                    # On Windows this asks for the final handle path, exposing MSIX
                    # redirection without reading any credential contents.
                    resolved = path.resolve(strict=True)
                except OSError:
                    saved["resolution_error"] = "unavailable"
                else:
                    saved.update(
                        resolved_path=str(resolved),
                        redirected=os.path.normcase(str(path)) != os.path.normcase(str(resolved)),
                    )
        except FileNotFoundError:
            saved["present"] = False
        except (OSError, ValueError, JevError):
            saved["error"] = "unavailable_or_unsafe_path"
    marker = env.get("JEV_CREDENTIAL_SOURCE")
    source = "not_loaded"
    marker_used = bool(settings.api_key) and marker in {"saved_dpapi", "process_environment"}
    if marker_used:
        source = marker
    elif settings.api_key:
        source = "process_environment" if present else "settings_supplied"
    issues = []
    if not settings.api_key:
        issues.append(
            {
                "code": "saved_key_not_loaded" if saved["present"] else "provider_key_missing",
                "severity": "info",
                "affects": "optional_cloud_decisions",
                "next_action": "Load the intended key with the configured launcher or provider "
                "environment variable, then reconnect this MCP process. "
                "Local Unreal tools remain usable.",
            }
        )
    if present and saved["present"] and source != "saved_dpapi":
        issues.append(
            {
                "code": "environment_overrides_saved_key",
                "severity": "info",
                "affects": "optional_cloud_decisions",
                "next_action": "The launcher prefers a process environment key over the saved "
                "file. Update that source locally and reconnect; never paste credentials in chat.",
            }
        )
    if saved["redirected"]:
        issues.append(
            {
                "code": "saved_key_path_redirected",
                "severity": "warning",
                "affects": "optional_cloud_decisions",
                "next_action": "The saved credential resolves to a different physical path. "
                "A packaged app can retain an older separate copy. Compare both paths and "
                "timestamps with the setup helper, then correct the intended local "
                "credential source and reconnect.",
            }
        )
    return {
        "provider": settings.provider,
        "environment_variable": variable,
        "environment_presence": {"process": present, **_windows_environment_presence(variable)},
        "loaded": bool(settings.api_key),
        "selected_source": source,
        "source_evidence": "launcher_reported" if marker_used else "local_configuration",
        "saved_file": saved,
        "authentication": "unknown",
        "provider_tested": False,
        "issues": issues,
        "note": "Presence and file timestamps do not prove key validity. A changed saved file "
        "does not replace a key already loaded by a running MCP process. Doctor never probes Jev.",
    }


def connection_issues(settings: Settings, editor: dict) -> list[dict]:
    """Actionable fixed text; never echoes external diagnostics or scans other endpoints."""
    issues = []
    if not settings.expected_project:
        issues.append(
            {
                "code": "project_required",
                "severity": "error",
                "next_action": "Set the intended absolute JEV_EXPECTED_PROJECT or select "
                "one explicit profile before editing.",
            }
        )
    if not editor.get("ready"):
        code = editor.get("error", {}).get("code", "editor_unavailable")
        actions = {
            "wrong_project": "Select the profile for the intended project; do not edit the "
            "other game's editor or change expected identity merely to silence this error.",
            "unauthorized": "Initialize the same local bridge token for this project's editor "
            "and MCP launcher, then reconnect. Do not copy the token into chat.",
            "missing_bridge_token": "Run the local bridge environment helper or select the "
            "explicit profile containing the intended token-file path.",
            "forbidden": "Check loopback binding and the selected bridge token/origin settings.",
            "editor_unavailable": "Build and launch the intended project with JevEditor enabled; "
            "check its configured loopback port. No other ports or projects were scanned.",
        }
        issues.append(
            {
                "code": code if code in actions else "editor_check_failed",
                "severity": "error",
                "next_action": actions.get(
                    code, "Inspect the selected project's bridge configuration and retry doctor."
                ),
            }
        )
    version = editor.get("identity", {}).get("bridge_version")
    if version and version != __version__.split("a")[0]:
        issues.append(
            {
                "code": "bridge_version_mismatch",
                "severity": "warning",
                "next_action": "Build/relaunch the matching plugin and reconnect MCP; "
                "capability checks are reported separately from version metadata.",
            }
        )
    return issues


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
