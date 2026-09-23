#!/usr/bin/env python3
"""
fetch_boss.py — bake the SDSS BOSS / eBOSS redshift surveys into the compact
"SurveyBin" the renderer reads (assets/catalogs/sdss_galaxies.bin).

Why this exists
---------------
Past the Local Group, every galaxy in the simulator was procedural. BOSS and
eBOSS measured the actual 3D positions of ~2 million galaxies and quasars out to
z ~ 2.2: the real cosmic web -- walls, filaments and voids -- over a quarter of
the sky. This bakes that into world coordinates so the engine can place real
galaxies where the survey saw them and keep generating the rest, exactly the
two-tier arrangement the stars use (real catalog + procedural complement,
reconciled by a selection function rather than a distance crossfade).

Inputs (assets/catalogs/sdss/, gitignored; urls.txt there lists them)
----------------------------------------------------------------------
  BOSS DR12 LSS  galaxy_DR12v5_CMASSLOWZTOT_{North,South}.fits.gz
                 CMASS + LOWZ, luminous red galaxies, 0.2 < z < 0.75
                 (Reid et al. 2016, MNRAS 455, 1553)
  eBOSS DR16 LSS eBOSS_{LRG,ELG,QSO}_clustering_data-{NGC,SGC}-vDR16.fits
                 LRG 0.6 < z < 1.0 (eBOSS's own LRGs; the combined
                 "LRGpCMASS" file is not used, and the ~2.3k targets both
                 surveys observed are removed here as duplicates),
                 ELG 0.6 < z < 1.1, QSO 0.8 < z < 2.2
                 (Ross et al. 2020, MNRAS 498, 2354; Raichoor et al. 2021)
  Base URLs: https://data.sdss.org/sas/dr12/boss/lss/
             https://data.sdss.org/sas/dr16/eboss/lss/catalogs/DR16/

Distance
--------
Comoving distance from redshift, flat LCDM with Planck 2015 (H0 = 67.74,
Om = 0.3089; Planck Collaboration 2016, A&A 594, A13, table 4 TT,TE,EE+lowP+
lensing+ext), radiation neglected (< 0.02% at z < 2.2). c/H0 = 14.43 Gly, the
Hubble distance the procedural generator already assumes.

Direction: RA/Dec (ICRS ~ J2000 equatorial) -> ecliptic -> the simulator's GL
axes (x = ecl x, y = ecl z, z = ecl y), the frame core/catalog.c and
tools/fetch_dustmap.py use.

Brightness
----------
The clustering catalogs carry position and redshift, not photometry. Each
object gets its tracer's typical absolute magnitude in the engine's galaxy
model (galaxy.c: M = -20.9 - 5 log10(r / 50 kly)), with +/-0.35 mag of
deterministic scatter: massive ellipticals for LOWZ / CMASS / LRG, star-forming
discs for ELG, luminous hosts for QSO (the AGN itself is not modelled here).

Selection function
------------------
The engine drops a procedural galaxy the survey would have seen from the Sun:
inside the footprint (a 1-degree RA/Dec occupancy mask built from the data),
inside the survey's comoving shell, and brighter than the survey limit. What
survives is what the survey missed, so real and procedural never double up.

Output: SurveyBin v1, little-endian
-----------------------------------
  header   uint32 magic 'OMVS' (0x53564D4F), version 1, count,
           mask_w (360), mask_h (180)
           float  dmin_ly, dmax_ly   comoving shell the survey covers
           float  m_lim              apparent limit in the engine's band
  mask     mask_w * mask_h bytes, row = Dec bin (-90..90), col = RA bin
           (0..360), 1 = observed
  records  count x { float pos_ly[3]; float z; float absmag;
                     uint8 tracer; uint8 pad[3] }        (24 bytes)
  tracer   0 LOWZ/CMASS, 1 LRG, 2 ELG, 3 QSO

Usage
-----
  python3 tools/fetch_boss.py --probe      # list each file's columns
  python3 tools/fetch_boss.py              # bake
  python3 tools/fetch_boss.py --preview survey.pgm   # a slab through it

Deps: numpy.
"""

import argparse
import gzip
import math
import os
import re
import struct
import sys

try:
    import numpy as np
except ImportError:
    sys.exit("fetch_boss.py needs numpy")

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIR = os.path.join(REPO, "assets", "catalogs", "sdss")
OUT_PATH = os.path.join(REPO, "assets", "catalogs", "sdss_galaxies.bin")

SURVEYBIN_MAGIC = 0x53564D4F        # 'OMVS' little-endian
SURVEYBIN_VERSION = 1
MASK_W, MASK_H = 360, 180

# Planck 2015 (TT,TE,EE+lowP+lensing+ext), flat.
H0 = 67.74
OM = 0.3089
C_KMS = 299792.458
MPC_LY = 3.2615638e6

LY_PER_AU = 1.0 / 63241.077

# (file stem, tracer id, z_min, z_max, typical M)
SOURCES = [
    ("galaxy_DR12v5_CMASSLOWZTOT_North.fits.gz", 0, 0.20, 0.75, -22.0),
    ("galaxy_DR12v5_CMASSLOWZTOT_South.fits.gz", 0, 0.20, 0.75, -22.0),
    ("eBOSS_LRG_clustering_data-NGC-vDR16.fits", 1, 0.60, 1.00, -22.2),
    ("eBOSS_LRG_clustering_data-SGC-vDR16.fits", 1, 0.60, 1.00, -22.2),
    ("eBOSS_ELG_clustering_data-NGC-vDR16.fits", 2, 0.60, 1.10, -20.8),
    ("eBOSS_ELG_clustering_data-SGC-vDR16.fits", 2, 0.60, 1.10, -20.8),
    ("eBOSS_QSO_clustering_data-NGC-vDR16.fits", 3, 0.80, 2.20, -22.5),
    ("eBOSS_QSO_clustering_data-SGC-vDR16.fits", 3, 0.80, 2.20, -22.5),
]
TRACER_NAMES = ["LOWZ/CMASS", "eBOSS LRG", "eBOSS ELG", "eBOSS QSO"]

# CMASS is i < 19.9: the faintest thing the survey promises to have seen.
SURVEY_M_LIM = 19.9
MAG_SCATTER = 0.35

J2000_EPS = math.radians(23.4392911)


# ------------------------------------------------------------ FITS tables
#
# FITS is 2880-byte blocks of 80-character header cards ending in END, then
# the data unit padded to the next 2880 boundary. A BINTABLE's rows are
# NAXIS1 bytes, NAXIS2 of them; each column's layout comes from TFORMn.
# Data is big-endian.

def _cards(buf, off):
    """Parse header blocks starting at off; return (header, data_offset)."""
    hdr = {}
    while True:
        block = buf[off:off + 2880].decode("ascii", "replace")
        off += 2880
        for i in range(0, 2880, 80):
            card = block[i:i + 80]
            key = card[:8].strip()
            if key == "END":
                return hdr, off
            if card[8:10] != "= ":
                continue
            val = card[10:].split("/")[0].strip() if "'" not in card[10:12] \
                else card[10:].split("'")[1].strip()
            if isinstance(val, str) and not card[10:].strip().startswith("'"):
                try:
                    val = int(val)
                except ValueError:
                    try:
                        val = float(val)
                    except ValueError:
                        pass
            hdr[key] = val
        if off >= len(buf):
            raise ValueError("FITS header has no END card")


_TFORM = re.compile(r"^\s*(\d*)([LXBIJKAEDCMPQ])")
_SIZE = {"L": 1, "B": 1, "I": 2, "J": 4, "K": 8, "A": 1, "E": 4, "D": 8,
         "C": 8, "M": 16, "P": 8, "Q": 16}
_NP = {"B": "u1", "I": ">i2", "J": ">i4", "K": ">i8", "E": ">f4", "D": ">f8"}


def read_table(path):
    """Return (column names, a structured array view of the first BINTABLE)."""
    if path.endswith(".gz"):
        with gzip.open(path, "rb") as f:
            buf = f.read()
    else:
        with open(path, "rb") as f:
            buf = f.read()
    off = 0
    while off < len(buf):
        hdr, data_off = _cards(buf, off)
        nbytes = abs(hdr.get("BITPIX", 8)) // 8
        for a in range(1, hdr.get("NAXIS", 0) + 1):
            nbytes *= hdr.get(f"NAXIS{a}", 0)
        if hdr.get("NAXIS", 0) == 0:
            nbytes = 0
        nbytes += hdr.get("PCOUNT", 0)
        if hdr.get("XTENSION") == "BINTABLE":
            return _bintable(buf, hdr, data_off)
        off = data_off + (nbytes + 2879) // 2880 * 2880
    raise ValueError(f"{path}: no BINTABLE extension")


def _bintable(buf, hdr, data_off):
    rowlen, nrows, nf = hdr["NAXIS1"], hdr["NAXIS2"], hdr["TFIELDS"]
    names, formats, offsets = [], [], []
    col = 0
    for i in range(1, nf + 1):
        name = str(hdr.get(f"TTYPE{i}", f"COL{i}")).upper()
        m = _TFORM.match(str(hdr[f"TFORM{i}"]))
        if not m:
            raise ValueError(f"bad TFORM{i}: {hdr[f'TFORM{i}']}")
        rep = int(m.group(1)) if m.group(1) else 1
        code = m.group(2)
        width = (rep + 7) // 8 if code == "X" else rep * _SIZE[code]
        if code in _NP and rep == 1 and name not in names:
            names.append(name)
            formats.append(_NP[code])
            offsets.append(col)
        col += width
    if col != rowlen:
        raise ValueError(f"TFORMs sum to {col} bytes, NAXIS1 says {rowlen}")
    dt = np.dtype({"names": names, "formats": formats, "offsets": offsets,
                   "itemsize": rowlen})
    table = np.frombuffer(buf, dtype=dt, count=nrows, offset=data_off)
    return names, table


# ------------------------------------------------------------ geometry

def comoving_ly(z):
    """Comoving distance in light-years, flat LCDM (see header)."""
    zg = np.linspace(0.0, max(3.0, float(np.max(z)) * 1.05), 30001)
    inv_e = 1.0 / np.sqrt(OM * (1.0 + zg) ** 3 + (1.0 - OM))
    cum = np.concatenate(([0.0], np.cumsum(0.5 * (inv_e[1:] + inv_e[:-1])
                                            * np.diff(zg))))
    return np.interp(z, zg, cum) * (C_KMS / H0) * MPC_LY


def radec_to_gl(ra_deg, dec_deg):
    """Unit vectors in the simulator's GL axes (see header), shape (n, 3)."""
    ra, dec = np.radians(ra_deg), np.radians(dec_deg)
    ex = np.cos(dec) * np.cos(ra)
    ey = np.cos(dec) * np.sin(ra)
    ez = np.sin(dec)
    ce, se = math.cos(J2000_EPS), math.sin(J2000_EPS)
    ly = ce * ey + se * ez          # equatorial -> ecliptic (about x)
    lz = -se * ey + ce * ez
    return np.stack([ex, lz, ly], axis=1)   # GL: x, y = ecl z, z = ecl y


def scatter(n, salt):
    """Deterministic +/-1 noise per row (a hash, so a rebake is identical)."""
    i = np.arange(n, dtype=np.uint64) + np.uint64(salt * 1000003)
    i ^= i >> np.uint64(33)
    i *= np.uint64(0xff51afd7ed558ccd)
    i ^= i >> np.uint64(33)
    i *= np.uint64(0xc4ceb9fe1a85ec53)
    i ^= i >> np.uint64(33)
    return (i >> np.uint64(11)).astype(np.float64) / float(1 << 53) * 2.0 - 1.0


# ------------------------------------------------------------ bake

def load_all():
    rows = []
    for k, (fname, tracer, zmin, zmax, mabs) in enumerate(SOURCES):
        path = os.path.join(SRC_DIR, fname)
        if not os.path.exists(path):
            sys.exit(f"[survey] missing {path} -- see {SRC_DIR}/urls.txt")
        names, t = read_table(path)
        for need in ("RA", "DEC", "Z"):
            if need not in names:
                sys.exit(f"[survey] {fname}: no {need} column (have {names[:12]}...)")
        ra = t["RA"].astype(np.float64)
        dec = t["DEC"].astype(np.float64)
        z = t["Z"].astype(np.float64)
        keep = np.isfinite(z) & (z > zmin) & (z < zmax)
        print(f"[survey] {fname}: {len(z)} rows, {int(keep.sum())} in "
              f"{zmin} < z < {zmax}")
        rows.append((ra[keep], dec[keep], z[keep], tracer, mabs, k))
    return rows


def dedupe(rows):
    """Drop eBOSS LRG rows BOSS already observed (same position within
    1 arcsec and |dz| < 0.002): ~2.3k of 175k."""
    key = {}
    for ra, dec, z, tracer, _, _ in rows:
        if tracer != 0:
            continue
        qa = np.rint(ra * 3600.0).astype(np.int64)
        qd = np.rint(dec * 3600.0).astype(np.int64)
        for a, d, zz in zip(qa.tolist(), qd.tolist(), z.tolist()):
            key[(a, d)] = zz
    out = []
    for ra, dec, z, tracer, mabs, k in rows:
        if tracer == 1 and key:
            qa = np.rint(ra * 3600.0).astype(np.int64)
            qd = np.rint(dec * 3600.0).astype(np.int64)
            dup = np.zeros(len(z), dtype=bool)
            for i, (a, d, zz) in enumerate(zip(qa.tolist(), qd.tolist(), z.tolist())):
                for da in (-1, 0, 1):
                    for dd in (-1, 0, 1):
                        z0 = key.get((a + da, d + dd))
                        if z0 is not None and abs(z0 - zz) < 0.002:
                            dup[i] = True
            print(f"[survey] {SOURCES[k][0]}: {int(dup.sum())} duplicates of BOSS CMASS removed")
            ra, dec, z = ra[~dup], dec[~dup], z[~dup]
        out.append((ra, dec, z, tracer, mabs, k))
    return out


def bake(preview):
    rows = dedupe(load_all())
    ra = np.concatenate([r[0] for r in rows])
    dec = np.concatenate([r[1] for r in rows])
    z = np.concatenate([r[2] for r in rows])
    tracer = np.concatenate([np.full(len(r[2]), r[3], np.uint8) for r in rows])
    absmag = np.concatenate([r[4] + MAG_SCATTER * scatter(len(r[2]), r[5])
                             for r in rows])

    d = comoving_ly(z)
    pos = radec_to_gl(ra, dec) * d[:, None]

    # Footprint: which 1-degree RA/Dec cells the survey observed at all.
    mask = np.zeros((MASK_H, MASK_W), dtype=np.uint8)
    ci = np.clip(np.floor(np.mod(ra, 360.0)).astype(int), 0, MASK_W - 1)
    cj = np.clip(np.floor(dec + 90.0).astype(int), 0, MASK_H - 1)
    mask[cj, ci] = 1
    # Solid-angle weighted coverage.
    lat = np.radians(np.arange(MASK_H) + 0.5 - 90.0)
    w = np.cos(lat)[:, None] * np.ones((1, MASK_W))
    cover = float((mask * w).sum() / w.sum())

    dmin, dmax = float(d.min()), float(d.max())
    n = len(z)
    rec = np.zeros(n, dtype=np.dtype([("pos", "<f4", 3), ("z", "<f4"),
                                      ("absmag", "<f4"), ("tracer", "u1"),
                                      ("pad", "u1", 3)]))
    rec["pos"] = pos.astype(np.float32)
    rec["z"] = z.astype(np.float32)
    rec["absmag"] = absmag.astype(np.float32)
    rec["tracer"] = tracer
    assert rec.dtype.itemsize == 24

    with open(OUT_PATH, "wb") as f:
        f.write(struct.pack("<IIIIIfff", SURVEYBIN_MAGIC, SURVEYBIN_VERSION, n,
                            MASK_W, MASK_H, dmin, dmax, SURVEY_M_LIM))
        f.write(mask.tobytes())
        f.write(rec.tobytes())

    print(f"[survey] {n} objects: " + ", ".join(
        f"{TRACER_NAMES[t]} {int((tracer == t).sum())}" for t in range(4)))
    print(f"[survey] comoving {dmin / 1e9:.2f} .. {dmax / 1e9:.2f} Gly, "
          f"footprint {cover * 100:.1f}% of the sky")
    print(f"[survey] wrote {OUT_PATH} ({os.path.getsize(OUT_PATH) / 1e6:.1f} MB)")

    if preview:
        write_preview(pos, preview)


def write_preview(pos, path, size=1024, slab_gly=0.3):
    """A slab through the survey, face-on to its densest direction: if the
    distances and frame are right, the cosmic web shows as filaments and
    voids; if they are wrong it comes out as smooth noise."""
    c = np.median(pos, axis=0)
    ax = c / np.linalg.norm(c)
    u = np.cross(ax, [0.0, 1.0, 0.0]); u /= np.linalg.norm(u)
    v = np.cross(ax, u)
    rel = pos - c
    sel = np.abs(rel @ ax) < slab_gly * 1e9
    x, y = rel[sel] @ u, rel[sel] @ v
    span = np.percentile(np.abs(np.concatenate([x, y])), 99)
    img, _, _ = np.histogram2d(y, x, bins=size, range=[[-span, span], [-span, span]])
    img = np.log1p(img)
    img = (255.0 * img / max(img.max(), 1e-9)).astype(np.uint8)
    with open(path, "wb") as f:
        f.write(f"P5 {size} {size} 255\n".encode())
        f.write(img[::-1].tobytes())
    print(f"[survey] preview {path}: {int(sel.sum())} objects in a "
          f"{2 * slab_gly:.1f} Gly slab, {2 * span / 1e9:.1f} Gly across")


def probe():
    for fname, *_ in SOURCES:
        path = os.path.join(SRC_DIR, fname)
        if not os.path.exists(path):
            print(f"{fname}: missing")
            continue
        names, t = read_table(path)
        print(f"{fname}: {len(t)} rows; columns: {', '.join(names)}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--probe", action="store_true", help="list columns and exit")
    ap.add_argument("--preview", metavar="PGM", help="write a slab image")
    a = ap.parse_args()
    if a.probe:
        probe()
    else:
        bake(a.preview)


if __name__ == "__main__":
    main()
