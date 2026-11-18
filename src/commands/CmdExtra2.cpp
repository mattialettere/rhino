// Altri comandi di Rhino 8 realizzati con openNURBS (terza parte):
// curve (Lines, PolylineThroughPoints, CurveThroughPt, Bisector, AngleLine,
// RoundedRectangle, Parabola, Hyperbola, Smooth, InsertKnot, CreateUVCrv),
// solidi (Box3Pt, Sphere3Pt), SubD (SubDSphere, SubDPlane, SubDTorus,
// SubDCylinder, SubDFromMesh), mesh (Weld, MeshPolyline, MeshToNURB),
// trasformazioni (Shear, SetPt, Align), selezione (SelColor, SelSmall),
// proprieta' (MatchProperties, SetUserText, GetUserText), Purge.
#include "commands/Engine.h"
#include "commands/Grips.h"
#include "core/SubDLimit.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <sstream>

namespace cad {
namespace {

const ON_Geometry* Geo(Engine& e, const ON_UUID& id) {
  const auto* o = e.Object(id);
  return o ? o->Geometry(nullptr) : nullptr;
}

bool Created(Engine& e, const ON_UUID& id, const char* what) {
  if (ON_UuidIsNil(id)) return e.Fail(std::string("impossibile creare ") + what);
  e.Out() << "  Creato: " << what << "\n";
  return true;
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

std::string Unquote(std::string s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
  return s;
}

const ON_3dmObjectAttributes& AttrsOf(Engine& e, const ON_UUID& id) {
  return *e.Object(id)->Attributes(&ON_3dmObjectAttributes::DefaultAttributes);
}

// Parametro del punto della curva piu' vicino a p (campionamento e raffinamento).
double ClosestT(const ON_Curve& c, const ON_3dPoint& p, double* dist = nullptr) {
  const ON_Interval d = c.Domain();
  const int n = 400;
  double bestT = d[0], best = 1e300;
  for (int i = 0; i <= n; i++) {
    const double t = d.ParameterAt(double(i) / n);
    const double dd = c.PointAt(t).DistanceTo(p);
    if (dd < best) { best = dd; bestT = t; }
  }
  double lo = std::max(d[0], bestT - d.Length() / n), hi = std::min(d[1], bestT + d.Length() / n);
  for (int it = 0; it < 60; it++) {  // sezione aurea nell'intervallo vicino
    const double a = lo + (hi - lo) * 0.382, b = lo + (hi - lo) * 0.618;
    if (c.PointAt(a).DistanceTo(p) < c.PointAt(b).DistanceTo(p)) hi = b; else lo = a;
  }
  const double t = 0.5 * (lo + hi);
  if (dist) *dist = c.PointAt(t).DistanceTo(p);
  return t;
}

// Distanza approssimata di p da un oggetto (per scegliere quello cliccato).
double DistanceTo(const ON_Geometry* g, const ON_3dPoint& p) {
  if (const auto* pt = ON_Point::Cast(g)) return pt->point.DistanceTo(p);
  if (const auto* c = ON_Curve::Cast(g)) { double d; ClosestT(*c, p, &d); return d; }
  if (const auto* b = ON_Brep::Cast(g)) {
    double best = 1e300;
    for (int i = 0; i < b->m_E.Count(); i++) { double d; ClosestT(b->m_E[i], p, &d); best = std::min(best, d); }
    const ON_BoundingBox bb = b->BoundingBox();
    if (bb.IsPointIn(p)) best = std::min(best, 0.0);
    return best;
  }
  if (const auto* m = ON_Mesh::Cast(g)) {
    double best = 1e300;
    for (int i = 0; i < m->m_V.Count(); i++) best = std::min(best, ON_3dPoint(m->m_V[i]).DistanceTo(p));
    return best;
  }
  if (g) {
    const ON_BoundingBox bb = g->BoundingBox();
    return bb.ClosestPoint(p).DistanceTo(p);
  }
  return 1e300;
}

ON_UUID NearestObject(Engine& e, const ON_3dPoint& p, const std::vector<ON_UUID>& exclude) {
  ON_UUID best = ON_nil_uuid;
  double bd = 1e300;
  for (const auto& id : e.AllObjectIds(false)) {
    if (std::find(exclude.begin(), exclude.end(), id) != exclude.end()) continue;
    const double d = DistanceTo(Geo(e, id), p);
    if (d < bd) { bd = d; best = id; }
  }
  return best;
}

// --- Curve --------------------------------------------------------------------
// Lines: segmenti separati, uno per ogni coppia di punti (Invio per finire).
bool CmdLines(Engine& e, TokenStream& ts) {
  auto a = e.ReadPoint(ts, "inizio della linea");
  if (!a) return false;
  int n = 0;
  while (e.MoreInput(ts)) {
    auto b = e.ReadPoint(ts, "fine della linea (Invio per finire)");
    if (!b) return false;
    if (a->DistanceTo(*b) > e.Tolerance()) { e.AddObject(new ON_LineCurve(*a, *b)); n++; }
    if (!e.MoreInput(ts)) break;
    a = e.ReadPoint(ts, "inizio della linea successiva");
    if (!a) return false;
  }
  e.SkipEnter(ts);
  if (!n) return e.Fail("nessuna linea creata");
  e.Out() << "  Create " << n << " linee\n";
  return true;
}

std::vector<ON_3dPoint> SelectedPoints(Engine& e, const std::vector<ON_UUID>& sel) {
  std::vector<ON_3dPoint> pts;
  for (const auto& id : sel) {
    const ON_Geometry* g = Geo(e, id);
    if (const auto* p = ON_Point::Cast(g)) pts.push_back(p->point);
    else if (const auto* pc = ON_PointCloud::Cast(g))
      for (int i = 0; i < pc->m_P.Count(); i++) pts.push_back(pc->m_P[i]);
  }
  return pts;
}

// PolylineThroughPoints: polilinea per i punti selezionati (nell'ordine di selezione).
bool CmdPolylineThroughPoints(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("PolylineThroughPoints", "seleziona i punti, poi Invio");
  if (sel.empty()) return false;
  auto pts = SelectedPoints(e, sel);
  if (pts.size() < 2) return e.Fail("servono almeno 2 punti");
  ON_Polyline pl;
  for (const auto& p : pts) pl.Append(p);
  if (OptYes(opts, "closed", false) && pts.size() > 2) pl.Append(pts.front());
  return Created(e, e.AddObject(new ON_PolylineCurve(pl)), "polilinea per i punti");
}

// CurveThroughPt: curva cubica interpolata per i punti selezionati.
bool CmdCurveThroughPt(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("CurveThroughPt", "seleziona i punti, poi Invio");
  if (sel.empty()) return false;
  auto pts = SelectedPoints(e, sel);
  if (pts.size() < 2) return e.Fail("servono almeno 2 punti");
  ON_NurbsCurve* c = InterpolateCubic(pts);
  if (!c) return e.Fail("interpolazione non riuscita");
  return Created(e, e.AddObject(c), "curva per i punti");
}

// Bisector: vertice dell'angolo, punto sul primo lato, punto sul secondo lato, lunghezza.
bool CmdBisector(Engine& e, TokenStream& ts) {
  auto v = e.ReadPoint(ts, "vertice dell'angolo");
  if (!v) return false;
  auto a = e.ReadPoint(ts, "punto sul primo lato");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "punto sul secondo lato");
  if (!b) return false;
  auto len = e.ReadNumber(ts, "lunghezza della bisettrice");
  if (!len) return false;
  ON_3dVector da = *a - *v, db = *b - *v;
  if (!da.Unitize() || !db.Unitize()) return e.Fail("lati nulli");
  ON_3dVector d = da + db;
  if (!d.Unitize()) return e.Fail("i lati sono opposti: bisettrice indefinita");
  if (std::fabs(*len) <= e.Tolerance()) return e.Fail("lunghezza nulla");
  return Created(e, e.AddObject(new ON_LineCurve(*v, *v + d * *len)), "bisettrice");
}

// AngleLine (Line con angolo): inizio e fine della linea di riferimento,
// angolo in gradi (antiorario sul piano XY), lunghezza.
bool CmdAngleLine(Engine& e, TokenStream& ts) {
  auto a = e.ReadPoint(ts, "inizio della linea di riferimento");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "fine della linea di riferimento");
  if (!b) return false;
  auto deg = e.ReadNumber(ts, "angolo");
  if (!deg) return false;
  auto len = e.ReadNumber(ts, "lunghezza");
  if (!len) return false;
  ON_3dVector d = *b - *a;
  if (!d.Unitize()) return e.Fail("riferimento nullo");
  d.Rotate(*deg * ON_PI / 180.0, ON_3dVector(0, 0, 1));
  if (std::fabs(*len) <= e.Tolerance()) return e.Fail("lunghezza nulla");
  return Created(e, e.AddObject(new ON_LineCurve(*a, *a + d * *len)), "linea ad angolo");
}

// RoundedRectangle: due angoli e raggio degli angoli (archi tangenti).
bool CmdRoundedRectangle(Engine& e, TokenStream& ts) {
  auto a = e.ReadPoint(ts, "primo angolo");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "altro angolo");
  if (!b) return false;
  auto r = e.ReadNumber(ts, "raggio degli angoli");
  if (!r) return false;
  const double x0 = std::min(a->x, b->x), x1 = std::max(a->x, b->x);
  const double y0 = std::min(a->y, b->y), y1 = std::max(a->y, b->y), z = a->z;
  const double R = *r;
  if (x1 - x0 <= e.Tolerance() || y1 - y0 <= e.Tolerance()) return e.Fail("rettangolo degenere");
  if (R <= 0 || 2 * R > std::min(x1 - x0, y1 - y0) + 1e-12) return e.Fail("raggio non valido per il rettangolo");
  auto* pc = new ON_PolyCurve();
  auto line = [&](double xa, double ya, double xb, double yb) {
    if (std::hypot(xb - xa, yb - ya) > 1e-12) pc->Append(new ON_LineCurve(ON_3dPoint(xa, ya, z), ON_3dPoint(xb, yb, z)));
  };
  auto arc = [&](double cx, double cy, double a0) {
    ON_Arc ar(ON_Circle(ON_Plane(ON_3dPoint(cx, cy, z), ON_3dVector(0, 0, 1)), R), ON_Interval(a0, a0 + ON_PI / 2));
    pc->Append(new ON_ArcCurve(ar));
  };
  line(x0 + R, y0, x1 - R, y0);
  arc(x1 - R, y0 + R, -ON_PI / 2);
  line(x1, y0 + R, x1, y1 - R);
  arc(x1 - R, y1 - R, 0);
  line(x1 - R, y1, x0 + R, y1);
  arc(x0 + R, y1 - R, ON_PI / 2);
  line(x0, y1 - R, x0, y0 + R);
  arc(x0 + R, y0 + R, ON_PI);
  if (!pc->IsValid() || !pc->IsClosed()) { delete pc; return e.Fail("rettangolo non valido"); }
  return Created(e, e.AddObject(pc), "rettangolo arrotondato");
}

// Conica come Bezier razionale quadratica (p0, p1 con peso w, p2).
ON_NurbsCurve* Conic(const ON_3dPoint& p0, const ON_3dPoint& p1, double w, const ON_3dPoint& p2) {
  auto* c = new ON_NurbsCurve(3, w != 1.0, 3, 3);
  c->m_knot[0] = c->m_knot[1] = 0;
  c->m_knot[2] = c->m_knot[3] = 1;
  if (w != 1.0) {
    c->SetCV(0, ON_4dPoint(p0.x, p0.y, p0.z, 1));
    c->SetCV(1, ON_4dPoint(p1.x * w, p1.y * w, p1.z * w, w));
    c->SetCV(2, ON_4dPoint(p2.x, p2.y, p2.z, 1));
  } else {
    c->SetCV(0, p0);
    c->SetCV(1, p1);
    c->SetCV(2, p2);
  }
  if (!c->IsValid()) { delete c; return nullptr; }
  return c;
}

// Parabola: vertice, fuoco, semiapertura (distanza dall'asse degli estremi).
bool CmdParabola(Engine& e, TokenStream& ts) {
  auto v = e.ReadPoint(ts, "vertice");
  if (!v) return false;
  auto f = e.ReadPoint(ts, "fuoco");
  if (!f) return false;
  auto w = e.ReadNumber(ts, "semiapertura");
  if (!w) return false;
  ON_3dVector ay = *f - *v;
  const double fl = ay.Length();
  if (!ay.Unitize() || *w <= e.Tolerance()) return e.Fail("parabola degenere");
  ON_3dVector ax = ON_CrossProduct(ay, ON_3dVector(0, 0, 1));
  if (!ax.Unitize()) { ax = ON_CrossProduct(ay, ON_3dVector(1, 0, 0)); ax.Unitize(); }
  const double W = *w, h = W * W / (4 * fl);
  ON_NurbsCurve* c = Conic(*v - ax * W + ay * h, *v - ay * h, 1.0, *v + ax * W + ay * h);
  if (!c) return e.Fail("parabola non valida");
  return Created(e, e.AddObject(c), "parabola");
}

// Hyperbola: centro, vertice (definisce a e l'asse), b, semiapertura del ramo.
bool CmdHyperbola(Engine& e, TokenStream& ts) {
  auto c = e.ReadPoint(ts, "centro");
  if (!c) return false;
  auto v = e.ReadPoint(ts, "vertice");
  if (!v) return false;
  auto b = e.ReadNumber(ts, "semiasse b");
  if (!b) return false;
  auto Y = e.ReadNumber(ts, "semiapertura del ramo");
  if (!Y) return false;
  ON_3dVector ax = *v - *c;
  const double a = ax.Length();
  if (!ax.Unitize() || *b <= e.Tolerance() || *Y <= e.Tolerance()) return e.Fail("iperbole degenere");
  ON_3dVector ay = ON_CrossProduct(ON_3dVector(0, 0, 1), ax);
  if (!ay.Unitize()) { ay = ON_CrossProduct(ON_3dVector(0, 1, 0), ax); ay.Unitize(); }
  const double B = *b, x0 = a * std::sqrt(1 + (*Y * *Y) / (B * B));
  // tangenti agli estremi si incontrano sull'asse in a^2/x0; peso = x0/a
  ON_NurbsCurve* cv = Conic(*c + ax * x0 - ay * *Y, *c + ax * (a * a / x0), x0 / a, *c + ax * x0 + ay * *Y);
  if (!cv) return e.Fail("iperbole non valida");
  return Created(e, e.AddObject(cv), "iperbole");
}

// Smooth: media dei punti di controllo con i vicini (Factor=, estremi fissi).
bool CmdSmooth(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("Smooth", "seleziona le curve da levigare, poi Invio");
  if (sel.empty()) return false;
  const double f = OptNum(opts, "factor", OptNum(opts, "smoothfactor", 0.5));
  int n = 0;
  for (const auto& id : sel) {
    const auto* c = ON_Curve::Cast(Geo(e, id));
    if (!c) continue;
    auto* nc = new ON_NurbsCurve();
    if (!c->GetNurbForm(*nc) || nc->CVCount() < 3) { delete nc; continue; }
    const bool closed = nc->IsPeriodic();
    std::vector<ON_3dPoint> p(nc->CVCount());
    for (int i = 0; i < nc->CVCount(); i++) nc->GetCV(i, p[i]);
    const int N = nc->CVCount();
    for (int i = closed ? 0 : 1; i < (closed ? N : N - 1); i++) {
      const ON_3dPoint& a = p[(i - 1 + N) % N];
      const ON_3dPoint& b = p[(i + 1) % N];
      nc->SetCV(i, p[i] + f * (0.5 * (a + b) - p[i]));
    }
    ON_3dmObjectAttributes at = AttrsOf(e, id);
    e.DeleteObject(id);
    e.AddObject(nc, &at);
    n++;
  }
  if (!n) return e.Fail("nessuna curva levigata");
  e.Out() << "  Levigate " << n << " curve\n";
  return true;
}

// InsertKnot: curva selezionata, poi i punti dove inserire i nodi.
bool CmdInsertKnot(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("InsertKnot", "seleziona la curva, poi Invio");
  if (sel.empty()) return false;
  const auto* c = ON_Curve::Cast(Geo(e, sel.front()));
  if (!c) return e.Fail("serve una curva");
  auto* nc = new ON_NurbsCurve();
  if (!c->GetNurbForm(*nc)) { delete nc; return e.Fail("curva non convertibile"); }
  int n = 0;
  do {
    auto p = e.ReadPick(ts, "punto dove inserire il nodo (Invio per finire)");
    if (!p) { delete nc; return false; }
    const double t = ClosestT(*nc, *p);
    if (t > nc->Domain()[0] && t < nc->Domain()[1] && nc->InsertKnot(t, 1)) n++;
  } while (e.MoreInput(ts));
  e.SkipEnter(ts);
  if (!n) { delete nc; return e.Fail("nessun nodo inserito"); }
  ON_3dmObjectAttributes at = AttrsOf(e, sel.front());
  e.DeleteObject(sel.front());
  e.AddObject(nc, &at);
  e.Out() << "  Inseriti " << n << " nodi\n";
  return true;
}

// CreateUVCrv: le curve di taglio di ogni faccia nello spazio parametrico (piano XY).
bool CmdCreateUVCrv(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("CreateUVCrv", "seleziona le superfici, poi Invio");
  if (sel.empty()) return false;
  int n = 0;
  for (const auto& id : sel) {
    const auto* b = ON_Brep::Cast(Geo(e, id));
    if (!b) continue;
    for (int ti = 0; ti < b->m_T.Count(); ti++) {
      const ON_Curve* c2 = b->m_T[ti].TrimCurveOf();
      if (!c2) continue;
      ON_Curve* c3 = c2->DuplicateCurve();
      c3->ChangeDimension(3);
      e.AddObject(c3);
      n++;
    }
  }
  if (!n) return e.Fail("nessuna curva creata");
  e.Out() << "  Create " << n << " curve UV\n";
  return true;
}

// --- Solidi -------------------------------------------------------------------
// Box3Pt: due estremi di un lato della base, larghezza, altezza.
bool CmdBox3Pt(Engine& e, TokenStream& ts) {
  auto a = e.ReadPoint(ts, "inizio del lato");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "fine del lato");
  if (!b) return false;
  auto c = e.ReadPoint(ts, "larghezza");
  if (!c) return false;
  auto h = e.ReadNumber(ts, "altezza");
  if (!h) return false;
  ON_3dVector x = *b - *a;
  x.z = 0;
  const double len = x.Length();
  if (!x.Unitize()) return e.Fail("lato nullo");
  ON_3dVector w = *c - *a;
  w.z = 0;
  w = w - x * (w * x);
  if (w.Length() <= e.Tolerance() || len <= e.Tolerance() || std::fabs(*h) <= e.Tolerance()) return e.Fail("parallelepipedo degenere");
  ON_3dVector up(0, 0, *h);
  ON_3dPoint p0 = *a, p1 = *a + x * len, p2 = p1 + w, p3 = p0 + w;
  if (ON_CrossProduct(x, w).z * *h < 0) std::swap(p1, p3);  // normali verso l'esterno
  ON_3dPoint corners[8] = {p0, p1, p2, p3, p0 + up, p1 + up, p2 + up, p3 + up};
  if (*h < 0) for (int i = 0; i < 4; i++) std::swap(corners[i], corners[i + 4]);
  ON_Brep* br = ON_BrepBox(corners);
  if (!br) return e.Fail("parallelepipedo non valido");
  return Created(e, e.AddObject(br), "parallelepipedo");
}

// Sphere3Pt: tre punti sulla circonferenza massima.
bool CmdSphere3Pt(Engine& e, TokenStream& ts) {
  auto a = e.ReadPoint(ts, "primo punto");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "secondo punto");
  if (!b) return false;
  auto c = e.ReadPoint(ts, "terzo punto");
  if (!c) return false;
  ON_Circle circle;
  if (!circle.Create(*a, *b, *c)) return e.Fail("i tre punti sono allineati");
  ON_Brep* br = ON_BrepSphere(ON_Sphere(circle.Center(), circle.Radius()));
  if (!br) return e.Fail("sfera non valida");
  return Created(e, e.AddObject(br), "sfera");
}

// --- SubD ---------------------------------------------------------------------
// Il SubD si costruisce da una gabbia a quadrilateri; la gabbia e' scalata
// perche' la superficie limite passi per la misura richiesta (raggio o lato).
ON_SubD* SubDFromCage(ON_Mesh* cage) {
  std::unique_ptr<ON_Mesh> m(cage);
  if (!m || !m->IsValid()) return nullptr;
  return ON_SubD::CreateFromMesh(m.get(), nullptr, nullptr);
}

// Riquadro della superficie limite.
ON_BoundingBox LimitBox(const ON_SubD& s) {
  SubDLimitDisplay lim = BuildSubDLimit(s, 2);
  return lim.mesh ? lim.mesh->BoundingBox() : s.BoundingBox();
}

bool AddSubD(Engine& e, ON_SubD* s, const char* what) {
  if (!s || !s->IsValid()) { delete s; return e.Fail(std::string(what) + " non valido"); }
  return Created(e, e.AddObject(s), what);
}

bool CmdSubDSphere(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto c = e.ReadPoint(ts, "centro della sfera");
  if (!c) return false;
  auto r = e.ReadNumber(ts, "raggio");
  if (!r) return false;
  if (*r <= e.Tolerance()) return e.Fail("raggio nullo");
  const int nu = std::max(3, OptInt(opts, "aroundcount", 8)), nv = std::max(2, OptInt(opts, "verticalcount", 4));
  const ON_3dPoint C = *c;
  ON_SubD* s = SubDFromCage(GridMesh(nu, nv, true, [&](double u, double v) {
    if (v <= 0 || v >= 1) return ON_3dPoint(0, 0, v <= 0 ? -1 : 1);
    const double a = 2 * ON_PI * u, b = -ON_PI / 2 + ON_PI * v;
    return ON_3dPoint(std::cos(b) * std::cos(a), std::cos(b) * std::sin(a), std::sin(b));
  }));
  if (!s) return e.Fail("SubD non valido");
  const ON_BoundingBox bb = LimitBox(*s);
  const ON_3dVector half = 0.5 * (bb.m_max - bb.m_min);
  ON_Xform x = ON_Xform::TranslationTransformation(C - ON_3dPoint::Origin) *
               ON_Xform::DiagonalTransformation(*r / half.x, *r / half.y, *r / half.z);
  s->Transform(x);
  return AddSubD(e, s, "sfera SubD");
}

bool CmdSubDPlane(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto a = e.ReadPoint(ts, "primo angolo");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "altro angolo");
  if (!b) return false;
  if (std::fabs(b->x - a->x) <= e.Tolerance() || std::fabs(b->y - a->y) <= e.Tolerance()) return e.Fail("piano degenere");
  const int nx = std::max(1, OptInt(opts, "xfaces", 1)), ny = std::max(1, OptInt(opts, "yfaces", 1));
  const ON_3dPoint A = *a, B = *b;
  // bordi con angoli (Corner) come in Rhino: il limite tocca gli angoli
  ON_SubD* s = SubDFromCage(GridMesh(nx, ny, false, [&](double u, double v) {
    return ON_3dPoint(A.x + u * (B.x - A.x), A.y + v * (B.y - A.y), A.z);
  }));
  return AddSubD(e, s, "piano SubD");
}

bool CmdSubDCylinder(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto c = e.ReadPoint(ts, "centro della base");
  if (!c) return false;
  auto r = e.ReadNumber(ts, "raggio");
  if (!r) return false;
  auto h = e.ReadNumber(ts, "altezza");
  if (!h) return false;
  if (*r <= e.Tolerance() || std::fabs(*h) <= e.Tolerance()) return e.Fail("cilindro degenere");
  const int n = std::max(3, OptInt(opts, "aroundcount", 8)), nv = std::max(1, OptInt(opts, "verticalcount", 2));
  const ON_3dPoint C = *c;
  const double H = *h;
  // parete aperta (bordi superiore e inferiore lasciati aperti come con Cap=None)
  ON_SubD* s = SubDFromCage(GridMesh(n, nv, true, [&](double u, double v) {
    const double a = 2 * ON_PI * u;
    return ON_3dPoint(std::cos(a), std::sin(a), v * H);
  }));
  if (!s) return e.Fail("SubD non valido");
  const ON_BoundingBox bb = LimitBox(*s);
  // i bordi sono pieghe: in altezza il limite coincide con la gabbia
  const double ext = 0.5 * (bb.m_max.x - bb.m_min.x);
  ON_Xform sc = ON_Xform::DiagonalTransformation(*r / ext, *r / ext, 1.0);
  s->Transform(ON_Xform::TranslationTransformation(C - ON_3dPoint::Origin) * sc);
  return AddSubD(e, s, "cilindro SubD");
}

bool CmdSubDTorus(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto c = e.ReadPoint(ts, "centro del toro");
  if (!c) return false;
  auto R = e.ReadNumber(ts, "raggio maggiore");
  if (!R) return false;
  auto r = e.ReadNumber(ts, "raggio minore");
  if (!r) return false;
  if (*r <= e.Tolerance() || *R <= *r) return e.Fail("toro non valido (serve raggio maggiore > minore > 0)");
  const int nu = std::max(3, OptInt(opts, "ringcount", 8)), nv = std::max(3, OptInt(opts, "segmentcount", 4));
  const ON_3dPoint C = *c;
  // si cerca il fattore della sezione per cui il limite ha il raggio minore voluto
  double k = 1.0;
  ON_SubD* s = nullptr;
  for (int it = 0; it < 4; it++) {
    delete s;
    const double Rr = *R, rr = *r * k;
    s = SubDFromCage(GridMesh(nu, nv, true, [&](double u, double v) {
      const double a = 2 * ON_PI * u, b = 2 * ON_PI * v;
      return ON_3dPoint((Rr + rr * std::cos(b)) * std::cos(a), (Rr + rr * std::cos(b)) * std::sin(a), rr * std::sin(b));
    }, true));
    if (!s) return e.Fail("SubD non valido");
    const double zmax = LimitBox(*s).m_max.z;
    if (zmax <= 0) break;
    k *= *r / zmax;
  }
  s->Transform(ON_Xform::TranslationTransformation(C - ON_3dPoint::Origin));
  return AddSubD(e, s, "toro SubD");
}

bool CmdSubDFromMesh(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("SubDFromMesh", "seleziona le mesh, poi Invio");
  if (sel.empty()) return false;
  const bool keep = OptYes(opts, "deleteinput", false) == false;
  int n = 0;
  for (const auto& id : sel) {
    const auto* m = ON_Mesh::Cast(Geo(e, id));
    if (!m) continue;
    ON_SubD* s = ON_SubD::CreateFromMesh(m, nullptr, nullptr);
    if (!s) continue;
    ON_3dmObjectAttributes at = AttrsOf(e, id);
    e.AddObject(s, &at);
    if (!keep) e.DeleteObject(id);
    n++;
  }
  if (!n) return e.Fail("nessuna mesh convertita");
  e.Out() << "  Convertite " << n << " mesh in SubD\n";
  return true;
}

// --- Mesh ---------------------------------------------------------------------
// Weld: unisce i vertici coincidenti (normali condivise, mesh liscia).
bool CmdWeld(Engine& e, TokenStream& ts) {
  e.ReadOptions(ts);
  auto sel = e.RequireSelection("Weld", "seleziona le mesh, poi Invio");
  if (sel.empty()) return false;
  int n = 0;
  for (const auto& id : sel) {
    auto* m = ON_Mesh::Cast(e.EditableGeometry(id));
    if (!m) continue;
    m->m_N.Destroy();
    m->CombineIdenticalVertices(true, true);
    m->CullUnusedVertices();
    m->ComputeVertexNormals();
    m->Compact();
    n++;
  }
  if (!n) return e.Fail("nessuna mesh selezionata");
  e.Out() << "  Saldate " << n << " mesh\n";
  return true;
}

// MeshPolyline: mesh da una polilinea chiusa (ventaglio dal baricentro).
bool CmdMeshPolyline(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("MeshPolyline", "seleziona le polilinee chiuse, poi Invio");
  if (sel.empty()) return false;
  int n = 0;
  for (const auto& id : sel) {
    const auto* c = ON_Curve::Cast(Geo(e, id));
    ON_Polyline pl;
    if (!c || !c->IsClosed() || !c->IsPolyline(&pl) || pl.Count() < 4) continue;
    const int k = pl.Count() - 1;
    auto* m = new ON_Mesh();
    if (k == 3 || k == 4) {
      for (int i = 0; i < k; i++) m->m_V.Append(ON_3fPoint(pl[i]));
      if (k == 3) m->SetTriangle(0, 0, 1, 2); else m->SetQuad(0, 0, 1, 2, 3);
    } else {
      ON_3dPoint cen = ON_3dPoint::Origin;
      for (int i = 0; i < k; i++) { cen += pl[i]; m->m_V.Append(ON_3fPoint(pl[i])); }
      m->m_V.Append(ON_3fPoint(cen / k));
      for (int i = 0; i < k; i++) m->SetTriangle(i, k, i, (i + 1) % k);
    }
    m->ComputeVertexNormals();
    if (!m->IsValid()) { delete m; continue; }
    e.AddObject(m);
    n++;
  }
  if (!n) return e.Fail("serve una polilinea chiusa");
  e.Out() << "  Create " << n << " mesh\n";
  return true;
}

// MeshToNURB: ogni faccia della mesh diventa una superficie del B-rep.
bool CmdMeshToNURB(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("MeshToNURB", "seleziona le mesh, poi Invio");
  if (sel.empty()) return false;
  const bool del = OptYes(opts, "deleteinput", false);
  int n = 0;
  for (const auto& id : sel) {
    const auto* m = ON_Mesh::Cast(Geo(e, id));
    if (!m) continue;
    ON_Brep* b = ON_BrepFromMesh(m->Topology(), true);
    if (!b) continue;
    b->SetTolerancesBoxesAndFlags();
    // openNURBS non calcola le tolleranze: i bordi coincidono esattamente
    for (int i = 0; i < b->m_E.Count(); i++) if (!(b->m_E[i].m_tolerance >= 0)) b->m_E[i].m_tolerance = 0;
    for (int i = 0; i < b->m_V.Count(); i++) if (!(b->m_V[i].m_tolerance >= 0)) b->m_V[i].m_tolerance = 0;
    for (int i = 0; i < b->m_T.Count(); i++)
      for (int k = 0; k < 2; k++) if (!(b->m_T[i].m_tolerance[k] >= 0)) b->m_T[i].m_tolerance[k] = 0;
    if (!b->IsValid()) { delete b; continue; }
    ON_3dmObjectAttributes at = AttrsOf(e, id);
    e.AddObject(b, &at);
    if (del) e.DeleteObject(id);
    n++;
  }
  if (!n) return e.Fail("nessuna mesh convertita");
  e.Out() << "  Convertite " << n << " mesh in polisuperfici\n";
  return true;
}

// --- Trasformazioni -------------------------------------------------------------
bool Apply(Engine& e, const std::vector<ON_UUID>& sel, const ON_Xform& x) {
  int n = 0;
  for (const auto& id : sel)
    if (ON_Geometry* g = e.EditableGeometry(id); g && g->Transform(x)) n++;
  e.Out() << "  Trasformati " << n << " oggetti\n";
  return n > 0 || e.Fail("nessun oggetto trasformato");
}

// Shear: origine, punto di riferimento (direzione dello scorrimento), angolo.
bool CmdShear(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("Shear");
  if (sel.empty()) return false;
  auto o = e.ReadPoint(ts, "origine");
  if (!o) return false;
  auto r = e.ReadPoint(ts, "punto di riferimento");
  if (!r) return false;
  auto deg = e.ReadNumber(ts, "angolo di scorrimento");
  if (!deg) return false;
  ON_3dVector d = *r - *o;
  if (!d.Unitize()) return e.Fail("riferimento nullo");
  ON_3dVector p = ON_CrossProduct(ON_3dVector(0, 0, 1), d);
  if (!p.Unitize()) return e.Fail("direzione verticale non ammessa");
  if (std::fabs(*deg) >= 89.9) return e.Fail("angolo troppo grande");
  const double t = std::tan(*deg * ON_PI / 180.0);
  // x' = x + t * ((x - o) . p) * d
  ON_Xform x = ON_Xform::IdentityTransformation;
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++) x.m_xform[i][j] += t * d[i] * p[j];
  const double op = ON_3dVector(*o) * p;
  for (int i = 0; i < 3; i++) x.m_xform[i][3] -= t * d[i] * op;
  return Apply(e, sel, x);
}

// SetPt: porta X, Y e/o Z degli oggetti al valore del punto (SetX=, SetY=, SetZ=).
bool CmdSetPt(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("SetPt");
  if (sel.empty()) return false;
  opts.merge(e.ReadOptions(ts));
  auto p = e.ReadPoint(ts, "posizione");
  if (!p) return false;
  const bool sx = OptYes(opts, "setx", false), sy = OptYes(opts, "sety", false), sz = OptYes(opts, "setz", true);
  ON_Xform x = ON_Xform::IdentityTransformation;
  if (sx) { x.m_xform[0][0] = 0; x.m_xform[0][3] = p->x; }
  if (sy) { x.m_xform[1][1] = 0; x.m_xform[1][3] = p->y; }
  if (sz) { x.m_xform[2][2] = 0; x.m_xform[2][3] = p->z; }
  return Apply(e, sel, x);
}

// Align: allinea gli oggetti al riquadro della selezione (Mode=Left, Right,
// Top, Bottom, HCenter, VCenter, Center), sul piano XY.
bool CmdAlign(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("Align");
  if (sel.empty()) return false;
  opts.merge(e.ReadOptions(ts));
  std::string mode = opts.count("mode") ? opts["mode"] : "";
  if (mode.empty() && e.MoreInput(ts)) {
    auto m = e.ReadString(ts, "allineamento (Left, Right, Top, Bottom, HCenter, VCenter, Center)");
    if (!m) return false;
    mode = NormalizeName(*m);
  }
  if (mode.empty()) mode = "left";
  ON_BoundingBox all;
  for (const auto& id : sel) if (const ON_Geometry* g = Geo(e, id)) all.Union(g->BoundingBox());
  int n = 0;
  for (const auto& id : sel) {
    ON_Geometry* g = e.EditableGeometry(id);
    if (!g) continue;
    const ON_BoundingBox bb = g->BoundingBox();
    ON_3dVector d(0, 0, 0);
    if (mode == "left") d.x = all.m_min.x - bb.m_min.x;
    else if (mode == "right") d.x = all.m_max.x - bb.m_max.x;
    else if (mode == "bottom") d.y = all.m_min.y - bb.m_min.y;
    else if (mode == "top") d.y = all.m_max.y - bb.m_max.y;
    else if (mode == "hcenter" || mode == "horizontalcenter") d.x = all.Center().x - bb.Center().x;
    else if (mode == "vcenter" || mode == "verticalcenter") d.y = all.Center().y - bb.Center().y;
    else if (mode == "center") { d.x = all.Center().x - bb.Center().x; d.y = all.Center().y - bb.Center().y; }
    else return e.Fail("allineamento sconosciuto: " + mode);
    if (d.Length() > 0) g->Transform(ON_Xform::TranslationTransformation(d));
    n++;
  }
  e.Out() << "  Allineati " << n << " oggetti\n";
  return true;
}

// --- Selezione e proprieta' --------------------------------------------------------
ON_Color DisplayColor(Engine& e, const ON_3dmObjectAttributes& a) {
  if (a.ColorSource() == ON::color_from_object) return a.m_color;
  ON_ModelComponentReference ref = e.Model().LayerFromIndex(a.m_layer_index);
  const auto* l = ON_Layer::Cast(ref.ModelComponent());
  return l ? l->Color() : ON_Color::Black;
}

// SelColor: seleziona gli oggetti che hanno il colore di visualizzazione di
// quelli selezionati (oppure il colore r,g,b indicato).
bool CmdSelColor(Engine& e, TokenStream& ts) {
  std::set<unsigned int> colors;
  for (const auto& id : e.Selection()) colors.insert(static_cast<unsigned int>(DisplayColor(e, AttrsOf(e, id))));
  if (colors.empty()) {
    auto s = e.ReadString(ts, "colore r,g,b");
    if (!s) return false;
    int r, g, b;
    if (std::sscanf(Unquote(*s).c_str(), "%d,%d,%d", &r, &g, &b) != 3) return e.Fail("colore non valido (r,g,b)");
    colors.insert(static_cast<unsigned int>(ON_Color(r, g, b)));
  }
  int n = 0;
  for (const auto& id : e.AllObjectIds(false))
    if (colors.count(static_cast<unsigned int>(DisplayColor(e, AttrsOf(e, id)))) && !e.IsSelected(id)) { e.Select(id); n++; }
  e.Out() << "  Selezionati " << n << " oggetti\n";
  return true;
}

// SelSmall: oggetti con diagonale del riquadro minore della misura indicata.
bool CmdSelSmall(Engine& e, TokenStream& ts) {
  auto s = e.ReadNumber(ts, "misura massima");
  if (!s) return false;
  int n = 0;
  for (const auto& id : e.AllObjectIds(false)) {
    const ON_Geometry* g = Geo(e, id);
    if (!g) continue;
    const ON_BoundingBox bb = g->BoundingBox();
    if (bb.IsValid() && bb.Diagonal().Length() < *s) { e.Select(id); n++; }
  }
  e.Out() << "  Selezionati " << n << " oggetti\n";
  return true;
}

// MatchProperties: gli oggetti selezionati prendono livello, colore, tipo di
// linea, materiale e spessore di stampa dell'oggetto cliccato.
bool CmdMatchProperties(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("MatchProperties", "seleziona gli oggetti da modificare, poi Invio");
  if (sel.empty()) return false;
  auto p = e.ReadPick(ts, "oggetto da cui copiare le proprieta'");
  if (!p) return false;
  const ON_UUID src = NearestObject(e, *p, sel);
  if (ON_UuidIsNil(src)) return e.Fail("oggetto di riferimento non trovato");
  const ON_3dmObjectAttributes s = AttrsOf(e, src);
  int n = 0;
  for (const auto& id : sel) {
    ON_3dmObjectAttributes* a = e.EditableAttributes(id);
    if (!a) continue;
    a->m_layer_index = s.m_layer_index;
    a->SetColorSource(s.ColorSource());
    a->m_color = s.m_color;
    a->SetLinetypeSource(s.LinetypeSource());
    a->m_linetype_index = s.m_linetype_index;
    a->SetMaterialSource(s.MaterialSource());
    a->m_material_index = s.m_material_index;
    a->SetPlotColorSource(s.PlotColorSource());
    a->m_plot_color = s.m_plot_color;
    a->SetPlotWeightSource(s.PlotWeightSource());
    a->m_plot_weight_mm = s.m_plot_weight_mm;
    n++;
  }
  e.Out() << "  Proprieta' copiate su " << n << " oggetti\n";
  return true;
}

// SetUserText: chiave e valore (testo utente dell'oggetto; valore "" la cancella).
bool CmdSetUserText(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("SetUserText");
  if (sel.empty()) return false;
  auto k = e.ReadString(ts, "chiave");
  if (!k) return false;
  auto v = e.ReadString(ts, "valore");
  if (!v) return false;
  const std::string key = Unquote(*k), val = Unquote(*v);
  if (key.empty()) return e.Fail("chiave vuota");
  const ON_wString wk(key.c_str()), wv(val.c_str());
  int n = 0;
  for (const auto& id : sel)
    if (ON_3dmObjectAttributes* a = e.EditableAttributes(id))
      if (a->SetUserString(wk, val.empty() ? nullptr : static_cast<const wchar_t*>(wv))) n++;
  e.Out() << "  Testo utente impostato su " << n << " oggetti\n";
  return true;
}

// GetUserText: elenca il testo utente degli oggetti selezionati.
bool CmdGetUserText(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("GetUserText");
  if (sel.empty()) return false;
  for (const auto& id : sel) {
    ON_ClassArray<ON_UserString> us;
    AttrsOf(e, id).GetUserStrings(us);
    for (int i = 0; i < us.Count(); i++) {
      if (us[i].m_key.Length() && us[i].m_key[0] == L'.') continue;  // chiavi interne
      e.Out() << "  " << ON_String(us[i].m_key).Array() << " = " << ON_String(us[i].m_string_value).Array() << "\n";
    }
  }
  return true;
}

// Purge: elimina livelli vuoti (non correnti, senza sottolivelli) e
// definizioni di blocco non usate.
bool CmdPurge(Engine& e, TokenStream&) {
  ONX_Model& m = e.Model();
  std::set<int> usedLayers;
  std::set<ON_UUID, UuidLess> usedDefs;
  {
    ONX_ModelComponentIterator it(m, ON_ModelComponent::Type::ModelGeometry);
    for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent()) {
      const auto* g = ON_ModelGeometryComponent::Cast(c);
      if (!g) continue;
      if (const auto* a = g->Attributes(nullptr)) usedLayers.insert(a->m_layer_index);
      if (const auto* ir = ON_InstanceRef::Cast(g->Geometry(nullptr))) usedDefs.insert(ir->m_instance_definition_uuid);
    }
  }
  // definizioni usate da altre definizioni (blocchi annidati) restano
  std::vector<ON_UUID> defsToRemove;
  {
    ONX_ModelComponentIterator it(m, ON_ModelComponent::Type::InstanceDefinition);
    for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent()) {
      const auto* d = ON_InstanceDefinition::Cast(c);
      if (!d) continue;
      const ON_SimpleArray<ON_UUID>& ids = d->InstanceGeometryIdList();
      for (int i = 0; i < ids.Count(); i++) {
        const ON_ModelGeometryComponent& g = m.ModelGeometryComponentFromId(ids[i]);
        if (const auto* ir = ON_InstanceRef::Cast(g.Geometry(nullptr))) usedDefs.insert(ir->m_instance_definition_uuid);
      }
    }
    for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent())
      if (!usedDefs.count(c->Id())) defsToRemove.push_back(c->Id());
  }
  std::set<ON_UUID, UuidLess> parents;
  std::vector<std::pair<ON_UUID, int>> layers;
  {
    ONX_ModelComponentIterator it(m, ON_ModelComponent::Type::Layer);
    for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent()) {
      const auto* l = ON_Layer::Cast(c);
      if (!l) continue;
      if (!ON_UuidIsNil(l->ParentLayerId())) parents.insert(l->ParentLayerId());
      layers.push_back({l->Id(), l->Index()});
    }
  }
  const ON_UUID current = m.m_settings.CurrentLayerId();
  int nl = 0, nd = 0;
  for (const auto& id : defsToRemove)
    if (!m.RemoveModelComponent(ON_ModelComponent::Type::InstanceDefinition, id).IsEmpty()) nd++;
  for (const auto& [id, idx] : layers) {
    if (usedLayers.count(idx) || parents.count(id) || ON_UuidCompare(id, current) == 0) continue;
    if (layers.size() - nl <= 1) break;  // resta almeno un livello
    if (!m.RemoveModelComponent(ON_ModelComponent::Type::Layer, id).IsEmpty()) nl++;
  }
  e.Out() << "  Eliminati " << nl << " livelli e " << nd << " definizioni di blocco non usati\n";
  return true;
}


// --- Punti di controllo ------------------------------------------------------------
// PointsOn: mostra i punti di controllo degli oggetti selezionati (F10).
bool CmdPointsOn(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("PointsOn", "seleziona gli oggetti, poi Invio");
  if (sel.empty()) return false;
  int n = 0;
  for (const auto& id : sel)
    if (SupportsGrips(Geo(e, id))) { e.pointsOn.insert(id); n++; }
  if (!n) return e.Fail("nessun oggetto con punti di controllo modificabili (polisuperfici e superfici tagliate non sono supportate)");
  e.ClearSelection();
  e.Out() << "  Punti di controllo attivi su " << n << " oggetti\n";
  return true;
}

// PointsOff: nasconde tutti i punti di controllo (F11).
bool CmdPointsOff(Engine& e, TokenStream&) {
  const size_t n = e.pointsOn.size();
  e.pointsOn.clear();
  e.Out() << "  Punti di controllo disattivati su " << n << " oggetti\n";
  return true;
}

// GripMove (interno, usato trascinando un punto nella vista):
// id dell'oggetto, indice i, indice j (-1 per le curve), nuova posizione.
bool CmdGripMove(Engine& e, TokenStream& ts) {
  auto sid = e.ReadString(ts, "id dell'oggetto");
  if (!sid) return false;
  auto i = e.ReadInt(ts, "indice del punto");
  if (!i) return false;
  auto j = e.ReadInt(ts, "secondo indice");
  if (!j) return false;
  auto p = e.ReadPoint(ts, "nuova posizione");
  if (!p) return false;
  const ON_UUID id = ON_UuidFromString(Unquote(*sid).c_str());
  const ON_Geometry* g = Geo(e, id);
  if (!g) return e.Fail("oggetto inesistente");
  ON_Geometry* ng = MoveGrip(g, *i, *j, *p);
  if (!ng) return e.Fail("punto di controllo non modificabile");
  e.pointsOn.erase(id);
  const ON_UUID nid = e.ReplaceObject(id, ng);
  if (ON_UuidIsNil(nid)) return e.Fail("sostituzione non riuscita");
  e.pointsOn.insert(nid);
  return true;
}

// --- File ------------------------------------------------------------------------
// Livello con lo stesso nome nel modello di destinazione (creato se manca).
int MatchLayer(ONX_Model& dst, const ONX_Model& src, int srcIndex) {
  const auto* sl = ON_Layer::Cast(src.LayerFromIndex(srcIndex).ModelComponent());
  if (!sl) return 0;
  ONX_ModelComponentIterator it(dst, ON_ModelComponent::Type::Layer);
  for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent())
    if (const auto* l = ON_Layer::Cast(c); l && ON_wString::EqualOrdinal(l->Name(), sl->Name(), true)) return l->Index();
  return dst.AddLayer(static_cast<const wchar_t*>(sl->Name()), sl->Color());
}

// Import: aggiunge al modello gli oggetti di un altro file .3dm (livelli per nome).
bool CmdImport(Engine& e, TokenStream& ts) {
  auto path = e.ReadString(ts, "file da importare");
  if (!path) return false;
  Document3dm src;
  if (!src.Load(Unquote(*path))) return e.Fail("impossibile aprire " + Unquote(*path));
  e.ClearSelection();
  int n = 0, skipped = 0;
  ONX_ModelComponentIterator it(src.Model(), ON_ModelComponent::Type::ModelGeometry);
  for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent()) {
    const auto* mg = ON_ModelGeometryComponent::Cast(c);
    const ON_Geometry* g = mg ? mg->Geometry(nullptr) : nullptr;
    const ON_3dmObjectAttributes* a = mg ? mg->Attributes(nullptr) : nullptr;
    if (!g || !a) continue;
    if (ON_InstanceRef::Cast(g)) { skipped++; continue; }  // i blocchi richiedono le definizioni
    ON_3dmObjectAttributes at = *a;
    at.m_layer_index = MatchLayer(e.Model(), src.Model(), a->m_layer_index);
    at.m_material_index = -1;
    at.SetMaterialSource(ON::material_from_layer);
    at.m_linetype_index = -1;
    at.SetLinetypeSource(ON::linetype_from_layer);
    at.RemoveFromAllGroups();
    const ON_UUID id = e.AddObject(g->Duplicate(), &at);
    if (!ON_UuidIsNil(id)) { e.Select(id); n++; }
  }
  e.Out() << "  Importati " << n << " oggetti";
  if (skipped) e.Out() << " (" << skipped << " blocchi non importati)";
  e.Out() << "\n";
  return n > 0 || e.Fail("nessun oggetto importato");
}

// Export (Esporta selezione): salva gli oggetti selezionati in un nuovo file Rhino 8.
bool CmdExport(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("Export", "seleziona gli oggetti da esportare, poi Invio");
  if (sel.empty()) return false;
  auto path = e.ReadString(ts, "nome del file");
  if (!path) return false;
  Document3dm out;
  out.NewEmpty();
  out.Model().m_settings.m_ModelUnitsAndTolerances = e.Model().m_settings.m_ModelUnitsAndTolerances;
  int n = 0;
  for (const auto& id : sel) {
    const ON_Geometry* g = Geo(e, id);
    if (!g || ON_InstanceRef::Cast(g)) continue;
    ON_3dmObjectAttributes at = AttrsOf(e, id);
    at.m_layer_index = MatchLayer(out.Model(), e.Model(), at.m_layer_index);
    at.m_material_index = -1;
    at.SetMaterialSource(ON::material_from_layer);
    at.m_linetype_index = -1;
    at.SetLinetypeSource(ON::linetype_from_layer);
    at.RemoveFromAllGroups();
    auto* a = new ON_3dmObjectAttributes(at);
    if (!out.Model().AddManagedModelGeometryComponent(g->Duplicate(), a, true).IsEmpty()) n++;
  }
  if (!n) return e.Fail("nessun oggetto esportabile");
  if (!out.Save(Unquote(*path), kRhino8ArchiveVersion)) return e.Fail("impossibile salvare " + Unquote(*path));
  e.Out() << "  Esportati " << n << " oggetti in " << Unquote(*path) << " (formato Rhino 8)\n";
  return true;
}

// IncrementalSave: salva "nome 001.3dm", "nome 002.3dm"... accanto al file corrente.
bool CmdIncrementalSave(Engine& e, TokenStream& ts) {
  std::string base = e.currentPath;
  if (base.empty()) {
    auto p = e.ReadString(ts, "nome del file");
    if (!p) return false;
    base = Unquote(*p);
  }
  if (base.size() > 4 && (base.substr(base.size() - 4) == ".3dm" || base.substr(base.size() - 4) == ".3DM")) base.resize(base.size() - 4);
  // toglie un eventuale " 001" gia' presente
  if (base.size() > 4 && base[base.size() - 4] == ' ' && std::all_of(base.end() - 3, base.end(), ::isdigit)) base.resize(base.size() - 4);
  std::string path;
  for (int k = 1; k < 1000; k++) {
    char num[8];
    std::snprintf(num, sizeof num, " %03d", k);
    path = base + num + ".3dm";
    if (FILE* f = std::fopen(path.c_str(), "rb")) { std::fclose(f); continue; }
    break;
  }
  if (!e.Doc().Save(path, kRhino8ArchiveVersion)) return e.Fail("impossibile salvare " + path);
  e.currentPath = path;
  e.Out() << "  Salvato " << path << " (formato Rhino 8)\n";
  return true;
}

// SelPrev: riseleziona gli oggetti dell'ultima selezione non vuota (prima di quella attuale).
bool CmdSelPrev(Engine& e, TokenStream&) {
  int n = 0;
  for (const auto& id : e.PreviousSelection())
    if (e.Object(id) && !e.IsSelected(id)) { e.Select(id); n++; }
  e.Out() << "  Selezionati " << n << " oggetti\n";
  return true;
}

// --- Isocurve ----------------------------------------------------------------------
struct TrimRegion {
  std::vector<std::vector<ON_2dPoint>> loops;
  bool Inside(double u, double v) const {
    if (loops.empty()) return true;
    bool in = false;
    for (const auto& poly : loops)
      for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
        if (((poly[i].y > v) != (poly[j].y > v)) &&
            (u < (poly[j].x - poly[i].x) * (v - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x))
          in = !in;
    return in;
  }
};

TrimRegion RegionOf(const ON_Brep& b, const ON_BrepFace& f) {
  TrimRegion r;
  for (int li = 0; li < f.m_li.Count(); li++) {
    const ON_BrepLoop& L = b.m_L[f.m_li[li]];
    std::vector<ON_2dPoint> poly;
    for (int k = 0; k < L.m_ti.Count(); k++) {
      const ON_BrepTrim& t = b.m_T[L.m_ti[k]];
      const ON_Interval dom = t.Domain();
      const int n = 64;
      for (int j = 0; j < n; j++) {
        const ON_3dPoint q = t.PointAt(dom.ParameterAt(double(j) / n));
        poly.push_back(ON_2dPoint(q.x, q.y));
      }
    }
    if (poly.size() >= 3) r.loops.push_back(std::move(poly));
  }
  return r;
}

// Isocurve della faccia a parametro costante c nella direzione dir (0: u costante),
// tagliata ai bordi della faccia.
void TrimmedIsocurves(const ON_Surface& srf, const TrimRegion& reg, int dir, double c, std::vector<ON_Curve*>& out) {
  // ON_Surface::IsoCurve(dir, c): dir=0 -> curva in u con v=c; qui dir indica il parametro costante
  const int isoDir = dir == 0 ? 1 : 0;
  std::unique_ptr<ON_Curve> iso(srf.IsoCurve(isoDir, c));
  if (!iso) return;
  const ON_Interval other = srf.Domain(1 - dir);
  auto in = [&](double t) { return dir == 0 ? reg.Inside(c, t) : reg.Inside(t, c); };
  auto edge = [&](double a, double b) {  // bisezione sul passaggio dentro/fuori
    const bool ia = in(a);
    for (int k = 0; k < 40; k++) { const double m = 0.5 * (a + b); if (in(m) == ia) a = m; else b = m; }
    return 0.5 * (a + b);
  };
  const int n = 256;
  double start = 0;
  bool inside = false;
  double prev = other[0];
  for (int j = 0; j <= n; j++) {
    const double t = other.ParameterAt(double(j) / n);
    const bool cur = in(t);
    if (j == 0) { inside = cur; start = t; prev = t; continue; }
    if (cur != inside) {
      const double x = edge(prev, t);
      if (inside && start <= other[0] && x >= other[1] - 1e-6 * other.Length()) {  // tutta l'isocurva
        out.push_back(iso.release());
        return;
      }
      if (inside && x - start > 1e-9 * other.Length()) {
        ON_Curve* piece = iso->DuplicateCurve();
        if (piece->Trim(ON_Interval(start, x))) out.push_back(piece); else delete piece;
      }
      inside = cur;
      start = x;
    }
    prev = t;
  }
  if (inside && other[1] - start > 1e-9 * other.Length()) {
    if (start <= other[0]) { out.push_back(iso.release()); return; }  // tutta l'isocurva (anche chiusa)
    ON_Curve* piece = iso->DuplicateCurve();
    if (piece->Trim(ON_Interval(start, other[1]))) out.push_back(piece); else delete piece;
  }
}

const ON_Brep* BrepOfGeometry(const ON_Geometry* g, std::unique_ptr<ON_Brep>& holder) {
  if (const auto* b = ON_Brep::Cast(g)) return b;
  if (g && g->HasBrepForm()) { holder.reset(g->BrepForm()); return holder.get(); }
  return nullptr;
}

// ExtractWireframe: spigoli e isocurve delle superfici selezionate come curve.
bool CmdExtractWireframe(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("ExtractWireframe", "seleziona superfici o polisuperfici, poi Invio");
  if (sel.empty()) return false;
  std::vector<ON_UUID> created;
  for (const auto& id : sel) {
    std::unique_ptr<ON_Brep> holder;
    const ON_Brep* b = BrepOfGeometry(Geo(e, id), holder);
    if (!b) continue;
    for (int ei = 0; ei < b->m_E.Count(); ei++)
      if (const ON_Curve* c = b->m_E[ei].EdgeCurveOf()) {
        ON_Curve* d = c->DuplicateCurve();
        if (b->m_E[ei].ProxyCurveIsReversed()) d->Reverse();
        d->Trim(b->m_E[ei].Domain());
        created.push_back(e.AddObject(d));
      }
    for (int fi = 0; fi < b->m_F.Count(); fi++) {
      const ON_Surface* srf = b->m_F[fi].SurfaceOf();
      if (!srf) continue;
      const TrimRegion reg = RegionOf(*b, b->m_F[fi]);
      ON_NurbsSurface ns;
      const bool hasNurb = srf->GetNurbForm(ns) > 0;
      for (int dir = 0; dir < 2; dir++) {
        // nodi interni della forma NURBS (una sfera ha 4 campate in u anche se la
        // superficie di rivoluzione ne ha una), riportati ai parametri della superficie
        std::vector<double> params;
        if (hasNurb && ns.SpanCount(dir) >= 2) {
          std::vector<double> sv(ns.SpanCount(dir) + 1);
          ns.GetSpanVector(dir, sv.data());
          const double mid = ns.Domain(1 - dir).Mid();
          for (size_t k = 1; k + 1 < sv.size(); k++) {
            double s0 = 0, t0 = 0;
            const double nu = dir == 0 ? sv[k] : mid, nv = dir == 0 ? mid : sv[k];
            if (!srf->GetSurfaceParameterFromNurbFormParameter(nu, nv, &s0, &t0)) { s0 = nu; t0 = nv; }
            params.push_back(dir == 0 ? s0 : t0);
          }
        } else if (srf->SpanCount(dir) >= 2) {
          std::vector<double> sv(srf->SpanCount(dir) + 1);
          srf->GetSpanVector(dir, sv.data());
          params.assign(sv.begin() + 1, sv.end() - 1);
        }
        if (params.empty() || params.size() > 64) continue;
        const size_t step = std::max<size_t>(1, params.size() / 16);
        for (size_t k = 0; k < params.size(); k += step) {
          std::vector<ON_Curve*> pieces;
          TrimmedIsocurves(*srf, reg, dir, params[k], pieces);
          for (auto* pc : pieces) created.push_back(e.AddObject(pc));
        }
      }
    }
  }
  if (created.empty()) return e.Fail("nessuna curva estratta");
  e.ClearSelection();
  for (const auto& id : created) e.Select(id);
  e.Out() << "  Estratte " << created.size() << " curve\n";
  return true;
}

// Punto (u,v) della faccia piu' vicino a p: griglia e poi Newton.
bool ClosestFacePoint(const ON_BrepFace& f, const ON_3dPoint& p, double& u, double& v, double& dist) {
  const ON_Surface* s = f.SurfaceOf();
  if (!s) return false;
  const ON_Interval du = s->Domain(0), dv = s->Domain(1);
  dist = 1e300;
  const int n = 40;
  for (int i = 0; i <= n; i++)
    for (int j = 0; j <= n; j++) {
      const double a = du.ParameterAt(double(i) / n), b = dv.ParameterAt(double(j) / n);
      const double d = s->PointAt(a, b).DistanceTo(p);
      if (d < dist) { dist = d; u = a; v = b; }
    }
  for (int it = 0; it < 30; it++) {
    ON_3dPoint P;
    ON_3dVector Su, Sv;
    if (!s->Ev1Der(u, v, P, Su, Sv)) break;
    const ON_3dVector r = P - p;
    const double a11 = Su * Su, a12 = Su * Sv, a22 = Sv * Sv, b1 = -(r * Su), b2 = -(r * Sv);
    const double det = a11 * a22 - a12 * a12;
    if (std::fabs(det) < 1e-30) break;
    const double dU = (b1 * a22 - b2 * a12) / det, dV = (a11 * b2 - a12 * b1) / det;
    u = std::clamp(u + dU, du[0], du[1]);
    v = std::clamp(v + dV, dv[0], dv[1]);
    if (std::fabs(dU) < 1e-14 * (1 + du.Length()) && std::fabs(dV) < 1e-14 * (1 + dv.Length())) break;
  }
  dist = s->PointAt(u, v).DistanceTo(p);
  return true;
}

// ExtractIsocurve: Direction=U|V|Both, poi il punto sulla superficie.
bool CmdExtractIsocurve(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("ExtractIsocurve", "seleziona la superficie, poi Invio");
  if (sel.empty()) return false;
  opts.merge(e.ReadOptions(ts));
  const std::string dirOpt = opts.count("direction") ? opts["direction"] : "u";
  std::unique_ptr<ON_Brep> holder;
  const ON_Brep* b = BrepOfGeometry(Geo(e, sel.front()), holder);
  if (!b) return e.Fail("serve una superficie");
  std::vector<ON_UUID> created;
  do {
    auto p = e.ReadPick(ts, "punto sulla superficie (Invio per finire)");
    if (!p) return false;
    int best = -1;
    double bu = 0, bv = 0, bd = 1e300;
    for (int fi = 0; fi < b->m_F.Count(); fi++) {
      double u, v, d;
      if (ClosestFacePoint(b->m_F[fi], *p, u, v, d) && d < bd) { bd = d; bu = u; bv = v; best = fi; }
    }
    if (best < 0) return e.Fail("punto non sulla superficie");
    const ON_Surface* srf = b->m_F[best].SurfaceOf();
    const TrimRegion reg = RegionOf(*b, b->m_F[best]);
    std::vector<ON_Curve*> pieces;
    // Direction=U: curva lungo u (v costante), come Rhino
    if (dirOpt == "u" || dirOpt == "both") TrimmedIsocurves(*srf, reg, 1, bv, pieces);
    if (dirOpt == "v" || dirOpt == "both") TrimmedIsocurves(*srf, reg, 0, bu, pieces);
    for (auto* pc : pieces) created.push_back(e.AddObject(pc));
  } while (e.MoreInput(ts));
  e.SkipEnter(ts);
  if (created.empty()) return e.Fail("nessuna isocurva estratta");
  e.Out() << "  Estratte " << created.size() << " isocurve\n";
  return true;
}

// --- SubD: pieghe e visualizzazione -----------------------------------------------------
// Spigoli SubD vicini ai punti cliccati (sulla superficie limite).
std::vector<unsigned> PickSubDEdges(Engine& e, TokenStream& ts, const ON_SubD& s) {
  SubDLimitDisplay lim = e.subdFlat ? BuildSubDFlat(s) : BuildSubDLimit(s, 2);
  std::vector<unsigned> ids;
  do {
    auto p = e.ReadPick(ts, "spigolo (Invio per finire)");
    if (!p) return {};
    double best = 1e300;
    unsigned id = 0;
    for (size_t k = 0; k < lim.edges.size(); k++) {
      const auto& w = lim.edges[k];
      for (size_t i = 0; i + 1 < w.size(); i++) {
        ON_Line l(w[i], w[i + 1]);
        double t;
        l.ClosestPointTo(*p, &t);
        const double d = l.PointAt(std::clamp(t, 0.0, 1.0)).DistanceTo(*p);
        if (d < best) { best = d; id = lim.edgeIds[k]; }
      }
    }
    if (best < 1e300 && std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
  } while (e.MoreInput(ts));
  e.SkipEnter(ts);
  return ids;
}

bool SetSubDCreases(Engine& e, TokenStream& ts, const char* cmd, ON_SubDEdgeTag tag) {
  auto sel = e.RequireSelection(cmd, "seleziona il SubD, poi Invio");
  if (sel.empty()) return false;
  const auto* s = ON_SubD::Cast(Geo(e, sel.front()));
  if (!s) return e.Fail("serve un SubD");
  auto ids = PickSubDEdges(e, ts, *s);
  if (ids.empty()) return e.NeedsInput() ? false : e.Fail("nessuno spigolo");
  auto* ns = new ON_SubD(*s);
  std::vector<ON_COMPONENT_INDEX> ci;
  for (unsigned id : ids) ci.push_back(ON_COMPONENT_INDEX(ON_COMPONENT_INDEX::subd_edge, (int)id));
  const unsigned changed = ns->SetEdgeTags(ci.data(), ci.size(), tag);
  ns->UpdateAllTagsAndSectorCoefficients(false);
  ns->ClearEvaluationCache();
  e.ReplaceObject(sel.front(), ns);
  e.Out() << "  Spigoli modificati: " << changed << "\n";
  return true;
}

bool CmdCrease(Engine& e, TokenStream& ts) { return SetSubDCreases(e, ts, "Crease", ON_SubDEdgeTag::Crease); }
bool CmdUncrease(Engine& e, TokenStream& ts) { return SetSubDCreases(e, ts, "Uncrease", ON_SubDEdgeTag::Smooth); }

// SubDDisplayToggle: SubD lisci o gabbia piatta (tasto Tab in Rhino).
bool CmdSubDDisplayToggle(Engine& e, TokenStream&) {
  e.subdFlat = !e.subdFlat;
  e.Out() << (e.subdFlat ? "  SubD: visualizzazione piatta\n" : "  SubD: visualizzazione liscia\n");
  return true;
}

// SelBadObjects: seleziona gli oggetti non validi.
bool CmdSelBadObjects(Engine& e, TokenStream&) {
  int n = 0;
  for (const auto& id : e.AllObjectIds(false))
    if (const ON_Geometry* g = Geo(e, id); g && !g->IsValid()) { e.Select(id); n++; }
  e.Out() << (n ? "  Oggetti non validi selezionati: " : "  Nessun oggetto non valido") << (n ? std::to_string(n) : "") << "\n";
  return true;
}

// ConicCurve: inizio, fine, vertice delle tangenti, rho (0 < rho < 1; 0.5 = parabola).
bool CmdConicCurve(Engine& e, TokenStream& ts) {
  auto a = e.ReadPoint(ts, "inizio della conica");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "fine della conica");
  if (!b) return false;
  auto v = e.ReadPoint(ts, "vertice (incrocio delle tangenti)");
  if (!v) return false;
  auto rho = e.ReadNumber(ts, "rho");
  if (!rho) return false;
  if (*rho <= 0 || *rho >= 1) return e.Fail("rho deve essere tra 0 e 1");
  ON_NurbsCurve* c = Conic(*a, *v, *rho / (1 - *rho), *b);
  if (!c) return e.Fail("conica non valida");
  return Created(e, e.AddObject(c), *rho < 0.5 ? "conica (ellisse)" : (*rho > 0.5 ? "conica (iperbole)" : "conica (parabola)"));
}

// ConvertToBezier: curve convertite in tratti di Bezier (policurva).
bool CmdConvertToBezier(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("ConvertToBezier", "seleziona le curve, poi Invio");
  if (sel.empty()) return false;
  int n = 0;
  for (const auto& id : sel) {
    const auto* c = ON_Curve::Cast(Geo(e, id));
    ON_NurbsCurve nc;
    if (!c || !c->GetNurbForm(nc)) continue;
    auto* pc = new ON_PolyCurve();
    for (int i = 0; i <= nc.CVCount() - nc.Order(); i++) {
      ON_BezierCurve bz;
      if (!nc.ConvertSpanToBezier(i, bz)) continue;
      auto* span = new ON_NurbsCurve();
      bz.GetNurbForm(*span);
      span->SetDomain(nc.m_knot[i + nc.Order() - 2], nc.m_knot[i + nc.Order() - 1]);
      pc->Append(span);
    }
    if (pc->Count() == 0 || !pc->IsValid()) { delete pc; continue; }
    e.ReplaceObject(id, pc);
    n++;
  }
  if (!n) return e.Fail("nessuna curva convertita");
  e.Out() << "  Convertite " << n << " curve in tratti di Bezier\n";
  return true;
}

}  // namespace

void RegisterExtra2Commands(Engine& e) {
  auto reg = [&](const char* n, const char* cat, const char* d, CommandFn f, bool mod = true) {
    CommandInfo i{n, cat, d, f};
    i.modifiesModel = mod;
    e.Register(i);
  };
  reg("Lines", "Curve", "Segmenti di linea separati", CmdLines);
  reg("PolylineThroughPoints", "Curve", "Polilinea per i punti selezionati", CmdPolylineThroughPoints);
  reg("CurveThroughPt", "Curve", "Curva interpolata per i punti selezionati", CmdCurveThroughPt);
  reg("Bisector", "Curve", "Bisettrice di un angolo", CmdBisector);
  reg("AngleLine", "Curve", "Linea ad angolo rispetto a una linea di riferimento", CmdAngleLine);
  reg("RoundedRectangle", "Curve", "Rettangolo con angoli arrotondati", CmdRoundedRectangle);
  reg("Parabola", "Curve", "Parabola (vertice, fuoco, semiapertura)", CmdParabola);
  reg("Hyperbola", "Curve", "Iperbole (centro, vertice, b, semiapertura)", CmdHyperbola);
  reg("Smooth", "Modifica", "Leviga le curve (Factor=)", CmdSmooth);
  reg("InsertKnot", "Modifica", "Inserisce nodi nella curva", CmdInsertKnot);
  reg("CreateUVCrv", "Curve", "Curve di taglio nello spazio parametrico", CmdCreateUVCrv);
  reg("Box3Pt", "Solidi", "Parallelepipedo da un lato, larghezza e altezza", CmdBox3Pt);
  reg("Sphere3Pt", "Solidi", "Sfera da tre punti", CmdSphere3Pt);
  reg("SubDSphere", "SubD", "Sfera SubD", CmdSubDSphere);
  reg("SubDPlane", "SubD", "Piano SubD", CmdSubDPlane);
  reg("SubDCylinder", "SubD", "Cilindro SubD (aperto)", CmdSubDCylinder);
  reg("SubDTorus", "SubD", "Toro SubD", CmdSubDTorus);
  reg("SubDFromMesh", "SubD", "SubD dalla gabbia di una mesh", CmdSubDFromMesh);
  reg("Weld", "Mesh", "Salda i vertici coincidenti delle mesh", CmdWeld);
  reg("MeshPolyline", "Mesh", "Mesh da polilinea chiusa", CmdMeshPolyline);
  reg("MeshToNURB", "Mesh", "Converte le mesh in polisuperfici", CmdMeshToNURB);
  reg("Shear", "Trasformazioni", "Scorrimento (origine, riferimento, angolo)", CmdShear);
  reg("SetPt", "Trasformazioni", "Imposta X, Y o Z degli oggetti", CmdSetPt);
  reg("Align", "Trasformazioni", "Allinea gli oggetti", CmdAlign);
  reg("SelColor", "Selezione", "Seleziona per colore", CmdSelColor, false);
  reg("SelSmall", "Selezione", "Seleziona gli oggetti piccoli", CmdSelSmall, false);
  reg("MatchProperties", "Proprieta'", "Copia le proprieta' da un oggetto", CmdMatchProperties);
  reg("SetUserText", "Proprieta'", "Imposta il testo utente", CmdSetUserText);
  reg("GetUserText", "Proprieta'", "Mostra il testo utente", CmdGetUserText, false);
  reg("Purge", "File", "Elimina livelli e blocchi non usati", CmdPurge);
  reg("Import", "File", "Importa gli oggetti di un file .3dm", CmdImport);
  reg("Export", "File", "Esporta gli oggetti selezionati in un file .3dm (Rhino 8)", CmdExport, false);
  reg("IncrementalSave", "File", "Salva con numero progressivo", CmdIncrementalSave, false);
  reg("SelPrev", "Selezione", "Riseleziona la selezione precedente", CmdSelPrev, false);
  reg("ExtractWireframe", "Curve", "Estrae spigoli e isocurve come curve", CmdExtractWireframe);
  reg("ExtractIsocurve", "Curve", "Estrae l'isocurva per un punto (Direction=U, V, Both)", CmdExtractIsocurve);
  reg("Crease", "SubD", "Spigoli SubD vivi", CmdCrease);
  reg("Uncrease", "SubD", "Spigoli SubD lisci", CmdUncrease);
  reg("SubDDisplayToggle", "SubD", "SubD lisci o a gabbia piatta", CmdSubDDisplayToggle, false);
  reg("SelBadObjects", "Selezione", "Seleziona gli oggetti non validi", CmdSelBadObjects, false);
  reg("ConicCurve", "Curve", "Conica (inizio, fine, vertice, rho)", CmdConicCurve);
  reg("ConvertToBezier", "Modifica", "Converte le curve in tratti di Bezier", CmdConvertToBezier);
  reg("PointsOn", "Modifica", "Mostra i punti di controllo (F10)", CmdPointsOn, false);
  reg("PointsOff", "Modifica", "Nasconde i punti di controllo (F11)", CmdPointsOff, false);
  reg("GripMove", "Interno", "Sposta un punto di controllo (trascinamento nella vista)", CmdGripMove);
}

}  // namespace cad
