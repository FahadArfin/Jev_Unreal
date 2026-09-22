import base64
import struct
import zlib

import pytest

from jev_unreal.capture import capture_content
from jev_unreal.errors import JevError

SIGNATURE = b"\x89PNG\r\n\x1a\n"


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


HEADER = chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0))
DATA = chunk(b"IDAT", zlib.compress(b"\x00\xff\x00\x00\xff"))
END = chunk(b"IEND", b"")
RAW_PNG = SIGNATURE + HEADER + DATA + END
PNG = base64.b64encode(RAW_PNG).decode()


def test_capture_emits_native_mcp_image_and_small_structured_metadata():
    result = capture_content(
        {
            "data": PNG,
            "width": 1,
            "height": 1,
            "mime_type": "image/png",
            "revision": "test",
        }
    )
    assert result.content[1].type == "image"
    assert result.content[1].data == PNG
    assert "data" not in result.structuredContent["result"]
    assert result.structuredContent["result"]["byte_count"] == len(base64.b64decode(PNG))


@pytest.mark.parametrize(
    "overrides",
    [
        {"data": "!"},
        {"data": "x" * 983041},
        {"data": None},
        {"data": base64.b64encode(b"not an image").decode()},
        {"width": 2},
        {"height": 2000},
        {"mime_type": "text/html"},
        {"width": True},
        {"height": True},
        {"width": "1"},
    ],
)
def test_capture_rejects_invalid_or_unbounded_content(overrides):
    with pytest.raises(JevError):
        capture_content(
            {"data": PNG, "width": 1, "height": 1, "mime_type": "image/png", **overrides}
        )


@pytest.mark.parametrize(
    "raw",
    [
        SIGNATURE + HEADER,
        RAW_PNG[:-1],
        RAW_PNG[:-12],
        RAW_PNG + b"trailing-data",
        SIGNATURE + HEADER + END,
        SIGNATURE + HEADER + chunk(b"IDAT", b"") + END,
        SIGNATURE + HEADER + DATA[:-1] + bytes([DATA[-1] ^ 1]) + END,
        SIGNATURE + HEADER + HEADER + DATA + END,
        SIGNATURE + DATA + HEADER + END,
        SIGNATURE + HEADER + struct.pack(">I", 2**32 - 1) + b"IDAT",
        SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 0, 6, 0, 0, 0)) + DATA + END,
        SIGNATURE + HEADER + DATA + chunk(b"tEXt", b"Comment\x00synthetic") + DATA + END,
        SIGNATURE + HEADER + chunk(b"FAIL", b"") + DATA + END,
    ],
)
def test_capture_rejects_truncated_or_structurally_invalid_png(raw):
    with pytest.raises(JevError):
        capture_content(
            {
                "data": base64.b64encode(raw).decode(),
                "width": 1,
                "height": 1,
                "mime_type": "image/png",
            }
        )


def test_capture_accepts_integral_json_number_dimensions_and_ancillary_chunks():
    raw = SIGNATURE + HEADER + chunk(b"tEXt", b"Comment\x00synthetic") + DATA + END
    result = capture_content(
        {
            "data": base64.b64encode(raw).decode(),
            "width": 1.0,
            "height": 1.0,
            "mime_type": "image/png",
        }
    )
    assert result.content[1].type == "image"
