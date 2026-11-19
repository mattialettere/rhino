#!/usr/bin/env python3
"""Genera esempi di addestramento per l'assistente AI di Kurva3D.

Ogni esempio e' una richiesta in italiano e lo script di comandi Kurva3D che la
realizza, con le coordinate calcolate dalle misure (non copiate). Le misure, le
parole e la forma degli oggetti cambiano a caso (seme fisso: lo stesso seme da'
lo stesso file). Ogni script viene poi verificato da Kurva3D in CI:

    python tools/ai-train/generate_dataset.py --count 3000 --out train.jsonl
    kurva3d-ai-test --validate-jsonl train.jsonl

Formato: una riga JSON per esempio, {"messages": [system, user, assistant],
"check": {"h": altezza, "w": lato maggiore in pianta}} (check facoltativo).
Solo libreria standard di Python.
"""
import argparse
import json
import math
import os
import random

HERE = os.path.dirname(os.path.abspath(__file__))


def n(x):
    """Numero nel formato dei comandi: intero se possibile, altrimenti 1 decimale."""
    x = round(float(x), 1)
    if abs(x - round(x)) < 1e-9:
        return str(int(round(x)))
    return ("%.1f" % x).rstrip("0").rstrip(".")


def p(*c):
    return ",".join(n(v) for v in c)


class Rng(random.Random):
    def step(self, lo, hi, s):
        """Valore a caso tra lo e hi, multiplo di s."""
        k = self.randint(int(math.ceil(lo / s)), int(hi // s))
        return k * s


def size_text(r, mm, unit=None):
    """Misura nel testo: in cm o in mm."""
    unit = unit or r.choice(["cm", "cm", "mm"])
    if unit == "cm":
        v = mm / 10.0
        return (n(v) if abs(v - round(v)) > 1e-9 else str(int(round(v)))) + " cm"
    return n(mm) + " mm"


def dims_text(r, a, b, unit=None):
    unit = unit or r.choice(["cm", "mm"])
    if unit == "cm":
        return "%sx%s cm" % (n(a / 10), n(b / 10))
    return "%sx%s mm" % (n(a), n(b))


def union_all(lines):
    lines += ["_SelAll", "_BooleanUnion"]


# --- Modelli di oggetti ------------------------------------------------------------------
def rect_table(r):
    W = r.step(700, 2000, 50)
    D = r.step(500, min(W, 1000), 50)
    H = r.step(700, 780, 10)
    T = r.step(20, 50, 5)
    L = r.step(40, 80, 5)
    e = r.step(20, 80, 10)
    cyl = r.random() < 0.4
    say = [
        "un tavolo %s alto %s" % (dims_text(r, W, D), size_text(r, H)),
        "tavolo rettangolare %s, altezza %s, con quattro gambe" % (dims_text(r, W, D), size_text(r, H)),
        "fammi un tavolo da %s con il piano spesso %s" % (dims_text(r, W, D), size_text(r, T, "mm")),
        "un tavolo di %s per %s con gambe %s" % (size_text(r, W), size_text(r, D), "rotonde" if cyl else "quadrate"),
    ]
    prompt = r.choice(say)
    if "rotonde" in prompt:
        cyl = True
    elif "quadrate" in prompt:
        cyl = False
    lh = H - T + 5
    lines = ["# Top %sx%s, z %s-%s. Four legs %s, z 0-%s, %s from the edges" % (n(W), n(D), n(H - T), n(H),
             ("diameter %s" % n(L)) if cyl else "%sx%s" % (n(L), n(L)), n(lh), n(e)),
             "_Box %s %s %s => Top" % (p(0, 0, H - T), p(W, D), n(T))]
    if cyl:
        rr = L / 2
        pts = [(e + rr, e + rr), (W - e - rr, e + rr), (W - e - rr, D - e - rr), (e + rr, D - e - rr)]
        for i, (x, y) in enumerate(pts):
            lines.append("_Cylinder %s %s %s => Leg%d" % (p(x, y, 0), n(rr), n(lh), i + 1))
    else:
        dx, dy = W - 2 * e - L, D - 2 * e - L
        lines += ["_Box %s %s %s => Leg" % (p(e, e, 0), p(e + L, e + L), n(lh)), "_SelNone", "_SelName Leg",
                  "_Copy %s %s %s %s _Enter" % (p(0, 0, 0), p(dx, 0, 0), p(dx, dy, 0), p(0, dy, 0))]
    union_all(lines)
    return prompt, lines, {"h": H, "w": max(W, D)}


def central_leg_table(r):
    H = r.step(700, 780, 10)
    T = r.step(20, 40, 5)
    round_top = r.random() < 0.7
    Dt = r.step(600, 1200, 50)
    W, D = r.step(600, 1200, 50), r.step(600, 900, 50)
    cd = r.step(60, 140, 10)
    bd = r.step(380, 650, 10)
    bh = r.step(20, 60, 5)
    if round_top:
        prompt = r.choice([
            "un tavolo con gamba centrale",
            "un tavolo rotondo con una gamba centrale",
            "tavolo tondo di diametro %s con una sola colonna centrale e base a disco" % size_text(r, Dt),
            "un tavolo rotondo di %s di diametro alto %s con gamba centrale" % (size_text(r, Dt), size_text(r, H)),
            "tavolino da bistrot rotondo con piede centrale, piano di %s" % size_text(r, Dt),
        ])
        if prompt.endswith("gamba centrale") and "di " not in prompt:
            Dt = 900
        top = "_Cylinder %s %s %s => Top" % (p(0, 0, H - T), n(Dt / 2), n(T))
        w = max(Dt, bd)
        plan = "Round top diameter %s" % n(Dt)
    else:
        prompt = r.choice([
            "un tavolo quadrato %s con una gamba centrale" % dims_text(r, W, W),
            "tavolo rettangolare %s con un'unica colonna al centro e base tonda" % dims_text(r, W, D),
        ])
        if "quadrato" in prompt:
            D = W
        top = "_Box %s %s %s => Top" % (p(-W / 2, -D / 2, H - T), p(W / 2, D / 2), n(T))
        w = max(W, D, bd)
        plan = "Top %sx%s centred on the column" % (n(W), n(D))
    ch = (H - T + 5) - (bh - 5)
    lines = ["# %s, z %s-%s. Column diameter %s, z %s-%s. Base disc diameter %s, z 0-%s" % (
        plan, n(H - T), n(H), n(cd), n(bh - 5), n(H - T + 5), n(bd), n(bh)),
        top,
        "_Cylinder %s %s %s => Column" % (p(0, 0, bh - 5), n(cd / 2), n(ch)),
        "_Cylinder %s %s %s => Base" % (p(0, 0, 0), n(bd / 2), n(bh)),
        "_SelNone", "_SelName Top Column Base", "_BooleanUnion"]
    return prompt, lines, {"h": H, "w": w}


def coffee_table(r):
    H = r.step(320, 480, 10)
    T = r.step(20, 40, 5)
    if r.random() < 0.5:
        W = r.step(500, 1200, 50)
        D = r.step(400, W, 50)
        prompt = r.choice([
            "un tavolino basso %s alto %s" % (dims_text(r, W, D), size_text(r, H)),
            "tavolino da salotto %s con quattro gambe cilindriche, altezza %s" % (dims_text(r, W, D), size_text(r, H)),
        ])
        rr = r.step(15, 25, 5)
        e = r.step(20, 60, 10)
        lines = ["# Top %sx%s, z %s-%s. Four round legs radius %s, z 0-%s" % (n(W), n(D), n(H - T), n(H), n(rr), n(H - T + 5)),
                 "_Box %s %s %s => Top" % (p(0, 0, H - T), p(W, D), n(T))]
        pts = [(e + rr, e + rr), (W - e - rr, e + rr), (W - e - rr, D - e - rr), (e + rr, D - e - rr)]
        for i, (x, y) in enumerate(pts):
            lines.append("_Cylinder %s %s %s => Leg%d" % (p(x, y, 0), n(rr), n(H - T + 5), i + 1))
        union_all(lines)
        return prompt, lines, {"h": H, "w": max(W, D)}
    Dt = r.step(400, 900, 50)
    k = r.choice([3, 4])
    prompt = r.choice([
        "un tavolino rotondo di %s con %d gambe" % (size_text(r, Dt), k),
        "tavolino tondo basso, diametro %s, altezza %s, %d gambe" % (size_text(r, Dt), size_text(r, H), k),
    ])
    rr = r.step(12, 22, 1)
    rp = Dt / 2 - rr - r.step(20, 50, 5)
    lines = ["# Round top diameter %s, z %s-%s. %d legs on a circle of radius %s, z 0-%s" % (
        n(Dt), n(H - T), n(H), k, n(rp), n(H - T + 5)),
        "_Cylinder %s %s %s => Top" % (p(0, 0, H - T), n(Dt / 2), n(T))]
    for i in range(k):
        a = 2 * math.pi * i / k + (math.pi / 4 if k == 4 else math.pi / 2)
        lines.append("_Cylinder %s %s %s => Leg%d" % (p(rp * math.cos(a), rp * math.sin(a), 0), n(rr), n(H - T + 5), i + 1))
    union_all(lines)
    return prompt, lines, {"h": H, "w": Dt}


def stool(r):
    H = r.step(420, 800, 10)
    T = r.step(25, 45, 5)
    Ds = r.step(280, 420, 10)
    k = r.choice([3, 3, 4])
    prompt = r.choice([
        "uno sgabello rotondo a %d gambe alto %s" % (k, size_text(r, H)),
        "sgabello con seduta tonda di %s e %d gambe" % (size_text(r, Ds), k),
        "uno sgabello da bar alto %s" % size_text(r, H),
    ])
    if "bar" in prompt:
        k = 4
    rr = r.step(12, 20, 1)
    # gambe leggermente dentro il bordo della seduta
    rp = Ds / 2 - rr - r.step(10, 30, 5)
    lines = ["# Round seat diameter %s, z %s-%s. %d round legs radius %s on a circle of radius %s, z 0-%s" % (
        n(Ds), n(H - T), n(H), k, n(rr), n(rp), n(H - T + 5)),
        "_Cylinder %s %s %s => Seat" % (p(0, 0, H - T), n(Ds / 2), n(T))]
    for i in range(k):
        a = 2 * math.pi * i / k + math.pi / 2
        lines.append("_Cylinder %s %s %s => Leg%d" % (p(rp * math.cos(a), rp * math.sin(a), 0), n(rr), n(H - T + 5), i + 1))
    union_all(lines)
    return prompt, lines, {"h": H, "w": Ds}


def chair(r):
    S = r.step(380, 500, 10)
    Hs = r.step(420, 480, 10)
    Hb = r.step(800, 1000, 10)
    T = r.step(25, 40, 5)
    L = r.step(30, 50, 5)
    bt = r.step(15, 30, 5)
    bz = r.step(Hs + 150, Hb - 150, 10)
    prompt = r.choice([
        "una sedia",
        "una sedia con lo schienale",
        "sedia con seduta %s a %s da terra e schienale alto %s" % (dims_text(r, S, S), size_text(r, Hs), size_text(r, Hb)),
        "una sedia semplice in legno, schienale fino a %s" % size_text(r, Hb),
    ])
    if prompt in ("una sedia", "una sedia con lo schienale"):
        S, Hs, Hb = 450, 450, 900
        bz = 650
    d = S - L
    lines = ["# Seat %sx%s, z %s-%s. Four legs %sx%s, z 0-%s. Rear posts up to z %s and a back panel z %s-%s" % (
        n(S), n(S), n(Hs - T), n(Hs), n(L), n(L), n(Hs - T + 5), n(Hb), n(bz), n(Hb)),
        "_Box %s %s %s => Seat" % (p(0, 0, Hs - T), p(S, S), n(T)),
        "_Box %s %s %s => Leg" % (p(0, 0, 0), p(L, L), n(Hs - T + 5)),
        "_SelNone", "_SelName Leg",
        "_Copy %s %s %s %s _Enter" % (p(0, 0, 0), p(d, 0, 0), p(d, d, 0), p(0, d, 0)),
        "_Box %s %s %s => PostL" % (p(0, S - L, Hs - 5), p(L, S), n(Hb - Hs + 5)),
        "_Box %s %s %s => PostR" % (p(S - L, S - L, Hs - 5), p(S, S), n(Hb - Hs + 5)),
        "_Box %s %s %s => Back" % (p(0, S - L / 2 - bt / 2, bz), p(S, S - L / 2 + bt / 2), n(Hb - bz)),
    ]
    union_all(lines)
    return prompt, lines, {"h": Hb, "w": S}


def shelf(r):
    L = r.step(400, 1500, 50)
    Dp = r.step(150, 350, 10)
    T = r.step(18, 40, 2)
    bh = r.step(100, 250, 10)
    bt = r.step(10, 25, 5)
    prompt = r.choice([
        "una mensola lunga %s con due staffe sotto" % size_text(r, L),
        "mensola %s profonda %s, spessore %s, con due supporti" % (size_text(r, L), size_text(r, Dp), size_text(r, T, "mm")),
        "una mensola a muro di %s" % size_text(r, L),
    ])
    x1 = round(L * 0.15)
    x2 = round(L * 0.85 - bt)
    lines = ["# Shelf %sx%s, thickness %s, z %s-%s. Two vertical brackets %s thick under it, z 0-%s" % (
        n(L), n(Dp), n(T), n(bh), n(bh + T), n(bt), n(bh + 5)),
        "_Box %s %s %s => Shelf" % (p(0, 0, bh), p(L, Dp), n(T)),
        "_Box %s %s %s => Bracket1" % (p(x1, 0, 0), p(x1 + bt, Dp), n(bh + 5)),
        "_Box %s %s %s => Bracket2" % (p(x2, 0, 0), p(x2 + bt, Dp), n(bh + 5)),
        "_SelNone", "_SelName Shelf Bracket1 Bracket2", "_BooleanUnion"]
    return prompt, lines, {"h": bh + T, "w": L}


def bookcase(r):
    W = r.step(500, 1200, 50)
    H = r.step(800, 2000, 50)
    Dp = r.step(250, 400, 10)
    t = r.step(18, 30, 2)
    k = r.randint(3, 6)
    prompt = r.choice([
        "una libreria %s alta %s con %d ripiani" % (size_text(r, W), size_text(r, H), k),
        "scaffale con %d ripiani, larghezza %s, altezza %s" % (k, size_text(r, W), size_text(r, H)),
    ])
    lines = ["# Two sides %s thick, z 0-%s. %d shelves from the floor to the top, between the sides" % (n(t), n(H), k),
             "_Box %s %s %s => SideL" % (p(0, 0, 0), p(t, Dp), n(H)),
             "_Box %s %s %s => SideR" % (p(W - t, 0, 0), p(W, Dp), n(H))]
    for i in range(k):
        z = (H - t) * i / (k - 1)
        lines.append("_Box %s %s %s => Shelf%d" % (p(t - 2, 0, z), p(W - t + 2, Dp), n(t), i + 1))
    union_all(lines)
    return prompt, lines, {"h": H, "w": W}


def vase(r):
    H = r.step(150, 500, 10)
    R = r.step(40, 120, 5)
    wall = r.step(4, 10, 1)
    b = r.step(6, 15, 1)
    sq = r.random() < 0.3
    if sq:
        prompt = r.choice([
            "un vaso quadrato cavo alto %s, lato %s" % (size_text(r, H), size_text(r, 2 * R)),
            "contenitore quadrato vuoto di lato %s e altezza %s" % (size_text(r, 2 * R), size_text(r, H)),
        ])
        lines = ["# Square body %s, z 0-%s, hollowed from z %s with walls %s thick" % (n(2 * R), n(H), n(b), n(wall)),
                 "_Box %s %s %s => Body" % (p(-R, -R, 0), p(R, R), n(H)),
                 "_Box %s %s %s => Inside" % (p(-R + wall, -R + wall, b), p(R - wall, R - wall), n(H)),
                 "_SelNone", "_SelName Body Inside", "_BooleanDifference"]
        return prompt, lines, {"h": H, "w": 2 * R}
    prompt = r.choice([
        "un vaso cilindrico cavo alto %s" % size_text(r, H),
        "vaso tondo di diametro %s e altezza %s" % (size_text(r, 2 * R), size_text(r, H)),
        "un portapenne cilindrico alto %s" % size_text(r, H),
        "un bicchiere alto %s di diametro %s" % (size_text(r, H), size_text(r, 2 * R)),
    ])
    lines = ["# Body diameter %s, z 0-%s, hollowed from z %s with a wall of %s" % (n(2 * R), n(H), n(b), n(wall)),
             "_Cylinder %s %s %s => Body" % (p(0, 0, 0), n(R), n(H)),
             "_Cylinder %s %s %s => Inside" % (p(0, 0, b), n(R - wall), n(H)),
             "_SelNone", "_SelName Body Inside", "_BooleanDifference"]
    return prompt, lines, {"h": H, "w": 2 * R}


def mug(r):
    H = r.step(80, 120, 5)
    R = r.step(35, 50, 1)
    wall = r.step(3, 6, 1)
    hr = r.step(18, 28, 1)
    ht = r.step(4, 7, 1)
    prompt = r.choice(["una tazza", "una tazza alta %s" % size_text(r, H), "tazza con manico, diametro %s" % size_text(r, 2 * R)])
    if prompt == "una tazza":
        H, R = 95, 40
    lines = ["# Body diameter %s, z 0-%s, hollowed from z 6. Handle: torus turned upright on the side" % (n(2 * R), n(H)),
             "_Cylinder %s %s %s => Body" % (p(0, 0, 0), n(R), n(H)),
             "_Cylinder %s %s %s => Inside" % (p(0, 0, 6), n(R - wall), n(H)),
             "_Torus %s %s %s => Handle" % (p(0, 0, 0), n(hr), n(ht)),
             "_SelNone", "_SelName Handle", "_Rotate3D %s %s 90" % (p(0, 0, 0), p(1, 0, 0)),
             "_Move %s %s" % (p(0, 0, 0), p(R + 2, 0, H / 2)),
             "_SelNone", "_SelName Body Handle", "_BooleanUnion => Mug",
             "_SelNone", "_SelName Mug Inside", "_BooleanDifference"]
    return prompt, lines, None


def block_with_holes(r):
    kind = r.randint(0, 2)
    if kind == 0:
        a = r.step(10, 100, 5)
        rh = r.step(2, a * 0.35, 1)
        prompt = r.choice([
            "un cubo di lato %s con un foro cilindrico di raggio %s al centro" % (size_text(r, a, "mm"), size_text(r, rh, "mm")),
            "cubo da %s forato al centro, foro di diametro %s" % (size_text(r, a, "mm"), size_text(r, 2 * rh, "mm")),
        ])
        lines = ["# Cube %s, z 0-%s. Through hole radius %s along Z in the centre" % (n(a), n(a), n(rh)),
                 "_Box %s %s %s => Body" % (p(0, 0, 0), p(a, a), n(a)),
                 "_Cylinder %s %s %s => Hole" % (p(a / 2, a / 2, -1), n(rh), n(a + 2)),
                 "_SelNone", "_SelName Body Hole", "_BooleanDifference"]
        return prompt, lines, {"h": a, "w": a}
    if kind == 1:
        W = r.step(60, 300, 10)
        D = r.step(40, W, 10)
        T = r.step(4, 20, 1)
        rh = r.step(2, 6, 1)
        m = r.step(8, min(20, D / 4), 1)  # fori distinti anche sulle piastre strette
        prompt = r.choice([
            "una piastra %s spessa %s con quattro fori agli angoli" % (dims_text(r, W, D, "mm"), size_text(r, T, "mm")),
            "piastra forata %s, spessore %s, fori di raggio %s ai 4 angoli" % (dims_text(r, W, D, "mm"), size_text(r, T, "mm"), size_text(r, rh, "mm")),
        ])
        lines = ["# Plate %sx%s, z 0-%s. Four holes radius %s, %s from the edges" % (n(W), n(D), n(T), n(rh), n(m)),
                 "_Box %s %s %s => Plate" % (p(0, 0, 0), p(W, D), n(T))]
        pts = [(m, m), (W - m, m), (W - m, D - m), (m, D - m)]
        for i, (x, y) in enumerate(pts):
            lines.append("_Cylinder %s %s %s => Hole%d" % (p(x, y, -1), n(rh), n(T + 2), i + 1))
        lines += ["_SelNone", "_SelName Plate Hole1 Hole2 Hole3 Hole4", "_BooleanDifference"]
        return prompt, lines, {"h": T, "w": W}
    R = r.step(30, 120, 5)
    T = r.step(5, 20, 1)
    k = r.choice([4, 5, 6, 8])
    rh = r.step(2, 6, 1)
    rp = R - rh - r.step(5, 12, 1)
    prompt = r.choice([
        "un disco di diametro %s spesso %s con %d fori disposti in cerchio" % (size_text(r, 2 * R, "mm"), size_text(r, T, "mm"), k),
        "flangia tonda da %s con %d fori" % (size_text(r, 2 * R, "mm"), k),
    ])
    lines = ["# Disc diameter %s, z 0-%s. %d holes radius %s on a circle of radius %s, made with a polar array" % (
        n(2 * R), n(T), k, n(rh), n(rp)),
        "_Cylinder %s %s %s => Disc" % (p(0, 0, 0), n(R), n(T)),
        "_Cylinder %s %s %s => Hole" % (p(rp, 0, -1), n(rh), n(T + 2)),
        "_SelNone", "_SelName Hole", "_ArrayPolar %s %d" % (p(0, 0, 0), k),
        "_SelAll", "_BooleanDifference"]
    return prompt, lines, None


def lamp(r):
    bd = r.step(140, 260, 10)
    bh = r.step(15, 30, 5)
    sr = r.step(6, 12, 1)
    Hs = r.step(300, 500, 10)
    shR = r.step(70, 120, 5)
    shH = r.step(90, 160, 10)
    if r.random() < 0.5:
        prompt = r.choice(["una lampada da tavolo", "lampada da scrivania alta %s" % size_text(r, Hs + bh),
                           "una lampada con base tonda e paralume cilindrico"])
        top = bh + Hs
        lines = ["# Base disc diameter %s, z 0-%s. Straight stem radius %s up to z %s. Shade: open tube around the top" % (
            n(bd), n(bh), n(sr), n(top)),
            "_Cylinder %s %s %s => Base" % (p(0, 0, 0), n(bd / 2), n(bh)),
            "_Cylinder %s %s %s => Stem" % (p(0, 0, bh - 5), n(sr), n(Hs + 5)),
            "_SelNone", "_SelName Base Stem", "_BooleanUnion => Body",
            "_Tube %s %s %s %s => Shade" % (p(0, 0, top - shH / 2), n(shR), n(shR - 3), n(shH))]
        return prompt, lines, {"h": top + shH / 2, "w": max(bd, 2 * shR)}
    reach = r.step(200, 400, 10)
    prompt = r.choice(["una lampada da tavolo ad arco", "lampada ad arco con lo stelo curvo",
                       "una lampada ad arco che sporge di %s" % size_text(r, reach)])
    lines = ["# Base disc diameter %s, z 0-%s. Bent stem: pipe along a curve up and forward by %s. Shade: open tube at the end" % (
        n(bd), n(bh), n(reach)),
        "_Cylinder %s %s %s => Base" % (p(0, 0, 0), n(bd / 2), n(bh)),
        "_InterpCrv %s %s %s %s _Enter => Path" % (p(0, 0, bh / 2), p(0, reach * 0.15, Hs * 0.55),
                                                   p(0, reach * 0.55, Hs * 0.95), p(0, reach, Hs)),
        "_SelNone", "_SelName Path", "_Pipe %s => Stem" % n(sr), "_SelNone", "_SelName Path", "_Delete",
        "_Tube %s %s %s %s => Shade" % (p(0, reach, Hs - shH), n(shR), n(shR - 3), n(shH))]
    return prompt, lines, None


def bench(r):
    L = r.step(800, 2000, 50)
    D = r.step(300, 450, 10)
    H = r.step(400, 480, 10)
    T = r.step(30, 50, 5)
    t = r.step(30, 60, 5)
    prompt = r.choice(["una panca lunga %s" % size_text(r, L), "panca %s alta %s con due fianchi pieni" % (dims_text(r, L, D), size_text(r, H))])
    lines = ["# Seat %sx%s, z %s-%s. Two solid side panels %s thick, z 0-%s, %s from the ends" % (
        n(L), n(D), n(H - T), n(H), n(t), n(H - T + 5), n(L * 0.1)),
        "_Box %s %s %s => Seat" % (p(0, 0, H - T), p(L, D), n(T)),
        "_Box %s %s %s => SideL" % (p(L * 0.1, 0, 0), p(L * 0.1 + t, D), n(H - T + 5)),
        "_Box %s %s %s => SideR" % (p(L * 0.9 - t, 0, 0), p(L * 0.9, D), n(H - T + 5))]
    union_all(lines)
    return prompt, lines, {"h": H, "w": L}


def stairs(r):
    k = r.randint(3, 10)
    rise = r.step(150, 200, 5)
    run = r.step(250, 320, 10)
    W = r.step(600, 1200, 50)
    prompt = r.choice(["una scala di %d gradini larga %s" % (k, size_text(r, W)),
                       "%d gradini con alzata %s e pedata %s" % (k, size_text(r, rise, "mm"), size_text(r, run, "mm"))])
    lines = ["# %d steps, rise %s, run %s, width %s. Each step is a block from the floor, overlapping the next by 1" % (
        k, n(rise), n(run), n(W))]
    for i in range(k):
        lines.append("_Box %s %s %s => Step%d" % (p(0, i * run, 0), p(W, (i + 1) * run + (1 if i < k - 1 else 0)), n((i + 1) * rise), i + 1))
    union_all(lines)
    return prompt, lines, {"h": k * rise, "w": max(W, k * run)}


def primitive(r):
    kind = r.randint(0, 6)
    if kind == 0:
        R = r.step(5, 200, 5)
        return (r.choice(["una sfera di raggio %s" % size_text(r, R), "sfera di diametro %s" % size_text(r, 2 * R)]),
                ["_Sphere %s %s" % (p(0, 0, R), n(R))], {"h": 2 * R, "w": 2 * R})
    if kind == 1:
        R, H = r.step(5, 150, 5), r.step(10, 500, 10)
        return (r.choice(["un cilindro di diametro %s alto %s" % (size_text(r, 2 * R), size_text(r, H)),
                          "cilindro raggio %s altezza %s" % (size_text(r, R), size_text(r, H))]),
                ["_Cylinder %s %s %s" % (p(0, 0, 0), n(R), n(H))], {"h": H, "w": 2 * R})
    if kind == 2:
        a, b, c = r.step(10, 500, 10), r.step(10, 500, 10), r.step(10, 500, 10)
        return (r.choice(["un parallelepipedo %s x %s x %s mm" % (n(a), n(b), n(c)),
                          "una scatola piena larga %s, profonda %s, alta %s" % (size_text(r, a), size_text(r, b), size_text(r, c))]),
                ["_Box %s %s %s" % (p(0, 0, 0), p(a, b), n(c))], {"h": c, "w": max(a, b)})
    if kind == 3:
        R, H = r.step(10, 150, 5), r.step(20, 400, 10)
        return ("un cono con base di raggio %s alto %s" % (size_text(r, R), size_text(r, H)),
                ["_Cone %s %s %s" % (p(0, 0, 0), n(R), n(H))], {"h": H, "w": 2 * R})
    if kind == 4:
        R, rr = r.step(20, 200, 5), r.step(3, 30, 1)
        rr = min(rr, R / 3)
        return (r.choice(["un toro di raggio %s e spessore %s" % (size_text(r, R), size_text(r, 2 * rr)),
                          "un anello (ciambella) di diametro %s" % size_text(r, 2 * (R + rr))]),
                ["_Torus %s %s %s" % (p(0, 0, rr), n(R), n(rr))], {"h": 2 * rr, "w": 2 * (R + rr)})
    if kind == 5:
        R, w, H = r.step(10, 100, 5), r.step(1, 10, 1), r.step(2, 200, 2)
        return (r.choice(["un tubo di diametro esterno %s, spessore %s, lungo %s" % (size_text(r, 2 * R, "mm"), size_text(r, w, "mm"), size_text(r, H, "mm")),
                          "una rondella di diametro %s con foro da %s, spessa %s" % (size_text(r, 2 * R, "mm"), size_text(r, 2 * (R - w), "mm"), size_text(r, H, "mm"))]),
                ["_Tube %s %s %s %s" % (p(0, 0, 0), n(R), n(R - w), n(H))], {"h": H, "w": 2 * R})
    a = r.step(20, 200, 5)
    f = r.step(1, max(1, a // 10), 1)
    return ("un cubo di %s con gli spigoli arrotondati di %s" % (size_text(r, a, "mm"), size_text(r, f, "mm")),
            ["_Box %s %s %s => Cube" % (p(0, 0, 0), p(a, a), n(a)), "_SelNone", "_SelName Cube", "_FilletEdge %s" % n(f)],
            {"h": a, "w": a})


def polygon_plate(r):
    k = r.choice([3, 5, 6, 8])
    R = r.step(30, 300, 10)
    T = r.step(5, 40, 5)
    names = {3: "triangolare", 5: "pentagonale", 6: "esagonale", 8: "ottagonale"}
    prompt = r.choice(["una piastra %s spessa %s" % (names[k], size_text(r, T, "mm")),
                       "un prisma %s con raggio %s e altezza %s" % (names[k], size_text(r, R), size_text(r, T))])
    lines = ["# Regular %d-sided outline with corner radius %s on the floor, extruded %s, then the curve is deleted" % (k, n(R), n(T)),
             "_Polygon NumSides=%d %s %s => Outline" % (k, p(0, 0, 0), p(R, 0, 0)),
             "_SelNone", "_SelName Outline", "_ExtrudeCrv _Solid=_Yes %s => Plate" % n(T),
             "_SelNone", "_SelName Outline", "_Delete"]
    return prompt, lines, {"h": T}


TEMPLATES = [
    (rect_table, 3), (central_leg_table, 3), (coffee_table, 2), (stool, 2), (chair, 2), (shelf, 2), (bookcase, 1),
    (vase, 2), (mug, 1), (block_with_holes, 2), (lamp, 2), (bench, 1), (stairs, 1), (primitive, 3), (polygon_plate, 1),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=3000)
    ap.add_argument("--seed", type=int, default=20261119)
    ap.add_argument("--out", default="kurva3d-train.jsonl")
    ap.add_argument("--system", default=os.path.join(HERE, "system_prompt.txt"))
    a = ap.parse_args()
    system = open(a.system, encoding="utf-8").read()
    r = Rng(a.seed)
    pool = [f for f, w in TEMPLATES for _ in range(w)]
    seen = set()
    rows = []
    tries = 0
    while len(rows) < a.count and tries < a.count * 20:
        tries += 1
        prompt, lines, check = r.choice(pool)(r)
        if prompt in seen:
            continue
        seen.add(prompt)
        row = {"messages": [{"role": "system", "content": system}, {"role": "user", "content": prompt},
                            {"role": "assistant", "content": "```kurva\n" + "\n".join(lines) + "\n```"}]}
        if check:
            row["check"] = check
        rows.append(row)
    with open(a.out, "w", encoding="utf-8") as f:
        for row in rows:
            f.write(json.dumps(row, ensure_ascii=False) + "\n")
    print("%d esempi scritti in %s" % (len(rows), a.out))


if __name__ == "__main__":
    main()
