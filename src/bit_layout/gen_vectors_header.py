#!/usr/bin/env python3
"""Transcode the shared bit-layout test vectors into a C++ header.

The JS test reads ``vectors.json`` directly; the C++ build has no JSON parser,
so this script (run at CMake configure/build time) emits the same vectors as
``constexpr`` arrays. Both languages therefore assert against the *identical*
committed file — the engine packer and frontend reader cannot drift.

Usage: gen_vectors_header.py <vectors.json> <output.h>
"""

from __future__ import annotations

import json
import sys
from pathlib import Path


def _cpp_double(x: float) -> str:
    # Preserve exact integers as e.g. 233.0 so the C++ literal is unambiguous.
    return repr(float(x))


def render(vectors: dict) -> str:
    out: list[str] = []
    w = out.append
    w("// AUTO-GENERATED from vectors.json by gen_vectors_header.py. DO NOT EDIT.")
    w("// Edit src/bit_layout/vectors.json instead; the build regenerates this.")
    w("#pragma once")
    w("#include <cstdint>")
    w("#include <cstddef>")
    w("")
    w("namespace bit_layout_vectors {")
    w("")
    w("struct ConfigVector {")
    w("    const char* name;")
    w("    double azimuth_min_deg, azimuth_max_deg, azimuth_step_deg;")
    w("    int bit_count, bytes_per_pixel, flag_bit_index;")
    w("};")
    w("struct AzimuthVector {")
    w("    const char* config; double azimuth;")
    w("    int bit_index, byte_offset; uint8_t mask;")
    w("};")
    w("struct RejectVector { const char* config; double azimuth; };")
    w("struct FlagVector { const char* config; int bit_index, byte_offset; uint8_t mask; };")
    w("")

    w("inline constexpr ConfigVector kConfigs[] = {")
    for c in vectors["configs"]:
        w("    {{ \"{name}\", {mn}, {mx}, {st}, {bc}, {bpp}, {flag} }},".format(
            name=c["name"],
            mn=_cpp_double(c["azimuth_min_deg"]),
            mx=_cpp_double(c["azimuth_max_deg"]),
            st=_cpp_double(c["azimuth_step_deg"]),
            bc=c["bit_count"], bpp=c["bytes_per_pixel"], flag=c["flag_bit_index"]))
    w("};")
    w("")

    w("inline constexpr AzimuthVector kAzimuthToBit[] = {")
    for a in vectors["azimuth_to_bit"]:
        w("    {{ \"{cfg}\", {az}, {idx}, {byte}, {mask} }},".format(
            cfg=a["config"], az=_cpp_double(a["azimuth"]),
            idx=a["bit_index"], byte=a["byte_offset"], mask=a["mask"]))
    w("};")
    w("")

    w("inline constexpr RejectVector kRejected[] = {")
    for r in vectors["rejected"]:
        w("    {{ \"{cfg}\", {az} }},".format(cfg=r["config"], az=_cpp_double(r["azimuth"])))
    w("};")
    w("")

    w("inline constexpr FlagVector kFlagBits[] = {")
    for f in vectors["flag_bit"]:
        w("    {{ \"{cfg}\", {idx}, {byte}, {mask} }},".format(
            cfg=f["config"], idx=f["bit_index"], byte=f["byte_offset"], mask=f["mask"]))
    w("};")
    w("")

    w("inline constexpr std::size_t kNumConfigs = sizeof(kConfigs) / sizeof(kConfigs[0]);")
    w("inline constexpr std::size_t kNumAzimuthToBit = sizeof(kAzimuthToBit) / sizeof(kAzimuthToBit[0]);")
    w("inline constexpr std::size_t kNumRejected = sizeof(kRejected) / sizeof(kRejected[0]);")
    w("inline constexpr std::size_t kNumFlagBits = sizeof(kFlagBits) / sizeof(kFlagBits[0]);")
    w("")
    w("}  // namespace bit_layout_vectors")
    w("")
    return "\n".join(out)


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        sys.stderr.write("usage: gen_vectors_header.py <vectors.json> <output.h>\n")
        return 2
    vectors = json.loads(Path(argv[1]).read_text())
    out_path = Path(argv[2])
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(render(vectors))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
