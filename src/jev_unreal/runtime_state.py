"""Bounded local receipts and atomic cross-process project leases; never a retry queue."""

import json
import os
import sqlite3
import time
import uuid
from contextlib import contextmanager

from .errors import JevError
from .team_policy import RuntimeConfig, digest, local_path, project_identity


class RuntimeStore:
    def __init__(self, config: RuntimeConfig, *, clock=time.time):
        self.config = config
        self.clock = clock
        self.project = project_identity(config.project_file)
        self.project_key = digest(self.project)
        self.owner = uuid.uuid4().hex
        directory = local_path(config.state_directory)
        directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.path = directory / (self.project_key + ".sqlite3")
        local_path(str(self.path))
        with self.connection() as db:
            db.executescript("""
                CREATE TABLE IF NOT EXISTS metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS receipts (
                    id TEXT PRIMARY KEY, created REAL NOT NULL, updated REAL NOT NULL,
                    body TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS leases (
                    project TEXT PRIMARY KEY, owner TEXT NOT NULL, id TEXT NOT NULL,
                    kind TEXT NOT NULL, expires REAL NOT NULL
                );
                CREATE TABLE IF NOT EXISTS plans (
                    session TEXT NOT NULL, id TEXT NOT NULL, action TEXT NOT NULL,
                    policy TEXT NOT NULL, expires REAL NOT NULL, consumed INTEGER NOT NULL,
                    PRIMARY KEY(session, id)
                );
            """)
            previous = db.execute("SELECT value FROM metadata WHERE key='project'").fetchone()
            if previous and previous[0] != self.project:
                raise JevError("state_identity", "Runtime state belongs to a different project.")
            db.execute("INSERT OR IGNORE INTO metadata VALUES ('project', ?)", (self.project,))
        try:
            os.chmod(self.path, 0o600)
        except OSError:
            pass  # Windows access is inherited from the explicitly chosen private directory.

    @contextmanager
    def connection(self):
        db = None
        try:
            local_path(str(self.path))
            db = sqlite3.connect(self.path, timeout=1, isolation_level=None)
            db.execute("PRAGMA trusted_schema=OFF")
            db.execute("PRAGMA synchronous=FULL")
            db.execute("PRAGMA journal_mode=DELETE")
            db.execute("PRAGMA max_page_count=4096")
            db.execute("BEGIN IMMEDIATE")
            yield db
            if db.in_transaction:
                db.execute("COMMIT")
        except sqlite3.Error:
            if db is not None and db.in_transaction:
                db.execute("ROLLBACK")
            raise JevError(
                "state_unavailable", "Bounded local runtime state is unavailable."
            ) from None
        except BaseException:
            if db is not None and db.in_transaction:
                db.execute("ROLLBACK")
            raise
        finally:
            if db is not None:
                db.close()

    def _prune(self, db):
        db.execute(
            "DELETE FROM receipts WHERE updated < ?",
            (self.clock() - self.config.receipt_retention_days * 86400,),
        )
        db.execute("DELETE FROM plans WHERE expires < ?", (self.clock(),))
        db.execute(
            "DELETE FROM receipts WHERE id IN (SELECT id FROM receipts "
            "ORDER BY created DESC LIMIT -1 OFFSET ?)",
            (self.config.max_receipts,),
        )

    def begin(self, action: str, params: dict, identity: dict, policy_hash: str) -> str:
        receipt_id = uuid.uuid4().hex
        # Only bounded identities and digests, never arguments, provider text, tokens or logs.
        evidence = {}
        for key in ("session_id", "world_path", "revision"):
            value = identity.get(key)
            if not isinstance(value, str) or not 1 <= len(value) <= 2048:
                raise JevError(
                    "state_identity", "Complete editor identity is required for receipts."
                )
            evidence[key] = value
        body = {
            "receipt_id": receipt_id,
            "project_file": self.project,
            "action": action,
            "identity": evidence,
            "policy_sha256": policy_hash,
            "request_sha256": digest(params),
            "status": "dispatched_uncertain",
            "owner": self.owner,
            "created_at": self.clock(),
        }
        for key in ("plan_id", "job_id"):
            value = params.get(key)
            if isinstance(value, str) and 1 <= len(value) <= 64:
                body[key] = value
        self.save(receipt_id, body)
        return receipt_id

    def save(self, receipt_id: str, body: dict):
        encoded = json.dumps(body, allow_nan=False, sort_keys=True)
        if len(encoded.encode()) > 16384:
            raise JevError("receipt_too_large", "Receipt metadata exceeds 16 KiB.")
        with self.connection() as db:
            self._prune(db)
            db.execute(
                "INSERT INTO receipts VALUES (?, ?, ?, ?) ON CONFLICT(id) "
                "DO UPDATE SET updated=excluded.updated, body=excluded.body",
                (receipt_id, self.clock(), self.clock(), encoded),
            )
            self._prune(db)

    def finish(self, receipt_id: str, status: str, *, result=None, error_code=None):
        record = self.get(receipt_id)
        record.pop("scope", None)
        record["status"] = status
        record["updated_at"] = self.clock()
        if result is not None:
            record["result_sha256"] = digest(result)
            for key in ("plan_id", "job_id", "status", "applied", "compiled", "readback_verified"):
                value = result.get(key)
                if type(value) is bool or (isinstance(value, str) and len(value) <= 64):
                    record["observed_" + key] = value
        if error_code:
            record["error_code"] = error_code
        self.save(receipt_id, record)

    def get(self, receipt_id: str) -> dict:
        with self.connection() as db:
            self._prune(db)
            row = db.execute("SELECT body FROM receipts WHERE id=?", (receipt_id,)).fetchone()
        if row is None:
            raise JevError("receipt_missing", "No retained local receipt for this project and ID.")
        result = json.loads(row[0])
        result.pop("owner", None)
        result["scope"] = (
            "Historical local observation only. Uncertain means no confirmed outcome; "
            "never replay from a receipt. Inspect the exact editor/project and fresh state."
        )
        return result

    def list_receipts(self, limit: int = 20) -> list[dict]:
        if type(limit) is not int or not 1 <= limit <= 100:
            raise JevError("invalid_request", "Receipt limit must be 1..100.")
        with self.connection() as db:
            self._prune(db)
            rows = db.execute("SELECT id FROM receipts ORDER BY created DESC LIMIT ?", (limit,))
            ids = [row[0] for row in rows]
        return [self.get(receipt_id) for receipt_id in ids]

    def forget(self, receipt_id: str) -> bool:
        with self.connection() as db:
            return db.execute("DELETE FROM receipts WHERE id=?", (receipt_id,)).rowcount == 1

    def authorize_plan(self, session: str, plan_id: str, action: str, policy: str):
        if not isinstance(plan_id, str) or not 1 <= len(plan_id) <= 64:
            raise JevError("invalid_plan", "Native preview did not return a bounded plan ID.")
        with self.connection() as db:
            self._prune(db)
            count = db.execute("SELECT COUNT(*) FROM plans").fetchone()[0]
            if count >= 128:
                raise JevError("too_many_plans", "Local reviewed plan capacity is full.")
            # Repeated plan IDs must never reset consumed state or grant another lifetime.
            db.execute(
                "INSERT OR IGNORE INTO plans VALUES (?, ?, ?, ?, ?, 0)",
                (session, plan_id, action, policy, self.clock() + 120),
            )

    def consume_plan(self, session: str, plan_id: str, action: str, policy: str):
        with self.connection() as db:
            changed = db.execute(
                "UPDATE plans SET consumed=1 WHERE session=? AND id=? "
                "AND action=? AND policy=? AND expires>? AND consumed=0",
                (session, plan_id, action, policy, self.clock()),
            ).rowcount
            if changed != 1:
                raise JevError(
                    "policy_plan_required",
                    "Apply needs an unconsumed preview from "
                    "this exact session and policy. Create and review a fresh preview.",
                )

    def acquire(self, seconds: int = 60, *, kind: str = "operation") -> tuple[str, bool]:
        if type(seconds) is not int or not 1 <= seconds <= 300:
            raise JevError("invalid_request", "Lease duration must be 1..300 seconds.")
        with self.connection() as db:
            row = db.execute(
                "SELECT owner,id,kind,expires FROM leases WHERE project=?", (self.project_key,)
            ).fetchone()
            if row and row[3] > self.clock():
                if row[0] != self.owner or (row[2] == "job" and kind != "job"):
                    raise JevError(
                        "project_leased", "Another client or job holds this project lease."
                    )
                return row[1], False
            lease_id = uuid.uuid4().hex
            db.execute(
                "INSERT OR REPLACE INTO leases VALUES (?, ?, ?, ?, ?)",
                (
                    self.project_key,
                    self.owner,
                    lease_id,
                    kind,
                    self.clock() + seconds,
                ),
            )
            return lease_id, True

    def renew(self, lease_id: str, seconds: int = 60):
        with self.connection() as db:
            changed = db.execute(
                "UPDATE leases SET expires=? WHERE project=? AND owner=? AND id=? AND expires>?",
                (self.clock() + seconds, self.project_key, self.owner, lease_id, self.clock()),
            )
            if changed.rowcount != 1:
                raise JevError("lease_lost", "The owned project lease expired or was cancelled.")

    def release(self, lease_id: str) -> bool:
        with self.connection() as db:
            return (
                db.execute(
                    "DELETE FROM leases WHERE project=? AND owner=? AND id=?",
                    (self.project_key, self.owner, lease_id),
                ).rowcount
                == 1
            )

    def lease_status(self) -> dict:
        with self.connection() as db:
            row = db.execute(
                "SELECT owner,id,kind,expires FROM leases WHERE project=?", (self.project_key,)
            ).fetchone()
        if not row or row[3] <= self.clock():
            return {"held": False}
        return {
            "held": True,
            "owned_by_this_client": row[0] == self.owner,
            "lease_id": row[1],
            "kind": row[2],
            "expires_at": row[3],
        }
