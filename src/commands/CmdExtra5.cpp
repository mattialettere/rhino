// Comandi di Rhino 8, gruppo 5 (solo openNURBS): MarkCurvature, Dir, Seam,
// InsertControlPoint, MakePeriodic, Fair, ToNURBS, SubDLoft, Fill (SubD),
// MeshPatch.
#include "commands/Engine.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace cad {
double CurveLengthOf(const ON_Curve& c);
double ParamAtLengthOf(const ON_Curve& c, double s);
namespace {

// Parametro del punto della curva piu' vicino a p (campioni + sezione aurea).
double ClosestParam(const ON_Curve& c, const ON_3dPoint& p) {
  const ON_Interval d = c.Domain();
  double best = 1e300, t0 = d[0];
  for (int i = 0; i <= 400; i++) {
    const double t = d.ParameterAt(i / 400.0);
    const double dd = c.PointAt(t).DistanceTo(p);
    if (dd < best) { best = dd; t0 = t; }
  }
  double a = std::max(d[0], t0 - d.Length() / 400), b = std::min(d[1], t0 + d.Length() / 400);
  for (int k = 0; k < 60; k++) {
    const double m1 = a + (b - a) * 0.381966, m2 = a + (b - a) * 0.618034;
    if (c.PointAt(m1).DistanceTo(p) < c.PointAt(m2).DistanceTo(p)) b = m2; else a = m1;
  }
  return 0.5 * (a + b);
}

const ON_Geometry* Geo(Engine& e, const ON_UUID& id) {
  const auto* o = e.Object(id);
  return o ? o->Geometry(nullptr) : nullptr;
}

double OptNum(const std::map<std::string, std::string>& o, const char* k, double def) {
  auto it = o.find(k);
  if (it == o.end()) return def;
  try { return std::stod(it->second); } catch (...) { return def; }
}

bool OptYes(const std::map<std::string, std::string>& o, const char* k, bool def) {
  auto it = o.find(k);
  if (it == o.end()) return def;
  return it->second == "yes" || it->second == "si" || it->second == "true" || it->second.empty();
}

// Curva (fra tutte o fra quelle indicate) piu' vicina al punto, con il parametro del punto vicino.
bool PickCurve(Engine& e, const ON_3dPoint& p, ON_UUID& idOut, double& tOut, const std::vector<ON_UUID>* only = nullptr) {
  double best = 1e300;
  const ON_Curve* out = nullptr;
  const auto ids = only ? *only : e.AllObjectIds(false);
  for (const auto& id : ids) {
    const auto* c = ON_Curve::Cast(Geo(e, id));
    if (!c) continue;
    const ON_Interval d = c->Domain();
    for (int i = 0; i <= 1000; i++) {
      const double t = d.ParameterAt(i / 1000.0);
      const double dd = c->PointAt(t).DistanceTo(p);
      if (dd < best) { best = dd; out = c; tOut = t; idOut = id; }
    }
  }
  if (!out) return false;
  const ON_Interval d = out->Domain();
  double a = std::max(d[0], tOut - d.Length() / 1000), b = std::min(d[1], tOut + d.Length() / 1000);
  for (int k = 0; k < 60; k++) {
    const double m1 = a + (b - a) * 0.381966, m2 = a + (b - a) * 0.618034;
    if (out->PointAt(m1).DistanceTo(p) < out->PointAt(m2).DistanceTo(p)) b = m2; else a = m1;
  }
  tOut = 0.5 * (a + b);
  return true;
}

// MarkCurvature: punto e cerchio di curvatura (cerchio osculatore) nel punto cliccato.
bool CmdMarkCurvature(Engine& e, TokenStream& ts) {
  auto p = e.ReadPick(ts, "punto sulla curva");
  if (!p) return false;
  ON_UUID id;
  double t = 0;
  if (!PickCurve(e, *p, id, t)) return e.Fail("nessuna curva");
  const auto* c = ON_Curve::Cast(Geo(e, id));
  ON_3dPoint P;
  ON_3dVector T, K;
  if (!c->EvCurvature(t, P, T, K)) return e.Fail("curvatura non calcolabile");
  e.AddObject(new ON_Point(P));
  const double k = K.Length();
  if (k < 1e-12) {
    e.Out() << "  Curvatura nulla (tratto rettilineo): raggio infinito\n";
    return true;
  }
  const double r = 1.0 / k;
  ON_3dVector N = K / k;
  ON_Plane pl(P + N * r, T, N);
  e.AddObject(new ON_ArcCurve(ON_Circle(pl, r)));
  e.Out() << "  Raggio di curvatura = " << r << ", curvatura = " << k << "\n";
  return true;
}

// Dir: mostra la direzione di curve, superfici e mesh; Flip=Yes la inverte.
bool CmdDir(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts, {"flip"});
  auto sel = e.RequireSelection("Dir", "seleziona gli oggetti, poi Invio");
  if (sel.empty()) return false;
  const bool flip = opts.count("flip") && OptYes(opts, "flip", true);
  int n = 0;
  for (const auto& id : sel) {
    const ON_Geometry* g = Geo(e, id);
    if (const auto* c = ON_Curve::Cast(g)) {
      const ON_3dVector T = c->TangentAt(c->Domain()[0]);
      e.Out() << "  Curva: inizio " << c->PointAtStart().x << "," << c->PointAtStart().y << "," << c->PointAtStart().z
              << " direzione " << T.x << "," << T.y << "," << T.z << "\n";
      if (flip) {
        ON_Curve* d = c->DuplicateCurve();
        d->Reverse();
        e.ReplaceObject(id, d);
      }
      n++;
    } else if (const auto* b = ON_Brep::Cast(g)) {
      e.Out() << "  Superficie: " << b->m_F.Count() << " facce\n";
      if (flip) {
        auto* d = b->Duplicate();
        d->Flip();
        e.ReplaceObject(id, d);
      }
      n++;
    } else if (const auto* x = ON_Extrusion::Cast(g)) {
      if (flip) {
        ON_Brep* d = x->BrepForm();
        if (d) { d->Flip(); e.ReplaceObject(id, d); }
      }
      n++;
    } else if (const auto* m = ON_Mesh::Cast(g)) {
      if (flip) {
        auto* d = new ON_Mesh(*m);
        d->Flip();
        e.ReplaceObject(id, d);
      }
      n++;
    }
  }
  if (!n) return e.Fail("nessuna curva, superficie o mesh");
  if (flip) e.Out() << "  Direzione invertita su " << n << " oggetti\n";
  return true;
}

// Seam: sposta il punto iniziale (giunzione) di una curva chiusa nel punto cliccato.
bool CmdSeam(Engine& e, TokenStream& ts) {
  auto p = e.ReadPick(ts, "nuovo punto di giunzione sulla curva chiusa");
  if (!p) return false;
  ON_UUID id;
  double t = 0;
  std::vector<ON_UUID> closed;
  for (const auto& oid : e.AllObjectIds(false))
    if (const auto* c = ON_Curve::Cast(Geo(e, oid)); c && c->IsClosed()) closed.push_back(oid);
  if (!PickCurve(e, *p, id, t, &closed)) return e.Fail("nessuna curva chiusa");
  ON_Curve* d = ON_Curve::Cast(Geo(e, id))->DuplicateCurve();
  if (!d->ChangeClosedCurveSeam(t)) { delete d; return e.Fail("giunzione non spostata"); }
  e.ReplaceObject(id, d);
  e.Out() << "  Giunzione spostata\n";
  return true;
}

// InsertControlPoint: aggiunge un punto di controllo vicino al punto cliccato
// (inserimento di un nodo: la forma della curva non cambia).
bool CmdInsertControlPoint(Engine& e, TokenStream& ts) {
  int n = 0;
  while (true) {
    auto p = e.ReadPick(ts, "punto sulla curva dove aggiungere il punto di controllo (Invio per finire)");
    if (!p) return n > 0 && !e.NeedsInput();
    ON_UUID id;
    double t = 0;
    if (!PickCurve(e, *p, id, t)) return e.Fail("nessuna curva");
    auto* nc = new ON_NurbsCurve;
    if (!ON_Curve::Cast(Geo(e, id))->GetNurbForm(*nc)) { delete nc; return e.Fail("curva non convertibile"); }
    if (!nc->InsertKnot(t, 1)) { delete nc; return e.Fail("punto non aggiunto"); }
    e.ReplaceObject(id, nc);
    e.Out() << "  Punti di controllo: " << nc->CVCount() << "\n";
    n++;
    if (!e.MoreInput(ts)) return true;
  }
}

// MakePeriodic: curve chiuse -> curve periodiche (lisce anche nella giunzione).
bool CmdMakePeriodic(Engine& e, TokenStream& ts) {
  e.ReadOptions(ts);
  auto sel = e.RequireSelection("MakePeriodic", "seleziona le curve chiuse, poi Invio");
  if (sel.empty()) return false;
  int n = 0;
  for (const auto& id : sel) {
    const auto* c = ON_Curve::Cast(Geo(e, id));
    if (!c || !c->IsClosed() || c->IsPeriodic()) continue;
    ON_NurbsCurve nc;
    if (!c->GetNurbForm(nc)) continue;
    int order = std::max(4, nc.Order());
    std::vector<ON_3dPoint> pts;
    for (int i = 0; i < nc.CVCount() - 1; i++) { ON_3dPoint q; nc.GetCV(i, q); pts.push_back(q); }
    // rette e poligoni hanno pochi punti: si infittisce per avere almeno order punti distinti
    while ((int)pts.size() < std::max(order, 4)) {
      std::vector<ON_3dPoint> more;
      for (size_t i = 0; i < pts.size(); i++) { more.push_back(pts[i]); more.push_back(0.5 * (pts[i] + pts[(i + 1) % pts.size()])); }
      pts = more;
    }
    auto* pc = new ON_NurbsCurve;
    if (!pc->CreatePeriodicUniformNurbs(3, order, (int)pts.size(), pts.data())) { delete pc; continue; }
    e.ReplaceObject(id, pc);
    n++;
  }
  if (!n) return e.Fail("nessuna curva chiusa non periodica");
  e.Out() << "  Rese periodiche " << n << " curve\n";
  return true;
}

// Fair: leviga le curve spostando i punti di controllo interni, senza
// allontanarsi dall'originale piu' di Tolerance= (default 10 volte la tolleranza).
bool CmdFair(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("Fair", "seleziona le curve, poi Invio");
  if (sel.empty()) return false;
  const double tol = OptNum(opts, "tolerance", 10 * e.Tolerance());
  int n = 0;
  for (const auto& id : sel) {
    const auto* c = ON_Curve::Cast(Geo(e, id));
    if (!c) continue;
    auto* nc = new ON_NurbsCurve;
    if (!c->GetNurbForm(*nc) || nc->CVCount() < 5) { delete nc; continue; }
    const int N = 200;
    std::vector<ON_3dPoint> ref(N + 1);
    for (int i = 0; i <= N; i++) ref[i] = c->PointAt(c->Domain().ParameterAt(double(i) / N));
    auto deviation = [&](const ON_NurbsCurve& k) {
      double m = 0;
      for (int i = 0; i <= N; i++) {
        m = std::max(m, k.PointAt(ClosestParam(k, ref[i])).DistanceTo(ref[i]));
      }
      return m;
    };
    const int cv = nc->CVCount();
    const bool closed = nc->IsClosed();
    const int lo = closed ? 0 : 2, hi = closed ? cv - 1 : cv - 2;  // aperta: estremi e tangenti fissi
    for (int it = 0; it < 100; it++) {
      ON_NurbsCurve trial(*nc);
      bool moved = false;
      for (int i = lo; i < hi; i++) {
        ON_3dPoint a, b, q;
        nc->GetCV((i - 1 + cv) % cv, a);
        nc->GetCV((i + 1) % cv, b);
        nc->GetCV(i, q);
        if (closed && (i == 0)) { nc->GetCV(cv - 2, a); }
        const ON_3dPoint target = q + 0.25 * (0.5 * (a + b) - q);
        if (target.DistanceTo(q) > 1e-12) { trial.SetCV(i, target); moved = true; }
      }
      if (closed) { ON_3dPoint f; trial.GetCV(0, f); trial.SetCV(cv - 1, f); }
      if (!moved || deviation(trial) > tol) break;
      *nc = trial;
    }
    e.ReplaceObject(id, nc);
    n++;
  }
  if (!n) return e.Fail("nessuna curva con almeno 5 punti di controllo");
  e.Out() << "  Levigate " << n << " curve\n";
  return true;
}

// ToNURBS: estrusioni e SubD -> superfici/polisuperfici NURBS.
bool CmdToNURBS(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("ToNURBS", "seleziona estrusioni o SubD, poi Invio");
  if (sel.empty()) return false;
  int n = 0;
  for (const auto& id : sel) {
    const ON_Geometry* g = Geo(e, id);
    if (!g || ON_Brep::Cast(g) || ON_Mesh::Cast(g)) continue;
    ON_Brep* b = nullptr;
    if (const auto* sd = ON_SubD::Cast(g)) b = sd->BrepForm(nullptr);  // vuoto con openNURBS pubblico
    else if (g->HasBrepForm()) b = g->BrepForm();
    if (!b) continue;
    b->Standardize();  // superfici proxy (es. ON_SurfaceProxy dell'estrusione) -> superfici vere
    b->Compact();
    // passaggio da archivio: il B-rep diventa identico a come sara' riletto dal file .3dm
    ON_Write3dmBufferArchive wa(0, 0, 80, ON::Version());
    ON_Object* rb = nullptr;
    if (wa.WriteObject(*b)) {
      ON_Read3dmBufferArchive ra(wa.SizeOfArchive(), wa.Buffer(), false, 80, ON::Version());
      if (ra.ReadObject(&rb) == 1 && ON_Brep::Cast(rb)) { delete b; b = ON_Brep::Cast(rb); } else delete rb;
    }
    e.ReplaceObject(id, b);
    n++;
  }
  if (!n) return e.Fail("nessun oggetto da convertire (estrusioni; i SubD non sono ancora convertibili)");
  e.Out() << "  Convertiti in NURBS " << n << " oggetti\n";
  return true;
}

// Punti a distanze uguali (per lunghezza) lungo la curva.
std::vector<ON_3dPoint> SampleByLength(const ON_Curve& c, int count, bool closed) {
  std::vector<ON_3dPoint> pts;
  const double L = CurveLengthOf(c);
  const int segs = closed ? count : count - 1;
  for (int i = 0; i < count; i++) {
    const double s = double(i) / segs;
    pts.push_back(c.PointAt(i == 0 ? c.Domain()[0] : ParamAtLengthOf(c, s * L)));
  }
  return pts;
}

// SubDLoft: SubD attraverso le curve (in ordine di selezione).
bool CmdSubDLoft(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts, {"closed"});
  auto sel = e.RequireSelection("SubDLoft", "seleziona le curve in ordine, poi Invio");
  if (sel.empty()) return false;
  std::vector<const ON_Curve*> cs;
  for (const auto& id : sel) if (const auto* c = ON_Curve::Cast(Geo(e, id))) cs.push_back(c);
  if (cs.size() < 2) return e.Fail("servono almeno 2 curve");
  int count = 0;
  for (auto* c : cs) {
    ON_NurbsCurve nc;
    if (c->GetNurbForm(nc)) count = std::max(count, nc.CVCount());
  }
  count = std::max(4, (int)OptNum(opts, "pointcount", count));
  const bool closedU = cs[0]->IsClosed();
  const bool closedV = opts.count("closed") && OptYes(opts, "closed", true);
  ON_Mesh cage;
  for (auto* c : cs)
    for (const auto& q : SampleByLength(*c, count, closedU)) cage.m_V.Append(ON_3fPoint(q));
  const int rows = (int)cs.size();
  auto idx = [&](int r, int i) { return (r % rows) * count + (i % count); };
  for (int r = 0; r < rows - 1 + (closedV ? 1 : 0); r++)
    for (int i = 0; i < count - 1 + (closedU ? 1 : 0); i++)
      cage.SetQuad(cage.m_F.Count(), idx(r, i), idx(r, i + 1), idx(r + 1, i + 1), idx(r + 1, i));
  ON_SubD* s = ON_SubD::CreateFromMesh(&cage, nullptr, nullptr);
  if (!s || !s->IsValid()) { delete s; return e.Fail("SubD non creata"); }
  e.AddObject(s);
  e.Out() << "  Creato SubD con " << cage.FaceCount() << " facce\n";
  return true;
}

// Bordi aperti di una mesh come anelli di vertici.
std::vector<std::vector<int>> BoundaryLoops(const ON_Mesh& m) {
  std::map<std::pair<int, int>, int> use;
  auto faceVerts = [&](int f) {
    const ON_MeshFace& F = m.m_F[f];
    std::vector<int> v{F.vi[0], F.vi[1], F.vi[2]};
    if (F.IsQuad()) v.push_back(F.vi[3]);
    return v;
  };
  for (int f = 0; f < m.FaceCount(); f++) {
    auto v = faceVerts(f);
    for (size_t k = 0; k < v.size(); k++) use[{v[k], v[(k + 1) % v.size()]}]++;
  }
  std::map<int, int> next;  // bordo orientato: lato a->b senza il gemello b->a
  for (const auto& [ed, cnt] : use)
    if (!use.count({ed.second, ed.first})) next[ed.second] = ed.first;  // verso opposto: il riempimento ha normale concorde
  std::vector<std::vector<int>> loops;
  std::set<int> done;
  for (const auto& [a, b] : next) {
    if (done.count(a)) continue;
    std::vector<int> loop;
    int v = a;
    while (!done.count(v) && next.count(v)) { done.insert(v); loop.push_back(v); v = next[v]; }
    if (loop.size() >= 3) loops.push_back(loop);
  }
  return loops;
}

// Fill (SubD): chiude i fori della gabbia SubD con una faccia per foro.
bool CmdFill(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("Fill", "seleziona i SubD con fori, poi Invio");
  if (sel.empty()) return false;
  int holes = 0;
  for (const auto& id : sel) {
    const auto* s = ON_SubD::Cast(Geo(e, id));
    if (!s) continue;
    std::unique_ptr<ON_Mesh> cage(s->GetControlNetMesh(nullptr, ON_SubDGetControlNetMeshPriority::Geometry));
    if (!cage) continue;
    cage->CombineIdenticalVertices(true, true);
    const auto loops = BoundaryLoops(*cage);
    if (loops.empty()) continue;
    // la gabbia puo' contenere n-agoni: si ricostruiscono come ngon della mesh
    ON_Mesh m(*cage);
    for (const auto& loop : loops) {
      ON_SimpleArray<unsigned int> fi;
      for (size_t k = 1; k + 1 < loop.size(); k++) {
        fi.Append((unsigned)m.m_F.Count());
        m.SetTriangle(m.m_F.Count(), loop[0], loop[k], loop[k + 1]);
      }
      if (fi.Count() > 1) m.AddNgon(fi);
      holes++;
    }
    m.m_N.Destroy();
    m.m_FN.Destroy();
    ON_SubD* r = ON_SubD::CreateFromMesh(&m, nullptr, nullptr);
    if (!r || !r->IsValid()) { delete r; continue; }
    e.ReplaceObject(id, r);
  }
  if (!holes) return e.Fail("nessun foro da chiudere");
  e.Out() << "  Chiusi " << holes << " fori\n";
  return true;
}

// MeshPatch: mesh triangolata (Delaunay nel piano medio) per punti e curve.
bool CmdMeshPatch(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("MeshPatch", "seleziona punti e curve, poi Invio");
  if (sel.empty()) return false;
  std::vector<ON_3dPoint> P;
  for (const auto& id : sel) {
    const ON_Geometry* g = Geo(e, id);
    if (const auto* p = ON_Point::Cast(g)) P.push_back(p->point);
    else if (const auto* pc = ON_PointCloud::Cast(g)) for (int i = 0; i < pc->m_P.Count(); i++) P.push_back(pc->m_P[i]);
    else if (const auto* c = ON_Curve::Cast(g)) {
      const int n = 32;
      for (int i = 0; i <= n; i++) {
        if (i == n && c->IsClosed()) break;
        P.push_back(c->PointAt(c->Domain().ParameterAt(double(i) / n)));
      }
    }
  }
  // via i doppioni
  std::vector<ON_3dPoint> Q;
  for (const auto& p : P) {
    bool dup = false;
    for (const auto& q : Q) if (q.DistanceTo(p) <= e.Tolerance()) { dup = true; break; }
    if (!dup) Q.push_back(p);
  }
  if (Q.size() < 3) return e.Fail("servono almeno 3 punti distinti");
  // piano medio: baricentro e direzione di minima dispersione (covarianza)
  ON_3dPoint C(0, 0, 0);
  for (const auto& q : Q) C += q;
  C = C / double(Q.size());
  double M[3][3] = {};
  for (const auto& q : Q) {
    const double d[3] = {q.x - C.x, q.y - C.y, q.z - C.z};
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) M[i][j] += d[i] * d[j];
  }
  const double tr = M[0][0] + M[1][1] + M[2][2];
  ON_3dVector nrm(0.3, 0.5, 0.8);
  for (int it = 0; it < 200; it++) {  // autovettore minimo = massimo di (tr I - M)
    ON_3dVector r(tr * nrm.x - (M[0][0] * nrm.x + M[0][1] * nrm.y + M[0][2] * nrm.z),
                  tr * nrm.y - (M[1][0] * nrm.x + M[1][1] * nrm.y + M[1][2] * nrm.z),
                  tr * nrm.z - (M[2][0] * nrm.x + M[2][1] * nrm.y + M[2][2] * nrm.z));
    if (!r.Unitize()) break;
    nrm = r;
  }
  if (nrm.z < 0) nrm = -nrm;
  ON_Plane pl(C, nrm);
  std::vector<ON_2dPoint> uv;
  for (const auto& q : Q) { double u, v; pl.ClosestPointTo(q, &u, &v); uv.emplace_back(u, v); }
  // Bowyer-Watson
  const int n = (int)uv.size();
  double minx = 1e300, miny = 1e300, maxx = -1e300, maxy = -1e300;
  for (auto& p : uv) { minx = std::min(minx, p.x); miny = std::min(miny, p.y); maxx = std::max(maxx, p.x); maxy = std::max(maxy, p.y); }
  const double D = std::max(maxx - minx, maxy - miny) * 20 + 1;
  uv.emplace_back(minx - D, miny - D);
  uv.emplace_back(maxx + D, miny - D);
  uv.emplace_back(0.5 * (minx + maxx), maxy + D);
  std::vector<std::array<int, 3>> tris{{n, n + 1, n + 2}};
  auto inCirc = [&](const std::array<int, 3>& t, const ON_2dPoint& p) {
    const ON_2dPoint &a = uv[t[0]], &b = uv[t[1]], &c = uv[t[2]];
    const double ax = a.x - p.x, ay = a.y - p.y, bx = b.x - p.x, by = b.y - p.y, cx = c.x - p.x, cy = c.y - p.y;
    const double det = (ax * ax + ay * ay) * (bx * cy - cx * by) - (bx * bx + by * by) * (ax * cy - cx * ay) +
                       (cx * cx + cy * cy) * (ax * by - bx * ay);
    const double orient = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    return orient > 0 ? det > 0 : det < 0;
  };
  for (int i = 0; i < n; i++) {
    std::vector<std::array<int, 3>> keep;
    std::map<std::pair<int, int>, int> edges;
    for (const auto& t : tris) {
      if (inCirc(t, uv[i])) {
        for (int k = 0; k < 3; k++) {
          int a = t[k], b = t[(k + 1) % 3];
          edges[{std::min(a, b), std::max(a, b)}]++;
        }
      } else keep.push_back(t);
    }
    for (const auto& [ed, cnt] : edges) if (cnt == 1) keep.push_back({ed.first, ed.second, i});
    tris = keep;
  }
  auto* m = new ON_Mesh;
  for (int i = 0; i < n; i++) m->m_V.Append(ON_3fPoint(Q[i]));
  for (const auto& t : tris) {
    if (t[0] >= n || t[1] >= n || t[2] >= n) continue;
    const ON_2dPoint &a = uv[t[0]], &b = uv[t[1]], &c = uv[t[2]];
    const double orient = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (std::fabs(orient) < 1e-14) continue;
    if (orient > 0) m->SetTriangle(m->m_F.Count(), t[0], t[1], t[2]);
    else m->SetTriangle(m->m_F.Count(), t[0], t[2], t[1]);
  }
  if (m->FaceCount() == 0) { delete m; return e.Fail("punti allineati: nessun triangolo"); }
  m->ComputeVertexNormals();
  e.AddObject(m);
  e.Out() << "  Creata mesh con " << m->FaceCount() << " triangoli\n";
  return true;
}

}  // namespace

void RegisterExtra5Commands(Engine& e) {
  auto reg = [&](const char* n, const char* cat, const char* d, CommandFn f) {
    CommandInfo i{n, cat, d, f};
    i.modifiesModel = true;
    e.Register(i);
  };
  reg("MarkCurvature", "Analizza", "Punto e cerchio di curvatura nel punto cliccato della curva", CmdMarkCurvature);
  reg("Dir", "Analizza", "Direzione di curve, superfici e mesh (Flip=Yes la inverte)", CmdDir);
  reg("Seam", "Curve", "Sposta la giunzione di una curva chiusa nel punto cliccato", CmdSeam);
  reg("InsertControlPoint", "Curve", "Aggiunge punti di controllo nei punti cliccati della curva", CmdInsertControlPoint);
  reg("MakePeriodic", "Curve", "Rende periodiche (lisce nella giunzione) le curve chiuse", CmdMakePeriodic);
  reg("Fair", "Curve", "Leviga le curve entro Tolerance=", CmdFair);
  reg("ToNURBS", "Superfici", "Converte estrusioni e SubD in NURBS", CmdToNURBS);
  reg("SubDLoft", "SubD", "SubD attraverso curve (PointCount=, Closed)", CmdSubDLoft);
  reg("Fill", "SubD", "Chiude i fori dei SubD con una faccia", CmdFill);
  reg("MeshPatch", "Mesh", "Mesh triangolata per punti e curve", CmdMeshPatch);
}

}  // namespace cad
