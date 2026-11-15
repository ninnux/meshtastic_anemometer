"""
Parametri e funzioni comuni del progetto stazione vento.
Tutte le misure sono in mm. Modifica qui, poi rilancia stazione.py.
"""
import math

import numpy as np
from manifold3d import CrossSection, Manifold

# ----------------------------------------------------------------------------
# PARAMETRI
# ----------------------------------------------------------------------------
P = dict(
    seg=96,                   # risoluzione dei cerchi

    # montaggio e asse
    palo_d=32.0,              # palo su cui si infila la base
    tol_tubo=0.4,             # gioco sul palo
    asse_d=8.0,               # tubo asse fisso (8x1 inox/ottone/alluminio, o carbonio 8x6)
    asse_tol=0.15,
    corpo_r=25.0,             # raggio di base e testa
    alette_r=31.0,            # raggio dei fori di unione testa/piastra
    vite_m3_foro=3.4,
    vite_m3_autofil=2.6,      # foro per vite/grano M3 autofilettante

    # cuscinetto 608 (uno solo, nel rotore)
    cusc_d=22.0,
    cusc_h=7.0,
    cusc_tol=0.15,            # gioco sul diametro della sede: aumenta se entra troppo duro
    cusc_anello_int_d=12.0,   # i collarini toccano solo l'anello interno

    # rotore
    rotore_r_coppa=80.0,      # raggio del centro coppa: più corto = meno inerzia ma meno coppia
    coppa_r=25.0,             # coppa Ø50
    coppa_sp=1.0,             # parete coppa (0.8 per alleggerire ancora)
    mozzo_h=7.6,              # 608 (7) + battuta 0.6
    braccio_h=7.0,            # braccio a lama: altezza (in opera, verticale)
    braccio_t=3.0,            # spessore (in opera, tangenziale)

    # reed + magneti
    reed_r=16.0,              # raggio di magneti e reed
    reed_lung=16.0,           # cava per ampolla ~14 mm
    reed_larg=2.6,            # ampolla fino a Ø2.5
    reed_prof=3.0,
    reed_canale_larg=2.4,     # fessure per i reofori: larghezza ...
    reed_canale_lung=3.0,     # ... e lunghezza oltre le estremità della cava
    magnete_d=6.0,            # disco al neodimio 6x3, magnetizzato assialmente
    magnete_h=3.0,
    n_magneti=2,              # opposti: rotore bilanciato, 2 impulsi/giro

    # potenziometro a rotazione continua 20k (default: bussola 3/8", albero 1/4")
    pot_bussola_d=9.8,
    pot_albero_d=6.35,
    pot_albero_piatto=0.0,    # >0 = albero a D, distanza piatto-asse (es. 2.4 per D 6 mm)
    pot_sporgenza=20.0,       # albero sopra la piastra, bussola compresa
    pot_spina_dist=0.0,       # >0 = foro antirotazione a questa distanza dall'asse
    pot_spina_d=2.5,
    pot_corpo_d=26.0,         # diametro (o diagonale) del corpo + gioco
    pot_corpo_h=22.0,         # profondità corpo + terminali

    # banderuola
    coda_sp=3.0,              # spessore piastra coda
    coda_fori=False,          # True = fori di alleggerimento nella pinna (meno peso, ma meno superficie)
    coda_lung=200.0,
    coda_h_braccio=30.0,
    coda_h_pinna=100.0,
)

SEG = P["seg"]


# ----------------------------------------------------------------------------
# PRIMITIVE
# ----------------------------------------------------------------------------
def cyl(h, r, z=0.0, x=0.0, y=0.0, r2=None, seg=None):
    return Manifold.cylinder(h, r, r if r2 is None else r2, seg or SEG).translate([x, y, z])


def box(x0, x1, y0, y1, z0, z1):
    return Manifold.cube([x1 - x0, y1 - y0, z1 - z0]).translate([x0, y0, z0])


def ring(h, r_out, r_in, z=0.0):
    return cyl(h, r_out, z) - cyl(h + 0.02, r_in, z - 0.01)


def hole_x(r, length, x=0.0, y=0.0, z=0.0):
    """Foro cilindrico lungo X che parte da x."""
    return Manifold.cylinder(length, r, r, 32).rotate([0, 90, 0]).translate([x, y, z])


def hole_y(r, length, x=0.0, y=0.0, z=0.0):
    """Foro cilindrico lungo Y centrato in y."""
    return Manifold.cylinder(length, r, r, 32).rotate([90, 0, 0]).translate([x, y + length / 2, z])


def _u(parts):
    out = parts[0]
    for p in parts[1:]:
        out = out + p
    return out


def alette(z0, h):
    """Tre orecchie con foro M3 (unione testa / piastra pot)."""
    out, holes = [], []
    for a in (90, 210, 330):
        lug = Manifold.hull(cyl(h, 5.5, z0, x=P["alette_r"]) + box(P["corpo_r"] - 6, P["corpo_r"] - 1, -7, 7, z0, z0 + h))
        out.append(lug.rotate([0, 0, a]))
        holes.append(cyl(h + 2, P["vite_m3_foro"] / 2, z0 - 1, x=P["alette_r"]).rotate([0, 0, a]))
    return _u(out), _u(holes)


# ----------------------------------------------------------------------------
# ESPORTAZIONE E ANTEPRIME
# ----------------------------------------------------------------------------
def write_stl(m, path):
    import trimesh
    mesh = m.to_mesh()
    v = np.asarray(mesh.vert_properties)[:, :3]
    f = np.asarray(mesh.tri_verts)
    trimesh.Trimesh(vertices=v, faces=f, process=False).export(path)


def render_png(items, path, elev=18, azim=-60, title=None, size=(10, 7)):
    """Render ombreggiato di una lista (nome, Manifold, colore)."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from mpl_toolkits.mplot3d.art3d import Poly3DCollection
    from matplotlib.colors import to_rgb

    fig = plt.figure(figsize=size, dpi=130)
    ax = fig.add_subplot(111, projection="3d")
    light = np.array([0.4, -0.5, 0.8])
    light /= np.linalg.norm(light)
    allv = []
    for _, m, col in items:
        mesh = m.to_mesh()
        v = np.asarray(mesh.vert_properties)[:, :3]
        tris = v[np.asarray(mesh.tri_verts)]
        n = np.cross(tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0])
        n /= np.linalg.norm(n, axis=1, keepdims=True) + 1e-12
        shade = 0.35 + 0.65 * np.clip(n @ light, 0, 1)
        fc = np.clip(shade[:, None] * np.array(to_rgb(col)), 0, 1)
        ax.add_collection3d(Poly3DCollection(tris, facecolors=fc, edgecolors="none", linewidths=0))
        allv.append(v)
    allv = np.vstack(allv)
    c = (allv.min(0) + allv.max(0)) / 2
    r = (allv.max(0) - allv.min(0)).max() / 2
    ax.set_xlim(c[0] - r, c[0] + r)
    ax.set_ylim(c[1] - r, c[1] + r)
    ax.set_zlim(c[2] - r, c[2] + r)
    ax.set_box_aspect((1, 1, 1))
    ax.view_init(elev=elev, azim=azim)
    ax.set_axis_off()
    if title:
        ax.set_title(title)
    plt.tight_layout()
    plt.savefig(path, bbox_inches="tight")
    plt.close(fig)
