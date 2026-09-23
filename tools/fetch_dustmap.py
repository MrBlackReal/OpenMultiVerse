#!/usr/bin/env python3
"""
fetch_dustmap.py — download the Edenhofer+ (2023) 3D dust map and bake it into
the compact "DustBin" cube the renderer samples.

Why this exists
---------------
Every other galactic-dust look in a space renderer is painted: someone decides
where the dark lanes go.  This map is a *reconstruction* — a Gaussian-process
inversion of extinction toward 54 million Gaia stars — so feeding it to the
volumetric pass makes the dark lanes appear because the dust is actually there.
It also gives real reddening for free: a star behind a cloud dims and reddens by
its true integrated column, not by an artistic falloff.

Source
------
  Edenhofer, Zucker, Frank, et al. (2023), A&A 685, A82
  "A parsec-scale Galactic 3D dust map out to 1.25 kpc from the Sun"
  Data: https://doi.org/10.5281/zenodo.8187943

  mean_and_std_healpix.fits, 3.25 GB: HEALPix nside=256 (786,432 directions,
  14' voxels) x 516 log-spaced distance shells from 69 pc to 1250 pc, float32,
  posterior mean followed by posterior standard deviation.  We keep the mean.

  Units are extinction in the Zhang, Green & Rix (2023) system, per parsec.
  Integrating along a ray gives that system's extinction; the published
  extinction curve converts it to a magnitude at any wavelength.

What this writes
----------------
  assets/catalogs/dust_local.bin  — DustBin: a dim^3 Cartesian cube of dust
  density, resampled into the simulator's GL axes so the shader samples it with
  a plain 3D texture lookup and no frame conversion at runtime.

  The raw FITS stays local (gitignored); the baked cube ships, exactly like
  gaia_stars.bin.  At the default 256^3 it is ~16 MB and its voxels are ~9.8 pc,
  which matches the map's own resolution at the far edge and undersamples it
  near the Sun.  That is the prototype trade; a nested inner cube is the
  obvious follow-up.

Deps: numpy (for a 1.6 GB memmapped resample).  FITS parsing and the HEALPix
RING indexing are implemented here rather than pulling in astropy + healpy,
keeping this in line with the other stdlib-ish tools in this directory.

Usage:
  python3 tools/fetch_dustmap.py --probe         # dump the FITS structure
  python3 tools/fetch_dustmap.py                 # download (if needed) + bake
  python3 tools/fetch_dustmap.py --dim 320 --preview dust.ppm
"""

import argparse
import math
import os
import struct
import sys

try:
    import numpy as np
except ImportError:
    sys.exit("fetch_dustmap.py needs numpy (the FITS cube is 1.6 GB of float32)")

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FITS_PATH = os.path.join(REPO, "assets", "catalogs",
                         "edenhofer2023_mean_std_healpix.fits")
OUT_PATH = os.path.join(REPO, "assets", "catalogs", "dust_local.bin")
URL = ("https://zenodo.org/records/8187943/files/"
       "mean_and_std_healpix.fits?download=1")

NSIDE = 256
R_MIN_PC = 69.0
R_MAX_PC = 1250.0

DUSTBIN_MAGIC = 0x4F4D5644          # 'OMVD' little-endian
DUSTBIN_VERSION = 1


# ---------------------------------------------------------------- download

def download(path, url):
    """Resumable fetch via curl; the file is 3.25 GB."""
    if os.path.exists(path) and os.path.getsize(path) >= 3252715200:
        print(f"[dust] have {path} ({os.path.getsize(path)/1e9:.2f} GB)")
        return
    print(f"[dust] downloading {url}\n[dust]   -> {path} (3.25 GB, resumable)")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    rc = os.system(f'curl -fL -C - -o "{path}" "{url}"')
    if rc != 0:
        sys.exit(f"[dust] download failed (curl exit {rc >> 8})")


# ------------------------------------------------------------ FITS reading
#
# FITS is simple enough to read directly: 2880-byte blocks of 80-character
# header cards terminated by an END card, then the data unit padded to the next
# 2880 boundary.  Data is BIG-endian, hence the '>' dtypes.

def _parse_cards(block_text):
    out = {}
    for i in range(0, len(block_text), 80):
        card = block_text[i:i + 80]
        if not card.strip():
            continue
        key = card[:8].strip()
        if key == "END":
            return out, True
        if card[8:10] != "= ":
            continue
        val = card[10:].split("/")[0].strip()
        if val.startswith("'"):
            val = val.strip("'").strip()
        elif val in ("T", "F"):
            val = (val == "T")
        else:
            try:
                val = int(val)
            except ValueError:
                try:
                    val = float(val)
                except ValueError:
                    pass
        out[key] = val
    return out, False


def read_hdus(path):
    """Walk the file and return [(header, data_offset, data_nbytes), ...]."""
    hdus = []
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        off = 0
        while off < size:
            hdr = {}
            done = False
            while not done:
                block = f.read(2880)
                if len(block) < 2880:
                    return hdus
                cards, done = _parse_cards(block.decode("ascii", "replace"))
                hdr.update(cards)
                off += 2880
            naxis = hdr.get("NAXIS", 0)
            nelem = 1
            for a in range(1, naxis + 1):
                nelem *= hdr.get(f"NAXIS{a}", 0)
            nbytes = 0 if naxis == 0 else nelem * abs(hdr.get("BITPIX", 0)) // 8
            hdus.append((hdr, off, nbytes))
            off += (nbytes + 2879) // 2880 * 2880
            f.seek(off)
    return hdus


def probe(path):
    if not os.path.exists(path):
        sys.exit(f"[dust] {path} not present yet")
    print(f"[dust] {path}  ({os.path.getsize(path)/1e9:.3f} GB)")
    for i, (hdr, off, nbytes) in enumerate(read_hdus(path)):
        naxis = hdr.get("NAXIS", 0)
        dims = [hdr.get(f"NAXIS{a}") for a in range(1, naxis + 1)]
        print(f"\n  HDU {i}: BITPIX={hdr.get('BITPIX')} NAXIS={naxis} dims={dims}"
              f" data@{off} ({nbytes/1e9:.3f} GB)")
        for k, v in hdr.items():
            if k not in ("SIMPLE", "BITPIX", "NAXIS", "EXTEND") and \
               not k.startswith("NAXIS"):
                print(f"      {k:8s} = {v!r}")


# ------------------------------------------------------- HEALPix RING index

def ang2pix_ring(nside, theta, phi):
    """Vectorised HEALPix RING ang2pix (HEALPix primer, Gorski+ 2005).

    theta = colatitude in [0, pi], phi = longitude in [0, 2pi).
    """
    theta = np.asarray(theta, dtype=np.float64)
    phi = np.mod(np.asarray(phi, dtype=np.float64), 2.0 * math.pi)
    z = np.cos(theta)
    za = np.abs(z)
    tt = phi * (2.0 / math.pi)                      # in [0, 4)
    npix = 12 * nside * nside
    ncap = 2 * nside * (nside - 1)
    pix = np.empty(z.shape, dtype=np.int64)

    # --- equatorial belt: |z| <= 2/3 -------------------------------------
    eq = za <= 2.0 / 3.0
    if np.any(eq):
        tt_e, z_e = tt[eq], z[eq]
        temp1 = nside * (0.5 + tt_e)
        temp2 = nside * z_e * 0.75
        jp = np.floor(temp1 - temp2).astype(np.int64)   # ascending edge
        jm = np.floor(temp1 + temp2).astype(np.int64)   # descending edge
        ir = nside + 1 + jp - jm                        # ring, 1..2*nside+1
        kshift = 1 - (ir & 1)
        ip = (jp + jm - nside + kshift + 1) // 2
        ip = np.mod(ip, 4 * nside)
        pix[eq] = ncap + (ir - 1) * 4 * nside + ip

    # --- polar caps -------------------------------------------------------
    po = ~eq
    if np.any(po):
        tt_p, z_p, za_p = tt[po], z[po], za[po]
        tp = tt_p - np.floor(tt_p)
        tmp = nside * np.sqrt(np.maximum(3.0 * (1.0 - za_p), 0.0))
        jp = np.floor(tp * tmp).astype(np.int64)
        jm = np.floor((1.0 - tp) * tmp).astype(np.int64)
        ir = jp + jm + 1
        ip = np.floor(tt_p * ir).astype(np.int64)
        ip = np.mod(ip, 4 * ir)
        north = z_p > 0
        out = np.where(north,
                       2 * ir * (ir - 1) + ip,
                       npix - 2 * ir * (ir + 1) + ip)
        pix[po] = out

    return pix


# ------------------------------------------------------------ frame rotation
#
# The map is in Galactic (l, b).  The simulator's world axes are the "GL frame"
# (equatorial J2000 -> ecliptic, then GL x = ecl x, y = ecl z, z = ecl y; see
# nebula.c eq_to_gl and core/catalog.c).  Composing those once here means the
# baked cube is already in world axes and the shader needs no rotation.

J2000_EPS = math.radians(23.4392911)

# Galactic pole / centre in equatorial J2000 (IAU 1958, as used for l, b).
_NGP_RA = math.radians(192.85948)
_NGP_DEC = math.radians(27.12825)
_GC_RA = math.radians(266.40510)
_GC_DEC = math.radians(-28.936175)


def _eq_unit(ra_rad, dec_rad):
    return np.array([math.cos(dec_rad) * math.cos(ra_rad),
                     math.cos(dec_rad) * math.sin(ra_rad),
                     math.sin(dec_rad)], dtype=np.float64)


def galactic_to_gl_matrix():
    """3x3 mapping a unit vector from Galactic to the simulator's GL axes.

    Built from explicit basis directions rather than an Euler composition:
    the columns of gal2eq are just the equatorial unit vectors of the galactic
    basis, which is far harder to get subtly wrong than a ZYZ triple.
      x_g -> galactic centre (l=0, b=0)
      z_g -> north galactic pole (b=+90)
      y_g  = z_g x x_g  (right-handed, l=+90)
    Determinant is -1 by design: the GL frame swaps the ecliptic Y and Z axes,
    exactly as core/catalog.c and nebula.c do.
    """
    x_g = _eq_unit(_GC_RA, _GC_DEC)
    z_g = _eq_unit(_NGP_RA, _NGP_DEC)
    x_g = x_g - z_g * float(np.dot(x_g, z_g))       # orthonormalise
    x_g /= np.linalg.norm(x_g)
    y_g = np.cross(z_g, x_g)
    gal2eq = np.column_stack((x_g, y_g, z_g))
    # Equatorial -> ecliptic, then ecliptic -> GL (x, y=ecl z, z=ecl y).
    ce, se = math.cos(J2000_EPS), math.sin(J2000_EPS)
    eq2ecl = np.array([[1, 0, 0], [0, ce, se], [0, -se, ce]], dtype=np.float64)
    ecl2gl = np.array([[1, 0, 0], [0, 0, 1], [0, 1, 0]], dtype=np.float64)
    return ecl2gl @ eq2ecl @ gal2eq


# ------------------------------------------------------------------- bake

def shell_edges(nshell):
    """Log-spaced shell centres, 69 pc .. 1250 pc (Edenhofer+ 2023 sec. 2)."""
    return np.logspace(math.log10(R_MIN_PC), math.log10(R_MAX_PC), nshell)


def bake(fits_path, out_path, dim, half_pc, gamma, preview):
    hdus = read_hdus(fits_path)
    # The mean cube is the first HDU carrying a (shell, pixel) float array.
    pick = None
    for hdr, off, nbytes in hdus:
        if hdr.get("NAXIS", 0) >= 2 and hdr.get("BITPIX") == -32:
            pick = (hdr, off, nbytes)
            break
    if pick is None:
        sys.exit("[dust] no float32 image HDU found — run --probe")
    hdr, off, nbytes = pick
    naxis = hdr["NAXIS"]
    dims = [hdr[f"NAXIS{a}"] for a in range(1, naxis + 1)]
    # FITS NAXIS1 is fastest-varying; numpy wants slowest-first.
    shape = tuple(reversed(dims))
    npix_sky = 12 * NSIDE * NSIDE
    print(f"[dust] mean HDU dims(FITS)={dims} -> numpy shape {shape}")

    data = np.memmap(fits_path, dtype=">f4", mode="r", offset=off,
                     shape=shape)
    # Find which axis is the sky and which is distance.
    if shape[-1] == npix_sky:
        nshell = shape[-2] if len(shape) > 1 else 1
        sky_last = True
    elif shape[0] == npix_sky:
        nshell = shape[-1]
        sky_last = False
    else:
        sys.exit(f"[dust] neither axis is {npix_sky} HEALPix pixels: {shape}")
    print(f"[dust] nside={NSIDE} npix={npix_sky} shells={nshell} "
          f"(sky axis {'last' if sky_last else 'first'})")

    centres = shell_edges(nshell)
    log_r0, log_r1 = math.log(R_MIN_PC), math.log(R_MAX_PC)

    R = galactic_to_gl_matrix()
    Rinv = R.T                       # GL -> Galactic (rotation: inverse = T)

    out = np.zeros((dim, dim, dim), dtype=np.float32)
    step = 2.0 * half_pc / dim
    axis = (np.arange(dim, dtype=np.float64) + 0.5) * step - half_pc

    # Slab by slab in z to keep the working set small.
    gx, gy = np.meshgrid(axis, axis, indexing="ij")
    for k in range(dim):
        gz = axis[k]
        # GL -> Galactic
        px = Rinv[0, 0] * gx + Rinv[0, 1] * gy + Rinv[0, 2] * gz
        py = Rinv[1, 0] * gx + Rinv[1, 1] * gy + Rinv[1, 2] * gz
        pz = Rinv[2, 0] * gx + Rinv[2, 1] * gy + Rinv[2, 2] * gz
        r = np.sqrt(px * px + py * py + pz * pz)
        inside = (r >= R_MIN_PC) & (r <= R_MAX_PC)
        if not np.any(inside):
            continue
        ri = r[inside]
        theta = np.arccos(np.clip(pz[inside] / ri, -1.0, 1.0))
        phi = np.arctan2(py[inside], px[inside])
        pix = ang2pix_ring(NSIDE, theta, phi)
        # Nearest log-spaced shell.
        t = (np.log(ri) - log_r0) / (log_r1 - log_r0)
        si = np.clip(np.rint(t * (nshell - 1)).astype(np.int64), 0, nshell - 1)
        vals = data[si, pix] if sky_last else data[pix, si]
        slab = np.zeros((dim, dim), dtype=np.float32)
        slab[inside] = np.asarray(vals, dtype=np.float32)
        out[:, :, k] = slab
        if (k + 1) % 32 == 0:
            print(f"[dust]   slab {k+1}/{dim}")

    finite = out[np.isfinite(out)]
    vmax = float(np.percentile(finite[finite > 0], 99.9)) if finite.size else 1.0
    print(f"[dust] density: max={float(np.nanmax(out)):.6g} "
          f"p99.9={vmax:.6g} mean={float(np.nanmean(out)):.6g}")

    enc = np.clip(out / vmax, 0.0, 1.0) ** (1.0 / gamma)
    enc = np.rint(enc * 255.0).astype(np.uint8)

    with open(out_path, "wb") as f:
        f.write(struct.pack("<IIIIfff",
                            DUSTBIN_MAGIC, DUSTBIN_VERSION, dim,
                            0,               # format 0 = uint8 gamma-encoded
                            float(half_pc), float(vmax), float(gamma)))
        f.write(enc.tobytes(order="C"))
    print(f"[dust] wrote {out_path} ({os.path.getsize(out_path)/1e6:.1f} MB, "
          f"{dim}^3, {2*half_pc/dim:.2f} pc/voxel)")

    if preview:
        write_preview(out, preview)


def write_preview(cube, path):
    """Column density through the cube along each axis — the sanity check.

    If the HEALPix indexing or the frame rotation is wrong this comes out as
    noise; if it is right, the Galactic plane reads as a continuous dark band
    and the Local Bubble as a cavity around the origin.
    """
    dim = cube.shape[0]
    panels = [cube.sum(axis=a) for a in (0, 1, 2)]
    img = np.concatenate(panels, axis=1)
    img = img / max(float(img.max()), 1e-12)
    img = (np.clip(img, 0, 1) ** (1 / 2.2) * 255).astype(np.uint8)
    h, w = img.shape
    with open(path, "wb") as f:
        f.write(b"P5\n%d %d\n255\n" % (w, h))
        f.write(img.tobytes())
    print(f"[dust] preview -> {path} ({w}x{h}, three axis projections)")


# -------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fits", default=FITS_PATH)
    ap.add_argument("--out", default=OUT_PATH)
    ap.add_argument("--dim", type=int, default=256, help="cube resolution")
    ap.add_argument("--half", type=float, default=R_MAX_PC,
                    help="cube half-extent in pc (default = map radius)")
    ap.add_argument("--gamma", type=float, default=2.2,
                    help="encode gamma; >1 keeps low-density detail")
    ap.add_argument("--preview", default=None, help="write a PGM sanity image")
    ap.add_argument("--probe", action="store_true",
                    help="dump the FITS structure and exit")
    ap.add_argument("--no-download", action="store_true")
    a = ap.parse_args()

    if a.probe:
        probe(a.fits)
        return
    if not a.no_download:
        download(a.fits, URL)
    bake(a.fits, a.out, a.dim, a.half, a.gamma, a.preview)


if __name__ == "__main__":
    main()
