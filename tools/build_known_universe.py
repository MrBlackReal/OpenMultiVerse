#!/usr/bin/env python3
"""
build_known_universe.py — merge everything we have real data for into one
universe JSON: the full Solar System (at the origin) plus every catalogued
exoplanet host system and nearby star, each at its true position in light-years.

It shells out to ./catalogtool to convert each CSV catalog, then concatenates
the resulting "bodies" arrays into a single file, de-duplicating stars by name
(the first occurrence wins; later duplicates and their planets are dropped so a
star catalogued in two sources is not placed twice).

Usage:
    python3 tools/build_known_universe.py \
        [--exoplanets assets/catalogs/exoplanets_sample.csv] \
        [--gaia       assets/catalogs/gaia_sample.csv] \
        [--solar      assets/universe.json] \
        [--out        assets/universes/known_universe.json]

Point --exoplanets / --gaia at full NASA Exoplanet Archive / Gaia exports to
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
        key = norm(b.get("name", ""))
        if b.get("type") != "star":
            # Re-point a planet at the surviving spelling of its host star.
            canon_parent = canon.get(norm(b.get("parent", "")))
            if canon_parent and canon_parent != b.get("parent", ""):
                b = dict(b, parent=canon_parent)
        if key in seen:
            continue
        seen.add(key)
        out.append(b)
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

    def root(b):
        cur, guard = b, 0
        while cur.get("type") != "star" and guard < 16:
            p = by_name.get(cur.get("parent", ""))
            if p is None:
                break
            cur, guard = p, guard + 1
        return cur

    def dist(star):
        p = star.get("pos_ly", [0.0, 0.0, 0.0])
        return (p[0] ** 2 + p[1] ** 2 + p[2] ** 2) ** 0.5

    stars = sorted((b for b in bodies if b.get("type") == "star"), key=dist)
    keep = {s.get("name", "") for s in stars[:max_systems]}
    keep.add("Sun")                    # the Solar System is always included
    return [b for b in bodies if root(b).get("name", "") in keep]


def main():
    def default_csv(full, sample):
        return full if os.path.exists(os.path.join(ROOT, full)) else sample

    ap = argparse.ArgumentParser()
    ap.add_argument("--exoplanets", default=default_csv(
        "assets/catalogs/exoplanets_full.csv", "assets/catalogs/exoplanets_sample.csv"))
    ap.add_argument("--gaia", default=default_csv(
        "assets/catalogs/gaia_full.csv", "assets/catalogs/gaia_sample.csv"))
    ap.add_argument("--solar", default="assets/universe.json")
    ap.add_argument("--out", default="assets/universes/known_universe.json")
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

    # Drop catalogue systems with no real position (parked at the origin).
    extra = [drop_unplaced(src) for src in extra]

    bodies = merge(solar, extra)
    bodies = cap_nearest(bodies, args.max_systems)

    universe = {
        "laws": {
            "G": 6.674e-11, "softening": 1e5, "time_scale": 1.0,
            "force_exp": 2.0, "lambda": 0.0, "pn_factor": 0.0,
            "c_light": 2.99792458e8,
        },
        "bodies": bodies,
    }
    # Carry the Solar System's procedural rings and asteroid belts verbatim.
    for key in ("rings", "asteroid_belts"):
        if key in solar:
            universe[key] = solar[key]

    out_path = os.path.join(ROOT, args.out)
    header = ("// OpenMultiVerse \"Known Universe\" — auto-generated by\n"
              "// tools/build_known_universe.py. Solar System at the origin;\n"
              "// catalogued stars/exoplanets at their true light-year positions.\n")
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(header)
        json.dump(universe, f, indent=2)
        f.write("\n")

    n_star = sum(1 for b in bodies if b.get("type") == "star")
    print(f"[known] wrote {len(bodies)} bodies ({n_star} stars) -> {args.out}")


if __name__ == "__main__":
    main()
