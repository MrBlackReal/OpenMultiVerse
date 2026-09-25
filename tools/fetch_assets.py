#!/usr/bin/env python3
"""Fetch the bulk model/texture assets that are not stored in git.

    tools/fetch_assets.py                 download whatever assets/manifest.tsv lists and is missing
    tools/fetch_assets.py --check         verify sha256 of files already on disk, download nothing
    tools/fetch_assets.py --only textures/mars,models/spacecraft   limit to path prefixes (under assets/)
    tools/fetch_assets.py --build         (maintainers) rebuild the manifest from the files on disk

The manifest (assets/manifest.tsv: path, bytes, sha256, url) is committed;
the data is not. Downloads are resumable-by-skip: a file whose size and hash
already match is left alone, and each file is written atomically. Stdlib only.

--build matches every file not tracked by git (ignored ones included, minus baked *.bc7.dds) under assets/models and
assets/textures against the URL lists in tools/links/ (prograde-data, PDS SBN),
by path mapping first and basename+size second, and hashes it. Files with no
known source are listed and left out of the manifest.
"""
import argparse, concurrent.futures as cf, csv, hashlib, os, subprocess, sys, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, "assets")
MANIFEST = os.path.join(ASSETS, "manifest.tsv")
LINKS = os.path.join(ROOT, "tools", "links")
UA = {"User-Agent": "OpenMultiVerse-fetch-assets/1"}

def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()

def read_manifest():
    with open(MANIFEST, newline="") as f:
        return list(csv.DictReader(f, delimiter="\t"))

def ok(dst, row):
    return (os.path.isfile(dst) and os.path.getsize(dst) == int(row["bytes"])
            and sha256(dst) == row["sha256"])

def download(row):
    dst = os.path.join(ASSETS, row["path"])
    if ok(dst, row):
        return row["path"], "ok"
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    tmp = dst + ".part"
    try:
        with urllib.request.urlopen(urllib.request.Request(row["url"], headers=UA), timeout=60) as r, \
                open(tmp, "wb") as f:
            while b := r.read(1 << 20):
                f.write(b)
        if sha256(tmp) != row["sha256"]:
            os.remove(tmp)
            return row["path"], "HASH MISMATCH (upstream changed?)"
        os.replace(tmp, dst)
        return row["path"], "downloaded"
    except Exception as e:
        if os.path.exists(tmp):
            os.remove(tmp)
        return row["path"], f"FAILED: {e}"

def fetch(args):
    rows = read_manifest()
    if args.only:
        pre = tuple(p.strip("/") for p in args.only.split(","))
        rows = [r for r in rows if r["path"].startswith(pre)]
    if args.check:
        bad = [r["path"] for r in rows if not ok(os.path.join(ASSETS, r["path"]), r)]
        print(f"{len(rows) - len(bad)}/{len(rows)} verified")
        for p in bad:
            print("  missing/bad:", p)
        return 1 if bad else 0
    failed = 0
    with cf.ThreadPoolExecutor(args.jobs) as ex:
        for i, (p, st) in enumerate(ex.map(download, rows), 1):
            if st != "ok":
                print(f"[{i}/{len(rows)}] {p}: {st}", flush=True)
            failed += st not in ("ok", "downloaded")
    print(f"done, {failed} failed" if failed else "done")
    return 1 if failed else 0

# ---- maintainer side ------------------------------------------------------

def tsv(name):
    with open(os.path.join(LINKS, name), newline="") as f:
        return list(csv.DictReader(f, delimiter="\t"))

def build():
    # candidate sources: (url, bytes) keyed by expected local path and by (basename, bytes)
    by_path, by_name = {}, {}
    for r in tsv("prograde_assets.tsv"):
        p, b = r["path"], int(r["bytes"] or 0)
        if p.startswith("images/"):
            _, body, rest = p.split("/", 2)
            local = f"textures/{body.lower()}/{rest}"
        elif p.startswith("models/"):
            rest = p[len("models/"):]
            local = ("models/spacecraft/" if "/" in rest else "models/small_bodies/") + rest
            # per-model LICENSE for the small-body PLYs sits at models/LICENSE
            if rest == "LICENSE":
                local = "models/small_bodies/LICENSE"
        else:
            continue
        by_path[local] = (r["url"], b)
    for r in tsv("sbn_links.tsv"):
        by_name.setdefault((r["url"].rsplit("/", 1)[-1], int(r["bytes"] or 0)), r["url"])

    tracked = set(subprocess.run(["git", "-C", ROOT, "ls-files", "assets/models", "assets/textures"],
                                 capture_output=True, text=True, check=True).stdout.splitlines())
    junk = {".directory", ".DS_Store", "flip.xsh"}
    files = []
    for top in ("models", "textures"):
        for d, _, names in os.walk(os.path.join(ASSETS, top)):
            for n in names:
                rel = os.path.relpath(os.path.join(d, n), ASSETS)
                if f"assets/{rel}" not in tracked and n not in junk and not n.endswith((".bc7.dds", ".part")):
                    files.append(rel)
    files.sort()
    rows, unknown = [], []
    for rel in files:
        full = os.path.join(ASSETS, rel)
        size = os.path.getsize(full)
        url = None
        if rel in by_path and by_path[rel][1] in (size, 0):
            url = by_path[rel][0]
        else:
            url = by_name.get((os.path.basename(rel), size))
        if url is None:
            unknown.append(rel)
            continue
        rows.append((rel, size, sha256(full), url))
    with open(MANIFEST, "w", newline="") as f:
        w = csv.writer(f, delimiter="\t", lineterminator="\n")
        w.writerow(["path", "bytes", "sha256", "url"])
        w.writerows(rows)
    print(f"{len(rows)} files in manifest, {len(unknown)} without a known source:")
    for u in unknown:
        print("  ", u)

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--only")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--build", action="store_true")
    a = ap.parse_args()
    if a.build:
        return build()
    if not os.path.exists(MANIFEST):
        sys.exit("assets/manifest.tsv missing")
    return fetch(a)

if __name__ == "__main__":
    sys.exit(main() or 0)
