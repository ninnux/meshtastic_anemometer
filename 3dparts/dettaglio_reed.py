#!/usr/bin/env python3
"""Dettaglio della base: sezione lungo il reed + vista in spaccato. -> dettaglio_reed.png"""
import os
import sys

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.path import Path
from matplotlib.patches import PathPatch

sys.argv.append("--no-png")
import stazione as S
from comune import P

b = S.base()
r, Lr = P["reed_r"], P["reed_lung"]
cw, cl = P["reed_canale_larg"], P["reed_canale_lung"]
zt, zs = S.Z_TOP, S.Z_SOCKET

fig = plt.figure(figsize=(14, 6), dpi=150)

# --- sezione verticale lungo l'asse del reed (piano x = reed_r) -------------
ax = fig.add_subplot(1, 2, 1)
cs = b.translate([-r, 0, 0]).rotate([0, -90, 0]).slice(0.0)
vs, cd = [], []
for p in cs.to_polygons():
    p = np.asarray(p)
    q = np.column_stack([p[:, 1], -p[:, 0]])
    vs += list(q) + [q[0]]
    cd += [Path.MOVETO] + [Path.LINETO] * (len(q) - 1) + [Path.CLOSEPOLY]
ax.add_patch(PathPatch(Path(vs, cd), fc="#5b8db8", ec="#1a1a1a", lw=0.6, hatch="///"))
matplotlib.rcParams["hatch.color"] = "white"
# reed e reofori
zr = zt - P["reed_prof"] + 1.2
ax.add_patch(plt.Rectangle((-7, zr - 1.1), 14, 2.2, fc="#9fd3a8", ec="#2a7a3a", lw=1, zorder=3))
xl = Lr / 2 + cl / 2 - 0.25
for s in (-1, 1):
    ax.plot([s * 7, s * (xl - 0.8), s * xl, s * xl, s * 6],
            [zr, zr, zr - 0.8, zs + 3, zs - 4], color="#c07000", lw=1.6, zorder=4)
ax.set_aspect("equal")
ax.set_xlim(-30, 30)
ax.set_ylim(zs - 8, zt + 6)
ax.grid(lw=0.2)
ax.set_xlabel("mm, lungo l'asse del reed")
ax.set_ylabel("quota (mm)")
ax.set_title("Sezione lungo il reed")
kw = dict(fontsize=8.5, bbox=dict(boxstyle="round,pad=0.25", fc="white", ec="#888", lw=0.5),
          arrowprops=dict(arrowstyle="-", lw=0.6))
ax.annotate("reed in ampolla nella cava", (0, zr + 1), (-29, zt + 4), **kw)
ax.annotate(f"fessure dritte {cw:.1f}×{cl + 0.5:.1f} mm\nfino alla camera", (xl, zt - 6), (12, zt + 3.5), **kw)
ax.annotate("camera sotto il reed", (-4, zs + 4), (-29, zs + 11), **kw)
ax.annotate("verso il palo", (6, zs - 3), (14, zs - 6), **kw)

# --- vista dall'alto: sezione orizzontale 1 mm sotto la faccia -------------
ax2 = fig.add_subplot(1, 2, 2)
for zcut, col, lab in ((zt - 1.0, "#5b8db8", "faccia superiore"), ):
    cs = b.slice(zcut)
    vs, cd = [], []
    for p in cs.to_polygons():
        p = np.asarray(p)
        vs += list(p) + [p[0]]
        cd += [Path.MOVETO] + [Path.LINETO] * (len(p) - 1) + [Path.CLOSEPOLY]
    ax2.add_patch(PathPatch(Path(vs, cd), fc=col, ec="#1a1a1a", lw=0.6, hatch="///"))
ax2.add_patch(plt.Rectangle((r - 1.1, -7), 2.2, 14, fc="#9fd3a8", ec="#2a7a3a", lw=1, zorder=3))
ax2.add_patch(plt.Circle((r, 0), P["magnete_d"] / 2, fc="none", ec="#202020", ls="--", lw=1, zorder=4))
ax2.set_aspect("equal")
ax2.set_xlim(-28, 28)
ax2.set_ylim(-28, 28)
ax2.grid(lw=0.2)
ax2.set_xlabel("mm")
ax2.set_title("Vista dall'alto (taglio 1 mm sotto la faccia)")
ax2.annotate("cava reed 16×2,6", (r, 3), (-27, 20), **kw)
ax2.annotate("fessure reofori", (r, Lr / 2 + 1.5), (-27, -20), **kw)
ax2.annotate("", (r, -Lr / 2 - 1.5), (-15, -18.5), arrowprops=dict(arrowstyle="-", lw=0.6))
ax2.annotate("passaggio magnete\n(tratteggio)", (r + 3, 0), (19, -22), **kw)
ax2.annotate("foro tubo asse", (0, 4), (-27, 8), **kw)

plt.tight_layout()
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dettaglio_reed.png")
plt.savefig(out, bbox_inches="tight")
print(out)
