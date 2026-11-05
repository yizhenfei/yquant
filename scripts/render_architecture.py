#!/usr/bin/env python3
"""Convert the repository's SVG sources to hand-drawn SVG and rendered PNG."""
import argparse
from pathlib import Path
import random
import shutil
import struct
import zlib
import subprocess
import tempfile
import xml.etree.ElementTree as ET

NS = "http://www.w3.org/2000/svg"
ET.register_namespace("", NS)
ROOT = Path(__file__).resolve().parents[1]


def sketch(source, destination):
    tree = ET.parse(source)
    rng = random.Random(2026)
    for parent in list(tree.iter()):
        for element in list(parent):
            tag = element.tag.removeprefix(f"{{{NS}}}")
            if tag == "g" and "font-family" in element.attrib:
                element.set("font-family", "Chalkboard SE,Comic Sans MS,PingFang SC,Heiti SC,sans-serif")
            if tag == "rect" and element.get("stroke") and not float(element.get("rx", 0)):
                x, y, w, h = (float(element.get(k, 0)) for k in ("x", "y", "width", "height"))
                points = [(x, y), (x+w, y), (x+w, y+h), (x, y+h), (x, y)]
                path = ET.Element(f"{{{NS}}}path", {k:v for k,v in element.attrib.items()
                    if k not in ("x", "y", "width", "height", "rx")})
                path.set("d", curved(points, rng) + " Z")
                index = list(parent).index(element)
                parent.remove(element)
                parent.insert(index, path)
            elif tag == "polyline":
                points = [tuple(map(float, p.split(","))) for p in element.get("points").split()]
                element.tag = f"{{{NS}}}path"
                del element.attrib["points"]
                element.set("d", curved(points, rng))
    destination.parent.mkdir(parents=True, exist_ok=True)
    tree.write(destination, encoding="utf-8", xml_declaration=True)


def curved(points, rng):
    result = f"M {points[0][0]} {points[0][1]}"
    for (x, y), (a, b) in zip(points, points[1:]):
        dx, dy = a-x, b-y
        offset = rng.uniform(-1.4, 1.4)
        cx, cy = (offset, 0) if dx == 0 else (0, offset)
        result += f" C {x+dx/3+cx:.2f} {y+dy/3+cy:.2f}, {x+2*dx/3+cx:.2f} {y+2*dy/3+cy:.2f}, {a} {b}"
    return result


def crop_bottom(source, destination, height):
    # The temporary canvas only adds rows below the drawing. Keeping the first
    # rows preserves their PNG filters without decoding or modifying pixels.
    data = source.read_bytes()
    chunks, compressed, offset = [], bytearray(), 8
    while offset < len(data):
        size = struct.unpack(">I", data[offset:offset+4])[0]
        kind = data[offset+4:offset+8]
        payload = data[offset+8:offset+8+size]
        offset += size + 12
        if kind == b"IHDR":
            width, old_height, depth, color, compression, filtering, interlace = struct.unpack(">IIBBBBB", payload)
            if depth != 8 or color not in (2, 6) or interlace or height > old_height:
                raise SystemExit("Unsupported Quick Look PNG format")
            payload = struct.pack(">IIBBBBB", width, height, depth, color, compression, filtering, interlace)
        if kind == b"IDAT":
            compressed.extend(payload)
        elif kind != b"IEND":
            chunks.append((kind, payload))
    stride = 1 + width * (4 if color == 6 else 3)
    chunks.extend([(b"IDAT", zlib.compress(zlib.decompress(compressed)[:stride*height])), (b"IEND", b"")])
    output = bytearray(data[:8])
    for kind, payload in chunks:
        output.extend(struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind+payload)))
    destination.write_bytes(output)


def render(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    if shutil.which("rsvg-convert"):
        subprocess.run(["rsvg-convert", "-o", str(destination), str(source)], check=True)
    elif shutil.which("qlmanage"):
        # Quick Look crops non-square SVG thumbnails. Render a temporary square
        # canvas, then remove its unused bottom rows.
        with tempfile.TemporaryDirectory(prefix="yquant-diagram-") as temporary:
            tree = ET.parse(source)
            svg = tree.getroot()
            _, _, width, height = map(float, svg.get("viewBox").split())
            side = int(max(width, height))
            svg.set("width", str(side))
            svg.set("height", str(side))
            svg.set("viewBox", f"0 0 {side} {side}")
            square = Path(temporary) / source.name
            tree.write(square, encoding="utf-8", xml_declaration=True)
            subprocess.run(["qlmanage", "-t", "-s", str(side), "-o", temporary, str(square)],
                           check=True, stdout=subprocess.DEVNULL)
            thumbnail = Path(temporary) / (square.name + ".png")
            crop_bottom(thumbnail, destination, int(height))
    else:
        raise SystemExit("Install librsvg (rsvg-convert), or render on macOS with qlmanage.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plain", action="store_true", help="render source previews without changing presentation files")
    parser.add_argument("--output-dir", type=Path, help="PNG output directory; required for source previews")
    args = parser.parse_args()
    if args.plain and not args.output_dir:
        parser.error("--plain requires --output-dir")
    for name in ("monod-architecture", "single-host-deployment"):
        source = ROOT / "docs/images" / (name + ".svg")
        if not args.plain:
            target = source.parent / "hand-drawn" / source.name
            sketch(source, target)
            source = target
        destination = (args.output_dir or ROOT / "docs/images") / (name + ".png")
        render(source, destination)
        print(destination)


if __name__ == "__main__":
    main()
