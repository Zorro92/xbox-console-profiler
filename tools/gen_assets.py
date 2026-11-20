"""Generate minimal solid/branded PNG icons required by the UWP Package.appxmanifest.

Uses only the Python standard library (struct + zlib) so no third-party images or PIL are needed.
"""

import os
import struct
import zlib


def png_chunk(chunk_type: bytes, data: bytes) -> bytes:
    crc = zlib.crc32(chunk_type + data) & 0xFFFFFFFF
    return struct.pack(">I", len(data)) + chunk_type + data + struct.pack(">I", crc)


def make_png(width: int, height: int, bg_rgb=(24, 30, 38), accent_rgb=(46, 160, 67)) -> bytes:
    """Create a simple RGBA PNG with a dark slate background and green Xbox-style border/bar."""
    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    border = max(2, min(width, height) // 16)
    bar_y0 = int(height * 0.42)
    bar_y1 = int(height * 0.58)
    bar_x0 = int(width * 0.18)
    bar_x1 = int(width * 0.82)

    rows = []
    for y in range(height):
        row = bytearray([0])  # filter type 0
        for x in range(width):
            is_border = x < border or x >= width - border or y < border or y >= height - border
            is_bar = bar_x0 <= x < bar_x1 and bar_y0 <= y < bar_y1
            r, g, b = accent_rgb if (is_border or is_bar) else bg_rgb
            row.extend((r, g, b, 255))
        rows.append(bytes(row))

    raw = b"".join(rows)
    idat = zlib.compress(raw, level=9)
    return sig + png_chunk(b"IHDR", ihdr) + png_chunk(b"IDAT", idat) + png_chunk(b"IEND", b"")


ASSETS = {
    "LockScreenLogo.scale-200.png": (48, 48),
    "SplashScreen.scale-200.png": (1240, 600),
    "Square150x150Logo.scale-200.png": (300, 300),
    "Square44x44Logo.scale-200.png": (88, 88),
    "Square44x44Logo.targetsize-24_altform-unplated.png": (24, 24),
    "StoreLogo.png": (50, 50),
    "Wide310x150Logo.scale-200.png": (620, 300),
}


def main() -> None:
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_dir = os.path.join(root, "src", "Assets")
    os.makedirs(out_dir, exist_ok=True)
    for name, (w, h) in ASSETS.items():
        path = os.path.join(out_dir, name)
        with open(path, "wb") as f:
            f.write(make_png(w, h))
        print(f"wrote {path} ({w}x{h})")


if __name__ == "__main__":
    main()
