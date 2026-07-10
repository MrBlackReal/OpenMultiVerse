#!/usr/bin/env python3
"""
fetch_catalogs.py — download the full NASA/ESA source catalogs the "Known
Universe" is built from, straight from their public query APIs, and write CSVs
whose headers drop into ./catalogtool (and build_known_universe.py) unchanged.

Three catalogs have live, queryable APIs and are fetched here:
  * exoplanets — NASA Exoplanet Archive TAP  (pscomppars: one row per planet)
  * gaia       — ESA Gaia Archive TAP        (nearby stars, become full bodies)
  * gaia-field — ESA Gaia Archive TAP        (~280k brightest stars, packed into
                 the far-field scenery StarBin the manifest's "star_catalog"
                 references — this is what takes the Known Universe to ~279k
                 bodies: ~16k curated + ~263k Gaia field stars)

Black holes have NO clean bulk feed, so they are NOT downloaded: the curated
assets/catalogs/black_holes.csv ships in the repo and the build folds it in
automatically.

Outputs (picked up automatically by build_known_universe.py, which prefers
*_full.csv over the shipped *_sample.csv, and references gaia_stars.bin):
  assets/catalogs/exoplanets_full.csv
  assets/catalogs/gaia_full.csv
  assets/catalogs/gaia_stars.bin        (far-field StarBin; gaia_field_full.csv cached beside it)

Usage:
  python3 tools/fetch_catalogs.py                 # all three, sane defaults
  python3 tools/fetch_catalogs.py --parallax-min 10   # only stars within ~100 pc
  python3 tools/fetch_catalogs.py --exo-limit 50 --gaia-limit 200 --skip-gaia-field  # quick test
  python3 tools/fetch_catalogs.py --build         # then rebuild the known universe

Stdlib only (urllib) — no third-party dependencies. Needs network access.
"""
import argparse
import os
import ssl
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

EXO_TAP = "https://exoplanetarchive.ipac.caltech.edu/TAP/sync"
GAIA_TAP = "https://gea.esac.esa.int/tap-server/tap/sync"

# The exact columns catalogtool's exoplanet importer reads (catalog.c).
EXO_COLS = ("hostname,pl_name,sy_dist,ra,dec,st_mass,st_rad,st_teff,"
            "pl_orbsmax,pl_orbper,pl_orbeccen,pl_orbincl,pl_orblper,"
            "pl_bmasse,pl_rade")


def http_post(url, params, timeout):
    """POST form-encoded params, return the decoded body text. Raises on HTTP
    error or an empty/again-later response."""
    data = urllib.parse.urlencode(params).encode("utf-8")
    req = urllib.request.Request(
        url, data=data,
        headers={"User-Agent": "OpenMultiVerse-fetch_catalogs/1.0"})
    ctx = ssl.create_default_context()
    try:
        with urllib.request.urlopen(req, timeout=timeout, context=ctx) as r:
            body = r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", "replace")[:500]
        sys.exit(f"error: {url} returned HTTP {e.code}\n{detail}")
    except urllib.error.URLError as e:
        sys.exit(f"error: could not reach {url}: {e.reason}")
    return body


def _top(limit):
    return f"top {int(limit)} " if limit and limit > 0 else ""


def looks_like_csv(text):
    """A CSV table has a header line with commas; error pages usually do not."""
    head = text.lstrip()[:200].lower()
    if head.startswith(("<", "error", "{")):
        return False
    first = text.strip().splitlines()[0] if text.strip() else ""
    return "," in first


def row_count(text):
    lines = [ln for ln in text.strip().splitlines() if ln.strip()]
    return max(0, len(lines) - 1)     # minus the header


def write_csv(text, out_path, label):
    if not looks_like_csv(text):
        sys.exit(f"error: {label} response was not CSV:\n{text[:500]}")
    n = row_count(text)
    if n == 0:
        sys.exit(f"error: {label} query returned no rows")
    with open(out_path, "w", encoding="utf-8", newline="") as f:
        f.write(text if text.endswith("\n") else text + "\n")
    print(f"[fetch] {label}: {n} rows -> {os.path.relpath(out_path, ROOT)}")


def fetch_exoplanets(out_path, limit, table, timeout):
    adql = (f"select {_top(limit)}{EXO_COLS} from {table} "
            f"where ra is not null and dec is not null and sy_dist is not null")
    text = http_post(EXO_TAP, {"query": adql, "format": "csv"}, timeout)
    write_csv(text, out_path, "exoplanets")


def gaia_query(adql, timeout, maxrec=None):
    params = {"REQUEST": "doQuery", "LANG": "ADQL", "FORMAT": "csv", "QUERY": adql}
    if maxrec:
        params["MAXREC"] = str(int(maxrec))
    return http_post(GAIA_TAP, params, timeout)


def fetch_gaia(out_path, limit, parallax_min, timeout):
    # teff_gspphot -> teff so the Gaia importer's "teff" column matches.
    adql = (f"select {_top(limit)}source_id, ra, dec, parallax, pmra, pmdec, "
            f"radial_velocity, teff_gspphot as teff "
            f"from gaiadr3.gaia_source "
            f"where parallax > {float(parallax_min)} and parallax is not null "
            f"order by parallax desc")
    write_csv(gaia_query(adql, timeout, maxrec=limit), out_path, "gaia")


def fetch_gaia_field(out_csv, out_bin, mag_limit, timeout, step=0.5):
    """Download the brightest Gaia stars — everything brighter than G=`mag_limit`
    with a positive parallax so a distance is computable — and pack them into the
    StarBin the manifest's "star_catalog" references: the frozen far-field
    scenery stars that make up the volumetric Milky Way disc. G<9.5 yields ~291k
    stars (~263k after de-dup with the curated nearby stars), matching the shipped
    Known Universe. Needs ./catalogtool built.

    Two deliberate choices dodge the sync endpoint's limits:
      * a magnitude CUTOFF, not `order by phot_g_mean_mag limit N` — sorting the
        full 1.8-billion-row gaia_source is far too expensive for sync, whereas a
        cutoff is a cheap indexed scan; and
      * PAGINATION into ~`step`-wide magnitude bins — a single sync request caps
        out around ~240k rows, so the brightness range is fetched in slices that
        each stay under the cap, then concatenated (bins are disjoint, no de-dup).
    """
    cols = ("source_id, ra, dec, parallax, pmra, pmdec, "
            "radial_velocity, teff_gspphot as teff")
    # Magnitude bin edges from a bright floor up to the cutoff.
    grid, m = [], 2.0
    while m < mag_limit:
        grid.append(round(m, 3)); m += step
    grid.append(float(mag_limit))
    bins = [f"phot_g_mean_mag < {grid[0]}"]
    bins += [f"phot_g_mean_mag >= {grid[i]} and phot_g_mean_mag < {grid[i+1]}"
             for i in range(len(grid) - 1)]

    total = 0
    with open(out_csv, "w", encoding="utf-8") as out:
        for i, cond in enumerate(bins):
            adql = (f"select {cols} from gaiadr3.gaia_source "
                    f"where parallax > 0 and {cond}")
            text = gaia_query(adql, timeout, maxrec=300000)
            if not looks_like_csv(text):
                sys.exit(f"error: gaia-field bin [{cond}] not CSV:\n{text[:400]}")
            lines = text.strip().splitlines()
            if i == 0:
                out.write(lines[0] + "\n")          # header once
            for ln in lines[1:]:
                out.write(ln + "\n")
            total += len(lines) - 1
            print(f"[fetch] gaia-field bin {i+1}/{len(bins)}: {len(lines)-1} rows")
    if total == 0:
        sys.exit("error: gaia-field query returned no rows")
    print(f"[fetch] gaia-field: {total} rows -> {os.path.relpath(out_csv, ROOT)}")

    tool = os.path.join(ROOT, "catalogtool")
    if not os.path.exists(tool):
        sys.exit("error: ./catalogtool not built — run `make catalogtool` first")
    subprocess.run([tool, "gaia-bin", out_csv, out_bin], check=True)
    print(f"[fetch] gaia-field: packed -> {os.path.relpath(out_bin, ROOT)}")


def main():
    ap = argparse.ArgumentParser(
        description="Download NASA/ESA source catalogs for the Known Universe.")
    ap.add_argument("--out-dir", default=os.path.join(ROOT, "assets", "catalogs"))
    ap.add_argument("--skip-exoplanets", action="store_true")
    ap.add_argument("--skip-gaia", action="store_true")
    ap.add_argument("--skip-gaia-field", action="store_true",
                    help="skip the far-field StarBin (gaia_stars.bin) download")
    ap.add_argument("--gaia-field-maglimit", type=float, default=9.5,
                    help="G-magnitude cutoff for the far-field scenery StarBin "
                         "(9.5 ~= 291k stars, ~263k after de-dup)")
    ap.add_argument("--exo-table", default="pscomppars",
                    help="NASA table: pscomppars (one row/planet, default) or ps")
    ap.add_argument("--exo-limit", type=int, default=0,
                    help="ADQL TOP cap on exoplanets (0 = no cap)")
    ap.add_argument("--gaia-limit", type=int, default=200000,
                    help="ADQL TOP cap on Gaia stars (0 = no cap)")
    ap.add_argument("--parallax-min", type=float, default=5.0,
                    help="minimum parallax in mas (5 ~= within 200 pc)")
    ap.add_argument("--timeout", type=float, default=300.0,
                    help="per-request timeout in seconds")
    ap.add_argument("--build", action="store_true",
                    help="run build_known_universe.py after downloading")
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)

    if not args.skip_exoplanets:
        fetch_exoplanets(os.path.join(args.out_dir, "exoplanets_full.csv"),
                         args.exo_limit, args.exo_table, args.timeout)
    if not args.skip_gaia:
        fetch_gaia(os.path.join(args.out_dir, "gaia_full.csv"),
                   args.gaia_limit, args.parallax_min, args.timeout)
    if not args.skip_gaia_field:
        fetch_gaia_field(os.path.join(args.out_dir, "gaia_field_full.csv"),
                         os.path.join(args.out_dir, "gaia_stars.bin"),
                         args.gaia_field_maglimit, args.timeout)

    print("[fetch] black holes: curated (assets/catalogs/black_holes.csv), "
          "not downloaded — no bulk feed exists.")

    if args.build:
        print("[fetch] rebuilding known universe ...")
        subprocess.run([sys.executable,
                        os.path.join(ROOT, "tools", "build_known_universe.py")],
                       check=True)


if __name__ == "__main__":
    main()
