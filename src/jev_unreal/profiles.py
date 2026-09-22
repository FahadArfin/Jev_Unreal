"""Explicit startup-only project/endpoint/token-file bundles for independent editors."""

import json
import os
import re
import stat
from dataclasses import dataclass, field
from pathlib import Path

from .errors import JevError

MAX_PROFILE_BYTES = 65536
MAX_TOKEN_BYTES = 4096


def _invalid(message: str):
    raise JevError("configuration", message)


def _bounded_file(path: Path, limit: int) -> bytes:
    # Do not follow links or read a pipe/device while resolving connection secrets.
    try:
        for ancestor in (*reversed(path.parents), path):
            metadata = ancestor.lstat()
            if stat.S_ISLNK(metadata.st_mode) or getattr(metadata, "st_file_attributes", 0) & 0x400:
                raise ValueError
        metadata = path.stat()
        if not stat.S_ISREG(metadata.st_mode) or metadata.st_size > limit:
            raise ValueError
        with path.open("rb") as handle:
            data = handle.read(limit + 1)
        if len(data) > limit:
            raise ValueError
        return data
    except (OSError, ValueError):
        _invalid("Cannot safely read the selected connection configuration file within its limit.")


def _absolute_path(value: str, *, project=False) -> str:
    if (
        not isinstance(value, str)
        or not 1 <= len(value) <= 2048
        or any(ord(character) < 32 for character in value)
        or not Path(value).is_absolute()
        or ".." in Path(value).parts
        or (project and Path(value).suffix.lower() != ".uproject")
    ):
        _invalid("Profile project and token-file paths must be explicit absolute local file paths.")
    # Network shares can authenticate to other machines merely by being accessed.
    if value.startswith(("\\\\", "//")):
        _invalid("Connection profile paths must be local, not network shares.")
    return str(Path(os.path.abspath(value)))


@dataclass(frozen=True)
class ConnectionProfile:
    id: str
    project_file: str
    bridge_url: str
    bridge_token_file: str = field(repr=False)


def read_profiles(path: str | Path) -> tuple[ConnectionProfile, ...]:
    """Read only the bounded profile definitions, never any referenced token files."""

    def unique_object(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError
            result[key] = value
        return result

    try:
        data = json.loads(
            _bounded_file(Path(_absolute_path(os.path.abspath(path))), MAX_PROFILE_BYTES).decode(
                "utf-8-sig"
            ),
            object_pairs_hook=unique_object,
        )
    except (ValueError, UnicodeError, RecursionError, TypeError):
        _invalid("Connection profiles must be a valid bounded JSON object with unique keys.")
    if (
        not isinstance(data, dict)
        or set(data) != {"version", "profiles"}
        or type(data["version"]) is not int
        or data["version"] != 1
        or not isinstance(data["profiles"], list)
        or not 1 <= len(data["profiles"]) <= 8
    ):
        _invalid("Connection profiles require version 1 and between one and eight profiles.")
    result = []
    names, projects, endpoints = set(), set(), set()
    for value in data["profiles"]:
        if not isinstance(value, dict) or set(value) != {
            "id",
            "project_file",
            "bridge_url",
            "bridge_token_file",
        }:
            _invalid(
                "Each profile requires only id, project_file, bridge_url and bridge_token_file."
            )
        if not isinstance(value["id"], str) or not re.fullmatch(
            "[a-z][a-z0-9_-]{0,47}", value["id"]
        ):
            _invalid("Profile IDs use 1-48 lowercase letters, numbers, hyphens or underscores.")
        url = value["bridge_url"]
        match = (
            re.fullmatch(r"http://127\.0\.0\.1:([1-9][0-9]{3,4})/?", url)
            if isinstance(url, str)
            else None
        )
        if match is None or not 1024 <= int(match[1]) <= 65535:
            _invalid("Profile bridge URLs require literal http://127.0.0.1 and port 1024-65535.")
        url = url.rstrip("/")
        project = _absolute_path(value["project_file"], project=True)
        token_file = _absolute_path(value["bridge_token_file"])
        project_key = os.path.normcase(project)
        if value["id"] in names or project_key in projects or url in endpoints:
            _invalid("Profile IDs, project paths and bridge endpoints must each be unique.")
        names.add(value["id"])
        projects.add(project_key)
        endpoints.add(url)
        result.append(ConnectionProfile(value["id"], project, url, token_file))
    return tuple(result)


def selected_profile(path: str | Path, profile_id: str) -> ConnectionProfile:
    for profile in read_profiles(path):
        if profile.id == profile_id:
            return profile
    _invalid("JEV_PROFILE does not name a profile in the selected profile file.")


def summaries(path: str | Path) -> list[dict]:
    return [
        {
            "id": profile.id,
            "project_file": profile.project_file,
            "bridge_url": profile.bridge_url,
            "token_file_configured": True,
        }
        for profile in read_profiles(path)
    ]


def read_token_file(path: str | Path) -> str:
    """Resolve one explicit token file, bounded to 4 KiB; errors never include its value/path."""
    try:
        data = _bounded_file(Path(_absolute_path(os.path.abspath(path))), MAX_TOKEN_BYTES)
        token = data.decode("utf-8-sig").strip()
        if (
            not 32 <= len(token) <= 256
            or not token.isascii()
            or any(c.isspace() for c in token)
        ):
            raise ValueError
        if any(ord(c) < 33 or ord(c) > 126 for c in token):
            raise ValueError
        return token
    except (JevError, ValueError, UnicodeError, TypeError):
        _invalid("Cannot read a valid bridge token from the selected token file (maximum 4 KiB).")
