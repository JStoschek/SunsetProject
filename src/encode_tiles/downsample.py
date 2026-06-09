from __future__ import annotations


def downsample_2x2(pixels: list[bytes]) -> bytes:
    """Byte-wise OR of four child pixels (bitmask downsample, ADR-0013).

    Bit i in the result is 1 iff any child has bit i set.  This applies to
    both visibility bits and the land/data flag bit.  Four all-zero (transparent)
    children produce an all-zero (transparent) result.  The downsampler has no
    knowledge of the azimuth parameters — it is a pure byte operation.
    """
    n = len(pixels[0])
    result = bytearray(n)
    for pixel in pixels:
        for j in range(n):
            result[j] |= pixel[j]
    return bytes(result)
