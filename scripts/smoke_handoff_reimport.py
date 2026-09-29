"""Reimport the saved public calibration mesh once in this repository's JevSandbox.

Prepare the owned fixture and review both calibration-existing alias approvals as
documented in docs/HANDOFF_IMPORT.md. Start with a freshly loaded, clean asset.
This applies one guarded reimport, verifies dimensions/pivot/material slots, and
checks that its saved package bytes remain unchanged. No save or provider request
is made. A successful reimport leaves the in-memory asset dirty; do not rerun it
or automatically save/discard editor changes. Reports contain summaries only.
"""

import asyncio
import hashlib
import json

from mcp import ClientSession
from smoke_roadmap_completion import ROOT, SANDBOX, connect
from unreal_handoff_fixture import ASSET_PATH

from jev_unreal.config import Settings
from jev_unreal.team_policy import project_identity


async def call(session: ClientSession, name: str, arguments: dict | None = None) -> dict:
    """Require structured success without printing raw server responses or errors."""
    reply = await session.call_tool(name, arguments or {})
    data = reply.structuredContent
    if reply.isError or not isinstance(data, dict) or data.get("ok") is not True:
        raise RuntimeError("The bounded reimport smoke tool call failed.")
    if not isinstance(data.get("result"), dict):
        raise RuntimeError("The bounded reimport smoke tool result was invalid.")
    return data["result"]


def saved_mesh_sha256() -> str:
    """Hash only the fixed, separately prepared sandbox package without loading it."""
    path = SANDBOX.parent / "Content/JevHandoff/SM_Calibration.uasset"
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


async def main() -> None:
    settings = Settings.from_env()
    if not settings.expected_project or project_identity(
        settings.expected_project
    ) != project_identity(str(SANDBOX)):
        raise RuntimeError("Only this repository's JevSandbox is allowed.")
    before = await asyncio.to_thread(saved_mesh_sha256)
    async with connect() as session:
        status = await call(session, "unreal_status")
        if project_identity(status["project_file"]) != project_identity(str(SANDBOX)):
            raise RuntimeError("The connected editor is not this repository's JevSandbox.")
        # Loading an existing saved package is distinct from saving an imported asset.
        await call(session, "unreal_asset_details", {"path": ASSET_PATH})
        status = await call(session, "unreal_status")
        preview = await call(
            session,
            "unreal_handoff_import_preview",
            {
                "alias": "calibration-existing",
                "operation": "reimport",
                "expected_state": {
                    key: status[key] for key in ("session_id", "world_path", "revision")
                },
            },
        )
        if preview.get("asset_path") != ASSET_PATH or preview.get("operation") != "reimport":
            raise RuntimeError("The preview does not target the owned calibration reimport.")
        result = await call(session, "unreal_handoff_import_apply", {"plan_id": preview["plan_id"]})
        if result.get("status") != "verified":
            raise RuntimeError("Fresh dimensions, pivot and material-slot verification failed.")
        after = await asyncio.to_thread(saved_mesh_sha256)
        if before != after:
            raise RuntimeError("The saved calibration package changed unexpectedly.")
    report = {
        "status": "verified",
        "operation": "reimport",
        "alias": "calibration-existing",
        "asset_path": ASSET_PATH,
        "measurements": "dimensions_pivot_material_slots_verified",
        "disk_hash_unchanged": True,
        "save_requested": False,
        "provider_requests": 0,
    }
    output = ROOT / "artifacts/roadmap-reimport-smoke.json"
    await asyncio.to_thread(
        output.write_text, json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(report))


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except Exception:
        # Do not print private configuration, paths or raw native/provider responses.
        print(
            json.dumps({"status": "failed", "guidance": "Inspect the owned fixture; do not retry."})
        )
        raise SystemExit(1) from None
