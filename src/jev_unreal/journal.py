"""Bounded, process-local plan records for review and ambiguous-apply recovery."""

import json
import math
import time
from collections import OrderedDict
from copy import deepcopy
from datetime import UTC, datetime

from .errors import JevError


class PlanJournal:
    """Observations from this client, never an editor transaction history or retry queue."""

    def __init__(self, clock=None):
        self.clock = clock or time.monotonic
        self._records: OrderedDict[str, tuple[float, int, dict]] = OrderedDict()
        self.max_records = 64
        self.max_bytes = 2 * 1024 * 1024
        self.max_record_bytes = 65536
        self.ttl_seconds = 900

    def _prune(self):
        now = self.clock()
        for key, (created, _, _) in list(self._records.items()):
            if now - created >= self.ttl_seconds:
                del self._records[key]

    def get(self, plan_id: str) -> dict:
        self._prune()
        if plan_id not in self._records:
            raise JevError("plan_record_missing", "No retained plan record in this MCP process.")
        created, _, value = self._records[plan_id]
        result = deepcopy(value)
        age = max(0, self.clock() - created)
        result["retention_remaining_seconds"] = max(0, round(self.ttl_seconds - age, 3))
        if result["status"] == "previewed":
            result["preview_expired"] = age >= result.get("expires_in_seconds", 120)
        result["scope"] = (
            "This MCP process's observations; no retry, Undo, save or fresh scene read."
        )
        return result

    def put(self, plan_id: str, **fields):
        self._prune()
        existing = self._records.get(plan_id)
        created = existing[0] if existing else self.clock()
        value = (
            deepcopy(existing[2])
            if existing
            else {
                "plan_id": plan_id,
                "recorded_at": datetime.now(UTC).isoformat(),
                "status": "unknown",
            }
        )
        value.update(deepcopy(fields))
        if "expires_in_seconds" in value:
            expiry = value["expires_in_seconds"]
            if (
                type(expiry) not in (int, float)
                or not 0 <= expiry <= 120
                or not math.isfinite(expiry)
            ):
                value.pop("expires_in_seconds")
                value["details_omitted"] = True
        value["updated_at"] = datetime.now(UTC).isoformat()
        # No raw exception text, arbitrary response bodies or screenshot data are retained.
        encoded = json.dumps(value, ensure_ascii=True, allow_nan=False).encode()
        if len(encoded) > self.max_record_bytes:
            value.pop("operations", None)
            value.pop("verification", None)
            value["details_omitted"] = True
            encoded = json.dumps(value, ensure_ascii=True, allow_nan=False).encode()
        if len(encoded) > self.max_record_bytes:
            raise JevError("plan_record_too_large", "Plan record exceeds its bounded storage.")
        self._records.pop(plan_id, None)
        self._records[plan_id] = (created, len(encoded), value)
        while (
            len(self._records) > self.max_records
            or sum(item[1] for item in self._records.values()) > self.max_bytes
        ):
            self._records.popitem(last=False)

    def contains(self, plan_id: str) -> bool:
        self._prune()
        return plan_id in self._records
