"""TileJSON 3.0 document builder."""

from __future__ import annotations


def build_tilejson(
    bounds: tuple[float, float, float, float],
    minzoom: int,
    maxzoom: int,
    tile_size: int = 256,
) -> dict:
    """Return a TileJSON 3.0 document as a plain dict.

    Args:
        bounds: (west, south, east, north) in decimal degrees.
        minzoom: Minimum zoom level (inclusive).
        maxzoom: Maximum zoom level (inclusive).
        tile_size: Tile pixel dimensions (default 256).
    """
    west, south, east, north = bounds
    return {
        "tilejson": "3.0.0",
        "tiles": ["{z}/{x}/{y}.png"],
        "scheme": "xyz",
        "bounds": [west, south, east, north],
        "minzoom": minzoom,
        "maxzoom": maxzoom,
        "tile_size": tile_size,
    }
