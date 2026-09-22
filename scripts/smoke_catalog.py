"""Read-only live MCP discovery smoke; uses explicit JEV_CATALOG_FILE endpoints.

No provider request or external tools/call is made. Output contains counts and
catalog hashes, not schemas, descriptions, endpoint URLs, or credentials.
"""

import argparse
import asyncio
import json
import os
import sys
import time

from jev_unreal.catalog import ToolCatalog
from jev_unreal.errors import JevError


async def run(config_path: str, query: str = "actor") -> dict:
    if not config_path:
        raise JevError(
            "catalog_config_missing", "Set JEV_CATALOG_FILE to a local configuration file."
        )
    catalog = ToolCatalog(config_path)
    started = time.monotonic()
    # Two refreshes, each independently bounded to 45 seconds by ToolCatalog.
    async with asyncio.timeout(95):
        first = await catalog.refresh()
        if not first.server_ids or first.entry_count == 0:
            raise JevError("catalog_empty", "The configured endpoints advertised no tools.")
        hits = catalog.search(query, limit=5)
        if not hits:
            raise JevError("catalog_query_empty", "The smoke search matched no advertised tools.")
        for hit in hits:
            entry = catalog.get(hit.id, version=first.version)
            if entry.id != hit.id or not isinstance(entry.input_schema, dict):
                raise JevError("catalog_smoke_failed", "Exact-schema retrieval failed.")
        try:
            catalog.get(hits[0].id, version="deliberately-stale-smoke-version")
        except JevError as exc:
            if exc.code != "catalog_stale":
                raise
        else:
            raise JevError("catalog_smoke_failed", "A stale version was not rejected.")
        second = await catalog.refresh()
        unchanged = first.version == second.version
        if unchanged:
            catalog.get(hits[0].id, version=first.version)
        else:
            try:
                catalog.get(hits[0].id, version=first.version)
            except JevError as exc:
                if exc.code != "catalog_stale":
                    raise
            else:
                raise JevError(
                    "catalog_smoke_failed", "A changed catalog accepted its old version."
                )
        return {
            "ok": True,
            "discovery_only": True,
            "provider_requests": 0,
            "external_tool_calls": 0,
            "server_count": len(second.server_ids),
            "tool_count": second.tool_count,
            "entry_count": second.entry_count,
            "warning_count": len(second.warnings),
            "schema_retrievals": len(hits),
            "stale_version_rejected": True,
            "unchanged_between_refreshes": unchanged,
            "catalog_version": second.version,
            "elapsed_ms": round((time.monotonic() - started) * 1000, 2),
        }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--query", default="actor", help="Local lexical search query (default: actor)"
    )
    args = parser.parse_args()
    try:
        result = asyncio.run(run(os.environ.get("JEV_CATALOG_FILE", ""), args.query))
    except (JevError, OSError, ValueError, TimeoutError) as exc:
        code = exc.code if isinstance(exc, JevError) else "catalog_smoke_failed"
        print(json.dumps({"ok": False, "error_code": code}), file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2, allow_nan=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
