#!/usr/bin/env python3
"""Sezione verticale (piano XZ) della stazione vento, con minuteria. -> sezione.png"""
import io
import os
import sys
import contextlib

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.path import Path
from matplotlib.patches import PathPatch
from manifold3d import Manifold

sys.argv.append("--no-png")
import stazione as S
from comune import P, cyl, ring, box

with contextlib.redirect_stdout(io.StringIO()):
    parts, _ = S.main()

# --- quote (stesse di stazione.assemblea) ---------------------------
rot, Hr = parts["rotore"]
ti, H1 = parts["testa"]
pp, tp = parts["piastra_pot"]
z_r = S.Z_TOP + S.COLLARE
z_ti = z_r + Hr + S.COLLARE
z_ts = z_ti + H1
z_pl = z_ts + tp
top = z_pl + P["pot_sporgenza"] + 2.0
# sezione spezzata: a sinistra (x<0) il rotore è ruotato per mostrare un braccio con la coppa,
# a destra (x>0) per mostrare un magnete sopra il reed
ROT_SX = -60     # bracci a -60/60/180 -> braccio a -X
ROT_DX = -30     # magneti a 0/180 -> magnete a +X

stampati = [
    ("base", parts["base"], "#5b8db8"),
    ("rotore", rot.translate([0, 0, z_r]).rotate([0, 0, ROT_SX]).trim_by_plane([-1, 0, 0], 0)
     + rot.translate([0, 0, z_r]).rotate([0, 0, ROT_DX]).trim_by_plane([1, 0, 0], 0), "#e06040"),
    ("testa", ti.translate([0, 0, z_ti]), "#5b8db8"),
    ("piastra pot", pp.translate([0, 0, z_ts]), "#7aa6cc"),
    ("mozzo banderuola", parts["mozzo_banderuola"].mirror([0, 0, 1]).translate([0, 0, top]), "#c84a30"),
    ("coda", parts["coda"].rotate([90, 0, 0]).translate([46, P["coda_sp"] / 2, top - P["coda_h_braccio"]]), "#c84a30"),
]
zc = z_r + P["mozzo_h"] / 2
cup = (parts["coppa_braccio"].rotate([90, 0, 0])
       .translate([P["rotore_r_coppa"], P["braccio_t"] / 2, zc]).rotate([0, 0, 180]))
stampati.append(("coppa", cup, "#e06040"))


def cusc(z):
    return ring(P["cusc_h"], 11, 9.3, z) + ring(P["cusc_h"], 6.05, 4, z)


t0 = S.Z_SOCKET + 1
t1 = z_ti + 20 - 0.5
metallo = [
    ("palo Ø32", ring(S.Z_SOCKET + 60, 16, 14, -60), "#b8bec4"),
    ("tubo asse Ø8", ring(t1 - t0, 4, 3, t0), "#555555"),
    ("608", cusc(z_r), "#8a8f96"),
    ("potenziometro", cyl(20, 11.5, z_ts - 20) + cyl(7, P["pot_bussola_d"] / 2 - 0.1, z_ts)
     + cyl(z_pl + P["pot_sporgenza"] - (z_ts + 7), P["pot_albero_d"] / 2, z_ts + 7), "#3b3b3b"),
    ("dado bussola", Manifold.cylinder(2.5, 8, 8, 6).translate([0, 0, z_pl]) - cyl(5, P["pot_bussola_d"] / 2, z_pl - 1), "#6d7278"),
    ("magnete", cyl(P["magnete_h"], P["magnete_d"] / 2, z_r, x=P["reed_r"])
     + cyl(P["magnete_h"], P["magnete_d"] / 2, z_r, x=-P["reed_r"]).trim_by_plane([1, 0, 0], 0), "#202020"),
    ("reed", box(P["reed_r"] - 1, P["reed_r"] + 1, -7, 7, S.Z_TOP - 2.6, S.Z_TOP - 0.6), "#2a7a3a"),
]


def section(m):
    """Taglio sul piano y=0 -> lista di contorni (x, z)."""
    cs = m.rotate([90, 0, 0]).slice(0.0)   # y -> -z', z -> y'
    polys = cs.to_polygons()
    return [np.column_stack([np.asarray(p)[:, 0], -np.asarray(p)[:, 1]]) for p in polys if len(p) >= 3]


def draw(ax, m, color, hatch=None, alpha=1.0, z=1):
    polys = section(m)
    if not polys:
        return
    verts, codes = [], []
    for p in polys:
        verts += list(p) + [p[0]]
        codes += [Path.MOVETO] + [Path.LINETO] * (len(p) - 1) + [Path.CLOSEPOLY]
    patch = PathPatch(Path(verts, codes), facecolor=color, edgecolor="#1a1a1a", lw=0.6,
                      hatch=hatch, alpha=alpha, zorder=z)
    ax.add_patch(patch)


fig, ax = plt.subplots(figsize=(10, 11.5), dpi=150)
matplotlib.rcParams["hatch.linewidth"] = 0.4
matplotlib.rcParams["hatch.color"] = "#ffffff"
for _, m, c in stampati:
    draw(ax, m, c, hatch="///", z=1)
for _, m, c in metallo:
    draw(ax, m, c, z=2)

ax.set_aspect("equal")
ax.set_xlim(-115, 175)
ax.set_ylim(-25, top + 85)
ax.axvline(0, color="#999999", lw=0.6, ls="-.", zorder=0)
ax.set_xlabel("mm (griglia 50 mm)")
ax.tick_params(axis="y", labelleft=False)
ax.grid(True, lw=0.2, alpha=0.5)
ax.set_title("Sezione verticale – stazione vento", fontsize=13)


def label(text, xy, xytext):
    ax.annotate(text, xy=xy, xytext=xytext, fontsize=8.5, zorder=5,
                arrowprops=dict(arrowstyle="-", lw=0.6, color="#333333"),
                bbox=dict(boxstyle="round,pad=0.25", fc="white", ec="#888888", lw=0.5),
                ha="left" if xytext[0] > xy[0] else "right", va="center")


R = 135
Lx = -60
rr = P["reed_r"]
label("palo Ø32", (-15, 10), (Lx - 30, 10))
label("base (fissa)\ninnesto palo + grano M4", (22, 30), (R, 20))
label("camera cavi → palo", (-8, S.Z_SOCKET + 3), (Lx - 30, S.Z_SOCKET - 6))
label("reed nella faccia della base\n(smusso: l'acqua scivola fuori)", (rr, S.Z_TOP - 1.6), (R, S.Z_TOP - 14))
label("magnete 6×3 (2 opposti)", (rr, z_r + 1.5), (R, z_r - 1))
label("collarino sull'anello interno", (5, S.Z_TOP + 1), (Lx - 30, S.Z_TOP - 11))
label("UN solo 608 ceramico,\nnel piano dei bracci", (-10, z_r + 3.5), (Lx - 30, z_r + 2))
label("mozzo minimo (≈2,9 cm³)", (14, z_r + 6.5), (R, z_r + 12))
label("braccio-lama 7×3 stampato\ninsieme alla coppa (parete 1 mm)", (-45, zc), (Lx - 30, z_ti + 25))
label("tubo asse fisso Ø8\n(dentro: fili del pot)", (2, z_ti + 12), (Lx - 30, z_ti + 10))
label("testa (fissa): fa da ombrello\nsul mozzo, 2 grani sul tubo", (20, z_ti + 22), (R, z_ti + 26))
label("potenziometro 20k\nrotazione continua", (8, z_ts - 10), (R, z_ts - 14))
label("piastra pot + 3 viti M3", (20, z_ts + 2), (R, z_ts + 4))
label("dado della bussola", (-7, z_pl + 1.2), (Lx - 30, z_pl - 6))
label("albero pot = asse della banderuola\n(12 mm dentro il mozzo, grano M3)", (-2, z_pl + 12), (Lx - 30, z_pl + 14))
label("gonna antipioggia", (-28, z_pl + 4), (Lx - 30, z_pl - 20))
label("mozzo banderuola\n(foro cieco: chiuso in alto)", (-5, top - 1), (Lx - 30, top + 22))
label("coda", (150, top + 20), (R + 10, top + 55))
label("contrappeso (rondelle su vite M8)", (-60, top - 5), (Lx - 30, top + 45))

plt.tight_layout()
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sezione.png")
plt.savefig(out, bbox_inches="tight")
print(out)
