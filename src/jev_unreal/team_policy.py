"""Explicit project policy for local coordination, receipts and named build jobs."""

import hashlib
import json
import os
import re
import stat
from enum import StrEnum
from pathlib import Path, PureWindowsPath
from typing import Annotated, Literal

from pydantic import BaseModel, ConfigDict, Field, ValidationError, field_validator

from .errors import JevError
from .profiles import _absolute_path, _bounded_file

MUTATING_ACTIONS = frozenset(
    {
        "preview",
        "apply",
        "frame",
        "validation_start",
        "validation_cancel",
        "functional_start",
        "functional_cancel",
        "blueprint_compile_preview",
        "blueprint_compile",
        "blueprint_pin_preview",
        "blueprint_graph_preview",
        "workflow_preview",
        "workflow_apply",
        "performance_start",
        "performance_cancel",
        "runtime_preview",
        "runtime_apply",
        "handoff_preview",
        "handoff_apply",
    }
)
PREVIEW_APPLY = {
    "preview": "apply",
    "blueprint_compile_preview": "blueprint_compile",
    "blueprint_pin_preview": "blueprint_compile",
    "blueprint_graph_preview": "blueprint_compile",
    "workflow_preview": "workflow_apply",
    "runtime_preview": "runtime_apply",
    "handoff_preview": "handoff_apply",
}
APPLY_ACTIONS = frozenset(PREVIEW_APPLY.values())


def project_identity(path: str) -> str:
    if "\\" in path or (len(path) > 1 and path[1] == ":"):
        return str(PureWindowsPath(path)).casefold()
    return os.path.normcase(os.path.abspath(path))


def digest(value: object) -> str:
    return hashlib.sha256(
        json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()
    ).hexdigest()


def local_path(value: str, *, exists: bool = False) -> Path:
    """Reject network paths, device paths and linked/reparse ancestors before local IO."""
    path = Path(_absolute_path(value))
    try:
        for item in (*reversed(path.parents), path):
            if not item.exists() and not item.is_symlink():
                continue
            metadata = item.lstat()
            if stat.S_ISLNK(metadata.st_mode) or getattr(metadata, "st_file_attributes", 0) & 0x400:
                raise ValueError
        if exists and not path.is_file():
            raise ValueError
    except (OSError, ValueError):
        raise JevError(
            "configuration", "Local runtime paths must not traverse links or devices."
        ) from None
    return path


class Strict(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True, frozen=True)


class JobKind(StrEnum):
    BUILD = "build"
    COOK = "cook"
    PACKAGE = "package"


Name = Annotated[str, Field(pattern=r"^[a-z][a-z0-9_-]{0,47}$", max_length=48)]


class NamedJob(Strict):
    name: Name
    kind: JobKind
    executable_sha256: str = Field(pattern=r"^[0-9a-f]{64}$")
    timeout_seconds: int = Field(default=600, ge=1, le=7200)
    max_output_bytes: int = Field(default=1048576, ge=1024, le=16 * 1048576)
    max_artifact_bytes: int = Field(default=1073741824, ge=1, le=100 * 1073741824)
    max_artifact_files: int = Field(default=4096, ge=1, le=100000)
    minimum_free_bytes: int = Field(default=1073741824, ge=0, le=500 * 1073741824)


class RuntimeConfig(Strict):
    version: Literal[1]
    project_file: str
    state_directory: str
    allowed_actions: list[str] = Field(default_factory=list, max_length=32)
    asset_roots: list[str] = Field(default_factory=list, max_length=32)
    allowed_operations: list[str] = Field(default_factory=list, max_length=64)
    blueprint_targets: list[Name] = Field(default_factory=list, max_length=64)
    functional_tests: list[Name] = Field(default_factory=list, max_length=64)
    validation_rules: list[Name] = Field(default_factory=list, max_length=64)
    receipt_retention_days: int = Field(default=7, ge=1, le=30)
    max_receipts: int = Field(default=256, ge=16, le=1024)
    engine_root: str | None = None
    dotnet_relative_root: str | None = Field(
        default=None,
        pattern=r"^Engine/Binaries/ThirdParty/DotNet/[0-9][0-9.]{0,31}/win-x64$",
    )
    dotnet_host_sha256: str | None = Field(default=None, pattern=r"^[0-9a-f]{64}$")
    output_directory: str | None = None
    editor_target: str | None = Field(default=None, pattern=r"^[A-Za-z][A-Za-z0-9_]{0,79}Editor$")
    jobs: list[NamedJob] = Field(default_factory=list, max_length=12)

    @field_validator("allowed_actions")
    @classmethod
    def known_actions(cls, values):
        if len(values) != len(set(values)) or not set(values) <= MUTATING_ACTIONS:
            raise ValueError("Policy must list unique known mutation actions.")
        return values

    @field_validator("asset_roots")
    @classmethod
    def roots(cls, values):
        if any(not re.fullmatch(r"/(?:Game|Engine|Temp)(?:/[A-Za-z0-9_]+)*", v) for v in values):
            raise ValueError("Roots use exact /Game, /Engine or explicit /Temp folder prefixes.")
        return values

    @field_validator("allowed_operations")
    @classmethod
    def operations(cls, values):
        if any(not re.fullmatch(r"[a-z][a-z0-9_]{0,63}", v) for v in values):
            raise ValueError("Operation names must be explicit lowercase identifiers.")
        return values


def read_runtime_config(filename: str, expected_project: str) -> RuntimeConfig:
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError
            result[key] = value
        return result

    try:
        raw = _bounded_file(local_path(filename, exists=True), 65536)
        data = json.loads(raw, object_pairs_hook=unique)
        config = RuntimeConfig.model_validate_json(json.dumps(data))
        if not expected_project or project_identity(config.project_file) != project_identity(
            expected_project
        ):
            raise ValueError
        project = local_path(config.project_file, exists=True)
        if project.suffix.lower() != ".uproject":
            raise ValueError
        state = local_path(config.state_directory)
        if state == project.parent or state.is_relative_to(project.parent):
            raise ValueError
        if config.jobs:
            if not config.engine_root or not config.output_directory:
                raise ValueError
            engine = local_path(config.engine_root)
            output = local_path(config.output_directory)
            if not engine.is_dir() or output.is_relative_to(engine):
                raise ValueError
            if output == project.parent or output.is_relative_to(project.parent):
                raise ValueError
            if len({job.name for job in config.jobs}) != len(config.jobs):
                raise ValueError
            if any(job.kind is JobKind.BUILD for job in config.jobs) and not config.editor_target:
                raise ValueError
            if any(job.kind in {JobKind.BUILD, JobKind.PACKAGE} for job in config.jobs) and (
                not config.dotnet_relative_root or not config.dotnet_host_sha256
            ):
                raise ValueError
        return config
    except (ValueError, TypeError, UnicodeError, RecursionError, ValidationError):
        raise JevError(
            "configuration",
            "Runtime configuration is invalid or names a different project. "
            "Use unique keys, local paths outside project source for state/output, and named jobs.",
        ) from None


class TeamPolicy:
    def __init__(self, config: RuntimeConfig):
        self.config = config
        self.hash = digest(config.model_dump(mode="json"))

    def check_path(self, value: str):
        if (
            ".." in value
            or "\\" in value
            or len(value) > 2048
            or any(ord(c) < 32 for c in value)
            or not any(
                value == root or value.startswith(root + "/") for root in self.config.asset_roots
            )
        ):
            raise JevError("policy_denied", "An asset or world is outside approved package roots.")

    def check(self, action: str, params: dict, status: dict):
        if action not in self.config.allowed_actions:
            raise JevError("policy_denied", "This mutation is not in the project team policy.")
        world = status.get("world_path")
        if not isinstance(world, str) or not world:
            raise JevError("policy_denied", "An exact current world is required by team policy.")
        self.check_path(world)

        self.check_data(params)
        if action in {
            "blueprint_compile_preview",
            "blueprint_pin_preview",
            "blueprint_graph_preview",
        }:
            if params.get("target_id") not in self.config.blueprint_targets:
                raise JevError("policy_denied", "Blueprint target alias is not approved.")
        if (
            action == "functional_start"
            and params.get("test_id") not in self.config.functional_tests
        ):
            raise JevError("policy_denied", "Gameplay test alias is not approved.")
        if action == "validation_start" and not set(params.get("rule_ids", [])) <= set(
            self.config.validation_rules
        ):
            raise JevError("policy_denied", "Validator alias is not approved.")
        for operation in params.get("operations", []):
            if operation.get("op") == "spawn_primitive":
                self.check_path("/Engine/BasicShapes/" + str(operation.get("shape", "")))

    def check_data(self, value, *, key="", result=False):
        """Check recursive normalized references, including implicit copied mesh/material state."""
        if key in {"expected_project", "project_file"}:
            return  # Local identity fields are not Unreal package paths.
        if isinstance(value, dict):
            for child_key, child in value.items():
                is_operation = child_key in {"op", "kind", "operation"} and (
                    not result
                    or child_key == "op"
                    or (child_key == "kind" and key in {"requested", "change"})
                    or (child_key == "operation" and key == "graph_edit")
                )
                if is_operation and (child not in self.config.allowed_operations):
                    raise JevError("policy_denied", "This operation is not approved by policy.")
                self.check_data(child, key=child_key, result=result)
        elif isinstance(value, list):
            for child in value:
                self.check_data(child, key=key, result=result)
        elif isinstance(value, str) and value.startswith("/"):
            if result and value.startswith("/Script/") and "class" in key:
                return  # Native class identity metadata is not a mutable package reference.
            self.check_path(value)
