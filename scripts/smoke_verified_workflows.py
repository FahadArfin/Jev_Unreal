"""Live inspect/edit/verify acceptance, restricted to this repository's unsaved sandbox.

Run after Initialize-Local.ps1 with the rendered editor open. No provider key needed.
"""

import asyncio
import base64
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


async def main():
    settings = Settings.from_env()
    assert settings.expected_project and project_identity(
        settings.expected_project
    ) == project_identity(str(SANDBOX)), "This smoke test only edits the isolated JevSandbox"
    env = {**os.environ, "OPENROUTER_API_KEY": "", "TYPESAFE_API_KEY": ""}
    params = StdioServerParameters(
        command=sys.executable, args=["-m", "jev_unreal", "serve"], env=env
    )
    prefix = f"JevVerified_{int(time.time())}"
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()

            async def call(name, arguments, error=None):
                response = await session.call_tool(name, arguments)
                assert not response.isError, response.content
                data = response.structuredContent
                assert isinstance(data, dict), response.content
                if error:
                    assert data.get("error", {}).get("code") == error, data
                    return data
                assert data["ok"], data
                return data["result"]

            status = await call("unreal_status", {})
            assert project_identity(status["project_file"]) == project_identity(str(SANDBOX))
            assert {
                "actor_details",
                "preview_expected_state",
                "set_material",
                "set_metadata",
            } <= set(status["capabilities"]), status
            tools = (await session.list_tools()).tools
            assert len(tools) == 44
            operations = [
                {
                    "op": "spawn_primitive",
                    "shape": "Cube",
                    "label": f"{prefix}_{index}",
                    "location": [6500 + offset, 100 * index, 250 + 70 * index],
                    "rotation": [0, yaw, 0],
                    "scale": scale,
                }
                for index, (offset, yaw, scale) in enumerate(
                    [
                        (0, 15, [0.8, 1.2, 1.0]),
                        (350, -20, [1.2, 0.8, 1.5]),
                        (800, 35, [1.0, 1.0, 0.7]),
                    ]
                )
            ]
            spawn = await call("unreal_preview", {"operations": operations})
            assert not (await call("unreal_actors", {"query": prefix}))["actors"]
            outcome = await call("unreal_apply", {"plan_id": spawn["plan_id"]})
            assert outcome["verification"]["status"] == "passed", outcome
            paths = [actor["path"] for actor in outcome["actors"]]
            details = await call("unreal_actor_details", {"actor_paths": paths})
            assert all(
                actor["bounds_available"] and actor["instance_id"] for actor in details["actors"]
            )
            snapshot = await call("unreal_snapshot", {"actor_paths": paths})
            assert (await call("unreal_diff", {"snapshot_id": snapshot["snapshot_id"]}))[
                "status"
            ] == "unchanged"
            workflow_results = []

            async def spatial(recipe):
                before = await call("unreal_actor_details", {"actor_paths": paths})
                proposal = await call(
                    "unreal_spatial_preview", {"recipe": {**recipe, "actor_paths": paths}}
                )
                after_preview = await call("unreal_actor_details", {"actor_paths": paths})
                assert before == after_preview, "Spatial preview changed inspected actors"
                plan_id = proposal["preview"]["plan_id"]
                assert (await call("unreal_plan", {"plan_id": plan_id}))["status"] == "pending"
                result = await call("unreal_apply", {"plan_id": plan_id})
                assert result["verification"]["status"] == "passed", result
                verified = await call(
                    "unreal_verify",
                    {
                        "checks": proposal["verification_checks"],
                        "expected_identity": snapshot["identity"],
                    },
                )
                assert verified["status"] == "passed", verified
                record = await call("unreal_plan", {"plan_id": plan_id})
                assert record["status"] == "applied" and record["executed"] is True, record
                await call("unreal_apply", {"plan_id": plan_id}, "unknown_plan")
                assert (await call("unreal_plan", {"plan_id": plan_id}))["status"] == "applied"
                workflow_results.append(
                    {
                        "kind": recipe["kind"],
                        "status": verified["status"],
                        "checks": len(proposal["verification_checks"]),
                    }
                )

            await spatial({"kind": "ground", "z_cm": 0})
            assert (await call("unreal_diff", {"snapshot_id": snapshot["snapshot_id"]}))[
                "status"
            ] == "changed"
            edit = [
                {"op": "set_metadata", "actor_path": paths[0], "label": prefix + "_NeverApplied"}
            ]
            stale = {key: details[key] for key in ("session_id", "world_path", "revision")}
            await call(
                "unreal_preview", {"operations": edit, "expected_state": stale}, "stale_plan"
            )
            current = await call("unreal_actor_details", {"actor_paths": paths})
            wrong_session = {key: current[key] for key in stale}
            wrong_session["session_id"] = "different-session"
            await call(
                "unreal_preview",
                {"operations": edit, "expected_state": wrong_session},
                "stale_plan",
            )
            assert (await call("unreal_actor_details", {"actor_paths": paths}))[
                "actors"
            ] == current["actors"]
            await spatial({"kind": "align", "axis": "y", "anchor": "min", "target_cm": 0})
            await spatial({"kind": "distribute", "axis": "x", "mode": "equal_gaps"})
            await spatial({"kind": "snap_grid", "grid_cm": 25})
            await spatial({"kind": "ground", "z_cm": 0})

            folder = f"Jev/Verified/{prefix}"
            metadata = await call(
                "unreal_preview",
                {
                    "operations": [
                        {
                            "op": "set_metadata",
                            "actor_path": path,
                            "label": f"{prefix}_Verified_{index}",
                            "folder": folder,
                        }
                        for index, path in enumerate(paths)
                    ]
                },
            )
            result = await call("unreal_apply", {"plan_id": metadata["plan_id"]})
            assert result["verification"]["status"] == "passed", result
            instance_ids = {actor["path"]: actor["instance_id"] for actor in details["actors"]}
            checks = [
                {
                    "kind": kind,
                    "actor_path": path,
                    "expected": value,
                    "expected_instance_id": instance_ids[path],
                }
                for index, path in enumerate(paths)
                for kind, value in [("label", f"{prefix}_Verified_{index}"), ("folder", folder)]
            ]
            assert (await call("unreal_verify", {"checks": checks}))["status"] == "passed"
            material_path = "/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial"
            material = await call(
                "unreal_preview",
                {
                    "operations": [
                        {
                            "op": "set_material",
                            "actor_path": paths[0],
                            "material_path": material_path,
                            "slot": 0,
                        }
                    ]
                },
            )
            result = await call("unreal_apply", {"plan_id": material["plan_id"]})
            assert result["verification"]["status"] == "passed", result
            material_check = {
                "kind": "material_slot",
                "actor_path": paths[0],
                "slot": 0,
                "expected_path": material_path,
                "expected_instance_id": instance_ids[paths[0]],
            }
            assert (await call("unreal_verify", {"checks": [material_check]}))["status"] == "passed"
            failed = await call(
                "unreal_verify",
                {
                    "checks": [
                        {"kind": "label", "actor_path": paths[0], "expected": "DeliberatelyWrong"}
                    ]
                },
            )
            assert failed["status"] == "failed", failed
            replaced = await call(
                "unreal_verify",
                {"checks": [{**material_check, "expected_instance_id": "different-instance"}]},
            )
            assert replaced["status"] == "unverifiable", replaced
            missing = await call(
                "unreal_verify",
                {
                    "checks": [
                        {
                            "kind": "label",
                            "actor_path": paths[0] + "_Missing",
                            "expected": "missing",
                        }
                    ]
                },
            )
            assert missing["status"] == "unverifiable", missing
            capture_reports = []
            for view in ("isometric", "top", "front", "right"):
                framed = await call("unreal_frame", {"actor_paths": paths, "view": view})
                assert framed["view"] == view and framed["framed_actor_paths"] == paths, framed
                captured = await session.call_tool("unreal_capture", {"max_dimension": 1024})
                assert not captured.isError, captured.structuredContent
                capture = captured.structuredContent["result"]
                assert capture["current_camera_location"] == framed["current_camera_location"]
                assert capture["current_camera_rotation"] == framed["current_camera_rotation"]
                image = next(item for item in captured.content if item.type == "image")
                artifact = ROOT / "artifacts" / f"verified-workflow-{view}.png"
                artifact.write_bytes(base64.b64decode(image.data))
                capture_reports.append(
                    {
                        "view": view,
                        "artifact": artifact.relative_to(ROOT).as_posix(),
                        "width": capture["width"],
                        "height": capture["height"],
                    }
                )
            await call("unreal_frame", {"actor_paths": paths, "view": "isometric"})
            final = await call("unreal_actor_details", {"actor_paths": paths})
            report = {
                "passed": True,
                "engine_version": status["engine_version"],
                "bridge_version": status["bridge_version"],
                "mcp_tools": len(tools),
                "spatial_workflows": workflow_results,
                "checks": [
                    "inspect_exact_instances",
                    "snapshot_unchanged_then_changed",
                    "preview_no_mutation",
                    "stale_inspection_rejected",
                    "wrong_session_rejected",
                    "plan_records",
                    "one_shot_apply",
                    "metadata_batch_readback",
                    "material_assignment_readback",
                    "fresh_requirement_checks",
                    "deliberate_mismatch_failed",
                    "wrong_instance_unverifiable",
                    "missing_actor_unverifiable",
                    "four_camera_presets_captured",
                ],
                "final_actors": final["actors"],
                "captures": capture_reports,
                "saved_to_disk": False,
                "cloud_used": False,
            }
            (ROOT / "artifacts/verified-workflows-v0.7.json").write_text(
                json.dumps(report, indent=2) + "\n", encoding="utf-8"
            )
            print(json.dumps(report, indent=2))


if __name__ == "__main__":
    asyncio.run(main())
