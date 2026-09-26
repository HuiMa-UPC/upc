#!/usr/bin/env python3
"""Embed the reviewed candidate profile in a deterministic C++ build artifact.

Only the standard library is needed on the build host. The device does not need
Python, JSON files, PNG files, or a writable filesystem to use this profile.
Changing the profile inputs changes the embedded content hash; it does not make
the model-specific candidate a universally validated machine profile.
"""

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import struct


PROFILE_ID = "station_172_17_101_78_zminus20_candidate_v1"
DOOR_FILE = "cabin_door_candidate.json"
PLATE_FILE = "station_172_17_101_78_candidate.json"
MAX_JSON_BYTES = 64 * 1024
MAX_PNG_BYTES = 1024 * 1024
MAX_TOTAL_BYTES = 8 * 1024 * 1024
MAX_PIXELS = 4096 * 4096


def read_bounded(path, limit):
    if not path.is_file() or path.stat().st_size > limit:
        raise ValueError("Missing or oversized profile asset: " + str(path))
    data = path.read_bytes()
    if not data or len(data) > limit:
        raise ValueError("Empty or oversized profile asset: " + str(path))
    return data


def byte_array(name, data):
    lines = ["inline constexpr unsigned char " + name + "[] = {"]
    for start in range(0, len(data), 16):
        lines.append("    " + ", ".join("0x%02x" % b for b in data[start:start + 16]) + ",")
    lines.append("};")
    return "\n".join(lines)


def load_profile(source_dir):
    source_dir = source_dir.resolve(strict=True)
    door = read_bounded(source_dir / DOOR_FILE, MAX_JSON_BYTES)
    plate = read_bounded(source_dir / PLATE_FILE, MAX_JSON_BYTES)
    # Parse now to reject malformed assets at build time; full numeric/ROI and
    # image validation still runs through the detector's common runtime parser.
    if not isinstance(json.loads(door), dict):
        raise ValueError("Door profile must be a JSON object")
    plate_object = json.loads(plate)
    if not isinstance(plate_object, dict) or plate_object.get("version") != 1:
        raise ValueError("Plate profile must be version 1")
    markers = plate_object.get("markers")
    if not isinstance(markers, list) or not 1 <= len(markers) <= 32:
        raise ValueError("Expected 1..32 marker templates")
    templates = {}
    total = len(door) + len(plate)
    for marker in markers:
        name = marker.get("template") if isinstance(marker, dict) else None
        if not isinstance(name, str) or not name or "\\" in name or ":" in name:
            raise ValueError("Invalid template name")
        relative = PurePosixPath(name)
        if relative.is_absolute() or ".." in relative.parts or str(relative) != name:
            raise ValueError("Template must have a canonical relative path: " + name)
        path = (source_dir / name).resolve(strict=True)
        try:
            path.relative_to(source_dir)
        except ValueError:
            raise ValueError("Template escapes profile directory: " + name)
        if name in templates:
            continue
        data = read_bounded(path, MAX_PNG_BYTES)
        if (len(data) < 33 or data[:8] != b"\x89PNG\r\n\x1a\n" or
                data[8:12] != b"\x00\x00\x00\x0d" or data[12:16] != b"IHDR"):
            raise ValueError("Expected a PNG template: " + name)
        width, height = struct.unpack(">II", data[16:24])
        if not (12 <= width <= 8192 and 12 <= height <= 8192 and width * height <= MAX_PIXELS):
            raise ValueError("Invalid template dimensions: " + name)
        total += len(data)
        if total > MAX_TOTAL_BYTES:
            raise ValueError("Bundled profile exceeds size limit")
        templates[name] = data

    return door, plate, templates


def list_inputs(source_dir):
    source_dir = source_dir.resolve(strict=True)
    # Validate the complete asset set during CMake configuration as well as
    # generation. Return exact JSON references, not a directory wildcard.
    _, _, templates = load_profile(source_dir)
    paths = []
    for name in [DOOR_FILE, PLATE_FILE] + sorted(templates):
        path = source_dir / name
        for candidate in (path, path.resolve(strict=True)):
            value = candidate.as_posix()
            if any(character in value for character in (";", "\r", "\n")):
                raise ValueError("Profile dependency path cannot contain semicolons or newlines: " + value)
            if value not in paths:
                paths.append(value)
    return paths


def render(source_dir):
    door, plate, templates = load_profile(source_dir)

    # Hash length-delimited named original files. No timestamp or machine path
    # is emitted, so identical inputs produce byte-identical generated headers.
    assets = [(DOOR_FILE, door), (PLATE_FILE, plate)] + sorted(templates.items())
    digest = hashlib.sha256()
    for name, data in assets:
        encoded_name = name.encode("utf-8")
        digest.update(struct.pack(">Q", len(encoded_name)))
        digest.update(encoded_name)
        digest.update(struct.pack(">Q", len(data)))
        digest.update(data)
    lines = [
        "// Generated by embed_cabin_profile.py. Do not edit.",
        "#pragma once",
        "#include <cstddef>",
        "namespace upc::visual::bundled_profile {",
        "inline constexpr const char kId[] = " + json.dumps(PROFILE_ID) + ";",
        "inline constexpr const char kSha256[] = " + json.dumps(digest.hexdigest()) + ";",
        byte_array("kDoorJson", door),
        byte_array("kPlateJson", plate),
    ]
    for index, (_, data) in enumerate(sorted(templates.items())):
        lines.append(byte_array("kTemplate" + str(index), data))
    lines += [
        "struct Asset { const char* name; const unsigned char* data; std::size_t size; };",
        "inline constexpr Asset kTemplates[] = {",
    ]
    for index, (name, _) in enumerate(sorted(templates.items())):
        symbol = "kTemplate" + str(index)
        lines.append("    {" + json.dumps(name) + ", " + symbol + ", sizeof(" + symbol + ")},")
    lines += ["};", "} // namespace upc::visual::bundled_profile", ""]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", required=True, type=Path)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--output", type=Path)
    mode.add_argument("--list-inputs", action="store_true",
                      help="Validate assets and print absolute CMake dependency paths, one per line")
    args = parser.parse_args()
    if args.list_inputs:
        print("\n".join(list_inputs(args.source_dir)))
        return
    result = render(args.source_dir)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # Always update the timestamp when an input dependency changed, so Make/Ninja
    # does not repeat generation indefinitely after a harmless source touch.
    with args.output.open("w", encoding="utf-8", newline="\n") as output:
        output.write(result)


if __name__ == "__main__":
    main()
