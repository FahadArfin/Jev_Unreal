"""Explicit approved-file checkpoints and fixed read-only source-control status commands."""

import asyncio
import hashlib
import json
import os
import re
import subprocess
from pathlib import PurePosixPath
from typing import Annotated, Any

from mcp.server.fastmcp import FastMCP
from mcp.types import ToolAnnotations
from pydantic import BaseModel, ConfigDict, Field, model_validator

from .errors import JevError
from .profiles import _bounded_file
from .team_policy import digest, local_path, project_identity

MAX_OUTPUT = 65536


class CheckpointConfig(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True, frozen=True)
    version: int = Field(default=1, ge=1, le=1)
    project_file: str
    approved_files: list[str] = Field(min_length=1, max_length=32)
    maximum_file_bytes: int = Field(default=8388608, ge=1, le=67108864)
    git_executable: str | None = None
    git_sha256: str | None = Field(default=None, pattern=r"^[0-9a-f]{64}$")
    p4_executable: str | None = None
    p4_sha256: str | None = Field(default=None, pattern=r"^[0-9a-f]{64}$")
    p4_port: str | None = Field(default=None, max_length=256)

    @model_validator(mode="after")
    def paths(self):
        if len(set(self.approved_files)) != len(self.approved_files):
            raise ValueError("Approved paths must be unique.")
        for name in self.approved_files:
            path = PurePosixPath(name)
            if (
                not 1 <= len(name) <= 256
                or path.is_absolute()
                or str(path) != name
                or any(part in {"..", ".", ".git", ".p4config"} for part in path.parts)
                or any(c in name for c in "\\:*?@#%")
                or "..." in name
                or any(ord(c) < 32 or ord(c) == 127 for c in name)
            ):
                raise ValueError(
                    "Use exact project-relative files, without links or wildcard syntax."
                )
            if any(
                part.lower() in {"saved", "intermediate", "binaries", ".git"} for part in path.parts
            ):
                raise ValueError("Generated/private engine output is not checkpoint scope.")
        for kind in ("git", "p4"):
            if bool(getattr(self, f"{kind}_executable")) != bool(getattr(self, f"{kind}_sha256")):
                raise ValueError("Configured executables require a pinned SHA-256.")
        if bool(self.p4_executable) != bool(self.p4_port):
            raise ValueError("Perforce requires an explicit reviewed TCP endpoint in p4_port.")
        if self.p4_port:
            match = re.fullmatch(
                r"(?:(?:ssl|tcp)(?:4|6|46|64)?:)?"
                r"(?:[A-Za-z0-9][A-Za-z0-9._-]*|\[[A-Fa-f0-9:]+\]):([0-9]{1,5})",
                self.p4_port,
            )
            if not match or not 1 <= int(match[1]) <= 65535:
                raise ValueError("p4_port must be a TCP host:port; command transports are refused.")
        return self


def read_checkpoint_config(filename, expected_project):
    def unique(pairs):
        value = {}
        for key, item in pairs:
            if key in value:
                raise ValueError
            value[key] = item
        return value

    try:
        raw = _bounded_file(local_path(filename, exists=True), 65536)
        config = CheckpointConfig.model_validate(json.loads(raw, object_pairs_hook=unique))
        project = local_path(config.project_file, exists=True)
        if (
            not expected_project
            or project.suffix.lower() != ".uproject"
            or project_identity(str(project)) != project_identity(expected_project)
        ):
            raise ValueError
        for kind in ("git", "p4"):
            if executable := getattr(config, f"{kind}_executable"):
                local_path(executable, exists=True)
        return config
    except (ValueError, TypeError, UnicodeError):
        raise JevError(
            "configuration", "Checkpoint configuration is invalid or for another project."
        ) from None


async def fixed_status_command(argv, root, env):
    """No shell, stderr disclosure, unbounded output, retry or background terminal window."""
    kwargs = {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}
    try:
        process = await asyncio.create_subprocess_exec(
            *argv,
            cwd=root,
            env=env,
            stdin=asyncio.subprocess.DEVNULL,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.DEVNULL,
            **kwargs,
        )
    except OSError:
        raise JevError(
            "vcs_unavailable", "Configured source-control utility could not start."
        ) from None

    async def collect():
        output = bytearray()
        while chunk := await process.stdout.read(4096):
            output.extend(chunk)
            if len(output) > MAX_OUTPUT:
                raise JevError("vcs_output_limit", "Source-control status exceeded 64 KiB.")
        if await process.wait() != 0:
            raise JevError(
                "vcs_unavailable", "Source-control status failed; no stderr is retained."
            )
        return bytes(output)

    try:
        return await asyncio.wait_for(collect(), timeout=10)
    except TimeoutError:
        raise JevError("vcs_timeout", "Source-control status exceeded ten seconds.") from None
    finally:
        if process.returncode is None:
            try:
                process.kill()
            except ProcessLookupError:
                pass
        await process.wait()


class ProjectCheckpoints:
    def __init__(self, bridge, config_file="", *, runner=fixed_status_command):
        self.bridge, self.runner = bridge, runner
        self.config = (
            read_checkpoint_config(config_file, bridge.settings.expected_project)
            if config_file
            else None
        )

    def _require(self):
        store = self.bridge.infrastructure.require()
        if self.config is None:
            raise JevError(
                "checkpoint_configuration_required", "Set reviewed JEV_CHECKPOINT_CONFIG."
            )
        if project_identity(self.config.project_file) != store.project:
            raise JevError("wrong_project", "Checkpoint and runtime projects differ.")
        return store

    def _files(self, files):
        self._require()
        if (
            not isinstance(files, list)
            or not 1 <= len(files) <= 32
            or any(not isinstance(name, str) for name in files)
            or len(set(files)) != len(files)
            or not set(files) <= set(self.config.approved_files)
        ):
            raise JevError("file_scope", "Select 1–32 exact files from the reviewed configuration.")
        for name in files:
            path = local_path(str(self._root() / name))
            if path.exists() and not path.is_file():
                raise JevError("file_scope", "Approved scope must remain files, not directories.")
        return files

    def _root(self):
        return local_path(self.config.project_file, exists=True).parent

    def _manifest(self, files):
        root, manifest = self._root(), []
        for name in self._files(files):
            path = local_path(str(root / name))
            if not path.is_relative_to(root):
                raise JevError("file_scope", "Checkpoint file escaped the approved project root.")
            if not path.exists():
                manifest.append({"path": name, "exists": False})
                continue
            before = path.stat()
            content = _bounded_file(path, self.config.maximum_file_bytes)
            after = path.stat()
            if (before.st_ino, before.st_size, before.st_mtime_ns) != (
                after.st_ino,
                after.st_size,
                after.st_mtime_ns,
            ):
                raise JevError("file_changed", "An approved file changed during hashing.")
            manifest.append(
                {
                    "path": name,
                    "exists": True,
                    "bytes": len(content),
                    "sha256": hashlib.sha256(content).hexdigest(),
                }
            )
        return manifest

    async def vcs_status(self, files):
        files = self._files(files)
        root = self._root()
        result = {
            "scope": "Only selected approved disk files; unsaved editor buffers are excluded.",
            "files": files,
            "modified_by_tool": False,
        }
        # No repository-provided pager, external fsmonitor, pathspec magic or environment injection.
        base_env = {
            k: v
            for k, v in os.environ.items()
            if k.upper()
            in {
                "PATH",
                "SYSTEMROOT",
                "WINDIR",
                "TEMP",
                "TMP",
                "HOME",
                "USERPROFILE",
                "P4USER",
                "P4CLIENT",
                "P4TICKETS",
                "P4TRUST",
            }
        }
        for kind in ("git", "p4"):
            executable = getattr(self.config, f"{kind}_executable")
            if not executable:
                result[kind] = {"status": "not_configured"}
                continue
            try:
                binary = _bounded_file(local_path(executable, exists=True), 67108864)
                if hashlib.sha256(binary).hexdigest() != getattr(self.config, f"{kind}_sha256"):
                    raise JevError("vcs_identity", "Configured source-control executable changed.")
                if kind == "git":
                    argv = [
                        executable,
                        "--literal-pathspecs",
                        "-c",
                        "core.fsmonitor=false",
                        "-c",
                        "core.untrackedCache=false",
                        "status",
                        "--porcelain=v1",
                        "-z",
                        "--ignore-submodules=all",
                        "--untracked-files=all",
                        "--",
                        *files,
                    ]
                    env = {
                        **base_env,
                        "GIT_CONFIG_NOSYSTEM": "1",
                        "GIT_CONFIG_GLOBAL": os.devnull,
                        "GIT_OPTIONAL_LOCKS": "0",
                        "GIT_TERMINAL_PROMPT": "0",
                    }
                    # Porcelain v1 always names paths relative to the repository
                    # root, which can be above an Unreal project's directory.
                    prefix_bytes = await self.runner(
                        [executable, "rev-parse", "--show-prefix"], root, env
                    )
                    if len(prefix_bytes) > 4096:
                        raise JevError("vcs_response", "Git project prefix exceeds its bound.")
                    prefix = prefix_bytes.decode("utf-8", errors="strict").rstrip("\r\n")
                    if (any(ord(c) < 32 for c in prefix)
                            or (prefix and (not prefix.endswith("/")
                                            or PurePosixPath(prefix).is_absolute()
                                            or ".." in PurePosixPath(prefix).parts))):
                        raise JevError("vcs_response", "Git project prefix is invalid.")
                else:
                    if any(character in str(root) for character in "@#%*") or "..." in str(root):
                        raise JevError("file_scope", "Perforce root contains revision syntax.")
                    argv = [
                        executable,
                        "-p",
                        self.config.p4_port,
                        "-E",
                        "P4CONFIG=",
                        "-E",
                        "P4LOGINSSO=",
                        "-E",
                        "P4ENVIRO=" + os.devnull,
                        "-d",
                        str(root),
                        "-ztag",
                        "opened",
                        *[str(root / name) for name in files],
                    ]
                    env = {**base_env, "P4CONFIG": "", "P4ENVIRO": os.devnull}
                output = await self.runner(argv, root, env)
                if len(output) > MAX_OUTPUT:
                    raise JevError("vcs_output_limit", "Source-control status exceeded 64 KiB.")
                if kind == "git":
                    if output and not output.endswith(b"\0"):
                        raise JevError("vcs_response", "Git porcelain response is incomplete.")
                    parts, entries, index, outside = output.split(b"\0"), [], 0, False
                    while index < len(parts) - 1:
                        if not parts[index]:
                            raise JevError("vcs_response", "Unexpected empty Git status entry.")
                        token = parts[index].decode("utf-8", errors="strict")
                        code, path = token[:2], token[3:]
                        if (len(token) < 4 or token[2] != " "
                                or (code not in {"??", "!!"}
                                    and any(c not in " MADRCUT" for c in code))):
                            raise JevError("vcs_response", "Unexpected Git porcelain response.")
                        path = path[len(prefix):] if path.startswith(prefix) else None
                        # Rename source paths are separate NUL tokens; do not interpret as options.
                        if "R" in code or "C" in code:
                            index += 1
                            if index >= len(parts) or not parts[index]:
                                raise JevError("vcs_response", "Git rename lacks its source path.")
                            source = parts[index].decode("utf-8", errors="strict")
                            source = source[len(prefix):] if source.startswith(prefix) else None
                            if path not in files and source in files:
                                path, outside = source, True
                        if path in files:
                            entries.append({"path": path, "status": code})
                        else:
                            outside = True
                        index += 1
                    result[kind] = {
                        "status": "observed",
                        "entries": entries,
                        "clean_selected_files": not entries and not outside,
                        "out_of_scope_paths_omitted": outside,
                    }
                else:
                    result[kind] = {
                        "status": "observed",
                        "opened_file_count": sum(
                            line.startswith(b"... depotFile ") for line in output.splitlines()
                        ),
                        "note": "Opened state only; not shelved/submitted content proof.",
                    }
            except (JevError, UnicodeError) as exc:
                result[kind] = {
                    "status": "unavailable",
                    "reason": exc.code if isinstance(exc, JevError) else "vcs_response",
                }
        return result

    async def create(self, files):
        files, store = self._files(files), self._require()
        context = await self.bridge.call("context", {"query": "", "limit": 1})
        if project_identity(context.get("project_file", "")) != store.project:
            raise JevError("wrong_project", "Checkpoint requires the configured editor project.")
        manifest = self._manifest(files)
        receipt_id = store.begin(
            "file_checkpoint", {"files": files}, context, self.bridge.infrastructure.policy.hash
        )
        row = store.get(receipt_id)
        count = context.get("dirty_package_count")
        row.update(
            status="checkpoint_recorded",
            files=manifest,
            manifest_sha256=digest(manifest),
            configuration_sha256=digest(self.config.model_dump()),
            dirty_package_count=count
            if type(count) in (int, float) and 0 <= count <= 1e9
            else None,
            unsaved_warning="Disk hashes exclude unsaved editor changes; no save was requested.",
            restoration_available=False,
        )
        store.save(receipt_id, row)
        return row

    def compare(self, checkpoint_id):
        row = self._require().get(checkpoint_id)
        if row.get("action") != "file_checkpoint" or row.get("configuration_sha256") != digest(
            self.config.model_dump()
        ):
            raise JevError(
                "checkpoint_scope", "Checkpoint is not from this approved configuration."
            )
        before = row["files"]
        after = self._manifest([item["path"] for item in before])
        return {
            "checkpoint_id": checkpoint_id,
            "matches_disk": before == after,
            "changed_files": [
                new["path"] for old, new in zip(before, after, strict=True) if old != new
            ],
            "unsaved_warning": row["unsaved_warning"],
            "restoration_available": False,
            "scope": "Hash comparison only; never saves, stages, checks out or restores files.",
        }


def register_checkpoint_tools(server: FastMCP, bridge, config_file=""):
    checkpoints = ProjectCheckpoints(bridge, config_file)
    read = ToolAnnotations(readOnlyHint=True, destructiveHint=False, openWorldHint=False)
    vcs_read = ToolAnnotations(readOnlyHint=True, destructiveHint=False, openWorldHint=True)
    local = ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=False)
    files_type = Annotated[list[str], Field(min_length=1, max_length=32)]

    @server.tool(annotations=vcs_read)
    async def unreal_project_vcs_status(files: files_type) -> dict[str, Any]:
        """Read Git/Perforce state of configured approved files using pinned fixed utilities.

        Requires JEV_CHECKPOINT_CONFIG and JEV_RUNTIME_CONFIG. No arbitrary argv or shell,
        no checkout, add, submit, save or credentials in responses. Perforce may contact its server.
        """
        try:
            return {"ok": True, "result": await checkpoints.vcs_status(files)}
        except JevError as exc:
            return exc.as_dict()

    @server.tool(annotations=local)
    async def unreal_project_checkpoint(files: files_type) -> dict[str, Any]:
        """Retain hashes of explicitly approved disk files and warn about unsaved editor buffers.

        Manifest only, not a backup. Never copies, saves, stages, reverts or restores project files.
        """
        try:
            return {"ok": True, "result": await checkpoints.create(files)}
        except JevError as exc:
            return exc.as_dict()

    @server.tool(annotations=read)
    def unreal_project_checkpoint_compare(
        checkpoint_id: Annotated[str, Field(pattern=r"^[a-f0-9]{32}$")],
    ) -> dict[str, Any]:
        """Compare current approved disk hashes with one private checkpoint. No file changes."""
        try:
            return {"ok": True, "result": checkpoints.compare(checkpoint_id)}
        except JevError as exc:
            return exc.as_dict()

    return checkpoints
