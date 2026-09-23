#!/usr/bin/env python3
"""HEAD-check the url column of a links TSV; adds status and bytes columns
in place. Usage: check_links.py FILE.tsv [threads]"""
import sys, urllib.request, concurrent.futures as cf

UA = {"User-Agent": "Mozilla/5.0 (X11; Linux x86_64) OpenMultiVerse-tools"}

def head(url):
    for method in ("HEAD", "GET"):          # some archives refuse HEAD
        try:
            req = urllib.request.Request(url, method=method, headers=dict(UA, Range="bytes=0-0"))
            with urllib.request.urlopen(req, timeout=60) as r:
                n = r.headers.get("Content-Range", "").rpartition("/")[2] or r.headers.get("Content-Length", "")
                return str(r.status), n
        except urllib.error.HTTPError as e:
            if method == "GET": return str(e.code), ""
        except Exception as e:
            if method == "GET": return type(e).__name__, ""
    return "?", ""

path = sys.argv[1]
lines = open(path).read().splitlines()
hdr = lines[0].split("\t")
hdr = [h for h in hdr if h not in ("status", "bytes")]
rows = [l.split("\t")[:len(hdr)] for l in lines[1:]]
ui = hdr.index("url")
with cf.ThreadPoolExecutor(int(sys.argv[2]) if len(sys.argv) > 2 else 8) as ex:
    res = list(ex.map(lambda r: head(r[ui]), rows))
with open(path, "w") as f:
    f.write("\t".join(hdr + ["status", "bytes"]) + "\n")
    for r, (s, n) in zip(rows, res): f.write("\t".join(r + [s, n]) + "\n")
bad = [(r[ui], s) for r, (s, _) in zip(rows, res) if s not in ("200", "206")]
print(f"{len(rows) - len(bad)}/{len(rows)} ok")
for u, s in bad: print(" ", s, u)
