"""Convert a bounded native viewport PNG to MCP image content, never filesystem access."""

import base64
import binascii
import json
import struct
import zlib

from mcp.types import CallToolResult, ImageContent, TextContent

from .errors import JevError


def _png_dimensions(raw: bytes) -> tuple[int, int]:
    """Validate bounded PNG structure/CRCs without decoding arbitrary image pixels."""
    if len(raw) < 33 or raw[:8] != b"\x89PNG\r\n\x1a\n":
        raise JevError("capture_failed", "Editor returned invalid PNG metadata.")
    offset, width, height, data_bytes = 8, 0, 0, 0
    seen_header = seen_data = data_ended = seen_palette = False
    color_type = None
    while offset < len(raw):
        if len(raw) - offset < 12:
            raise JevError("capture_failed", "Editor returned a truncated PNG chunk.")
        length = struct.unpack_from(">I", raw, offset)[0]
        end = offset + 12 + length
        if end > len(raw):
            raise JevError("capture_failed", "Editor returned a truncated PNG chunk.")
        kind = raw[offset + 4 : offset + 8]
        if any(not (65 <= value <= 90 or 97 <= value <= 122) for value in kind):
            raise JevError("capture_failed", "Editor returned an invalid PNG chunk type.")
        payload = raw[offset + 8 : offset + 8 + length]
        crc = struct.unpack_from(">I", raw, offset + 8 + length)[0]
        if zlib.crc32(kind + payload) != crc:
            raise JevError("capture_failed", "Editor returned a PNG checksum mismatch.")
        if not seen_header and kind != b"IHDR":
            raise JevError(
                "capture_failed", "Editor returned PNG chunks without an initial header."
            )
        if kind == b"IHDR":
            if seen_header or length != 13:
                raise JevError("capture_failed", "Editor returned an invalid PNG header.")
            width, height, depth, color_type, compression, filtering, interlace = struct.unpack(
                ">IIBBBBB", payload
            )
            depths = {0: {1, 2, 4, 8, 16}, 2: {8, 16}, 3: {1, 2, 4, 8}, 4: {8, 16}, 6: {8, 16}}
            if (
                not 1 <= width <= 1024
                or not 1 <= height <= 1024
                or depth not in depths.get(color_type, set())
                or compression != 0
                or filtering != 0
                or interlace not in {0, 1}
            ):
                raise JevError("capture_failed", "Editor returned unsupported PNG header values.")
            seen_header = True
        elif kind == b"PLTE":
            if (
                seen_palette
                or seen_data
                or color_type in {0, 4}
                or not 1 <= length <= 768
                or length % 3
                or (color_type == 3 and length // 3 > 2**depth)
            ):
                raise JevError("capture_failed", "Editor returned an invalid PNG palette.")
            seen_palette = True
        elif kind == b"IDAT":
            if data_ended or (color_type == 3 and not seen_palette):
                raise JevError("capture_failed", "Editor returned invalid PNG data ordering.")
            seen_data = True
            data_bytes += length
        elif kind == b"IEND":
            if length != 0 or not seen_data or data_bytes == 0 or end != len(raw):
                raise JevError("capture_failed", "Editor returned an invalid PNG end marker.")
            return width, height
        elif kind[0] < 97:
            raise JevError("capture_failed", "Editor returned an unknown critical PNG chunk.")
        if seen_data and kind != b"IDAT":
            data_ended = True
        offset = end
    raise JevError("capture_failed", "Editor returned PNG data without an end marker.")


def capture_content(result: dict) -> CallToolResult:
    if not isinstance(result, dict):
        raise JevError("capture_failed", "Editor did not return capture metadata.")
    data = result.get("data")
    if not isinstance(data, str) or len(data) > 983040 or result.get("mime_type") != "image/png":
        raise JevError("capture_failed", "Editor did not return a bounded PNG capture.")
    try:
        raw = base64.b64decode(data, validate=True)
    except (ValueError, binascii.Error):
        raise JevError("capture_failed", "Editor returned invalid image encoding.") from None
    width, height = _png_dimensions(raw)
    if (
        type(result.get("width")) not in (int, float)
        or type(result.get("height")) not in (int, float)
        or width != result.get("width")
        or height != result.get("height")
    ):
        raise JevError("capture_failed", "Capture dimensions do not match the image.")
    metadata = {key: value for key, value in result.items() if key != "data"}
    metadata["byte_count"] = len(raw)
    structured = {"ok": True, "result": metadata}
    return CallToolResult(
        content=[
            TextContent(type="text", text=json.dumps(structured)),
            ImageContent(type="image", data=data, mimeType="image/png"),
        ],
        structuredContent=structured,
    )
