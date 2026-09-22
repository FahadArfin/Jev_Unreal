"""Real two-editor MCP acceptance, restricted to two repository-owned sandboxes.

The caller must launch both editors and configure the installed sandbox's native
validator aliases first. This script never launches/stops editors or saves maps.
Run: uv run python scripts/smoke_connections.py --profiles artifacts/live-profiles.json
"""

import argparse
import asyncio
import json
import os
import sys
import time
import uuid
from contextlib import AsyncExitStack
from pathlib import Path
from urllib.parse import urlparse

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

from jev_unreal.bridge import UnrealBridge, project_identity
from jev_unreal.config import Settings
from jev_unreal.errors import JevError
from jev_unreal.profiles import ConnectionProfile, read_profiles, read_token_file

ROOT = Path(__file__).resolve().parents[1]
PROJECTS = {
    "primary": ROOT / "examples/JevSandbox/JevSandbox.uproject",
    "installed": ROOT / "artifacts/install-acceptance/JevSandbox/JevSandbox.uproject",
}
REPORT = ROOT / "artifacts/connections-smoke-v0.5.json"
CUBE = "/Engine/BasicShapes/Cube.Cube"
RULES = {
    "localization": "/Script/DataValidation.EditorValidator_Localization",
    "material": "/Script/DataValidation.EditorValidator_Material",
}


def require(condition, message: str):
    if not condition:
        raise RuntimeError(message)


def restricted_profiles(path: Path) -> dict[str, ConnectionProfile]:
    profiles = {profile.id: profile for profile in read_profiles(path)}
    require(set(profiles) == set(PROJECTS), "Use exactly primary and installed sandbox profiles.")
    for name, profile in profiles.items():
        require(
            project_identity(profile.project_file) == project_identity(str(PROJECTS[name])),
            "Connection smoke refuses any project outside its two exact repository sandboxes.",
        )
        require(PROJECTS[name].is_file(), "Both sandbox project files must exist before this test.")
    require(
        profiles["primary"].bridge_url != profiles["installed"].bridge_url,
        "The two sandbox profiles must use different loopback ports.",
    )
    return profiles


async def connect(stack: AsyncExitStack, path: Path, profile: ConnectionProfile) -> ClientSession:
    # Deliberately invalid legacy fields prove atomic profile selection. A launcher
    # that mixes any of these with the selected profile cannot pass authentication.
    environment = {
        **os.environ,
        "JEV_PROFILES_FILE": str(path),
        "JEV_PROFILE": profile.id,
        "JEV_EXPECTED_PROJECT": str(ROOT / "artifacts/SHOULD_NOT_CONNECT.uproject"),
        "JEV_BRIDGE_URL": "http://127.0.0.1:1",
        "JEV_BRIDGE_PORT": "invalid-legacy-port",
        "JEV_BRIDGE_TOKEN": "deliberately-wrong-legacy-token-000000000",
        "JEV_BRIDGE_TOKEN_FILE": str(ROOT / "artifacts/SHOULD_NOT_READ.token"),
        "OPENROUTER_API_KEY": "",
        "TYPESAFE_API_KEY": "",
        "JEV_PROVIDER": "openrouter",
        "JEV_MODEL": "typesafe/jev-1.13",
        "JEV_MAX_REQUESTS": "1",
        "JEV_CATALOG_FILE": "",
    }
    parameters = StdioServerParameters(
        command=sys.executable, args=["-m", "jev_unreal", "serve"], env=environment
    )
    read, write = await stack.enter_async_context(stdio_client(parameters))
    session = await stack.enter_async_context(ClientSession(read, write))
    await session.initialize()
    tools = (await session.list_tools()).tools
    require(len(tools) == 40, "Each selected MCP server must expose the matching 40-tool catalog.")
    return session


async def call(session: ClientSession, name: str, arguments=None, *, error=None) -> dict:
    response = await session.call_tool(name, arguments or {})
    require(not response.isError, f"MCP protocol/tool error while calling {name}.")
    data = response.structuredContent
    require(isinstance(data, dict), f"Missing structured response from {name}.")
    if error:
        require(data.get("ok") is False, f"{name} unexpectedly succeeded instead of {error}.")
        require(data.get("error", {}).get("code") == error, f"{name} did not report {error}.")
        return data
    require(
        data.get("ok") is True, f"{name} failed: {data.get('error', {}).get('code', 'unknown')}."
    )
    require(isinstance(data.get("result"), dict), f"Missing result from {name}.")
    return data["result"]


async def context(session: ClientSession, project: Path) -> dict:
    result = await call(session, "unreal_context", {"limit": 200})
    require(
        project_identity(result.get("project_file", "")) == project_identity(str(project)),
        "Connected editor changed project identity.",
    )
    require(not result.get("actor_scan_incomplete", True), "Sandbox actor scan must be complete.")
    require(not result.get("play_in_editor"), "Stop Play In Editor before this connection test.")
    require(not result.get("simulating"), "Stop Simulate before this connection test.")
    return result


def same_scene(before: dict, after: dict):
    require(before["session_id"] == after["session_id"], "Editor restarted during acceptance.")
    require(before["world_path"] == after["world_path"], "Editor world changed during acceptance.")
    require(
        before["revision"] == after["revision"], "Rejected or preview-only call changed the scene."
    )
    require(
        before["examined_actors"] == after["examined_actors"], "Actor count changed unexpectedly."
    )


async def spawn_preview(session: ClientSession, label: str) -> dict:
    return await call(
        session,
        "unreal_preview",
        {
            "operations": [
                {
                    "op": "spawn_primitive",
                    "shape": "Cube",
                    "label": label,
                    "location": [9500, 0, 75],
                    "scale": [1, 1, 1.5],
                }
            ]
        },
    )


async def apply_and_verify(session: ClientSession, plan: dict, label: str, identity: dict) -> dict:
    applied = await call(session, "unreal_apply", {"plan_id": plan["plan_id"]})
    require(applied.get("applied") is True, "Own plan did not confirm application.")
    require(applied.get("verification", {}).get("status") == "passed", "Apply readback failed.")
    require(len(applied["actors"]) == 1, "Expected exactly one sandbox cube.")
    actor_path = applied["actors"][0]["path"]
    inspected = await call(session, "unreal_actor_details", {"actor_paths": [actor_path]})
    actor = inspected["actors"][0]
    verified = await call(
        session,
        "unreal_verify",
        {
            "checks": [
                {
                    "kind": "label",
                    "actor_path": actor_path,
                    "expected": label,
                    "expected_instance_id": actor["instance_id"],
                }
            ],
            "expected_identity": {
                key: identity[key] for key in ("project_file", "session_id", "world_path")
            },
        },
    )
    require(verified["status"] == "passed", "Fresh actor label/identity verification failed.")
    await call(session, "unreal_apply", {"plan_id": plan["plan_id"]}, error="unknown_plan")
    receipt = await call(session, "unreal_plan", {"plan_id": plan["plan_id"]})
    require(receipt["status"] == "applied", "Replay altered the native success receipt.")
    return {
        "plan_id": plan["plan_id"],
        "actor_path": actor_path,
        "label": label,
        "fresh_verification": verified["status"],
        "native_receipt_status": receipt["status"],
        "replay": "unknown_plan",
    }


async def validator_job(session: ClientSession, alias: str, expected: str) -> dict:
    started = await call(
        session,
        "unreal_validation_start",
        {
            "rule_ids": [alias],
            "asset_paths": [CUBE],
        },
    )
    job_id = started["job_id"]
    try:
        async with asyncio.timeout(30):
            result = started
            while result["state"] in {"queued", "running"}:
                await asyncio.sleep(0.15)
                result = await call(session, "unreal_validation_job", {"job_id": job_id})
    except TimeoutError:
        # Cancel only this test's job, once; do not leave pending validator work.
        async with asyncio.timeout(5):
            await call(session, "unreal_validation_cancel", {"job_id": job_id})
        raise RuntimeError(
            "Validation exceeded this smoke test's 30-second polling budget."
        ) from None
    require(result["state"] == "completed", f"{alias} validation did not complete.")
    require(result["verdict"] == expected, f"{alias} returned an unexpected validation verdict.")
    require(result["completed"] == result["total"] == 1, "Expected one asset/rule result.")
    require(len(result["results"]) == 1, "Expected one native validation evidence row.")
    row = result["results"][0]
    require(row["asset_path"] == CUBE and row["rule_id"] == alias, "Validation evidence mismatch.")
    require(row["result"] == expected, "Native row disagrees with the expected overall verdict.")
    # Do not persist arbitrary project log text or provider payloads.
    return {
        "job_id": job_id,
        "rule_id": alias,
        "asset_path": CUBE,
        "state": result["state"],
        "verdict": result["verdict"],
        "completed": result["completed"],
        "total": result["total"],
        "elapsed_seconds": result["elapsed_seconds"],
        "truncated": result["truncated"],
        "error_count": row["error_count"],
        "warning_count": row["warning_count"],
        "loaded_for_validation": row["loaded_for_validation"],
        "package_dirty_before": row["package_dirty_before"],
        "package_dirty_after": row["package_dirty_after"],
        "save_requested": False,
    }


async def run(profiles_path: Path):
    profiles_path = await asyncio.to_thread(profiles_path.absolute)
    profiles = await asyncio.to_thread(restricted_profiles, profiles_path)
    report = {
        "passed": False,
        "checks": [],
        "editors": {},
        "validation_jobs": [],
        "provider_requests": 0,
        "cloud_used": False,
        "save_requested": False,
        "scope": "Two explicitly restricted repository sandboxes; unsaved cubes only.",
    }
    began = time.monotonic()
    try:
        async with AsyncExitStack() as stack:
            sessions = {}
            initial = {}
            # Keep all Unreal requests sequential, including across the two editors.
            for name in ("primary", "installed"):
                sessions[name] = await connect(stack, profiles_path, profiles[name])
                initial[name] = await context(sessions[name], PROJECTS[name])
                status = initial[name]
                require(status["bridge_version"] == "0.5.0", "Rebuild both native plugins for 0.5.")
                require(
                    {"plan_status", "validation_start", "validation_job"}
                    <= set(status["capabilities"]),
                    "Missing matching native workflow capabilities.",
                )
                report["editors"][name] = {
                    "project_file": status["project_file"],
                    "bridge_url": profiles[name].bridge_url,
                    "port": urlparse(profiles[name].bridge_url).port,
                    "session_id": status["session_id"],
                    "engine_version": status["engine_version"],
                    "bridge_version": status["bridge_version"],
                    "mcp_tools": 40,
                    "initial_actor_count": status["examined_actors"],
                }
            require(
                initial["primary"]["session_id"] != initial["installed"]["session_id"],
                "Two distinct native editor sessions are required.",
            )
            report["checks"].append(
                "two_profile_connections_override_conflicting_legacy_environment"
            )
            primary, installed = sessions["primary"], sessions["installed"]
            prefix = "JevConnection_" + uuid.uuid4().hex[:12]
            primary_label, installed_label = prefix + "_Primary", prefix + "_Installed"
            alpha_plan = await spawn_preview(primary, primary_label)
            same_scene(initial["primary"], await context(primary, PROJECTS["primary"]))
            await call(
                installed, "unreal_apply", {"plan_id": alpha_plan["plan_id"]}, error="unknown_plan"
            )
            same_scene(initial["installed"], await context(installed, PROJECTS["installed"]))
            same_scene(initial["primary"], await context(primary, PROJECTS["primary"]))
            report["checks"].append("cross_editor_plan_rejected_with_both_actor_counts_unchanged")
            report["editors"]["primary"]["applied"] = await apply_and_verify(
                primary, alpha_plan, primary_label, initial["primary"]
            )
            beta_plan = await spawn_preview(installed, installed_label)
            same_scene(initial["installed"], await context(installed, PROJECTS["installed"]))
            report["editors"]["installed"]["applied"] = await apply_and_verify(
                installed, beta_plan, installed_label, initial["installed"]
            )
            report["checks"].append("own_plans_apply_once_and_fresh_label_instance_checks_pass")

            # Intentionally bypass profiles in this one read-only negative test to
            # prove native endpoint authentication does not replace project binding.
            wrong = UnrealBridge(
                Settings(
                    bridge_url=profiles["primary"].bridge_url,
                    bridge_token=await asyncio.to_thread(
                        read_token_file, profiles["primary"].bridge_token_file
                    ),
                    expected_project=profiles["installed"].project_file,
                )
            )
            try:
                try:
                    await wrong.call("context")
                except JevError as exc:
                    require(exc.code == "wrong_project", "Mismatched binding failed unexpectedly.")
                else:
                    raise RuntimeError(
                        "A mismatched project binding was allowed to read the editor."
                    )
            finally:
                await wrong.close()
            report["checks"].append("authenticated_endpoint_with_wrong_project_refuses_read")

            rules = await call(installed, "unreal_validation_rules")
            require(
                rules["enabled"] and rules["configuration_valid"],
                "Enable the two documented validator aliases in the installed sandbox first.",
            )
            configured = {row["id"]: row for row in rules["rules"]}
            for alias, class_path in RULES.items():
                require(
                    alias in configured
                    and configured[alias]["available"]
                    and configured[alias]["class_path"] == class_path,
                    "Installed sandbox validation policy does not match the intended native rules.",
                )
            report["validation_jobs"].append(
                await validator_job(installed, "localization", "valid")
            )
            report["validation_jobs"].append(
                await validator_job(installed, "material", "not_validated")
            )
            report["checks"].append("real_native_selected_validation_valid_and_not_validated")
            for name, session in sessions.items():
                final = await context(session, PROJECTS[name])
                require(
                    final["examined_actors"] == initial[name]["examined_actors"] + 1,
                    "Only one smoke cube should have been added to each sandbox.",
                )
                report["editors"][name]["final_actor_count"] = final["examined_actors"]
                metrics = await session.call_tool("jev_status", {})
                require(
                    not metrics.isError and isinstance(metrics.structuredContent, dict),
                    "Provider counters were unavailable.",
                )
                decisions = metrics.structuredContent["decisions"]
                require(
                    decisions["requests_sent"] == 0 and not decisions["configured"],
                    "Connection smoke must make no provider request or load provider credentials.",
                )
            report["checks"].append("no_provider_credentials_or_requests_and_no_save_requested")
            report["passed"] = True
    finally:
        report["elapsed_seconds"] = round(time.monotonic() - began, 3)
        REPORT.parent.mkdir(parents=True, exist_ok=True)
        REPORT.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2, allow_nan=False))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profiles", type=Path, required=True)
    options = parser.parse_args()
    asyncio.run(run(options.profiles))
