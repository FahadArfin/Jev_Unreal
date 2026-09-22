"""Explicit local configuration. Never search files for credentials."""

import os
from dataclasses import dataclass, field
from urllib.parse import urlparse

from .errors import JevError
from .profiles import read_token_file, selected_profile


def loopback_url(value: str) -> str:
    try:
        parsed = urlparse(value)
    except ValueError:
        raise JevError("configuration", "Bridge URL is malformed.") from None
    if (
        parsed.scheme != "http"
        or parsed.hostname not in {"127.0.0.1", "::1"}
        or parsed.username
        or parsed.password
        or parsed.path not in {"", "/"}
        or parsed.query
        or parsed.fragment
    ):
        raise JevError("configuration", "Bridge URL must be a literal loopback HTTP origin.")
    try:
        if parsed.port is None or not 1024 <= parsed.port <= 65535:
            raise ValueError
    except ValueError:
        raise JevError("configuration", "Bridge URL must include a valid port.") from None
    return value.rstrip("/")


@dataclass(frozen=True)
class Settings:
    provider: str = "openrouter"
    api_key: str = field(default="", repr=False)
    model: str = "typesafe/jev-1.13"
    bridge_url: str = "http://127.0.0.1:9845"
    bridge_token: str = field(default="", repr=False)
    expected_project: str = ""
    profile_id: str = ""
    catalog_file: str = ""
    max_requests: int = 100
    cache_seconds: float = 60
    timeout_seconds: float = 15

    def __post_init__(self):
        if self.provider not in {"openrouter", "typesafe"}:
            raise JevError("configuration", "JEV_PROVIDER must be openrouter or typesafe.")
        loopback_url(self.bridge_url)
        if not 1 <= self.max_requests <= 10000:
            raise JevError("configuration", "JEV_MAX_REQUESTS must be between 1 and 10000.")
        if not 0 <= self.cache_seconds <= 3600 or not 0 < self.timeout_seconds <= 120:
            raise JevError("configuration", "Cache or timeout is outside the allowed range.")

    @property
    def endpoint(self) -> str:
        return {
            "openrouter": "https://openrouter.ai/api/alpha/decisions",
            "typesafe": "https://api.typesafe.ai/v1/systemone",
        }[self.provider]

    @classmethod
    def from_env(cls):
        provider = os.getenv("JEV_PROVIDER", "openrouter")
        profile_file = os.getenv("JEV_PROFILES_FILE", "")
        profile_id = os.getenv("JEV_PROFILE", "")
        if bool(profile_file) != bool(profile_id):
            raise JevError("configuration", "Set both JEV_PROFILES_FILE and JEV_PROFILE together.")
        if profile_file:
            profile = selected_profile(profile_file, profile_id)
            token = read_token_file(profile.bridge_token_file)
            bridge_url = profile.bridge_url
            expected_project = profile.project_file
        else:
            token = os.getenv("JEV_BRIDGE_TOKEN", "")
            if not token and (token_file := os.getenv("JEV_BRIDGE_TOKEN_FILE")):
                token = read_token_file(token_file)
            bridge_url = os.getenv("JEV_BRIDGE_URL", "")
            if not bridge_url:
                port = os.getenv("JEV_BRIDGE_PORT", "9845")
                if (
                    not 4 <= len(port) <= 5
                    or not port.isascii()
                    or not port.isdecimal()
                    or not 1024 <= int(port) <= 65535
                ):
                    raise JevError(
                        "configuration", "JEV_BRIDGE_PORT must be between 1024 and 65535."
                    )
                bridge_url = f"http://127.0.0.1:{int(port)}"
            expected_project = os.getenv("JEV_EXPECTED_PROJECT", "")
        try:
            return cls(
                provider=provider,
                api_key=os.getenv(
                    "OPENROUTER_API_KEY" if provider == "openrouter" else "TYPESAFE_API_KEY", ""
                ),
                model=os.getenv(
                    "JEV_MODEL", "typesafe/jev-1.13" if provider == "openrouter" else "jev-1.13.0"
                ),
                bridge_url=bridge_url,
                bridge_token=token,
                expected_project=expected_project,
                profile_id=profile_id,
                catalog_file=os.getenv("JEV_CATALOG_FILE", ""),
                max_requests=int(os.getenv("JEV_MAX_REQUESTS", "100")),
            )
        except ValueError:
            raise JevError("configuration", "JEV_MAX_REQUESTS must be an integer.") from None
