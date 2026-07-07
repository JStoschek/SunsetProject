"""Run the single-box engine once per Processing Box from a manifest.

The engine stays single-box (ADR-0015); this driver is the documented N-run
loop turned into an artifact — read a `boxes.json`, run
`compute_azimuth_range` per box into one folder, then hand that folder to
`encode_tiles` for the deepest-interior merge.  It is the natural seam the
future coastline auto-generator will emit onto.
"""
