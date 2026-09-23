#!/usr/bin/env python3
"""List every file under prograde-data's Solar System folder with its raw
download URL and a kind (model, texture, normal map, ...). Writes
prograde_links.tsv (kind, body, path, url) and prograde_urls.txt (one URL
per line, for wget -i / aria2c -i). check_links.py adds sizes."""
import json, sys, time, urllib.parse, urllib.request, os

PROJECT = "Dexter9313/prograde-data"
REF     = "master"
ROOT    = "solarsystem/systems/Solar System"
API     = "https://gitlab.com/api/v4/projects/" + urllib.parse.quote(PROJECT, safe="")
OUT     = os.path.dirname(os.path.abspath(__file__))

def get(url, tries=6):
    req = urllib.request.Request(url, headers={"User-Agent": "OpenMultiVerse-tools"})
    for t in range(tries):
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                return json.load(r), r.headers
        except (OSError, urllib.error.URLError) as e:   # resets, 429s
            if t == tries - 1: raise
            print(f"retry {t + 1}: {e}", file=sys.stderr)
            time.sleep(5 * 2 ** t)

def tree():
    page, files = 1, []
    while True:
        q = urllib.parse.urlencode({"path": ROOT, "ref": REF, "recursive": "true",
                                    "per_page": 100, "page": page})
        items, h = get(f"{API}/repository/tree?{q}")
        files += [i["path"] for i in items if i["type"] == "blob"]
        nxt = h.get("X-Next-Page")
        if not nxt: return files
        page = int(nxt)

def raw_url(path):
    return (f"https://gitlab.com/{PROJECT}/-/raw/{REF}/" +
            urllib.parse.quote(path) + "?inline=false")

def kind(path):
    name = path.lower().rsplit("/", 1)[-1]
    ext  = name.rsplit(".", 1)[-1] if "." in name else ""
    if name.startswith("license"): return "license"
    if "/debris/" in path: return "debris-data"
    if "/orbital-params/" in path: return "orbital-params"
    if ext == "blend": return "model-source"
    if ext in ("obj", "mtl", "ply", "fbx", "gltf", "glb", "3ds", "dae", "stl"): return "model"
    if ext in ("png", "jpg", "jpeg", "tif", "tiff", "dds", "ktx", "exr", "hdr", "bmp", "webp"):
        if any(t in name for t in ("normal", "nrm", "_norm", "bump")): return "normal-map"
        if any(t in name for t in ("height", "elev", "disp", "dem", "topo")): return "height-map"
        if any(t in name for t in ("spec", "rough", "metal", "gloss")): return "specular-map"
        if any(t in name for t in ("night", "light", "emiss", "city")): return "night-map"
        if "cloud" in name: return "cloud-map"
        if "ring" in name: return "ring-texture"
        return "diffuse-map" if "diffuse" in name else "texture"
    if ext in ("json", "txt", "csv", "dat", "xml", "ini", "cfg"): return "data"
    return ext or "other"

def body(path):
    rest = path[len(ROOT) + 1:].split("/")
    if len(rest) > 2: return rest[1]                       # images/Earth/normal.jpg
    return rest[-1].rsplit(".", 1)[0] if len(rest) == 2 else ""   # models/Deimos.ply

files = tree()
print(f"{len(files)} files", file=sys.stderr)
rows = [(kind(p), body(p), p[len(ROOT) + 1:], raw_url(p)) for p in files]
with open(os.path.join(OUT, "prograde_links.tsv"), "w") as f:
    f.write("kind\tbody\tpath\turl\n")
    for r in rows: f.write("\t".join(map(str, r)) + "\n")
with open(os.path.join(OUT, "prograde_urls.txt"), "w") as f:
    for r in rows: f.write(r[3] + "\n")
# The visual assets alone (no orbital tables, debris data or OS clutter).
ASSETS = ("model", "model-source", "diffuse-map", "texture", "normal-map", "height-map",
          "specular-map", "night-map", "cloud-map", "ring-texture", "license", "bin")
with open(os.path.join(OUT, "prograde_assets.tsv"), "w") as f:
    f.write("kind\tbody\tpath\turl\n")
    for r in rows:
        if r[0] in ASSETS: f.write("\t".join(r) + "\n")
with open(os.path.join(OUT, "prograde_asset_urls.txt"), "w") as f:
    for r in rows:
        if r[0] in ASSETS: f.write(r[3] + "\n")
