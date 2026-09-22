"""Explicit local configuration. Never search files for credentials."""

import os
from dataclasses import dataclass, field
from pathlib import Path
from urllib.parse import urlparse

from .errors import JevError


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
        if parsed.port is None:
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
        token = os.getenv("JEV_BRIDGE_TOKEN", "")
        if not token and (token_file := os.getenv("JEV_BRIDGE_TOKEN_FILE")):
            try:
                token = Path(token_file).read_text(encoding="utf-8-sig").strip()
            except (OSError, UnicodeError):
                raise JevError("configuration", "Cannot read JEV_BRIDGE_TOKEN_FILE.") from None
        try:
            return cls(
                provider=provider,
                api_key=os.getenv(
                    "OPENROUTER_API_KEY" if provider == "openrouter" else "TYPESAFE_API_KEY", ""
                ),
                model=os.getenv(
                    "JEV_MODEL", "typesafe/jev-1.13" if provider == "openrouter" else "jev-1.13.0"
                ),
                bridge_url=os.getenv("JEV_BRIDGE_URL", "http://127.0.0.1:9845"),
                bridge_token=token,
                expected_project=os.getenv("JEV_EXPECTED_PROJECT", ""),
                max_requests=int(os.getenv("JEV_MAX_REQUESTS", "100")),
            )
        except ValueError:
            raise JevError("configuration", "JEV_MAX_REQUESTS must be an integer.") from None
