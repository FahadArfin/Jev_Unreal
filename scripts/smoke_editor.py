"""Exercise the full stdio MCP -> authenticated Unreal bridge in the isolated sandbox.

This intentionally creates unsaved test actors. It refuses any other project.
Run with JEV_BRIDGE_TOKEN_FILE and JEV_EXPECTED_PROJECT set. No provider key needed.
"""

import argparse
import asyncio
import base64
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


async def main(require_capture: bool = False):
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
            assert applied["verification"]["status"] == "passed", applied
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
            assert moved["verification"]["status"] == "passed", moved
            await call("unreal_apply", {"plan_id": competing["plan_id"]}, "stale_plan")
            observed = await call("unreal_actors", {"query": prefix})
            assert len(observed["actors"]) == 2
            limited = await call("unreal_actors", {"query": prefix, "limit": 1})
            assert len(limited["actors"]) == 1 and limited["truncated"]
            await call("unreal_assets", {"path": "/Game", "limit": 5})
            context = await call("unreal_context", {"query": prefix})
            assert context["project_file"] == status["project_file"]
            assert len(context["actors"]) == 2
            mesh = await call("unreal_asset_details", {"path": "/Engine/BasicShapes/Cube.Cube"})
            assert mesh["static_mesh"]["bounds_cm"]["size"] == [100, 100, 100], mesh
            validation = await call("unreal_validate", {"query": prefix})
            assert validation["scanned_actors"] == 2
            assert validation["scan_incomplete"] is False
            mesh_plan = await call(
                "unreal_preview",
                {
                    "operations": [
                        {
                            "op": "spawn_static_mesh",
                            "asset_path": "/Engine/BasicShapes/Cube.Cube",
                            "label": prefix + "_Asset",
                            "location": [500, 300, 50],
                        }
                    ]
                },
            )
            mesh_result = await call("unreal_apply", {"plan_id": mesh_plan["plan_id"]})
            assert mesh_result["verification"]["status"] == "passed", mesh_result
            assert mesh_result["actors"][0]["static_mesh_path"] == "/Engine/BasicShapes/Cube.Cube"
            layouts = []
            frame_paths = []
            for kind, parameters, count in [
                ("grid", {"rows": 2, "columns": 2}, 4),
                ("stairs", {"steps": 4}, 4),
                ("room", {"inner_size_cm": [400, 300, 250]}, 5),
            ]:
                proposal = await call(
                    "unreal_layout_preview",
                    {
                        "layout": {
                            "kind": kind,
                            "label_prefix": prefix + "_" + kind,
                            "origin": [1000 + 1500 * len(layouts), 0, 0],
                            "yaw_degrees": 30,
                            **parameters,
                        }
                    },
                )
                assert len(proposal["operations"]) == count
                assert not (await call("unreal_actors", {"query": prefix + "_" + kind}))["actors"]
                outcome = await call("unreal_apply", {"plan_id": proposal["plan_id"]})
                assert outcome["verification"]["status"] == "passed", outcome
                if kind == "stairs":
                    frame_paths = [actor["path"] for actor in outcome["actors"]]
                layouts.append(
                    {
                        "kind": kind,
                        "actors": len(outcome["actors"]),
                        "verification": outcome["verification"]["status"],
                    }
                )
            if require_capture:
                framed = await call("unreal_frame", {"actor_paths": frame_paths})
                assert framed["framed_actor_paths"] == frame_paths
            captured = await session.call_tool("unreal_capture", {"max_dimension": 1024})
            capture = captured.structuredContent
            if require_capture:
                assert not captured.isError, capture
            if not captured.isError:
                if require_capture:
                    assert capture["result"]["current_camera_location"] == framed[
                        "current_camera_location"
                    ], "Capture did not use the framed camera"
                image = next(item for item in captured.content if item.type == "image")
                destination = SANDBOX.parents[2] / "artifacts" / "editor-viewport.png"
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(base64.b64decode(image.data))
                capture = {
                    "status": "captured",
                    **capture["result"],
                    "artifact": "artifacts/editor-viewport.png",
                }
            else:
                assert capture["error"]["code"] == "viewport_unavailable", capture
                capture = {"status": "viewport_unavailable", "headless_expected": True}
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
                    "compact_context",
                    "exact_mesh_dimensions",
                    "existing_static_mesh_placement",
                    "bounded_scene_validation",
                    "layout_previews_no_mutation",
                    "three_layouts_applied_and_verified",
                ],
                "layouts": layouts,
                "capture": capture,
                "framing": "verified" if require_capture else "not_requested",
                "test_actors": [a["label"] for a in observed["actors"]],
                "saved_to_disk": False,
            }
            print(json.dumps(report, indent=2))
            destination = SANDBOX.parents[2] / "artifacts" / "editor-smoke-v0.3.json"
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--require-capture", action="store_true")
    asyncio.run(main(parser.parse_args().require_capture))
