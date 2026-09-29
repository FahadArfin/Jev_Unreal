"""Fresh official MCP verification of the explicitly prepared local calibration asset."""

import asyncio
import json
import os
import sys
import time
from copy import deepcopy
from pathlib import Path

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client
from unreal_handoff_fixture import ASSET_PATH, read_calibration_bundle

from jev_unreal.bridge import project_identity
from jev_unreal.config import Settings

ROOT = Path(__file__).resolve().parents[1]
SANDBOX = ROOT / "examples/JevSandbox/JevSandbox.uproject"


async def main():
    settings = Settings.from_env()
    assert project_identity(settings.expected_project) == project_identity(str(SANDBOX))
    bundle_root, manifest = read_calibration_bundle(ROOT)
    asset = manifest["assets"][0]
    params = StdioServerParameters(
        command=sys.executable,
        args=["-m", "jev_unreal", "serve"],
        env={**os.environ, "OPENROUTER_API_KEY": "", "TYPESAFE_API_KEY": ""},
    )
    report = {
        "scope": "Fresh native MCP reads of explicitly saved public JevSandbox fixture",
        "source_hashes_verified": True,
        "provider_requests": 0,
        "import_or_save_requested_by_mcp": False,
    }
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()

            async def call(name, arguments=None):
                response = await session.call_tool(name, arguments or {})
                assert not response.isError, response.content
                payload = response.structuredContent
                assert isinstance(payload, dict), "Missing structured MCP result: " + name
                assert payload.get("ok"), payload
                return payload["result"]

            catalog = (await session.list_tools()).tools
            handoff = next(tool for tool in catalog if tool.name == "unreal_handoff_verify")
            assert handoff.outputSchema, "Handoff tool must advertise structured output."
            assert handoff.annotations.readOnlyHint is True
            report["tool_count"] = len(catalog)
            deadline = time.monotonic() + 90
            while True:
                response = await session.call_tool("unreal_status", {})
                assert not response.isError, response.content
                status = response.structuredContent
                assert isinstance(status, dict), "Missing structured status."
                if status.get("ok"):
                    status = status["result"]
                    break
                assert status.get("error", {}).get("code") == "editor_unavailable", status
                assert time.monotonic() < deadline, "Sandbox readiness timed out."
                await asyncio.sleep(1)
            assert project_identity(status["project_file"]) == project_identity(str(SANDBOX))
            assert status["bridge_version"] == "0.9.0"
            report["identity"] = {
                key: status[key]
                for key in (
                    "project_file", "session_id", "world_path", "revision", "engine_version"
                )
            }
            details = await call("unreal_asset_details", {"path": ASSET_PATH})
            assert details["path"] == ASSET_PATH and details["loaded"] is True
            positive = await call("unreal_handoff_verify", {"asset": asset})
            assert positive["status"] == "passed", positive
            assert all(check["status"] == "passed" for check in positive["checks"])
            assert positive["observation_source"] == "native_bridge"
            for key in ("project_file", "session_id", "world_path"):
                assert positive["identity"][key] == status[key]
            wrong = deepcopy(asset)
            wrong["bounds_center_cm"][2] += 25
            negative = await call("unreal_handoff_verify", {"asset": wrong})
            assert negative["status"] == "failed", negative
            assert {
                check["check"]: check["status"] for check in negative["checks"]
            } == {
                "bounds_size_cm": "passed",
                "bounds_center_cm": "failed",
                "material_slots": "passed",
            }, negative
            final = await call("unreal_status")
            for key in ("project_file", "session_id", "world_path"):
                assert final[key] == status[key], "Connected editor identity changed."
            metrics = (await session.call_tool("jev_status", {})).structuredContent
            assert metrics["decisions"]["requests_sent"] == 0
            report.update(positive=positive, intentional_wrong_pivot=negative)
    (bundle_root / "live-mcp-verification.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    asyncio.run(main())
