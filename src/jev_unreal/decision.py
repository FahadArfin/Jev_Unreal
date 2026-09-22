"""Bounded, asynchronous Jev calls. State stays in memory and is never logged."""

import asyncio
import copy
import hashlib
import json
import time
from collections import OrderedDict

import httpx

from .config import Settings
from .errors import JevError
from .schema import request_body, validate_answers


class DecisionClient:
    def __init__(self, settings: Settings, transport: httpx.AsyncBaseTransport | None = None):
        self.settings = settings
        self._http = httpx.AsyncClient(
            transport=transport,
            timeout=settings.timeout_seconds,
            follow_redirects=False,
            trust_env=False,
        )
        # Serializing also coalesces repeated concurrent requests via the cache recheck.
        self._lock = asyncio.Lock()
        self._cache: OrderedDict[str, tuple[float, dict]] = OrderedDict()
        self.requests = 0
        self.cache_hits = 0
        self.failures = 0
        self._blocked_until = 0.0

    async def close(self):
        await self._http.aclose()

    def metrics(self) -> dict:
        return {
            "provider": self.settings.provider,
            "model": self.settings.model,
            "requests_sent": self.requests,
            "request_limit": self.settings.max_requests,
            "cache_hits": self.cache_hits,
            "configured": bool(self.settings.api_key),
        }

    async def decide(self, state, questions: dict) -> dict:
        body = request_body(state, questions, self.settings.model)
        if not self.settings.api_key:
            raise JevError("missing_api_key", "Configure the provider API key locally to use Jev.")
        digest = hashlib.sha256(json.dumps(body, sort_keys=True).encode()).hexdigest()
        async with self._lock:
            now = time.monotonic()
            cached = self._cache.get(digest)
            if cached and now < cached[0]:
                self.cache_hits += 1
                self._cache.move_to_end(digest)
                return {**copy.deepcopy(cached[1]), "cached": True, "latency_ms": 0.0}
            if now < self._blocked_until:
                raise JevError("circuit_open", "Provider is unavailable; retry after 30 seconds.")
            if self.requests >= self.settings.max_requests:
                raise JevError("request_limit", "Session provider request limit reached.")
            self.requests += 1
            start = time.perf_counter()
            try:
                async with self._http.stream(
                    "POST",
                    self.settings.endpoint,
                    headers={
                        "Authorization": f"Bearer {self.settings.api_key}",
                        "Content-Type": "application/json",
                    },
                    json=body,
                ) as response:
                    if response.status_code != 200:
                        code = "rate_limited" if response.status_code == 429 else "provider_error"
                        raise JevError(code, f"Provider returned HTTP {response.status_code}.")
                    content = bytearray()
                    async for chunk in response.aiter_bytes():
                        content.extend(chunk)
                        if len(content) > 262144:
                            raise JevError("invalid_response", "Provider response exceeds 256 KiB.")
                try:
                    raw = json.loads(content)
                except (ValueError, RecursionError):
                    raise JevError(
                        "invalid_response", "Provider response is not valid JSON."
                    ) from None
                result = validate_answers(raw, body["questions"])
            except (httpx.HTTPError, JevError) as exc:
                self.failures += 1
                if self.failures >= 3:
                    self._blocked_until = time.monotonic() + 30
                if isinstance(exc, JevError):
                    raise
                raise JevError(
                    "provider_unavailable", "Provider request failed or timed out."
                ) from None
            self.failures = 0
            result.update(
                provider=self.settings.provider,
                cached=False,
                latency_ms=round((time.perf_counter() - start) * 1000, 2),
            )
            self._cache[digest] = (
                time.monotonic() + self.settings.cache_seconds,
                copy.deepcopy(result),
            )
            self._cache.move_to_end(digest)
            while len(self._cache) > 128:
                self._cache.popitem(last=False)
            return result
