"""New roadmap acceptance through fresh SDK stdio sessions in JevSandbox only.

Requires reviewed runtime/checkpoint/handoff configs and the public calibration
bundle. Creates an unsaved two-box layout and one unsaved static mesh import.
Makes no provider calls. Refuses every project except this repository's sandbox.
"""

import asyncio
import json
import os
import sys
import uuid
from contextlib import asynccontextmanager
from pathlib import Path

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

from jev_unreal.config import Settings
from jev_unreal.team_policy import project_identity

ROOT = Path(__file__).resolve().parents[1]
SANDBOX = ROOT / "examples/JevSandbox/JevSandbox.uproject"


@asynccontextmanager
async def connect(groups="all"):
    env = {
        **os.environ,
        "JEV_TOOL_GROUPS": groups,
        "OPENROUTER_API_KEY": "",
        "TYPESAFE_API_KEY": "",
    }
    params = StdioServerParameters(
        command=sys.executable, args=["-m", "jev_unreal", "serve"], env=env
    )
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()
            yield session


async def call(session, name, args=None):
    reply = await session.call_tool(name, args or {})
    data = reply.structuredContent
    errors = [getattr(item, "text", "")[:1000] for item in reply.content if reply.isError]
    assert not reply.isError and isinstance(data, dict) and data.get("ok"), (name, data, errors)
    return data["result"]


async def main():
    settings = Settings.from_env()
    assert settings.expected_project and project_identity(
        settings.expected_project
    ) == project_identity(str(SANDBOX)), "Only the repository sandbox is allowed."
    report = {"provider_requests": 0}
    async with connect("core") as session:
        status = await call(session, "unreal_status")
        assert project_identity(status["project_file"]) == project_identity(str(SANDBOX))
        assert {"compact_read", "runtime_gameplay", "handoff_import"} <= set(
            status["capabilities"]
        )
        report["core_tools"] = len((await session.list_tools()).tools)
        groups = await call(
            session,
            "jev_tool_groups_activate",
            {"groups": ["inspection", "scene"], "client_supports_list_changed": True},
        )
        report["dynamic_groups"] = groups
        assert "unreal_preview" in {t.name for t in (await session.list_tools()).tools}
    async with connect() as session:
        report["all_tools"] = len((await session.list_tools()).tools)
        run = await call(
            session,
            "unreal_workflow_run_preview",
            {
                "recipe": {
                    "kind": "grid",
                    "rows": 1,
                    "columns": 2,
                    "label_prefix": "JevRoadmap_" + uuid.uuid4().hex[:8],
                    "origin": [500.0, 500.0, 0.0],
                }
            },
        )
        assert run["status"] == "awaiting_review"
    # A different MCP process opens the same private run database and original native plan.
    async with connect() as session:
        resumed = await call(session, "unreal_workflow_run", {"run_id": run["run_id"]})
        assert resumed["status"] == "awaiting_review"
        applied = await call(
            session,
            "unreal_workflow_run_apply",
            {"run_id": run["run_id"], "review_sha256": run["review_sha256"]},
        )
        assert applied["status"] == "verified", applied
        report["durable_workflow"] = applied["status"]
        checkpoint = await call(
            session, "unreal_project_checkpoint", {"files": ["JevSandbox.uproject"]}
        )
        compared = await call(
            session,
            "unreal_project_checkpoint_compare",
            {"checkpoint_id": checkpoint["receipt_id"]},
        )
        report["checkpoint"] = compared
        assert compared["matches_disk"] is True and not compared["changed_files"]
        report["vcs"] = await call(
            session, "unreal_project_vcs_status", {"files": ["JevSandbox.uproject"]}
        )
        status = await call(session, "unreal_status")
        review = await call(
            session,
            "unreal_handoff_import_preview",
            {
                "alias": "calibration",
                "operation": "import",
                "expected_state": {
                    key: status[key] for key in ("session_id", "world_path", "revision")
                },
            },
        )
        imported = await call(
            session, "unreal_handoff_import_apply", {"plan_id": review["plan_id"]}
        )
        assert imported["status"] == "verified", imported
        report["handoff"] = imported
        report["runtime_policy"] = await call(session, "unreal_runtime_status")
    output = ROOT / "artifacts/roadmap-live-smoke.json"
    await asyncio.to_thread(output.write_text, json.dumps(report, indent=2) + "\n")
    print(json.dumps({"verified": True, "tools": report["all_tools"], "report": str(output)}))


if __name__ == "__main__":
    asyncio.run(main())
