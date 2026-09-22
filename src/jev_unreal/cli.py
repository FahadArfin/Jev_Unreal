import argparse
import asyncio
import json
import sys
from pathlib import Path

from . import __version__
from .bridge import UnrealBridge
from .catalog import ToolCatalog
from .config import Settings
from .decision import DecisionClient
from .errors import JevError
from .layouts import LAYOUT_CATALOG
from .verification import verify_fresh

WORKFLOW_CAPABILITIES = {
    "actor_details",
    "preview_expected_state",
    "set_material",
    "set_metadata",
    "frame_views",
}

PROJECT_WORKFLOW_FEATURES = {
    "mesh_editing": ("Mesh replacement and duplication", {"replace_mesh", "duplicate_mesh"}),
    "plan_review": ("Native plan review", {"pending_plans", "plan_status"}),
    "blueprint_inspection": ("Blueprint inspection", {"blueprint_inspect"}),
    "asset_dependencies": ("Asset dependencies", {"asset_dependencies"}),
    "import_provenance": ("Asset import provenance", {"asset_import_info"}),
    "asset_validation": (
        "Project asset validation",
        {"validation_rules", "validation_start", "validation_job", "validation_cancel"},
    ),
    "functional_testing": (
        "Project functional testing",
        {"functional_tests", "functional_start", "functional_job", "functional_cancel"},
    ),
}
PROJECT_WORKFLOW_CAPABILITIES = set().union(
    *(capabilities for _, capabilities in PROJECT_WORKFLOW_FEATURES.values())
)


def read_checks(path: Path) -> dict:
    with path.open("rb") as handle:
        content = handle.read(65537)
    if len(content) > 65536:
        raise JevError("request_too_large", "Verification file exceeds 64 KiB.")
    data = json.loads(content)
    if (
        not isinstance(data, dict)
        or "checks" not in data
        or set(data) - {"checks", "expected_identity", "expected_revision"}
    ):
        raise JevError("invalid_request", "Use checks and optional expected_identity/revision.")
    return data


def setup_command(args) -> dict:
    from .setup import apply_plan, inspect_setup, plan_install, plan_uninstall, read_plan

    if args.setup_command == "inspect":
        return inspect_setup(
            args.project,
            source_plugin=args.source_plugin,
            engine_root=args.engine_root,
            port=args.port,
        )
    if args.setup_command == "apply":
        return apply_plan(read_plan(args.file))
    result = (
        plan_install(args.project, args.source_plugin)
        if args.setup_command == "plan"
        else plan_uninstall(args.project)
    )
    if args.output:
        # Refuse to overwrite an unrelated file or an earlier review without explicit removal.
        output = Path(args.output)
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open("x", encoding="utf-8") as handle:
            json.dump(result, handle, indent=2, allow_nan=False)
            handle.write("\n")
    return result


async def run_command(args, settings: Settings) -> dict:
    if args.command == "layouts":
        return LAYOUT_CATALOG
    if args.command == "catalog":
        catalog = ToolCatalog(Path(settings.catalog_file) if settings.catalog_file else None)
        await catalog.refresh()
        if args.catalog_command == "search":
            return {
                "catalog": catalog.status(),
                "hits": [
                    hit.model_dump(mode="json")
                    for hit in catalog.search(args.query, limit=args.limit)
                ],
            }
        if args.catalog_command == "get":
            return catalog.get(args.tool_id).model_dump(mode="json")
        return catalog.status()
    if args.command == "doctor":
        bridge = UnrealBridge(settings)
        try:
            status = await bridge.call("status")
            editor = {"ready": True, "identity": status}
        except JevError as exc:
            editor = {"ready": False, "error": exc.as_dict()["error"]}
        finally:
            await bridge.close()
        capabilities = editor.get("identity", {}).get("capabilities", [])
        available = (
            {v for v in capabilities if isinstance(v, str)}
            if isinstance(capabilities, list)
            else set()
        )
        missing_capabilities = sorted(WORKFLOW_CAPABILITIES - available)
        missing_project_capabilities = sorted(PROJECT_WORKFLOW_CAPABILITIES - available)
        project_features = {
            feature: {
                "label": label,
                "ready": capabilities <= available,
                "missing_capabilities": sorted(capabilities - available),
            }
            for feature, (label, capabilities) in PROJECT_WORKFLOW_FEATURES.items()
        }
        catalog = ToolCatalog(Path(settings.catalog_file) if settings.catalog_file else None)
        return {
            "version": __version__,
            "ready": editor["ready"]
            and bool(settings.expected_project)
            and not missing_capabilities
            and not missing_project_capabilities,
            "editor": editor,
            "workflow_compatibility": {
                "ready": not missing_capabilities,
                "missing_capabilities": missing_capabilities,
            },
            "project_workflow_compatibility": {
                "ready": not missing_project_capabilities,
                "missing_capabilities": missing_project_capabilities,
                "features": project_features,
                "policy_verified": False,
                "note": "Capability support only. Project validation/functional-test policies "
                "may remain disabled; doctor does not enable them or execute project code.",
            },
            "project_bound": bool(settings.expected_project),
            "connection_profile": settings.profile_id or None,
            "bridge_url": settings.bridge_url,
            "provider": {
                "configured": bool(settings.api_key),
                "model": settings.model,
                "tested": False,
                "required_for_local_tools": False,
            },
            "catalog": catalog.status(),
            "next_steps": (
                []
                if editor["ready"]
                else [
                    "Initialize the bridge environment, build and launch the intended editor.",
                ]
            )
            + (
                []
                if settings.expected_project
                else [
                    "Set JEV_EXPECTED_PROJECT to the intended absolute .uproject before edits.",
                ]
            )
            + (
                ["Rebuild and relaunch the matching JevEditor plugin for verified editing tools."]
                if editor["ready"] and missing_capabilities
                else []
            )
            + (
                [
                    "Rebuild and relaunch the matching JevEditor 0.5 or later for: "
                    + ", ".join(
                        feature["label"]
                        for feature in project_features.values()
                        if not feature["ready"]
                    )
                    + "."
                ]
                if editor["ready"] and missing_project_capabilities
                else []
            ),
        }
    if args.command in {"status", "context", "inspect", "verify"}:
        bridge = UnrealBridge(settings)
        try:
            if args.command == "inspect":
                return await bridge.call("actor_details", {"actor_paths": args.actor_paths})
            if args.command == "verify":
                payload = await asyncio.to_thread(read_checks, Path(args.file))
                return await verify_fresh(bridge, **payload)
            return await bridge.call(args.command)
        finally:
            await bridge.close()
    client = DecisionClient(settings)
    try:
        if args.command == "smoke":
            return await client.decide(
                {"engine": "Unreal", "message": "Compiler error C2065: undeclared identifier"},
                {
                    "category": {
                        "type": "choice",
                        "instructions": "Classify this diagnostic.",
                        "criteria": {
                            "compile": "C++ compilation error",
                            "art": "Visual art feedback",
                        },
                    }
                },
            )
        path = Path(args.file)
        if (await asyncio.to_thread(path.stat)).st_size > 65536:
            raise JevError("request_too_large", "Input file exceeds 64 KiB.")
        data = json.loads(await asyncio.to_thread(path.read_text, encoding="utf-8"))
        return await client.decide(data["state"], data["questions"])
    finally:
        await client.close()


def main():
    parser = argparse.ArgumentParser(description="Jev decisions + guarded Unreal editor MCP")
    parser.add_argument("--version", action="version", version=__version__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("serve", help="Run the MCP stdio server")
    commands.add_parser("status", help="Inspect the configured Unreal editor")
    commands.add_parser("context", help="Read compact editor context without a model key")
    commands.add_parser("inspect", help="Inspect exact actor paths without editing").add_argument(
        "actor_paths", nargs="+"
    )
    commands.add_parser(
        "verify", help="Check current actors against a bounded JSON checks file"
    ).add_argument("file")
    commands.add_parser("doctor", help="Check editor connectivity/configuration; no cloud calls")
    commands.add_parser("layouts", help="Show available measured blockout recipes")
    profiles = commands.add_parser("profiles", help="List explicitly configured editor connections")
    profile_commands = profiles.add_subparsers(dest="profiles_command", required=True)
    profile_commands.add_parser(
        "list", help="Read names and endpoints without tokens"
    ).add_argument("file")
    setup = commands.add_parser("setup", help="Inspect or review local plugin installation changes")
    setup_commands = setup.add_subparsers(dest="setup_command", required=True)
    inspection = setup_commands.add_parser("inspect", help="Read-only local setup diagnostics")
    inspection.add_argument("--project", required=True)
    inspection.add_argument("--source-plugin")
    inspection.add_argument("--engine-root")
    inspection.add_argument("--port", type=int, default=9845)
    install = setup_commands.add_parser("plan", help="Preview source plugin install/upgrade")
    install.add_argument("--project", required=True)
    install.add_argument("--source-plugin", required=True)
    install.add_argument("--output")
    uninstall = setup_commands.add_parser("uninstall-plan", help="Preview removal of owned files")
    uninstall.add_argument("--project", required=True)
    uninstall.add_argument("--output")
    setup_commands.add_parser("apply", help="Apply an unchanged reviewed JSON plan").add_argument(
        "file"
    )
    catalog = commands.add_parser("catalog", help="Discover explicitly configured local MCP tools")
    catalog_commands = catalog.add_subparsers(dest="catalog_command", required=True)
    catalog_commands.add_parser("status", help="Refresh and summarize the configured catalog")
    search = catalog_commands.add_parser("search", help="Refresh and search tool descriptions")
    search.add_argument("query")
    search.add_argument("--limit", type=int, default=8)
    catalog_commands.add_parser("get", help="Refresh and retrieve an exact schema").add_argument(
        "tool_id"
    )
    commands.add_parser("smoke", help="Make one small real provider request")
    commands.add_parser("decide", help="Evaluate a JSON state/questions file").add_argument("file")
    args = parser.parse_args()
    try:
        if args.command == "setup":
            print(json.dumps(setup_command(args), indent=2, allow_nan=False))
            return
        if args.command == "profiles":
            from .profiles import summaries

            print(json.dumps({"profiles": summaries(args.file)}, indent=2, allow_nan=False))
            return
        settings = Settings.from_env()
        if args.command == "serve":
            from .server import create_server

            create_server(settings).run(transport="stdio")
        else:
            result = asyncio.run(run_command(args, settings))
            print(json.dumps(result, indent=2, allow_nan=False))
            if args.command == "doctor" and not result["ready"]:
                raise SystemExit(1)
            if args.command == "verify" and result["status"] != "passed":
                raise SystemExit(1)
    except JevError as exc:
        print(json.dumps(exc.as_dict()), file=sys.stderr)
        raise SystemExit(1) from None
    except (OSError, ValueError, KeyError, TypeError):
        print(
            '{"ok":false,"error":{"code":"input_error","message":"Invalid input file."}}',
            file=sys.stderr,
        )
        raise SystemExit(1) from None
