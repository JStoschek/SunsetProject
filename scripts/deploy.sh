#!/usr/bin/env bash
#
# deploy.sh — push the Sunset Visibility Map frontend to S3 + CloudFront.
#
# Encapsulates the per-asset Content-Type / Cache-Control matrix and the gzip
# gotcha so you never have to remember them. See docs/deploy-runbook.md for the
# full story.
#
# Usage:
#   scripts/deploy.sh frontend      # index.html, app JS, vendor, geojson  (code — short TTL)
#   scripts/deploy.sh tiles         # overlay tiles/*.bin + tilejson       (immutable)
#   scripts/deploy.sh basemap       # basemap.pmtiles + fonts + sprites    (immutable)
#   scripts/deploy.sh all           # everything
#
# Flags:
#   --dry-run        show what would sync; make no changes, no invalidation
#   --no-invalidate  sync only; skip the CloudFront invalidation
#   --yes            don't prompt for confirmation
#
# Config (override via env or scripts/deploy.env):
#   BUCKET, DIST_ID, DOMAIN, FRONTEND_DIR
#
set -euo pipefail

# ---------------------------------------------------------------------------
# Config — defaults for this project; override via env or scripts/deploy.env
# ---------------------------------------------------------------------------
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
[ -f "$HERE/deploy.env" ] && source "$HERE/deploy.env"

BUCKET="${BUCKET:-sunsetscout-site}"
DIST_ID="${DIST_ID:-E1WIL7R541F6RP}"
DOMAIN="${DOMAIN:-sunsetscout.com}"
FRONTEND_DIR="${FRONTEND_DIR:-$REPO/frontend}"

# Cache-Control values
SHORT="public, max-age=300"                       # code/data that changes on deploy (not fingerprinted)
IMMUT="public, max-age=31536000, immutable"       # content-addressed-ish data (path-stable)

# ---------------------------------------------------------------------------
# What counts as "frontend code" — EDIT THESE when the frontend's file set
# changes (e.g. when you redo the frontend and add/rename/remove files).
# Anything NOT listed here is intentionally not deployed (dev-only artifacts
# like boxes.html, encode_azimuth.js, tests/, and the source-only us_land.geojson).
# ---------------------------------------------------------------------------
HTML_FILES=(index.html how-it-works.html)
APP_JS=(bit_layout.js colorizer.js fill_mask.js format_guard.js sunset_azimuth.js)
GEOJSON_FILES=(us_land_fill.geojson)
# vendor/ is synced wholesale (its *.js and *.css).
# SEO files: (localfile, s3key, content-type) triples, deployed with the frontend.
SEO_FILES=(
  "robots.txt|robots.txt|text/plain"
  "sitemap.xml|sitemap.xml|application/xml"
  "og-image.png|og-image.png|image/png"   # 1200x630 social preview; skipped until it exists
)

# ---------------------------------------------------------------------------
DRY_RUN=""; DO_INVALIDATE=1; ASSUME_YES=0
CMD=""
for arg in "$@"; do
  case "$arg" in
    frontend|tiles|basemap|all) CMD="$arg" ;;
    --dry-run)       DRY_RUN="--dryrun" ;;
    --no-invalidate) DO_INVALIDATE=0 ;;
    --yes|-y)        ASSUME_YES=1 ;;
    *) echo "Unknown argument: $arg" >&2; exit 2 ;;
  esac
done
[ -z "$CMD" ] && { echo "Usage: scripts/deploy.sh {frontend|tiles|basemap|all} [--dry-run] [--no-invalidate] [--yes]" >&2; exit 2; }

S3="s3://$BUCKET"
say() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
run() { echo "+ $*"; "$@"; }

preflight() {
  say "Preflight"
  local who; who="$(aws sts get-caller-identity --query Arn --output text)"
  echo "AWS identity : $who"
  echo "Bucket       : $S3"
  echo "Distribution : $DIST_ID  ($DOMAIN)"
  echo "Frontend dir : $FRONTEND_DIR"
  aws s3api head-bucket --bucket "$BUCKET" >/dev/null 2>&1 || { echo "ERROR: bucket $BUCKET not reachable" >&2; exit 1; }
  [ -d "$FRONTEND_DIR" ] || { echo "ERROR: frontend dir $FRONTEND_DIR missing" >&2; exit 1; }
  if [ -z "$DRY_RUN" ] && [ "$ASSUME_YES" -eq 0 ]; then
    read -r -p $'\nProceed with real upload to '"$S3"$'? [y/N] ' ans
    [[ "$ans" =~ ^[Yy]$ ]] || { echo "Aborted."; exit 0; }
  fi
}

cp1() { # cp1 <localfile> <s3key> <content-type> <cache-control>
  local f="$1" key="$2" ct="$3" cc="$4"
  [ -f "$f" ] || { echo "  skip (missing): $f"; return 0; }
  run aws s3 cp $DRY_RUN "$f" "$S3/$key" --content-type "$ct" --cache-control "$cc" --no-progress
}

deploy_frontend() {
  say "Frontend code (short TTL)"
  for f in "${HTML_FILES[@]}";    do cp1 "$FRONTEND_DIR/$f" "$f" "text/html"            "$SHORT"; done
  for f in "${APP_JS[@]}";        do cp1 "$FRONTEND_DIR/$f" "$f" "application/javascript" "$SHORT"; done
  for f in "${GEOJSON_FILES[@]}"; do cp1 "$FRONTEND_DIR/$f" "$f" "application/geo+json"   "$SHORT"; done
  for spec in "${SEO_FILES[@]}"; do
    IFS='|' read -r sf skey sct <<< "$spec"
    cp1 "$FRONTEND_DIR/$sf" "$skey" "$sct" "$SHORT"
  done
  # vendor: js + css (wholesale, --delete safe: vendor/ is its own prefix)
  run aws s3 sync $DRY_RUN "$FRONTEND_DIR/vendor/" "$S3/vendor/" --delete \
      --exclude "*" --include "*.js"  --content-type application/javascript --cache-control "$SHORT" --no-progress
  run aws s3 sync $DRY_RUN "$FRONTEND_DIR/vendor/" "$S3/vendor/" \
      --exclude "*" --include "*.css" --content-type text/css              --cache-control "$SHORT" --no-progress
  # tilejson travels with the frontend (it's small metadata, short TTL)
  cp1 "$FRONTEND_DIR/tiles/tilejson.json" "tiles/tilejson.json" "application/json" "$SHORT"
}

deploy_tiles() {
  say "Overlay tiles (immutable) — 85k .bin files, this takes a while"
  # .bin are ALREADY gzip-compressed on disk. Serve as octet-stream with NO
  # Content-Encoding — the client inflates them. Do NOT add --content-encoding.
  run aws s3 sync $DRY_RUN "$FRONTEND_DIR/tiles/" "$S3/tiles/" \
      --exclude "*" --include "*.bin" \
      --content-type application/octet-stream --cache-control "$IMMUT" --no-progress
  cp1 "$FRONTEND_DIR/tiles/tilejson.json" "tiles/tilejson.json" "application/json" "$SHORT"
}

deploy_basemap() {
  say "Basemap (immutable) — pmtiles is ~2 GB"
  cp1 "$FRONTEND_DIR/basemap/basemap.pmtiles" "basemap/basemap.pmtiles" "application/octet-stream" "$IMMUT"
  run aws s3 sync $DRY_RUN "$FRONTEND_DIR/basemap/fonts/" "$S3/basemap/fonts/" \
      --content-type application/x-protobuf --cache-control "$IMMUT" --no-progress
  run aws s3 sync $DRY_RUN "$FRONTEND_DIR/basemap/sprites/" "$S3/basemap/sprites/" \
      --exclude "*" --include "*.png"  --content-type image/png         --cache-control "$IMMUT" --no-progress
  run aws s3 sync $DRY_RUN "$FRONTEND_DIR/basemap/sprites/" "$S3/basemap/sprites/" \
      --exclude "*" --include "*.json" --content-type application/json  --cache-control "$IMMUT" --no-progress
}

invalidate() { # invalidate <path> [<path> ...]
  [ "$DO_INVALIDATE" -eq 1 ] || { echo "  (skipping invalidation: --no-invalidate)"; return 0; }
  [ -z "$DRY_RUN" ] || { echo "  (dry-run: would invalidate: $*)"; return 0; }
  say "CloudFront invalidation: $*"
  run aws cloudfront create-invalidation --distribution-id "$DIST_ID" --paths "$@" \
      --query 'Invalidation.{Id:Id,Status:Status}' --output json
}

preflight
case "$CMD" in
  frontend) deploy_frontend
            # invalidate only the code paths — leaves tile/pmtiles edge caches warm
            invalidate "/" "/vendor/*" "/tiles/tilejson.json" \
                       "/robots.txt" "/sitemap.xml" "/og-image.png" \
                       $(printf '/%s ' "${HTML_FILES[@]}") \
                       $(printf '/%s ' "${APP_JS[@]}") $(printf '/%s ' "${GEOJSON_FILES[@]}") ;;
  tiles)    deploy_tiles
            invalidate "/tiles/*" ;;            # 1 path; evicts changed tiles, keeps everything else warm
  basemap)  deploy_basemap
            invalidate "/basemap/*" ;;
  all)      deploy_frontend; deploy_tiles; deploy_basemap
            invalidate "/*" ;;                  # 1 wildcard path (cheap), evicts everything
esac

say "Done."
