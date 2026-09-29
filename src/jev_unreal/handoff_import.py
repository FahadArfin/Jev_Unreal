"""Reviewed imports from locally configured, retained-source Blender bundles."""

import asyncio
import math
import time
from pathlib import Path
from typing import Annotated, Any, Literal

from mcp.server.fastmcp import FastMCP
from mcp.types import ToolAnnotations
from pydantic import Field, ValidationError, model_validator

from .bridge import UnrealBridge
from .domain_workflows import Strict
from .errors import JevError
from .handoff import HandoffAsset, HandoffManifest, compare_asset, inspect_bundle
from .setup import _absolute, _json
from .team_policy import digest, project_identity
from .workflows import ExpectedState

Alias = Annotated[str, Field(min_length=1, max_length=64, pattern=r"^[A-Za-z0-9_-]+$")]
PlanId = Annotated[str, Field(min_length=1, max_length=64)]


class Bundle(Strict):
    id: Alias
    directory: str = Field(min_length=1, max_length=2048)
    asset_id: str = Field(min_length=1, max_length=128, pattern=r"^[A-Za-z0-9_.-]+$")


class ImportConfig(Strict):
    version: Literal[1]
    project_file: str = Field(min_length=1, max_length=2048)
    bundles: list[Bundle] = Field(max_length=32)

    @model_validator(mode="after")
    def unique(self):
        if len({b.id for b in self.bundles}) != len(self.bundles):
            raise ValueError("Bundle aliases must be unique.")
        if not Path(self.project_file).is_absolute() or any(
            not Path(b.directory).is_absolute() for b in self.bundles
        ):
            raise ValueError("Project and bundle directories require absolute local paths.")
        return self


class HandoffImporter:
    def __init__(self, bridge: UnrealBridge, config_file: str = "", clock=time.monotonic):
        self.bridge = bridge
        self.config_file = config_file
        self.clock = clock
        self.plans: dict[str, dict] = {}
        self.lock = asyncio.Lock()

    def _config(self) -> ImportConfig:
        if not self.config_file:
            raise JevError("handoff_disabled", "Configure JEV_HANDOFF_CONFIG locally first.")
        try:
            config = ImportConfig.model_validate(_json(_absolute(self.config_file), 128 * 1024))
            expected = self.bridge.settings.expected_project
            if not expected or project_identity(config.project_file) != project_identity(expected):
                raise JevError(
                    "wrong_project", "Handoff configuration must bind the expected project."
                )
            _absolute(config.project_file)
            return config
        except (OSError, ValidationError, ValueError):
            raise JevError(
                "configuration", "Handoff configuration is missing or invalid."
            ) from None

    def _bundle(self, config: ImportConfig, alias: str) -> dict:
        entry = next((b for b in config.bundles if b.id == alias), None)
        if entry is None:
            raise JevError("bundle_not_allowed", "Select an explicitly configured bundle alias.")
        try:
            root = _absolute(entry.directory)
            manifest = HandoffManifest.model_validate(_json(root / "handoff.json", 1024 * 1024))
            asset = next((a for a in manifest.assets if a.asset_id == entry.asset_id), None)
            if asset is None:
                raise JevError("invalid_bundle", "Configured asset ID is absent from handoff.json.")
            mesh = next(f for f in manifest.files if f.path == asset.mesh_file)
            if Path(mesh.path).suffix.lower() != ".fbx" or mesh.bytes > 64 * 1024 * 1024:
                raise JevError("invalid_bundle", "Reviewed import requires a bounded FBX mesh.")
            checked = inspect_bundle(manifest, root)
            if checked["status"] != "passed":
                raise JevError("bundle_changed", "Retained source, mesh or texture hashes failed.")
            return {
                "alias": alias,
                "asset": asset,
                "source_sha256": mesh.sha256,
                "digest": digest(
                    {"config": config.model_dump(), "manifest": manifest.model_dump()}
                ),
                "license": manifest.source_license,
                "provenance": manifest.provenance,
                "files_verified": len(manifest.files),
            }
        except (OSError, ValidationError, ValueError, StopIteration):
            raise JevError(
                "invalid_bundle", "Bundle manifest or recorded files are invalid."
            ) from None

    async def bundles(self) -> dict:
        async with self.lock:
            return await self._bundles()

    async def _bundles(self) -> dict:
        config = await asyncio.to_thread(self._config)
        native = await self.bridge.call("handoff_manifest")
        aliases = native.get("aliases")
        if not isinstance(aliases, list) or len(aliases) > 64:
            raise JevError("bridge_error", "Native handoff policy returned invalid aliases.")
        rows = []
        for entry in config.bundles:
            approved = [r for r in aliases if isinstance(r, dict) and r.get("alias") == entry.id]
            rows.append(
                {
                    "alias": entry.id,
                    "asset_id": entry.asset_id,
                    "native_approved": len(approved) == 1 and native.get("enabled") is True,
                    "asset_path": approved[0].get("asset_path") if len(approved) == 1 else None,
                }
            )
        return {
            "enabled": native.get("enabled") is True,
            "bundles": rows,
            "scope": "Configured aliases only; bundle hashes are checked during preview/apply.",
        }

    async def preview(self, alias: str, operation: str, expected_state: ExpectedState) -> dict:
        async with self.lock:
            return await self._preview(alias, operation, expected_state)

    async def _preview(self, alias: str, operation: str, expected_state: ExpectedState) -> dict:
        config = await asyncio.to_thread(self._config)
        bundle = await asyncio.to_thread(self._bundle, config, alias)
        native = await self.bridge.call("handoff_manifest")
        aliases = native.get("aliases")
        if native.get("enabled") is not True or not isinstance(aliases, list) or len(aliases) > 64:
            raise JevError(
                "handoff_disabled", "Native project handoff policy is disabled or invalid."
            )
        rows = [r for r in aliases if isinstance(r, dict) and r.get("alias") == alias]
        asset: HandoffAsset = bundle["asset"]
        if (
            len(rows) != 1
            or rows[0].get("asset_path") != asset.unreal_asset_path
            or rows[0].get("source_sha256") != bundle["source_sha256"]
        ):
            raise JevError(
                "bundle_mismatch", "Native alias path/hash differs from the local manifest."
            )
        now = self.clock()
        self.plans = {
            key: value for key, value in self.plans.items() if now - value["created"] < 120
        }
        if len(self.plans) >= 64:
            raise JevError("too_many_plans", "Wait for old import previews to expire.")
        result = await self.bridge.call(
            "handoff_preview",
            {
                "alias": alias,
                "operation": operation,
                "expected_state": expected_state.model_dump(),
            },
        )
        plan_id = result.get("plan_id")
        expiry = result.get("expires_in_seconds")
        if (
            not isinstance(plan_id, str)
            or not 1 <= len(plan_id) <= 64
            or result.get("asset_path") != asset.unreal_asset_path
            or result.get("source_sha256") != bundle["source_sha256"]
            or result.get("operation") != operation
            or result.get("alias") != alias
            or type(expiry) not in (int, float)
            or not math.isfinite(expiry)
            or not 0 < expiry <= 120
            or not isinstance(result.get("project_file"), str)
            or project_identity(result["project_file"]) != project_identity(config.project_file)
            or any(result.get(key) != value for key, value in expected_state.model_dump().items())
        ):
            raise JevError("bridge_error", "Native preview does not match the reviewed bundle.")
        self.plans[plan_id] = {
            **bundle,
            "created": now,
            "operation": operation,
            "expected_state": expected_state.model_dump(),
            "expiry": expiry,
        }
        return {
            **result,
            "retained_source_verified": True,
            "files_verified": bundle["files_verified"],
            "license": bundle["license"],
            "provenance": bundle["provenance"],
            "contract": asset.model_dump(),
            "review": "Review source license, asset replacement and contract. Apply once; "
            "inspect fresh measurements afterward. Importers run trusted native code.",
        }

    async def apply(self, plan_id: str) -> dict:
        async with self.lock:
            return await self._apply(plan_id)

    async def _apply(self, plan_id: str) -> dict:
        plan = self.plans.pop(plan_id, None)
        if plan is None:
            raise JevError(
                "unknown_plan", "Import plan is unknown or already consumed; do not retry."
            )
        if self.clock() - plan["created"] >= plan["expiry"]:
            raise JevError("stale_plan", "Import preview expired; inspect and review again.")
        config = await asyncio.to_thread(self._config)
        current = await asyncio.to_thread(self._bundle, config, plan["alias"])
        if current["digest"] != plan["digest"]:
            raise JevError("stale_plan", "Bundle contract or local configuration changed.")
        result = await self.bridge.call("handoff_apply", {"plan_id": plan_id})
        asset: HandoffAsset = plan["asset"]
        if (
            result.get("asset_path") != asset.unreal_asset_path
            or result.get("source_sha256") != plan["source_sha256"]
            or result.get("plan_id") != plan_id
            or result.get("alias") != plan["alias"]
            or result.get("operation") != plan["operation"]
            or not isinstance(result.get("project_file"), str)
            or project_identity(result["project_file"]) != project_identity(config.project_file)
            or any(
                result.get(key) != plan["expected_state"][key]
                for key in ("session_id", "world_path")
            )
        ):
            return {
                "status": "needs_review",
                "native_result": result,
                "verification_error": "Native import identity differs from reviewed bundle.",
            }
        if result.get("status") != "imported":
            return {
                "status": "needs_review",
                "native_result": result,
                "verification_error": "Native import did not report imported.",
            }
        try:
            observed = await self.bridge.call(
                "workflow_inspect",
                {
                    "kind": "asset_diagnosis",
                    "target_path": asset.unreal_asset_path,
                    "dependency_depth": 1,
                },
            )
            if not isinstance(observed.get("project_file"), str) or project_identity(
                observed["project_file"]
            ) != project_identity(self.bridge.settings.expected_project):
                raise JevError("wrong_project", "Post-import observation changed project.")
            if any(
                observed.get(key) != plan["expected_state"][key]
                for key in ("session_id", "world_path")
            ) or not observed.get("revision"):
                raise JevError("wrong_project", "Post-import observation changed editor identity.")
            verification = compare_asset(asset, observed)
            return {
                "status": "verified" if verification["status"] == "passed" else "needs_review",
                "native_result": result,
                "verification": verification,
                "retained_source_verified": True,
                "scope": "Fresh dimensions, pivot-relative bounds and ordered slots only. "
                "Visuals, collision and gameplay still need acceptance; no save is requested.",
            }
        except JevError as exc:
            return {
                "status": "needs_review",
                "native_result": result,
                "verification_error": exc.as_dict()["error"],
            }


def register_handoff_import_tools(
    server: FastMCP, bridge: UnrealBridge, config_file: str = ""
) -> None:
    importer = HandoffImporter(bridge, config_file)

    @server.tool(annotations=ToolAnnotations(readOnlyHint=True, destructiveHint=False))
    async def unreal_handoff_bundles() -> dict[str, Any]:
        """List local retained-source bundle aliases and their native project approvals.

        Configure JEV_HANDOFF_CONFIG locally. Tool arguments never accept filesystem paths.
        This discovery does not hash files, import assets or grant permission.
        """
        try:
            return {"ok": True, "result": await importer.bundles()}
        except JevError as exc:
            return exc.as_dict()

    @server.tool(
        annotations=ToolAnnotations(
            readOnlyHint=False,
            destructiveHint=False,
            idempotentHint=False,
        )
    )
    async def unreal_handoff_import_preview(
        alias: Alias,
        operation: Literal["import", "reimport"],
        expected_state: ExpectedState,
    ) -> dict[str, Any]:
        """Verify retained Blender files and review one exact approved FBX import/reimport.

        Alias, mesh hash and /Game target must agree in local bundle and native project policy.
        Reimport replaces a clean, already-loaded approved mesh. No arbitrary source paths,
        importer settings, materials/textures imports or automatic saves. Plan expires in 120s.
        """
        try:
            return {"ok": True, "result": await importer.preview(alias, operation, expected_state)}
        except JevError as exc:
            return exc.as_dict()

    @server.tool(
        annotations=ToolAnnotations(
            readOnlyHint=False,
            destructiveHint=True,
            idempotentHint=False,
        )
    )
    async def unreal_handoff_import_apply(plan_id: PlanId) -> dict[str, Any]:
        """Consume a reviewed import, rehash retained sources and verify native measurements.

        Changed/expired/failed plans are consumed. No retries or complete rollback guarantee.
        'verified' means dimensions/pivot/ordered slots match; it is not visual acceptance.
        Native receipts can remain uncertain after timeout. Inspect target; never blindly repeat.
        """
        try:
            return {"ok": True, "result": await importer.apply(plan_id)}
        except JevError as exc:
            return exc.as_dict()
