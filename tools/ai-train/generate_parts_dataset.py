#!/usr/bin/env python3
"""Genera esempi di addestramento per il lavoro PER PARTI dell'assistente AI di Kurva3D.

Ogni esempio e' una conversazione completa come quella che fa Kurva3D con un modello
addestrato (Generator::RunParts in modo compatto):

    system    istruzioni brevi (parts_system_prompt.txt = ai::CompactPlanSystemPrompt())
    user      richiesta + "Step 1 ..."           (ai::PlanRequest(richiesta, {}, true))
    assistant ```plan  # misure  # Distinctive  PART ...```
    user      "Part 1 of N: ..."                  (ai::PartRequest(piano, 0))
    assistant ```kurva ...```                     comandi della parte 1
    ...       una coppia per ogni parte

Gli oggetti sono modelli parametrici di molte famiglie (mobili, oggetti di casa, strumenti
musicali, veicoli, velivoli, ...): misure, proporzioni, colori, numero di parti e parole
della richiesta cambiano a caso (seme fisso), le coordinate sono calcolate dalle misure e
l'ingombro di ogni parte nel piano e' calcolato dalla geometria. Kurva3D verifica ogni
esempio (richieste identiche, piano valido, ogni parte eseguita e dentro il suo ingombro):

    python tools/ai-train/generate_parts_dataset.py --count 400 --out parts.jsonl
    kurva3d-ai-test --validate-parts-jsonl parts.jsonl

Solo libreria standard di Python.
"""
import argparse
import json
import math
import os
import random

HERE = os.path.dirname(os.path.abspath(__file__))


# --- Numeri e testi ----------------------------------------------------------------------
def n(x):
    """Numero nel formato dei comandi: intero se possibile, altrimenti 1 decimale."""
    x = round(float(x), 1)
    if abs(x) < 0.05:
        x = 0.0
    if abs(x - round(x)) < 1e-9:
        return str(int(round(x)))
    return ("%.1f" % x).rstrip("0").rstrip(".")


def p(*c):
    return ",".join(n(v) for v in c)


def g6(v):
    """Come F() di Plan.cpp: %.6g, senza -0."""
    if abs(v) < 5e-7:
        v = 0.0
    return "%.6g" % v


def box_text(b):
    return "%s,%s,%s %s,%s,%s" % tuple(g6(v) for v in b)


class Rng(random.Random):
    def step(self, lo, hi, s):
        k = self.randint(int(math.ceil(lo / s)), int(hi // s))
        return k * s


def size_text(r, mm):
    if mm >= 1000 and r.random() < 0.5:
        v = mm / 1000.0
        return ("%.2f" % v).rstrip("0").rstrip(".").replace(".", ",") + " m"
    if r.random() < 0.7:
        v = mm / 10.0
        return (n(v).replace(".", ",")) + " cm"
    return n(mm) + " mm"


COLORS = {
    "rosso": (200, 30, 30), "rossa": (200, 30, 30), "blu": (40, 70, 170), "verde": (40, 130, 60),
    "giallo": (230, 200, 40), "gialla": (230, 200, 40), "nero": (35, 35, 35), "nera": (35, 35, 35),
    "bianco": (240, 240, 240), "bianca": (240, 240, 240), "arancione": (240, 130, 30), "grigio": (150, 150, 155),
    "grigia": (150, 150, 155),
}
WOOD = [(190, 140, 90), (150, 100, 60), (120, 70, 30), (205, 155, 95), (165, 85, 30)]
METAL = [(180, 180, 190), (150, 150, 160), (200, 200, 205)]
DARK = [(30, 30, 30), (40, 40, 40), (30, 22, 20)]


def ask(r, noun, extra=""):
    lead = r.choice(["", "", "disegna ", "fammi ", "modella ", "crea ", "vorrei ", "mi serve "])
    s = lead + noun + (" " + extra if extra else "")
    return s.strip()


# --- Parti con il loro ingombro ------------------------------------------------------------
AXES = {"x": ((1, 0, 0), (0, 1, 0), (0, 0, 1)), "y": ((0, 1, 0), (1, 0, 0), (0, 0, 1)),
        "z": ((0, 0, 1), (1, 0, 0), (0, 1, 0))}


def add(a, b, k=1.0):
    return tuple(a[i] + k * b[i] for i in range(3))


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


class Part:
    def __init__(self, name, what):
        self.name, self.what, self.lines = name, what, []
        self.lo, self.hi = [1e300] * 3, [-1e300] * 3

    # ingombro
    def pt(self, q, pad=(0, 0, 0)):
        for k in range(3):
            self.lo[k] = min(self.lo[k], q[k] - pad[k])
            self.hi[k] = max(self.hi[k], q[k] + pad[k])

    def box_of(self):
        return [math.floor(self.lo[0]), math.floor(self.lo[1]), math.floor(self.lo[2]),
                math.ceil(self.hi[0]), math.ceil(self.hi[1]), math.ceil(self.hi[2])]

    def nm(self, name):
        return " => " + (name or self.name)

    def raw(self, line):
        self.lines.append(line)
        return self

    def note(self, text):
        return self.raw("# " + text)

    # forme
    def box(self, x0, y0, z0, x1, y1, h, name=None, measure=True):
        x0, x1 = min(x0, x1), max(x0, x1)
        y0, y1 = min(y0, y1), max(y0, y1)
        self.raw("_Box %s %s %s%s" % (p(x0, y0, z0), p(x1, y1), n(h), self.nm(name)))
        if measure:
            self.pt((x0, y0, z0))
            self.pt((x1, y1, z0 + h))
        return self

    def cbox(self, cx, cy, z0, w, d, h, name=None, measure=True):
        return self.box(cx - w / 2, cy - d / 2, z0, cx + w / 2, cy + d / 2, h, name, measure)

    def cyl(self, c, r, h, name=None, measure=True):
        self.raw("_Cylinder %s %s %s%s" % (p(*c), n(r), n(h), self.nm(name)))
        if measure:
            self.pt((c[0], c[1], c[2]), (r, r, 0))
            self.pt((c[0], c[1], c[2] + h), (r, r, 0))
        return self

    def cone(self, c, r, h, name=None):
        self.raw("_Cone %s %s %s%s" % (p(*c), n(r), n(h), self.nm(name)))
        self.pt(c, (r, r, 0))
        self.pt((c[0], c[1], c[2] + h))
        return self

    def sphere(self, c, r, name=None):
        self.raw("_Sphere %s %s%s" % (p(*c), n(r), self.nm(name)))
        self.pt(c, (r, r, r))
        return self

    def ellipsoid(self, c, rx, ry, rz, name=None):
        self.raw("_Ellipsoid %s %s %s %s%s" % (p(*c), n(rx), n(ry), n(rz), self.nm(name)))
        self.pt(c, (rx, ry, rz))
        return self

    def pipe(self, r, pts, name=None):
        self.raw("_PipeThrough %s %s _Enter%s" % (n(r), " ".join(p(*q) for q in pts), self.nm(name)))
        for q in pts:
            self.pt(q, (r, r, r))
        return self

    def sloft(self, secs, name=None):
        """secs: [((x,y,z), larghezza, altezza)], in ordine lungo l'oggetto."""
        self.raw("_SectionLoft %s _Enter%s" % (" ".join("%s %s %s" % (p(*c), n(w), n(h)) for c, w, h in secs),
                                                self.nm(name)))
        m = len(secs)
        for i, (c, w, h) in enumerate(secs):
            a, b = secs[max(0, i - 1)][0], secs[min(m - 1, i + 1)][0]
            d = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
            L = math.sqrt(sum(v * v for v in d)) or 1
            if abs(d[2]) / L > 0.7:
                self.pt(c, (w / 2, h / 2, 0))
            else:
                hx, hy = d[0], d[1]
                hl = math.hypot(hx, hy) or 1
                ux, uy = -hy / hl, hx / hl
                self.pt(c, (abs(ux) * w / 2, abs(uy) * w / 2, h / 2))
        return self

    def lathe(self, c, pairs, name=None):
        self.raw("_Lathe %s %s _Enter%s" % (p(*c), " ".join(p(rr, hh) for rr, hh in pairs), self.nm(name)))
        R = max(rr for rr, hh in pairs)
        self.pt((c[0], c[1], c[2] + pairs[0][1]), (R, R, 0))
        self.pt((c[0], c[1], c[2] + pairs[-1][1]), (R, R, 0))
        return self

    def plate(self, t, pts, name=None, close=True):
        self.raw("_Plate %s %s _Close%s" % (n(t), " ".join(p(*q) for q in pts), self.nm(name)))
        nx = ny = nz = 0.0
        for i in range(len(pts)):
            u, v = pts[i], pts[(i + 1) % len(pts)]
            nx += (u[1] - v[1]) * (u[2] + v[2])
            ny += (u[2] - v[2]) * (u[0] + v[0])
            nz += (u[0] - v[0]) * (u[1] + v[1])
        L = math.sqrt(nx * nx + ny * ny + nz * nz) or 1
        pad = (abs(nx) / L * t / 2, abs(ny) / L * t / 2, abs(nz) / L * t / 2)
        for q in pts:
            self.pt(q, pad)
        return self

    def symext(self, half, z0, h, tmp="Outline", name=None):
        """Contorno simmetrico (meta' destra, dall'alto al basso sull'asse x = 0) estruso di h."""
        self.raw("_SymCurve %s _Enter => %s" % (" ".join(p(x, y, z0) for x, y in half), tmp))
        self.raw("_ExtrudeCrv _Solid=_Yes %s @%s%s" % (n(h), tmp, self.nm(name)))
        self.raw("_Delete @%s" % tmp)
        for x, y in half:
            self.pt((x, y, z0))
            self.pt((-x, y, z0 + h))
        return self

    def wheel(self, c, r, w, axis, spokes, name=None):
        self.raw("_Wheel %s %s %s %s %d%s" % (p(*c), n(r), n(w), axis, spokes, self.nm(name)))
        a, u, v = AXES[axis]
        half = w / 2
        pad = tuple(abs(a[k]) * half + (abs(u[k]) + abs(v[k])) * r for k in range(3))
        self.pt(c, pad)
        return self

    def blades(self, c, axis, k, L, chord, t, name=None):
        self.raw("_Blades %s %s %d %s %s %s%s" % (p(*c), axis, k, n(L), n(chord), n(t), self.nm(name)))
        a, u, v = AXES[axis]
        hub = min(chord * 0.6, L * 0.3)
        self.pt(c, tuple(abs(a[i]) * 1.5 * t + (abs(u[i]) + abs(v[i])) * hub for i in range(3)))
        r0 = min(chord * 0.5, L * 0.15)
        for i in range(k):
            ang = 2 * math.pi * i / k
            d = tuple(u[j] * math.cos(ang) + v[j] * math.sin(ang) for j in range(3))
            s = cross(a, d)
            for q in (add(add(c, d, r0), s, -chord * 0.3), add(add(c, d, L), s, -chord / 2),
                      add(add(c, d, L), s, chord / 2), add(add(c, d, r0), s, chord * 0.3)):
                self.pt(q, tuple(abs(a[j]) * t / 2 for j in range(3)))
        return self

    def mirror(self, name=None):
        nm = name or self.name
        self.raw("_Mirror 0,0,0 0,1,0 @%s => %s" % (nm, nm))
        lo, hi = self.lo[0], self.hi[0]
        self.lo[0] = min(lo, -hi)
        self.hi[0] = max(hi, -lo)
        return self

    def cut(self, cutter):
        self.raw("_BooleanDifference @%s @%s => %s" % (self.name, cutter, self.name))
        return self

    def color(self, rgb, name=None):
        return self.raw("_SetObjectColor %d,%d,%d @%s" % (rgb[0], rgb[1], rgb[2], name or self.name))

    def delete(self, *names):
        return self.raw("_Delete " + " ".join("@" + x for x in names))


class Obj:
    def __init__(self, summary, features):
        self.summary, self.features, self.parts = summary, features, []

    def part(self, name, what):
        P = Part(name, what)
        self.parts.append(P)
        return P


# --- Famiglie di oggetti ------------------------------------------------------------------
def table(r):
    W, D, H = r.step(800, 2000, 50), r.step(600, 1000, 50), r.step(720, 780, 10)
    D = min(D, W)
    T, L = r.step(25, 45, 5), r.step(45, 80, 5)
    round_legs = r.random() < 0.4
    q = ask(r, r.choice(["un tavolo", "un tavolo da pranzo", "un tavolo di legno"]),
            r.choice(["", "%s x %s" % (size_text(r, W), size_text(r, D)), "alto %s" % size_text(r, H)]))
    o = Obj("Table %sx%s, %s high, standing on z = 0" % (n(W), n(D), n(H)),
            "flat rectangular top on four %s legs at the corners" % ("round" if round_legs else "square"))
    c = r.choice(WOOD)
    o.part("Top", "flat top %sx%s, %s thick: _Box" % (n(W), n(D), n(T))).box(0, 0, H - T, W, D, T).color(c)
    P = o.part("Legs", "four %s legs under the corners: %s" % ("round" if round_legs else "square",
                                                              "_Cylinder" if round_legs else "_Box"))
    e = L / 2 + r.step(20, 60, 10)
    for i, (x, y) in enumerate([(e, e), (W - e, e), (W - e, D - e), (e, D - e)]):
        if round_legs:
            P.cyl((x, y, 0), L / 2, H - T + 1)
        else:
            P.cbox(x, y, 0, L, L, H - T + 1)
    P.color(tuple(max(0, v - 30) for v in c))
    return q, o


def round_table(r):
    Dm, H = r.step(700, 1400, 50), r.step(720, 760, 10)
    T = r.step(25, 40, 5)
    q = ask(r, r.choice(["un tavolo rotondo", "un tavolo tondo con gamba centrale", "un tavolino rotondo da bar"]),
            r.choice(["", "di diametro %s" % size_text(r, Dm)]))
    o = Obj("Round table, diameter %s, %s high, on z = 0" % (n(Dm), n(H)),
            "round top on a single central column with a wide round foot")
    c = r.choice(WOOD)
    o.part("Top", "round top: _Cylinder").cyl((0, 0, H - T), Dm / 2, T).color(c)
    cr = r.step(35, 60, 5)
    o.part("Column", "central column: _Lathe").lathe((0, 0, 30), [(cr * 1.3, 0), (cr, (H - T - 30) * 0.3), (cr * 0.8, (H - T - 30) * 0.7), (cr * 1.1, H - T - 30 + 1)]).color(c)
    fr = r.step(220, min(380, Dm * 0.4), 10)
    o.part("Foot", "round foot on the floor: _Lathe").lathe((0, 0, 0), [(fr, 0), (fr * 0.95, 15), (cr * 1.5, 32)]).color(r.choice(DARK))
    return q, o


def chair(r):
    S, Hs, Hb = r.step(400, 500, 10), r.step(430, 470, 10), r.step(820, 950, 10)
    L, T = r.step(30, 45, 5), r.step(25, 40, 5)
    q = ask(r, r.choice(["una sedia", "una sedia di legno", "una sedia da cucina"]))
    o = Obj("Chair %sx%s, seat at %s, backrest up to %s along the back edge (y = %s)" % (n(S), n(S), n(Hs), n(Hb), n(S)),
            "square seat at chair height, four legs, tall backrest only on the back edge")
    c = r.choice(WOOD)
    o.part("Seat", "seat board, %s thick: _Box" % n(T)).box(0, 0, Hs - T, S, S, T).color(c)
    P = o.part("Legs", "four legs under the corners: _Box")
    for x, y in [(0, 0), (S - L, 0), (S - L, S - L), (0, S - L)]:
        P.box(x, y, 0, x + L, y + L, Hs - T + 1)
    P.color(c)
    B = o.part("Backrest", "two rear posts and a back panel between them: _Box")
    B.box(0, S - L, Hs - 1, L, S, Hb - Hs + 1).box(S - L, S - L, Hs - 1, S, S, Hb - Hs + 1)
    B.box(L, S - L + 5, Hb - 260, S - L, S - 5, 220).color(c)
    return q, o


def stool(r):
    H, R = r.step(450, 800, 10), r.step(150, 200, 5)
    k = r.choice([3, 4])
    q = ask(r, r.choice(["uno sgabello", "uno sgabello alto da bar", "uno sgabello a %d gambe" % k]))
    o = Obj("Stool %s high, round seat diameter %s, %d splayed legs, on z = 0" % (n(H), n(2 * R), k),
            "round seat on %d thin splayed legs joined by a ring" % k)
    c = r.choice(WOOD + METAL)
    o.part("Seat", "round seat 40 thick: _Cylinder").cyl((0, 0, H - 40), R, 40).color(c)
    P = o.part("Legs", "%d splayed legs from under the seat to the floor: _PipeThrough" % k)
    lr = r.step(10, 16, 1)
    for i in range(k):
        a = 2 * math.pi * i / k + math.pi / 4
        top = (R * 0.6 * math.cos(a), R * 0.6 * math.sin(a), H - 39)
        bot = ((R + 60) * math.cos(a), (R + 60) * math.sin(a), lr)
        P.pipe(lr, [top, bot])
    P.color(r.choice(METAL + DARK))
    zr = H * 0.35
    rr = R * 0.6 + (R + 60 - R * 0.6) * (1 - (zr - lr) / (H - 39 - lr))
    o.part("Ring", "foot ring between the legs: _PipeThrough around").pipe(
        8, [(rr * math.cos(2 * math.pi * i / 8), rr * math.sin(2 * math.pi * i / 8), zr) for i in range(9)]).color(r.choice(METAL))
    return q, o


def bookcase(r):
    W, D, H = r.step(600, 1600, 50), r.step(250, 400, 10), r.step(1200, 2200, 50)
    k = r.randint(3, 6)
    t = r.step(18, 25, 1)
    q = ask(r, r.choice(["una libreria", "uno scaffale per libri", "una libreria con %d ripiani" % k]),
            r.choice(["", "alta %s" % size_text(r, H)]))
    o = Obj("Bookcase %s wide, %s deep, %s high, on z = 0" % (n(W), n(D), n(H)),
            "two tall sides, %d horizontal shelves between them and a thin back panel" % k)
    c = r.choice(WOOD + [(240, 240, 240)])
    o.part("Sides", "two vertical side panels: _Box").box(0, 0, 0, t, D, H).box(W - t, 0, 0, W, D, H).color(c)
    S = o.part("Shelves", "%d shelves from the bottom to the top: _Box" % k)
    for i in range(k):
        z = 0 if i == 0 else (H - t) * i / (k - 1)
        S.box(t - 1, 0, z, W - t + 1, D - 8, t)
    S.color(c)
    o.part("Back", "thin back panel: _Box").box(t - 1, D - 8, 0, W - t + 1, D, H).color(tuple(max(0, v - 25) for v in c))
    return q, o


def floor_lamp(r):
    H, br = r.step(1400, 1800, 50), r.step(140, 200, 10)
    sr = r.step(150, 250, 10)
    sh = r.step(200, 320, 10)
    q = ask(r, r.choice(["una lampada a stelo", "una piantana", "una lampada da terra"]))
    o = Obj("Floor lamp %s high on z = 0: base, pole, conical shade on top" % n(H + sh * 0.3),
            "heavy round base, thin tall pole, wide conical shade at the top")
    o.part("Base", "round heavy base: _Lathe").lathe((0, 0, 0), [(br, 0), (br * 0.95, 20), (br * 0.4, 45)]).color(r.choice(DARK))
    o.part("Pole", "thin vertical pole: _PipeThrough").pipe(12, [(0, 0, 30), (0, 0, H)]).color(r.choice(METAL))
    o.part("Shade", "conical shade around the top of the pole: _Lathe").lathe(
        (0, 0, H - sh * 0.7), [(sr, 0), (sr * 0.75, sh * 0.5), (sr * 0.5, sh)]).color(r.choice([(240, 230, 200), (230, 200, 60), (240, 240, 240)]))
    return q, o


def mug(r):
    H, R = r.step(80, 120, 5), r.step(35, 50, 1)
    q = ask(r, r.choice(["una tazza", "una tazza da caffe'", "un boccale", "una tazza con manico"]))
    col = r.choice(list(COLORS.items()))
    if r.random() < 0.4:
        q += " " + col[0]
    o = Obj("Mug %s wide and %s high, standing on z = 0, handle on the +X side" % (n(2 * R), n(H)),
            "hollow cylinder open at the top with one loop handle on the side")
    B = o.part("Body", "hollow round cup: _Lathe outside, _Lathe inside, _BooleanDifference")
    B.lathe((0, 0, 0), [(R - 2, 0), (R, H * 0.5), (R + 1, H)])
    B.raw("_Lathe %s %s _Enter => Inside" % (p(0, 0, 6), " ".join(p(rr, hh) for rr, hh in [(R - 6, 0), (R - 4, H * 0.5 - 6), (R - 3, H - 6 + 1)])))
    B.cut("Inside").color(col[1])
    hr = r.step(5, 7, 1)
    o.part("Handle", "curved handle on the side: _PipeThrough").pipe(
        hr, [(R - 2, 0, H * 0.8), (R + 24, 0, H * 0.76), (R + 30, 0, H * 0.5), (R + 22, 0, H * 0.25), (R - 2, 0, H * 0.2)]).color(col[1])
    return q, o


def bottle(r):
    H, R = r.step(220, 360, 10), r.step(30, 45, 1)
    q = ask(r, r.choice(["una bottiglia", "una bottiglia di vino", "una bottiglia d'acqua", "una bottiglia di birra"]))
    o = Obj("Bottle %s wide and %s high, standing on z = 0, turned around the vertical axis" % (n(2 * R), n(H)),
            "tall round body, sloping shoulder, long thin neck, small cap")
    nh = H * r.uniform(0.28, 0.38)
    o.part("Body", "round body, shoulder and neck: _Lathe").lathe(
        (0, 0, 0), [(R - 2, 0), (R, 8), (R, H - nh - 40), (R * 0.6, H - nh - 10), (R * 0.4, H - nh + 10), (R * 0.38, H - 12)]).color(
        r.choice([(40, 110, 60), (120, 70, 30), (200, 220, 230)]))
    o.part("Cap", "small cap on top of the neck: _Cylinder").cyl((0, 0, H - 14), R * 0.42, 14).color(r.choice(METAL + DARK))
    return q, o


def teapot(r):
    H, R = r.step(150, 240, 10), r.step(80, 120, 5)
    q = ask(r, r.choice(["una teiera", "una caffettiera", "un bollitore"]))
    o = Obj("Teapot about %s high standing on z = 0: round body, lid with knob, spout on the -Y side, handle on the +Y side" % n(H),
            "round belly, lid with a knob, spout rising on one side and a loop handle on the other")
    bh = H * 0.75
    col = r.choice([(240, 240, 235), (200, 30, 30), (40, 70, 170), (150, 150, 160)])
    o.part("Body", "round belly turned around the vertical axis: _Lathe").lathe(
        (0, 0, 0), [(R * 0.6, 0), (R * 0.9, bh * 0.27), (R, bh * 0.53), (R * 0.85, bh * 0.87), (R * 0.55, bh)]).color(col)
    o.part("Lid", "lid and knob on top: _Lathe").lathe(
        (0, 0, bh - 2), [(R * 0.55, 0), (R * 0.45, H * 0.1), (R * 0.12, H * 0.15), (R * 0.1, H * 0.19), (R * 0.16, H * 0.22), (R * 0.02, H * 0.25)]).color(col)
    o.part("Spout", "spout rising from the side: _SectionLoft").sloft(
        [((0, -R * 0.8, bh * 0.4), 36, 36), ((0, -R * 1.25, bh * 0.65), 24, 24), ((0, -R * 1.6, bh * 0.95), 15, 15)]).color(col)
    o.part("Handle", "loop handle: _PipeThrough").pipe(
        8, [(0, R * 0.8, bh * 0.8), (0, R * 1.35, bh * 0.87), (0, R * 1.55, bh * 0.6), (0, R * 1.35, bh * 0.33), (0, R * 0.85, bh * 0.27)]).color(col)
    return q, o


def bowed(r):
    """Violino, viola, violoncello, contrabbasso: stessa famiglia, misure diverse."""
    kind = r.choice(["violino", "violino", "viola", "violoncello", "violoncello", "contrabbasso"])
    S = {"violino": 1.0, "viola": 1.15, "violoncello": 2.1, "contrabbasso": 3.15}[kind] * r.uniform(0.97, 1.03)
    q = ask(r, {"violino": "un violino", "viola": "una viola", "violoncello": "un violoncello",
                "contrabbasso": "un contrabbasso"}[kind], r.choice(["", "", "con le corde", "con l'archetto no"]))
    q = q.replace(" con l'archetto no", "")
    BL, BW, rib = 356 * S, 208 * S, 32 * S * (1.25 if S > 2 else 1)
    L = 595 * S
    o = Obj("%s lying on its back, length along Y (%s): body %s long and %s wide, ribs %s high" % (
        kind.capitalize().replace("Violino", "Violin").replace("Violoncello", "Cello").replace("Contrabbasso", "Double bass").replace("Viola", "Viola"),
        n(L), n(BL), n(BW), n(rib)),
            "hourglass body with two f-holes and an arched top, neck ending in a scroll, four strings over a tall bridge, "
            "black fingerboard and tailpiece" + (", endpin at the bottom" if S > 2 else ""))
    w = BW / 2
    half = [(0, BL), (w * 0.43, BL * 0.986), (w * 0.69, BL * 0.947), (w * 0.81, BL * 0.876), (w * 0.79, BL * 0.806),
            (w * 0.65, BL * 0.747), (w * 0.55, BL * 0.697), (w * 0.53, BL * 0.604), (w * 0.54, BL * 0.534), (w * 0.58, BL * 0.463),
            (w * 0.69, BL * 0.416), (w * 0.88, BL * 0.365), (w * 0.98, BL * 0.281), (w * 0.99, BL * 0.197), (w * 0.91, BL * 0.107),
            (w * 0.72, BL * 0.039), (w * 0.40, BL * 0.006), (0, 0)]
    col = r.choice([(165, 85, 30), (150, 70, 25), (180, 100, 40)])
    B = o.part("Body", "hourglass outline (_SymCurve) extruded for the ribs, arched top lofted to a smaller copy, f-holes as thin _PipeThrough strokes on the top")
    B.symext(half, 0, rib)
    arch = rib * 0.44
    B.raw("_SymCurve %s _Enter => Rim" % " ".join(p(x, y, rib) for x, y in half))
    B.raw("_Copy 0,0,0 0,0,%s _Enter @Rim => Crest" % n(arch))
    B.raw("_Scale %s 0.55 @Crest" % p(0, BL / 2, rib + arch))
    B.raw("_Loft @Rim @Crest => Arch")
    B.raw("_Cap @Arch")
    B.raw("_BooleanUnion @Body @Arch => Body")
    B.delete("Rim", "Crest")
    B.color(col)
    # f-hole: due tratti appoggiati sulla tavola (niente tagli: sulla tavola bombata sono lenti e fragili),
    # creati dopo il colore della cassa, quindi restano del colore predefinito
    for sx in (1, -1):
        B.pipe(2.5 * S, [(sx * w * 0.38, BL * 0.58, rib + arch * 0.7), (sx * w * 0.32, BL * 0.53, rib + arch * 0.8),
                         (sx * w * 0.33, BL * 0.48, rib + arch * 0.8), (sx * w * 0.39, BL * 0.43, rib + arch * 0.8),
                         (sx * w * 0.36, BL * 0.385, rib + arch * 0.7)])
    B.pt((0, 0, rib + arch))
    nz = rib + 4 * S
    o.part("Neck", "neck along Y (_SectionLoft), scroll at the end (_Torus turned upright, _Sphere)").sloft(
        [((0, BL - 11 * S, nz), 34 * S, 24 * S), ((0, BL + 64 * S, nz + S), 30 * S, 22 * S), ((0, BL + 126 * S, nz + 2 * S), 26 * S, 20 * S)]).sloft(
        [((0, BL + 122 * S, nz + 4 * S), 26 * S, 26 * S), ((0, BL + 156 * S, nz + 6 * S), 24 * S, 26 * S), ((0, BL + 192 * S, nz + 8 * S), 22 * S, 24 * S)]).raw(
        "_Torus %s %s %s => Neck" % (p(0, L - 32 * S, nz + 12 * S), n(16 * S), n(7 * S))).raw(
        "_Rotate3D %s %s 90 @Neck" % (p(0, L - 32 * S, nz + 12 * S), p(0, L - 31 * S, nz + 12 * S)))
    o.parts[-1].pt((0, L - 32 * S, nz + 12 * S), (23 * S, 23 * S, 23 * S))
    o.parts[-1].sphere((0, L - 32 * S, nz + 12 * S), 10 * S).color(col)
    fz = nz + 16 * S
    o.part("Fingerboard", "black tapered board above the neck and the top: _Plate").plate(
        6 * S, [(-12 * S, BL + 126 * S, fz), (12 * S, BL + 126 * S, fz), (21 * S, BL * 0.59, fz), (-21 * S, BL * 0.59, fz)]).color((30, 22, 20))
    P = o.part("Pegs", "four pegs across the pegbox with oval heads: _PipeThrough, _Ellipsoid")
    for i in range(4):
        y = BL + 134 * S + 16 * S * i
        P.pipe(3 * S, [(-27 * S, y, nz + 6 * S + S * i * 0.3), (27 * S, y, nz + 6 * S + S * i * 0.3)])
    for i in range(4):
        y = BL + 134 * S + 16 * S * i
        P.ellipsoid(((-30 if i % 2 == 0 else 30) * S, y, nz + 6 * S), 3 * S, 8 * S, 8 * S)
    P.color((30, 22, 20))
    by = BL * 0.45
    o.part("Bridge", "thin upright bridge standing on the top: _Plate in the plane y = %s" % n(by)).plate(
        3 * S, [(-21 * S, by, rib + arch - 2), (21 * S, by, rib + arch - 2), (16 * S, by, rib + arch + 30 * S), (-16 * S, by, rib + arch + 30 * S)]).color((235, 205, 150))
    T = o.part("Tailpiece", "black tailpiece near the bottom end: _SymCurve, _ExtrudeCrv" + (", endpin: _PipeThrough" if S > 2 else ""))
    T.symext([(0, BL * 0.38), (20 * S, BL * 0.365), (16 * S, BL * 0.22), (9 * S, BL * 0.085), (0, BL * 0.07)], rib + arch * 0.4, 4 * S, tmp="TpOutline")
    if S > 2:
        T.pipe(6 * S, [(0, 4 * S, rib / 2), (0, -60 * S, rib / 2)])
    T.color((30, 22, 20))
    Sg = o.part("Strings", "four strings from the tailpiece over the bridge to the nut: _PipeThrough")
    top = rib + arch + 30 * S
    for i, xs in enumerate([-1.5, -0.5, 0.5, 1.5]):
        Sg.pipe(0.6 * S, [(xs * 5 * S, BL * 0.365, rib + arch * 0.4 + 4 * S), (xs * 12 * S, by, top)])
        Sg.pipe(0.6 * S, [(xs * 12 * S, by, top), (xs * 7 * S, BL + 126 * S, fz + 4 * S)])
    Sg.color((215, 215, 205))
    return q, o


def guitar(r):
    electric = r.random() < 0.45
    q = ask(r, "una chitarra elettrica" if electric else r.choice(["una chitarra", "una chitarra acustica", "una chitarra classica"]))
    BL = r.step(440, 520, 10) if not electric else r.step(400, 460, 10)
    BW = r.step(340, 400, 10) if not electric else r.step(320, 360, 10)
    D = r.step(90, 110, 5) if not electric else r.step(40, 50, 5)
    NL = r.step(440, 500, 10)
    L = BL + NL + 180
    o = Obj("%s guitar lying on its back, length along Y (%s), body %s long, %s wide, %s deep, top at z = %s" % (
        "Electric" if electric else "Acoustic", n(L), n(BL), n(BW), n(D), n(D)),
            ("solid flat body with two cutaways, two pickups and control knobs, long neck, six strings, no sound hole" if electric
             else "figure-of-eight hollow body with a round sound hole, long neck, flat headstock with six pegs, six strings"))
    w = BW / 2
    if electric:
        half = [(0, BL), (w * 0.35, BL * 0.98), (w * 0.62, BL * 1.04), (w * 0.72, BL * 0.93), (w * 0.62, BL * 0.78),
                (w * 0.66, BL * 0.62), (w * 0.9, BL * 0.42), (w, BL * 0.22), (w * 0.8, BL * 0.06), (w * 0.4, BL * 0.005), (0, 0)]
    else:
        half = [(0, BL), (w * 0.45, BL * 0.985), (w * 0.68, BL * 0.92), (w * 0.72, BL * 0.81), (w * 0.62, BL * 0.66),
                (w * 0.6, BL * 0.58), (w * 0.75, BL * 0.47), (w * 0.95, BL * 0.33), (w, BL * 0.18), (w * 0.85, BL * 0.06), (w * 0.45, BL * 0.005), (0, 0)]
    col = r.choice([(205, 155, 95), (200, 30, 30), (35, 35, 35), (240, 240, 240), (40, 70, 170)] if electric else [(205, 155, 95), (190, 140, 90), (150, 90, 40)])
    B = o.part("Body", ("solid body outline (_SymCurve) extruded %s" if electric else "body outline (_SymCurve) extruded %s, round sound hole cut") % n(D))
    B.symext(half, 0, D)
    if not electric:
        B.cyl((0, BL * 0.62, D - 8), BW * 0.12, 20, name="Hole", measure=False).cut("Hole")
    B.color(col)
    o.part("Neck", "neck along Y: _SectionLoft").sloft(
        [((0, BL * 0.9, D - 20), 56, 36), ((0, BL + NL * 0.5, D - 18), 52, 34), ((0, BL + NL, D - 17), 48, 32)]).color((120, 70, 30))
    o.part("Fingerboard", "dark tapered board on the neck: _Plate").plate(
        6, [(-22, BL + NL, D + 2), (22, BL + NL, D + 2), (28, BL * (0.78 if electric else 0.7), D + 2), (-28, BL * (0.78 if electric else 0.7), D + 2)]).color((40, 30, 25))
    H = o.part("Head", "flat headstock with six tuning pegs: _Box, _PipeThrough")
    H.box(-45, BL + NL - 10, D - 22, 45, BL + NL + 180, 16)
    for i in range(3):
        y = BL + NL + 40 + 45 * i
        H.pipe(4, [(-60, y, D - 14), (60, y, D - 14)])
    H.color((120, 70, 30))
    by = BL * 0.25
    o.part("Bridge", "bridge on the top: _Box").box(-80 if not electric else -45, by - 15, D - 1, 80 if not electric else 45, by + 15, 11).color((40, 30, 25))
    if electric:
        P = o.part("Pickups", "two pickups under the strings and three knobs: _Box, _Cylinder")
        P.box(-40, BL * 0.42, D - 1, 40, BL * 0.42 + 20, 6).box(-40, BL * 0.58, D - 1, 40, BL * 0.58 + 20, 6)
        for i in range(3):
            P.cyl((w * 0.55, BL * (0.15 + 0.08 * i), D - 1), 9, 14)
        P.color((30, 30, 30))
    S = o.part("Strings", "six thin strings from the bridge to the nut: _PipeThrough")
    for i in range(6):
        x = (i - 2.5) * 10
        S.pipe(0.7, [(x, by, D + 11), (x * 0.72, BL + NL - 2, D + 9)])
    S.color((215, 215, 205))
    return q, o


def bicycle(r):
    R = r.step(310, 360, 5)
    WB = r.step(980, 1100, 10)
    q = ask(r, r.choice(["una bicicletta", "una bici", "una bicicletta da corsa", "una mountain bike"]))
    yr, yf = R, R + WB
    zc = R
    o = Obj("Bicycle %s long (along Y, front wheel at y = %s), wheels standing in the plane x = 0, on z = 0" % (n(WB + 2 * R), n(yf)),
            "two large spoked wheels, diamond frame of tubes, handlebar at the front, saddle on top, pedals and cranks in the middle")
    tc = r.choice([(200, 30, 30), (40, 70, 170), (35, 35, 35), (40, 130, 60), (240, 240, 240)])
    o.part("Wheels", "rear and front spoked wheels, axle along X: _Wheel").wheel((0, yr, zc), R, 40, "x", 24).wheel((0, yf, zc), R, 40, "x", 24).color((30, 30, 30))
    yb = yr + WB * 0.42
    zb = zc * 0.85
    yseat, zseat = yr + WB * 0.3, zc + R * 1.55
    yhead, zhead = yf - WB * 0.1, zc + R * 1.6
    F = o.part("Frame", "diamond frame: seat tube, top tube, down tube, chain stays and seat stays, fork: _PipeThrough")
    for a, b in [((0, yb, zb), (0, yseat, zseat)), ((0, yseat, zseat), (0, yhead, zhead)), ((0, yb, zb), (0, yhead, zhead - 80)),
                 ((0, yb, zb), (0, yr, zc)), ((0, yseat, zseat), (0, yr, zc)), ((0, yhead, zhead + 40), (0, yf, zc))]:
        F.pipe(16, [a, b])
    F.color(tc)
    o.part("Handlebar", "stem and handlebar across the bike: _PipeThrough").pipe(
        14, [(0, yhead, zhead + 40), (0, yhead - 20, zhead + 110)]).pipe(12, [(-220, yhead - 30, zhead + 110), (220, yhead - 30, zhead + 110)]).color((40, 40, 40))
    o.part("Saddle", "saddle on the seat post: _SectionLoft").pipe(13, [(0, yseat, zseat), (0, yseat - 15, zseat + 110)]).sloft(
        [((0, yseat - 130, zseat + 125), 150, 40), ((0, yseat - 40, zseat + 128), 90, 35), ((0, yseat + 110, zseat + 125), 40, 25)]).color((35, 35, 35))
    P = o.part("Pedals", "crank arms and pedals at the bottom bracket: _PipeThrough, _Box")
    cl = 170
    P.pipe(10, [(-70, yb, zb), (70, yb, zb)]).pipe(9, [(70, yb, zb), (70, yb + cl * 0.7, zb - cl * 0.7)]).pipe(9, [(-70, yb, zb), (-70, yb - cl * 0.7, zb + cl * 0.7)])
    P.box(75, yb + cl * 0.7 - 50, zb - cl * 0.7 - 12, 175, yb + cl * 0.7 + 50, 24).box(-175, yb - cl * 0.7 - 50, zb + cl * 0.7 - 12, -75, yb - cl * 0.7 + 50, 24)
    P.color((60, 60, 60))
    return q, o


def car(r):
    kind = r.choice(["auto", "sportiva", "suv", "furgone"])
    L = {"auto": r.step(3900, 4600, 50), "sportiva": r.step(4200, 4700, 50), "suv": r.step(4400, 4900, 50), "furgone": r.step(4800, 5400, 50)}[kind]
    W = r.step(1750, 1950, 10)
    Hb = {"auto": 650, "sportiva": 500, "suv": 850, "furgone": 1100}[kind]
    Rw = {"auto": 320, "sportiva": 340, "suv": 380, "furgone": 340}[kind]
    zb = Rw * 0.9
    colname = r.choice(list(COLORS.keys()))
    noun = {"auto": r.choice(["un'automobile", "una macchina", "un'auto"]), "sportiva": r.choice(["un'automobile sportiva", "una macchina da corsa", "un'auto sportiva"]),
            "suv": "un SUV", "furgone": "un furgone"}[kind]
    q = ask(r, noun, r.choice(["", colname if kind != "suv" else ""]))
    col = COLORS[colname] if colname in q else r.choice(list(COLORS.values()))
    o = Obj("%s %s long (along Y, front at y = %s), %s wide, wheels standing on z = 0" % (
        {"auto": "Car", "sportiva": "Sports car", "suv": "SUV", "furgone": "Van"}[kind], n(L), n(L), n(W)),
            {"auto": "rounded body, glass cabin in the middle, four wheels at the corners, headlights at the front",
             "sportiva": "very low wedge-shaped body, small low cabin set back, wide wheels, rear spoiler",
             "suv": "tall boxy body, high cabin almost as long as the body, big wheels, roof rails",
             "furgone": "tall box-shaped body, short sloped nose, cargo area at the back, four wheels"}[kind])
    w = W / 2
    half = [(0, L), (w * 0.65, L * 0.993), (w * 0.94, L * 0.95), (w, L * 0.84), (w, L * 0.19), (w * 0.95, L * 0.05), (w * 0.66, L * 0.002), (0, 0)]
    o.part("Body", "lower body with a rounded outline seen from above: _SymCurve, _ExtrudeCrv %s at z %s" % (n(Hb), n(zb))).symext(half, zb, Hb).color(col)
    cz = zb + Hb
    cl = {"auto": 0.48, "sportiva": 0.36, "suv": 0.62, "furgone": 0.75}[kind]
    ch = {"auto": 520, "sportiva": 380, "suv": 650, "furgone": 900}[kind]
    y0 = L * (0.22 if kind != "sportiva" else 0.3)
    y1 = y0 + L * cl
    secs = [((0, y0, cz), W * 0.75, 100), ((0, y0 + (y1 - y0) * 0.2, cz), W * 0.82, ch * 1.7),
            ((0, y0 + (y1 - y0) * 0.8, cz), W * 0.8, ch * 1.7), ((0, y1, cz), W * 0.7, 100)]
    o.part("Cabin", "cabin with the windows on top of the body: _SectionLoft along Y").sloft(secs).color((120, 160, 200))
    Wh = o.part("Wheels", "four wheels at the corners, axle along X: _Wheel, then _Mirror")
    ww = r.step(200, 280, 10)
    for y in (L * 0.17, L * 0.8):
        Wh.wheel((w - ww / 2 + 20, y, Rw), Rw, ww, "x", 0)
    Wh.mirror().color((30, 30, 30))
    o.part("Lights", "two headlights at the front: _Ellipsoid, then _Mirror").ellipsoid((w * 0.7, L - 25, zb + Hb * 0.6), 120, 40, 60).mirror().color((250, 240, 170))
    if kind == "sportiva":
        Sp = o.part("Spoiler", "rear spoiler on two struts: _Plate, _PipeThrough")
        Sp.pipe(15, [(w * 0.5, L * 0.04, cz - 10), (w * 0.5, L * 0.04, cz + 160)]).mirror()
        Sp.plate(20, [(-w * 0.9, -20, cz + 170), (w * 0.9, -20, cz + 170), (w * 0.9, L * 0.06, cz + 170), (-w * 0.9, L * 0.06, cz + 170)]).color(r.choice(DARK))
    if kind == "suv":
        o.part("Rails", "two roof rails on top of the cabin: _PipeThrough, then _Mirror").pipe(
            15, [(w * 0.5, y0 + (y1 - y0) * 0.2, cz + ch * 0.85 + 15), (w * 0.5, y0 + (y1 - y0) * 0.8, cz + ch * 0.85 + 15)]).mirror().color(r.choice(METAL))
    return q, o


def truck(r):
    L, W = r.step(6500, 9000, 100), r.step(2300, 2500, 50)
    q = ask(r, r.choice(["un camion", "un autocarro", "un camion con il cassone"]))
    Rw = 500
    o = Obj("Truck %s long (along Y, front at y = %s), %s wide, on z = 0" % (n(L), n(L), n(W)),
            "tall cab at the front, long cargo box behind it, six large wheels on three axles")
    w = W / 2
    cabL = 2200
    col = r.choice(list(COLORS.values()))
    o.part("Chassis", "long flat chassis above the axles: _Box").box(-w * 0.8, 300, Rw * 1.4, w * 0.8, L - 200, 300).color((40, 40, 40))
    o.part("Cab", "cab with a rounded front: _Box, _SectionLoft for the roof").box(-w, L - cabL, Rw * 1.4, w, L, 1700).sloft(
        [((0, L - cabL, Rw * 1.4 + 1700), W * 0.9, 120), ((0, L - cabL / 2, Rw * 1.4 + 1700), W * 0.95, 400), ((0, L - 50, Rw * 1.4 + 1700), W * 0.9, 120)]).color(col)
    o.part("Cargo", "large cargo box behind the cab: _Box").box(-w, 0, Rw * 1.4 + 300, w, L - cabL - 100, 2600).color(r.choice([(240, 240, 240), (150, 150, 155), (200, 200, 205)]))
    Wh = o.part("Wheels", "six wheels on three axles, axle along X: _Wheel, then _Mirror")
    for y in (L - cabL * 0.55, L * 0.28, L * 0.28 - 1200):
        Wh.wheel((w - 160, y, Rw), Rw, 300, "x", 0)
    Wh.mirror().color((30, 30, 30))
    return q, o


def tractor(r):
    Rr, Rf = r.step(700, 850, 10), r.step(400, 500, 10)
    WB = r.step(2300, 2700, 50)
    W = r.step(1900, 2300, 50)
    q = ask(r, r.choice(["un trattore", "un trattore agricolo", "un trattore con la cabina"]))
    w = W / 2
    o = Obj("Tractor %s long (along Y, front at y = %s), %s wide, on z = 0" % (n(WB + Rr + Rf), n(WB + Rr + Rf), n(W)),
            "very large rear wheels, small front wheels, long engine hood at the front, square glass cabin over the rear axle, exhaust pipe")
    yr, yf = Rr, Rr + WB
    col = r.choice([(40, 130, 60), (200, 30, 30), (40, 70, 170), (230, 200, 40)])
    Wh = o.part("Wheels", "two big rear wheels and two small front wheels, axle along X: _Wheel, then _Mirror")
    Wh.wheel((w - 250, yr, Rr), Rr, 500, "x", 0).wheel((w - 400, yf, Rf), Rf, 300, "x", 0).mirror().color((30, 30, 30))
    o.part("Body", "chassis between the axles: _Box").box(-450, yr - 300, Rf * 0.9, 450, yf + 200, 500).color(col)
    o.part("Hood", "long engine hood at the front: _SectionLoft along Y").sloft(
        [((0, yr + 600, Rf * 0.9 + 500), 800, 900), ((0, yf - 200, Rf * 0.9 + 480), 760, 850), ((0, yf + Rf, Rf * 0.9 + 420), 640, 700)]).color(col)
    cz = Rf * 0.9 + 500
    C = o.part("Cabin", "square cabin with a roof over the rear axle: _Box, _SectionLoft for the windows")
    C.box(-w + 300, yr - 500, cz, w - 300, yr + 700, 200).sloft(
        [((0, yr + 100, cz + 200), W - 650, 50), ((0, yr + 100, cz + 1300), W - 650, 1150), ((0, yr + 100, cz + 1500), W - 600, 50)])
    C.box(-w + 250, yr - 550, cz + 1500, w - 250, yr + 750, 80).color((120, 160, 200))
    o.part("Exhaust", "vertical exhaust pipe beside the hood: _PipeThrough").pipe(45, [(330, yf - 500, cz + 300), (330, yf - 500, cz + 1500)]).color((60, 60, 60))
    return q, o


def helicopter(r):
    L = r.step(10000, 14000, 500)
    Rr = r.step(5000, 6500, 250)
    q = ask(r, r.choice(["un elicottero", "un elicottero da soccorso", "un elicottero con i pattini"]))
    o = Obj("Helicopter %s long (along Y, nose at y = %s), main rotor diameter %s, on skids at z = 0" % (n(L), n(L), n(2 * Rr)),
            "round cabin with a big glass nose, long thin tail boom, main rotor on a mast on top, small tail rotor on the side of the tail, landing skids")
    col = r.choice([(200, 30, 30), (240, 240, 240), (230, 200, 40), (40, 70, 170)])
    fz = 600
    cabL = L * 0.38
    y0 = L - cabL
    o.part("Fuselage", "rounded cabin with the engine on top: _SectionLoft along Y").sloft(
        [((0, y0, fz + 1100), 1600, 1600), ((0, y0 + cabL * 0.4, fz + 1100), 2300, 2200), ((0, y0 + cabL * 0.8, fz + 1000), 1900, 1800), ((0, L, fz + 800), 500, 500)]).color(col)
    o.part("Tail", "long tail boom and a vertical fin at its end: _SectionLoft, _Plate").sloft(
        [((0, y0 + 200, fz + 1500), 900, 900), ((0, L * 0.3, fz + 1600), 450, 450), ((0, 300, fz + 1700), 280, 280)]).plate(
        80, [(0, 0, fz + 1500), (0, 700, fz + 1600), (0, 500, fz + 2900), (0, 0, fz + 3000)]).color(col)
    hz = fz + 2200 + 500
    o.part("Rotor", "mast and main rotor with four blades, axis along Z: _PipeThrough, _Blades").pipe(
        100, [(0, y0 + cabL * 0.4, fz + 2000), (0, y0 + cabL * 0.4, hz)]).blades((0, y0 + cabL * 0.4, hz), "z", 4, Rr, 400, 60).color((60, 60, 60))
    o.part("TailRotor", "small tail rotor on the side of the fin, axis along X: _Blades").blades((180, 300, fz + 2300), "x", 2, 1000, 200, 40).color((60, 60, 60))
    S = o.part("Skids", "two landing skids with struts: _PipeThrough, then _Mirror")
    S.pipe(60, [(900, y0 - 300, 60), (900, L - 600, 60), (900, L - 300, 250)]).pipe(50, [(900, y0 + 300, 60), (600, y0 + 300, fz + 300)]).pipe(
        50, [(900, y0 + cabL * 0.7, 60), (600, y0 + cabL * 0.7, fz + 300)]).mirror().color((60, 60, 60))
    return q, o


def airplane(r):
    small = r.random() < 0.4
    L = r.step(7000, 9000, 250) if small else r.step(30000, 45000, 1000)
    span = L * r.uniform(1.1, 1.3) if small else L * r.uniform(0.85, 1.0)
    q = ask(r, r.choice(["un aereo da turismo", "un piccolo aereo a elica"]) if small else r.choice(["un aereo", "un aereo di linea", "un aeroplano"]))
    D = L * (0.12 if small else 0.1)
    fz = D * 0.4 + (L * 0.02)
    o = Obj("%s, length along Y (%s, nose at y = %s), wingspan %s, fuselage %s high" % ("Light propeller plane" if small else "Airliner", n(L), n(L), n(span), n(D)),
            ("high straight wings, propeller at the nose, small tail fin and tail wings" if small else
             "long round fuselage, two swept wings with engines below, tall tail fin and small tail wings"))
    col = (240, 240, 245)
    o.part("Fuselage", "long round fuselage along Y: _SectionLoft").sloft(
        [((0, 0, fz + D * 0.9), D * 0.1, D * 0.15), ((0, L * 0.12, fz + D * 0.6), D * 0.6, D * 0.6), ((0, L * 0.5, fz + D / 2), D, D),
         ((0, L * 0.88, fz + D / 2), D * 0.95, D * 0.95), ((0, L, fz + D * 0.45), D * 0.2, D * 0.2)]).color(col)
    s = span / 2
    if small:
        wz = fz + D * 0.95
        o.part("Wings", "straight high wing over the cabin: _Plate").plate(
            L * 0.02, [(-s, L * 0.55, wz), (s, L * 0.55, wz), (s, L * 0.7, wz), (-s, L * 0.7, wz)]).color(col)
        o.part("Propeller", "two-blade propeller at the nose, axis along Y: _Blades").blades((0, L + 40, fz + D * 0.45), "y", 2, D * 0.9, D * 0.12, D * 0.04).color((40, 40, 40))
    else:
        wz = fz + D * 0.25
        o.part("Wings", "swept wing, then _Mirror: _Plate").plate(
            D * 0.1, [(D * 0.4, L * 0.55, wz), (s, L * 0.38, wz + D * 0.1), (s, L * 0.33, wz + D * 0.1), (D * 0.4, L * 0.42, wz)]).mirror().color((200, 200, 210))
        o.part("Engines", "engine under each wing, then _Mirror: _SectionLoft").sloft(
            [((s * 0.38, L * 0.56, wz - D * 0.3), D * 0.45, D * 0.45), ((s * 0.38, L * 0.52, wz - D * 0.3), D * 0.55, D * 0.55), ((s * 0.38, L * 0.47, wz - D * 0.3), D * 0.4, D * 0.4)]).mirror().color((150, 150, 160))
    o.part("Fin", "vertical tail fin: _Plate").plate(D * 0.06, [(0, L * 0.12, fz + D * 0.8), (0, 0, fz + D * 0.9), (0, -L * 0.02, fz + D * 2.1), (0, L * 0.05, fz + D * 2.1)]).color((40, 70, 160))
    o.part("Stabilizers", "small horizontal tail wings, then _Mirror: _Plate").plate(
        D * 0.05, [(D * 0.1, L * 0.1, fz + D * 0.8), (span * 0.17, L * 0.03, fz + D * 0.85), (span * 0.17, 0, fz + D * 0.85), (D * 0.1, 0, fz + D * 0.8)]).mirror().color((40, 70, 160))
    return q, o


def drone(r):
    A = r.step(180, 320, 10)
    q = ask(r, r.choice(["un drone", "un drone quadricottero", "un quadricottero", "un drone con quattro eliche"]))
    o = Obj("Quadcopter drone %s across, flat on z = 0" % n(2 * A + 150),
            "small central body, four arms in an X, a motor and a propeller at the end of each arm, landing legs")
    o.part("Body", "central body: _Box, camera under it: _Sphere").cbox(0, 0, 40, A * 0.7, A, 50).sphere((0, A * 0.3, 35), 22).color((40, 40, 40))
    P = o.part("Arms", "four arms in an X from the body to the motors: _PipeThrough")
    ends = [(A * math.cos(a), A * math.sin(a)) for a in (math.pi / 4, 3 * math.pi / 4, 5 * math.pi / 4, 7 * math.pi / 4)]
    for x, y in ends:
        P.pipe(10, [(0, 0, 65), (x, y, 65)])
    P.color((60, 60, 60))
    M = o.part("Motors", "a motor on the end of each arm: _Cylinder")
    for x, y in ends:
        M.cyl((x, y, 55), 18, 30)
    M.color((150, 150, 160))
    Pr = o.part("Propellers", "two-blade propeller on each motor, axis along Z: _Blades")
    pl = r.step(70, 110, 5)
    for x, y in ends:
        Pr.blades((x, y, 92), "z", 2, pl, 18, 3)
    Pr.color(r.choice([(200, 30, 30), (240, 130, 30), (240, 240, 240)]))
    Lg = o.part("Legs", "two landing legs under the body: _PipeThrough, then _Mirror")
    Lg.pipe(5, [(A * 0.2, -A * 0.35, 40), (A * 0.3, -A * 0.35, 4), (A * 0.3, A * 0.35, 4), (A * 0.2, A * 0.35, 40)]).mirror().color((40, 40, 40))
    return q, o


def sailboat(r):
    L = r.step(6000, 12000, 500)
    B = L * r.uniform(0.3, 0.36)
    q = ask(r, r.choice(["una barca a vela", "una barca", "un veliero", "una barca a vela di %s" % size_text(r, L)]))
    o = Obj("Sailboat %s long (along Y, bow at y = %s), %s wide: hull up to the deck at z = %s, keel below z = 0, mast and sail" % (n(L), n(L), n(B), n(L * 0.15)),
            "pointed hull narrowing at the bow, fin keel below, tall mast with a triangular sail")
    w, dz = B / 2, L * 0.15
    half = [(0, L), (w * 0.55, L * 0.9), (w * 0.92, L * 0.69), (w, L * 0.44), (w * 0.88, L * 0.12), (w * 0.62, 0), (0, 0)]
    H = o.part("Hull", "deck outline (_SymCurve) at z %s lofted to a smaller copy at z 0 (_Copy, _Scale, _Loft, _Cap)" % n(dz))
    H.raw("_SymCurve %s _Enter => Deck" % " ".join(p(x, y, dz) for x, y in half))
    H.raw("_Copy 0,0,0 0,0,%s _Enter @Deck => Bottom" % n(-dz)).raw("_Scale %s 0.6 @Bottom" % p(0, L / 2, 0))
    H.raw("_Loft @Deck @Bottom => Hull").raw("_Cap @Hull").delete("Deck", "Bottom")
    for x, y in half:
        H.pt((x, y, dz))
        H.pt((-x, y, 0))
    H.color(r.choice([(240, 240, 240), (40, 70, 170), (200, 30, 30)]))
    o.part("Keel", "fin keel under the hull: _Plate in the plane x = 0").plate(
        L * 0.025, [(0, L * 0.44, 50), (0, L * 0.62, 50), (0, L * 0.59, -L * 0.17), (0, L * 0.5, -L * 0.17)]).color((60, 60, 70))
    mh = L * 1.2
    o.part("Mast", "tall mast: _PipeThrough").pipe(L * 0.01, [(0, L * 0.6, dz - 50), (0, L * 0.6, dz + mh)]).color((180, 180, 190))
    o.part("Sail", "triangular main sail behind the mast: _Plate in the plane x = 0").plate(
        40, [(0, L * 0.59, dz + 400), (0, L * 0.59, dz + mh * 0.95), (0, L * 0.22, dz + 400)]).color((250, 250, 245))
    return q, o


def fan(r):
    H = r.step(1100, 1500, 50)
    R = r.step(180, 260, 10)
    k = r.choice([3, 4, 5])
    q = ask(r, r.choice(["un ventilatore", "un ventilatore a piantana", "un ventilatore con %d pale" % k]))
    o = Obj("Standing fan %s high on z = 0: base, pole, motor on top facing +Y, blades and guard in front" % n(H + R),
            "heavy round base, thin tall pole, %d wide blades turning around a horizontal axis, round wire guard" % k)
    o.part("Base", "heavy round base: _Lathe").lathe((0, 0, 0), [(200, 0), (190, 30), (60, 60)]).color((60, 60, 60))
    o.part("Pole", "thin vertical pole: _PipeThrough").pipe(15, [(0, 0, 40), (0, 0, H - 50)]).color((200, 200, 200))
    o.part("Motor", "motor housing along Y on top of the pole: _SectionLoft").sloft(
        [((0, -150, H), 140, 140), ((0, -60, H), 160, 160), ((0, 20, H), 100, 100)]).color((230, 230, 230))
    o.part("Blades", "%d blades in front of the motor, axis along Y: _Blades" % k).blades((0, 60, H), "y", k, R - 30, R * 0.55, 8).color(r.choice([(120, 170, 220), (240, 240, 240), (230, 200, 40)]))
    o.part("Guard", "round guard around the blades: _Wheel with many thin spokes, axle along Y").wheel((0, 60, H), R, 60, "y", 24).color((200, 200, 200))
    return q, o


def wind_turbine(r):
    H = r.step(30000, 80000, 5000)
    R = H * r.uniform(0.45, 0.55)
    q = ask(r, r.choice(["una pala eolica", "una turbina eolica", "un generatore eolico"]))
    o = Obj("Wind turbine: tower %s high on z = 0, rotor diameter %s facing -Y" % (n(H), n(2 * R)),
            "very tall slender tapered tower, nacelle on top, three long thin blades on a hub at the front")
    o.part("Tower", "tapered round tower: _Lathe").lathe((0, 0, 0), [(H * 0.035, 0), (H * 0.025, H * 0.5), (H * 0.017, H)]).color((240, 240, 240))
    o.part("Nacelle", "nacelle on top of the tower along Y: _SectionLoft").sloft(
        [((0, H * 0.08, H + H * 0.02), H * 0.05, H * 0.05), ((0, -H * 0.02, H + H * 0.02), H * 0.06, H * 0.06), ((0, -H * 0.09, H + H * 0.02), H * 0.045, H * 0.045)]).color((230, 230, 230))
    o.part("Rotor", "three long blades on the hub, axis along Y: _Blades").blades((0, -H * 0.11, H + H * 0.02), "y", 3, R, R * 0.07, R * 0.012).color((245, 245, 245))
    return q, o


def rocket(r):
    H = r.step(1500, 60000, 500)
    R = H * r.uniform(0.045, 0.07)
    q = ask(r, r.choice(["un razzo", "un razzo spaziale", "un missile giocattolo"]))
    o = Obj("Rocket %s high standing on z = 0 on its fins" % n(H), "tall round body, pointed nose cone, four fins at the bottom, engine nozzle")
    o.part("Body", "long round body: _Lathe").lathe((0, 0, H * 0.06), [(R, 0), (R, H * 0.7)]).color((240, 240, 240))
    o.part("Nose", "pointed nose cone on top: _Lathe").lathe((0, 0, H * 0.76 - 1), [(R, 0), (R * 0.75, H * 0.1), (R * 0.05, H * 0.24)]).color((200, 30, 30))
    F = o.part("Fins", "four fins at the bottom: _Plate in the planes x = 0 and y = 0")
    for sx, sy in [(1, 0), (-1, 0), (0, 1), (0, -1)]:
        F.plate(R * 0.08, [(sx * R * 0.9, sy * R * 0.9, H * 0.3), (sx * R * 2.2, sy * R * 2.2, H * 0.06), (sx * R * 2.2, sy * R * 2.2, 0), (sx * R * 0.9, sy * R * 0.9, H * 0.06)])
    F.color((200, 30, 30))
    o.part("Nozzle", "engine nozzle under the body: _Lathe").lathe((0, 0, H * 0.01), [(R * 0.7, 0), (R * 0.45, H * 0.05 + 1)]).color((60, 60, 60))
    return q, o


def house(r):
    W, D, H = r.step(6000, 12000, 500), r.step(5000, 9000, 500), r.step(2700, 3200, 100)
    q = ask(r, r.choice(["una casa", "una casetta", "una casa con il tetto a due falde"]))
    o = Obj("House %sx%s, walls %s high on z = 0, gable roof along Y" % (n(W), n(D), n(H)),
            "box-shaped walls, pitched roof with two slopes, front door, windows, chimney on the roof")
    o.part("Walls", "walls of the house: _Box").box(0, 0, 0, W, D, H).color(r.choice([(240, 230, 210), (230, 200, 160), (240, 240, 240)]))
    rh = W * 0.3
    R = o.part("Roof", "two roof slopes meeting at the ridge: _Plate")
    R.plate(150, [(-300, -300, H), (W / 2, -300, H + rh), (W / 2, D + 300, H + rh), (-300, D + 300, H)])
    R.plate(150, [(W + 300, -300, H), (W / 2, -300, H + rh), (W / 2, D + 300, H + rh), (W + 300, D + 300, H)]).color((170, 60, 40))
    o.part("Door", "front door on the wall y = 0: _Box").box(W / 2 - 450, -60, 0, W / 2 + 450, 20, 2100).color((120, 70, 30))
    Wn = o.part("Windows", "windows on the front wall: _Box")
    for x in (W * 0.2, W * 0.8):
        Wn.box(x - 600, -50, 1000, x + 600, 20, 1200)
    Wn.color((120, 160, 200))
    o.part("Chimney", "chimney on the roof: _Box").box(W * 0.68, D * 0.6, H + rh * 0.3, W * 0.68 + 600, D * 0.6 + 600, rh * 0.9).color((150, 60, 50))
    return q, o


def tree(r):
    H = r.step(3000, 9000, 250)
    q = ask(r, r.choice(["un albero", "un pino", "un abete", "un albero da frutto"]))
    conifer = any(k in q for k in ("pino", "abete"))
    o = Obj("%s %s high standing on z = 0" % ("Conifer" if conifer else "Tree", n(H)),
            "brown trunk, " + ("tall green cone of branches in layers" if conifer else "big round green crown of leaves on top"))
    tr = H * 0.035
    o.part("Trunk", "round trunk: _Lathe").lathe((0, 0, 0), [(tr * 1.4, 0), (tr, H * 0.15), (tr * 0.8, H * 0.5)]).color((110, 70, 40))
    if conifer:
        C = o.part("Crown", "three green cones stacked: _Lathe")
        for i in range(3):
            z = H * (0.2 + 0.25 * i)
            C.lathe((0, 0, z), [(H * (0.28 - 0.06 * i), 0), (H * 0.02, H * 0.35)])
    else:
        o.part("Crown", "round crown of leaves: _Ellipsoid").ellipsoid((0, 0, H * 0.62), H * 0.33, H * 0.33, H * 0.38)
    o.parts[-1].color((40, 120, 50))
    return q, o


def snowman(r):
    R = r.step(250, 450, 10)
    q = ask(r, r.choice(["un pupazzo di neve", "un pupazzo di neve con il cappello"]))
    o = Obj("Snowman about %s high standing on z = 0" % n(R * 5.4), "three white balls getting smaller upwards, black hat, carrot nose, stick arms")
    z1, z2, z3 = R, 2 * R + R * 0.6, 2 * R + 1.2 * R + R * 0.4
    o.part("Body", "three balls stacked: _Sphere").sphere((0, 0, z1 * 0.95), R).sphere((0, 0, z2 * 0.97), R * 0.7).sphere((0, 0, z3 * 0.98), R * 0.45).color((245, 245, 250))
    o.part("Hat", "black top hat: _Cylinder").cyl((0, 0, z3 + R * 0.3), R * 0.5, R * 0.06).cyl((0, 0, z3 + R * 0.32), R * 0.3, R * 0.5).color((30, 30, 30))
    o.part("Nose", "carrot nose pointing to -Y: _SectionLoft").sloft([((0, -R * 0.4, z3), R * 0.12, R * 0.12), ((0, -R * 0.6, z3), R * 0.07, R * 0.07), ((0, -R * 0.8, z3), R * 0.02, R * 0.02)]).color((240, 130, 30))
    o.part("Arms", "two stick arms, then _Mirror: _PipeThrough").pipe(
        R * 0.04, [(R * 0.6, 0, z2), (R * 1.1, 0, z2 + R * 0.4), (R * 1.4, 0, z2 + R * 0.5)]).mirror().color((110, 70, 40))
    return q, o


def wheelbarrow(r):
    Rw = r.step(170, 220, 5)
    q = ask(r, r.choice(["una carriola", "una carriola da giardino"]))
    o = Obj("Wheelbarrow about 1550 long (along Y, wheel at the front), standing on z = 0",
            "one spoked wheel at the front, a deep tub, two long handles going back, two short legs under the back")
    o.part("Wheel", "spoked wheel at the front, axle along X: _Wheel").wheel((0, 1200, Rw), Rw, 80, "x", 12).color((40, 40, 40))
    T = o.part("Tub", "tub: _SymCurve top outline, copied down, scaled smaller, _Loft and _Cap")
    half = [(0, 1100), (250, 1050), (330, 800), (320, 450), (250, 250), (0, 250)]
    T.raw("_SymCurve %s _Enter => Top" % " ".join(p(x, y, 650) for x, y in half))
    T.raw("_Copy 0,0,0 0,0,-270 _Enter @Top => Bottom").raw("_Scale 0,675,380 0.65 @Bottom").raw("_Loft @Top @Bottom => Tub").raw("_Cap @Tub").delete("Top", "Bottom")
    for x, y in half:
        T.pt((x, y, 650))
        T.pt((-x, y, 380))
    T.color(r.choice([(40, 120, 60), (200, 30, 30), (40, 70, 170)]))
    o.part("Handles", "two handles from the axle back to the grips: _PipeThrough, then _Mirror").pipe(
        15, [(60, 1200, Rw), (200, 800, 420), (270, 300, 560), (310, -150, 700)]).mirror().color((150, 100, 60))
    o.part("Legs", "two short legs under the back of the tub: _PipeThrough, then _Mirror").pipe(12, [(230, 450, 0), (230, 400, 480)]).mirror().color((150, 100, 60))
    return q, o


def robot(r):
    H = r.step(300, 1800, 50)
    q = ask(r, r.choice(["un robot", "un robot giocattolo", "un robottino"]))
    s = H / 1000
    o = Obj("Toy robot %s high standing on z = 0" % n(H), "box-shaped body and head, round eyes, antenna on the head, tube arms and box legs")
    o.part("Legs", "two box legs: _Box").box(-160 * s, -80 * s, 0, -40 * s, 80 * s, 360 * s).box(40 * s, -80 * s, 0, 160 * s, 80 * s, 360 * s).color((150, 150, 160))
    o.part("Body", "box body on the legs: _Box").box(-200 * s, -120 * s, 350 * s, 200 * s, 120 * s, 380 * s).color(r.choice([(40, 70, 170), (200, 30, 30), (230, 200, 40)]))
    o.part("Head", "box head with two round eyes and an antenna: _Box, _Sphere, _PipeThrough").box(
        -130 * s, -110 * s, 740 * s, 130 * s, 110 * s, 200 * s).sphere((-55 * s, -110 * s, 860 * s), 25 * s).sphere((55 * s, -110 * s, 860 * s), 25 * s).pipe(
        8 * s, [(0, 0, 935 * s), (0, 0, 1000 * s - 8 * s)]).color((180, 180, 190))
    o.part("Arms", "two arms hanging from the shoulders, then _Mirror: _PipeThrough").pipe(
        30 * s, [(200 * s, 0, 690 * s), (260 * s, -20 * s, 560 * s), (250 * s, -60 * s, 420 * s)]).mirror().color((150, 150, 160))
    return q, o


def umbrella(r):
    R = r.step(1000, 1600, 50)
    H = r.step(2200, 2600, 50)
    q = ask(r, r.choice(["un ombrellone", "un ombrellone da spiaggia", "un ombrellone da giardino"]))
    o = Obj("Patio umbrella %s high, canopy diameter %s, on z = 0" % (n(H), n(2 * R)), "heavy base, tall thin pole, wide shallow conical canopy on top")
    o.part("Base", "heavy round base: _Lathe").lathe((0, 0, 0), [(280, 0), (250, 60), (60, 90)]).color((60, 60, 60))
    o.part("Pole", "thin pole: _PipeThrough").pipe(20, [(0, 0, 60), (0, 0, H + 50)]).color((240, 240, 240))
    o.part("Canopy", "wide shallow cone of fabric: _Lathe").lathe((0, 0, H - 400), [(R, 0), (R * 0.5, 280), (30, 420)]).color(r.choice([(230, 200, 40), (200, 30, 30), (40, 70, 170), (240, 240, 240)]))
    return q, o


def motorcycle(r):
    R = r.step(300, 340, 5)
    WB = r.step(1350, 1550, 10)
    q = ask(r, r.choice(["una moto", "una motocicletta", "uno scooter"]))
    o = Obj("Motorcycle %s long (along Y, front at y = %s), wheels in the plane x = 0, on z = 0" % (n(WB + 2 * R), n(WB + 2 * R)),
            "two solid wheels, engine block between them, fuel tank and seat on top, handlebar at the front, exhaust pipe")
    col = r.choice([(200, 30, 30), (35, 35, 35), (40, 70, 170), (230, 200, 40)])
    yr, yf = R, R + WB
    o.part("Wheels", "rear and front wheels, axle along X: _Wheel").wheel((0, yr, R), R, 140, "x", 0).wheel((0, yf, R), R, 110, "x", 0).color((30, 30, 30))
    o.part("Engine", "engine block between the wheels: _Box").cbox(0, yr + WB * 0.45, R * 0.6, 300, 450, 380).color((150, 150, 160))
    o.part("Tank", "fuel tank and seat on top: _SectionLoft").sloft(
        [((0, yr + 80, R + 560), 240, 80), ((0, yr + WB * 0.4, R + 600), 280, 180), ((0, yr + WB * 0.65, R + 640), 300, 260), ((0, yr + WB * 0.8, R + 620), 200, 140)]).color(col)
    o.part("Fork", "front fork and handlebar: _PipeThrough").pipe(22, [(0, yf, R), (0, yf - 220, R + 760)]).pipe(14, [(-380, yf - 250, R + 780), (380, yf - 250, R + 780)]).color((180, 180, 190))
    o.part("Exhaust", "exhaust pipe along the right side: _PipeThrough").pipe(40, [(160, yr + WB * 0.45, R * 0.8), (200, yr + WB * 0.2, R * 0.7), (200, yr - 100, R * 0.9)]).color((180, 180, 190))
    return q, o


def piano(r):
    W = r.step(1400, 1550, 10)
    q = ask(r, r.choice(["un pianoforte verticale", "un pianoforte", "un piano verticale"]))
    o = Obj("Upright piano %s wide (along X), keyboard facing -Y, on z = 0" % n(W),
            "tall wooden case, keyboard sticking out at the front with white and black keys, stool in front")
    c = r.choice([(35, 35, 35), (120, 70, 30)])
    o.part("Case", "tall case and the keyboard shelf: _Box").box(0, 0, 0, W, 600, 1250).box(0, -200, 650, W, 0, 80).color(c)
    K = o.part("Keys", "white key bed and black keys on the shelf: _Box")
    K.box(60, -190, 730, W - 60, -40, 20)
    for i in range(r.randint(10, 18)):
        x = 100 + i * (W - 200) / 18
        K.box(x, -110, 750, x + 14, -40, 12)
    K.color((240, 240, 235))
    o.part("Stool", "piano stool in front of the keyboard: _Box").box(W / 2 - 300, -560, 0, W / 2 + 300, -199, 500).color(c)
    return q, o


FAMILIES = [
    (table, 3), (round_table, 2), (chair, 3), (stool, 2), (bookcase, 2), (floor_lamp, 2), (mug, 2), (bottle, 2),
    (teapot, 2), (bowed, 4), (guitar, 4), (bicycle, 4), (car, 4), (truck, 2), (tractor, 3), (helicopter, 3),
    (airplane, 3), (drone, 3), (sailboat, 2), (fan, 2), (wind_turbine, 2), (rocket, 2), (house, 2), (tree, 2),
    (snowman, 1), (wheelbarrow, 2), (robot, 2), (umbrella, 1), (motorcycle, 3), (piano, 1),
]


def plan_request(request):
    """Come ai::PlanRequest(request, {}, true)."""
    return ("Request: " + request + "\n\nStep 1: think about what makes this object recognisable, then write only the "
            "plan in a ```plan block: the \"# ...\" line, the \"# Distinctive: ...\" line and one PART line per part "
            "with its box in mm.")


def part_request(parts, i):
    """Come ai::PartRequest(plan, i) senza richiesta (nessuna tecnica dagli esempi)."""
    P = parts[i]
    s = ("Part %d of %d: %s - %s. Its box goes from %s. Write ONLY the commands of this part in one ```kurva block, "
         "with every coordinate inside that box, and name its final objects => %s." % (
             i + 1, len(parts), P.name, P.what, box_text(P.box_of()), P.name))
    if i > 0:
        s += " Already built:" + "".join(" " + q.name for q in parts[:i]) + " (use @Name to work on them)."
    return s


def conversation(system, request, o):
    plan = "```plan\n# %s\n# Distinctive: %s\n" % (o.summary, o.features)
    for P in o.parts:
        plan += "PART %s | %s | %s\n" % (P.name, P.what, box_text(P.box_of()))
    plan += "```"
    msgs = [{"role": "system", "content": system}, {"role": "user", "content": plan_request(request)},
            {"role": "assistant", "content": plan}]
    for i, P in enumerate(o.parts):
        msgs.append({"role": "user", "content": part_request(o.parts, i)})
        msgs.append({"role": "assistant", "content": "```kurva\n" + "\n".join(P.lines) + "\n```"})
    return {"request": request, "messages": msgs}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=400)
    ap.add_argument("--seed", type=int, default=20261127)
    ap.add_argument("--out", default="kurva3d-parts.jsonl")
    ap.add_argument("--system", default=os.path.join(HERE, "parts_system_prompt.txt"))
    a = ap.parse_args()
    system = open(a.system, encoding="utf-8").read()
    r = Rng(a.seed)
    pool = [f for f, w in FAMILIES for _ in range(w)]
    rows, seen, tries = [], set(), 0
    while len(rows) < a.count and tries < a.count * 30:
        tries += 1
        request, o = r.choice(pool)(r)
        key = request + "|" + o.summary
        if key in seen:
            continue
        seen.add(key)
        rows.append(conversation(system, request, o))
    with open(a.out, "w", encoding="utf-8") as f:
        for row in rows:
            f.write(json.dumps(row, ensure_ascii=False) + "\n")
    print("%d oggetti scritti in %s" % (len(rows), a.out))


if __name__ == "__main__":
    main()
