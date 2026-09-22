import argparse
import asyncio
import json
import sys
from pathlib import Path

from . import __version__
from .bridge import UnrealBridge
from .config import Settings
from .decision import DecisionClient
from .errors import JevError


async def run_command(args, settings: Settings) -> dict:
    if args.command == "status":
        bridge = UnrealBridge(settings)
        try:
            return await bridge.call("status")
        finally:
            await bridge.close()
    client = DecisionClient(settings)
    try:
        if args.command == "smoke":
            return await client.decide(
                {"engine": "Unreal", "message": "Compiler error C2065: undeclared identifier"},
                {
                    "category": {
                        "type": "choice",
                        "instructions": "Classify this diagnostic.",
                        "criteria": {
                            "compile": "C++ compilation error",
                            "art": "Visual art feedback",
                        },
                    }
                },
            )
        path = Path(args.file)
        if (await asyncio.to_thread(path.stat)).st_size > 65536:
            raise JevError("request_too_large", "Input file exceeds 64 KiB.")
        data = json.loads(await asyncio.to_thread(path.read_text, encoding="utf-8"))
        return await client.decide(data["state"], data["questions"])
    finally:
        await client.close()


def main():
    parser = argparse.ArgumentParser(description="Jev decisions + guarded Unreal editor MCP")
    parser.add_argument("--version", action="version", version=__version__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("serve", help="Run the MCP stdio server")
    commands.add_parser("status", help="Inspect the configured Unreal editor")
    commands.add_parser("smoke", help="Make one small real provider request")
    commands.add_parser("decide", help="Evaluate a JSON state/questions file").add_argument("file")
    args = parser.parse_args()
    try:
        settings = Settings.from_env()
        if args.command == "serve":
            from .server import create_server

            create_server(settings).run(transport="stdio")
        else:
            print(json.dumps(asyncio.run(run_command(args, settings)), indent=2, allow_nan=False))
    except JevError as exc:
        print(json.dumps(exc.as_dict()), file=sys.stderr)
        raise SystemExit(1) from None
    except (OSError, ValueError, KeyError, TypeError):
        print(
            '{"ok":false,"error":{"code":"input_error","message":"Invalid input file."}}',
            file=sys.stderr,
        )
        raise SystemExit(1) from None
