"""Exercise two trusted real source releases in disposable project descriptors.

No editor, compiler, network request or credential content is used. This checks
installation behavior on the current host, not clean-host Unreal acceptance.
The new workspace and private backup receipts are retained for local inspection.
"""

from __future__ import annotations

import argparse
import json
import platform
import subprocess
import sys
from pathlib import Path

from jev_unreal import setup
from jev_unreal.benchmarks import fingerprint
from jev_unreal.errors import JevError


def require(condition: bool) -> None:
    if not condition:
        raise JevError("setup_smoke_failed", "A source installation acceptance check failed.")


def snapshot(source: str | Path, destination: Path) -> tuple[Path, dict]:
    root, descriptor, records = setup._source(source)
    for name, expected in records.items():
        data = setup._read(root / name)
        require({"sha256": setup._digest(data), "bytes": len(data)} == expected)
        setup._atomic_write(destination / name, data)
    return destination, {
        "version": descriptor.get("VersionName"),
        "source_sha256": fingerprint(records),
        "files": len(records),
        "bytes": sum(record["bytes"] for record in records.values()),
    }


def verify_installed(project: Path, source: Path) -> None:
    _, _, expected = setup._source(source)
    destination = project.parent / "Plugins/JevEditor"
    manifest = setup._manifest(destination, project)
    require(manifest is not None and manifest["files"] == expected)
    require(all(setup._record(destination / name) == record for name, record in expected.items()))


def interrupted_upgrade(project: Path, current: Path) -> str:
    script = """
import os, sys
from pathlib import Path
from jev_unreal import setup
project, current = map(Path, sys.argv[1:])
manifest = project.parent / 'Plugins/JevEditor' / setup.MANIFEST
original = setup._atomic_write
def crash_after_manifest(path, data):
    original(path, data)
    if path == manifest:
        os._exit(73)
setup._atomic_write = crash_after_manifest
setup.apply_plan(setup.plan_install(project, current))
sys.exit(74)
"""
    result = subprocess.run(
        [sys.executable, "-c", script, str(project), str(current)],
        capture_output=True,
        timeout=120,
        check=False,
    )
    require(result.returncode == 73)
    return (project.parent / "Plugins/.JevEditor.install.lock").read_text(encoding="utf-8")


def lifecycle(workspace: Path, kind: str, baseline: Path, current: Path) -> dict:
    project = workspace / kind / "Acceptance.uproject"
    descriptor = {
        "FileVersion": 3,
        "EngineAssociation": "5.8",
        "Description": "Disposable setup acceptance",
        "Plugins": [{"Name": "ExampleDisabledPlugin", "Enabled": False}],
    }
    if kind == "cpp_descriptor":
        descriptor["Modules"] = [{"Name": "Acceptance", "Type": "Runtime"}]
    setup._atomic_write(project, setup._json_bytes(descriptor))
    untouched = project.parent / "Content/Keep.txt"
    setup._atomic_write(untouched, b"Unrelated project content must remain unchanged.\n")
    destination = project.parent / "Plugins/JevEditor"
    checks = []

    setup.apply_plan(setup.plan_install(project, baseline))
    verify_installed(project, baseline)
    checks.append("baseline_install_exact_hashes")

    old_records = setup._source(baseline)[2]
    victim_name = next(name for name in old_records if name.endswith(".cpp"))
    victim = destination / victim_name
    original = victim.read_bytes()
    victim.write_bytes(b"// Disposable local modification\n" + original)
    plan = setup.plan_install(project, current)
    require(not plan["can_apply"])
    try:
        setup.apply_plan(plan)
    except JevError:
        pass
    else:
        require(False)
    require(victim.read_bytes() == b"// Disposable local modification\n" + original)
    setup._atomic_write(victim, original)
    checks.append("upgrade_preserves_modified_source")

    operation_id = interrupted_upgrade(project, current)
    recovery = setup.plan_recovery(project, operation_id)
    require(recovery["can_apply"])
    require(recovery["mode"] == "restore_original_bytes")
    setup.apply_recovery_plan(recovery)
    verify_installed(project, baseline)
    checks.append("process_exit_upgrade_restores_exact_baseline")

    setup.apply_plan(setup.plan_install(project, current))
    verify_installed(project, current)
    checks.append("upgrade_exact_hashes")

    new_records = setup._source(current)[2]
    repair_name = next(name for name in new_records if name.endswith(".cpp"))
    (destination / repair_name).unlink()
    repair = setup.plan_install(project, current)
    require([action["path"] for action in repair["actions"]] == [repair_name])
    setup.apply_plan(repair)
    verify_installed(project, current)
    checks.append("missing_owned_source_repaired")

    generated = destination / "Binaries/Win64/Keep.txt"
    setup._atomic_write(generated, b"Synthetic generated-file preservation marker.\n")
    result = setup.apply_plan(setup.plan_uninstall(project))
    require(result["status"] == "uninstalled")
    require(setup._json(project) == descriptor)
    require(all(not (destination / name).exists() for name in new_records))
    require(not (destination / setup.MANIFEST).exists())
    require(untouched.read_bytes() == b"Unrelated project content must remain unchanged.\n")
    require(generated.read_bytes() == b"Synthetic generated-file preservation marker.\n")
    require(not (destination.parent / ".JevEditor.install.lock").exists())
    checks.extend(("uninstall_restores_project_entries", "uninstall_preserves_unrelated_files"))
    return {
        "project_kind": kind,
        "checks_passed": checks,
        "check_count": len(checks),
        "engine_build_performed": False,
        "runtime_verified": False,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-plugin", required=True)
    parser.add_argument("--source-plugin", required=True)
    parser.add_argument(
        "--workspace", required=True, help="New disposable directory; must not exist"
    )
    parser.add_argument("--report", required=True, help="New sanitized JSON report")
    args = parser.parse_args(argv)
    try:
        workspace = setup._absolute(args.workspace)
        report_path = setup._absolute(args.report)
        if report_path.exists():
            raise JevError("setup_conflict", "Choose a new report filename.")
        workspace.mkdir(parents=True, exist_ok=False)
        baseline, baseline_info = snapshot(args.baseline_plugin, workspace / "sources/baseline")
        current, current_info = snapshot(args.source_plugin, workspace / "sources/current")
        require(baseline_info["source_sha256"] != current_info["source_sha256"])
        report = {
            "schema_version": 1,
            "provenance": "existing_development_host_source_lifecycle",
            "platform": platform.system(),
            "python": platform.python_version(),
            "baseline": baseline_info,
            "current": current_info,
            "projects": [
                lifecycle(workspace, kind, baseline, current)
                for kind in ("blueprint_descriptor", "cpp_descriptor")
            ],
            "fresh_host_verified": False,
            "engine_build_performed": False,
            "runtime_verified": False,
            "provider_requests": 0,
        }
        with report_path.open("x", encoding="utf-8") as handle:
            json.dump(report, handle, indent=2)
            handle.write("\n")
        print(json.dumps(report, indent=2))
        return 0
    except (JevError, OSError, subprocess.TimeoutExpired):
        print(
            json.dumps(
                {
                    "error": "setup_smoke_failed",
                    "message": "Source lifecycle did not complete. "
                    "Retained local receipts need inspection.",
                }
            ),
            file=sys.stderr,
        )
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
