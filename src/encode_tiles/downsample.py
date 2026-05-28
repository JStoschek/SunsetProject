from __future__ import annotations


def downsample_2x2(
    pixels: list[tuple[int, int, int, int]],
) -> tuple[int, int, int, int]:
    """Alpha-weighted 2×2 downsample for the azimuth/elevation tile pyramid."""
    opaque = [(r, g, a) for r, g, _b, a in pixels if a > 0]
    if not opaque:
        return (0, 0, 0, 0)
    total_alpha = sum(a for _, _, a in opaque)
    r = round(sum(r * a for r, _, a in opaque) / total_alpha)
    g = round(sum(g * a for _, g, a in opaque) / total_alpha)
    return (r, g, 0, 255)
