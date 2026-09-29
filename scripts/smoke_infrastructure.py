"""Explicit live sandbox acceptance; never launch an editor or select another project.

Default: official MCP clients exercise policy/leases/receipts and create one unsaved Cube.
Jobs: --jobs-only --editor-closed --job build [--job cook] [--job package].
Named jobs run only when explicitly selected; entrypoint pins are taken from the selected
licensed UE 5.8 installation. All state/config/output is private local AppData.
"""

import argparse
import asyncio
import json
import os
import socket
import sys
import uuid
from contextlib import AsyncExitStack
from pathlib import Path
from urllib.parse import urlparse

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

from jev_unreal.bridge import project_identity
from jev_unreal.config import Settings
from jev_unreal.jobs import EXECUTABLES, file_hash
from jev_unreal.team_policy import JobKind, local_path

ROOT = Path(__file__).resolve().parents[1]
SANDBOX = ROOT / "examples/JevSandbox/JevSandbox.uproject"
CUBE = "/Engine/BasicShapes/Cube.Cube"


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


async def call(session, name, arguments=None, *, error=None):
    response = await session.call_tool(name, arguments or {})
    require(not response.isError, f"MCP protocol error from {name}.")
    payload = response.structuredContent
    require(isinstance(payload, dict), f"Missing structured result from {name}.")
    if error:
        require(
            payload.get("ok") is False and payload.get("error", {}).get("code") == error,
            f"{name} did not refuse with {error}.",
        )
        return {"refusal": error}
    require(
        payload.get("ok") is True,
        f"{name} failed with {payload.get('error', {}).get('code', 'unknown')}.",
    )
    return payload["result"]


async def connect(stack, config):
    environment = {
        **os.environ,
        "JEV_RUNTIME_CONFIG": str(config),
        "OPENROUTER_API_KEY": "",
        "TYPESAFE_API_KEY": "",
        "JEV_CATALOG_FILE": "",
    }
    parameters = StdioServerParameters(
        command=sys.executable, args=["-m", "jev_unreal", "serve"], env=environment
    )
    read, write = await stack.enter_async_context(stdio_client(parameters))
    client = await stack.enter_async_context(ClientSession(read, write))
    await client.initialize()
    return client


def prepare_config(args, run_directory):
    config = {
        "version": 1,
        "project_file": str(SANDBOX),
        "state_directory": str(run_directory / "state"),
        "allowed_actions": ["preview", "apply", "frame"],
        "asset_roots": ["/Game", "/Engine", "/Temp"],
        "allowed_operations": ["spawn_primitive"],
        "jobs": [],
    }
    if args.jobs_only:
        engine = local_path(str(Path(args.engine_root).absolute()))
        version_file = local_path(str(engine / "Engine/Build/Build.version"), exists=True)
        require(version_file.stat().st_size <= 65536, "Unexpected engine version metadata size.")
        version = json.loads(version_file.read_text(encoding="utf-8-sig"))
        require(
            version.get("MajorVersion") == 5 and version.get("MinorVersion") == 8,
            "Named-job smoke is restricted to a licensed UE 5.8 installation.",
        )
        dotnet_relative = "Engine/Binaries/ThirdParty/DotNet/10.0/win-x64"
        dotnet = local_path(str(engine / dotnet_relative / "dotnet.exe"), exists=True)
        config.update(
            engine_root=str(engine),
            dotnet_relative_root=dotnet_relative,
            dotnet_host_sha256=file_hash(dotnet),
            editor_target="JevSandboxEditor",
            output_directory=str(run_directory / "outputs"),
        )
        for name in args.job:
            kind = JobKind(name)
            executable = local_path(str(engine / EXECUTABLES[kind]), exists=True)
            config["jobs"].append(
                {
                    "name": name,
                    "kind": name,
                    "executable_sha256": file_hash(executable),
                    "timeout_seconds": 3600,
                    "max_output_bytes": 16 * 1048576,
                    "max_artifact_bytes": 20 * 1073741824,
                    "max_artifact_files": 100000,
                    "minimum_free_bytes": 1073741824,
                }
            )
    destination = run_directory / "runtime.json"
    destination.write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")
    return destination


async def live_smoke(config, report):
    label = "JevInfrastructure_" + uuid.uuid4().hex[:12]
    preview_args = {
        "operations": [
            {"op": "spawn_primitive", "shape": "Cube", "label": label, "location": [12000, 0, 50]}
        ]
    }
    async with AsyncExitStack() as stack:
        first = await connect(stack, config)
        second = await connect(stack, config)
        status = await call(first, "unreal_status")
        require(
            project_identity(status["project_file"]) == project_identity(str(SANDBOX)),
            "Connected editor is not this repository's sandbox.",
        )
        report["native_identity"] = {
            key: status[key]
            for key in (
                "project_file",
                "session_id",
                "world_path",
                "revision",
                "engine_version",
                "bridge_version",
            )
        }
        team = await call(first, "unreal_team_status")
        require(
            team["configured"] and team["durable_receipts"], "Runtime policy was not activated."
        )
        lease = await call(first, "unreal_project_lease", {"seconds": 120})
        report["lease_conflict"] = await call(
            second, "unreal_preview", preview_args, error="project_leased"
        )
        released = await call(
            first, "unreal_project_lease_release", {"lease_id": lease["lease_id"]}
        )
        require(released["released"], "Owned lease was not released.")
        next_lease = await call(second, "unreal_project_lease", {"seconds": 120})
        await call(second, "unreal_project_lease_release", {"lease_id": next_lease["lease_id"]})
        plan = await call(first, "unreal_preview", preview_args)
        applied = await call(first, "unreal_apply", {"plan_id": plan["plan_id"]})
        require(applied.get("applied") is True, "Native apply did not report success.")
        actor_path = applied["actors"][0]["path"]
        records = await call(first, "unreal_durable_receipts", {"limit": 20})
        receipt = next(
            row
            for row in records
            if row.get("plan_id") == plan["plan_id"] and row["action"] == "apply"
        )
        require(
            receipt["status"] == "response_observed", "Apply receipt was not durably completed."
        )
        report["apply_receipt_id"] = receipt["receipt_id"]
        recipes = await first.read_resource("jev://recipes/fr/move_actor")
        recipe = json.loads(recipes.contents[0].text)
        require(recipe["executes_tools"] is False, "A recipe must not execute anything.")
        report["recipe_resource"] = {
            "locale": recipe["locale"],
            "translation_status": recipe["translation_status"],
        }
        cube = await call(
            first,
            "unreal_workflow_inspect",
            {"query": {"kind": "asset_diagnosis", "target_path": CUBE, "dependency_depth": 1}},
        )
        require(
            all(abs(value - 100) < 0.1 for value in cube["bounds_size_cm"]),
            "Fresh engine Cube dimensions differ from the explicit 100 cm fixture contract.",
        )
        report["fresh_cube_measurement"] = {
            "asset_path": cube["asset_path"],
            "bounds_size_cm": cube["bounds_size_cm"],
        }
        report["handoff_roundtrip"] = "Not exercised: Engine Cube is not a /Game DCC handoff asset."
    async with AsyncExitStack() as stack:
        reconnected = await connect(stack, config)
        current = await call(reconnected, "unreal_status")
        require(
            current["session_id"] == status["session_id"], "Editor changed during reconnect smoke."
        )
        retained = await call(
            reconnected, "unreal_durable_receipt", {"receipt_id": receipt["receipt_id"]}
        )
        require(
            retained["status"] == "response_observed", "Durable receipt did not survive reconnect."
        )
        report["replay_refusal"] = await call(
            reconnected, "unreal_apply", {"plan_id": plan["plan_id"]}, error="policy_plan_required"
        )
        verification = await call(
            reconnected,
            "unreal_verify",
            {"checks": [{"kind": "label", "actor_path": actor_path, "expected": label}]},
        )
        require(
            verification["status"] == "passed", "Fresh actor verification failed after reconnect."
        )
        report["fresh_verification"] = verification
        report["receipt_survived_mcp_restart"] = True
        report["editor_crash_recovery_measured"] = False
        report["saved"] = False


async def jobs_smoke(config, args, report):
    async with AsyncExitStack() as stack:
        session = await connect(stack, config)
        report["jobs"] = []
        for name in args.job:
            plan = await call(session, "unreal_named_job_preview", {"name": name})
            require(plan["identity"]["project_file"] == str(SANDBOX), "Job project changed.")
            started = await call(session, "unreal_named_job_start", {"plan_id": plan["plan_id"]})
            job_id = started["job_id"]
            async with asyncio.timeout(3660):
                while True:
                    result = await call(session, "unreal_named_job", {"job_id": job_id})
                    if result["status"] not in {"starting_uncertain", "running"}:
                        break
                    await asyncio.sleep(1)
            report["jobs"].append(result)
            require(
                result["status"] in {"succeeded", "output_unverified"}
                and result.get("exit_code") == 0,
                f"Named {name} job failed; inspect its retained private receipt.",
            )
        report["all_job_outputs_verified"] = all(
            row["status"] == "succeeded" for row in report["jobs"]
        )
        report["scope"] = (
            "Real named job exits/output evidence; no package launch or gameplay acceptance."
        )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs-only", action="store_true")
    parser.add_argument(
        "--editor-closed",
        action="store_true",
        help="Explicit operator attestation that this sandbox's editor is closed",
    )
    parser.add_argument(
        "--job", choices=[kind.value for kind in JobKind], action="append", default=[]
    )
    parser.add_argument("--engine-root", default="C:/Program Files/UE_5.8")
    args = parser.parse_args()
    require(bool(args.job) == args.jobs_only, "Use --jobs-only with explicit --job kinds only.")
    require(len(args.job) == len(set(args.job)), "Do not repeat a named job in the same run.")
    settings = Settings.from_env()
    require(
        settings.expected_project
        and project_identity(settings.expected_project) == project_identity(str(SANDBOX)),
        "Only this repository's exact sandbox is permitted.",
    )
    if args.jobs_only:
        require(args.editor_closed, "Confirm the sandbox editor is closed with --editor-closed.")
        endpoint = urlparse(settings.bridge_url)
        with socket.socket(
            socket.AF_INET6 if endpoint.hostname == "::1" else socket.AF_INET, socket.SOCK_STREAM
        ) as probe:
            probe.settimeout(1)
            require(
                probe.connect_ex((endpoint.hostname, endpoint.port)) != 0,
                "The selected bridge endpoint is listening; close the sandbox editor first.",
            )
    require(
        os.name == "nt" and os.environ.get("LOCALAPPDATA"), "Use a local Windows AppData profile."
    )
    run_id = uuid.uuid4().hex
    private = local_path(str(Path(os.environ["LOCALAPPDATA"]) / "JevUnreal/acceptance" / run_id))
    private.mkdir(parents=True, exist_ok=False)
    config = prepare_config(args, private)
    report = {
        "run_id": run_id,
        "mode": "named_jobs" if args.jobs_only else "live_bridge",
        "cloud_requests": 0,
        "status": "running",
        "private_runtime_directory": str(private),
        "clean_host_acceptance": False,
        "human_acceptance": False,
    }
    destination = ROOT / "artifacts" / f"infrastructure-{run_id}.json"
    destination.parent.mkdir(parents=True, exist_ok=True)
    try:
        asyncio.run(
            jobs_smoke(config, args, report) if args.jobs_only else live_smoke(config, report)
        )
        report["status"] = "completed"
    except Exception as exc:
        report["status"] = "failed"
        report["error_type"] = type(exc).__name__
        raise
    finally:
        destination.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({"status": report["status"], "report": str(destination)}))


if __name__ == "__main__":
    main()
