#!/usr/bin/env python3
"""
Stazione vento in linea – palette in mezzo, banderuola in testa
===============================================================
Dal basso verso l'alto:

  palo Ø32 -> BASE      fissa: innesto palo, reed sulla faccia superiore, blocca il tubo asse
           -> ROTORE    palette: mozzo minimo su UN 608 attorno al tubo fisso Ø8, nel piano dei bracci;
                        coppe e bracci stampati in un pezzo; 2 magneti nella faccia inferiore
           -> TESTA     fissa in cima al tubo: innesto tubo + vano potenziometro,
                        chiusa dalla piastra del pot (3 viti M3)
           -> BANDERUOLA mozzo calzato DIRETTAMENTE sull'albero del potenziometro + coda

Il tubo asse (es. 8x1 inox/ottone/alluminio, o carbonio 8x6) è il perno del rotore
e fa passare i fili del potenziometro fino al palo. Il reed è nella base fissa:
i suoi fili scendono direttamente nel palo. La banderuola non ha cuscinetti propri:
gira sulle boccole del potenziometro (va tenuta leggera e ben bilanciata).

Uso:
    pip install manifold3d trimesh numpy matplotlib
    python3 stazione.py            # -> stl/*.stl e assieme.png
I parametri stanno in comune.py.
"""
import io
import os
import sys
import contextlib

from manifold3d import Manifold

from comune import P, SEG, cyl, box, ring, hole_x, hole_y, _u, alette, write_stl, render_png

Z_SOCKET = 38.0   # profondità innesto palo
Z_TOP = 56.0      # faccia superiore base (reed)
COLLARE = 2.0     # distanziale sull'anello interno -> luce sopra/sotto il mozzo


def r_asse():
    return (P["asse_d"] + P["asse_tol"]) / 2


def collare(z):
    """Distanziale che appoggia solo sull'anello interno del 608."""
    return ring(COLLARE, P["cusc_anello_int_d"] / 2 - 0.3, r_asse() + 0.4, z)


# ----------------------------------------------------------------------------
def base():
    R = P["corpo_r"]
    rp = (P["palo_d"] + P["tol_tubo"]) / 2
    r = P["reed_r"]
    Lr = P["reed_lung"]
    cw = P["reed_canale_larg"]       # larghezza fessure reofori
    cl = P["reed_canale_lung"]       # lunghezza fessure oltre la cava
    z_cam = Z_SOCKET + 8.0           # soffitto della camera sotto il reed
    solid = cyl(Z_TOP, R)
    smusso = cyl(3.01, R + 1, Z_TOP - 3) - cyl(3.02, R, Z_TOP - 3.01, r2=20)   # l'acqua scivola fuori
    cut = _u([
        smusso,
        cyl(Z_SOCKET, rp, -0.01),                                   # innesto palo
        cyl(8.01, 12, Z_SOCKET - 0.01),                             # camera cavi sopra il palo
        cyl(Z_TOP - Z_SOCKET + 1, r_asse(), Z_SOCKET),              # foro tubo
        hole_x(P["vite_m3_autofil"] / 2, 30, x=-30, z=50),          # grano tubo
        hole_y(P["vite_m3_autofil"] / 2 + 0.4, 2 * R, y=-R, z=15),  # vite blocco palo
        # reed sulla faccia superiore
        box(r - P["reed_larg"] / 2, r + P["reed_larg"] / 2, -Lr / 2, Lr / 2, Z_TOP - P["reed_prof"], Z_TOP + 1),
        # due fessure rettangolari DRITTE dalle estremità della cava fino alla camera sotto:
        # sovrappongono la cava di 0.5 mm, quindi il reoforo piega e scende senza ostacoli
        box(r - cw / 2, r + cw / 2, Lr / 2 - 0.5, Lr / 2 + cl, z_cam - 0.01, Z_TOP + 1),
        box(r - cw / 2, r + cw / 2, -Lr / 2 - cl, -Lr / 2 + 0.5, z_cam - 0.01, Z_TOP + 1),
        # camera sotto il reed, aperta sulla camera cavi del palo (soffitto = ponte di 9 mm)
        box(10, r + 3, -Lr / 2 - cl - 1, Lr / 2 + cl + 1, Z_SOCKET - 0.01, z_cam),
    ])
    return (solid - cut) + (collare(Z_TOP) - cyl(5, r_asse(), Z_TOP - 1))


def rotore():
    """Mozzo minimo su UN solo 608, nel piano dei bracci. z=0 = faccia inferiore del 608.
    Stampa così com'è (fondo sul piatto)."""
    ch = P["cusc_h"]
    h = P["mozzo_h"]
    sede_r = (P["cusc_d"] + P["cusc_tol"]) / 2
    r_m = P["reed_r"]
    angs_bracci = (0, 120, 240)
    angs_mag = [30 + i * 360 / P["n_magneti"] for i in range(P["n_magneti"])]  # tra i bracci

    anello = cyl(h, sede_r + 2.4)
    bossoli = [Manifold.hull(cyl(3.6, sede_r + 2.4) + cyl(3.6, P["magnete_d"] / 2 + 1.5, x=r_m)).rotate([0, 0, a])
               for a in angs_mag]
    lw = P["braccio_t"] + 0.3 + 4.0
    attacchi = [box(sede_r, 26, -lw / 2, lw / 2, 0, h).rotate([0, 0, a]) for a in angs_bracci]
    solid = _u([anello] + bossoli + attacchi)

    cut = [cyl(ch, sede_r, -0.01), cyl(h + 2, 10.0, -1)]          # sede + battuta superiore (solo anello esterno)
    for a in angs_bracci:
        cut.append(box(14, 27, -(P["braccio_t"] + 0.3) / 2, (P["braccio_t"] + 0.3) / 2, -1, h + 1).rotate([0, 0, a]))
        cut.append(hole_y(P["vite_m3_autofil"] / 2, lw + 2, x=21, z=h / 2).rotate([0, 0, a]))
    for a in angs_mag:
        cut.append(cyl(P["magnete_h"] + 0.2, P["magnete_d"] / 2 + 0.15, -0.01, x=r_m).rotate([0, 0, a]))
    return solid - _u(cut), ch


def coppa_braccio():
    """Coppa + braccio in un pezzo solo: niente bossolo né vite al raggio massimo.
    Stampa con il bordo della coppa sul piatto; il braccio è una lama piatta sul piatto
    (in opera: 7 mm in verticale, 3 mm in senso tangenziale)."""
    R = P["coppa_r"]
    t = P["coppa_sp"]
    Rc = P["rotore_r_coppa"]
    bh, bt = P["braccio_h"], P["braccio_t"]
    shell = Manifold.sphere(R, SEG).trim_by_plane([0, 0, 1], 0)
    inner = Manifold.sphere(R - t, SEG).trim_by_plane([0, 0, 1], 0)
    lama = box(-(Rc - 14), -R + 6, -bh / 2, bh / 2, 0, bt)
    raccordo = Manifold.hull(box(-(R + 10), -R + 6, -bh / 2, bh / 2, 0, bt) + box(-(R + 1), -R + 6, -10, 10, 0, bt))
    foro = cyl(bt + 2, 1.6, -1, x=-(Rc - 21))
    out = (shell + lama + raccordo) - (inner + foro)
    return max(out.decompose(), key=lambda p: p.volume())   # scarta schegge degeneri sul piano z=0


def testa():
    """Fissa sul tubo, contiene il potenziometro. z=0 faccia inferiore. Stampa capovolta."""
    R = P["corpo_r"]
    z_sock = 20.0
    z_cup = z_sock + (14 - 4.5)
    H = z_cup + P["pot_corpo_h"]
    # la testa (R25) fa da ombrello sul mozzo; niente gonna perché i bracci escono a filo del mozzo
    solid = cyl(H, R) + collare(-COLLARE)
    lug, lug_holes = alette(H - 5, 5)
    solid = solid + lug
    rc = P["pot_corpo_d"] / 2
    cut = _u([
        lug_holes,
        cyl(z_sock + COLLARE + 0.1, r_asse(), -COLLARE - 0.05),
        cyl(z_cup - z_sock, 4.5, z_sock, r2=rc),          # fondo conico (niente ponti)
        cyl(H - z_cup + 0.01, rc, z_cup),
        hole_x(P["vite_m3_autofil"] / 2, 30, x=0, z=6),
        hole_x(P["vite_m3_autofil"] / 2, 30, x=0, z=15),
    ])
    return solid - cut, H


def piastra_pot():
    """Chiude la testa e regge il pot (corpo sotto, albero in su). Stampa piatta."""
    t = 4.0
    solid = cyl(t, P["corpo_r"])
    lug, lug_holes = alette(0, t)
    solid = solid + lug
    cut = lug_holes + cyl(t + 2, P["pot_bussola_d"] / 2, -1)
    if P["pot_spina_dist"] > 0:
        cut = cut + cyl(t + 2, P["pot_spina_d"] / 2, -1, x=P["pot_spina_dist"])
    return solid - cut, t


PROF_FORO = 12.0   # quanto albero del pot entra nel mozzo


def mozzo_banderuola():
    """Mozzo banderuola sull'albero del pot. Stampa con la faccia superiore sul piatto (z=0);
    in opera è capovolto: z=0 è la sommità.
    Disco 0..8, boss 8..14 col foro cieco per l'albero (chiuso in alto: niente acqua),
    gonna r27..29 che scende sopra la piastra senza toccare le alette."""
    Rd = P["corpo_r"] + 4
    sp = P["coda_sp"] + 0.4
    hb = P["coda_h_braccio"]
    z_bot = 2.0 + PROF_FORO                       # fondo del boss
    gonna_h = P["pot_sporgenza"] + 2.0 - 8.0 - 1.0          # dal disco fino a 1 mm sopra la piastra
    solid = _u([
        cyl(8, Rd),
        cyl(z_bot - 8 + 0.01, 8, 8 - 0.01),
        ring(gonna_h, Rd, Rd - 2, 8),
        box(20, 44, -sp / 2 - 4, sp / 2 + 4, 0, 10),          # ponte sopra le alette della testa
        box(40, 84, -sp / 2 - 4, sp / 2 + 4, 0, hb),          # blocco coda (oltre r=37: libero dalle alette)
        box(-78, -20, -7, 7, 0, 10),                          # braccio contrappeso
    ])
    p = (P["pot_albero_d"] + 0.15) / 2
    bore = cyl(PROF_FORO + 0.01, p, z_bot - PROF_FORO)
    if P["pot_albero_piatto"] > 0:
        bore = bore - box(P["pot_albero_piatto"], 10, -10, 10, 0, 20)
    cut = _u([
        bore,
        hole_x(P["vite_m3_autofil"] / 2, 20, x=0, z=z_bot - 3.5),          # grano sull'albero
        box(46, 90, -sp / 2, sp / 2, -1, hb + 1),                          # cava coda
        hole_y(1.7, 30, x=56, z=hb / 2),
        hole_y(1.7, 30, x=74, z=hb / 2),
        cyl(12, 4.3, -1, x=-68), cyl(12, 4.3, -1, x=-50),                   # contrappeso: vite M8 + rondelle
    ])
    return solid - cut


def coda():
    """Piastra coda + pinna, stampa in piano; si monta di taglio nella cava del mozzo."""
    from manifold3d import CrossSection
    Lc = P["coda_lung"]
    hb = P["coda_h_braccio"]
    hp = P["coda_h_pinna"]
    poly = [(0, 0), (Lc, 0), (Lc, hp), (Lc - 40, hp), (Lc - 110, hb), (0, hb)]
    plate = CrossSection([poly]).extrude(P["coda_sp"])
    holes = cyl(10, 1.7, -1, x=10, y=hb / 2) + cyl(10, 1.7, -1, x=28, y=hb / 2)
    if P["coda_fori"]:
        holes = holes + cyl(10, 18, -1, x=Lc - 32, y=hp * 0.55) + cyl(10, 9, -1, x=Lc - 75, y=hb + 14)
    return plate - holes


# ----------------------------------------------------------------------------
def assemblea(parts):
    out = []
    out.append(("palo", Manifold.cylinder(100 + Z_SOCKET, P["palo_d"] / 2, P["palo_d"] / 2, 32).translate([0, 0, -100]), "#d0d4d8"))
    out.append(("base", parts["base"], "#5b8db8"))
    z_r = Z_TOP + COLLARE
    rot, Hr = parts["rotore"]
    out.append(("rotore", rot.translate([0, 0, z_r]), "#e06040"))
    zc = z_r + P["mozzo_h"] / 2
    for a in (0, 120, 240):
        cp = (parts["coppa_braccio"].rotate([90, 0, 0])
              .translate([P["rotore_r_coppa"], P["braccio_t"] / 2, zc]).rotate([0, 0, a]))
        out.append(("coppa", cp, "#e06040"))
    z_ti = z_r + Hr + COLLARE
    ti, H1 = parts["testa"]
    out.append(("testa", ti.translate([0, 0, z_ti]), "#5b8db8"))
    z_ts = z_ti + H1
    pp, tp = parts["piastra_pot"]
    out.append(("piastra_pot", pp.translate([0, 0, z_ts]), "#7aa6cc"))
    z_pl = z_ts + tp
    top = z_pl + P["pot_sporgenza"] + 2.0      # sommità mozzo = punta albero + 2 mm
    out.append(("mozzo_banderuola", parts["mozzo_banderuola"].mirror([0, 0, 1]).translate([0, 0, top]), "#c84a30"))
    out.append(("coda", parts["coda"].rotate([90, 0, 0]).translate([46, P["coda_sp"] / 2, top - P["coda_h_braccio"]]), "#c84a30"))
    t0 = Z_SOCKET + 1
    t1 = z_ti + 20 - 0.5
    out.append(("asse", Manifold.cylinder(t1 - t0, 4, 4, 24).translate([0, 0, t0]), "#333333"))
    return out, t1 - t0


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    outdir = os.path.join(here, "stl")
    os.makedirs(outdir, exist_ok=True)
    for f in os.listdir(outdir):
        os.remove(os.path.join(outdir, f))
    parts = {
        "base": base(),
        "rotore": rotore(),
        "coppa_braccio": coppa_braccio(),
        "testa": testa(),
        "piastra_pot": piastra_pot(),
        "mozzo_banderuola": mozzo_banderuola(),
        "coda": coda(),
    }
    qty = dict(base=1, rotore=1, coppa_braccio=3, testa=1, piastra_pot=1, mozzo_banderuola=1, coda=1)
    print(f"{'pezzo':20s} {'q.tà':>4s}  {'ingombro (mm)':>22s}  vol cm3")
    for name, obj in parts.items():
        m = obj[0] if isinstance(obj, tuple) else obj
        assert not m.is_empty(), name
        write_stl(m, os.path.join(outdir, f"{name}.stl"))
        bb = m.bounding_box()
        dims = [bb[i + 3] - bb[i] for i in range(3)]
        print(f"{name:20s} {qty[name]:>4d}  {dims[0]:6.1f} x {dims[1]:6.1f} x {dims[2]:6.1f}  {m.volume()/1000:6.1f}  {m.status()}")
    items, tube_len = assemblea(parts)
    print(f"tubo asse Ø{P['asse_d']:.0f}: lunghezza {tube_len:.0f} mm")
    print(f"altezza sopra il palo (fino al mozzo): {max(m.bounding_box()[5] for n, m, _ in items if n != 'coda') - Z_SOCKET:.0f} mm")
    if "--no-png" not in sys.argv:
        render_png(items, os.path.join(here, "assieme.png"), elev=12, azim=-55,
                   title="Stazione vento", size=(8, 10))
        print("anteprima: assieme.png")
    return parts, items


if __name__ == "__main__":
    main()
