#!/usr/bin/env python3
"""Every shape-model download on the PDS Small Bodies Node shape-model index
(https://sbn.psi.edu/pds/shape-models/): the archival originals (WRL, TAB,
ICQ, OBJ, ...) and SBN's derived OBJ / USDZ conversions. That index is a JS
app; its table lives in shape-models/js/app.Data.js, parsed here.
Writes sbn_links.tsv (body, type, dataset, role, format, url) and
sbn_urls.txt."""
import os, re, urllib.request

BASE = "https://sbn.psi.edu/pds/"
OUT  = os.path.dirname(os.path.abspath(__file__))
UA   = {"User-Agent": "Mozilla/5.0 (X11; Linux x86_64) OpenMultiVerse-tools"}

def get(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=60) as r:
        return r.read().decode("utf-8", "replace")

js = get(BASE + "shape-models/js/app.Data.js")
# Many links are written `Stooke.basepath + 'file.tab'`: resolve the dataset
# base URLs from app.Datasets.js (keys may carry a suffix, e.g. Hudson_radar).
dsjs = get(BASE + "shape-models/js/app.Datasets.js")
bases = dict(re.findall(r"""'?(\w+)'?\s*:\s*\{[^}]*?basepath\s*:\s*'([^']*)'""", dsjs))
def basepath(ident):
    for k, v in bases.items():
        if k == ident or k.startswith(ident + "_"): return v
    raise KeyError(ident)
def resolve(expr):
    """A JS string expression: 'a', X.basepath + 'a', 'X.basepath' + 'a', 'a' + 'b'."""
    out = ""
    for part in re.split(r"\s*\+\s*", expr.strip()):
        lit = part.strip("'")
        m = re.fullmatch(r"(\w+)\.basepath", lit)
        out += basepath(m.group(1)) if m else lit
    return out
tok = re.compile(r"""(?P<key>name|type|downloadLink|path|fileFormat|primary|derived|ios|default)\s*:\s*(?:(?P<val>'[^'\n]*'(?:\s*\+\s*'[^'\n]*')*|\w+\.basepath\s*\+\s*'[^'\n]*')|\[|\{)""")
rows, body, btype, dataset, role, pending = [], "", "", "", "", None
for m in tok.finditer(js):
    k, v = m.group("key"), m.group("val")
    if v is not None: v = resolve(v)
    if k == "name" and v is not None:
        # a body name is followed by `type:`; a dataset name by `link:`
        nxt = js[m.end():m.end() + 80]
        if re.match(r"\s*,\s*type\s*:", nxt): body = v
        else: dataset = v
    elif k == "type" and v is not None: btype = v
    elif k in ("primary", "derived"): role = k
    elif k == "ios": role = "preview-ar"
    elif k == "default": role = "preview"
    elif k in ("downloadLink", "path") and v is not None:
        pending = v if v.startswith("http") else BASE + v
    elif k == "fileFormat" and pending:
        rows.append((body, btype, dataset, role, v or "", pending)); pending = None

seen, uniq = set(), []
for r in rows:
    if r[5] not in seen: seen.add(r[5]); uniq.append(r)
with open(os.path.join(OUT, "sbn_links.tsv"), "w") as f:
    f.write("body\ttype\tdataset\trole\tformat\turl\n")
    for r in uniq: f.write("\t".join(r) + "\n")
with open(os.path.join(OUT, "sbn_urls.txt"), "w") as f:
    for r in uniq:
        if not r[3].startswith("preview"): f.write(r[5] + "\n")
print(f"{len(uniq)} links, {len({r[0] for r in uniq})} bodies")
