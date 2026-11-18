// Altri comandi di Rhino 8 (quarta parte): deformazioni (Twist, Taper, Bend),
// ArrayCrv, ConvertToPolyline, FilletCorners, BlendCrv, UnifyMeshNormals,
// FillMeshHoles.
#include "commands/Deform.h"
#include "commands/Engine.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>

namespace cad {
namespace {

const ON_Geometry* Geo(Engine& e, const ON_UUID& id) {
  const auto* o = e.Object(id);
  return o ? o->Geometry(nullptr) : nullptr;
}

int OptInt(const std::map<std::string, std::string>& o, const char* k, int def) {
  auto it = o.find(k);
  return it == o.end() ? def : std::atoi(it->second.c_str());
}

double OptNum(const std::map<std::string, std::string>& o, const char* k, double def) {
  auto it = o.find(k);
  return it == o.end() ? def : std::atof(it->second.c_str());
}

bool OptYes(const std::map<std::string, std::string>& o, const char* k, bool def) {
  auto it = o.find(k);
  if (it == o.end()) return def;
  return it->second == "yes" || it->second == "si" || it->second == "true" || it->second == "1";
}

// Applica la deformazione agli oggetti selezionati (sostituiti, stesso id).
bool ApplyDeform(Engine& e, const std::vector<ON_UUID>& sel, const PointMap& f) {
  int n = 0, skipped = 0;
  for (const auto& id : sel) {
    ON_Geometry* g = DeformGeometry(Geo(e, id), f);
    if (!g) { skipped++; continue; }
    if (!g->IsValid()) { delete g; skipped++; continue; }
    e.ReplaceObject(id, g);
    n++;
  }
  e.Out() << "  Deformati " << n << " oggetti";
  if (skipped) e.Out() << " (" << skipped << " non supportati)";
  e.Out() << "\n";
  return n > 0 || e.Fail("nessun oggetto deformato");
}

struct Axis {
  ON_3dPoint s;
  ON_3dVector x, y, z;
  double len;
};

bool MakeAxis(const ON_3dPoint& a, const ON_3dPoint& b, Axis& ax) {
  ax.s = a;
  ax.x = b - a;
  ax.len = ax.x.Length();
  if (!ax.x.Unitize()) return false;
  ax.y = ON_CrossProduct(std::fabs(ax.x.z) < 0.9 ? ON_3dVector(0, 0, 1) : ON_3dVector(1, 0, 0), ax.x);
  ax.y.Unitize();
  ax.z = ON_CrossProduct(ax.x, ax.y);
  return true;
}

// Twist: inizio e fine dell'asse, angolo (gradi). Infinite=Yes torce anche fuori dall'asse.
bool CmdTwist(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("Twist");
  if (sel.empty()) return false;
  opts.merge(e.ReadOptions(ts));
  auto a = e.ReadPoint(ts, "inizio dell'asse di torsione");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "fine dell'asse di torsione");
  if (!b) return false;
  auto deg = e.ReadNumber(ts, "angolo di torsione");
  if (!deg) return false;
  Axis ax;
  if (!MakeAxis(*a, *b, ax)) return e.Fail("asse nullo");
  const bool infinite = OptYes(opts, "infinite", false);
  const double ang = *deg * ON_PI / 180.0;
  return ApplyDeform(e, sel, [&](const ON_3dPoint& p) {
    double s = (p - ax.s) * ax.x / ax.len;
    if (!infinite) s = std::clamp(s, 0.0, 1.0);
    ON_Xform r;
    r.Rotation(ang * s, ax.x, ax.s);
    return r * p;
  });
}

// Taper: inizio e fine dell'asse, distanza all'inizio e alla fine (rapporto di scala radiale).
bool CmdTaper(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("Taper");
  if (sel.empty()) return false;
  opts.merge(e.ReadOptions(ts));
  auto a = e.ReadPoint(ts, "inizio dell'asse di rastremazione");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "fine dell'asse di rastremazione");
  if (!b) return false;
  auto r0 = e.ReadNumber(ts, "distanza iniziale");
  if (!r0) return false;
  auto r1 = e.ReadNumber(ts, "distanza finale");
  if (!r1) return false;
  Axis ax;
  if (!MakeAxis(*a, *b, ax)) return e.Fail("asse nullo");
  if (std::fabs(*r0) < 1e-12) return e.Fail("la distanza iniziale non puo' essere nulla");
  const bool infinite = OptYes(opts, "infinite", false), flat = OptYes(opts, "flat", false);
  const double R0 = *r0, R1 = *r1;
  return ApplyDeform(e, sel, [&](const ON_3dPoint& p) {
    double s = (p - ax.s) * ax.x / ax.len;
    if (!infinite) s = std::clamp(s, 0.0, 1.0);
    const double k = (R0 + (R1 - R0) * s) / R0;
    const ON_3dVector v = p - ax.s;
    const double u = v * ax.x, py = v * ax.y, pz = v * ax.z;
    // Flat=Yes: rastrema solo in una direzione (y dell'asse)
    return ax.s + ax.x * u + ax.y * (py * k) + ax.z * (flat ? pz : pz * k);
  });
}

// Bend: inizio e fine della dorsale, punto per cui passa la dorsale piegata.
bool CmdBend(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("Bend");
  if (sel.empty()) return false;
  auto a = e.ReadPoint(ts, "inizio della dorsale");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "fine della dorsale");
  if (!b) return false;
  auto p = e.ReadPoint(ts, "punto da attraversare");
  if (!p) return false;
  ON_3dVector x = *b - *a;
  const double L = x.Length();
  if (!x.Unitize()) return e.Fail("dorsale nulla");
  ON_3dVector w = *p - *a;
  ON_3dVector y = w - x * (w * x);
  if (!y.Unitize()) return e.Fail("il punto e' sulla dorsale: niente da piegare");
  const ON_3dVector z = ON_CrossProduct(x, y);
  // cerchio per l'inizio, tangente alla dorsale, che passa per il punto
  const double R = (w * w) / (2.0 * (w * y));
  const ON_3dPoint C = *a + y * R;
  const ON_3dPoint S = *a;
  return ApplyDeform(e, sel, [=](const ON_3dPoint& q) {
    const ON_3dVector v = q - S;
    const double s = v * x, h = v * y, dz = v * z;
    if (s <= 0) return q;
    const double sc = std::min(s, L), th = sc / R;
    const ON_3dPoint A = C - y * (R * std::cos(th)) + x * (R * std::sin(th));
    const ON_3dVector T = x * std::cos(th) + y * std::sin(th);
    const ON_3dVector N = y * std::cos(th) - x * std::sin(th);
    return A + T * (s - sc) + N * h + z * dz;
  });
}

// Curva piu' vicina al punto (esclusi gli oggetti indicati).
const ON_Curve* NearestCurve(Engine& e, const ON_3dPoint& p, const std::vector<ON_UUID>& exclude, ON_UUID* idOut = nullptr) {
  double best = 1e300;
  const ON_Curve* out = nullptr;
  for (const auto& id : e.AllObjectIds(false)) {
    if (std::find(exclude.begin(), exclude.end(), id) != exclude.end()) continue;
    const auto* c = ON_Curve::Cast(Geo(e, id));
    if (!c) continue;
    const ON_Interval d = c->Domain();
    for (int i = 0; i <= 400; i++) {
      const double dd = c->PointAt(d.ParameterAt(i / 400.0)).DistanceTo(p);
      if (dd < best) { best = dd; out = c; if (idOut) *idOut = id; }
    }
  }
  return out;
}

// Parametro alla frazione f della lunghezza (campionamento fitto e interpolazione).
double ArcLengthParam(const ON_Curve& c, double f) {
  const ON_Interval d = c.Domain();
  const int n = 2000;
  std::vector<double> acc(n + 1, 0.0);
  ON_3dPoint prev = c.PointAt(d[0]);
  for (int i = 1; i <= n; i++) {
    const ON_3dPoint q = c.PointAt(d.ParameterAt(double(i) / n));
    acc[i] = acc[i - 1] + q.DistanceTo(prev);
    prev = q;
  }
  const double target = std::clamp(f, 0.0, 1.0) * acc[n];
  const auto it = std::lower_bound(acc.begin(), acc.end(), target);
  const int i = std::max(1, int(it - acc.begin()));
  const double seg = acc[i] - acc[i - 1];
  const double u = seg > 0 ? (target - acc[i - 1]) / seg : 0.0;
  return d.ParameterAt((i - 1 + u) / n);
}

// ArrayCrv: copie degli oggetti lungo la curva (Count=), orientate con la tangente.
bool CmdArrayCrv(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("ArrayCrv", "seleziona gli oggetti da disporre, poi Invio");
  if (sel.empty()) return false;
  opts.merge(e.ReadOptions(ts));
  auto pick = e.ReadPick(ts, "curva del percorso");
  if (!pick) return false;
  const ON_Curve* path = NearestCurve(e, *pick, sel);
  if (!path) return e.Fail("curva del percorso non trovata");
  int n = OptInt(opts, "count", 0);
  if (n <= 0) {
    auto c = e.ReadInt(ts, "numero di elementi");
    if (!c) return false;
    n = *c;
  }
  if (n < 2) return e.Fail("servono almeno 2 elementi");
  const bool rigid = opts.count("orientation") && opts["orientation"] == "notrotate";
  // l'oggetto e' riferito all'inizio del percorso
  std::unique_ptr<ON_Curve> pc(path->DuplicateCurve());
  const ON_3dPoint base = pc->PointAtStart();
  const ON_3dVector t0 = pc->TangentAt(pc->Domain()[0]);
  const bool closed = pc->IsClosed();
  std::vector<ON_UUID> created;
  for (int k = 1; k < n; k++) {
    const double f = closed ? double(k) / n : double(k) / (n - 1);
    const double t = ArcLengthParam(*pc, f);
    const ON_3dPoint P = pc->PointAt(t);
    const ON_3dVector T = pc->TangentAt(t);
    ON_Xform x = ON_Xform::TranslationTransformation(P - base);
    if (!rigid) {
      ON_Xform r;
      r.Rotation(t0, T, base);
      x = x * r;
    }
    for (const auto& id : sel) {
      const ON_Geometry* g = Geo(e, id);
      if (!g) continue;
      ON_Geometry* c = g->Duplicate();
      c->Transform(x);
      ON_3dmObjectAttributes at = *e.Object(id)->Attributes(&ON_3dmObjectAttributes::DefaultAttributes);
      created.push_back(e.AddObject(c, &at));
    }
  }
  e.Out() << "  Create " << created.size() << " copie lungo la curva\n";
  return !created.empty() || e.Fail("nessuna copia creata");
}

// ConvertToPolyline (Convert, Output=Lines): curve sostituite da polilinee entro la tolleranza.
void Flatten(const ON_Curve& c, double t0, double t1, double tol, std::vector<ON_3dPoint>& out, int depth) {
  const ON_3dPoint a = c.PointAt(t0), b = c.PointAt(t1);
  const double tm = 0.5 * (t0 + t1);
  const ON_Line chord(a, b);
  double dev = 0;
  for (double f : {0.25, 0.5, 0.75}) {
    const ON_3dPoint q = c.PointAt(t0 + f * (t1 - t0));
    dev = std::max(dev, chord.Length() > 0 ? chord.DistanceTo(q) : q.DistanceTo(a));
  }
  if (dev > tol && depth < 24) {
    Flatten(c, t0, tm, tol, out, depth + 1);
    Flatten(c, tm, t1, tol, out, depth + 1);
  } else {
    out.push_back(b);
  }
}

bool CmdConvertToPolyline(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("ConvertToPolyline", "seleziona le curve, poi Invio");
  if (sel.empty()) return false;
  const double tol = OptNum(opts, "tolerance", 0.01);
  if (tol <= 0) return e.Fail("tolleranza non valida");
  int n = 0;
  for (const auto& id : sel) {
    const auto* c = ON_Curve::Cast(Geo(e, id));
    if (!c) continue;
    // si parte dalle campate (nodi e spigoli restano vertici)
    const int spans = std::max(1, c->SpanCount());
    std::vector<double> sv(spans + 1);
    if (!c->GetSpanVector(sv.data())) { sv = {c->Domain()[0], c->Domain()[1]}; }
    std::vector<ON_3dPoint> pts{c->PointAtStart()};
    for (size_t i = 0; i + 1 < sv.size(); i++) {
      // ogni campata in almeno 4 pezzi, poi suddivisione adattiva
      for (int k = 0; k < 4; k++) {
        const double a = sv[i] + (sv[i + 1] - sv[i]) * k / 4.0, b = sv[i] + (sv[i + 1] - sv[i]) * (k + 1) / 4.0;
        Flatten(*c, a, b, tol, pts, 0);
      }
    }
    // toglie i punti allineati entro la tolleranza
    ON_Polyline pl;
    for (const auto& p : pts) {
      if (pl.Count() >= 2) {
        const ON_Line l(pl[pl.Count() - 2], p);
        if (l.DistanceTo(pl[pl.Count() - 1]) < tol * 0.01) pl.Remove(pl.Count() - 1);
      }
      if (pl.Count() == 0 || pl[pl.Count() - 1].DistanceTo(p) > 1e-12) pl.Append(p);
    }
    if (pl.Count() < 2) continue;
    e.ReplaceObject(id, new ON_PolylineCurve(pl));
    n++;
  }
  if (!n) return e.Fail("nessuna curva convertita");
  e.Out() << "  Convertite " << n << " curve in polilinee\n";
  return true;
}

// FilletCorners: raccorda con archi gli angoli delle polilinee selezionate.
bool CmdFilletCorners(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("FilletCorners", "seleziona le polilinee, poi Invio");
  if (sel.empty()) return false;
  auto r = e.ReadNumber(ts, "raggio di raccordo");
  if (!r) return false;
  if (*r <= 0) return e.Fail("raggio non valido");
  int n = 0;
  for (const auto& id : sel) {
    const auto* c = ON_Curve::Cast(Geo(e, id));
    ON_Polyline pl;
    if (!c || !c->IsPolyline(&pl) || pl.Count() < 3) continue;
    const bool closed = pl.IsClosed();
    std::vector<ON_3dPoint> P(pl.Array(), pl.Array() + pl.Count() - (closed ? 1 : 0));
    const int m = (int)P.size();
    // per ogni vertice interno: punti di tangenza e arco
    struct Corner { bool ok = false; ON_3dPoint a, b; ON_Arc arc; };
    std::vector<Corner> corners(m);
    for (int i = 0; i < m; i++) {
      if (!closed && (i == 0 || i == m - 1)) continue;
      const ON_3dPoint& V = P[i];
      ON_3dVector u = P[(i - 1 + m) % m] - V, w = P[(i + 1) % m] - V;
      const double lu = u.Length(), lw = w.Length();
      if (!u.Unitize() || !w.Unitize()) continue;
      const double ang = std::acos(std::clamp(u * w, -1.0, 1.0));
      if (ang < 1e-6 || ang > ON_PI - 1e-6) continue;  // allineati
      const double d = *r / std::tan(ang / 2);
      if (d > 0.5 * lu + 1e-9 || d > 0.5 * lw + 1e-9) continue;  // raggio troppo grande per il lato
      Corner& k = corners[i];
      k.a = V + u * d;
      k.b = V + w * d;
      ON_3dVector bis = u + w;
      bis.Unitize();
      const ON_3dPoint mid = V + bis * (*r / std::sin(ang / 2) - *r);
      k.ok = k.arc.Create(k.a, mid, k.b);
    }
    auto* out = new ON_PolyCurve();
    auto addLine = [&](const ON_3dPoint& a, const ON_3dPoint& b) {
      if (a.DistanceTo(b) > 1e-9) out->Append(new ON_LineCurve(a, b));
    };
    const int segs = closed ? m : m - 1;
    for (int i = 0; i < segs; i++) {
      const int j = (i + 1) % m;
      const ON_3dPoint start = corners[i].ok ? corners[i].b : P[i];
      const ON_3dPoint end = corners[j].ok ? corners[j].a : P[j];
      addLine(start, end);
      if (corners[j].ok && (closed || j != m - 1)) out->Append(new ON_ArcCurve(corners[j].arc));
    }
    if (out->Count() == 0 || !out->IsValid()) { delete out; continue; }
    e.ReplaceObject(id, out);
    n++;
  }
  if (!n) return e.Fail("nessuna polilinea raccordata");
  e.Out() << "  Raccordate " << n << " polilinee\n";
  return true;
}

// Estremo della curva piu' vicino al punto: posizione, direzione "uscente" e vettore curvatura.
struct CurveEnd {
  ON_3dPoint p;
  ON_3dVector out;  // verso l'esterno della curva
  ON_3dVector k;    // vettore curvatura
};

CurveEnd EndNear(const ON_Curve& c, const ON_3dPoint& pick) {
  const bool atStart = pick.DistanceTo(c.PointAtStart()) < pick.DistanceTo(c.PointAtEnd());
  const double t = atStart ? c.Domain()[0] : c.Domain()[1];
  CurveEnd r;
  ON_3dPoint P;
  ON_3dVector T, K;
  c.EvCurvature(t, P, T, K);
  r.p = P;
  r.out = atStart ? -T : T;
  r.k = K;
  return r;
}

// BlendCrv: raccordo tra le estremita' di due curve (Continuity=Position, Tangency, Curvature).
bool CmdBlendCrv(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto p1 = e.ReadPick(ts, "prima curva, vicino all'estremita' da raccordare");
  if (!p1) return false;
  ON_UUID id1;
  const ON_Curve* c1 = NearestCurve(e, *p1, {}, &id1);
  if (!c1) return e.Fail("prima curva non trovata");
  auto p2 = e.ReadPick(ts, "seconda curva, vicino all'estremita' da raccordare");
  if (!p2) return false;
  const ON_Curve* c2 = NearestCurve(e, *p2, {id1});
  if (!c2) return e.Fail("seconda curva non trovata");
  opts.merge(e.ReadOptions(ts));
  const std::string cont = opts.count("continuity") ? opts["continuity"] : "curvature";
  const CurveEnd a = EndNear(*c1, *p1), b = EndNear(*c2, *p2);
  const double chord = a.p.DistanceTo(b.p);
  if (chord < e.Tolerance()) return e.Fail("le estremita' coincidono");
  ON_NurbsCurve* nc = nullptr;
  if (cont == "position") {
    nc = new ON_NurbsCurve();
    ON_LineCurve(a.p, b.p).GetNurbForm(*nc);
  } else if (cont == "tangency") {
    const double h = chord / 3.0;
    nc = new ON_NurbsCurve(3, false, 4, 4);
    nc->SetCV(0, a.p);
    nc->SetCV(1, a.p + a.out * h);
    nc->SetCV(2, b.p + b.out * h);
    nc->SetCV(3, b.p);
    nc->MakeClampedUniformKnotVector(1.0);
  } else {
    // grado 5: P2 = P0 + 2h T + (5/4) h^2 K riproduce la curvatura all'estremita'
    const double h = chord / 5.0;
    nc = new ON_NurbsCurve(3, false, 6, 6);
    nc->SetCV(0, a.p);
    nc->SetCV(1, a.p + a.out * h);
    nc->SetCV(2, a.p + a.out * (2 * h) + a.k * (1.25 * h * h));
    nc->SetCV(3, b.p + b.out * (2 * h) + b.k * (1.25 * h * h));
    nc->SetCV(4, b.p + b.out * h);
    nc->SetCV(5, b.p);
    nc->MakeClampedUniformKnotVector(1.0);
  }
  if (!nc->IsValid()) { delete nc; return e.Fail("raccordo non valido"); }
  e.AddObject(nc);
  e.Out() << "  Creata curva di raccordo (" << cont << ")\n";
  return true;
}

// UnifyMeshNormals: orienta in modo coerente le facce di ogni parte connessa.
bool CmdUnifyMeshNormals(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("UnifyMeshNormals", "seleziona le mesh, poi Invio");
  if (sel.empty()) return false;
  int flipped = 0, meshes = 0;
  for (const auto& id : sel) {
    const auto* m = ON_Mesh::Cast(Geo(e, id));
    if (!m) continue;
    auto* nm = new ON_Mesh(*m);
    const ON_MeshTopology& top = nm->Topology();
    const int nf = nm->FaceCount();
    std::vector<int> state(nf, 0);  // 0 non visitata, 1 ok, 2 da girare
    // direzione con cui la faccia percorre lo spigolo topologico (tv0->tv1 = +1)
    auto dirOn = [&](int fi, int ei) {
      const ON_MeshTopologyFace& f = top.m_topf[fi];
      for (int k = 0; k < 4; k++)
        if (f.m_topei[k] == ei) return f.m_reve[k] ? -1 : 1;
      return 0;
    };
    for (int seed = 0; seed < nf; seed++) {
      if (state[seed]) continue;
      state[seed] = 1;
      std::vector<int> stack{seed};
      while (!stack.empty()) {
        const int fi = stack.back();
        stack.pop_back();
        const ON_MeshTopologyFace& f = top.m_topf[fi];
        const int sides = f.IsTriangle() ? 3 : 4;
        for (int k = 0; k < sides; k++) {
          const int ei = f.m_topei[k];
          const ON_MeshTopologyEdge& te = top.m_tope[ei];
          if (te.m_topf_count != 2) continue;
          const int other = te.m_topfi[0] == fi ? te.m_topfi[1] : te.m_topfi[0];
          if (state[other]) continue;
          const int d1 = dirOn(fi, ei) * (state[fi] == 2 ? -1 : 1);
          const int d2 = dirOn(other, ei);
          state[other] = (d1 == d2) ? 2 : 1;  // coerente se percorrono lo spigolo in versi opposti
          stack.push_back(other);
        }
      }
    }
    int nflip = 0;
    for (int fi = 0; fi < nf; fi++)
      if (state[fi] == 2) {
        ON_MeshFace& f = nm->m_F[fi];
        if (f.IsTriangle()) std::swap(f.vi[1], f.vi[2]), f.vi[3] = f.vi[2];
        else std::swap(f.vi[1], f.vi[3]);
        nflip++;
      }
    nm->DestroyTopology();
    nm->m_FN.Destroy();
    nm->m_N.Destroy();
    nm->ComputeVertexNormals();
    e.ReplaceObject(id, nm);
    flipped += nflip;
    meshes++;
  }
  if (!meshes) return e.Fail("nessuna mesh selezionata");
  e.Out() << "  Facce girate: " << flipped << "\n";
  return true;
}

// FillMeshHoles: chiude i fori delle mesh con ventagli di triangoli.
bool CmdFillMeshHoles(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("FillMeshHoles", "seleziona le mesh, poi Invio");
  if (sel.empty()) return false;
  int holes = 0, meshes = 0;
  for (const auto& id : sel) {
    const auto* m = ON_Mesh::Cast(Geo(e, id));
    if (!m) continue;
    auto* nm = new ON_Mesh(*m);
    nm->m_dV.Destroy();
    const ON_MeshTopology& top = nm->Topology();
    // spigoli di bordo orientati come li percorre la loro faccia (a -> b)
    std::map<int, int> next;  // vertice topologico -> successivo lungo il bordo (verso opposto alla faccia)
    for (int ei = 0; ei < top.m_tope.Count(); ei++) {
      const ON_MeshTopologyEdge& te = top.m_tope[ei];
      if (te.m_topf_count != 1) continue;
      const ON_MeshTopologyFace& f = top.m_topf[te.m_topfi[0]];
      int rev = 0;
      for (int k = 0; k < 4; k++) if (f.m_topei[k] == ei) rev = f.m_reve[k];
      const int a = te.m_topvi[rev ? 1 : 0], b = te.m_topvi[rev ? 0 : 1];
      next[b] = a;  // il riempimento percorre il bordo al contrario
    }
    std::set<int> used;
    std::vector<std::vector<int>> loops;
    for (const auto& [start, nx] : next) {
      if (used.count(start)) continue;
      std::vector<int> loop;
      int v = start;
      while (!used.count(v) && next.count(v)) {
        used.insert(v);
        loop.push_back(v);
        v = next[v];
      }
      if (v == start && loop.size() >= 3) loops.push_back(loop);
    }
    std::vector<std::vector<ON_3dPoint>> pts;
    for (const auto& loop : loops) {
      std::vector<ON_3dPoint> p;
      for (int tv : loop) p.push_back(ON_3dPoint(nm->m_V[top.m_topv[tv].m_vi[0]]));
      pts.push_back(p);
    }
    nm->DestroyTopology();
    for (const auto& p : pts) {
      const int base = nm->m_V.Count();
      ON_3dPoint c(0, 0, 0);
      for (const auto& q : p) { c += q; nm->m_V.Append(ON_3fPoint(q)); }
      nm->m_V.Append(ON_3fPoint(c / double(p.size())));
      const int cn = (int)p.size();
      for (int i = 0; i < cn; i++) nm->SetTriangle(nm->m_F.Count(), base + i, base + (i + 1) % cn, base + cn);
    }
    if (pts.empty()) { delete nm; continue; }
    nm->m_N.Destroy();
    nm->m_FN.Destroy();
    nm->CombineIdenticalVertices();
    nm->ComputeVertexNormals();
    nm->Compact();
    e.ReplaceObject(id, nm);
    holes += (int)pts.size();
    meshes++;
  }
  if (!meshes) return e.Fail("nessun foro trovato");
  e.Out() << "  Fori chiusi: " << holes << "\n";
  return true;
}

}  // namespace

void RegisterExtra3Commands(Engine& e) {
  auto reg = [&](const char* n, const char* cat, const char* d, CommandFn f, bool mod = true) {
    CommandInfo i{n, cat, d, f};
    i.modifiesModel = mod;
    e.Register(i);
  };
  reg("Twist", "Trasformazioni", "Torsione attorno a un asse", CmdTwist);
  reg("Taper", "Trasformazioni", "Rastremazione lungo un asse", CmdTaper);
  reg("Bend", "Trasformazioni", "Piega lungo una dorsale", CmdBend);
  reg("ArrayCrv", "Trasformazioni", "Serie lungo una curva", CmdArrayCrv);
  reg("ConvertToPolyline", "Curve", "Converte le curve in polilinee (Tolerance=)", CmdConvertToPolyline);
  reg("FilletCorners", "Curve", "Raccorda gli angoli delle polilinee", CmdFilletCorners);
  reg("BlendCrv", "Curve", "Raccordo tra due curve (Continuity=)", CmdBlendCrv);
  reg("Blend", "Curve", "Come BlendCrv", CmdBlendCrv);
  reg("UnifyMeshNormals", "Mesh", "Orienta in modo coerente le facce delle mesh", CmdUnifyMeshNormals);
  reg("FillMeshHoles", "Mesh", "Chiude i fori delle mesh", CmdFillMeshHoles);
}

}  // namespace cad
