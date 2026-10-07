"""Reproduce the lossless decoder fixtures using only Python's standard library.

ICC bytes are a checked-in lcms sRGB profile (see README), not generated here.
"""

import pathlib
import struct
import zlib


ROOT = pathlib.Path(__file__).resolve().parent


def chunk(name, data):
    return (
            struct.pack(">I", len(data))
            + name
            + data
            + struct.pack(">I", zlib.crc32(name + data))
    )


def png(name, width, height, channels, samples, depth=8, metadata=b""):
    color = {1: 0, 2: 4, 3: 2, 4: 6}[channels]

    packed = (
        bytes(samples)
        if depth == 8
        else struct.pack(">" + "H" * len(samples), *samples)
    )

    stride = width * channels * (depth // 8)

    rows = b"".join(
        b"\0" + packed[y * stride : (y + 1) * stride]
        for y in range(height)
    )

    data = (
            b"\x89PNG\r\n\x1a\n"
            + chunk(
        b"IHDR",
        struct.pack(
            ">IIBBBBB",
            width,
            height,
            depth,
            color,
            0,
            0,
            0,
        ),
    )
    )

    data += (
            metadata
            + chunk(b"IDAT", zlib.compress(rows))
            + chunk(b"IEND", b"")
    )

    (ROOT / name).write_bytes(data)


# ---------------------------------------------------------------------------
# PNG orientation fixtures
# ---------------------------------------------------------------------------

shades = [20, 50, 80, 110, 140, 170]

for orientation in range(1, 9):
    # Little-endian TIFF with one SHORT orientation tag, no next IFD.
    exif = b"II" + struct.pack("<HIH", 42, 8, 1)
    exif += struct.pack(
        "<HHIHHI",
        0x112,
        3,
        1,
        orientation,
        0,
        0,
    )

    png(
        f"orientation-{orientation}.png",
        3,
        2,
        1,
        shades,
        metadata=chunk(b"eXIf", exif),
    )


# ---------------------------------------------------------------------------
# Basic PNG fixtures
# ---------------------------------------------------------------------------

rgb = [
    255, 0, 0,
    0, 255, 0,
    0, 0, 255,
    17, 33, 201,
    255, 255, 255,
    0, 0, 0,
]

png(
    "rgb8.png",
    3,
    2,
    3,
    rgb,
)

png(
    "gray16.png",
    3,
    2,
    1,
    [0, 257, 32768, 65535, 25600, 40000],
    depth=16,
)

png(
    "rgb16.png",
    3,
    2,
    3,
    [sample * 257 for sample in rgb],
    depth=16,
)

png(
    "rgba8.png",
    3,
    1,
    4,
    [
        255, 0, 0, 255,
        0, 255, 0, 128,
        0, 0, 255, 0,
    ],
)

png(
    "rgba16.png",
    3,
    1,
    4,
    [
        65535, 0, 0, 32768,
        0, 65535, 0, 65535,
        0, 0, 65535, 0,
    ],
    depth=16,
)

png(
    "gray-alpha16.png",
    2,
    1,
    2,
    [
        65535, 32768,
        0, 65535,
    ],
    depth=16,
)


# ---------------------------------------------------------------------------
# ICC profile fixtures
# ---------------------------------------------------------------------------

png(
    "invalid-icc.png",
    3,
    2,
    3,
    rgb,
    metadata=chunk(
        b"iCCP",
        b"bad\0\0" + zlib.compress(b"not an ICC profile"),
        ),
)

profile = ROOT / "srgb.icc"

if profile.exists():
    png(
        "srgb-icc.png",
        3,
        2,
        3,
        rgb,
        metadata=chunk(
            b"iCCP",
            b"sRGB\0\0" + zlib.compress(profile.read_bytes()),
            ),
    )

profile = ROOT / "linear-rgb.icc"

if profile.exists():
    png(
        "linear-icc.png",
        3,
        2,
        3,
        rgb,
        metadata=chunk(
            b"iCCP",
            b"linearRGB\0\0" + zlib.compress(profile.read_bytes()),
            ),
    )


# ---------------------------------------------------------------------------
# RGB image fixtures
# ---------------------------------------------------------------------------

for name, width, height in [
    ("bridge", 96, 73),
    ("pen", 64, 64),
]:
    raw = (
            ROOT.parent
            / "pdq"
            / f"{name}-{width}x{height}.rgb"
    ).read_bytes()

    png(
        f"{name}.png",
        width,
        height,
        3,
        raw,
    )


# ---------------------------------------------------------------------------
# Invalid / corrupted PNG fixtures
# ---------------------------------------------------------------------------

# Correct CRC, absurd dimensions, no valid pixels:
# header limit must reject before decoding.
(ROOT / "oversized.png").write_bytes(
    b"\x89PNG\r\n\x1a\n"
    + chunk(
        b"IHDR",
        struct.pack(
            ">IIBBBBB",
            100000,
            100000,
            8,
            2,
            0,
            0,
            0,
        ),
    )
    + chunk(b"IDAT", zlib.compress(b""))
    + chunk(b"IEND", b"")
)


# Truncated PNG.
valid = (ROOT / "bridge.png").read_bytes()

(ROOT / "truncated.png").write_bytes(
    valid[: len(valid) // 2]
)


# Invalid IDAT CRC in otherwise complete data.
bad = bytearray(valid)

pos = bad.index(b"IDAT")
size = struct.unpack(
    ">I",
    bad[pos - 4 : pos],
)[0]

bad[pos + 4 + size] ^= 1

(ROOT / "bad-crc.png").write_bytes(bad)


# ---------------------------------------------------------------------------
# TIFF fixture
# ---------------------------------------------------------------------------

# Minimal baseline TIFF, RGB8 with associated (premultiplied) alpha.
entries = [
    (256, 4, 1, 3),
    (257, 4, 1, 1),
    (258, 3, 4, 146),
    (259, 3, 1, 1),
    (262, 3, 1, 2),
    (273, 4, 1, 154),
    (277, 3, 1, 4),
    (278, 4, 1, 1),
    (279, 4, 1, 12),
    (284, 3, 1, 1),
    (338, 3, 1, 1),
]

data = (
        b"II"
        + struct.pack("<HIH", 42, 8, len(entries))
)

data += b"".join(
    struct.pack("<HHII", *entry)
    for entry in entries
)

data += struct.pack(
    "<I4H",
    0,
    8,
    8,
    8,
    8,
)

data += bytes([
    128, 0, 0, 128,
    0, 255, 0, 255,
    0, 0, 0, 0,
])

(ROOT / "associated-alpha.tiff").write_bytes(data)


# Linear HDR float TIFF, with straight alpha in [0, 1].
def hdr_tiff(name, values):
    bands = 4
    width = len(values) // bands
    tags = [
        (256, 4, 1, width), (257, 4, 1, 1), (258, 3, bands, 158),
        (259, 3, 1, 1), (262, 3, 1, 2), (273, 4, 1, 174),
        (277, 3, 1, bands), (278, 4, 1, 1), (279, 4, 1, len(values) * 4),
        (284, 3, 1, 1), (338, 3, 1, 2), (339, 3, bands, 166),
    ]
    header = b"II" + struct.pack("<HIH", 42, 8, len(tags))
    header += b"".join(struct.pack("<HHII", *tag) for tag in tags)
    header += struct.pack("<I8H", 0, *([32] * bands), *([3] * bands))
    assert len(header) == 174
    (ROOT / name).write_bytes(header + struct.pack("<" + "f" * len(values), *values))


hdr_tiff("hdr-float.tiff", [0, 0, 0, 1, 1, 1, 1, 1, 4, 4, 4, 1, 1, 0, 0, 0.5])
hdr_tiff("hdr-nan.tiff", [float("nan"), 0, 0, 1])
hdr_tiff("hdr-bad-alpha.tiff", [1, 1, 1, 2])

# RGBE stores (mantissa + 0.5) * 2 ** (exponent - 136), except exponent zero = black.
(ROOT / "hdr-radiance.hdr").write_bytes(
    b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 3\n"
    + bytes([0, 0, 0, 0, 128, 128, 128, 129, 128, 128, 128, 131])
)
