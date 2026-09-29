"""Live single-root hierarchy copy with nontrivial rotation in exact JevSandbox.

This supplements native multi-node tests; it does not establish live multi-node
attachment acceptance. Leaves two public fixture actors unsaved for inspection.
"""

import asyncio
import json
import os
import sys
import uuid

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client
from smoke_mesh_workflows import (
    CUBE,
    ROOT,
    SANDBOX,
    apply_once,
    call,
    context,
    mesh_preview,
    require,
    typed_edit,
    verify,
)

from jev_unreal.bridge import project_identity
from jev_unreal.config import Settings


async def main():
    settings = Settings.from_env()
    require(
        project_identity(settings.expected_project) == project_identity(str(SANDBOX)),
        "Only the exact repository sandbox is supported.",
    )
    params = StdioServerParameters(
        command=sys.executable,
        args=["-m", "jev_unreal", "serve"],
        env={
            **os.environ,
            "JEV_PROFILES_FILE": "",
            "JEV_PROFILE": "",
            "JEV_EXPECTED_PROJECT": str(SANDBOX),
            "JEV_BRIDGE_URL": settings.bridge_url,
            "JEV_BRIDGE_TOKEN": settings.bridge_token,
            "JEV_BRIDGE_TOKEN_FILE": "",
            "OPENROUTER_API_KEY": "",
            "TYPESAFE_API_KEY": "",
        },
    )
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()
            status = await context(session)
            require(status["bridge_version"] == "0.10.0", "Matching native plugin required.")
            require("mesh_attachment_copy" in status["capabilities"], "Missing capability.")
            prefix = "JevHierarchySmoke_" + uuid.uuid4().hex[:12]
            spawned = await typed_edit(session, [{
                "op": "spawn_static_mesh", "asset_path": CUBE,
                "label": prefix, "location": [-18000, 0, 100],
                "rotation": [13.1234, 55.1234, -17.4321], "scale": [1, 1, 1],
            }])
            source = spawned["actors"][0]["path"]
            snapshot = await call(session, "unreal_snapshot", {"actor_paths": [source]})
            proposal, _ = await mesh_preview(session, {
                "kind": "duplicate", "actor_paths": [source],
                "offset_cm": [250, 0, 0], "preserve_attachments": True,
            })
            applied = await apply_once(session, proposal["preview"]["plan_id"])
            checks = applied["verification_checks"]
            require(any(check["kind"] == "attachment" for check in checks),
                    "Apply omitted attachment checks.")
            count = await verify(session, checks, snapshot["identity"])
            metrics = (await session.call_tool("jev_status", {})).structuredContent
            require(metrics["decisions"]["requests_sent"] == 0, "Provider call was unexpected.")
            report = {
                "status": "passed", "scope": "Live single-root hierarchy copy",
                "multi_node_native_suite_required": True, "saved": False,
                "identity": snapshot["identity"], "checked_fields": count,
                "verification": applied["verification"], "provider_requests": 0,
            }
            (ROOT / "artifacts/hierarchy-single-root-v0.9.json").write_text(
                json.dumps(report, indent=2) + "\n", encoding="utf-8"
            )
            print(json.dumps(report, indent=2))


if __name__ == "__main__":
    asyncio.run(main())
