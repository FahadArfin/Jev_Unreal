"""Real stdio reconnect and project-inspection acceptance in the disposable sandbox only."""

import asyncio
import json
import os
import sys
import time
from pathlib import Path

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

from jev_unreal.bridge import project_identity
from jev_unreal.config import Settings

ROOT = Path(__file__).resolve().parents[1]
SANDBOX = ROOT / "examples/JevSandbox/JevSandbox.uproject"


async def call(session, name, args=None, expected_error=None):
    response = await session.call_tool(name, args or {})
    assert not response.isError, response.content
    payload = response.structuredContent
    assert isinstance(payload, dict), response.content
    if expected_error:
        assert payload.get("error", {}).get("code") == expected_error, payload
        return payload
    assert payload.get("ok"), payload
    return payload["result"]


async def main():
    settings = Settings.from_env()
    assert settings.expected_project and project_identity(
        settings.expected_project
    ) == project_identity(str(SANDBOX)), "Only this repository's disposable sandbox is allowed"
    params = StdioServerParameters(
        command=sys.executable,
        args=["-m", "jev_unreal", "serve"],
        env={**os.environ, "OPENROUTER_API_KEY": "", "TYPESAFE_API_KEY": ""},
    )
    label = f"JevReconnect_{int(time.time())}"
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()
            tools = {tool.name for tool in (await session.list_tools()).tools}
            assert len(tools) == 40, tools
            status = await call(session, "unreal_status")
            assert status["bridge_version"] == "0.5.0"
            assert project_identity(status["project_file"]) == project_identity(str(SANDBOX))
            dependencies = await call(
                session,
                "unreal_asset_dependencies",
                {
                    "asset_path": "/Engine/BasicShapes/Cube.Cube",
                    "limit": 1,
                },
            )
            provenance = await call(
                session,
                "unreal_asset_import_info",
                {
                    "asset_path": "/Engine/BasicShapes/Cube.Cube",
                },
            )
            await call(
                session,
                "unreal_blueprint_inspect",
                {
                    "asset_path": "/Engine/BasicShapes/Cube.Cube",
                },
                "unsupported_asset",
            )
            rules = await call(session, "unreal_validation_rules")
            assert rules["enabled"] is False
            await call(
                session,
                "unreal_validation_start",
                {
                    "rule_ids": ["unapproved"],
                    "asset_paths": ["/Engine/BasicShapes/Cube.Cube"],
                },
                "validation_disabled",
            )
            tests = await call(session, "unreal_functional_tests")
            assert tests["enabled"] is False
            await call(
                session,
                "unreal_functional_start",
                {
                    "test_id": "unapproved",
                    "expected_state": {
                        k: status[k] for k in ("session_id", "world_path", "revision")
                    },
                },
                "functional_disabled",
            )
            proposal = await call(
                session,
                "unreal_preview",
                {
                    "operations": [
                        {
                            "op": "spawn_primitive",
                            "shape": "Cube",
                            "label": label,
                            "location": [8000, 0, 50],
                        }
                    ]
                },
            )
            pending = await call(session, "unreal_pending_plans")
            assert proposal["plan_id"] in {row["plan_id"] for row in pending["plans"]}
            receipt = await call(session, "unreal_plan", {"plan_id": proposal["plan_id"]})
            assert receipt["scope"] == "editor_session_memory" and receipt["status"] == "pending"
            applied = await call(session, "unreal_apply", {"plan_id": proposal["plan_id"]})
            assert applied["verification"]["status"] == "passed", applied
            actor = applied["actors"][0]
    # New MCP process: all Python preview/journal/snapshot caches have disappeared.
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()
            reconnected = await call(session, "unreal_status")
            assert reconnected["session_id"] == status["session_id"]
            receipt = await call(session, "unreal_plan", {"plan_id": proposal["plan_id"]})
            assert receipt["status"] == "applied" and receipt["executed"] is True, receipt
            assert receipt["scope"] == "editor_session_memory", receipt
            assert "native_lookup_error" not in receipt
            verified = await call(
                session,
                "unreal_verify",
                {
                    "checks": [
                        {
                            "kind": "label",
                            "actor_path": actor["path"],
                            "expected": label,
                        }
                    ]
                },
            )
            assert verified["status"] == "passed", verified
            # Never replay applied plans as recovery; inspection is sufficient here.
    evidence = {
        "tool_count": len(tools),
        "engine_version": status["engine_version"],
        "bridge_version": status["bridge_version"],
        "project_file": status["project_file"],
        "session_id": status["session_id"],
        "dependencies": dependencies,
        "import_provenance": provenance,
        "native_receipt_after_stdio_restart": receipt,
        "fresh_verification": verified,
        "default_validation_policy_refused": True,
        "default_functional_policy_refused": True,
        "cloud_requests": 0,
        "saved": False,
        "blueprint_fixture_coverage": "Native automation, separate from this smoke",
        "functional_execution_coverage": "Native PIE automation, separate from this smoke",
    }
    destination = ROOT / "artifacts/roadmap-smoke-v0.5.json"
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"ok": True, "tools": len(tools), "reconnect_verified": True}))


if __name__ == "__main__":
    asyncio.run(main())
