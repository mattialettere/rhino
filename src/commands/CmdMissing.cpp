// Comandi di Rhino 8 aggiunti dopo il confronto con Rhino 8 (docs/CONFRONTO_RHINO8.md),
// solo openNURBS: InsertKink, SoftEditCrv, SoftEditSrf, SoftMove, Stretch, Maelstrom,
// MoveUVN, Cage, MeshFromLines, SplitEdge, JoinEdge, UnjoinEdge, RebuildEdges,
// NamedSelections, LayerStateManager, Alias, CommandHistory.
#include "commands/Deform.h"
#include "commands/Engine.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace cad {
// Curve 3D degli spigoli ricalcolate dalle curve di taglio (CmdMissingOcct.cpp; senza
// OpenCASCADE non fa niente).
void RebuildBrepEdges(ON_Brep& b);

namespace {

const ON_Geometry* Geo(Engine& e, const ON_UUID& id) {
  const auto* o = e.Object(id);
  return o ? o->Geometry(nullptr) : nullptr;
}

double OptNum(const std::map<std::string, std::string>& o, const char* k, double def) {
  auto it = o.find(k);
  if (it == o.end()) return def;
  try { return std::stod(it->second); } catch (...) { return def; }
}

std::string Unquote(std::string n) {
  if (n.size() >= 2 && n.front() == '"' && n.back() == '"') n = n.substr(1, n.size() - 2);
  return n;
}

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

// Lunghezza della curva tra due parametri (Simpson sulle derivate).
double LengthBetween(const ON_Curve& c, double a, double b) {
  if (b <= a) return 0;
  const int n = 200;
  const double h = (b - a) / n;
  auto speed = [&](double t) { return c.DerivativeAt(t).Length(); };
  double s = speed(a) + speed(b);
  for (int i = 1; i < n; i++) s += speed(a + i * h) * (i % 2 ? 4 : 2);
  return s * h / 3;
}

// Parametri (u,v) del punto della superficie piu' vicino a p (griglia + Newton).
bool ClosestUV(const ON_Surface& s, const ON_3dPoint& p, double& u, double& v) {
  const ON_Interval U = s.Domain(0), V = s.Domain(1);
  double best = 1e300;
  for (int i = 0; i <= 40; i++)
    for (int j = 0; j <= 40; j++) {
      const double uu = U.ParameterAt(i / 40.0), vv = V.ParameterAt(j / 40.0);
      const double d = s.PointAt(uu, vv).DistanceTo(p);
      if (d < best) { best = d; u = uu; v = vv; }
    }
  for (int it = 0; it < 30; it++) {
    ON_3dPoint P;
    ON_3dVector Su, Sv, Suu, Suv, Svv;
    if (!s.Ev2Der(u, v, P, Su, Sv, Suu, Suv, Svv)) break;
    const ON_3dVector r = P - p;
    const double fu = r * Su, fv = r * Sv;
    const double a = Su * Su + r * Suu, b = Su * Sv + r * Suv, c = Sv * Sv + r * Svv;
    const double det = a * c - b * b;
    if (std::fabs(det) < 1e-30) break;
    const double du = (c * fu - b * fv) / det, dv = (a * fv - b * fu) / det;
    u = std::clamp(u - du, U.Min(), U.Max());
    v = std::clamp(v - dv, V.Min(), V.Max());
    if (std::fabs(du) < 1e-12 * (1 + U.Length()) && std::fabs(dv) < 1e-12 * (1 + V.Length())) break;
  }
  return true;
}

// Attenuazione morbida: 1 al centro, 0 a distanza 1 (tangente nulla ai due estremi).
double Falloff(double x) {
  x = std::fabs(x);
  if (x >= 1) return 0;
  const double s = 1 - x * x;
  return s * s;
}

// Curva piu' vicina al punto (tra gli oggetti visibili), con il parametro del punto piu' vicino.
bool NearestCurve(Engine& e, const ON_3dPoint& p, ON_UUID& id, double& t) {
  double best = 1e300;
  for (const auto& oid : e.AllObjectIds(false)) {
    const auto* c = ON_Curve::Cast(Geo(e, oid));
    if (!c) continue;
    if (c->BoundingBox().MinimumDistanceTo(p) > best) continue;
    const double tt = ClosestParam(*c, p);
    const double d = c->PointAt(tt).DistanceTo(p);
    if (d < best) { best = d; id = oid; t = tt; }
  }
  return best < 1e300;
}

// Superficie (non tagliata o faccia singola) piu' vicina al punto.
bool NearestSurfaceObject(Engine& e, const ON_3dPoint& p, ON_UUID& id) {
  double best = 1e300;
  for (const auto& oid : e.AllObjectIds(false)) {
    const ON_Geometry* g = Geo(e, oid);
    const ON_Surface* s = ON_Surface::Cast(g);
    const ON_Brep* b = ON_Brep::Cast(g);
    if (!s && !(b && b->m_F.Count() == 1) && !ON_Extrusion::Cast(g)) continue;
    const ON_Surface* sf = s ? s : (b ? b->m_F[0].SurfaceOf() : nullptr);
    std::unique_ptr<ON_Brep> tmp;
    if (!sf) {
      tmp.reset(ON_Extrusion::Cast(g)->BrepForm());
      if (!tmp || tmp->m_F.Count() != 1) continue;
      sf = tmp->m_F[0].SurfaceOf();
    }
    double u, v;
    if (!ClosestUV(*sf, p, u, v)) continue;
    const double d = sf->PointAt(u, v).DistanceTo(p);
    if (d < best) { best = d; id = oid; }
  }
  return best < 1e300;
}

// Inserisce nodi finche' la curva ha almeno "spans" campate (la forma non cambia).
void Refine(ON_NurbsCurve& nc, int spans) {
  if (nc.Degree() < 3) nc.IncreaseDegree(3);
  const ON_Interval d = nc.Domain();
  if (nc.SpanCount() >= spans) return;
  for (int k = 1; k < spans; k++) {
    const double t = d.ParameterAt(double(k) / spans);
    bool exists = false;
    for (int i = 0; i < nc.KnotCount(); i++) if (std::fabs(nc.m_knot[i] - t) < 1e-12 * (1 + d.Length())) exists = true;
    if (!exists) nc.InsertKnot(t, 1);
  }
}

void Refine(ON_NurbsSurface& ns, int spans) {
  for (int dir = 0; dir < 2; dir++) {
    if (ns.Degree(dir) < 3) ns.IncreaseDegree(dir, 3);
    const ON_Interval d = ns.Domain(dir);
    if (ns.SpanCount(dir) >= spans) continue;
    for (int k = 1; k < spans; k++) {
      const double t = d.ParameterAt(double(k) / spans);
      bool exists = false;
      for (int i = 0; i < ns.KnotCount(dir); i++) if (std::fabs(ns.m_knot[dir][i] - t) < 1e-12 * (1 + d.Length())) exists = true;
      if (!exists) ns.InsertKnot(dir, t, 1);
    }
  }
}

void AddToCV(ON_NurbsCurve& c, int i, const ON_3dVector& v) {
  ON_4dPoint cv;
  c.GetCV(i, cv);
  const double w = c.IsRational() ? cv.w : 1.0;
  c.SetCV(i, ON_4dPoint(cv.x + v.x * w, cv.y + v.y * w, cv.z + v.z * w, w));
}

void AddToCV(ON_NurbsSurface& s, int i, int j, const ON_3dVector& v) {
  ON_4dPoint cv;
  s.GetCV(i, j, cv);
  const double w = s.IsRational() ? cv.w : 1.0;
  s.SetCV(i, j, ON_4dPoint(cv.x + v.x * w, cv.y + v.y * w, cv.z + v.z * w, w));
}

// --- InsertKink ------------------------------------------------------------------
// Nodo di molteplicita' pari al grado nel punto scelto: la curva puo' avere uno spigolo li'.
bool CmdInsertKink(Engine& e, TokenStream& ts) {
  int n = 0;
  while (true) {
    auto p = e.ReadPick(ts, n ? "punto della curva per lo spigolo (Invio per finire)" : "punto della curva per lo spigolo");
    if (!p) { if (e.NeedsInput()) return false; break; }
    ON_UUID id;
    double t;
    if (!NearestCurve(e, *p, id, t)) return e.Fail("nessuna curva");
    const ON_Curve* c = ON_Curve::Cast(Geo(e, id));
    auto* nc = new ON_NurbsCurve;
    if (!c->GetNurbForm(*nc)) { delete nc; return e.Fail("curva non convertibile"); }
    if (nc->Degree() < 2) { delete nc; e.Out() << "  La curva di grado 1 ha gia' spigoli in ogni nodo\n"; }
    else {
      if (!nc->InsertKnot(t, nc->Degree())) { delete nc; return e.Fail("nodo non inserito (troppo vicino a un estremo?)"); }
      e.ReplaceObject(id, nc);
      n++;
    }
    if (!e.MoreInput(ts)) break;
  }
  e.SkipEnter(ts);
  if (!n) return e.Fail("nessuno spigolo inserito");
  e.Out() << "  Inseriti " << n << " spigoli\n";
  return true;
}

// --- SoftEditCrv -----------------------------------------------------------------
// Sposta un punto della curva trascinando i punti di controllo vicini con un'attenuazione
// morbida lungo la curva (Length= distanza misurata sulla curva; FixEnds=Yes tiene fermi gli estremi).
bool CmdSoftEditCrv(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts, {"fixends"});
  auto p = e.ReadPick(ts, "punto della curva da spostare");
  if (!p) return false;
  ON_UUID id;
  double t0;
  if (!NearestCurve(e, *p, id, t0)) return e.Fail("nessuna curva");
  const ON_Curve* c = ON_Curve::Cast(Geo(e, id));
  const ON_3dPoint from = c->PointAt(t0);
  auto to = e.ReadPoint(ts, "nuova posizione");
  if (!to) return false;
  opts.merge(e.ReadOptions(ts, {"fixends"}));
  const double total = LengthBetween(*c, c->Domain()[0], c->Domain()[1]);
  const double L = OptNum(opts, "length", total * 0.25);
  const bool fixEnds = opts.count("fixends") && opts["fixends"] != "no";
  if (L <= 0) return e.Fail("lunghezza non valida");
  auto* nc = new ON_NurbsCurve;
  if (!c->GetNurbForm(*nc)) { delete nc; return e.Fail("curva non convertibile"); }
  Refine(*nc, 16);
  const double s0 = LengthBetween(*nc, nc->Domain()[0], t0);
  const ON_3dVector delta = *to - from;
  std::vector<double> w(nc->CVCount());
  for (int i = 0; i < nc->CVCount(); i++) {
    const double g = nc->GrevilleAbcissa(i);
    const double si = LengthBetween(*nc, nc->Domain()[0], g);
    w[i] = Falloff((si - s0) / L);
    if (fixEnds && !nc->IsClosed() && (i == 0 || i == nc->CVCount() - 1)) w[i] = 0;
  }
  // il punto scelto deve arrivare esattamente sul nuovo punto: si scala lo spostamento
  ON_NurbsCurve test(*nc);
  for (int i = 0; i < test.CVCount(); i++) AddToCV(test, i, delta * w[i]);
  const double moved = (test.PointAt(t0) - nc->PointAt(t0)) * delta;
  const double k = moved > 1e-14 ? delta.LengthSquared() / moved : 0;
  if (k <= 0) { delete nc; return e.Fail("il punto non si puo' spostare (estremo bloccato?)"); }
  for (int i = 0; i < nc->CVCount(); i++) AddToCV(*nc, i, delta * (w[i] * k));
  e.ReplaceObject(id, nc);
  e.Out() << "  Curva modificata su una lunghezza di " << L << "\n";
  return true;
}

// --- SoftEditSrf -----------------------------------------------------------------
// Come SoftEditCrv su una superficie: UDistance=/VDistance= in frazione del dominio (0..1).
bool CmdSoftEditSrf(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts, {"fixedges"});
  auto p = e.ReadPick(ts, "punto della superficie da spostare");
  if (!p) return false;
  ON_UUID id;
  if (!NearestSurfaceObject(e, *p, id)) return e.Fail("nessuna superficie (non tagliata o a faccia singola)");
  const ON_Geometry* g = Geo(e, id);
  std::unique_ptr<ON_Brep> brep;
  if (const auto* b = ON_Brep::Cast(g)) brep.reset(b->Duplicate());
  else if (const auto* x = ON_Extrusion::Cast(g)) brep.reset(x->BrepForm());
  else if (const auto* s = ON_Surface::Cast(g)) brep.reset(s->BrepForm());
  if (!brep || brep->m_F.Count() != 1) return e.Fail("serve una superficie a faccia singola");
  ON_NurbsSurface ns;
  if (brep->m_F[0].SurfaceOf()->GetNurbForm(ns) != 1) return e.Fail("superficie non convertibile senza cambiare i parametri");
  Refine(ns, 12);
  double u0, v0;
  if (!ClosestUV(ns, *p, u0, v0)) return e.Fail("punto non proiettato");
  const ON_3dPoint from = ns.PointAt(u0, v0);
  auto to = e.ReadPoint(ts, "nuova posizione");
  if (!to) return false;
  opts.merge(e.ReadOptions(ts, {"fixedges"}));
  const double du = std::max(1e-3, OptNum(opts, "udistance", 0.25)), dv = std::max(1e-3, OptNum(opts, "vdistance", 0.25));
  const bool fixEdges = opts.count("fixedges") && opts["fixedges"] != "no";
  const ON_Interval DU = ns.Domain(0), DV = ns.Domain(1);
  const ON_3dVector delta = *to - from;
  const int nu = ns.CVCount(0), nv = ns.CVCount(1);
  std::vector<double> w(nu * nv);
  for (int i = 0; i < nu; i++)
    for (int j = 0; j < nv; j++) {
      const double gu = ns.GrevilleAbcissa(0, i), gv = ns.GrevilleAbcissa(1, j);
      double x = Falloff((DU.NormalizedParameterAt(gu) - DU.NormalizedParameterAt(u0)) / du) *
                 Falloff((DV.NormalizedParameterAt(gv) - DV.NormalizedParameterAt(v0)) / dv);
      if (fixEdges && (i == 0 || j == 0 || i == nu - 1 || j == nv - 1)) x = 0;
      w[i * nv + j] = x;
    }
  ON_NurbsSurface test(ns);
  for (int i = 0; i < nu; i++) for (int j = 0; j < nv; j++) AddToCV(test, i, j, delta * w[i * nv + j]);
  const double moved = (test.PointAt(u0, v0) - from) * delta;
  const double k = moved > 1e-14 ? delta.LengthSquared() / moved : 0;
  if (k <= 0) return e.Fail("il punto non si puo' spostare");
  for (int i = 0; i < nu; i++) for (int j = 0; j < nv; j++) AddToCV(ns, i, j, delta * (w[i * nv + j] * k));
  // stessa parametrizzazione: i tagli restano validi, si ricalcolano gli spigoli 3D
  const int si = brep->m_F[0].m_si;
  delete brep->m_S[si];
  brep->m_S[si] = new ON_NurbsSurface(ns);
  brep->m_F[0].SetProxySurface(brep->m_S[si]);
  RebuildBrepEdges(*brep);
  brep->DestroyMesh(ON::any_mesh);
  ON_Geometry* out = brep.release();
  if (!ON_Brep::Cast(g) && !ON_Extrusion::Cast(g) && ON_Brep::Cast(out)->m_F[0].m_li.Count() == 1 &&
      ON_Brep::Cast(out)->IsSurface()) {
    ON_Brep* b = ON_Brep::Cast(out);
    out = new ON_NurbsSurface(ns);
    delete b;
  }
  e.ReplaceObject(id, out);
  e.Out() << "  Superficie modificata\n";
  return true;
}

// --- SoftMove --------------------------------------------------------------------
// Sposta i punti di controllo degli oggetti selezionati vicini al punto base, con attenuazione
// morbida fino a Radius= (sfera di influenza).
bool CmdSoftMove(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("SoftMove", "seleziona gli oggetti da deformare, poi Invio");
  if (sel.empty()) return false;
  auto a = e.ReadPoint(ts, "punto da spostare");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "nuova posizione");
  if (!b) return false;
  opts.merge(e.ReadOptions(ts));
  ON_BoundingBox bb;
  for (const auto& id : sel) if (const ON_Geometry* g = Geo(e, id)) bb.Union(g->BoundingBox());
  const double r = OptNum(opts, "radius", bb.IsValid() ? bb.Diagonal().Length() * 0.25 : 1.0);
  if (r <= 0) return e.Fail("raggio non valido");
  const ON_3dVector d = *b - *a;
  const ON_3dPoint c = *a;
  int n = 0;
  for (const auto& id : sel) {
    ON_Geometry* g = DeformGeometry(Geo(e, id), [&](const ON_3dPoint& q) { return q + d * Falloff(q.DistanceTo(c) / r); });
    if (g && !ON_UuidIsNil(e.ReplaceObject(id, g))) n++;
  }
  if (!n) return e.Fail("nessun oggetto deformabile");
  e.Out() << "  Deformati " << n << " oggetti (raggio " << r << ")\n";
  return true;
}

// --- Stretch ---------------------------------------------------------------------
// Allunga lungo un asse: inizio dell'asse (fisso), fine dell'asse e sua nuova posizione.
// Trasformazione lineare esatta: p' = p + ((p-a)·(s-a)/|s-a|²)(t-s).
bool CmdStretch(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("Stretch", "seleziona gli oggetti da allungare, poi Invio");
  if (sel.empty()) return false;
  auto a = e.ReadPoint(ts, "inizio dell'asse di allungamento");
  if (!a) return false;
  auto s = e.ReadPoint(ts, "fine dell'asse di allungamento");
  if (!s) return false;
  auto t = e.ReadPoint(ts, "nuova fine dell'asse");
  if (!t) return false;
  const ON_3dVector axis = *s - *a;
  const double L2 = axis.LengthSquared();
  if (L2 < e.Tolerance() * e.Tolerance()) return e.Fail("asse troppo corto");
  const ON_3dVector w = axis / L2, m = *t - *s;
  ON_Xform x = ON_Xform::IdentityTransformation;
  for (int r = 0; r < 3; r++) {
    for (int k = 0; k < 3; k++) x.m_xform[r][k] += m[r] * w[k];
    x.m_xform[r][3] = -m[r] * (ON_3dVector(*a) * w);
  }
  int n = 0;
  for (const auto& id : sel) {
    const ON_Geometry* g = Geo(e, id);
    if (!g) continue;
    ON_Geometry* c = g->Duplicate();
    // le estrusioni non accettano trasformazioni non uniformi: si passa al B-rep
    if (!c->Transform(x)) {
      delete c;
      c = nullptr;
      if (const auto* ex = ON_Extrusion::Cast(g)) {
        ON_Brep* br = ex->BrepForm();
        if (br && br->Transform(x)) c = br; else delete br;
      }
    }
    if (c && !ON_UuidIsNil(e.ReplaceObject(id, c))) n++;
  }
  if (!n) return e.Fail("nessun oggetto allungato");
  e.Out() << "  Allungati " << n << " oggetti\n";
  return true;
}

// --- Maelstrom -------------------------------------------------------------------
// Vortice attorno all'asse Z del punto centro: rotazione Angle= piena entro il primo raggio,
// che si annulla al secondo raggio.
bool CmdMaelstrom(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("Maelstrom", "seleziona gli oggetti da deformare, poi Invio");
  if (sel.empty()) return false;
  auto c = e.ReadPoint(ts, "centro del vortice");
  if (!c) return false;
  auto r1 = e.ReadNumber(ts, "primo raggio (rotazione piena)");
  if (!r1) return false;
  auto r2 = e.ReadNumber(ts, "secondo raggio (nessuna rotazione)");
  if (!r2) return false;
  auto ang = e.ReadNumber(ts, "angolo di rotazione (gradi)");
  if (!ang) return false;
  const double a1 = std::min(*r1, *r2), a2 = std::max(*r1, *r2);
  if (a2 <= 0) return e.Fail("raggi non validi");
  const double A = *ang * ON_PI / 180.0;
  const ON_3dPoint C = *c;
  auto f = [&](const ON_3dPoint& q) {
    const double dx = q.x - C.x, dy = q.y - C.y, r = std::hypot(dx, dy);
    double k = r <= a1 ? 1.0 : (r >= a2 ? 0.0 : (a2 - r) / (a2 - a1));
    k = k * k * (3 - 2 * k);  // smussato
    const double th = A * k, cs = std::cos(th), sn = std::sin(th);
    return ON_3dPoint(C.x + dx * cs - dy * sn, C.y + dx * sn + dy * cs, q.z);
  };
  int n = 0;
  for (const auto& id : sel) {
    ON_Geometry* g = DeformGeometry(Geo(e, id), f);
    if (g && !ON_UuidIsNil(e.ReplaceObject(id, g))) n++;
  }
  if (!n) return e.Fail("nessun oggetto deformabile");
  e.Out() << "  Vortice applicato a " << n << " oggetti\n";
  return true;
}

// --- MoveUVN ---------------------------------------------------------------------
// Sposta il punto di controllo scelto di una superficie nelle direzioni U, V e normale
// della superficie in quel punto (U=, V=, N= distanze).
bool CmdMoveUVN(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto p = e.ReadPick(ts, "punto di controllo della superficie");
  if (!p) return false;
  opts.merge(e.ReadOptions(ts));
  if (!opts.count("u") && !opts.count("v") && !opts.count("n")) {
    auto d = e.ReadNumber(ts, "distanza lungo la normale");
    if (!d) return false;
    opts["n"] = std::to_string(*d);
  }
  const double U = OptNum(opts, "u", 0), V = OptNum(opts, "v", 0), N = OptNum(opts, "n", 0);
  // punto di controllo piu' vicino tra le superfici
  ON_UUID best = ON_nil_uuid;
  int bi = -1, bj = -1;
  double bd = 1e300;
  ON_NurbsSurface bestS;
  for (const auto& oid : e.AllObjectIds(false)) {
    const ON_Geometry* g = Geo(e, oid);
    const ON_Surface* s = ON_Surface::Cast(g);
    const ON_Brep* b = ON_Brep::Cast(g);
    if (!s && b && b->m_F.Count() == 1) s = b->m_F[0].SurfaceOf();
    if (!s) continue;
    ON_NurbsSurface ns;
    if (s->GetNurbForm(ns) != 1) continue;
    for (int i = 0; i < ns.CVCount(0); i++)
      for (int j = 0; j < ns.CVCount(1); j++) {
        ON_3dPoint q;
        ns.GetCV(i, j, q);
        const double d = q.DistanceTo(*p);
        if (d < bd) { bd = d; best = oid; bi = i; bj = j; bestS = ns; }
      }
  }
  if (bi < 0) return e.Fail("nessuna superficie con punti di controllo");
  const double gu = bestS.GrevilleAbcissa(0, bi), gv = bestS.GrevilleAbcissa(1, bj);
  ON_3dPoint P;
  ON_3dVector Du, Dv;
  bestS.Ev1Der(gu, gv, P, Du, Dv);
  ON_3dVector n = ON_CrossProduct(Du, Dv);
  Du.Unitize();
  n.Unitize();
  Dv = ON_CrossProduct(n, Du);
  AddToCV(bestS, bi, bj, Du * U + Dv * V + n * N);
  const ON_Geometry* g = Geo(e, best);
  ON_Geometry* out = nullptr;
  if (const auto* b = ON_Brep::Cast(g)) {
    ON_Brep* nb = b->Duplicate();
    const int si = nb->m_F[0].m_si;
    delete nb->m_S[si];
    nb->m_S[si] = new ON_NurbsSurface(bestS);
    nb->m_F[0].SetProxySurface(nb->m_S[si]);
    RebuildBrepEdges(*nb);
    nb->DestroyMesh(ON::any_mesh);
    out = nb;
  } else {
    out = new ON_NurbsSurface(bestS);
  }
  e.ReplaceObject(best, out);
  e.Out() << "  Punto (" << bi << "," << bj << ") spostato U=" << U << " V=" << V << " N=" << N << "\n";
  return true;
}

// --- Cage ------------------------------------------------------------------------
// Gabbia di controllo indipendente (senza oggetti catturati): due angoli opposti del
// parallelepipedo (o BoundingBox degli oggetti selezionati), PointCount=X,Y,Z, Degree=.
bool CmdCage(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  ON_BoundingBox bb;
  for (const auto& id : e.Selection()) if (const ON_Geometry* g = Geo(e, id)) bb.Union(g->BoundingBox());
  if (!bb.IsValid()) {
    auto a = e.ReadPoint(ts, "primo angolo della gabbia");
    if (!a) return false;
    auto b = e.ReadPoint(ts, "angolo opposto della gabbia");
    if (!b) return false;
    bb.Set(*a, false);
    bb.Set(*b, true);
    opts.merge(e.ReadOptions(ts));
  }
  int cnt[3] = {4, 4, 4};
  if (opts.count("pointcount")) std::sscanf(opts["pointcount"].c_str(), "%d,%d,%d", &cnt[0], &cnt[1], &cnt[2]);
  const int deg = std::clamp((int)OptNum(opts, "degree", 3), 1, 5);
  const double pad = std::max(bb.Diagonal().Length() * 0.01, e.Tolerance() * 10);
  for (int k = 0; k < 3; k++) if (bb.m_max[k] - bb.m_min[k] < pad) { bb.m_min[k] -= pad / 2; bb.m_max[k] += pad / 2; }
  for (int k = 0; k < 3; k++) cnt[k] = std::clamp(cnt[k], 2, 30);
  auto* mc = new ON_MorphControl;
  mc->m_varient = 3;
  if (!mc->m_nurbs_cage.Create(bb, std::min(deg, cnt[0] - 1) + 1, std::min(deg, cnt[1] - 1) + 1, std::min(deg, cnt[2] - 1) + 1,
                               cnt[0], cnt[1], cnt[2])) {
    delete mc;
    return e.Fail("gabbia non creata");
  }
  ON_Xform x = ON_Xform::IdentityTransformation;
  for (int k = 0; k < 3; k++) {
    const ON_Interval d = mc->m_nurbs_cage.Domain(k);
    const double sc = d.Length() / (bb.m_max[k] - bb.m_min[k]);
    x.m_xform[k][k] = sc;
    x.m_xform[k][3] = d[0] - bb.m_min[k] * sc;
  }
  mc->m_nurbs_cage0 = x;
  e.AddObject(mc);
  e.Out() << "  Gabbia " << cnt[0] << "x" << cnt[1] << "x" << cnt[2] << " creata (usa CageEdit per catturare oggetti)\n";
  return true;
}

// --- MeshFromLines ---------------------------------------------------------------
// Mesh dalle facce chiuse (triangoli e quadrilateri) formate da una rete di linee.
bool CmdMeshFromLines(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("MeshFromLines", "seleziona le linee, poi Invio");
  if (sel.empty()) return false;
  const double tol = e.Tolerance() * 10;
  std::vector<ON_3dPoint> V;
  auto vid = [&](const ON_3dPoint& p) {
    for (size_t i = 0; i < V.size(); i++) if (V[i].DistanceTo(p) <= tol) return (int)i;
    V.push_back(p);
    return (int)V.size() - 1;
  };
  std::set<std::pair<int, int>> E;
  auto addSeg = [&](const ON_3dPoint& a, const ON_3dPoint& b) {
    const int i = vid(a), j = vid(b);
    if (i != j) E.insert({std::min(i, j), std::max(i, j)});
  };
  for (const auto& id : sel) {
    const auto* c = ON_Curve::Cast(Geo(e, id));
    if (!c) continue;
    ON_Polyline pl;
    if (c->IsPolyline(&pl) >= 2) {
      for (int k = 0; k + 1 < pl.Count(); k++) addSeg(pl[k], pl[k + 1]);
    } else if (c->IsLinear(e.Tolerance())) {
      addSeg(c->PointAtStart(), c->PointAtEnd());
    }
  }
  if (E.empty()) return e.Fail("nessuna linea");
  // le linee si spezzano nei punti in cui passano per un vertice di un'altra linea
  std::set<std::pair<int, int>> E2;
  for (const auto& [a, b] : E) {
    std::vector<std::pair<double, int>> on;
    const ON_Line L(V[a], V[b]);
    for (int k = 0; k < (int)V.size(); k++) {
      if (k == a || k == b) continue;
      double t;
      if (L.ClosestPointTo(V[k], &t) && t > 1e-9 && t < 1 - 1e-9 && L.PointAt(t).DistanceTo(V[k]) <= tol) on.push_back({t, k});
    }
    std::sort(on.begin(), on.end());
    int prev = a;
    for (auto& [t, k] : on) { E2.insert({std::min(prev, k), std::max(prev, k)}); prev = k; }
    E2.insert({std::min(prev, b), std::max(prev, b)});
  }
  std::map<int, std::set<int>> adj;
  for (const auto& [a, b] : E2) { adj[a].insert(b); adj[b].insert(a); }
  auto has = [&](int a, int b) { return adj.count(a) && adj[a].count(b); };
  std::set<std::vector<int>> faces;
  auto planar = [&](const std::vector<int>& f) {
    if (f.size() == 3) return true;
    ON_Plane pl;
    if (!pl.CreateFromPoints(V[f[0]], V[f[1]], V[f[2]])) return false;
    return std::fabs(pl.DistanceTo(V[f[3]])) <= V[f[0]].DistanceTo(V[f[2]]) * 0.25;
  };
  for (const auto& [a, nb] : adj)
    for (int b : nb) {
      if (b <= a) continue;
      for (int c : adj[b]) {
        if (c == a) continue;
        if (has(c, a)) {  // triangolo
          std::vector<int> f{a, b, c};
          auto s = f; std::sort(s.begin(), s.end());
          if (s[0] == a) faces.insert(s);
          continue;
        }
        for (int d : adj[c]) {
          if (d == a || d == b || !has(d, a)) continue;
          if (has(a, c) || has(b, d)) continue;  // con diagonale: sono triangoli
          std::vector<int> f{a, b, c, d};
          if (!planar(f)) continue;
          // quadrilatero: chiave = sequenza ordinata a partire dal minimo
          auto s = f; std::sort(s.begin(), s.end());
          if (s[0] != a) continue;
          std::vector<int> key = f;
          if (key[1] > key[3]) std::swap(key[1], key[3]);
          faces.insert({key[0], key[1], key[2], key[3], -1});
        }
      }
    }
  if (faces.empty()) return e.Fail("le linee non formano facce chiuse di 3 o 4 lati");
  auto* m = new ON_Mesh;
  for (const auto& p : V) m->m_V.Append(ON_3fPoint(p));
  for (const auto& f : faces) {
    if (f.size() == 3) m->SetTriangle(m->m_F.Count(), f[0], f[1], f[2]);
    else m->SetQuad(m->m_F.Count(), f[0], f[1], f[2], f[3]);
  }
  m->CullUnusedVertices();
  m->ComputeVertexNormals();
  m->Compact();
  e.AddObject(m);
  e.Out() << "  Mesh con " << faces.size() << " facce\n";
  return true;
}

// --- spigoli dei B-rep -----------------------------------------------------------
// B-rep e spigolo piu' vicino al punto.
bool NearestBrepEdge(Engine& e, const ON_3dPoint& p, ON_UUID& id, int& ei, double& t, const ON_UUID* only = nullptr) {
  double best = 1e300;
  for (const auto& oid : e.AllObjectIds(false)) {
    if (only && ON_UuidCompare(*only, oid) != 0) continue;
    const auto* b = ON_Brep::Cast(Geo(e, oid));
    if (!b) continue;
    for (int i = 0; i < b->m_E.Count(); i++) {
      const double tt = ClosestParam(b->m_E[i], p);
      const double d = b->m_E[i].PointAt(tt).DistanceTo(p);
      if (d < best) { best = d; id = oid; ei = i; t = tt; }
    }
  }
  return best < 1e300;
}

// UnjoinEdge: separa le due facce lungo lo spigolo scelto (lo spigolo diventa due spigoli nudi).
bool CmdUnjoinEdge(Engine& e, TokenStream& ts) {
  auto p = e.ReadPick(ts, "spigolo da separare");
  if (!p) return false;
  ON_UUID id;
  int ei;
  double t;
  if (!NearestBrepEdge(e, *p, id, ei, t)) return e.Fail("nessuno spigolo");
  ON_Brep* b = ON_Brep::Cast(Geo(e, id))->Duplicate();
  ON_BrepEdge& E = b->m_E[ei];
  if (E.m_ti.Count() < 2) { delete b; return e.Fail("lo spigolo e' gia' nudo"); }
  // il secondo trim riceve uno spigolo nuovo con la stessa curva e vertici nuovi
  const int ti = E.m_ti[1];
  ON_BrepTrim& T = b->m_T[ti];
  const int c3 = b->AddEdgeCurve(E.EdgeCurveOf()->DuplicateCurve());
  // i vertici restano condivisi: le due facce si toccano ancora negli estremi
  const int v0i = E.m_vi[0], v1i = E.m_vi[1];
  const double etol = E.m_tolerance;
  ON_BrepEdge& NE = b->NewEdge(b->m_V[v0i], b->m_V[v1i], c3, nullptr, etol);
  const int nei = NE.m_edge_index;
  b->m_E[nei].m_ti.Append(ti);
  ON_BrepEdge& E2 = b->m_E[ei];  // NewEdge puo' riallocare
  for (int k = 0; k < E2.m_ti.Count(); k++) if (E2.m_ti[k] == ti) { E2.m_ti.Remove(k); break; }
  b->m_T[ti].m_ei = nei;
  b->m_T[ti].m_type = ON_BrepTrim::boundary;
  for (int k = 0; k < b->m_E[ei].m_ti.Count(); k++)
    if (b->m_E[ei].m_ti.Count() == 1) b->m_T[b->m_E[ei].m_ti[k]].m_type = ON_BrepTrim::boundary;
  b->SetTrimTypeFlags(true);
  b->SetTolerancesBoxesAndFlags(true, true, true, true, true, true, true, true);
  b->Compact();
  e.ReplaceObject(id, b);
  e.Out() << "  Spigolo separato in due spigoli nudi\n";
  return true;
}

// --- NamedSelections -------------------------------------------------------------
// Selezioni con nome salvate nel documento (testo del documento "Kurva3D.NamedSelection.<nome>").
const char* kNamedSelPrefix = "Kurva3D.NamedSelection.";

bool CmdNamedSelections(Engine& e, TokenStream& ts) {
  auto act = e.ReadString(ts, "azione (Save, Restore, Delete, List)");
  if (!act) return false;
  const std::string a = NormalizeName(*act);
  if (a == "list") {
    int n = 0;
    const ON_wString pref(kNamedSelPrefix);
    ON_ClassArray<ON_UserString> all;
    if (e.Model().GetDocumentUserStrings(all)) {
      for (int i = 0; i < all.Count(); i++)
        if (all[i].m_key.Left(pref.Length()) == pref) {
          e.Out() << "  " << ON_String(all[i].m_key.Right(all[i].m_key.Length() - pref.Length())).Array() << "\n";
          n++;
        }
    }
    if (!n) e.Out() << "  Nessuna selezione con nome\n";
    return true;
  }
  auto name = e.ReadString(ts, "nome della selezione");
  if (!name) return false;
  const ON_wString key = ON_wString(kNamedSelPrefix) + ON_wString(Unquote(*name).c_str());
  if (a == "save") {
    auto sel = e.Selection();
    if (sel.empty()) return e.Fail("nessun oggetto selezionato");
    std::string v;
    for (const auto& id : sel) {
      char buf[64];
      ON_UuidToString(id, buf);
      if (!v.empty()) v += ";";
      v += buf;
    }
    e.Model().SetDocumentUserString(key, static_cast<const wchar_t*>(ON_wString(v.c_str())));
    e.Out() << "  Selezione \"" << Unquote(*name) << "\" salvata (" << sel.size() << " oggetti)\n";
    return true;
  }
  ON_wString val;
  if (!e.Model().GetDocumentUserString(key, val)) return e.Fail("selezione inesistente: " + Unquote(*name));
  if (a == "delete") {
    e.Model().SetDocumentUserString(key, nullptr);
    e.Out() << "  Selezione eliminata\n";
    return true;
  }
  if (a != "restore") return e.Fail("azione sconosciuta: usa Save, Restore, Delete o List");
  e.ClearSelection();
  std::stringstream ss(ON_String(val).Array());
  std::string tok;
  int n = 0;
  while (std::getline(ss, tok, ';')) {
    const ON_UUID id = ON_UuidFromString(tok.c_str());
    if (e.Object(id)) { e.Select(id); n++; }
  }
  e.Out() << "  Selezionati " << n << " oggetti\n";
  return true;
}

// --- LayerStateManager -----------------------------------------------------------
// Stati dei livelli (acceso, bloccato, colore, colore e spessore di stampa, tipo di linea)
// salvati nel documento come testo "Kurva3D.LayerState.<nome>".
const char* kLayerStatePrefix = "Kurva3D.LayerState.";

bool CmdLayerStateManager(Engine& e, TokenStream& ts) {
  auto act = e.ReadString(ts, "azione (Save, Restore, Delete, List)");
  if (!act) return false;
  const std::string a = NormalizeName(*act);
  ONX_Model& m = e.Model();
  if (a == "list") {
    ON_ClassArray<ON_UserString> all;
    int n = 0;
    const ON_wString pref(kLayerStatePrefix);
    if (e.Model().GetDocumentUserStrings(all))
      for (int i = 0; i < all.Count(); i++)
        if (all[i].m_key.Left(pref.Length()) == pref) {
          e.Out() << "  " << ON_String(all[i].m_key.Right(all[i].m_key.Length() - pref.Length())).Array() << "\n";
          n++;
        }
    if (!n) e.Out() << "  Nessuno stato dei livelli\n";
    return true;
  }
  auto name = e.ReadString(ts, "nome dello stato");
  if (!name) return false;
  const ON_wString key = ON_wString(kLayerStatePrefix) + ON_wString(Unquote(*name).c_str());
  if (a == "save") {
    std::ostringstream o;
    ONX_ModelComponentIterator it(m, ON_ModelComponent::Type::Layer);
    for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent()) {
      const ON_Layer* L = ON_Layer::Cast(c);
      if (!L) continue;
      char buf[64];
      ON_UuidToString(L->Id(), buf);
      const ON_Color col = L->Color(), pc = L->PlotColor();
      o << buf << ' ' << (L->IsVisible() ? 1 : 0) << ' ' << (L->IsLocked() ? 1 : 0) << ' ' << (unsigned)col << ' '
        << (unsigned)pc << ' ' << L->PlotWeight() << ' ' << L->LinetypeIndex() << ' ' << L->RenderMaterialIndex() << "\n";
    }
    e.Model().SetDocumentUserString(key, static_cast<const wchar_t*>(ON_wString(o.str().c_str())));
    e.Out() << "  Stato dei livelli \"" << Unquote(*name) << "\" salvato\n";
    return true;
  }
  ON_wString val;
  if (!e.Model().GetDocumentUserString(key, val) || val.IsEmpty()) return e.Fail("stato inesistente: " + Unquote(*name));
  if (a == "delete") {
    e.Model().SetDocumentUserString(key, nullptr);
    e.Out() << "  Stato eliminato\n";
    return true;
  }
  if (a != "restore") return e.Fail("azione sconosciuta: usa Save, Restore, Delete o List");
  std::istringstream in(ON_String(val).Array());
  std::string line;
  int n = 0;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string sid;
    int vis, lock, lt, mat;
    unsigned col, pc;
    double pw;
    if (!(ls >> sid >> vis >> lock >> col >> pc >> pw >> lt >> mat)) continue;
    const ON_UUID lid = ON_UuidFromString(sid.c_str());
    ON_ModelComponentReference ref = m.ComponentFromId(ON_ModelComponent::Type::Layer, lid);
    ON_Layer* L = const_cast<ON_Layer*>(ON_Layer::Cast(ref.ModelComponent()));
    if (!L) continue;
    L->SetVisible(vis != 0);
    L->SetLocked(lock != 0);
    L->SetColor(ON_Color(col));
    L->SetPlotColor(ON_Color(pc));
    L->SetPlotWeight(pw);
    L->SetLinetypeIndex(lt);
    L->SetRenderMaterialIndex(mat);
    n++;
  }
  e.Out() << "  Ripristinati " << n << " livelli\n";
  return true;
}

}  // namespace

#ifndef CAD_WITH_OCCT
void RebuildBrepEdges(ON_Brep&) {}
#endif

void RegisterMissingCommands(Engine& e) {
  auto reg = [&](const char* n, const char* cat, const char* d, CommandFn f, bool mod = true) {
    CommandInfo i{n, cat, d, f};
    i.modifiesModel = mod;
    e.Register(i);
  };
  reg("InsertKink", "Curve", "Inserisce uno spigolo (nodo multiplo) nella curva", CmdInsertKink);
  reg("SoftEditCrv", "Curve", "Sposta un punto della curva con attenuazione morbida (Length=, FixEnds)", CmdSoftEditCrv);
  reg("SoftEditSrf", "Superfici", "Sposta un punto della superficie con attenuazione morbida (UDistance=, VDistance=, FixEdges)", CmdSoftEditSrf);
  reg("SoftMove", "Trasforma", "Sposta i punti vicini con attenuazione morbida (Radius=)", CmdSoftMove);
  reg("Stretch", "Trasforma", "Allunga gli oggetti lungo un asse", CmdStretch);
  reg("Maelstrom", "Trasforma", "Deformazione a vortice attorno a un centro", CmdMaelstrom);
  reg("MoveUVN", "Trasforma", "Sposta un punto di controllo nelle direzioni U, V e normale (U=, V=, N=)", CmdMoveUVN);
  reg("Cage", "Trasforma", "Crea una gabbia di controllo (PointCount=, Degree=)", CmdCage);
  reg("MeshFromLines", "Mesh", "Mesh dalle facce chiuse formate da linee", CmdMeshFromLines);
  reg("UnjoinEdge", "Solidi", "Separa le facce lungo uno spigolo", CmdUnjoinEdge);
  reg("NamedSelections", "Selezione", "Selezioni con nome (Save, Restore, Delete, List)", CmdNamedSelections);
  reg("LayerStateManager", "Livelli", "Stati dei livelli con nome (Save, Restore, Delete, List)", CmdLayerStateManager);
}

}  // namespace cad
