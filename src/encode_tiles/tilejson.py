"""TileJSON 3.0 document builder (sunset-bitmask format, ADR-0013)."""

from __future__ import annotations


def build_tilejson(
    bounds: tuple[float, float, float, float],
    minzoom: int,
    maxzoom: int,
    tile_size: int = 512,
    format_version: int = 1,
    azimuth_min_deg: float = 0.0,
    azimuth_max_deg: float = 0.0,
    azimuth_step_deg: float = 1.0,
    bit_count: int = 0,
    bytes_per_pixel: int = 0,
) -> dict:
    """Return a TileJSON 3.0 document for the sunset-bitmask tileset.

    All azimuth parameters must come from the GeoTIFF metadata tags, not
    re-typed — they flow through encode_source_to_base_raster unchanged.
    """
    west, south, east, north = bounds
    return {
        "tilejson": "3.0.0",
        "tiles": ["{z}/{x}/{y}.bin"],
        "format": "sunset-bitmask",
        "format_version": format_version,
        "scheme": "xyz",
        "bounds": [west, south, east, north],
        "minzoom": minzoom,
        "maxzoom": maxzoom,
        "tile_size": tile_size,
        "azimuth_min_deg": azimuth_min_deg,
        "azimuth_max_deg": azimuth_max_deg,
        "azimuth_step_deg": azimuth_step_deg,
        "bit_count": bit_count,
        "bytes_per_pixel": bytes_per_pixel,
    }
