"""Exercise the full stdio MCP -> authenticated Unreal bridge in the isolated sandbox.

This intentionally creates unsaved test actors. It refuses any other project.
Run with JEV_BRIDGE_TOKEN_FILE and JEV_EXPECTED_PROJECT set. No provider key needed.
"""

import asyncio
import json
import os
import sys
import time
from pathlib import Path

import httpx
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

from jev_unreal.bridge import project_identity
from jev_unreal.config import Settings

SANDBOX = Path(__file__).resolve().parents[1] / "examples/JevSandbox/JevSandbox.uproject"


async def main():
    settings = Settings.from_env()
    sandbox = SANDBOX
    assert settings.expected_project, "Set JEV_EXPECTED_PROJECT"
    assert project_identity(settings.expected_project) == project_identity(str(sandbox)), (
        "Smoke tests are restricted to this repository's JevSandbox"
    )
    async with httpx.AsyncClient(trust_env=False, timeout=15) as client:
        unauthorized = await client.post(
            settings.bridge_url + "/jev/v1/call", json={"action": "status", "params": {}}
        )
        assert unauthorized.status_code == 401
        browser = await client.post(
            settings.bridge_url + "/jev/v1/call",
            headers={
                "Authorization": f"Bearer {settings.bridge_token}",
                "Origin": "http://evil.test",
            },
            json={"action": "status", "params": {}},
        )
        assert browser.status_code == 403
    env = dict(os.environ)
    env["OPENROUTER_API_KEY"] = ""
    env["TYPESAFE_API_KEY"] = ""
    params = StdioServerParameters(
        command=sys.executable, args=["-m", "jev_unreal", "serve"], env=env
    )
    prefix = f"JevSmoke_{int(time.time())}"
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()

            async def call(name, arguments, code=None):
                response = await session.call_tool(name, arguments)
                assert not response.isError, response.content
                data = response.structuredContent
                assert isinstance(data, dict)
                if code:
                    assert data["error"]["code"] == code, data
                    return data
                assert data["ok"], data
                return data["result"]

            status = await call("unreal_status", {})
            assert project_identity(status["project_file"]) == project_identity(str(sandbox))
            before = await call("unreal_actors", {"query": prefix})
            assert before["actors"] == []
            plan = await call(
                "unreal_preview",
                {
                    "operations": [
                        {
                            "op": "spawn_primitive",
                            "shape": "Cube",
                            "label": prefix + "_Cube",
                            "location": [0, 0, 50],
                            "scale": [2, 2, 1],
                        },
                        {
                            "op": "spawn_primitive",
                            "shape": "Sphere",
                            "label": prefix + "_Sphere",
                            "location": [300, 0, 100],
                        },
                    ]
                },
            )
            still_empty = await call("unreal_actors", {"query": prefix})
            assert still_empty["actors"] == [], "Preview mutated the scene"
            applied = await call("unreal_apply", {"plan_id": plan["plan_id"]})
            assert applied["applied"] and len(applied["actors"]) == 2
            await call("unreal_apply", {"plan_id": plan["plan_id"]}, "unknown_plan")
            actor = applied["actors"][0]
            pending = await call(
                "unreal_preview",
                {
                    "operations": [
                        {
                            "op": "set_transform",
                            "actor_path": actor["path"],
                            "location": [100, 50, 60],
                        },
                    ]
                },
            )
            competing = await call(
                "unreal_preview",
                {
                    "operations": [
                        {
                            "op": "set_transform",
                            "actor_path": actor["path"],
                            "location": [500, 0, 100],
                        },
                    ]
                },
            )
            moved = await call("unreal_apply", {"plan_id": pending["plan_id"]})
            assert moved["actors"][0]["location"] == [100, 50, 60]
            await call("unreal_apply", {"plan_id": competing["plan_id"]}, "stale_plan")
            observed = await call("unreal_actors", {"query": prefix})
            assert len(observed["actors"]) == 2
            limited = await call("unreal_actors", {"query": prefix, "limit": 1})
            assert len(limited["actors"]) == 1 and limited["truncated"]
            await call("unreal_assets", {"path": "/Game", "limit": 5})
            report = {
                "passed": True,
                "engine_version": status["engine_version"],
                "mcp_tools": len((await session.list_tools()).tools),
                "checks": [
                    "auth_required",
                    "browser_rejected",
                    "project_identity",
                    "preview_no_mutation",
                    "batch_spawn",
                    "transform",
                    "one_shot_plan",
                    "stale_plan",
                    "bounded_inspection",
                    "asset_registry",
                ],
                "test_actors": [a["label"] for a in observed["actors"]],
                "saved_to_disk": False,
            }
            print(json.dumps(report, indent=2))


if __name__ == "__main__":
    asyncio.run(main())
