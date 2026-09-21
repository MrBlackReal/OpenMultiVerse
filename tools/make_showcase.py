#!/usr/bin/env python3
"""
make_showcase.py — generate assets/shots/showcase.json, the simulator's
reference showcase film (CINEMATIC.md §13.1).

    python3 tools/make_showcase.py            # writes assets/shots/showcase.json

One continuous ~90 s camera move, no hard cuts, outward from Earth:

    Earth -> Saturn -> the Sun and Solar System -> Alpha Centauri
          -> the Lagoon nebula -> Sagittarius A* -> the Milky Way, face-on

Why a generator instead of a hand-written JSON: every key is derived from real
catalogue coordinates in the simulator's own frame (the same equatorial -> GL
conversion as core/catalog.c), and transits between subjects cross up to four
orders of magnitude of scale per leg — numbers nobody wants to type by hand.
Retime or retarget by editing the SUBJECT and TIMELINE sections below.

Pacing rules (the brief: cinematic, polished, never fast enough to make anyone
motion sick):
  * every subject gets a hold of ~6-8 s with its own slow move;
  * transits are 6-8 s, eased in and out at the subjects, and spaced
    geometrically in distance so speed changes smoothly across scales;
  * the camera looks at where it is going (or back at what it is leaving),
    never whips: consecutive look targets are at most ~90 degrees apart;
  * the path is a centripetal Catmull-Rom spline (cinema_cam.c), which cannot
    loop or overshoot at a change of scale.
"""
import json
import math
import os

# ------------------------------------------------------------------ frame
AU_PER_LY = 9.461e15 / 1.496e11          # core/common.h LY / AU
J2000_EPS = 23.4392911                    # core/catalog.c

def eq_to_gl(ra_deg, dec_deg, dist_ly):
    """Equatorial J2000 -> simulator GL frame, in AU (catalog.c)."""
    d = math.pi / 180.0
    eps = J2000_EPS * d
    ra, dec = ra_deg * d, dec_deg * d
    ce, se = math.cos(eps), math.sin(eps)
    xe = math.cos(dec) * math.cos(ra)
    ye = math.cos(dec) * math.sin(ra)
    ze = math.sin(dec)
    x_ecl, y_ecl, z_ecl = xe, ye * ce + ze * se, -ye * se + ze * ce
    r = dist_ly * AU_PER_LY
    return [x_ecl * r, z_ecl * r, y_ecl * r]          # GL: x, y=ecl z, z=ecl y

def add(*vs):  return [sum(c) for c in zip(*vs)]
def sub(a, b): return [x - y for x, y in zip(a, b)]
def mul(v, s): return [x * s for x in v]
def norm(v):   return math.sqrt(sum(x * x for x in v))
def unit(v):   n = norm(v); return [x / n for x in v]
def cross(a, b):
    return [a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]]

def yaw_pitch(frm, to):
    d = unit(sub(to, frm))
    return (math.degrees(math.atan2(d[2], d[0])),
            math.degrees(math.asin(max(-1.0, min(1.0, d[1])))))

def basis(forward):
    """(forward, side, up) with `up` near ecliptic north."""
    f = unit(forward)
    side = unit(cross([0.0, 1.0, 0.0], f))
    up = cross(f, side)
    return f, side, up

# ------------------------------------------------------------------ subjects
# Planet positions at the start of the film (after the 2-year warm-up), from
# the simulator's own log. Near keys are ANCHORED to the body, so these only
# steer the transits and the lighting directions; the planets barely move at
# the film's slow timescales.
SUN    = [0.0, 0.0, 0.0]
EARTH  = [-0.137945, -0.000133, 0.970066]
SATURN = [2.741115, -0.258953, 8.618133]
R_EARTH  = 6371e3 / 1.496e11             # AU
R_SATURN = 58232e3 / 1.496e11

ACEN   = eq_to_gl(219.9021, -60.8340, 4.344)          # Alpha Cen A
LAGOON = eq_to_gl(270.92, -24.38, 4100.0)             # nebula.c table
LAGOON_R = 4100.0 * AU_PER_LY * math.radians(90.0 / 60.0 / 2.0)
SGRA   = [x * AU_PER_LY for x in (-1457.805894, -2606.474905, -26505.352053)]
POLE   = unit(eq_to_gl(192.859, 27.128, 1.0))         # galactic north pole

# ------------------------------------------------------------------ keys
keys = []

def key(t, *, pos=None, anchor=None, offset=None, look_at=None, look_pos=None,
        subject=None, fov=45.0, aperture=0.0, focus=None, timescale=1.0,
        shutter=180.0, ease="linear"):
    """One shot key. Transit keys aim with look_pos (a fixed direction) rather
    than look_at, so the title card only appears once a subject key is reached
    — not while the destination is still a dot. `subject` names static scenery
    (a nebula or galaxy) for the title card."""
    k = {"t": round(t, 3)}
    if anchor:
        k["anchor"] = anchor
        k["offset"] = [float("%.10g" % x) for x in offset]
    else:
        k["pos"] = [float("%.10g" % x) for x in pos]
    if look_at:
        k["look_at"] = look_at
    else:
        y, p = yaw_pitch(pos, look_pos)
        k["yaw"], k["pitch"] = round(y, 4), round(p, 4)
    if subject:
        k["subject"] = subject
    k["fov"] = fov
    k["aperture"] = aperture          # explicit: 0 = depth of field off
    if focus:
        k["focus"] = focus
    k["timescale"] = timescale
    k["shutter"] = shutter
    k["ease"] = ease
    keys.append(k)

# ---- 0-11 s  EARTH: slow push around the day side, terminator in view ----
s_dir = unit(sub(SUN, EARTH))                         # toward the Sun
f, side, up = basis(s_dir)
def earth_off(a_sun, a_side, a_up, radii):
    return mul(unit(add(mul(f, a_sun), mul(side, a_side), mul(up, a_up))),
               radii * R_EARTH)
TS_EARTH = 0.02          # ~0.5 h per second: clouds drift, Earth turns gently
key(0.0,  anchor="Earth", offset=earth_off(0.80, 0.55, 0.20, 5.2),
    look_at="Earth", fov=32, aperture=2.8, focus="Earth",
    timescale=TS_EARTH, ease="inout")
key(5.5,  anchor="Earth", offset=earth_off(0.55, 0.80, 0.25, 4.6),
    look_at="Earth", fov=32, aperture=2.8, focus="Earth",
    timescale=TS_EARTH, ease="inout")
key(11.0, anchor="Earth", offset=earth_off(0.30, 0.90, 0.35, 30.0),
    look_at="Earth", fov=36, aperture=5.6, focus="Earth",
    timescale=TS_EARTH, ease="in")

# ---- 11-19 s  transit to SATURN, turning from Earth to face it ----------
to_sat = unit(sub(SATURN, EARTH))
p1 = add(EARTH, mul(to_sat, 0.02), mul(up, 0.006))
# Face Saturn from the start. (Looking back at Earth here looks sunward: its
# night side, a dark disc lost in the Sun's glare.)
key(13.5, pos=p1, look_pos=SATURN, fov=40, timescale=0.05, shutter=120)
p2 = add(EARTH, mul(to_sat, 0.9), mul(up, 0.08))
key(16.0, pos=p2, look_pos=SATURN, fov=40, timescale=0.05, shutter=90)
f_s, side_s, up_s = basis(unit(sub(SUN, SATURN)))
def sat_off(a_sun, a_side, a_up, radii):
    return mul(unit(add(mul(f_s, a_sun), mul(side_s, a_side), mul(up_s, a_up))),
               radii * R_SATURN)
key(18.5, anchor="Saturn", offset=add(mul(to_sat, -0.03), sat_off(0.6, 0.3, 0.2, 40)),
    look_at="Saturn", fov=38, timescale=0.05, shutter=120)

# ---- 19-28 s  SATURN: slow orbit from the sunlit side round to backlit ----
TS_SAT = 0.03
key(21.5, anchor="Saturn", offset=sat_off(0.75, 0.45, 0.30, 26.0),
    look_at="Saturn", fov=34, aperture=4.0, focus="Saturn",
    timescale=TS_SAT, ease="inout")
key(25.0, anchor="Saturn", offset=sat_off(0.05, 0.95, 0.35, 23.0),
    look_at="Saturn", fov=34, aperture=4.0, focus="Saturn",
    timescale=TS_SAT, ease="inout")
# Backlit: the camera on the far side, the Sun just behind Saturn's limb.
key(28.0, anchor="Saturn", offset=sat_off(-0.85, 0.25, 0.20, 26.0),
    look_at="Saturn", fov=36, aperture=0.0, timescale=TS_SAT, ease="inout")

# ---- 28-36.5 s  pull straight back along the Sun line: no turn ----------
# Saturn shrinks in front of the Sun over ~6 s, then the whole system does.
key(31.0, pos=add(SATURN, mul(f_s, -0.12), mul(up_s, 0.02)),
    look_at="Sun", fov=40, timescale=0.3, shutter=150)
key(34.0, pos=add(SATURN, mul(f_s, -1.5), mul(up_s, 0.5)),
    look_at="Sun", fov=45, timescale=1.0, shutter=150)
a_dir = unit(ACEN)
f_a, side_a, up_a = basis(a_dir)
key(36.5, pos=add(mul(a_dir, 60.0), mul(up_a, 50.0)),
    look_at="Sun", fov=48, timescale=1.0, shutter=120)

# ---- 36.5-41.5 s  interstellar: turn from the Sun toward Alpha Centauri ---
key(39.0, pos=add(mul(a_dir, 2.5e4), mul(up_a, 5e3)),
    look_pos=ACEN, fov=48, shutter=90)
back = mul(a_dir, -1.0)
key(41.5, anchor="Alpha Cen A", offset=add(mul(back, 1.8e3), mul(up_a, 400.0)),
    look_at="Alpha Cen A", fov=45, shutter=120)

# ---- 41.5-49 s  ALPHA CENTAURI: A and B in one frame, slow arc -----------
key(44.5, anchor="Alpha Cen A", offset=add(mul(back, 70.0), mul(side_a, 30.0), mul(up_a, 22.0)),
    look_at="Alpha Cen A", fov=42, ease="inout")
key(49.0, anchor="Alpha Cen A", offset=add(mul(back, 25.0), mul(side_a, 60.0), mul(up_a, 30.0)),
    look_at="Alpha Cen A", fov=42, ease="inout")

# ---- 49-55.5 s  transit toward Sagittarius and the Lagoon nebula ---------
d_l = unit(sub(LAGOON, ACEN))
f_l, side_l, up_l = basis(d_l)
q = add(ACEN, mul(d_l, 3e3), mul(up_l, 1e3))
key(51.5, pos=q, look_pos=LAGOON, fov=45, shutter=90)
q = add(ACEN, mul(d_l, 4e5))
key(53.5, pos=q, look_pos=LAGOON, fov=45, shutter=60)
q = sub(LAGOON, mul(d_l, 8e7))
key(55.5, pos=q, look_pos=LAGOON, fov=45, shutter=60)

# ---- 55.5-61.5 s  LAGOON: a close pass along its face --------------------
q = add(LAGOON, mul(d_l, -2.2 * LAGOON_R * 0.9), mul(side_l, 0.9 * LAGOON_R))
key(58.0, pos=q, look_pos=LAGOON, subject="Lagoon (M8)", fov=45,
    shutter=150, ease="inout")
q = add(LAGOON, mul(d_l, -1.4 * LAGOON_R), mul(side_l, 1.3 * LAGOON_R),
        mul(up_l, 0.4 * LAGOON_R))
key(61.5, pos=q, look_pos=LAGOON, subject="Lagoon (M8)", fov=45,
    shutter=150, ease="inout")

# ---- 61.5-66.5 s  on toward the galactic centre --------------------------
d_s = unit(sub(SGRA, LAGOON))
f_c, side_c, up_c = basis(d_s)
q = add(LAGOON, mul(side_l, 3.0 * LAGOON_R), mul(d_s, 3.0 * LAGOON_R))
# The galactic interior is featureless haze: cross it quickly (CINEMATIC.md
# §13.1) and spend the time on the black hole instead.
key(63.0, pos=q, look_pos=SGRA, fov=45, shutter=90)
q = sub(SGRA, mul(d_s, 2.5e8))
key(64.5, pos=q, look_pos=SGRA, fov=45, shutter=60)
key(66.5, anchor="Sagittarius A*", offset=add(mul(d_s, -2e5), mul(up_c, 4e4)),
    look_at="Sagittarius A*", fov=44, shutter=90)

# ---- 66.5-77 s  SAGITTARIUS A*: in to 2.6 AU, a slow arc round the disc ---
# The shadow and photon ring need the camera within a few AU (Rs ~0.08 AU).
key(69.0, anchor="Sagittarius A*", offset=add(mul(d_s, -300.0), mul(up_c, 90.0)),
    look_at="Sagittarius A*", fov=42, shutter=120)
key(71.5, anchor="Sagittarius A*", offset=add(mul(d_s, -2.1), mul(side_c, 0.9), mul(up_c, 1.2)),
    look_at="Sagittarius A*", fov=40, shutter=180, ease="inout")
key(77.0, anchor="Sagittarius A*", offset=add(mul(d_s, -1.2), mul(side_c, 2.0), mul(up_c, 0.8)),
    look_at="Sagittarius A*", fov=40, shutter=180, ease="inout")

# ---- 77-90 s  rise out of the disc; the Milky Way face-on ----------------
# Straight up the galactic pole from the centre, looking back down at it, so
# the reveal lands centred on the galaxy; then a long, slow hold.
key(79.5, anchor="Sagittarius A*", offset=add(mul(POLE, 3e3), mul(side_c, 40.0)),
    look_at="Sagittarius A*", fov=44, shutter=120)
key(82.0, anchor="Sagittarius A*", offset=add(mul(POLE, 3e7), mul(side_c, 2e6)),
    look_at="Sagittarius A*", fov=48, shutter=60)
key(84.5, anchor="Sagittarius A*", offset=add(mul(POLE, 3.2e9), mul(side_c, 4e8)),
    look_at="Sagittarius A*", subject="Milky Way", fov=50, shutter=120)
key(90.0, anchor="Sagittarius A*", offset=add(mul(POLE, 5.2e9), mul(side_c, 7e8)),
    look_at="Sagittarius A*", subject="Milky Way", fov=50, shutter=180, ease="out")

# ------------------------------------------------------------------ write
HEADER = """// Showcase — 90 s, the reference film for the cinematic renderer.
// GENERATED by tools/make_showcase.py — edit that, not this file.
//
//   ./verse --preset assets/universes/known_universe.json --cinematic \\
//           --output showcase.mp4 --shot-script assets/shots/showcase.json \\
//           --res 1920x1080 --fps 30 --samples 32 --letterbox --grain 0.02 \\
//           --cinematic-info --no-orbits --audio assets/soundtrack_1.ogg
//
// One continuous move, no cuts, outward from Earth: Earth -> Saturn -> the
// Solar System -> Alpha Centauri -> the Lagoon nebula -> Sagittarius A* ->
// the Milky Way face-on. Positions come from catalogue coordinates in the
// simulator's frame; see the generator for the pacing rules.
"""

def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, "..", "assets", "shots", "showcase.json")
    body = json.dumps({"name": "Showcase", "keys": keys}, indent=None)
    # One key per line, for diffs and hand inspection.
    body = body.replace('"keys": [', '"keys": [\n    ').replace("}, {", "},\n    {")
    body = body.replace("}]}", "}\n  ]\n}")
    with open(out, "w") as fh:
        fh.write(HEADER + body + "\n")
    print("wrote %s (%d keys, %.1f s)" % (os.path.normpath(out), len(keys), keys[-1]["t"]))

if __name__ == "__main__":
    main()
