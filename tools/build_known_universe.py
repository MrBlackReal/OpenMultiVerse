#!/usr/bin/env python3
"""
build_known_universe.py — merge everything we have real data for into one
universe JSON: the full Solar System (at the origin) plus every catalogued
exoplanet host system and nearby star, each at its true position in light-years,
plus curated real black holes (galactic stellar-mass holes, Sgr A*, and famous
extragalactic SMBHs/quasars).

It shells out to ./catalogtool to convert each CSV catalog, concatenates the
resulting "bodies" arrays, and de-duplicates stars by name (the first occurrence
wins; later duplicates and their planets are dropped so a star catalogued in two
sources is not placed twice). It then writes TWO files:
  * a lean **manifest** JSON (--out): laws + the curated Solar System and black
    holes inline (human-readable), plus a "body_catalog" reference; and
  * a **BodyBin** binary (--out-bin): the bulk catalog bodies (nearby stars +
    exoplanet systems), written by shelling out to `verse --export-body-catalog`
    (the engine owns the Keplerian->state math + the binary format).
So it needs ./verse AND ./catalogtool built (`make`), and a headless-capable GPU
for the export step (same requirement as tools/shot.sh).

Usage:
    python3 tools/build_known_universe.py \
        [--exoplanets assets/catalogs/exoplanets_sample.csv] \
        [--gaia       assets/catalogs/gaia_sample.csv] \
        [--blackholes assets/catalogs/black_holes.csv] \
        [--solar      assets/universe.json] \
        [--out        assets/universes/known_universe.json] \
        [--out-bin    assets/catalogs/known_universe_bodies.bin]

To fetch the full NASA/ESA catalogs first, run tools/fetch_catalogs.py (it
downloads exoplanets_full.csv + gaia_full.csv via the archive query APIs, which
this script then prefers automatically). Or point --exoplanets / --gaia at full
NASA Exoplanet Archive / Gaia exports to
build a larger universe.  The simulator decouples cross-system gravity and draws
distant stars as a cheap far-field point pass, so the full catalogs (~16k
bodies) run in real time — --max-systems just chooses how big a slice you want,
not whether it is runnable.
"""
import argparse
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def strip_json_comments(text):
    """Remove // line comments that the loader allows but json.loads does not,
    without touching // that appears inside a string literal."""
    out = []
    in_str = False
    esc = False
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if in_str:
            out.append(c)
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == '"':
                in_str = False
            i += 1
            continue
        if c == '"':
            in_str = True
            out.append(c)
            i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def load_jsonc(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.loads(strip_json_comments(f.read()))


def catalogtool(kind, csv_path, out_path):
    tool = os.path.join(ROOT, "catalogtool")
    if not os.path.exists(tool):
        sys.exit("error: ./catalogtool not built — run `make catalogtool` first")
    subprocess.run([tool, kind, csv_path, out_path], check=True)
    return load_jsonc(out_path)["bodies"]


def norm(name):
    """Normalization key for matching the same object across catalogs that
    differ only in case or whitespace (e.g. "Barnard's Star" vs "Barnard's
    star")."""
    return " ".join(name.split()).lower()


def drop_unplaced(bodies):
    """Remove catalogue systems whose star has no real position.  When a source
    lacks a distance (e.g. many Kepler hosts have no parallax), the importer
    parks the star at the origin (0,0,0) — right on top of the Solar System.
    Such systems are placeholders, not real placements, so drop the whole
    system (star + planets)."""
    by_name = {b.get("name", ""): b for b in bodies}

    def root(b):
        cur, guard = b, 0
        while cur.get("type") != "star" and guard < 16:
            p = by_name.get(cur.get("parent", ""))
            if p is None:
                break
            cur, guard = p, guard + 1
        return cur

    def placed(star):
        p = star.get("pos_ly")
        return bool(p) and (p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) > 1e-6

    return [b for b in bodies if placed(root(b))]


def merge(solar, extra_sources):
    """solar: parsed assets/universe.json. extra_sources: list of bodies lists.

    Concatenates every source, then de-duplicates so a star or planet
    catalogued in more than one source appears once:
      - stars are matched case-insensitively; the first (curated) spelling wins;
      - a duplicate star's planets are re-parented to the surviving star, so the
        union of its planets across sources is kept (no system is lost, none is
        doubled);
      - planets/moons are then de-duplicated by normalized name.
    The Solar System is listed first, so its names are authoritative.
    """
    raw = list(solar["bodies"])
    for src in extra_sources:
        raw += src

    # Canonical spelling for each star (first occurrence wins).
    canon = {}
    for b in raw:
        if b.get("type") == "star":
            canon.setdefault(norm(b.get("name", "")), b.get("name", ""))

    out, seen = [], set()
    for b in raw:
        is_star = b.get("type") == "star"
        # Namespace the dedup key by star-vs-not so a stellar component and a
        # planet that differ only in letter case do not collide.  Astronomical
        # naming uses an uppercase suffix for a stellar companion ("55 Cnc B")
        # and a lowercase one for a planet ("55 Cnc b"); norm() lowercases both,
        # so without the namespace the star would be dropped as a "duplicate" of
        # the planet, orphaning the star's own planets.
        key = ("star:" if is_star else "obj:") + norm(b.get("name", ""))
        if not is_star:
            # Re-point a planet at the surviving spelling of its host star.
            canon_parent = canon.get(norm(b.get("parent", "")))
            if canon_parent and canon_parent != b.get("parent", ""):
                b = dict(b, parent=canon_parent)
        if key in seen:
            continue
        seen.add(key)
        out.append(b)
    return out


def dedup_positional(bodies, eps_ly=0.1):
    """Drop a star that sits within eps_ly of an earlier-listed star.  The same
    physical star often appears in two catalogs under unrelatable names (an
    exoplanet host like "Proxima Cen" vs its bare Gaia source_id), so name
    de-dup misses it and the render shows two stars on top of each other.
    Sources are ordered curated-first, so the named/curated entry survives.
    Stars with bodies parented under them are never dropped (that would orphan
    the children); real binaries are far tighter than eps_ly and both members
    are usually curated, so only cross-catalog duplicates match."""
    has_children = {b.get("parent", "") for b in bodies if b.get("parent")}
    grid = {}

    def cell(p):
        return (int(p[0] // eps_ly), int(p[1] // eps_ly), int(p[2] // eps_ly))

    out, dropped = [], 0
    for b in bodies:
        if b.get("type") != "star":
            out.append(b)
            continue
        p = b.get("pos_ly", [0.0, 0.0, 0.0])
        cx, cy, cz = cell(p)
        dup = False
        if b.get("name", "") not in has_children:
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    for dz in (-1, 0, 1):
                        for q in grid.get((cx + dx, cy + dy, cz + dz), ()):
                            d2 = ((p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2
                                  + (p[2] - q[2]) ** 2)
                            if d2 < eps_ly * eps_ly:
                                dup = True
                                break
                        if dup:
                            break
                    if dup:
                        break
                if dup:
                    break
        if dup:
            dropped += 1
            continue
        grid.setdefault((cx, cy, cz), []).append(p)
        out.append(b)
    if dropped:
        print(f"[known] dropped {dropped} cross-catalog duplicate stars "
              f"(same position within {eps_ly} ly)")
    return out


def cap_nearest(bodies, max_systems):
    """Keep the Solar System plus the `max_systems` nearest other star systems
    (a whole system = the star and everything parented under it). max_systems<=0
    keeps everything.  This now just sizes the universe to taste — the simulator
    handles the full catalogs (~16k bodies) in real time (cross-system gravity is
    decoupled and distant stars render as far-field points), so trimming is
    optional, not required for the result to run."""
    if max_systems <= 0:
        return bodies
    by_name = {b.get("name", ""): b for b in bodies}

    roots = ("star", "black_hole", "quasar")   # a system root is a star OR a hole

    def root(b):
        cur, guard = b, 0
        while cur.get("type") not in roots and guard < 16:
            p = by_name.get(cur.get("parent", ""))
            if p is None:
                break
            cur, guard = p, guard + 1
        return cur

    def dist(star):
        p = star.get("pos_ly", [0.0, 0.0, 0.0])
        return (p[0] ** 2 + p[1] ** 2 + p[2] ** 2) ** 0.5

    stars = sorted((b for b in bodies if b.get("type") in roots), key=dist)
    keep = {s.get("name", "") for s in stars[:max_systems]}
    keep.add("Sun")                    # the Solar System is always included
    return [b for b in bodies if root(b).get("name", "") in keep]


def split_curated(bodies):
    """Partition into (curated, bulk). Curated bodies stay human-readable in the
    manifest JSON; bulk bodies go to the BodyBin. A body is curated iff it is a
    black hole/quasar or its root (walking parent links) is the Sun — the SAME
    rule the engine's exporter uses (universe.c body_is_curated), so the two
    halves reassemble to exactly the input set with nothing dropped or doubled."""
    by_name = {b.get("name", ""): b for b in bodies}
    roots = ("star", "black_hole", "quasar")

    def root(b):
        cur, guard = b, 0
        while cur.get("type") not in roots and guard < 32:
            p = by_name.get(cur.get("parent", ""))
            if p is None:
                break
            cur, guard = p, guard + 1
        return cur

    def curated(b):
        return (b.get("type") in ("black_hole", "quasar")
                or root(b).get("name", "") == "Sun")

    cur, bulk = [], []
    for b in bodies:
        (cur if curated(b) else bulk).append(b)
    return cur, bulk


def export_body_catalog(bulk, laws, tmp, out_bin):
    """Resolve `bulk` bodies to absolute state and write them to a BodyBin, by
    shelling out to `verse --export-body-catalog` (the engine owns the
    Keplerian→state math and the binary format). Needs ./verse built and a
    headless-capable GPU (same requirement as the tools/shot.sh renders)."""
    verse = os.path.join(ROOT, "verse")
    if not os.path.exists(verse):
        sys.exit("error: ./verse not built — run `make` first")
    bulk_json = os.path.join(tmp, "bulk.json")
    with open(bulk_json, "w", encoding="utf-8") as f:
        json.dump({"laws": laws, "bodies": bulk}, f)
    subprocess.run([verse, "--export-body-catalog", out_bin,
                    "--preset", bulk_json], check=True)


def main():
    def default_csv(full, sample):
        return full if os.path.exists(os.path.join(ROOT, full)) else sample

    ap = argparse.ArgumentParser()
    ap.add_argument("--exoplanets", default=default_csv(
        "assets/catalogs/exoplanets_full.csv", "assets/catalogs/exoplanets_sample.csv"))
    ap.add_argument("--gaia", default=default_csv(
        "assets/catalogs/gaia_full.csv", "assets/catalogs/gaia_sample.csv"))
    ap.add_argument("--blackholes", default="assets/catalogs/black_holes.csv",
                    help="curated real black-hole CSV (no bulk feed exists; "
                         "empty string to skip)")
    ap.add_argument("--solar", default="assets/universe.json")
    ap.add_argument("--out", default="assets/universes/known_universe.json",
                    help="manifest JSON to write (laws + curated bodies + refs)")
    ap.add_argument("--out-bin", default="assets/catalogs/known_universe_bodies.bin",
                    help="BodyBin for the bulk catalog bodies the manifest references")
    ap.add_argument("--star-catalog", default="assets/catalogs/gaia_stars.bin",
                    help="StarBin of far-field scenery stars to reference (empty to omit)")
    ap.add_argument("--max-systems", type=int, default=0,
                    help="keep the Solar System + this many nearest star systems "
                         "(default 0 = everything, matching the shipped preset; "
                         "the full catalogs run in real time, so this just sizes "
                         "the universe to a smaller slice if you want one)")
    args = ap.parse_args()

    tmp = os.path.join(ROOT, "tools", "_known_tmp")
    os.makedirs(tmp, exist_ok=True)

    solar = load_jsonc(os.path.join(ROOT, args.solar))

    extra = []
    if args.exoplanets:
        extra.append(catalogtool("exoplanets",
                                 os.path.join(ROOT, args.exoplanets),
                                 os.path.join(tmp, "exo.json")))
    if args.gaia:
        extra.append(catalogtool("gaia",
                                 os.path.join(ROOT, args.gaia),
                                 os.path.join(tmp, "gaia.json")))
    if args.blackholes:
        extra.append(catalogtool("blackholes",
                                 os.path.join(ROOT, args.blackholes),
                                 os.path.join(tmp, "bh.json")))

    # Drop catalogue systems with no real position (parked at the origin).
    extra = [drop_unplaced(src) for src in extra]

    bodies = merge(solar, extra)
    bodies = dedup_positional(bodies)
    bodies = cap_nearest(bodies, args.max_systems)

    laws = {
        "G": 6.674e-11, "softening": 1e5, "time_scale": 1.0,
        "force_exp": 2.0, "lambda": 0.0, "pn_factor": 0.0,
        "c_light": 2.99792458e8,
    }

    # Split: the curated Solar System + black holes stay inline in the manifest;
    # the bulk catalog bodies (nearby stars + exoplanet systems) go to the
    # BodyBin so the JSON stays a lean, readable manifest.
    curated, bulk = split_curated(bodies)
    export_body_catalog(bulk, laws, tmp, os.path.join(ROOT, args.out_bin))

    manifest = {"laws": laws, "body_catalog": args.out_bin}
    if args.star_catalog:
        manifest["star_catalog"] = args.star_catalog
    manifest["bodies"] = curated
    # Carry the Solar System's procedural rings and asteroid belts verbatim.
    for key in ("rings", "asteroid_belts"):
        if key in solar:
            manifest[key] = solar[key]

    out_path = os.path.join(ROOT, args.out)
    header = ("// OpenMultiVerse \"Known Universe\" — manifest, auto-generated by\n"
              "// tools/build_known_universe.py. Laws + the curated Solar System and\n"
              "// black holes inline; bulk catalog bodies load from body_catalog (a\n"
              "// BodyBin), far-field scenery stars from star_catalog (a StarBin).\n")
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(header)
        json.dump(manifest, f, indent=2)
        f.write("\n")

    n_bh = sum(1 for b in curated if b.get("type") in ("black_hole", "quasar"))
    print(f"[known] manifest: {len(curated)} curated bodies ({n_bh} black holes) "
          f"-> {args.out}")
    print(f"[known] body_catalog: {len(bulk)} bulk bodies -> {args.out_bin}")


if __name__ == "__main__":
    main()
