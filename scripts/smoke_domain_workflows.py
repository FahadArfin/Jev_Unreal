"""Official MCP round-trip for domain workflows in this repository's isolated sandbox."""

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


def state(result):
    return {key: result[key] for key in ("session_id", "world_path", "revision")}


async def main():
    settings = Settings.from_env()
    assert project_identity(settings.expected_project) == project_identity(str(SANDBOX))
    params = StdioServerParameters(
        command=sys.executable,
        args=["-m", "jev_unreal", "serve"],
        env={**os.environ, "OPENROUTER_API_KEY": "", "TYPESAFE_API_KEY": ""},
    )
    report = {"provider_requests": 0, "scope": "unsaved public fixtures in exact JevSandbox"}
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()

            async def call(name, arguments=None, error=None):
                response = await session.call_tool(name, arguments or {})
                assert not response.isError, response.content
                payload = response.structuredContent
                assert isinstance(payload, dict)
                if error:
                    assert payload.get("error", {}).get("code") == error, payload
                    return payload
                assert payload.get("ok"), payload
                return payload["result"]

            report["tool_count"] = len((await session.list_tools()).tools)
            assert report["tool_count"] == 54
            ready_deadline = time.monotonic() + 90
            while True:
                ready = (await session.call_tool("unreal_status", {})).structuredContent
                if isinstance(ready, dict) and ready.get("ok"):
                    status = ready["result"]
                    break
                assert (
                    isinstance(ready, dict)
                    and ready.get("error", {}).get("code") == "editor_unavailable"
                ), ready
                assert time.monotonic() < ready_deadline, "Sandbox bridge readiness timed out"
                await asyncio.sleep(1)
            assert project_identity(status["project_file"]) == project_identity(str(SANDBOX))
            assert status["bridge_version"] == "0.8.0"
            report.update(engine_version=status["engine_version"], session_id=status["session_id"])
            prefix = f"JevDomain_{int(time.time())}"
            plan = await call(
                "unreal_preview",
                {
                    "expected_state": state(status),
                    "operations": [
                        {
                            "op": "spawn_primitive",
                            "shape": "Cube",
                            "label": prefix + "Floor",
                            "location": [8000, 0, -50],
                            "scale": [20, 20, 1],
                        },
                        {
                            "op": "spawn_primitive",
                            "shape": "Cube",
                            "label": prefix + "Prop",
                            "location": [8000, 0, 400],
                        },
                    ],
                },
            )
            await call("unreal_apply", {"plan_id": plan["plan_id"]})
            actors = (await call("unreal_actors", {"query": prefix}))["actors"]
            paths = {a["label"].removeprefix(prefix): a["path"] for a in actors}
            assert set(paths) == {"Floor", "Prop"}
            surface = await call(
                "unreal_surface_preview",
                {
                    "query": {
                        "kind": "surface",
                        "actor_path": paths["Prop"],
                        "surface_paths": [paths["Floor"]],
                        "align_to_normal": True,
                    }
                },
            )
            assert surface["surface_measurement"]["placement_valid"]
            await call("unreal_apply", {"plan_id": surface["plan_id"]})
            details = await call("unreal_actor_details", {"actor_paths": [paths["Prop"]]})
            assert abs(details["actors"][0]["location"][2] - 51) < 0.01
            for key in (
                "receives_decals",
                "render_custom_depth",
                "custom_depth_stencil_value",
                "translucency_sort_priority",
            ):
                assert key in details["actors"][0]["mesh_settings"]
            report["terrain_applied_and_fresh_verified"] = True

            diagnosis = await call(
                "unreal_workflow_inspect",
                {
                    "query": {
                        "kind": "asset_diagnosis",
                        "target_path": "/Engine/BasicShapes/Cube.Cube",
                        "dependency_depth": 3,
                    }
                },
            )
            assert diagnosis["lod_count"] >= 1 and diagnosis["simple_collision_shapes"] >= 1
            assert len(diagnosis["dependencies"]) <= 128
            report["loaded_mesh_diagnosis"] = True

            camera = await call("unreal_workflow_inspect", {"query": {"kind": "camera"}})
            desired = {
                "kind": "camera",
                "location": [7700, 0, 190],
                "rotation": [-25, 0, 0],
                "fov_degrees": 60,
            }
            preview = await call(
                "unreal_workflow_preview", {"change": desired, "expected_state": state(camera)}
            )
            applied = await call("unreal_workflow_apply", {"plan_id": preview["plan_id"]})
            assert applied["status"] == "applied" and applied["readback_verified"]
            await call("unreal_workflow_apply", {"plan_id": preview["plan_id"]}, "plan_consumed")
            receipt = await call("unreal_workflow_receipt", {"plan_id": preview["plan_id"]})
            assert receipt["status"] == "applied"
            await call("unreal_capture", {"max_dimension": 512})
            current = await call("unreal_workflow_inspect", {"query": {"kind": "camera"}})
            restore = await call(
                "unreal_workflow_preview",
                {
                    "change": {
                        "kind": "camera",
                        **{key: camera[key] for key in ("location", "rotation", "fov_degrees")},
                    },
                    "expected_state": state(current),
                },
            )
            assert (await call("unreal_workflow_apply", {"plan_id": restore["plan_id"]}))[
                "readback_verified"
            ]
            report["camera_capture_restore_and_receipt"] = True

            await call(
                "unreal_workflow_preview",
                {
                    "change": {
                        "kind": "material_scalar",
                        "target_path": "/Game/Unapproved.Unapproved",
                        "parameter": "Value",
                        "value": 1,
                    },
                    "expected_state": state(await call("unreal_status")),
                },
                "target_not_allowed",
            )
            for kind in ("rig", "widgets"):
                query = {"kind": kind, "target_path": "/Engine/BasicShapes/Cube.Cube"}
                await call(
                    "unreal_workflow_inspect",
                    {"query": query},
                    "unsupported_asset" if kind == "rig" else "asset_not_loaded",
                )
            report["policy_and_wrong_asset_refusals"] = True

            captures = []
            for _ in range(2):
                job = await call(
                    "unreal_performance_start",
                    {
                        "protocol_id": "sandbox-idle-poll-500ms",
                        "expected_state": state(await call("unreal_status")),
                        "sample_count": 20,
                    },
                )
                deadline = time.monotonic() + 65
                while job["status"] == "running" and time.monotonic() < deadline:
                    await asyncio.sleep(0.5)
                    job = await call("unreal_performance_job", {"job_id": job["job_id"]})
                assert job["status"] == "completed", job
                assert job["editor_tick_interval"]["count"] == 20
                captures.append(job)
            comparison = await call(
                "unreal_performance_compare",
                {
                    "baseline_job_id": captures[0]["job_id"],
                    "candidate_job_id": captures[1]["job_id"],
                },
            )
            assert comparison["statistical_significance_established"] is False
            report["timing_captures"] = captures
            report["comparison"] = comparison
            metrics = (await session.call_tool("jev_status", {})).structuredContent
            report["model_requests_after"] = metrics["decisions"]["requests_sent"]
            assert report["model_requests_after"] == 0
    destination = ROOT / "artifacts/domain-workflows-v0.8.json"
    destination.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(
        json.dumps(
            {
                key: value
                for key, value in report.items()
                if key not in {"timing_captures", "comparison"}
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    asyncio.run(main())
