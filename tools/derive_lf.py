#!/usr/bin/env python3
"""
derive_lf.py — measure the shipped catalog's luminosity function and its
detection limit, so the procedural population can be generated to match.

Why this exists
---------------
The two-tier population needs two numbers that must NOT be invented:

  1. The stellar luminosity function -- how many stars per unit volume at each
     absolute magnitude. The procedural tier samples from this, so generated
     stars and catalog stars are drawn from the same population by
     construction rather than by tuning.

  2. The catalog's detection limit -- the apparent magnitude beyond which it
     stops seeing things. This is the selection function: a procedural star is
     emitted only if the catalog COULD NOT have detected it, which is what
     makes the two tiers complement instead of double-count.

Both are measured from assets/catalogs/gaia_stars.bin itself.

The completeness trap
---------------------
The catalog's raw absolute-magnitude histogram is NOT the luminosity function.
A magnitude-limited survey sees intrinsically bright stars much further away
than faint ones, so the raw histogram massively over-represents the bright end.
The fix is a volume-complete subsample: inside some radius the survey catches
essentially everything, and only there does counting give the true LF. This
script finds that radius from the data instead of assuming one.

Usage:
  python3 tools/derive_lf.py
  python3 tools/derive_lf.py --emit src/core/stellar_lf.h
"""

import argparse
import math
import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(REPO, "assets", "catalogs", "gaia_stars.bin")

STARBIN_MAGIC = 0x4F4D5653
REC = 56
FLAG_PLACEHOLDER_DIST = 0x01
LY_PER_PC = 3.261563


def read_catalog(path):
    """Yield (dist_ly, abs_mag) for rows with a real distance and magnitude."""
    with open(path, "rb") as f:
        magic, version, count, rsize = struct.unpack("<IIII", f.read(16))
        if magic != STARBIN_MAGIC:
            sys.exit(f"[lf] {path}: bad magic {magic:#x}")
        print(f"[lf] {os.path.basename(path)}: v{version}, {count} records, "
              f"{rsize} B each")
        if rsize != REC:
            sys.exit(f"[lf] unexpected record size {rsize} (expected {REC})")
        blob = f.read(count * rsize)

    out = []
    skipped_flag = skipped_nan = 0
    bad_dist = bad_mag = 0
    for i in range(count):
        o = i * rsize
        x, y, z = struct.unpack_from("<fff", blob, o + 8)
        flags = blob[o + 43]
        absmag = struct.unpack_from("<f", blob, o + 44)[0]
        if flags & FLAG_PLACEHOLDER_DIST:
            skipped_flag += 1          # scenery distance, not a measurement
            continue
        if absmag != absmag:           # NaN
            skipped_nan += 1
            continue
        d = math.sqrt(x * x + y * y + z * z)
        if d <= 0.0:
            continue
        # Data-quality screen. A near-zero or negative parallax inverts into an
        # absurd distance, which then inverts into an absurd absolute
        # magnitude; both poison a 1/Vmax estimate, where the weight is
        # 1/d_max^3. Nothing stellar is brighter than about M = -10 (that is
        # ~10^6 Lsun) and this catalog is built as a nearby-star cut, so a row
        # past 100 kly is noise rather than a real halo member.
        if d > 100000.0:
            bad_dist += 1
            continue
        if absmag < -10.0 or absmag > 25.0:
            bad_mag += 1
            continue
        out.append((d, absmag))
    print(f"[lf] usable {len(out)}  (skipped {skipped_flag} placeholder-distance,"
          f" {skipped_nan} without abs_mag, {bad_dist} implausible distance,"
          f" {bad_mag} implausible abs_mag)")
    return out


def apparent(absmag, dist_ly):
    d_pc = max(dist_ly / LY_PER_PC, 1e-9)
    return absmag + 5.0 * math.log10(d_pc) - 5.0


def detection_limit(stars):
    """Find where the apparent-magnitude counts turn over.

    In a magnitude-limited survey, log N(<m) rises roughly linearly with m
    while the survey is complete, then rolls over as it starts missing things.
    The peak of the per-bin histogram is a robust, assumption-free estimate of
    where that happens.
    """
    hist = {}
    for d, M in stars:
        m = apparent(M, d)
        hist[round(m * 2) / 2] = hist.get(round(m * 2) / 2, 0) + 1
    if not hist:
        sys.exit("[lf] no stars to measure")
    peak_m = max(hist, key=lambda k: hist[k])
    # The peak bin under-reports when the final bin is partially filled, and a
    # hard max is hostage to one outlier. Take the 99th percentile of apparent
    # magnitude as the working limit.
    mags = sorted(apparent(M, d) for d, M in stars)
    p99 = mags[int(0.99 * (len(mags) - 1))]
    lo, hi = min(hist), max(hist)
    print(f"\n[lf] apparent magnitude: range {lo:.1f} .. {hi:.1f}, "
          f"histogram peaks at m = {peak_m:.1f}, p99 = {p99:.2f}")
    print("[lf]   m      count")
    for m in sorted(hist):
        if hist[m] < 20:
            continue
        bar = "#" * min(60, hist[m] * 60 // max(hist.values()))
        print(f"[lf] {m:6.1f} {hist[m]:8d}  {bar}")
    return p99


def completeness_radius(stars, m_limit):
    """Largest radius inside which the catalog still looks volume-complete.

    A complete sample has N(<r) proportional to r^3, so N/r^3 is flat and then
    falls once the survey starts missing stars. Report the radius where the
    density has dropped to 80% of its innermost value.
    """
    ds = sorted(d for d, _ in stars)
    if not ds:
        return 0.0
    probes = [r for r in (5, 10, 15, 20, 25, 30, 40, 50, 65, 80, 100,
                          130, 160, 200, 260, 326) ]
    print("\n[lf] volume completeness (stars per 1000 ly^3):")
    dens = []
    import bisect
    for r in probes:
        n = bisect.bisect_right(ds, r)
        v = (4.0 / 3.0) * math.pi * r ** 3
        d = n / v * 1000.0
        dens.append((r, n, d))
        print(f"[lf]   r < {r:4d} ly : {n:7d} stars, density {d:8.4f}")
    ref = max(d for _, _, d in dens[:3]) if dens else 0.0
    r_complete = probes[0]
    for r, n, d in dens:
        if ref > 0 and d >= 0.80 * ref:
            r_complete = r
    print(f"[lf] -> volume-complete to about {r_complete} ly "
          f"(density within 80% of peak)")
    return r_complete


def luminosity_function(stars, m_limit, r_max_ly):
    """Stars per ly^3 per 1-mag bin, by the 1/Vmax estimator.

    This catalog is nowhere volume-complete -- it is a bright cut at m ~ 9.5,
    so even at 10 ly the faint M dwarfs that dominate by number are already
    missing. Counting inside any radius therefore undercounts the faint end
    badly.

    1/Vmax (Schmidt 1968) is the standard cure for a magnitude-limited sample:
    a star of absolute magnitude M could only have been detected out to
        d_max = 10^((m_limit - M + 5) / 5)  parsecs,
    so it represents 1/V(d_max) stars per unit volume. Summing those weights
    per bin gives an unbiased density without needing a complete subsample.
    V is capped at the catalog's own outer radius, since nothing beyond it was
    included regardless of brightness.

    The faint end still cannot be recovered: below roughly M = 13 the
    detection volume shrinks to nothing and the bin is empty or dominated by
    one or two stars. Those bins are reported but flagged -- they need an
    external volume-complete sample (the Gaia Catalogue of Nearby Stars is the
    obvious source) rather than extrapolation.
    """
    bins = {}
    counts = {}
    for d, M in stars:
        b = math.floor(M)
        d_max_pc = 10.0 ** ((m_limit - M + 5.0) / 5.0)
        d_max_ly = min(d_max_pc * LY_PER_PC, r_max_ly)
        if d_max_ly <= 0.0:
            continue
        vmax = (4.0 / 3.0) * math.pi * d_max_ly ** 3
        bins[b] = bins.get(b, 0.0) + 1.0 / vmax
        counts[b] = counts.get(b, 0) + 1
    print(f"\n[lf] luminosity function by 1/Vmax "
          f"(m_limit {m_limit:.1f}, catalog radius {r_max_ly:.0f} ly):")
    print("[lf]   M_abs    count     d_max(ly)     per ly^3")
    table = []
    peak = max(bins.values()) if bins else 1.0
    for M in sorted(bins):
        d_max_pc = 10.0 ** ((m_limit - M + 5.0) / 5.0)
        d_max_ly = min(d_max_pc * LY_PER_PC, r_max_ly)
        phi = bins[M]
        weak = " (sparse)" if counts[M] < 20 else ""
        table.append((M, counts[M], phi))
        bar = "#" * min(44, int(44 * phi / peak))
        print(f"[lf] {M:6d}  {counts[M]:7d}   {d_max_ly:10.1f}   "
              f"{phi:.3e}  {bar}{weak}")
    total = sum(phi for _, _, phi in table)
    print(f"[lf] total implied density {total:.4e} stars/ly^3 "
          f"({total * 1000:.3f} per 1000 ly^3)")
    print("[lf] for reference the true local value is about 4e-3 /ly^3; "
          "a large shortfall here is the unrecoverable faint end.")
    return table


def emit_header(path, table, m_limit, r_complete):
    """r_complete is the catalog's outer radius (see CATALOG_MAX_LY)."""
    lo = min(M for M, _, _ in table)
    hi = max(M for M, _, _ in table)
    with open(path, "w") as f:
        f.write("/*\n"
                " * stellar_lf.h - GENERATED by tools/derive_lf.py. Do not edit.\n"
                " *\n"
                " * The shipped catalog's own luminosity function and detection\n"
                " * limit, measured from assets/catalogs/gaia_stars.bin. The\n"
                " * procedural population samples this so generated stars and\n"
                " * catalog stars come from one population by construction.\n"
                " */\n#pragma once\n\n")
        f.write(f"/* Apparent magnitude beyond which the catalog stops seeing "
                f"things. */\n#define CATALOG_MAG_LIMIT   {m_limit:.2f}f\n\n")
        f.write(f"/* Outermost catalog row (ly). NOT a completeness radius:\n"
                f" * this catalog is nowhere volume-complete. */\n"
                f"#define CATALOG_MAX_LY      {r_complete:.1f}f\n\n")
        f.write(f"#define STELLAR_LF_M_MIN    {lo}\n"
                f"#define STELLAR_LF_M_MAX    {hi}\n"
                f"#define STELLAR_LF_BINS     {hi - lo + 1}\n\n")
        f.write("/* stars per ly^3 in each 1-mag bin, starting at "
                "STELLAR_LF_M_MIN */\n"
                "static const float STELLAR_LF[STELLAR_LF_BINS] = {\n")
        by_m = {M: phi for M, _, phi in table}
        for M in range(lo, hi + 1):
            f.write(f"    {by_m.get(M, 0.0):.6e}f,   /* M = {M:+d} */\n")
        tot = sum(phi for _, _, phi in table)
        f.write("};\n\n"
                "/* Summed LF: implied total stellar density, stars per ly^3.\n"
                " * Used to set each procedural cascade's magnitude cut so a\n"
                " * coarse cell emits its brightest few members at the correct\n"
                " * space density instead of a luminosity-boosted fake. */\n"
                f"#define STELLAR_DENSITY_LY3 {tot:.6e}f\n")
    print(f"\n[lf] wrote {path}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=BIN)
    ap.add_argument("--emit", default=None, help="write a C header here")
    a = ap.parse_args()

    stars = read_catalog(a.bin)
    m_limit = detection_limit(stars)
    r_max = max(d for d, _ in stars)
    completeness_radius(stars, m_limit)      # diagnostics only
    table = luminosity_function(stars, m_limit, r_max)
    if a.emit:
        emit_header(a.emit, table, m_limit, r_max)


if __name__ == "__main__":
    main()
