"""Read-only native/MCP acceptance of compact reads, groups and capture comparison.

Requires explicit configuration for this repository's already-open JevSandbox.
Never loads provider keys, probes the provider, starts PIE or edits the scene.
"""

import asyncio
import base64
import json
import os
import sys
from pathlib import Path

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

from jev_unreal.config import Settings
from jev_unreal.team_policy import project_identity

ROOT = Path(__file__).resolve().parents[1]
SANDBOX = ROOT / "examples/JevSandbox/JevSandbox.uproject"


def encoded_bytes(value):
    return len(json.dumps(value, separators=(",", ":"), ensure_ascii=False).encode("utf-8"))


async def main():
    settings = Settings.from_env()
    assert settings.expected_project and (
        project_identity(settings.expected_project) == project_identity(str(SANDBOX))
    ), "Select this repository's isolated JevSandbox explicitly."
    folder = ROOT / "artifacts/efficiency-smoke"
    folder.mkdir(parents=True, exist_ok=True)
    report = {"provider_requests": 0, "scene_mutations": 0, "gameplay_executed": False}
    initial = None
    for groups in ("all", "core"):
        params = StdioServerParameters(
            command=sys.executable, args=["-m", "jev_unreal", "serve"],
            env={**os.environ, "OPENROUTER_API_KEY": "", "TYPESAFE_API_KEY": "",
                 "JEV_TOOL_GROUPS": groups, "JEV_CATALOG_FILE": "", "JEV_RUNTIME_CONFIG": ""},
        )
        async with stdio_client(params) as (read, write):
            async with ClientSession(read, write) as session:
                await session.initialize()

                async def call(name, arguments=None):
                    response = await session.call_tool(name, arguments or {})
                    data = response.structuredContent
                    assert not response.isError and isinstance(data, dict), name
                    assert data.get("ok") is True, (name, data)
                    return data["result"], response

                status, _ = await call("unreal_status")
                assert project_identity(status["project_file"]) == project_identity(str(SANDBOX))
                assert status["bridge_version"] == "0.9.0"
                if initial is None:
                    initial = status
                for field in ("session_id", "world_path", "revision"):
                    assert status[field] == initial[field], "Sandbox state changed during smoke."
                catalog, _ = await call("jev_tool_groups")
                report[groups] = {key: catalog[key] for key in (
                    "advertised_tools", "schema_bytes", "catalog_sha256"
                )}
                health, _ = await call("jev_provider_health")
                assert not health["key_present"] and health["requests_sent"] == 0
                routed, _ = await call("jev_route_selective", {
                    "goal": "Inspect the current project", "explicit_tool": "unreal_status",
                })
                assert routed["selected"] == "unreal_status"
                assert not routed["routing"]["provider_request_sent"]
                if groups == "core":
                    refused = await session.call_tool("unreal_apply", {"plan_id": "not-a-plan"})
                    assert refused.isError
                    schema, _ = await call("jev_tool_schema", {"name": "unreal_apply"})
                    assert not schema["active"]
                    continue
                actors, _ = await call("unreal_actors", {"limit": 3})
                paths = [row["path"] for row in actors["actors"]]
                assert paths, "Sandbox needs at least one loaded actor."
                detail, _ = await call("unreal_actor_details", {"actor_paths": paths})
                request = {"source": "actor_details", "actor_paths": paths,
                           "fields": ["label", "bounds_cm"], "page_size": 1}
                compact, _ = await call("unreal_read", {"request": request})
                seen = compact["items"][:]
                page = compact
                while page["next_cursor"]:
                    page, _ = await call("unreal_read", {"request": {
                        **request, "cursor": page["next_cursor"],
                    }})
                    seen.extend(page["items"])
                assert {row["path"] for row in seen} == set(paths)
                delta, _ = await call("unreal_read", {"request": {
                    **request, "since": compact["read_id"],
                }})
                assert not delta["items"] and not delta["delta"]["removed_from_result"]
                for row in seen:
                    native = next(a for a in detail["actors"] if a["path"] == row["path"])
                    for key in ("bounds_cm", "editable", "edit_blockers", "instance_id"):
                        assert row[key] == native[key]
                report["compact"] = {
                    "actors": len(paths), "raw_actor_rows_bytes": encoded_bytes(detail["actors"]),
                    "projected_actor_rows_bytes": encoded_bytes(seen), "unchanged_delta_rows": 0,
                    "scope": "Row bytes only; not tokens or a workflow speed measurement.",
                }
                state = {key: status[key] for key in ("session_id", "world_path", "revision")}
                captures = []
                for label in ("before", "after"):
                    capture, response = await call("unreal_acceptance_capture", {
                        "protocol_id": "efficiency-smoke", "expected_state": state,
                        "max_dimension": 256,
                    })
                    images = [part for part in response.content if part.type == "image"]
                    assert len(images) == 1
                    (folder / f"{label}.png").write_bytes(base64.b64decode(images[0].data))
                    captures.append(capture["receipt_id"])
                compared, _ = await call("unreal_acceptance_compare", {
                    "baseline_id": captures[0], "candidate_id": captures[1],
                    "include_images": True,
                })
                assert compared["comparable"] and compared["status"] == "inconclusive"
                report["capture_comparison"] = compared
                final, _ = await call("unreal_status")
                assert all(final[key] == status[key] for key in (
                    "session_id", "world_path", "revision", "play_in_editor", "simulating"
                ))
    assert report["core"]["schema_bytes"] < report["all"]["schema_bytes"]
    report["ok"] = True
    report["sandbox_identity_verified"] = True
    (folder / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: report[key] for key in (
        "ok", "all", "core", "compact", "provider_requests", "scene_mutations"
    )}))


if __name__ == "__main__":
    asyncio.run(main())
