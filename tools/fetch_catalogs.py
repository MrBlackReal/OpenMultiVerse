#!/usr/bin/env python3
"""
fetch_catalogs.py — download the NASA/ESA source catalogs the "Known Universe"
is built from, plus optical backfill catalogs, and write CSVs whose headers
drop into ./catalogtool and build_known_universe.py unchanged.

Source feeds:
  * exoplanets  — NASA Exoplanet Archive TAP (PSCompPars / PS)
  * gaia        — ESA Gaia DR3 TAP (nearby stars, become full bodies)
  * gaia-field  — ESA Gaia DR3 TAP (bright far-field scenery stars)
  * tycho-2     — CDS/VizieR Tycho-2 optical catalog (fainter visual backfill)
  * hipparcos   — CDS/VizieR Hipparcos main catalog (bright-star backfill)

The supplemental catalogs are normalized to the same 8-column schema used by
`catalogtool gaia-bin`. Gaia stars remain authoritative when catalogs overlap:
before packing, supplemental stars are cross-matched against the Gaia field
and against each other by position and duplicates are discarded.

Important distance note:
  Tycho-2 has no per-star trigonometric parallax for the full catalog, so its
  supplemental stars use --supplemental-parallax as a scenery-only fallback
  (default 0.2 mas = 5 kpc). Their sky positions and optical magnitudes are
  catalog-derived, but that fallback distance is NOT a measured distance.
  Hipparcos stars use their catalog parallax when it is positive.

Outputs:
  assets/catalogs/exoplanets_full.csv
  assets/catalogs/gaia_full.csv
  assets/catalogs/gaia_field_full.csv
  assets/catalogs/supplemental_stars_full.csv
  assets/catalogs/gaia_field_merged.csv
  assets/catalogs/gaia_stars.bin

Usage:
  python3 tools/fetch_catalogs.py
  python3 tools/fetch_catalogs.py --gaia-field-maglimit 11
  python3 tools/fetch_catalogs.py --tycho-mag-min 9.5 --tycho-mag-max 12
  python3 tools/fetch_catalogs.py --skip-supplemental
  python3 tools/fetch_catalogs.py --build

Stdlib only (urllib) — no third-party dependencies. Needs network access.
"""

import argparse
import csv
import math
import os
import ssl
import subprocess
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

EXO_TAP = "https://exoplanetarchive.ipac.caltech.edu/TAP/sync"
GAIA_TAP = "https://gea.esac.esa.int/tap-server/tap/sync"
VIZIER_TSV = "https://vizier.cds.unistra.fr/viz-bin/asu-tsv"

CATALOG_EPOCH = 2016.0  # Gaia DR3 reference epoch.
# Cross-catalog duplicate matching (merge_supplemental_and_pack). Positions are
# all propagated to the Gaia epoch first, so separations are real astrometric
# agreement. A pair within DUP_TIGHT is the same star; out to DUP_RADIUS it is
# only when the magnitudes agree (the bands differ — Gaia G, Tycho VT,
# Hipparcos V — hence the loose tolerance). Measured parallaxes must also
# agree, which keeps a genuine foreground/background pair apart.
DUP_RADIUS_ARCSEC = 3.0
DUP_TIGHT_ARCSEC = 1.0
DUP_MAG_TOL = 1.0
DUP_PLX_FRAC = 0.25

# The exact columns catalogtool's exoplanet importer reads (catalog.c).
EXO_COLS = (
    "hostname,pl_name,sy_dist,ra,dec,st_mass,st_rad,st_teff,"
    "pl_orbsmax,pl_orbper,pl_orbeccen,pl_orbincl,pl_orblper,"
    "pl_bmasse,pl_rade"
)

# The exact schema used by catalogtool's Gaia/StarBin importer. "mag" is the
# observed magnitude (Gaia G, Tycho VT, Hipparcos V): the importer turns it
# into an absolute magnitude with the row's distance, so stars with a
# placeholder distance (Tycho) still show at their real brightness. It is
# optional — CSVs written before it existed still merge and pack.
#   placeholder_dist  1 = the parallax is a scenery placeholder (Tycho-2), not a
#                     measurement: dedupe by sky position alone.
#   xref_hip          Tycho-2's own Hipparcos cross-identification, for exact
#                     ID-based dedupe against the Hipparcos rows.
STAR_COLS = (
    "source_id,ra,dec,parallax,pmra,pmdec,radial_velocity,teff,mag,"
    "placeholder_dist,xref_hip"
)
STAR_REQUIRED = set(STAR_COLS.split(",")) - {"mag", "placeholder_dist", "xref_hip"}


def http_post(url, params, timeout):
    """POST form-encoded params, return decoded body text."""
    data = urllib.parse.urlencode(params).encode("utf-8")
    req = urllib.request.Request(
        url, data=data, headers={"User-Agent": "OpenMultiVerse-fetch_catalogs/1.0"}
    )
    ctx = ssl.create_default_context()
    try:
        with urllib.request.urlopen(req, timeout=timeout, context=ctx) as r:
            return r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", "replace")[:500]
        sys.exit(f"error: {url} returned HTTP {e.code}\n{detail}")
    except urllib.error.URLError as e:
        sys.exit(f"error: could not reach {url}: {e.reason}")


def http_get(url, params, timeout):
    """GET a VizieR/ASU query, return decoded body text."""
    query = urllib.parse.urlencode(params)
    req = urllib.request.Request(
        f"{url}?{query}", headers={"User-Agent": "OpenMultiVerse-fetch_catalogs/1.0"}
    )
    ctx = ssl.create_default_context()
    try:
        with urllib.request.urlopen(req, timeout=timeout, context=ctx) as r:
            return r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", "replace")[:500]
        sys.exit(f"error: {url} returned HTTP {e.code}\n{detail}")
    except urllib.error.URLError as e:
        sys.exit(f"error: could not reach {url}: {e.reason}")


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
    return max(0, len(lines) - 1)


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
    adql = (
        f"select {_top(limit)}{EXO_COLS} from {table} "
        f"where ra is not null and dec is not null and sy_dist is not null"
    )
    text = http_post(EXO_TAP, {"query": adql, "format": "csv"}, timeout)
    write_csv(text, out_path, "exoplanets")


def gaia_query(adql, timeout, maxrec=None):
    params = {"REQUEST": "doQuery", "LANG": "ADQL", "FORMAT": "csv", "QUERY": adql}
    if maxrec:
        params["MAXREC"] = str(int(maxrec))
    return http_post(GAIA_TAP, params, timeout)


def fetch_gaia(out_path, limit, parallax_min, timeout):
    # teff_gspphot -> teff so the Gaia importer's "teff" column matches.
    adql = (
        f"select {_top(limit)}source_id, ra, dec, parallax, pmra, pmdec, "
        f"radial_velocity, teff_gspphot as teff "
        f"from gaiadr3.gaia_source "
        f"where parallax > {float(parallax_min)} and parallax is not null "
        f"order by parallax desc"
    )
    write_csv(gaia_query(adql, timeout, maxrec=limit), out_path, "gaia")


def fetch_gaia_field(out_csv, out_bin, mag_limit, timeout, step=0.5):
    """Download bright Gaia stars in magnitude bins and pack the StarBin."""
    cols = (
        "source_id, ra, dec, parallax, pmra, pmdec, "
        "radial_velocity, teff_gspphot as teff, phot_g_mean_mag as mag"
    )
    grid, m = [], 2.0
    while m < mag_limit:
        grid.append(round(m, 3))
        m += step
    grid.append(float(mag_limit))
    bins = [f"phot_g_mean_mag < {grid[0]}"]
    bins += [
        f"phot_g_mean_mag >= {grid[i]} and phot_g_mean_mag < {grid[i + 1]}"
        for i in range(len(grid) - 1)
    ]

    total = 0
    with open(out_csv, "w", encoding="utf-8") as out:
        for i, cond in enumerate(bins):
            adql = (
                f"select {cols} from gaiadr3.gaia_source where parallax > 0 and {cond}"
            )
            text = gaia_query(adql, timeout, maxrec=300000)
            if not looks_like_csv(text):
                sys.exit(f"error: gaia-field bin [{cond}] not CSV:\n{text[:400]}")
            lines = text.strip().splitlines()
            if i == 0:
                out.write(lines[0] + "\n")
            for ln in lines[1:]:
                out.write(ln + "\n")
            total += len(lines) - 1
            print(f"[fetch] gaia-field bin {i + 1}/{len(bins)}: {len(lines) - 1} rows")
    if total == 0:
        sys.exit("error: gaia-field query returned no rows")
    print(f"[fetch] gaia-field: {total} rows -> {os.path.relpath(out_csv, ROOT)}")

    tool = os.path.join(ROOT, "catalogtool")
    if not os.path.exists(tool):
        sys.exit("error: ./catalogtool not built — run `make catalogtool` first")
    subprocess.run([tool, "gaia-bin", out_csv, out_bin], check=True)
    print(f"[fetch] gaia-field: packed -> {os.path.relpath(out_bin, ROOT)}")


def vizier_query(source, out_cols, constraints, timeout, out_max=250000):
    """Query VizieR through its ASU TSV endpoint."""
    params = {
        "-source": source,
        "-out": ",".join(out_cols),
        "-mime": "tsv",
        "-out.max": str(int(out_max)),
        "-out.meta": "-huD",
    }
    params.update(constraints)
    return http_get(VIZIER_TSV, params, timeout)


def parse_vizier_tsv(text, wanted_cols):
    """Parse VizieR TSV while tolerating metadata/comment lines."""
    raw_lines = [ln.rstrip("\r") for ln in text.splitlines() if ln.strip()]
    header_index = None
    header = None
    wanted_lower = {c.lower() for c in wanted_cols}

    for i, line in enumerate(raw_lines):
        if line.startswith("#") or "\t" not in line:
            continue
        cells = [c.strip() for c in line.split("\t")]
        lower = {c.lower() for c in cells}
        if len(lower & wanted_lower) >= max(1, len(wanted_lower) // 2):
            header_index = i
            header = cells
            break

    if header_index is None:
        sys.exit(f"error: VizieR response missing expected TSV header:\n{text[:700]}")

    rows = []
    for line in raw_lines[header_index + 1 :]:
        if line.startswith("#"):
            continue
        cells = line.split("\t")
        if len(cells) != len(header):
            continue
        # VizieR can emit a separator line in some modes.
        if not any(c.strip().replace("-", "") for c in cells):
            continue
        rows.append(dict(zip(header, (c.strip() for c in cells))))
    return rows


def ffloat(value):
    if value is None:
        return None
    s = str(value).strip()
    if not s or s in {"~", "-", "--", "null", "NULL"}:
        return None
    try:
        return float(s)
    except ValueError:
        return None


def fint(value):
    x = ffloat(value)
    return None if x is None else int(round(x))


def propagate_radec(
    ra_deg, dec_deg, pmra_masyr, pmdec_masyr, from_epoch, to_epoch=CATALOG_EPOCH
):
    """Propagate small proper-motion shifts from catalog epoch to Gaia epoch."""
    if pmra_masyr is None:
        pmra_masyr = 0.0
    if pmdec_masyr is None:
        pmdec_masyr = 0.0
    dt = to_epoch - from_epoch
    dec_rad = math.radians(dec_deg)
    cos_dec = max(1e-8, abs(math.cos(dec_rad)))
    ra = ra_deg + (pmra_masyr * dt) / (3_600_000.0 * cos_dec)
    dec = dec_deg + (pmdec_masyr * dt) / 3_600_000.0
    ra %= 360.0
    return ra, max(-90.0, min(90.0, dec))


def tycho_source_id(tyc1, tyc2, tyc3):
    # Stay within positive signed 64-bit space and avoid Gaia source_id overlap.
    number = int(tyc1) * 10_000_000 + int(tyc2) * 10 + int(tyc3)
    return (0x54 << 48) | number


def hip_source_id(hip):
    return (0x48 << 48) | int(hip)


def teff_from_bv(bv):
    """Ballesteros-style color-temperature estimate; only used for Hipparcos."""
    if bv is None or bv <= -0.4 or bv >= 2.0:
        return None
    return 4600.0 * (1.0 / (0.92 * bv + 1.7) + 1.0 / (0.92 * bv + 0.62))


def _write_star_rows(out_path, rows):
    with open(out_path, "w", encoding="utf-8", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(STAR_COLS.split(","))
        writer.writerows(rows)
    print(
        f"[fetch] supplemental stars: {len(rows)} rows -> {os.path.relpath(out_path, ROOT)}"
    )


def fetch_tycho2(rows_out, mag_min, mag_max, timeout, step=0.5):
    """Fetch Tycho-2 in magnitude slices; propagate positions from J2000 to J2016."""
    wanted = [
        "RAmdeg",
        "DEmdeg",
        "pmRA",
        "pmDE",
        "VTmag",
        "BTmag",
        "TYC1",
        "TYC2",
        "TYC3",
        "HIP",
    ]
    rows_by_id = {}
    mag = mag_min
    bins = []
    while mag < mag_max:
        hi = min(mag + step, mag_max)
        bins.append((mag, hi))
        mag = hi

    for i, (lo, hi) in enumerate(bins, 1):
        # Inclusive range is intentional; source-ID de-dup removes boundary repeats.
        text = vizier_query(
            "I/259/tyc2",
            wanted,
            {"VTmag": f"{lo}..{hi}"},
            timeout,
            out_max=250000,
        )
        rows = parse_vizier_tsv(text, wanted)
        for r in rows:
            t1, t2, t3 = fint(r.get("TYC1")), fint(r.get("TYC2")), fint(r.get("TYC3"))
            ra0, dec0 = ffloat(r.get("RAmdeg")), ffloat(r.get("DEmdeg"))
            mag_v = ffloat(r.get("VTmag"))
            if None in (t1, t2, t3, ra0, dec0, mag_v):
                continue
            if not (mag_min <= mag_v <= mag_max):
                continue
            pmra, pmdec = ffloat(r.get("pmRA")), ffloat(r.get("pmDE"))
            ra, dec = propagate_radec(ra0, dec0, pmra, pmdec, 2000.0)
            sid = tycho_source_id(t1, t2, t3)
            # Colour: Johnson B-V ~= 0.85 (BT - VT) (ESA 1997, Tycho vol. 1).
            bt = ffloat(r.get("BTmag"))
            teff = teff_from_bv(0.85 * (bt - mag_v)) if bt is not None else None
            rows_by_id[sid] = (
                str(sid),
                f"{ra:.8f}",
                f"{dec:.8f}",
                "",
                "" if pmra is None else f"{pmra:.4f}",
                "" if pmdec is None else f"{pmdec:.4f}",
                "",
                "" if teff is None else f"{teff:.1f}",
                f"{mag_v:.3f}",
                "1",  # placeholder distance
                "" if fint(r.get("HIP")) is None else str(fint(r.get("HIP"))),
            )
        print(f"[fetch] tycho-2 bin {i}/{len(bins)}: {len(rows)} rows")

    rows_out.extend(rows_by_id.values())


def fetch_hipparcos(rows_out, max_mag, timeout):
    """Fetch bright Hipparcos stars with positive parallaxes and propagate to J2016."""
    wanted = ["HIP", "RAICRS", "DEICRS", "Plx", "pmRA", "pmDE", "Vmag", "B-V"]
    text = vizier_query(
        "I/239/hip_main",
        wanted,
        {"Vmag": f"<={max_mag}", "Plx": ">0"},
        timeout,
        out_max=250000,
    )
    rows = parse_vizier_tsv(text, wanted)
    seen = set()
    for r in rows:
        hip = fint(r.get("HIP"))
        ra0, dec0 = ffloat(r.get("RAICRS")), ffloat(r.get("DEICRS"))
        plx = ffloat(r.get("Plx"))
        mag = ffloat(r.get("Vmag"))
        if None in (hip, ra0, dec0, plx, mag) or hip in seen:
            continue
        if plx <= 0 or mag > max_mag:
            continue
        seen.add(hip)
        pmra, pmdec = ffloat(r.get("pmRA")), ffloat(r.get("pmDE"))
        ra, dec = propagate_radec(ra0, dec0, pmra, pmdec, 1991.25)
        teff = teff_from_bv(ffloat(r.get("B-V")))
        sid = hip_source_id(hip)
        rows_out.append(
            (
                str(sid),
                f"{ra:.8f}",
                f"{dec:.8f}",
                f"{plx:.6f}",
                "" if pmra is None else f"{pmra:.4f}",
                "" if pmdec is None else f"{pmdec:.4f}",
                "",
                "" if teff is None else f"{teff:.1f}",
                f"{mag:.3f}",
                "",
                "",
            )
        )
    print(f"[fetch] hipparcos: {len(rows)} rows returned, {len(seen)} usable")


def fetch_supplemental(out_path, tycho_min, tycho_max, hip_max, timeout):
    rows = []
    fetch_tycho2(rows, tycho_min, tycho_max, timeout)
    before_hip = len(rows)
    fetch_hipparcos(rows, hip_max, timeout)
    print(f"[fetch] supplemental: {len(rows) - before_hip} Hipparcos rows appended")
    _write_star_rows(out_path, rows)


class SkyIndex:
    """Cross-catalog duplicate finder on the celestial sphere.

    Cells are cubes in unit-vector space DUP_RADIUS wide. (An RA/Dec grid —
    the first version — breaks near the poles: a few arcseconds there span
    many degrees of RA, so the neighbour search missed real duplicates.)
    Only entries from a DIFFERENT catalogue can match: a catalogue never lists
    one star twice, but it does list close binaries, and both must survive."""

    def __init__(self, radius_arcsec):
        self.r = math.radians(radius_arcsec / 3600.0)
        self.cells = {}

    @staticmethod
    def unit(ra, dec):
        ra, dec = math.radians(ra), math.radians(dec)
        c = math.cos(dec)
        return (c * math.cos(ra), c * math.sin(ra), math.sin(dec))

    def _key(self, u):
        return (
            int(math.floor(u[0] / self.r)),
            int(math.floor(u[1] / self.r)),
            int(math.floor(u[2] / self.r)),
        )

    def add(self, ra, dec, mag, plx, placeholder, src):
        u = self.unit(ra, dec)
        self.cells.setdefault(self._key(u), []).append((u, mag, plx, placeholder, src))

    def matches(self, ra, dec, mag, plx, placeholder, src):
        u = self.unit(ra, dec)
        kx, ky, kz = self._key(u)
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    for v, m2, p2, ph2, s2 in self.cells.get(
                        (kx + dx, ky + dy, kz + dz), ()
                    ):
                        if s2 == src:
                            continue
                        chord = math.dist(u, v)
                        sep = math.degrees(chord) * 3600.0  # small-angle arcsec
                        if sep > DUP_RADIUS_ARCSEC:
                            continue
                        if sep > DUP_TIGHT_ARCSEC:
                            if mag is not None and m2 is not None:
                                if abs(mag - m2) > DUP_MAG_TOL:
                                    continue
                            elif sep > 2.0:
                                continue
                        if (
                            not placeholder
                            and not ph2
                            and plx
                            and p2
                            and plx > 0
                            and p2 > 0
                        ):
                            if abs(plx - p2) > DUP_PLX_FRAC * max(plx, p2) + 1.0:
                                continue  # a different star along the line of sight
                        return True
        return False


def _catalog_tag(source_id):
    try:
        return int(source_id) >> 48
    except (TypeError, ValueError):
        return 0


def merge_supplemental_and_pack(gaia_csv, supplemental_csv, merged_csv, out_bin):
    """Merge and de-duplicate the star feeds, then pack via catalogtool.

    Preference order Gaia > Hipparcos > Tycho-2: the better-measured entry
    survives (Gaia's astrometry; then Hipparcos, which has a real parallax,
    over Tycho-2, which has only a placeholder). Tycho rows that Tycho-2
    itself cross-identifies with a Hipparcos star already present are dropped
    by ID; everything else is matched on the sky (see SkyIndex)."""
    if not os.path.exists(supplemental_csv):
        sys.exit(f"error: missing supplemental catalog: {supplemental_csv}")
    cols = STAR_COLS.split(",")
    index = SkyIndex(DUP_RADIUS_ARCSEC)
    stats = {
        "gaia": 0,
        "hip_kept": 0,
        "hip_dup": 0,
        "tyc_kept": 0,
        "tyc_dup_id": 0,
        "tyc_dup_pos": 0,
    }
    hip_present = set()  # HIP numbers represented in the output (kept or matched)

    with tempfile.NamedTemporaryFile(
        mode="w",
        encoding="utf-8",
        newline="",
        delete=False,
        dir=os.path.dirname(os.path.abspath(merged_csv)),
        suffix=".csv",
    ) as tmp:
        tmp_path = tmp.name
        writer = csv.writer(tmp)
        writer.writerow(cols)

        # 1. Gaia: authoritative, copied whole — by column name, so a Gaia CSV
        #    written before the newer columns existed still merges.
        with open(gaia_csv, "r", encoding="utf-8", newline="") as f:
            reader = csv.DictReader(f)
            if not STAR_REQUIRED.issubset(reader.fieldnames or set()):
                sys.exit(
                    f"error: Gaia field schema missing columns: {sorted(STAR_REQUIRED)}"
                )
            for row in reader:
                ra, dec = ffloat(row.get("ra")), ffloat(row.get("dec"))
                if ra is None or dec is None:
                    continue
                writer.writerow([row.get(c, "") or "" for c in cols])
                index.add(
                    ra,
                    dec,
                    ffloat(row.get("mag")),
                    ffloat(row.get("parallax")),
                    False,
                    "gaia",
                )
                stats["gaia"] += 1

        # 2 and 3: supplemental rows, Hipparcos first, then Tycho-2 (two
        # streaming passes over the file keep memory flat for millions of rows).
        for want_tag in (0x48, 0x54):
            with open(supplemental_csv, "r", encoding="utf-8", newline="") as f:
                for row in csv.DictReader(f):
                    tag = _catalog_tag(row.get("source_id"))
                    if tag != want_tag:
                        continue
                    ra, dec = ffloat(row.get("ra")), ffloat(row.get("dec"))
                    if ra is None or dec is None:
                        continue
                    mag = ffloat(row.get("mag"))
                    plx = ffloat(row.get("parallax"))
                    placeholder = (row.get("placeholder_dist") or "").strip() == "1"
                    if tag == 0x48:
                        hip = int(row["source_id"]) & ((1 << 48) - 1)
                        dup = index.matches(ra, dec, mag, plx, placeholder, "hip")
                        hip_present.add(hip)
                        if dup:
                            stats["hip_dup"] += 1
                            continue
                        stats["hip_kept"] += 1
                        src = "hip"
                    else:
                        xref = fint(row.get("xref_hip"))
                        if xref is not None and xref in hip_present:
                            stats["tyc_dup_id"] += 1
                            continue
                        if index.matches(ra, dec, mag, plx, placeholder, "tycho"):
                            stats["tyc_dup_pos"] += 1
                            continue
                        stats["tyc_kept"] += 1
                        src = "tycho"
                    writer.writerow([row.get(c, "") or "" for c in cols])
                    index.add(ra, dec, mag, plx, placeholder, src)

    os.replace(tmp_path, merged_csv)
    total = stats["gaia"] + stats["hip_kept"] + stats["tyc_kept"]
    print(
        f"[fetch] star merge: Gaia {stats['gaia']}; Hipparcos kept {stats['hip_kept']}, "
        f"duplicates {stats['hip_dup']}; Tycho-2 kept {stats['tyc_kept']}, duplicates "
        f"{stats['tyc_dup_id']} by HIP id + {stats['tyc_dup_pos']} by position; total {total}"
    )
    print(f"[fetch] merged CSV -> {os.path.relpath(merged_csv, ROOT)}")

    tool = os.path.join(ROOT, "catalogtool")
    if not os.path.exists(tool):
        sys.exit("error: ./catalogtool not built — run `make catalogtool` first")
    subprocess.run([tool, "gaia-bin", merged_csv, out_bin], check=True)
    print(f"[fetch] merged star field packed -> {os.path.relpath(out_bin, ROOT)}")


def main():
    ap = argparse.ArgumentParser(
        description="Download NASA/ESA/CDS star catalogs for the Known Universe."
    )
    ap.add_argument("--out-dir", default=os.path.join(ROOT, "assets", "catalogs"))
    ap.add_argument("--skip-exoplanets", action="store_true")
    ap.add_argument("--skip-gaia", action="store_true")
    ap.add_argument(
        "--skip-gaia-field",
        action="store_true",
        help="skip the Gaia far-field StarBin download",
    )
    ap.add_argument(
        "--skip-supplemental",
        action="store_true",
        help="skip Tycho-2 and Hipparcos optical backfill catalogs",
    )
    ap.add_argument(
        "--gaia-field-maglimit",
        type=float,
        default=9.5,
        help="Gaia G-magnitude cutoff for the far-field scenery (default: 9.5)",
    )
    ap.add_argument(
        "--tycho-mag-min",
        type=float,
        default=9.5,
        help="Tycho-2 VT lower bound for fainter backfill (default: 9.5)",
    )
    ap.add_argument(
        "--tycho-mag-max",
        type=float,
        default=12.0,
        help="Tycho-2 VT upper bound for fainter backfill (default: 12.0)",
    )
    ap.add_argument(
        "--hipparcos-max-mag",
        type=float,
        default=6.0,
        help="Hipparcos V upper bound for bright-star backfill (default: 6.0)",
    )
    ap.add_argument(
        "--supplemental-parallax",
        type=float,
        default=0.2,
        help="placeholder parallax in mas for Tycho-2 scenery stars (default: 0.2)",
    )
    ap.add_argument(
        "--exo-table",
        default="pscomppars",
        help="NASA table: pscomppars (one row/planet, default) or ps",
    )
    ap.add_argument(
        "--exo-limit",
        type=int,
        default=0,
        help="ADQL TOP cap on exoplanets (0 = no cap)",
    )
    ap.add_argument(
        "--gaia-limit",
        type=int,
        default=200000,
        help="ADQL TOP cap on nearby Gaia stars (0 = no cap)",
    )
    ap.add_argument(
        "--parallax-min",
        type=float,
        default=5.0,
        help="minimum parallax in mas for nearby Gaia stars (default: 5)",
    )
    ap.add_argument(
        "--timeout",
        type=float,
        default=300.0,
        help="per-request timeout in seconds",
    )
    ap.add_argument(
        "--build",
        action="store_true",
        help="run build_known_universe.py after downloading",
    )
    args = ap.parse_args()

    if args.tycho_mag_max <= args.tycho_mag_min:
        ap.error("--tycho-mag-max must be greater than --tycho-mag-min")
    if args.gaia_field_maglimit <= 0:
        ap.error("--gaia-field-maglimit must be positive")
    if args.supplemental_parallax <= 0:
        ap.error("--supplemental-parallax must be positive")

    os.makedirs(args.out_dir, exist_ok=True)

    if not args.skip_exoplanets:
        fetch_exoplanets(
            os.path.join(args.out_dir, "exoplanets_full.csv"),
            args.exo_limit,
            args.exo_table,
            args.timeout,
        )

    if not args.skip_gaia:
        fetch_gaia(
            os.path.join(args.out_dir, "gaia_full.csv"),
            args.gaia_limit,
            args.parallax_min,
            args.timeout,
        )

    gaia_field_csv = os.path.join(args.out_dir, "gaia_field_full.csv")
    out_bin = os.path.join(args.out_dir, "gaia_stars.bin")

    if not args.skip_gaia_field:
        fetch_gaia_field(
            gaia_field_csv,
            out_bin,
            args.gaia_field_maglimit,
            args.timeout,
        )

    supplemental_csv = os.path.join(args.out_dir, "supplemental_stars_full.csv")
    if not args.skip_supplemental:
        # The fallback is scenery-only; write it into the Tycho rows below.
        # Kept as a parameter so the distance assumption is explicit at the CLI.
        fetch_supplemental(
            supplemental_csv,
            args.tycho_mag_min,
            args.tycho_mag_max,
            args.hipparcos_max_mag,
            args.timeout,
        )

        # Patch the Tycho rows' distance field to the configured scenery fallback.
        # Hipparcos rows already carry measured parallax and are left untouched.
        tmp_path = supplemental_csv + ".tmp"
        with (
            open(supplemental_csv, "r", encoding="utf-8", newline="") as src,
            open(tmp_path, "w", encoding="utf-8", newline="") as dst,
        ):
            reader = csv.DictReader(src)
            writer = csv.DictWriter(dst, fieldnames=STAR_COLS.split(","))
            writer.writeheader()
            for row in reader:
                sid = int(row["source_id"])
                catalog_tag = sid >> 48
                if catalog_tag == 0x54 and not row.get("parallax"):
                    row["parallax"] = f"{args.supplemental_parallax:.6f}"
                writer.writerow(row)
        os.replace(tmp_path, supplemental_csv)

        if not args.skip_gaia_field:
            merge_supplemental_and_pack(
                gaia_field_csv,
                supplemental_csv,
                os.path.join(args.out_dir, "gaia_field_merged.csv"),
                out_bin,
            )
        else:
            print(
                "[fetch] supplemental catalog fetched, but --skip-gaia-field prevents StarBin merge"
            )
    elif not args.skip_gaia_field:
        # Preserve the original fast path when supplemental feeds are disabled.
        tool = os.path.join(ROOT, "catalogtool")
        if not os.path.exists(tool):
            sys.exit("error: ./catalogtool not built — run `make catalogtool` first")
        subprocess.run([tool, "gaia-bin", gaia_field_csv, out_bin], check=True)
        print(f"[fetch] gaia-field: packed -> {os.path.relpath(out_bin, ROOT)}")

    print(
        "[fetch] black holes: curated (assets/catalogs/black_holes.csv), "
        "not downloaded — no bulk feed exists."
    )

    if args.build:
        print("[fetch] rebuilding known universe ...")
        subprocess.run(
            [sys.executable, os.path.join(ROOT, "tools", "build_known_universe.py")],
            check=True,
        )


if __name__ == "__main__":
    main()
