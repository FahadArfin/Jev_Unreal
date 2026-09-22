"""Real MCP mesh workflow acceptance in this repository's disposable sandbox only.

The caller opens the matching rendered editor. This script never controls editor
processes, calls a provider, saves a map, or connects to another game project.
Run: uv run python scripts/smoke_mesh_workflows.py
"""

import argparse
import asyncio
import base64
import hashlib
import json
import os
import sys
import uuid
from contextvars import ContextVar
from datetime import UTC, datetime
from pathlib import Path

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

from jev_unreal.bridge import project_identity
from jev_unreal.config import Settings

ROOT = Path(__file__).resolve().parents[1]
SANDBOX = ROOT / "examples/JevSandbox/JevSandbox.uproject"
ARTIFACTS = ROOT / "artifacts"
REPORT = ARTIFACTS / "mesh-workflows-v0.6.json"
DIAGNOSTICS = ARTIFACTS / "mesh-workflows-v0.6-diagnostics.json"
CUBE = "/Engine/BasicShapes/Cube.Cube"
SPHERE = "/Engine/BasicShapes/Sphere.Sphere"
OVERRIDE_MATERIAL = "/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"
STATE_FIELDS = ("session_id", "world_path", "revision")
COPIED_FIELDS = (
    "static_mesh_path",
    "folder",
    "materials",
    "material_slot_count",
    "material_override_count",
    "mesh_settings",
)
STAGE = ContextVar("mesh_smoke_stage", default="preflight")
LAST_PLAN = ContextVar("mesh_smoke_plan", default=None)
ERROR_CODES = {
    "actor_not_found",
    "actor_unsupported",
    "asset_not_found",
    "asset_unsupported",
    "asset_unavailable",
    "apply_failed",
    "bad_request",
    "bridge_error",
    "capability_unavailable",
    "editor_busy",
    "editor_unavailable",
    "expired_plan",
    "forbidden",
    "invalid_mesh_preview",
    "invalid_mesh_recipe",
    "invalid_request",
    "level_locked",
    "material_slot_invalid",
    "missing_bridge_token",
    "play_mode",
    "project_required",
    "rate_limited",
    "rollback_failed",
    "stale_plan",
    "unauthorized",
    "unknown_plan",
    "wrong_project",
    "viewport_unavailable",
    "capture_failed",
}


class SmokeFailure(RuntimeError):
    """Only fixed, sanitized acceptance messages are included in the report."""

    def __init__(self, message, *, tool=None, error_code=None):
        self.message = message
        self.stage = STAGE.get()
        self.plan_id = LAST_PLAN.get()
        self.tool = tool
        self.error_code = (
            error_code if isinstance(error_code, str) and error_code in ERROR_CODES else None
        )
        detail = f"{message} Stage: {self.stage}."
        if self.error_code:
            detail += f" Error code: {self.error_code}."
        super().__init__(detail)

    def summary(self):
        return {
            "message": self.message,
            "stage": self.stage,
            "tool": self.tool,
            "error_code": self.error_code,
        }


def stage(name, plan_id=None):
    STAGE.set(name)
    LAST_PLAN.set(plan_id)


def smoke_failure(exception, depth=0):
    """Recover our original failure through the SDK's nested task-group wrappers."""
    if isinstance(exception, SmokeFailure):
        return exception
    if isinstance(exception, BaseExceptionGroup) and depth < 16:
        for child in exception.exceptions[:64]:
            failure = smoke_failure(child, depth + 1)
            if failure is not None:
                return failure
    return None


def require(condition, message):
    if not condition:
        raise SmokeFailure(message)


async def call(session, name, arguments=None, *, error=None):
    if arguments and "plan_id" in arguments:
        plan_id = arguments["plan_id"]
        if isinstance(plan_id, str) and 1 <= len(plan_id) <= 64 and plan_id.isascii():
            LAST_PLAN.set(plan_id)
    response = await session.call_tool(name, arguments or {})
    require(not response.isError, f"MCP tool/schema failure from {name}.")
    data = response.structuredContent
    require(isinstance(data, dict), f"Missing structured response from {name}.")
    if error:
        if not (data.get("ok") is False and data.get("error", {}).get("code") == error):
            raise SmokeFailure(
                f"{name} did not reject with the expected {error} code.",
                tool=name,
                error_code=data.get("error", {}).get("code"),
            )
        return data
    if data.get("ok") is not True:
        raise SmokeFailure(
            f"{name} did not return a successful result.",
            tool=name,
            error_code=data.get("error", {}).get("code"),
        )
    result = data.get("result")
    require(isinstance(result, dict), f"Missing result object from {name}.")
    return result


async def context(session):
    result = await call(session, "unreal_context", {"limit": 200})
    require(
        project_identity(result.get("project_file", "")) == project_identity(str(SANDBOX)),
        "The connected editor is not the exact repository sandbox.",
    )
    require(not result.get("actor_scan_incomplete", True), "Sandbox actor scan is incomplete.")
    require(not result.get("play_in_editor"), "Stop Play In Editor before mesh acceptance.")
    require(not result.get("simulating"), "Stop Simulate before mesh acceptance.")
    return result


def same_scene(before, after):
    require(
        all(before[field] == after[field] for field in STATE_FIELDS),
        "A preview or rejected operation changed the scene identity/revision.",
    )
    require(
        before["examined_actors"] == after["examined_actors"],
        "A preview or rejected operation changed the actor count.",
    )


async def details(session, paths):
    result = await call(session, "unreal_actor_details", {"actor_paths": paths})
    require(result.get("truncated") is False, "Exact actor inspection was incomplete.")
    require([actor["path"] for actor in result["actors"]] == paths, "Actor order changed.")
    for actor in result["actors"]:
        require(actor.get("instance_id"), "Actor inspection omitted live identity.")
        require(actor.get("editable") is True, "Fixture actor is not natively editable.")
        require(actor.get("materials_truncated") is False, "Fixture materials were truncated.")
        require(
            all(field in actor for field in COPIED_FIELDS),
            "The matching native mesh inspection fields are missing.",
        )
    return result


async def apply_once(session, plan_id):
    result = await call(session, "unreal_apply", {"plan_id": plan_id})
    require(
        result.get("verification", {}).get("status") == "passed",
        "Native apply readback did not pass its typed expectations.",
    )
    record = await call(session, "unreal_plan", {"plan_id": plan_id})
    require(record.get("status") == "applied", "Native receipt did not record applied.")
    require(record.get("executed") is True, "Native receipt omitted confirmed execution.")
    after = await context(session)
    await call(session, "unreal_apply", {"plan_id": plan_id}, error="unknown_plan")
    same_scene(after, await context(session))
    require(
        (await call(session, "unreal_plan", {"plan_id": plan_id}))["status"] == "applied",
        "A replay overwrote the successful historical receipt.",
    )
    return result


async def typed_edit(session, operations):
    before = await context(session)
    proposal = await call(
        session,
        "unreal_preview",
        {
            "operations": operations,
            "expected_state": {field: before[field] for field in STATE_FIELDS},
        },
    )
    same_scene(before, await context(session))
    return await apply_once(session, proposal["plan_id"])


async def mesh_preview(session, recipe):
    paths = recipe["actor_paths"]
    before = await details(session, paths)
    scene = await context(session)
    result = await call(session, "unreal_mesh_preview", {"recipe": recipe})
    require(before == await details(session, paths), "Mesh preview changed the source actors.")
    same_scene(scene, await context(session))
    plan_id = result["preview"]["plan_id"]
    receipt = await call(session, "unreal_plan", {"plan_id": plan_id})
    require(receipt.get("status") == "pending", "Mesh preview has no pending native receipt.")
    return result, before


async def verify(session, checks, identity):
    require(isinstance(checks, list) and checks, "Mesh workflow omitted fresh checks.")
    result = await call(session, "unreal_verify", {"checks": checks, "expected_identity": identity})
    require(result.get("status") == "passed", "Fresh typed mesh checks did not all pass.")
    return len(checks)


async def capture(session, paths, stage):
    framed = await call(session, "unreal_frame", {"actor_paths": paths, "view": "isometric"})
    require(framed.get("framed_actor_paths") == paths, "Viewport framed unexpected actors.")
    response = await session.call_tool("unreal_capture", {"max_dimension": 1024})
    require(not response.isError, "Native viewport capture returned a tool error.")
    payload = response.structuredContent
    require(isinstance(payload, dict) and payload.get("ok") is True, "Capture failed.")
    result = payload["result"]
    for field in ("current_camera_location", "current_camera_rotation"):
        require(result[field] == framed[field], "Capture did not match the reviewed camera.")
    images = [item for item in response.content if item.type == "image"]
    require(len(images) == 1, "Expected exactly one native viewport image.")
    data = base64.b64decode(images[0].data, validate=True)
    require(data.startswith(b"\x89PNG\r\n\x1a\n"), "Native capture was not a PNG.")
    target = ARTIFACTS / f"mesh-workflow-v0.6-{stage}.png"
    target.write_bytes(data)
    return {
        "stage": stage,
        "artifact": target.relative_to(ROOT).as_posix(),
        "sha256": hashlib.sha256(data).hexdigest(),
        "width": result["width"],
        "height": result["height"],
        "framed_target_count": len(paths),
        "visual_review": "not_performed_by_script",
    }


async def run(report):
    stage("preflight")
    settings = Settings.from_env()
    require(
        settings.expected_project
        and project_identity(settings.expected_project) == project_identity(str(SANDBOX)),
        "Configure only this repository's exact examples/JevSandbox/JevSandbox.uproject.",
    )
    require(SANDBOX.is_file(), "The repository sandbox project does not exist.")
    environment = {
        **os.environ,
        # Pin the settings already checked above. A profile/token file changed
        # between this check and child startup cannot retarget the smoke run.
        "JEV_PROFILES_FILE": "",
        "JEV_PROFILE": "",
        "JEV_EXPECTED_PROJECT": str(SANDBOX),
        "JEV_BRIDGE_URL": settings.bridge_url,
        "JEV_BRIDGE_TOKEN": settings.bridge_token,
        "JEV_BRIDGE_TOKEN_FILE": "",
        "JEV_BRIDGE_PORT": "9845",
        "OPENROUTER_API_KEY": "",
        "TYPESAFE_API_KEY": "",
        "JEV_PROVIDER": "openrouter",
        "JEV_MODEL": "typesafe/jev-1.13",
        "JEV_CATALOG_FILE": "",
    }
    parameters = StdioServerParameters(
        command=sys.executable, args=["-m", "jev_unreal", "serve"], env=environment
    )
    prefix = "JevMeshSmoke_" + uuid.uuid4().hex[:12]
    stage("stdio_connect")
    async with stdio_client(parameters) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()
            stage("sandbox_identity_and_assets")
            tools = {tool.name for tool in (await session.list_tools()).tools}
            require(len(tools) == 40, "Expected the matching 40-tool MCP catalog.")
            require("unreal_mesh_preview" in tools, "Mesh workflow MCP tool is unavailable.")
            status = await context(session)
            require(status.get("bridge_version") == "0.6.0", "Expected native bridge 0.6.0.")
            require(
                {"replace_mesh", "duplicate_mesh", "preview_expected_state"}
                <= set(status.get("capabilities", [])),
                "The native mesh capabilities are missing.",
            )
            report.update(engine_version=status["engine_version"], bridge_version="0.6.0")
            report["mcp_tools"] = len(tools)
            cube = await call(session, "unreal_asset_details", {"path": CUBE})
            sphere = await call(session, "unreal_asset_details", {"path": SPHERE})
            require(
                cube["static_mesh"]["material_slot_count"]
                == sphere["static_mesh"]["material_slot_count"]
                == 1,
                "The public engine mesh fixtures must each expose one material slot.",
            )
            default_material = cube["static_mesh"]["material_slots"][0]["material_path"] or None
            require(
                default_material and default_material != OVERRIDE_MATERIAL,
                "Fixture default material must exist and differ from its override.",
            )
            stage("fixture_spawn")
            spawned = await typed_edit(
                session,
                [
                    {
                        "op": "spawn_static_mesh",
                        "asset_path": CUBE,
                        "label": prefix + "_Source",
                        "location": [10000, 0, 160],
                        "rotation": [0, 20, 0],
                        "scale": [1.2, 0.8, 1.4],
                    }
                ],
            )
            source_path = spawned["actors"][0]["path"]
            stage("fixture_folder")
            await typed_edit(
                session,
                [{"op": "set_metadata", "actor_path": source_path, "folder": "Jev/MeshSmoke"}],
            )
            stage("fixture_material")
            await typed_edit(
                session,
                [
                    {
                        "op": "set_material",
                        "actor_path": source_path,
                        "slot": 0,
                        "material_path": OVERRIDE_MATERIAL,
                    }
                ],
            )
            paths = [source_path]
            stage("capture_before")
            report["captures"].append(await capture(session, paths, "before"))
            baseline = await call(session, "unreal_snapshot", {"actor_paths": paths})
            identity = baseline["identity"]
            baseline_actor = (await details(session, paths))["actors"][0]
            for policy, mesh, material in (
                ("preserve_slots", SPHERE, OVERRIDE_MATERIAL),
                ("mesh_defaults", CUBE, default_material),
            ):
                stage("replace_" + policy + "_preview")
                snapshot = await call(session, "unreal_snapshot", {"actor_paths": paths})
                proposal, before = await mesh_preview(
                    session,
                    {
                        "kind": "replace",
                        "actor_paths": paths,
                        "asset_path": mesh,
                        "material_policy": policy,
                    },
                )
                stage("replace_" + policy + "_apply", proposal["preview"]["plan_id"])
                await apply_once(session, proposal["preview"]["plan_id"])
                stage("replace_" + policy + "_verify", proposal["preview"]["plan_id"])
                count = await verify(session, proposal["verification_checks"], identity)
                actor = (await details(session, paths))["actors"][0]
                require(actor["static_mesh_path"] == mesh, "Replacement mesh path differs.")
                require(actor["materials"][0]["path"] == material, "Material policy differs.")
                for field in ("instance_id", "location", "rotation", "scale", "folder", "label"):
                    require(
                        actor[field] == before["actors"][0][field],
                        "Replacement changed identity or placement.",
                    )
                require(
                    actor["mesh_settings"] == before["actors"][0]["mesh_settings"],
                    "Replacement changed common settings.",
                )
                if policy == "mesh_defaults":
                    require(
                        actor["material_override_count"] == 0, "Mesh defaults retained overrides."
                    )
                diff = await call(session, "unreal_diff", {"snapshot_id": snapshot["snapshot_id"]})
                require(diff["status"] == "changed", "Replacement snapshot failed to show change.")
                report["workflows"].append(
                    {
                        "kind": "replace",
                        "material_policy": policy,
                        "fresh_checks": count,
                        "snapshot_diff": "changed",
                    }
                )
                stage("capture_" + policy)
                report["captures"].append(await capture(session, paths, policy))

            # Configure an effective override again so duplication must copy it.
            stage("duplicate_fixture_material")
            await typed_edit(
                session,
                [
                    {
                        "op": "set_material",
                        "actor_path": source_path,
                        "slot": 0,
                        "material_path": OVERRIDE_MATERIAL,
                    }
                ],
            )
            source_snapshot = await call(session, "unreal_snapshot", {"actor_paths": paths})
            stage("duplicate_preview")
            proposal, before = await mesh_preview(
                session,
                {
                    "kind": "duplicate",
                    "actor_paths": paths,
                    "offset_cm": [250, 0, 0],
                    "label_suffix": "_Copy",
                },
            )
            duplicate_scene = await context(session)
            stage("duplicate_apply", proposal["preview"]["plan_id"])
            result = await apply_once(session, proposal["preview"]["plan_id"])
            stage("duplicate_verify", proposal["preview"]["plan_id"])
            require(
                (await context(session))["examined_actors"]
                == duplicate_scene["examined_actors"] + 1,
                "Duplication did not add exactly one actor.",
            )
            new_path = result["actors"][0]["path"]
            require(new_path != source_path, "Duplication reused the source object path.")
            fresh = await details(session, [source_path, new_path])
            source, duplicate = fresh["actors"]
            require(source == before["actors"][0], "Duplication changed its source actor.")
            require(source["instance_id"] != duplicate["instance_id"], "Duplicate reused identity.")
            for field in COPIED_FIELDS:
                require(
                    source[field] == duplicate[field], "Duplicate changed declared copied settings."
                )
            require(
                duplicate["location"] == [source["location"][0] + 250, *source["location"][1:]],
                "Duplicate offset differs.",
            )
            require(duplicate["rotation"] == source["rotation"], "Duplicate rotation differs.")
            require(duplicate["scale"] == source["scale"], "Duplicate scale differs.")
            require(duplicate["label"] == source["label"] + "_Copy", "Duplicate label differs.")
            unchanged = await call(
                session, "unreal_diff", {"snapshot_id": source_snapshot["snapshot_id"]}
            )
            require(unchanged["status"] == "unchanged", "Duplication altered the source snapshot.")
            checks = result.get("verification_checks")
            require(isinstance(checks, list) and checks, "Duplicate omitted resolved fresh checks.")
            require(
                {check.get("kind") for check in checks} == {"mesh", "transform_equals"},
                "Duplicate checks did not cover both mesh state and transform.",
            )
            require(
                all(
                    check.get("actor_path") == new_path
                    and check.get("expected_instance_id") == duplicate["instance_id"]
                    for check in checks
                ),
                "Duplicate checks were not bound to the new exact actor identity.",
            )
            count = await verify(session, checks, identity)
            report["workflows"].append(
                {
                    "kind": "duplicate",
                    "fresh_checks": count,
                    "source_snapshot_diff": "unchanged",
                    "new_identity": True,
                }
            )
            stage("capture_duplicated")
            report["captures"].append(await capture(session, [source_path, new_path], "duplicated"))

            # Existing typed edits change the reviewed source. No arbitrary property setter
            # or test-only fault hook is invoked by this live MCP script.
            for change in ("material", "transform"):
                stage("stale_" + change + "_preview")
                stale, _ = await mesh_preview(
                    session,
                    {
                        "kind": "duplicate",
                        "actor_paths": paths,
                        "offset_cm": [0, 250, 0],
                        "label_suffix": "_MustNotSpawn",
                    },
                )
                operation = (
                    {
                        "op": "set_material",
                        "actor_path": source_path,
                        "slot": 0,
                        "material_path": default_material,
                    }
                    if change == "material"
                    else {
                        "op": "set_transform",
                        "actor_path": source_path,
                        "location": [10000, 25, 160],
                    }
                )
                require(
                    operation.get("material_path", True), "Fixture default material is missing."
                )
                stage("stale_" + change + "_source_edit")
                await typed_edit(session, [operation])
                scene = await context(session)
                observed = await details(session, [source_path, new_path])
                stage("stale_" + change + "_apply", stale["preview"]["plan_id"])
                await call(
                    session,
                    "unreal_apply",
                    {"plan_id": stale["preview"]["plan_id"]},
                    error="stale_plan",
                )
                same_scene(scene, await context(session))
                require(
                    observed == await details(session, [source_path, new_path]),
                    "Rejected stale apply changed actors.",
                )
                report["negative_checks"].append("stale_source_" + change)
            require(
                baseline_actor["instance_id"] == source["instance_id"],
                "Replacement changed the source identity.",
            )
            report["negative_checks"].extend(["preview_no_mutation", "one_shot_replay_refused"])
            report["passed"] = True
            stage("completed")


async def main():
    report = {
        "schema_version": 1,
        "scope": "repository_sandbox_live_mesh_workflows",
        "created_at_utc": datetime.now(UTC).isoformat(),
        "passed": False,
        "workflows": [],
        "negative_checks": [],
        "captures": [],
        "save_requested": False,
        "cloud_used": False,
        "limitations": [
            "Fresh checks cover reported fields, not full asset contents or gameplay.",
            "Live fixture common settings use defaults; native fixtures cover other settings.",
            "Captures require separate visual review; no art-quality claim is automatic.",
            "All scene edits remain unsaved; the script does not undo or delete its fixtures.",
        ],
    }
    ARTIFACTS.mkdir(exist_ok=True)
    diagnostics = {"created_at_utc": report["created_at_utc"], "local_only": True}
    try:
        await run(report)
    except BaseException as exc:
        failure = smoke_failure(exc)
        if failure is not None:
            report["failure"] = failure.summary()
            diagnostics["failure"] = {**failure.summary(), "plan_id": failure.plan_id}
            raise failure from None
        report["failure"] = {"kind": type(exc).__name__, "stage": STAGE.get()}
        diagnostics["failure"] = {**report["failure"], "plan_id": LAST_PLAN.get()}
        raise
    finally:
        diagnostics["passed"] = report["passed"]
        DIAGNOSTICS.write_text(json.dumps(diagnostics, indent=2) + "\n", encoding="utf-8")
        REPORT.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(report, indent=2))


if __name__ == "__main__":
    argparse.ArgumentParser(description=__doc__).parse_args()
    asyncio.run(main())
