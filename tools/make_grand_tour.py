#!/usr/bin/env python3
"""
make_grand_tour.py — generate assets/shots/grand_tour.json, the long showcase:
everything the engine renders, in one continuous camera move, no cuts.

    python3 tools/make_grand_tour.py        # writes assets/shots/grand_tour.json

    Earth -> the Moon (a low pass over its terrain) -> Venus -> Mercury
          -> Mars -> Jupiter -> Saturn -> Uranus -> Neptune -> the Solar System
          -> Alpha Centauri -> the Lagoon nebula -> Sagittarius A*
          -> the Milky Way, face-on -> Andromeda and a star system inside it
          -> Centaurus A -> M87 -> the deep field

It follows make_showcase.py's pacing rules (a slow hold per subject, transits
eased and spaced geometrically in distance, no whip pans) and shares its frame
conventions; the Alpha Centauri -> Milky Way leg is the same move as there.

Positions: the planets are where the simulator puts them after the 2-year
warm-up (its own log); near keys are ANCHORED to their body, so these only
steer transits and lighting. The Andromeda system is procedural: it becomes
a real body when the camera slows within ~1 ly of it (starsys.c), and its
name is a pure function of its lattice cell, so the keys there are absolute
positions round the star's logged location and the name is the title card.
"""

import json
import math
import os

AU_PER_LY = 9.461e15 / 1.496e11  # core/common.h LY / AU
J2000_EPS = 23.4392911  # core/catalog.c
KM = 1.0 / 1.496e8  # AU per km


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
    return [x_ecl * r, z_ecl * r, y_ecl * r]


def zero():
    return [0.0, 0.0, 0.0]


def add(*vs):
    return [sum(c) for c in zip(*vs)]


def sub(a, b):
    return [x - y for x, y in zip(a, b)]


def mul(v, s):
    return [x * s for x in v]


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def norm(v):
    return math.sqrt(dot(v, v))


def unit(v):
    n = norm(v)
    return [x / n for x in v]


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def basis(forward):
    """(forward, side, up) with `up` near ecliptic north."""
    f = unit(forward)
    side = unit(cross([0.0, 1.0, 0.0], f))
    return f, side, cross(f, side)


def slerp(a, b, u):
    d = max(-1.0, min(1.0, dot(a, b)))
    th = math.acos(d)
    if th < 1e-6:
        return unit(lerp(a, b, u))
    sn = math.sin(th)
    return unit(add(mul(a, math.sin((1 - u) * th) / sn), mul(b, math.sin(u * th) / sn)))


def smoother(u):
    return u * u * u * (u * (6 * u - 15) + 10)


def lerp(a, b, t):
    return add(mul(a, 1.0 - t), mul(b, t))


# ------------------------------------------------------------------ subjects
# (name, position after warm-up in AU, radius in km) — from the simulator.
PLANETS = {
    "Sun": ([0.005590713, -0.000124145, -0.002792324], 696000.0),
    "Mercury": ([0.359218766, -0.044104858, -0.143661618], 2439.7),
    "Venus": ([0.047517388, -0.012474311, -0.728319831], 6051.8),
    "Earth": ([-0.137945192, -0.000132977, 0.970066463], 6371.0),
    "Moon": ([-0.139107549, -0.000029174, 0.967796186], 1737.4),
    "Mars": ([1.293622071, -0.019096888, 0.602823031], 3389.5),
    "Jupiter": ([-0.943100993, 0.000012448, 5.083897510], 69911.0),
    "Saturn": ([2.741114566, -0.258953069, 8.618132570], 58232.0),
    "Uranus": ([16.233580136, -0.253814015, -11.663063235], 25362.0),
    "Neptune": ([18.640360113, 0.057210374, -23.630307622], 24622.0),
}
SUN = PLANETS["Sun"][0]


def P(name):
    return PLANETS[name][0]


def R(name):
    return PLANETS[name][1] * KM


ACEN = eq_to_gl(219.9021, -60.8340, 4.344)
LAGOON = eq_to_gl(270.92, -24.38, 4100.0)
LAGOON_R = 4100.0 * AU_PER_LY * math.radians(90.0 / 60.0 / 2.0)
SGRA = [x * AU_PER_LY for x in (-1457.805894, -2606.474905, -26505.352053)]
POLE = unit(eq_to_gl(192.859, 27.128, 1.0))

M31 = eq_to_gl(10.68, 41.27, 2.54e6)
M31_R = 2.54e6 * AU_PER_LY * math.tan(math.radians(190.0 / 60.0 / 2.0))
# A 0.49 M_sun star with five planets in Andromeda's inner disc (probe run:
# "[StarSys] promoted 'OMV 613.309.747.2' ... in Andromeda (M31) at ...").
M31_SYS_NAME = "OMV 613.309.747.2"
M31_SYS = [118719005726.0, 88350316734.8, 62769070362.3]
CENA = eq_to_gl(201.37, -43.02, 1.20e7)
CENA_R = 1.20e7 * AU_PER_LY * math.tan(math.radians(26.0 / 60.0 / 2.0))
M87 = eq_to_gl(187.71, 12.39, 5.30e7)
M87_R = 5.30e7 * AU_PER_LY * math.tan(math.radians(8.0 / 60.0 / 2.0))

# ------------------------------------------------------------------ keys
# The script below authors sparse WAYPOINTS. resample() turns them into the
# dense keys the engine plays: one smooth path with continuous velocity, so the
# camera never lurches at a key or stops dead between them (the engine's own
# per-segment ease and spline give a speed jump at nearly every key).
APOS = {k: v[0] for k, v in PLANETS.items()}
APOS["Alpha Cen A"] = ACEN
APOS["Sagittarius A*"] = SGRA

WP = []
T = [0.0]  # running clock


def key(dt, *, pos=None, anchor=None, offset=None, look_at=None, look_pos=None,
        look_dir=None, subject=None, fov=45.0, aperture=0.0, focus=None,
        timescale=1.0, shutter=180.0, ease="linear"):
    """Append a waypoint `dt` seconds after the previous one. The view is
    look_at (a body, tracked), or a fixed direction: look_dir, or toward
    look_pos (for an anchored key, look_pos is relative to the anchor's listed
    position). `ease` is accepted for readability and ignored: motion between
    waypoints is continuous, holds are slow drifts rather than stops."""
    T[0] += dt
    w = dict(t=T[0], anchor=anchor, look_at=look_at, subject=subject, fov=fov,
             aperture=aperture, focus=focus, timescale=timescale, shutter=shutter,
             lookpt=None)
    if anchor:
        w["off"] = list(offset)
        w["here"] = add(APOS[anchor], offset)
    else:
        w["off"] = None
        w["here"] = list(pos)
    if look_at:
        w["lookpt"] = APOS[look_at]
        w["dir"] = unit(sub(APOS[look_at], w["here"]))
    elif look_dir:
        w["dir"] = unit(look_dir)
    else:
        w["lookpt"] = list(look_pos)
        w["dir"] = unit(sub(look_pos, w["here"]))
    WP.append(w)


def sun_off(name, a_sun, a_side, a_up, radii):
    """Offset from a body in its sun-facing frame, `radii` body radii out."""
    f, side, up = basis(sub(SUN, P(name)))
    return mul(unit(add(mul(f, a_sun), mul(side, a_side), mul(up, a_up))), radii * R(name))


# ================================================================== EARTH
# The opening of make_showcase.py: over Europe on the day side, the slow spin
# carrying it toward the terminator.
WARMUP_DAYS = 2.0 * 365.0
EARTH_DAY = 0.99727
EARTH_OBL = math.radians(23.44)
TS_EARTH = 0.005


def earth_site(lat_deg, lon_deg, t):
    a = 2.0 * math.pi * (WARMUP_DAYS + TS_EARTH * t) / EARTH_DAY
    la, lo = math.radians(lat_deg), -math.radians(lon_deg)
    lx, ly, lz = math.cos(la) * math.cos(lo), math.sin(la), math.cos(la) * math.sin(lo)
    tx = lx * math.cos(a) - lz * math.sin(a)
    tz = lx * math.sin(a) + lz * math.cos(a)
    co, so = math.cos(EARTH_OBL), math.sin(EARTH_OBL)
    return unit([co * tx + so * ly, -so * tx + co * ly, tz])


f_e, _, _ = basis(sub(SUN, P("Earth")))


def europe_off(t, sun_bias, radii):
    return mul(unit(add(earth_site(47.0, 10.0, t), mul(f_e, sun_bias))), radii * R("Earth"))


key(0.0, anchor="Earth", offset=europe_off(0.0, 0.35, 5.2), look_at="Earth", fov=32,
    aperture=2.8, focus="Earth", timescale=TS_EARTH, ease="inout")
key(5.5, anchor="Earth", offset=europe_off(5.5, 0.55, 4.6), look_at="Earth", fov=32,
    aperture=2.8, focus="Earth", timescale=TS_EARTH, ease="inout")
key(3.0, anchor="Earth", offset=sun_off("Earth", 0.4, 0.8, 0.45, 16.0), look_at="Earth",
    fov=36, timescale=TS_EARTH, ease="in")

# ================================================================== MOON
# Near-frozen clock so directions computed here stay true. The low pass runs
# along the terminator (sun ~12 degrees up, from the side: long shadows) over
# the Moon's ecliptic-north face, so the local vertical is near the film's
# up and the horizon stays level; 16 km above the mean radius clears the
# highest terrain (~10 km) everywhere.
TS_MOON = 1e-5
e_dir = unit(sub(P("Earth"), P("Moon")))
# Leave Earth toward the Moon: swing round to the far side of Earth, then fly
# past it (3 radii abreast, sunlit side) with the Moon dead ahead, so the
# camera never has to whip from Earth to the Moon.
m_hat = mul(e_dir, -1.0)
sv_e = unit(sub(SUN, P("Earth")))
lat_e = unit(sub(sv_e, mul(m_hat, dot(sv_e, m_hat))))
key(3.5, anchor="Earth", offset=mul(add(mul(m_hat, -14.0), mul(lat_e, 3.0)), R("Earth")),
    look_at="Earth", fov=34, timescale=TS_EARTH)
key(3.5, anchor="Earth", offset=mul(add(mul(m_hat, 10.0), mul(lat_e, 3.0)), R("Earth")),
    look_at="Moon", fov=38, timescale=TS_MOON, shutter=120)
key(2.5, anchor="Moon", offset=sun_off("Moon", 0.7, 0.4, 0.6, 9.0), look_at="Moon", fov=38,
    timescale=TS_MOON, shutter=150)
key(3.5, anchor="Moon", offset=sun_off("Moon", 0.35, 0.2, 0.9, 2.4), look_at="Moon", fov=40,
    timescale=TS_MOON, ease="inout")

f_m, side_m, up_m = basis(sub(SUN, P("Moon")))
n0 = unit(add(mul(f_m, 0.21), mul(up_m, 0.977)))
R_M = R("Moon")


def moon_site(theta, alt_km):
    n = unit(add(mul(n0, math.cos(theta)), mul(side_m, math.sin(theta))))
    return mul(n, R_M + alt_km * KM), n


def along(n, pitch_deg):
    """Heading along the pass (the tangent toward +side), pitched down."""
    t = unit(sub(side_m, mul(n, dot(side_m, n))))
    p = math.radians(pitch_deg)
    return add(mul(t, math.cos(p)), mul(n, math.sin(p)))


o, n = moon_site(-0.075, 90.0)
key(3.5, anchor="Moon", offset=o, look_dir=along(n, -14.0), fov=50, timescale=TS_MOON,
    shutter=150, ease="in")
o, n = moon_site(-0.045, 18.0)
key(3.0, anchor="Moon", offset=o, look_dir=along(n, -6.0), fov=55, timescale=TS_MOON, shutter=150)
o, n = moon_site(-0.015, 16.0)
key(3.5, anchor="Moon", offset=o, look_dir=along(n, -4.5), fov=55, timescale=TS_MOON, shutter=150)
o, n = moon_site(0.015, 17.0)
key(3.5, anchor="Moon", offset=o, look_dir=along(n, -5.0), fov=55, timescale=TS_MOON, shutter=150)
o, n = moon_site(0.05, 140.0)
key(3.0, anchor="Moon", offset=o, look_dir=along(n, -12.0), fov=50, timescale=TS_MOON,
    shutter=150, ease="out")


# ================================================================== PLANETS
TS_PL = 0.005


def depart_off(name, nxt, radii):
    """Where the camera leaves `name` for `nxt`: behind and beside the planet,
    on the sunlit side of the line to the destination, so the planet is
    already nearly ahead of the camera and the turn onto the transit is short
    (the camera then passes it at ~1.5 radii instead of whipping round)."""
    d = unit(sub(P(nxt), P(name)))
    sv = unit(sub(SUN, P(name)))
    lat = sub(sv, mul(d, dot(sv, d)))
    lat = unit(lat) if norm(lat) > 0.2 else [0.0, 1.0, 0.0]
    return mul(unit(add(mul(d, -3.5), mul(lat, 1.5))), radii * R(name))


def visit(name, hold, radii_far, radii_near, nxt=None, fov=36, side=0.45, up=0.3):
    """Arrive from the direction of travel, swing round the sunlit side, and
    leave from the departure bearing toward `nxt`."""
    arr = sun_off(name, 0.75, side, up, radii_near * 1.15)
    dep = depart_off(name, nxt, radii_near) if nxt else sun_off(name, 0.45, side + 0.45, up, radii_near)
    mid = mul(slerp(unit(arr), unit(dep), 0.5), radii_near * 1.1 * R(name))
    key(3.0, anchor=name, offset=sun_off(name, 0.8, side, up, radii_far), look_at=name,
        fov=fov + 4, timescale=TS_PL, shutter=120)
    key(hold * 0.4, anchor=name, offset=arr, look_at=name, fov=fov, timescale=TS_PL)
    key(hold * 0.3, anchor=name, offset=mid, look_at=name, fov=fov, timescale=TS_PL)
    key(hold * 0.3, anchor=name, offset=dep, look_at=name, fov=fov, timescale=TS_PL)
    return dep


def transit(frm, to, dep=None, lift=0.0, dts=(3.0, 2.5)):
    """Two free keys from one planet toward the next, looking ahead. The path
    leaves from the departure offset and passes the planet beside it."""
    a, b = P(frm), P(to)
    d = unit(sub(b, a))
    L = norm(sub(b, a))
    up = [0.0, 1.0, 0.0]
    lat = zero() if dep is None else sub(dep, mul(d, dot(dep, d)))
    p1 = add(a, lat, mul(d, 0.04 * L), mul(up, lift * 0.3 * L))
    key(dts[0], pos=p1, look_pos=b, fov=42, timescale=TS_PL, shutter=90)
    p2 = add(a, mul(d, 0.8 * L), mul(up, lift * 0.5 * L))
    key(dts[1], pos=p2, look_pos=b, fov=42, timescale=TS_PL, shutter=60)


# Moon -> Venus crosses the Sun: arc over it (lift) and see it pass below.
transit("Moon", "Venus", lift=0.45, dts=(3.0, 3.5))
d_ve = visit("Venus", 6.0, 40, 5.0, nxt="Mercury")
transit("Venus", "Mercury", d_ve)
d_me = visit("Mercury", 6.0, 40, 5.0, nxt="Mars")
transit("Mercury", "Mars", d_me, lift=0.1)
d_ma = visit("Mars", 6.5, 40, 4.2, nxt="Jupiter", side=0.5)
transit("Mars", "Jupiter", d_ma, lift=0.05)
d_ju = visit("Jupiter", 6.5, 30, 4.0, nxt="Saturn", fov=38)
transit("Jupiter", "Saturn", d_ju, lift=0.05)
# Saturn: from the sunlit side round to backlit (make_showcase.py's hold).
key(2.5, anchor="Saturn", offset=sun_off("Saturn", 0.6, 0.3, 0.2, 40), look_at="Saturn", fov=38,
    timescale=TS_PL, shutter=120)
key(3.0, anchor="Saturn", offset=sun_off("Saturn", 0.75, 0.45, 0.30, 26.0), look_at="Saturn",
    fov=34, aperture=4.0, focus="Saturn", timescale=TS_PL, ease="inout")
key(3.5, anchor="Saturn", offset=sun_off("Saturn", 0.05, 0.95, 0.35, 23.0), look_at="Saturn",
    fov=34, aperture=4.0, focus="Saturn", timescale=TS_PL, ease="inout")
key(3.0, anchor="Saturn", offset=sun_off("Saturn", -0.85, 0.25, 0.20, 26.0), look_at="Saturn",
    fov=36, timescale=TS_PL, ease="inout")
transit("Saturn", "Uranus", lift=0.05, dts=(3.0, 3.0))
d_ur = visit("Uranus", 6.0, 40, 5.0, nxt="Neptune")
transit("Uranus", "Neptune", d_ur, lift=0.05)
visit("Neptune", 6.0, 40, 5.0)

# ================================================================== SOLAR SYSTEM
# Rise out of the plane from Neptune, looking back at the Sun: the whole
# system, then on to Alpha Centauri as in make_showcase.py.
nep = P("Neptune")
key(3.0, pos=add(nep, mul(unit(sub(nep, SUN)), 2.0), [0.0, 3.0, 0.0]), look_at="Sun", fov=42,
    timescale=0.3, shutter=150)
key(3.0, pos=add(mul(unit(nep), 45.0), [0.0, 35.0, 0.0]), look_at="Sun", fov=46, timescale=1.0,
    shutter=150)
a_dir = unit(ACEN)
f_a, side_a, up_a = basis(a_dir)
key(3.0, pos=add(mul(a_dir, 60.0), mul(up_a, 50.0)), look_at="Sun", fov=48, shutter=120)

# ================================================================== ALPHA CEN -> MILKY WAY
# make_showcase.py 36.5-90 s, the same keys.
key(2.5, pos=add(mul(a_dir, 2.5e4), mul(up_a, 5e3)), look_pos=ACEN, fov=48, shutter=90)
back = mul(a_dir, -1.0)
key(2.5, anchor="Alpha Cen A", offset=add(mul(back, 1.8e3), mul(up_a, 400.0)),
    look_at="Alpha Cen A", fov=45, shutter=120)
key(3.0, anchor="Alpha Cen A", offset=add(mul(back, 70.0), mul(side_a, 30.0), mul(up_a, 22.0)),
    look_at="Alpha Cen A", fov=42, ease="inout")
key(4.5, anchor="Alpha Cen A", offset=add(mul(back, 25.0), mul(side_a, 60.0), mul(up_a, 30.0)),
    look_at="Alpha Cen A", fov=42, ease="inout")

d_l = unit(sub(LAGOON, ACEN))
f_l, side_l, up_l = basis(d_l)
key(2.5, pos=add(ACEN, mul(d_l, 3e3), mul(up_l, 1e3)), look_pos=LAGOON, fov=45, shutter=90)
key(2.0, pos=add(ACEN, mul(d_l, 4e5)), look_pos=LAGOON, fov=45, shutter=60)
key(2.0, pos=sub(LAGOON, mul(d_l, 8e7)), look_pos=LAGOON, fov=45, shutter=60)
key(2.5, pos=add(LAGOON, mul(d_l, -2.2 * LAGOON_R * 0.9), mul(side_l, 0.9 * LAGOON_R)),
    look_pos=LAGOON, subject="Lagoon (M8)", fov=45, shutter=150, ease="inout")
key(3.5, pos=add(LAGOON, mul(d_l, -1.4 * LAGOON_R), mul(side_l, 1.3 * LAGOON_R),
                 mul(up_l, 0.4 * LAGOON_R)),
    look_pos=LAGOON, subject="Lagoon (M8)", fov=45, shutter=150, ease="inout")

d_s = unit(sub(SGRA, LAGOON))
f_c, side_c, up_c = basis(d_s)
key(1.5, pos=add(LAGOON, mul(side_l, 3.0 * LAGOON_R), mul(d_s, 3.0 * LAGOON_R)), look_pos=SGRA,
    fov=45, shutter=90)
key(1.5, pos=sub(SGRA, mul(d_s, 2.5e8)), look_pos=SGRA, fov=45, shutter=60)
key(2.0, anchor="Sagittarius A*", offset=add(mul(d_s, -2e5), mul(up_c, 4e4)),
    look_at="Sagittarius A*", fov=44, shutter=90)
key(2.5, anchor="Sagittarius A*", offset=add(mul(d_s, -300.0), mul(up_c, 90.0)),
    look_at="Sagittarius A*", fov=42, shutter=120)
key(2.5, anchor="Sagittarius A*", offset=add(mul(d_s, -2.1), mul(side_c, 0.9), mul(up_c, 1.2)),
    look_at="Sagittarius A*", fov=40, shutter=180, ease="inout")
key(5.5, anchor="Sagittarius A*", offset=add(mul(d_s, -1.2), mul(side_c, 2.0), mul(up_c, 0.8)),
    look_at="Sagittarius A*", fov=40, shutter=180, ease="inout")
key(2.5, anchor="Sagittarius A*", offset=add(mul(POLE, 3e3), mul(side_c, 40.0)),
    look_at="Sagittarius A*", fov=44, shutter=120)
key(2.5, anchor="Sagittarius A*", offset=add(mul(POLE, 3e7), mul(side_c, 2e6)),
    look_at="Sagittarius A*", fov=48, shutter=60)
key(2.5, anchor="Sagittarius A*", offset=add(mul(POLE, 3.2e9), mul(side_c, 4e8)),
    look_at="Sagittarius A*", subject="Milky Way", fov=50, shutter=120)
mw_view = add(SGRA, mul(POLE, 5.2e9), mul(side_c, 7e8))
key(5.0, anchor="Sagittarius A*", offset=add(mul(POLE, 5.2e9), mul(side_c, 7e8)),
    look_at="Sagittarius A*", subject="Milky Way", fov=50, shutter=180, ease="inout")

# ================================================================== ANDROMEDA
# Turn from the Milky Way to Andromeda and cross 2.5 Mly, then fall into its
# inner disc and slow to a stop at a single star: slow enough (< ~1 ly/s
# within ~1 ly) for the star to become a real system with its planets.
d_31 = unit(sub(M31, mw_view))
f_31, side_31, up_31 = basis(d_31)
key(2.5, pos=add(mw_view, mul(d_31, 2e9)), look_pos=M31, fov=48, shutter=90)
key(2.5, pos=add(mw_view, mul(d_31, 6e10)), look_pos=M31, fov=48, shutter=60)
key(2.5, pos=sub(M31, mul(d_31, 4.0 * M31_R)), look_pos=M31, subject="Andromeda (M31)", fov=46,
    shutter=90)
key(4.0, pos=add(sub(M31, mul(d_31, 2.6 * M31_R)), mul(side_31, 0.6 * M31_R)), look_pos=M31,
    subject="Andromeda (M31)", fov=46, shutter=150, ease="inout")
d_sys = unit(sub(M31_SYS, add(sub(M31, mul(d_31, 2.6 * M31_R)), mul(side_31, 0.6 * M31_R))))
f_y, side_y, up_y = basis(d_sys)
key(2.5, pos=sub(M31_SYS, mul(d_sys, 3e8)), look_pos=M31_SYS, fov=46, shutter=60)
key(2.5, pos=sub(M31_SYS, mul(d_sys, 2e6)), look_pos=M31_SYS, fov=46, shutter=60)
key(2.5, pos=add(sub(M31_SYS, mul(d_sys, 2e4)), mul(up_y, 4e3)), look_pos=M31_SYS, fov=44,
    shutter=90, ease="in")
key(2.5, pos=add(sub(M31_SYS, mul(d_sys, 12.0)), mul(side_y, 3.0), mul(up_y, 4.0)),
    look_pos=M31_SYS, subject=M31_SYS_NAME, fov=42, shutter=150, ease="inout")
key(5.5, pos=add(sub(M31_SYS, mul(d_sys, 5.0)), mul(side_y, 7.0), mul(up_y, 3.0)),
    look_pos=M31_SYS, subject=M31_SYS_NAME, fov=42, shutter=150, ease="inout")

# ================================================================== CENTAURUS A, M87
d_ca = unit(sub(CENA, M31_SYS))
f_ca, side_ca, up_ca = basis(d_ca)
key(2.5, pos=add(M31_SYS, mul(up_y, 1e4), mul(d_ca, 2e4)), look_pos=CENA, fov=46, shutter=90)
key(2.0, pos=add(M31_SYS, mul(d_ca, 1e9)), look_pos=CENA, fov=46, shutter=60)
key(2.5, pos=add(M31_SYS, mul(d_ca, 1e11)), look_pos=CENA, fov=46, shutter=60)
key(2.5, pos=sub(CENA, mul(d_ca, 6.0 * CENA_R)), look_pos=CENA, subject="Centaurus A", fov=44,
    shutter=90)
key(4.5, pos=add(sub(CENA, mul(d_ca, 3.5 * CENA_R)), mul(side_ca, 1.5 * CENA_R)), look_pos=CENA,
    subject="Centaurus A", fov=44, shutter=150, ease="inout")

d_87 = unit(sub(M87, CENA))
f_87, side_87, up_87 = basis(d_87)
key(2.5, pos=add(CENA, mul(d_87, 4.0 * CENA_R), mul(side_ca, 2.0 * CENA_R)), look_pos=M87, fov=46,
    shutter=90)
key(2.5, pos=add(CENA, mul(d_87, 3e11)), look_pos=M87, fov=46, shutter=60)
key(2.5, pos=sub(M87, mul(d_87, 8.0 * M87_R)), look_pos=M87, subject="Virgo A (M87)", fov=44,
    shutter=90)
key(4.5, pos=add(sub(M87, mul(d_87, 4.5 * M87_R)), mul(up_87, 1.5 * M87_R)), look_pos=M87,
    subject="Virgo A (M87)", fov=44, shutter=150, ease="inout")

# ================================================================== DEEP FIELD
# Pull straight back from M87 until the galaxies around it become a field.
key(3.0, pos=sub(M87, mul(d_87, 6e11)), look_pos=M87, fov=50, shutter=90)
key(3.5, pos=sub(M87, mul(d_87, 6e12)), look_pos=M87, fov=55, shutter=120)
key(6.0, pos=sub(M87, mul(d_87, 1.5e13)), look_pos=M87, fov=58, shutter=180, ease="out")

# ------------------------------------------------------------------ resample
STEP = 0.25  # seconds between emitted keys


def vec_tangent(ts, vs, i):
    """Velocity (units/s) at sample i of a path, continuous in time. Speed is
    the geometric mean of the two adjoining segments' speeds, capped at 3x the
    slower so a geometric run (each leg ~30x longer) accelerates exponentially
    with no overshoot; the ends start and stop at rest."""
    if i <= 0 or i >= len(vs) - 1 or vs[i - 1] is None or vs[i + 1] is None:
        return zero()
    a, b = sub(vs[i], vs[i - 1]), sub(vs[i + 1], vs[i])
    la, lb = norm(a), norm(b)
    dta, dtb = ts[i] - ts[i - 1], ts[i + 1] - ts[i]
    if la == 0.0 or lb == 0.0 or dta <= 0.0 or dtb <= 0.0:
        return zero()
    ua, ub = mul(a, 1.0 / la), mul(b, 1.0 / lb)
    if dot(ua, ub) < -0.2:
        return zero()
    va, vb = la / dta, lb / dtb
    m = min(math.sqrt(va * vb), 3.0 * min(va, vb))
    return mul(unit(add(ua, ub)), m)


def dir_tangent(ts, vs, i):
    if i <= 0 or i >= len(vs) - 1:
        return zero()
    return mul(sub(vs[i + 1], vs[i - 1]), 1.0 / (ts[i + 1] - ts[i - 1]))


def hermite(ta, tb, pa, pb, ma, mb, t):
    """Quintic Hermite with zero acceleration at both ends: position and
    velocity continuous, and no kick where one segment hands over to the next
    (a cubic leaves the acceleration jumping at every key)."""
    dt = tb - ta
    s = (t - ta) / dt
    h0 = 1 - 10 * s**3 + 15 * s**4 - 6 * s**5
    h1 = s - 6 * s**3 + 8 * s**4 - 3 * s**5
    h4 = -4 * s**3 + 7 * s**4 - 3 * s**5
    h5 = 10 * s**3 - 15 * s**4 + 6 * s**5
    return [h0 * a + h1 * dt * c + h4 * dt * d + h5 * b
            for a, b, c, d in zip(pa, pb, ma, mb)]


def pchip_slope(ts, ys, i):
    """Monotone (Fritsch-Carlson) slope of a scalar path at sample i."""
    if i <= 0 or i >= len(ys) - 1:
        return 0.0
    d0 = (ys[i] - ys[i - 1]) / (ts[i] - ts[i - 1])
    d1 = (ys[i + 1] - ys[i]) / (ts[i + 1] - ts[i])
    if d0 * d1 <= 0.0:
        return 0.0
    w0 = 2 * (ts[i + 1] - ts[i]) + (ts[i] - ts[i - 1])
    w1 = (ts[i + 1] - ts[i]) + 2 * (ts[i] - ts[i - 1])
    return (w0 + w1) / (w0 / d0 + w1 / d1)


def scalar(field, i, t, log=False):
    ts = [w["t"] for w in WP]
    f = math.log if log else (lambda x: x)
    ys = [f(w[field]) for w in WP]
    v = hermite(ts[i], ts[i + 1], [ys[i]], [ys[i + 1]], [pchip_slope(ts, ys, i)],
                [pchip_slope(ts, ys, i + 1)], t)[0]
    return math.exp(v) if log else v


def in_frame(w, c):
    """Waypoint position in compute frame c: an anchor's offset space, or
    absolute (c None)."""
    if c is None:
        return w["here"]
    return w["off"] if w["anchor"] == c else sub(w["here"], APOS[c])


def fmt(v):
    return [float("%.17g" % x) for x in v]


def yaw_pitch(k, d):
    k["yaw"] = round(math.degrees(math.atan2(d[2], d[0])), 4)
    k["pitch"] = round(math.degrees(math.asin(max(-1.0, min(1.0, d[1])))), 4)


def radial_path(ts, vs, T, i, t):
    """Camera position for a segment that closes on / opens from one subject T:
    interpolate log-distance and bearing separately, so a dive from 2e4 AU to
    12 AU closes at a steady fractional rate (an exponential dolly) instead of
    cruising in and braking at the last instant, and a hold swings round the
    subject on an arc instead of cutting a chord."""
    rel = [sub(v, T) for v in vs]
    rho = [math.log(max(norm(r), 1e-12)) for r in rel]
    u = [unit(r) if norm(r) > 1e-12 else [1.0, 0.0, 0.0] for r in rel]
    r = hermite(ts[i], ts[i + 1], [rho[i]], [rho[i + 1]], [pchip_slope(ts, rho, i)],
                [pchip_slope(ts, rho, i + 1)], t)[0]
    d = unit(hermite(ts[i], ts[i + 1], u[i], u[i + 1], dir_tangent(ts, u, i),
                     dir_tangent(ts, u, i + 1), t))
    return add(T, mul(d, math.exp(r)))


def resample():
    ts = [w["t"] for w in WP]
    out = []
    for i in range(len(WP) - 1):
        a, b = WP[i], WP[i + 1]
        n = max(1, int(round((b["t"] - a["t"]) / STEP)))
        # Compute frame: a shared anchor -> its offset space, else absolute.
        c = a["anchor"] if a["anchor"] and a["anchor"] == b["anchor"] else None
        vs = [in_frame(w, c) for w in WP]
        ma, mb = vec_tangent(ts, vs, i), vec_tangent(ts, vs, i + 1)
        lp = [w["lookpt"] for w in WP]
        dv = [w["dir"] for w in WP]
        # A segment that stays on one subject moves in (log-distance, bearing).
        radial, tc = False, None
        if a["lookpt"] is not None and b["lookpt"] is not None:
            tc = a["lookpt"] if c is None else sub(a["lookpt"], APOS[c])
            tb = b["lookpt"] if c is None else sub(b["lookpt"], APOS[c])
            ra, rb = norm(sub(vs[i], tc)), norm(sub(vs[i + 1], tc))
            radial = min(ra, rb) > 0.0 and norm(sub(tc, tb)) < 0.02 * min(ra, rb)
        for j in range(n):
            t = a["t"] + (b["t"] - a["t"]) * j / n
            half = j >= n / 2.0
            if radial:
                p = radial_path(ts, vs, tc, i, t)
            else:
                p = hermite(ts[i], ts[i + 1], vs[i], vs[i + 1], ma, mb, t)
            absp = p if c is None else add(APOS[c], p)
            k = {"t": round(t, 3)}
            if j == 0:
                # the waypoint itself, in its own form
                if a["anchor"]:
                    k["anchor"], k["offset"] = a["anchor"], fmt(a["off"])
                else:
                    k["pos"] = fmt(a["here"])
            elif c:
                k["anchor"], k["offset"] = c, fmt(p)
            else:
                # leaving/joining an anchor: stay in its frame so the camera
                # rides with the body instead of the warm-up snapshot of it
                fr = (b["anchor"] if half else a["anchor"]) or a["anchor"] or b["anchor"]
                if fr:
                    k["anchor"], k["offset"] = fr, fmt(sub(absp, APOS[fr]))
                else:
                    k["pos"] = fmt(absp)
            if radial:
                d = unit(sub(a["lookpt"], absp))
            else:
                # subject changes: one smooth turn spread over the segment
                u = (t - a["t"]) / (b["t"] - a["t"])
                d = slerp(a["dir"], b["dir"], smoother(u))
            if a["look_at"] and a["look_at"] == b["look_at"]:
                k["look_at"] = a["look_at"]
            else:
                yaw_pitch(k, d)
            k["_d"], k["_seg"] = d, i
            if a["subject"]:
                k["subject"] = a["subject"]
            k["fov"] = round(scalar("fov", i, t), 4)
            if a["aperture"] > 0 and b["aperture"] > 0:
                u = (t - a["t"]) / (b["t"] - a["t"])
                u = u * u * (3.0 - 2.0 * u)
                k["aperture"] = round(a["aperture"] ** (1.0 - u) * b["aperture"] ** u, 4)
            else:
                k["aperture"] = a["aperture"]
            if a["focus"]:
                k["focus"] = a["focus"]
            k["timescale"] = float("%.6g" % scalar("timescale", i, t, log=True))
            k["shutter"] = round(scalar("shutter", i, t), 2)
            k["ease"] = "linear"
            out.append(k)
    w = WP[-1]
    k = {"t": round(w["t"], 3)}
    if w["anchor"]:
        k["anchor"], k["offset"] = w["anchor"], fmt(w["off"])
    else:
        k["pos"] = fmt(w["here"])
    if w["look_at"]:
        k["look_at"] = w["look_at"]
    else:
        yaw_pitch(k, w["dir"])
    k["_d"], k["_seg"] = w["dir"], len(WP) - 1
    if w["subject"]:
        k["subject"] = w["subject"]
    k.update(fov=w["fov"], aperture=w["aperture"], timescale=w["timescale"],
             shutter=w["shutter"], ease="linear")
    if w["focus"]:
        k["focus"] = w["focus"]
    out.append(k)
    return out


# ------------------------------------------------------------------ write
HEADER = """// Grand tour — the long showcase: everything the engine renders, one move.
// GENERATED by tools/make_grand_tour.py — edit that, not this file.
//
//   ./verse --preset assets/universes/known_universe.json --cinematic \\
//           --output grand_tour.mp4 --shot-script assets/shots/grand_tour.json \\
//           --res 1920x1080 --fps 30 --samples 32 --letterbox --grain 0.02 \\
//           --cinematic-info --no-orbits --audio assets/soundtrack.ogg
//
// Earth -> the Moon's terrain -> Venus -> Mercury -> Mars -> Jupiter -> Saturn
// -> Uranus -> Neptune -> Alpha Centauri -> Lagoon -> Sagittarius A* -> the
// Milky Way -> Andromeda and a system inside it -> Centaurus A -> M87.
"""


MAX_PAN = 45.0  # deg/s, peak, for a turn between two different subjects
MAX_ORBIT = 40.0  # deg/s, peak, for a swing round one subject


def budget_turns():
    """Stretch any segment whose turn would exceed MAX_PAN (a smoother-step
    turn peaks at 1.875x its mean rate), pushing every later waypoint back."""
    for i in range(len(WP) - 1):
        a, b = WP[i], WP[i + 1]
        same = (a["lookpt"] is not None and b["lookpt"] is not None
                and norm(sub(a["lookpt"], b["lookpt"])) < 0.02 * min(
                    norm(sub(a["here"], a["lookpt"])), norm(sub(b["here"], b["lookpt"]))))
        if same:
            # swinging round one subject: the bearing turns, the subject stays put
            ua, ub = unit(sub(a["here"], a["lookpt"])), unit(sub(b["here"], b["lookpt"]))
            ang = math.degrees(math.acos(max(-1.0, min(1.0, dot(ua, ub)))))
            need = 1.875 * ang / MAX_ORBIT
        else:
            ang = math.degrees(math.acos(max(-1.0, min(1.0, dot(a["dir"], b["dir"])))))
            need = 1.875 * ang / MAX_PAN
        dt = b["t"] - a["t"]
        if need > dt:
            for w in WP[i + 1:]:
                w["t"] += need - dt


def refine_turns(limit=MAX_PAN, rounds=8):
    """Measure the real pan rate of every segment (the bearing splines run a
    little hotter than the ideal turn) and stretch what is still over."""
    for _ in range(rounds):
        keys = resample()
        peak = [0.0] * len(WP)
        for k0, k1 in zip(keys, keys[1:]):
            dt = k1["t"] - k0["t"]
            if dt <= 0:
                continue
            ang = math.degrees(math.acos(max(-1.0, min(1.0, dot(k0["_d"], k1["_d"])))))
            peak[k0["_seg"]] = max(peak[k0["_seg"]], ang / dt)
        grew = 0.0
        for i in range(len(WP) - 1):
            if peak[i] > limit * 1.02:
                dt = WP[i + 1]["t"] - WP[i]["t"]
                extra = dt * min(peak[i] / limit - 1.0, 0.5)
                for w in WP[i + 1:]:
                    w["t"] += extra
                grew += extra
        if grew == 0.0:
            break
    return resample()


def main():
    budget_turns()
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, "..", "assets", "shots", "grand_tour.json")
    keys = refine_turns()
    for k in keys:
        k.pop("_d"), k.pop("_seg")
    body = json.dumps({"name": "Grand tour", "keys": keys}, indent=None)
    body = body.replace('"keys": [', '"keys": [\n    ').replace("}, {", "},\n    {")
    body = body.replace("}]}", "}\n  ]\n}")
    with open(out, "w") as fh:
        fh.write(HEADER + body + "\n")
    print("wrote %s (%d keys, %.1f s)" % (os.path.normpath(out), len(keys), keys[-1]["t"]))


if __name__ == "__main__":
    main()
