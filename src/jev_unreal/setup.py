"""Local, reviewable source-plugin installation; no credentials or subprocess execution.

Only files enumerated in an installer-owned manifest can be replaced or removed.
Plans describe exact byte hashes and are recomputed before any write. Backups are
retained in the selected project's Plugins directory, including on failed attempts.
"""

import hashlib
import importlib.metadata
import json
import os
import platform
import shutil
import socket
import stat
import sys
import uuid
from contextlib import contextmanager
from pathlib import Path, PurePosixPath

from .errors import JevError

OWNER = "https://github.com/FahadArfin/Jev_Unreal"
MANIFEST = ".jev-install.json"
MAX_FILES = 512
MAX_FILE_BYTES = 2 * 1024 * 1024
MAX_TOTAL_BYTES = 32 * 1024 * 1024
MAX_JSON_BYTES = 1024 * 1024
_EXTENSIONS = {".h", ".hpp", ".cpp", ".cs", ".inl", ".c", ".mm"}


def _error(message: str, code: str = "setup_conflict"):
    raise JevError(code, message)


def _json_bytes(value: dict) -> bytes:
    return (json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False) + "\n").encode()


def _digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _absolute(value: str | Path) -> Path:
    # Do not resolve before inspecting: resolve() would silently follow links.
    if str(value).startswith(("\\\\", "//")):
        _error("Setup paths must be local, not network shares.")
    raw = Path(value).expanduser()
    if not raw.is_absolute():
        raw = Path.cwd() / raw
    if ".." in raw.parts:
        _error("Use an absolute path without parent traversal.", "invalid_request")
    path = Path(os.path.abspath(raw))
    _safe_path(path)
    return path


def _safe_path(path: Path) -> None:
    if str(path).startswith(("\\\\", "//")):
        _error("Setup paths must be local, not network shares.")
    for component in (*reversed(path.parents), path):
        try:
            metadata = component.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(metadata.st_mode) or getattr(metadata, "st_file_attributes", 0) & 0x400:
            _error("Symbolic links and Windows reparse points are not supported for setup paths.")
        if component != path and not stat.S_ISDIR(metadata.st_mode):
            _error("A setup path's parent is not a directory.")


def _read(path: Path, limit: int = MAX_FILE_BYTES) -> bytes:
    _safe_path(path)
    metadata = path.stat()
    if not stat.S_ISREG(metadata.st_mode) or metadata.st_size > limit:
        _error("A setup file is not a regular file or exceeds the size limit.")
    with path.open("rb") as handle:
        data = handle.read(limit + 1)
    if len(data) > limit:
        _error("A setup file grew beyond the size limit.")
    return data


def _json(path: Path, limit: int = MAX_JSON_BYTES) -> dict:
    def unique_object(pairs):
        value = {}
        for key, item in pairs:
            if key in value:
                raise ValueError("Duplicate JSON key")
            value[key] = item
        return value

    def reject_constant(_value):
        raise ValueError("Non-finite JSON value")

    try:
        result = json.loads(
            _read(path, limit).decode("utf-8-sig"),
            object_pairs_hook=unique_object,
            parse_constant=reject_constant,
        )
    except (ValueError, UnicodeError, RecursionError):
        _error("A setup JSON file is malformed.", "invalid_request")
    if not isinstance(result, dict):
        _error("A setup JSON file must contain an object.", "invalid_request")
    return result


def _record(path: Path) -> dict | None:
    _safe_path(path)
    if not path.exists():
        return None
    data = _read(path)
    return {"sha256": _digest(data), "bytes": len(data)}


def _relative(value: str) -> str:
    if not isinstance(value, str) or not value or len(value) > 512:
        _error("An owned plugin path is invalid.")
    parts = PurePosixPath(value).parts
    if (
        value != "/".join(parts)
        or "\\" in value
        or ":" in value
        or any(p in {".", ".."} or p.endswith((" ", ".")) for p in parts)
        or any(
            p.split(".", 1)[0].upper()
            in {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"}
            | {prefix + suffix for prefix in ("COM", "LPT") for suffix in "123456789¹²³"}
            for p in parts
        )
        or any(c in '<>"|?*' for c in value)
        or any(ord(c) < 32 for c in value)
        or PurePosixPath(value).is_absolute()
        or not (
            value == "JevEditor.uplugin"
            or (parts[0] == "Source" and len(parts) > 1 and Path(value).suffix in _EXTENSIONS)
            or value == "Resources/Icon128.png"
        )
    ):
        _error("The installer manifest contains an unsupported plugin path.")
    return value


def _project(value: str | Path) -> tuple[Path, dict]:
    path = _absolute(value)
    if path.suffix.lower() != ".uproject":
        _error("Select an existing .uproject file.", "invalid_request")
    data = _json(path)
    if type(data.get("FileVersion")) is not int or data["FileVersion"] < 1:
        _error("The selected project has no valid FileVersion.", "invalid_request")
    plugins = data.get("Plugins", [])
    if not isinstance(plugins, list) or len(plugins) > 512:
        _error("The project's Plugins field is invalid.")
    if any(not isinstance(item, dict) or not isinstance(item.get("Name"), str) for item in plugins):
        _error("The project contains a malformed plugin entry.")
    matches = [item for item in plugins if item["Name"].casefold() == "jeveditor"]
    if len(matches) > 1 or (matches and matches[0]["Name"] != "JevEditor"):
        _error("The project contains ambiguous JevEditor plugin entries.")
    if matches and "Enabled" in matches[0] and type(matches[0]["Enabled"]) is not bool:
        _error("The project's JevEditor Enabled field is not a boolean.")
    return path, data


def _entry(project: dict) -> dict | None:
    return next((p for p in project.get("Plugins", []) if p["Name"] == "JevEditor"), None)


def _replace_entry(project: dict, entry: dict | None, *, remove_empty: bool = False) -> dict:
    result = json.loads(json.dumps(project))
    plugins = result.get("Plugins", [])
    index = next((i for i, p in enumerate(plugins) if p["Name"] == "JevEditor"), None)
    if index is not None:
        if entry is None:
            plugins.pop(index)
        else:
            plugins[index] = entry
    elif entry is not None:
        plugins.append(entry)
    if plugins or not remove_empty:
        result["Plugins"] = plugins
    else:
        result.pop("Plugins", None)
    return result


def _source(value: str | Path) -> tuple[Path, dict, dict]:
    root = _absolute(value)
    descriptor = _json(root / "JevEditor.uplugin")
    if not isinstance(descriptor.get("Modules"), list) or not any(
        isinstance(module, dict)
        and module.get("Name") == "JevEditor"
        and module.get("Type") == "Editor"
        for module in descriptor["Modules"]
    ):
        _error("Source is not a JevEditor editor plugin.")
    files = {"JevEditor.uplugin": _record(root / "JevEditor.uplugin")}
    source = root / "Source"
    _safe_path(source)
    if not source.is_dir():
        _error("The source plugin has no Source directory.")
    visited, pending = 0, [source]
    while pending:
        with os.scandir(pending.pop()) as entries:
            for entry in entries:
                visited += 1
                if visited > MAX_FILES * 4:
                    _error("Source plugin has too many entries.")
                path = Path(entry.path)
                _safe_path(path)
                if entry.is_dir(follow_symlinks=False):
                    pending.append(path)
                elif entry.is_file(follow_symlinks=False) and path.suffix in _EXTENSIONS:
                    relative = _relative(path.relative_to(root).as_posix())
                    files[relative] = _record(path)
    icon = root / "Resources" / "Icon128.png"
    _safe_path(icon)
    if icon.exists():
        files["Resources/Icon128.png"] = _record(icon)
    if len(files) < 2 or len(files) > MAX_FILES:
        _error("Source plugin file count is outside the supported range.")
    if len({key.casefold() for key in files}) != len(files):
        _error("Source paths collide on case-insensitive filesystems.")
    if sum(item["bytes"] for item in files.values()) > MAX_TOTAL_BYTES:
        _error("Source plugin exceeds the installation size limit.")
    return root, descriptor, dict(sorted(files.items()))


def _manifest(destination: Path, project: Path) -> dict | None:
    path = destination / MANIFEST
    if not path.exists():
        _safe_path(path)
        return None
    value = _json(path)
    expected = {"schema", "owner", "project_file", "source_version", "files", "project_entry"}
    if (
        set(value) != expected
        or value.get("schema") != 1
        or value.get("owner") != OWNER
        or value.get("project_file") != str(project)
        or not isinstance(value.get("files"), dict)
        or len(value["files"]) > MAX_FILES
        or not isinstance(value.get("project_entry"), dict)
    ):
        _error(
            "Existing installation manifest is unknown, malformed, or belongs to another project."
        )
    for key, record in value["files"].items():
        _relative(key)
        if (
            not isinstance(record, dict)
            or set(record) != {"sha256", "bytes"}
            or not isinstance(record["sha256"], str)
            or len(record["sha256"]) != 64
            or any(c not in "0123456789abcdef" for c in record["sha256"])
            or type(record["bytes"]) is not int
            or not 0 <= record["bytes"] <= MAX_FILE_BYTES
        ):
            _error("The installation manifest contains an invalid file record.")
    if len({key.casefold() for key in value["files"]}) != len(value["files"]):
        _error("The installation manifest has colliding file paths.")
    if sum(record["bytes"] for record in value["files"].values()) > MAX_TOTAL_BYTES:
        _error("The installation manifest exceeds the total file size limit.")
    entry = value["project_entry"]
    if (
        set(entry) != {"original", "installed", "had_plugins_field"}
        or type(entry["had_plugins_field"]) is not bool
        or not isinstance(entry["installed"], dict)
        or entry["installed"].get("Name") != "JevEditor"
        or entry["installed"].get("Enabled") is not True
        or (entry["original"] is not None and not isinstance(entry["original"], dict))
        or (entry["original"] is not None and entry["original"].get("Name") != "JevEditor")
    ):
        _error("The installation manifest's project entry is invalid.")
    return value


def _additional_plugins(project_path: Path, project: dict) -> list[str]:
    paths = project.get("AdditionalPluginDirectories", [])
    if not isinstance(paths, list) or len(paths) > 32 or any(not isinstance(p, str) for p in paths):
        _error("AdditionalPluginDirectories is malformed or too large.")
    found = []
    for value in paths:
        # Unreal permits relative ../ directories here; normalize only for an explicit read.
        path = (
            Path(os.path.abspath(project_path.parent / value)) / "JevEditor" / "JevEditor.uplugin"
        )
        _safe_path(path)
        if path.is_file():
            found.append(str(path))
    return found


def _make_plan(project_file, source_plugin=None, *, uninstall=False) -> dict:
    project_path, project = _project(project_file)
    destination = project_path.parent / "Plugins" / "JevEditor"
    _safe_path(destination)
    manifest = _manifest(destination, project_path)
    if uninstall and manifest is None:
        _error("This project has no installer-owned JevEditor installation.")
    source_path, descriptor, desired = (None, {}, {})
    if not uninstall:
        source_path, descriptor, desired = _source(source_plugin)
        if source_path == destination or destination in source_path.parents:
            _error("Source must be outside the destination plugin directory.")
    owned = manifest["files"] if manifest else {}
    current = {name: _record(destination / name) for name in sorted(set(owned) | set(desired))}
    conflicts, preserved, actions = [], [], []
    duplicate_plugins = _additional_plugins(project_path, project)
    if duplicate_plugins and not uninstall:
        conflicts.append({"path": "AdditionalPluginDirectories", "reason": "duplicate_plugin"})
    for name, old in current.items():
        if name in owned and old is not None and old != owned[name]:
            preserved.append({"path": name, "reason": "locally_modified"})
            if not uninstall:
                conflicts.append({"path": name, "reason": "locally_modified"})
            continue
        if name not in owned and old is not None:
            conflicts.append({"path": name, "reason": "unowned_existing_file"})
            continue
        new = desired.get(name)
        if new != old:
            actions.append(
                {"path": name, "action": "write" if new else "remove", "before": old, "after": new}
            )
    installed_entry = {**(_entry(project) or {"Name": "JevEditor"}), "Enabled": True}
    entry = (
        manifest["project_entry"]
        if manifest
        else {
            "original": _entry(project),
            "installed": installed_entry,
            "had_plugins_field": "Plugins" in project,
        }
    )
    if manifest and _entry(project) != entry["installed"]:
        conflicts.append({"path": project_path.name, "reason": "project_plugin_entry_modified"})
    if uninstall:
        # Keeping modified plugin sources is safe; disable/remove the owned project entry.
        patched_project = _replace_entry(
            project, entry["original"], remove_empty=not entry["had_plugins_field"]
        )
        next_manifest = None
    else:
        patched_project = _replace_entry(project, entry["installed"])
        next_manifest = {
            "schema": 1,
            "owner": OWNER,
            "project_file": str(project_path),
            "source_version": descriptor.get("VersionName"),
            "files": desired,
            "project_entry": entry,
        }
    original_project_bytes = _read(project_path)
    project_changed = project != patched_project
    plan = {
        "schema": 1,
        "operation": "uninstall" if uninstall else "install",
        "project_file": str(project_path),
        "destination": str(destination),
        "source_plugin": str(source_path) if source_path else None,
        "source_version": descriptor.get("VersionName")
        if not uninstall
        else manifest["source_version"],
        "source_files": desired,
        "current_files": current,
        "manifest_before": _record(destination / MANIFEST),
        "manifest_after": next_manifest,
        "project_before": {
            "sha256": _digest(original_project_bytes),
            "bytes": len(original_project_bytes),
        },
        "project_after": patched_project if project_changed else None,
        "project_entry_before": _entry(project),
        "project_entry_after": _entry(patched_project),
        "actions": actions,
        "conflicts": conflicts,
        "preserved": preserved,
        "can_apply": not conflicts,
        "duplicate_plugin_paths": duplicate_plugins,
        "scope": "Listed plugin sources, owned manifest, and the JevEditor project entry only.",
        "backup_policy": "Original bytes retained under this project's Plugins/.JevEditorBackups.",
        "requires_editor_closed": True,
        "build_performed": False,
        "untracked_files": "Unlisted files and generated Binaries/Intermediate are never removed.",
    }
    plan["plan_id"] = _digest(_json_bytes(plan))
    return plan


def plan_install(project_file: str | Path, source_plugin: str | Path) -> dict:
    """Plan installation, update, or repair without modifying the project."""
    return _make_plan(project_file, source_plugin)


def plan_uninstall(project_file: str | Path) -> dict:
    """Plan removal of unchanged owned files; retain all modified and untracked files."""
    return _make_plan(project_file, uninstall=True)


def read_plan(path: str | Path) -> dict:
    return _json(_absolute(path), 4 * MAX_JSON_BYTES)


def _atomic_write(path: Path, data: bytes) -> None:
    _safe_path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    _safe_path(path)
    temporary = path.with_name(f".{path.name}.jev-tmp-{uuid.uuid4().hex}")
    try:
        with temporary.open("xb") as handle:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


@contextmanager
def _setup_lease(project_path: Path):
    """A kernel lease survives neither a crash nor process exit; no PID guessing.

    The small lease file is retained. Deleting it would let two processes lock
    different inodes on POSIX. The separate operation lock identifies the receipt.
    """
    path = project_path.parent / "Plugins/.JevEditor.setup.lease"
    _safe_path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a+b") as handle:
        if handle.seek(0, os.SEEK_END) == 0:
            handle.write(b"0")
            handle.flush()
        handle.seek(0)
        try:
            if os.name == "nt":
                import msvcrt

                msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl

                fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            _error("Another setup operation is active; wait until it exits.", "setup_busy")
        try:
            yield
        finally:
            handle.seek(0)
            if os.name == "nt":
                msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(handle.fileno(), fcntl.LOCK_UN)


def apply_plan(plan: dict) -> dict:
    """Apply an unchanged plan once. Does not close editors, compile, or launch anything."""
    if (
        not isinstance(plan, dict)
        or plan.get("schema") != 1
        or plan.get("operation") not in {"install", "uninstall"}
        or not isinstance(plan.get("project_file"), str)
        or (plan["operation"] == "install" and not isinstance(plan.get("source_plugin"), str))
    ):
        _error("The setup plan is malformed.", "invalid_request")
    project_path, _ = _project(plan["project_file"])
    fresh = _make_plan(
        project_path, plan.get("source_plugin"), uninstall=plan["operation"] == "uninstall"
    )
    if fresh != plan:
        _error("Setup changed. Generate and review a fresh plan.", "stale_setup_plan")
    if not plan["can_apply"]:
        _error("Resolve every listed setup conflict before applying this plan.")
    with _setup_lease(project_path):
        return _apply_plan(plan)


def _apply_plan(plan: dict) -> dict:
    if (
        not isinstance(plan, dict)
        or plan.get("schema") != 1
        or plan.get("operation") not in {"install", "uninstall"}
        or not isinstance(plan.get("project_file"), str)
        or (plan["operation"] == "install" and not isinstance(plan.get("source_plugin"), str))
    ):
        _error("The setup plan is malformed.", "invalid_request")
    fresh = _make_plan(
        plan["project_file"], plan.get("source_plugin"), uninstall=plan["operation"] == "uninstall"
    )
    if fresh != plan:
        _error(
            "Source, project, installation, or plan changed. Generate and review a fresh plan.",
            "stale_setup_plan",
        )
    if not plan["can_apply"]:
        _error("Resolve every listed setup conflict before applying this plan.")
    destination, project_path = Path(plan["destination"]), Path(plan["project_file"])
    parent = destination.parent
    _safe_path(parent)
    parent.mkdir(parents=True, exist_ok=True)
    lock = parent / ".JevEditor.install.lock"
    _safe_path(lock)
    try:
        handle = lock.open("x", encoding="utf-8")
    except FileExistsError:
        _error("Another setup operation or a retained lock exists. Inspect it before retrying.")
    operation_id = uuid.uuid4().hex
    with handle:
        handle.write(operation_id)
    backup = parent / ".JevEditorBackups" / operation_id
    changed: list[tuple[Path, bytes | None, bytes | None]] = []
    retain_lock = False
    try:
        # Revalidate under the cooperative project lock, before creating backups.
        if (
            _make_plan(
                plan["project_file"],
                plan.get("source_plugin"),
                uninstall=plan["operation"] == "uninstall",
            )
            != plan
        ):
            _error("Installation changed while acquiring the setup lock.", "stale_setup_plan")
        writes: list[tuple[Path, bytes | None, dict | None]] = []
        for action in plan["actions"]:
            data = None
            if action["action"] == "write":
                data = _read(Path(plan["source_plugin"]) / action["path"])
                if {"sha256": _digest(data), "bytes": len(data)} != action["after"]:
                    _error("Source changed after planning.", "stale_setup_plan")
            writes.append((destination / action["path"], data, action["before"]))
        if plan["project_after"] is not None:
            writes.append(
                (project_path, _json_bytes(plan["project_after"]), plan["project_before"])
            )
        manifest_data = _json_bytes(plan["manifest_after"]) if plan["manifest_after"] else None
        manifest_after_record = (
            {"sha256": _digest(manifest_data), "bytes": len(manifest_data)}
            if manifest_data is not None
            else None
        )
        if manifest_after_record != plan["manifest_before"]:
            writes.append((destination / MANIFEST, manifest_data, plan["manifest_before"]))
        if not writes:
            return {
                "status": "unchanged",
                "operation": plan["operation"],
                "plan_id": plan["plan_id"],
                "changed_files": 0,
                "backup_directory": None,
                "build_performed": False,
            }
        _safe_path(backup)
        backup.mkdir(parents=True, exist_ok=False)
        records = []
        total = 0
        for index, (path, data, expected) in enumerate(writes):
            if _record(path) != expected:
                _error("A destination file changed after planning.", "stale_setup_plan")
            before = _read(path) if expected is not None else None
            total += len(before or b"")
            if total > MAX_TOTAL_BYTES + MAX_JSON_BYTES * 2:
                _error("Installation backup exceeds the size limit.")
            backup_name = f"{index:04d}.original"
            if before is not None:
                _atomic_write(backup / backup_name, before)
            records.append(
                {
                    "path": str(path),
                    "before": expected,
                    "after": {"sha256": _digest(data), "bytes": len(data)}
                    if data is not None
                    else None,
                    "backup": backup_name if before is not None else None,
                }
            )
        _atomic_write(
            backup / "receipt.json",
            _json_bytes(
                {
                    "schema": 2,
                    "owner": OWNER,
                    "operation_id": operation_id,
                    "project_file": str(project_path),
                    "plan_id": plan["plan_id"],
                    "operation": plan["operation"],
                    "status": "prepared",
                    "files": records,
                }
            ),
        )
        for path, data, expected in writes:
            if _record(path) != expected:
                _error("A destination file changed during setup.", "stale_setup_plan")
            before = _read(path) if expected is not None else None
            if data is None:
                _safe_path(path)
                path.unlink()
            else:
                _atomic_write(path, data)
            changed.append((path, before, data))
        receipt = _json(backup / "receipt.json")
        receipt["status"] = "complete"
        _atomic_write(backup / "receipt.json", _json_bytes(receipt))
        return {
            "status": "partially_uninstalled"
            if plan["preserved"]
            else ("uninstalled" if plan["operation"] == "uninstall" else "installed"),
            "operation": plan["operation"],
            "plan_id": plan["plan_id"],
            "changed_files": len(changed),
            "backup_directory": str(backup),
            "preserved": plan["preserved"],
            "project_file": str(project_path),
            "build_performed": False,
            "next_steps": [
                "Build the selected project with its licensed Unreal Engine installation.",
                "Launch the editor with its bridge token and the selected JEV_BRIDGE_PORT.",
                "Set JEV_EXPECTED_PROJECT and matching JEV_BRIDGE_URL, then run jev-unreal doctor.",
            ]
            if plan["operation"] == "install"
            else [
                "Close and rebuild the selected project before opening it again.",
                "Review retained backups, local modifications, and generated files manually.",
            ],
        }
    except BaseException:
        rollback_failed = False
        for path, before, written in reversed(changed):
            try:
                expected = (
                    {"sha256": _digest(written), "bytes": len(written)}
                    if written is not None
                    else None
                )
                if _record(path) != expected:
                    rollback_failed = True
                    continue
                if before is None:
                    path.unlink()
                else:
                    _atomic_write(path, before)
            except (OSError, JevError):
                rollback_failed = True
        # Keep exact crash-recovery evidence even when ordinary rollback succeeds.
        try:
            receipt = _json(backup / "receipt.json")
            receipt["status"] = "rollback_failed" if rollback_failed else "rolled_back"
            _atomic_write(backup / "receipt.json", _json_bytes(receipt))
        except (OSError, JevError):
            pass
        if rollback_failed:
            retain_lock = True
            _error(
                "Setup could not fully restore files. Inspect the retained backup receipts.",
                "setup_rollback_failed",
            )
        raise
    finally:
        _safe_path(lock)
        if not retain_lock and lock.exists() and _read(lock, 128).decode() == operation_id:
            lock.unlink()


def _operation_id(value: str) -> str:
    if (
        not isinstance(value, str)
        or len(value) != 32
        or any(c not in "0123456789abcdef" for c in value)
    ):
        _error(
            "An operation ID must be exactly 32 lowercase hexadecimal characters.",
            "invalid_request",
        )
    return value


def _valid_record(value: dict | None) -> bool:
    return value is None or (
        isinstance(value, dict)
        and set(value) == {"sha256", "bytes"}
        and isinstance(value["sha256"], str)
        and len(value["sha256"]) == 64
        and all(c in "0123456789abcdef" for c in value["sha256"])
        and type(value["bytes"]) is int
        and 0 <= value["bytes"] <= MAX_FILE_BYTES
    )


def _recovery_receipt(project_path: Path, operation_id: str) -> tuple[Path, dict]:
    directory = project_path.parent / "Plugins/.JevEditorBackups" / _operation_id(operation_id)
    receipt = _json(directory / "receipt.json")
    if receipt.get("schema") == 1:
        _error(
            "Legacy receipts have no after hashes; inspect and restore them manually.",
            "legacy_setup_receipt",
        )
    expected = {
        "schema",
        "owner",
        "project_file",
        "plan_id",
        "operation",
        "operation_id",
        "status",
        "files",
    }
    if (
        set(receipt) != expected
        or receipt.get("schema") != 2
        or receipt.get("owner") != OWNER
        or receipt.get("project_file") != str(project_path)
        or receipt.get("operation_id") != operation_id
        or not isinstance(receipt.get("operation"), str)
        or receipt["operation"] not in {"install", "uninstall"}
        or not isinstance(receipt.get("status"), str)
        or receipt.get("status")
        not in {"prepared", "complete", "rolled_back", "rollback_failed", "recovered"}
        or not isinstance(receipt.get("plan_id"), str)
        or len(receipt["plan_id"]) != 64
        or any(c not in "0123456789abcdef" for c in receipt["plan_id"])
        or not isinstance(receipt.get("files"), list)
        or not 1 <= len(receipt["files"]) <= MAX_FILES + 2
    ):
        _error("The interrupted operation receipt is malformed or belongs to another project.")
    destination = project_path.parent / "Plugins/JevEditor"
    seen, total = set(), 0
    for index, item in enumerate(receipt["files"]):
        if (
            not isinstance(item, dict)
            or set(item) != {"path", "before", "after", "backup"}
            or not isinstance(item["path"], str)
            or not _valid_record(item["before"])
            or not _valid_record(item["after"])
            or item["before"] == item["after"]
            or item["backup"] != (f"{index:04d}.original" if item["before"] else None)
        ):
            _error("An interrupted operation file record is invalid.")
        target = _absolute(item["path"])
        if str(target) != item["path"] or str(target).casefold() in seen:
            _error("An interrupted operation has ambiguous target paths.")
        seen.add(str(target).casefold())
        if target not in {project_path, destination / MANIFEST}:
            try:
                _relative(target.relative_to(destination).as_posix())
            except ValueError:
                _error("Recovery target is outside the selected plugin and project.")
        if item["before"] is not None:
            if _record(directory / item["backup"]) != item["before"]:
                _error("A recovery backup is missing or its original hash has changed.")
            total += item["before"]["bytes"]
    if total > MAX_TOTAL_BYTES + MAX_JSON_BYTES * 2:
        _error("Recovery backups exceed the total size limit.")
    return directory, receipt


def plan_recovery(project_file: str | Path, operation_id: str) -> dict:
    """Read-only preview of exact restoration, or cleanup after a committed install.

    Files with unrelated edits block the entire recovery. A matching prepared
    receipt is not a signature; use only the selected project's trusted backups.
    """
    project_path, _ = _project(project_file)
    directory, receipt = _recovery_receipt(project_path, operation_id)
    lock = project_path.parent / "Plugins/.JevEditor.install.lock"
    lock_record = _record(lock)
    conflicts, actions, unchanged = [], [], []
    if lock_record is not None and _read(lock, 128) != operation_id.encode():
        conflicts.append({"path": lock.name, "reason": "different_operation_lock"})
    if lock_record is None:
        # A closed receipt must never rewind a later successful installation
        # whose files happen to have the same after hashes.
        conflicts.append({"path": lock.name, "reason": "operation_not_pending"})
    committed = receipt["status"] == "complete"
    for item in reversed(receipt["files"]):
        current = _record(Path(item["path"]))
        wanted = item["after"] if committed else item["before"]
        if current == wanted:
            unchanged.append(item["path"])
        elif not committed and current == item["after"]:
            actions.append(
                {
                    "path": item["path"],
                    "before": current,
                    "after": wanted,
                    "backup": item["backup"],
                    "action": "restore" if wanted else "remove",
                }
            )
        else:
            conflicts.append({"path": item["path"], "reason": "externally_modified"})
    if committed and lock_record is None:
        conflicts.append({"path": "receipt.json", "reason": "operation_already_complete"})
    if receipt["status"] == "recovered" and lock_record is None:
        conflicts.append({"path": "receipt.json", "reason": "operation_already_recovered"})
    plan = {
        "schema": 1,
        "operation": "recover",
        "project_file": str(project_path),
        "operation_id": operation_id,
        "receipt_before": _record(directory / "receipt.json"),
        "receipt_status": receipt["status"],
        "lock_before": lock_record,
        "mode": "finish_committed_install" if committed else "restore_original_bytes",
        "actions": actions,
        "unchanged": unchanged,
        "conflicts": conflicts,
        "can_apply": not conflicts,
        "requires_editor_closed": True,
        "requires_no_active_setup": True,
        "build_performed": False,
    }
    plan["plan_id"] = _digest(_json_bytes(plan))
    return plan


def apply_recovery_plan(plan: dict) -> dict:
    """Recover under the OS lease, rechecking every target and backup before writing."""
    if (
        not isinstance(plan, dict)
        or plan.get("schema") != 1
        or plan.get("operation") != "recover"
        or not isinstance(plan.get("project_file"), str)
    ):
        _error("The recovery plan is malformed.", "invalid_request")
    operation_id = _operation_id(plan.get("operation_id"))
    project_path, _ = _project(plan["project_file"])
    with _setup_lease(project_path):
        fresh = plan_recovery(project_path, operation_id)
        if plan != fresh:
            _error(
                "Recovery targets, receipt, backups or lock changed; review a fresh plan.",
                "stale_setup_plan",
            )
        if not fresh["can_apply"]:
            _error("Recovery preserves external edits. Resolve every listed conflict first.")
        directory, receipt = _recovery_receipt(project_path, operation_id)
        lock = project_path.parent / "Plugins/.JevEditor.install.lock"
        if plan["lock_before"] is None:
            with lock.open("x", encoding="utf-8") as handle:
                handle.write(operation_id)
        changed = 0
        for action in plan["actions"]:
            target = Path(action["path"])
            if _record(target) != action["before"]:
                _error(
                    "A recovery target changed. Its external edit was preserved.",
                    "stale_setup_plan",
                )
            if action["after"] is None:
                _safe_path(target)
                target.unlink()
            else:
                data = _read(directory / action["backup"])
                if {"sha256": _digest(data), "bytes": len(data)} != action["after"]:
                    _error("A recovery backup changed. Review a fresh plan.", "stale_setup_plan")
                _atomic_write(target, data)
            changed += 1
        # On failure the receipt and identifying lock survive, so recovery can
        # resume from hashes after inspection instead of guessing which writes ran.
        receipt["status"] = (
            "complete" if plan["mode"] == "finish_committed_install" else "recovered"
        )
        _atomic_write(directory / "receipt.json", _json_bytes(receipt))
        if _read(lock, 128) != operation_id.encode():
            _error("The setup lock changed; it was preserved.", "setup_conflict")
        lock.unlink()
        return {
            "status": "recovered",
            "mode": plan["mode"],
            "plan_id": plan["plan_id"],
            "changed_files": changed,
            "backup_directory": str(directory),
            "project_file": str(project_path),
            "build_performed": False,
            "next_steps": [
                "Review a fresh install plan before retrying installation.",
                "Build and authenticate the exact project after installing.",
            ],
        }


def list_recovery(project_file: str | Path) -> dict:
    """List bounded local receipt metadata; never remove a lock based on its age."""
    project_path, _ = _project(project_file)
    parent = project_path.parent / "Plugins"
    lock = _presence(parent / ".JevEditor.install.lock")
    root = parent / ".JevEditorBackups"
    _safe_path(root)
    receipts, truncated = [], False
    if root.is_dir():
        with os.scandir(root) as entries:
            for index, entry in enumerate(entries):
                if index >= 128:
                    truncated = True
                    break
                try:
                    operation_id = _operation_id(entry.name)
                    data = _json(Path(entry.path) / "receipt.json")
                    if data.get("owner") != OWNER or data.get("project_file") != str(project_path):
                        continue
                    status = data.get("status")
                    if not isinstance(status, str):
                        status = "unknown"
                    receipts.append(
                        {
                            "operation_id": operation_id,
                            "status": status
                            if status
                            in {
                                "prepared",
                                "complete",
                                "rolled_back",
                                "rollback_failed",
                                "recovered",
                            }
                            else "unknown",
                            "reviewed_recovery_supported": data.get("schema") == 2,
                        }
                    )
                except (OSError, JevError):
                    continue
    return {
        "project_file": str(project_path),
        "lock": lock,
        "receipts": sorted(receipts, key=lambda item: item["operation_id"]),
        "truncated": truncated,
        "active_process_checked": False,
        "scope": "Receipt discovery only; applying recovery requires the exclusive OS lease.",
    }


def _python_dependencies() -> dict:
    packages = {}
    for name, minimum, maximum in (
        ("httpx", (0, 28, 0), (0, 29, 0)),
        ("mcp", (1, 26, 0), (2, 0, 0)),
        ("pydantic", (2, 11, 0), (3, 0, 0)),
    ):
        try:
            version = importlib.metadata.version(name)
            parts = version.split(".")
            parsed = tuple(int(p) for p in parts) if all(p.isdigit() for p in parts) else None
            compatible = (
                minimum <= (*parsed, *([0] * (3 - len(parsed)))) < maximum if parsed else None
            )
            packages[name] = {"version": version, "compatible_release": compatible}
        except importlib.metadata.PackageNotFoundError:
            packages[name] = {"version": None, "compatible_release": False}
    return {
        "python": platform.python_version(),
        "python_supported": sys.version_info >= (3, 12),
        "packages": packages,
        "dependency_imports_tested": False,
        "scope": "Installed distribution metadata only; prerelease compatibility is unknown.",
    }


def _presence(path: Path) -> dict:
    try:
        _safe_path(path)
        metadata = path.stat()
        return {
            "path": str(path),
            "present": True,
            "regular_file": stat.S_ISREG(metadata.st_mode),
            "bytes": metadata.st_size,
            "contents_read": False,
        }
    except FileNotFoundError:
        return {"path": str(path), "present": False, "contents_read": False}
    except (OSError, JevError):
        return {
            "path": str(path),
            "present": None,
            "error": "unavailable_or_unsafe_path",
            "contents_read": False,
        }


def _toolchain() -> dict:
    if os.name != "nt":
        return {
            "platform": platform.system(),
            "compiler": shutil.which("clang++"),
            "verified_by_build": False,
        }
    compilers, sdks = [], []
    for base in {
        os.environ.get("ProgramFiles", "C:/Program Files"),
        os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)"),
    }:
        for year in ("2022", "2025", "2026", "18"):
            for edition in ("Community", "Professional", "Enterprise", "BuildTools"):
                root = Path(base) / "Microsoft Visual Studio" / year / edition / "VC/Tools/MSVC"
                try:
                    _safe_path(root)
                    if not root.is_dir():
                        continue
                    for index, version in enumerate(root.iterdir()):
                        if index >= 32:
                            break
                        compiler = version / "bin/Hostx64/x64/cl.exe"
                        if _presence(compiler).get("regular_file"):
                            compilers.append({"version": version.name, "path": str(compiler)})
                except (OSError, JevError):
                    continue
        root = Path(base) / "Windows Kits/10/Include"
        try:
            _safe_path(root)
            if root.is_dir():
                for index, version in enumerate(root.iterdir()):
                    if index >= 32:
                        break
                    if _presence(version / "um/Windows.h").get("regular_file"):
                        sdks.append(version.name)
        except (OSError, JevError):
            continue
    return {
        "platform": "Windows",
        "msvc": compilers,
        "windows_sdk": sorted(set(sdks)),
        "search_scope": "Standard Visual Studio and Windows Kits locations only.",
        "verified_by_build": False,
    }


def inspect_setup(
    project_file: str | Path,
    *,
    source_plugin: str | Path | None = None,
    engine_root: str | Path | None = None,
    port: int = 9845,
    credential_directory: str | Path | None = None,
) -> dict:
    """Inspect explicit local paths and bind-test a loopback port; never read secret files."""
    if type(port) is not int or not 1024 <= port <= 65535:
        _error("Bridge port must be an integer between 1024 and 65535.", "invalid_request")
    project_path, project = _project(project_file)
    destination = project_path.parent / "Plugins/JevEditor"
    issues, next_steps = [], []
    installed = {"present": False, "managed": False, "version": None}
    try:
        if (destination / "JevEditor.uplugin").exists():
            descriptor = _json(destination / "JevEditor.uplugin")
            installed.update(present=True, version=descriptor.get("VersionName"))
        manifest = _manifest(destination, project_path)
        installed["managed"] = manifest is not None
        installed["modified_files"] = [
            key
            for key, expected in (manifest["files"] if manifest else {}).items()
            if _record(destination / key) != expected
        ]
    except (JevError, OSError):
        issues.append(
            "Installed plugin cannot be safely inspected; inspect its manifest and paths."
        )
    source = None
    if source_plugin is not None:
        root, descriptor, files = _source(source_plugin)
        source = {
            "path": str(root),
            "version": descriptor.get("VersionName"),
            "files": len(files),
            "bytes": sum(v["bytes"] for v in files.values()),
        }
    engine = {"checked": False, "project_association": project.get("EngineAssociation")}
    if engine_root is not None:
        root = _absolute(engine_root)
        engine["checked"] = True
        engine["root"] = str(root)
        try:
            version = _json(root / "Engine/Build/Build.version")
            engine["version"] = {
                k: version.get(k)
                for k in ("MajorVersion", "MinorVersion", "PatchVersion", "Changelist")
            }
            engine["build_tool_present"] = _presence(
                root
                / (
                    "Engine/Build/BatchFiles/Build.bat"
                    if os.name == "nt"
                    else "Engine/Build/BatchFiles/Linux/Build.sh"
                )
            )["present"]
        except (OSError, JevError):
            issues.append(
                "Engine build metadata is missing or malformed at the selected engine root."
            )
    else:
        next_steps.append(
            "Pass --engine-root to inspect the selected licensed Unreal installation."
        )
    credentials = None
    if credential_directory is None and os.environ.get("LOCALAPPDATA"):
        credential_directory = Path(os.environ["LOCALAPPDATA"]) / "JevUnreal"
    if credential_directory is not None:
        local = _absolute(credential_directory)
        credentials = {
            name: _presence(local / name)
            for name in ("bridge.token", "openrouter.dpapi", "typesafe.dpapi")
        }
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        try:
            probe.bind(("127.0.0.1", port))
            port_status = "available_at_check"
        except OSError:
            port_status = "in_use_or_unavailable"
    if port_status != "available_at_check":
        next_steps.append(
            "Run doctor to identify the authenticated bridge, or select an unused port."
        )
    recovery = _presence(project_path.parent / "Saved/Autosaves/PackageRestoreData.json")
    if recovery["present"]:
        next_steps.append(
            "Review Unreal's autosave recovery prompt; setup never deletes recovery data."
        )
    if not installed["present"]:
        next_steps.append(
            "Generate and review a setup plan, close this project's editor, then apply."
        )
    if _entry(project) is None or _entry(project).get("Enabled") is not True:
        next_steps.append(
            "An install plan will explicitly enable JevEditor in this project's Plugins entry."
        )
    return {
        "project_file": str(project_path),
        "project_enabled": bool(_entry(project) and _entry(project).get("Enabled") is True),
        "installed": installed,
        "source": source,
        "engine": engine,
        "toolchain": _toolchain(),
        "python_environment": _python_dependencies(),
        "installer_recovery": list_recovery(project_path),
        "credentials": credentials,
        "recovery_metadata": recovery,
        "additional_plugin_paths": _additional_plugins(project_path, project),
        "port": {
            "host": "127.0.0.1",
            "port": port,
            "status": port_status,
            "bridge_authenticated": False,
        },
        "issues": issues,
        "next_steps": next_steps,
        "scope": "Local metadata and port bind probe; no secrets read, provider calls, or builds.",
        "runtime_verified": False,
    }
