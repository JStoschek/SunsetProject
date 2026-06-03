from __future__ import annotations


def downsample_2x2(
    pixels: list[tuple[int, int, int, int]],
) -> tuple[int, int, int, int]:
    """Union (hull) 2×2 downsample for the azimuth-range tile pyramid.

    Each opaque pixel encodes a visible-azimuth interval as bytes (R = min_az,
    G = max_az) with R <= G (see encode_pixel).  The never-visible-land sentinel
    encodes as (R=255, G=0) — an *empty* interval (R > G).  Transparent pixels
    (alpha 0) are ocean / no-data.

    A zoomed-out overview pixel should see an azimuth iff ANY of the sub-pixels
    it covers sees it, so the overview interval is the hull of the sub-pixels'
    non-empty intervals: R = min of the R's, G = max of the G's.

    The previous implementation alpha-*averaged* R and G across the block.  That
    is meaningless for interval endpoints: averaging a visible foreshore pixel
    (e.g. R=53, G=174) with an adjacent never-visible sentinel (255, 0) yields
    (154, 87) — an empty interval (R > G) that is opaque-dark at every date.
    Because the visible foreshore abuts the never-visible shadow band all along
    the shore, that painted a thin dark line tracing every coastline at low zoom,
    independent of the underlying (correct) full-resolution data.
    """
    opaque = [(r, g) for r, g, _b, a in pixels if a > 0]
    if not opaque:
        return (0, 0, 0, 0)
    # Non-empty visible intervals only (R <= G); sentinels (R > G) contribute
    # nothing visible and must not drag the hull.
    visible = [(r, g) for r, g in opaque if r <= g]
    if not visible:
        # Every covered sub-pixel is never-visible land → keep the sentinel.
        return (255, 0, 0, 255)
    r = min(r for r, _ in visible)
    g = max(g for _, g in visible)
    return (r, g, 0, 255)
