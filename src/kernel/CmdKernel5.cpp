// Comandi di Rhino 8 con OpenCASCADE, gruppo 5: FilletSrf, ChamferSrf, BlendSrf,
// OffsetCrvOnSrf, ExtendCrvOnSrf, RebuildCrvNonUniform, UnrollSrf, Smash, Silhouette.
#include "commands/Engine.h"
#include "kernel/OcctConvert.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <Bnd_Box.hxx>
#include <gp_Pln.hxx>
#include <GeomLib_IsPlanarSurface.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <GeomAPI_IntSS.hxx>
#include <GeomAPI_PointsToBSpline.hxx>
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <Geom_OffsetSurface.hxx>
#include <Geom_Surface.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Wire.hxx>
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace cad {
double CurveLengthOf(const ON_Curve& c);
double ParamAtLengthOf(const ON_Curve& c, double s);
ON_Brep* SectionsSurface(const std::vector<ON_NurbsCurve>& secs, const std::vector<double>& v, bool periodic, double tol);

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

gp_Pnt G(const ON_3dPoint& p) { return gp_Pnt(p.x, p.y, p.z); }
ON_3dPoint O(const gp_Pnt& p) { return ON_3dPoint(p.X(), p.Y(), p.Z()); }
ON_3dVector O(const gp_Vec& v) { return ON_3dVector(v.X(), v.Y(), v.Z()); }

bool IsSurfaceLike(const ON_Geometry* g) {
  return g && (ON_Brep::Cast(g) || ON_Extrusion::Cast(g) || ON_Surface::Cast(g));
}

struct Picked {
  ON_UUID id;
  TopoDS_Shape shape;
  TopoDS_Face face;
};

// Faccia del modello piu' vicina al punto.
bool PickFace(Engine& e, const ON_3dPoint& p, Picked& out, const ON_UUID* exclude = nullptr) {
  double best = 1e300;
  const TopoDS_Shape v = BRepBuilderAPI_MakeVertex(G(p)).Vertex();
  for (const auto& oid : e.AllObjectIds(false)) {
    if (exclude && *exclude == oid) continue;
    const ON_Geometry* g = Geo(e, oid);
    if (!IsSurfaceLike(g)) continue;
    try {
      TopoDS_Shape s = occt::GeometryToShape(*g, e.Tolerance());
      for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next()) {
        BRepExtrema_DistShapeShape d(v, ex.Current());
        if (d.IsDone() && d.Value() < best) { best = d.Value(); out = {oid, s, TopoDS::Face(ex.Current())}; }
      }
    } catch (...) {
    }
  }
  return !out.face.IsNull();
}

// Superficie di una faccia con il suo verso e le funzioni di servizio.
struct FaceSurf {
  TopoDS_Face f;
  Handle(Geom_Surface) s;
  double tol;
  explicit FaceSurf(const TopoDS_Face& face, double t) : f(face), s(BRep_Tool::Surface(face)), tol(t) {}
  bool Project(const ON_3dPoint& p, double& u, double& v, ON_3dPoint* q = nullptr) const {
    GeomAPI_ProjectPointOnSurf pr(G(p), s);
    if (!pr.IsDone() || pr.NbPoints() == 0) return false;
    pr.LowerDistanceParameters(u, v);
    if (q) *q = O(pr.NearestPoint());
    return true;
  }
  // Normale nel verso della faccia (conta l'orientamento della faccia nel solido).
  ON_3dVector Normal(double u, double v) const {
    BRepGProp_Face gf(f);
    gp_Pnt q;
    gp_Vec n;
    gf.Normal(u, v, q, n);
    ON_3dVector N = O(n);
    N.Unitize();
    return N;
  }
  // +1 se la normale geometrica della superficie concorda con quella della faccia.
  double GeomSign(double u, double v) const {
    gp_Pnt q;
    gp_Vec du, dv;
    s->D1(u, v, q, du, dv);
    return O(du.Crossed(dv)) * Normal(u, v) >= 0 ? 1.0 : -1.0;
  }
  bool Inside(double u, double v, double t = -1) const {
    BRepClass_FaceClassifier cl(f, gp_Pnt2d(u, v), t > 0 ? t : tol);
    return cl.State() == TopAbs_IN || cl.State() == TopAbs_ON;
  }
};

// Campiona una curva di intersezione nel tratto in cui la funzione ok() vale,
// vicino al punto di riferimento. Restituisce i parametri dei campioni.
std::vector<double> UsefulRun(const Handle(Geom_Curve)& c, const ON_3dPoint& nearPt, double span,
                              const std::function<bool(double)>& ok, int samples, bool& closedOut) {
  double a = c->FirstParameter(), b = c->LastParameter();
  closedOut = false;
  if (Precision::IsInfinite(a) || Precision::IsInfinite(b)) {
    GeomAPI_ProjectPointOnCurve pr(G(nearPt), c);
    const double t0 = pr.NbPoints() ? pr.LowerDistanceParameter() : 0.0;
    a = t0 - span;
    b = t0 + span;
  }
  const int N = 400;
  std::vector<double> t(N + 1);
  std::vector<bool> good(N + 1);
  for (int i = 0; i <= N; i++) { t[i] = a + (b - a) * i / N; good[i] = ok(t[i]); }
  if (std::all_of(good.begin(), good.end(), [](bool g) { return g; }) && c->Value(a).Distance(c->Value(b)) < 1e-7) {
    closedOut = true;
    std::vector<double> r;
    for (int i = 0; i < samples; i++) r.push_back(a + (b - a) * i / samples);
    return r;
  }
  // tratto valido piu' lungo
  int bs = -1, bl = 0;
  for (int i = 0; i <= N;) {
    if (!good[i]) { i++; continue; }
    int j = i;
    while (j <= N && good[j]) j++;
    if (j - i > bl) { bl = j - i; bs = i; }
    i = j;
  }
  if (bs < 0 || bl < 2) return {};
  // estremi raffinati per bisezione
  auto refine = [&](double in, double out) {
    for (int k = 0; k < 40; k++) { const double m = 0.5 * (in + out); if (ok(m)) in = m; else out = m; }
    return in;
  };
  double ta = t[bs], tb = t[bs + bl - 1];
  if (bs > 0) ta = refine(ta, t[bs - 1]);
  if (bs + bl <= N) tb = refine(tb, t[bs + bl]);
  std::vector<double> r;
  for (int i = 0; i < samples; i++) r.push_back(ta + (tb - ta) * i / (samples - 1));
  return r;
}

// Arco razionale quadratico (un solo tratto) da q1 a q2 con centro c.
ON_NurbsCurve ArcSection(const ON_3dPoint& c, const ON_3dPoint& q1, const ON_3dPoint& q2) {
  ON_3dVector a = q1 - c, b = q2 - c;
  const double r = 0.5 * (a.Length() + b.Length());
  a.Unitize();
  b.Unitize();
  ON_3dVector m = a + b;
  ON_NurbsCurve nc(3, true, 3, 3);
  nc.m_knot[0] = nc.m_knot[1] = 0;
  nc.m_knot[2] = nc.m_knot[3] = 1;
  const double cosh = std::sqrt(std::max(0.0, 0.5 * (1 + a * b)));
  ON_3dPoint apex = 0.5 * (q1 + q2);
  if (m.Unitize() && cosh > 1e-6) apex = c + m * (r / cosh);
  nc.SetCV(0, ON_4dPoint(q1.x, q1.y, q1.z, 1));
  nc.SetCV(1, ON_4dPoint(apex.x * cosh, apex.y * cosh, apex.z * cosh, cosh));
  nc.SetCV(2, ON_4dPoint(q2.x, q2.y, q2.z, 1));
  return nc;
}

std::vector<double> ChordParams(const std::vector<ON_3dPoint>& p, bool periodic) {
  std::vector<double> v{0};
  for (size_t i = 1; i < p.size(); i++) v.push_back(v.back() + std::max(1e-9, p[i].DistanceTo(p[i - 1])));
  if (periodic) v.push_back(v.back() + std::max(1e-9, p.front().DistanceTo(p.back())));
  for (auto& x : v) x /= v.back();
  return v;
}

double ShapeSpan(const TopoDS_Face& a, const TopoDS_Face& b) {
  BRepExtrema_DistShapeShape d(a, b);
  double umin, umax, vmin, vmax;
  double span = 0;
  for (const TopoDS_Face* f : {&a, &b}) {
    BRepTools::UVBounds(*f, umin, umax, vmin, vmax);
    Handle(Geom_Surface) s = BRep_Tool::Surface(*f);
    span = std::max(span, s->Value(umin, vmin).Distance(s->Value(umax, vmax)));
  }
  return 2 * span + 1;
}

// FilletSrf / ChamferSrf: superficie di raccordo (palla che rotola) o di smusso tra due superfici.
// variable=true (VariableFilletSrf): raggio che varia linearmente da Radius= a EndRadius=.
bool SrfCorner(Engine& e, TokenStream& ts, bool fillet, bool variable = false) {
  auto opts = e.ReadOptions(ts);
  const char* key = fillet ? "radius" : "distance";
  double r = OptNum(opts, key, 1.0);
  auto p1 = e.ReadPick(ts, "prima superficie");
  if (!p1) return false;
  opts.merge(e.ReadOptions(ts));
  r = OptNum(opts, key, r);
  auto p2 = e.ReadPick(ts, "seconda superficie");
  if (!p2) return false;
  if (r <= 0) return e.Fail("valore non valido");
  Picked a, b;
  if (!PickFace(e, *p1, a)) return e.Fail("nessuna superficie");
  if (!PickFace(e, *p2, b)) return e.Fail("nessuna seconda superficie");
  if (a.face.IsSame(b.face)) return e.Fail("scegli due superfici diverse");
  const double tol = e.Tolerance();
  FaceSurf A(a.face, tol), B(b.face, tol);
  double u1, v1, u2, v2;
  if (!A.Project(*p1, u1, v1) || !B.Project(*p2, u2, v2)) return e.Fail("proiezione non riuscita");
  const ON_3dVector n1 = A.Normal(u1, v1), n2 = B.Normal(u2, v2);
  // il raccordo sta dalla parte in cui ogni superficie "vede" il punto scelto sull'altra
  const double s1 = n1 * (*p2 - *p1) >= 0 ? 1 : -1, s2 = n2 * (*p1 - *p2) >= 0 ? 1 : -1;
  Handle(Geom_Surface) S1 = A.s, S2 = B.s;
  if (fillet) {
    S1 = new Geom_OffsetSurface(A.s, r * s1 * A.GeomSign(u1, v1));
    S2 = new Geom_OffsetSurface(B.s, r * s2 * B.GeomSign(u2, v2));
  }
  GeomAPI_IntSS inter(S1, S2, tol * 0.1);
  if (!inter.IsDone() || inter.NbLines() == 0) return e.Fail(fillet ? "raggio troppo grande o superfici che non si incontrano" : "le superfici non si intersecano");
  const ON_3dPoint mid = 0.5 * (*p1 + *p2);
  // curva guida piu' vicina ai punti scelti
  Handle(Geom_Curve) spine;
  double bd = 1e300;
  for (int i = 1; i <= inter.NbLines(); i++) {
    GeomAPI_ProjectPointOnCurve pr(G(mid), inter.Line(i));
    if (pr.NbPoints() && pr.LowerDistance() < bd) { bd = pr.LowerDistance(); spine = inter.Line(i); }
  }
  if (spine.IsNull()) return e.Fail("nessuna curva di intersezione");
  // punti di contatto sulle due superfici per un punto della guida
  auto contacts = [&](double t, ON_3dPoint& c, ON_3dPoint& q1, ON_3dPoint& q2, double* uv = nullptr) {
    c = O(spine->Value(t));
    double ua, va, ub, vb;
    if (fillet) {
      if (!A.Project(c, ua, va, &q1) || !B.Project(c, ub, vb, &q2)) return false;
    } else {
      gp_Pnt pp;
      gp_Vec d1;
      spine->D1(t, pp, d1);
      ON_3dVector T = O(d1);
      T.Unitize();
      double a0, a1, b0, b1;
      if (!A.Project(c, a0, a1) || !B.Project(c, b0, b1)) return false;
      // direzione sulla superficie, perpendicolare all'intersezione, verso il punto scelto
      ON_3dVector w1 = ON_CrossProduct(A.Normal(a0, a1), T), w2 = ON_CrossProduct(B.Normal(b0, b1), T);
      w1.Unitize();
      w2.Unitize();
      if (w1 * (*p1 - c) < 0) w1 = -w1;
      if (w2 * (*p2 - c) < 0) w2 = -w2;
      if (!A.Project(c + w1 * r, ua, va, &q1) || !B.Project(c + w2 * r, ub, vb, &q2)) return false;
    }
    if (uv) { uv[0] = ua; uv[1] = va; uv[2] = ub; uv[3] = vb; }
    return true;
  };
  auto ok = [&](double t) {
    ON_3dPoint c, q1, q2;
    double uv[4];
    return contacts(t, c, q1, q2, uv) && A.Inside(uv[0], uv[1]) && B.Inside(uv[2], uv[3]);
  };
  bool closed = false;
  const auto ts_ = UsefulRun(spine, mid, ShapeSpan(a.face, b.face), ok, 25, closed);
  if (ts_.size() < 2) return e.Fail("il raccordo non sta sulle superfici");
  std::vector<ON_NurbsCurve> secs;
  std::vector<ON_3dPoint> centers;
  const double rEnd = variable ? OptNum(opts, "endradius", r) : r;
  for (size_t ti = 0; ti < ts_.size(); ti++) {
    const double t = ts_[ti];
    ON_3dPoint c, q1, q2;
    if (!contacts(t, c, q1, q2)) continue;
    if (fillet && variable && std::fabs(rEnd - r) > tol) {
      // centro della palla di raggio rt: si parte da quello di raggio r e si corregge in modo che
      // disti rt da entrambe le superfici (2 equazioni nelle direzioni delle due normali)
      const double rt = r + (rEnd - r) * double(ti) / double(ts_.size() - 1);
      for (int it = 0; it < 40; it++) {
        double ua, va, ub, vb;
        if (!A.Project(c, ua, va, &q1) || !B.Project(c, ub, vb, &q2)) break;
        ON_3dVector n1 = c - q1, n2 = c - q2;
        const double d1 = n1.Length(), d2 = n2.Length();
        if (d1 < 1e-12 || d2 < 1e-12) break;
        n1 = n1 / d1;
        n2 = n2 / d2;
        const double k = n1 * n2, det = 1 - k * k;
        const double f1 = rt - d1, f2 = rt - d2;
        if (std::fabs(f1) + std::fabs(f2) < tol * 1e-3) break;
        double a, b;
        if (det < 1e-6) { a = 0.5 * (f1 + f2); b = 0; }
        else { a = (f1 - k * f2) / det; b = (f2 - k * f1) / det; }
        c = c + n1 * a + n2 * b;
      }
      double ua, va, ub, vb;
      if (!A.Project(c, ua, va, &q1) || !B.Project(c, ub, vb, &q2)) continue;
    }
    if (fillet) secs.push_back(ArcSection(c, q1, q2));
    else {
      ON_NurbsCurve l(3, false, 2, 2);
      l.m_knot[0] = 0;
      l.m_knot[1] = 1;
      l.SetCV(0, q1);
      l.SetCV(1, q2);
      secs.push_back(l);
    }
    centers.push_back(c);
  }
  if (secs.size() < 2) return e.Fail("sezioni insufficienti");
  ON_Brep* br = nullptr;
  try { br = SectionsSurface(secs, ChordParams(centers, closed), closed, tol); } catch (...) {}
  if (!br) return e.Fail("superficie non creata");
  e.AddObject(br);
  e.Out() << "  Creata superficie di " << (fillet ? "raccordo" : "smusso") << "\n";
  return true;
}
bool CmdFilletSrf(Engine& e, TokenStream& ts) { return SrfCorner(e, ts, true); }
bool CmdChamferSrf(Engine& e, TokenStream& ts) { return SrfCorner(e, ts, false); }
bool CmdVariableFilletSrf(Engine& e, TokenStream& ts) { return SrfCorner(e, ts, true, true); }

// Spigolo del modello (di una superficie) piu' vicino al punto, con la sua faccia.
bool PickEdge(Engine& e, const ON_3dPoint& p, TopoDS_Edge& edge, TopoDS_Face& face) {
  double best = 1e300;
  const TopoDS_Shape v = BRepBuilderAPI_MakeVertex(G(p)).Vertex();
  for (const auto& oid : e.AllObjectIds(false)) {
    const ON_Geometry* g = Geo(e, oid);
    if (!IsSurfaceLike(g)) continue;
    try {
      TopoDS_Shape s = occt::GeometryToShape(*g, e.Tolerance());
      for (TopExp_Explorer fx(s, TopAbs_FACE); fx.More(); fx.Next())
        for (TopExp_Explorer ex(fx.Current(), TopAbs_EDGE); ex.More(); ex.Next()) {
          if (BRep_Tool::Degenerated(TopoDS::Edge(ex.Current()))) continue;
          BRepExtrema_DistShapeShape d(v, ex.Current());
          if (d.IsDone() && d.Value() < best - 1e-12) { best = d.Value(); edge = TopoDS::Edge(ex.Current()); face = TopoDS::Face(fx.Current()); }
        }
    } catch (...) {
    }
  }
  return !edge.IsNull();
}

// BlendSrf: superficie di raccordo con tangenza (G1) tra due spigoli di superfici.
bool CmdBlendSrf(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto p1 = e.ReadPick(ts, "primo spigolo");
  if (!p1) return false;
  auto p2 = e.ReadPick(ts, "secondo spigolo");
  if (!p2) return false;
  opts.merge(e.ReadOptions(ts));
  const double bulge = OptNum(opts, "bulge", 1.0);
  TopoDS_Edge E[2];
  TopoDS_Face F[2];
  if (!PickEdge(e, *p1, E[0], F[0]) || !PickEdge(e, *p2, E[1], F[1])) return e.Fail("spigoli non trovati");
  if (E[0].IsSame(E[1])) return e.Fail("scegli due spigoli diversi");
  const int N = 25;
  const double tol = e.Tolerance();
  std::vector<ON_3dPoint> P[2];
  std::vector<ON_3dVector> D[2];
  for (int k = 0; k < 2; k++) {
    BRepAdaptor_Curve ac(E[k]);
    FaceSurf fs(F[k], tol);
    const double a = ac.FirstParameter(), b = ac.LastParameter();
    for (int i = 0; i < N; i++) {
      const double t = a + (b - a) * i / (N - 1);
      gp_Pnt q;
      gp_Vec d1;
      ac.D1(t, q, d1);
      const ON_3dPoint Pk = O(q);
      double u, v;
      fs.Project(Pk, u, v);
      ON_3dVector T = O(d1);
      T.Unitize();
      ON_3dVector d = ON_CrossProduct(T, fs.Normal(u, v));
      d.Unitize();
      // verso l'esterno della superficie: il punto un po' piu' in la' deve essere fuori
      const double step = 1e-3 * std::max(1.0, ac.Value(a).Distance(ac.Value(b)));
      double uu, vv;
      if (fs.Project(Pk - d * step, uu, vv) && !fs.Inside(uu, vv)) d = -d;
      P[k].push_back(Pk);
      D[k].push_back(d);
    }
  }
  // stessi versi: si gira il secondo spigolo se conviene
  if (P[0].front().DistanceTo(P[1].back()) + P[0].back().DistanceTo(P[1].front()) <
      P[0].front().DistanceTo(P[1].front()) + P[0].back().DistanceTo(P[1].back())) {
    std::reverse(P[1].begin(), P[1].end());
    std::reverse(D[1].begin(), D[1].end());
  }
  std::vector<ON_NurbsCurve> secs;
  std::vector<ON_3dPoint> mids;
  for (int i = 0; i < N; i++) {
    const double k = bulge * P[0][i].DistanceTo(P[1][i]) / 3.0;
    ON_NurbsCurve c(3, false, 4, 4);
    c.MakeClampedUniformKnotVector(1.0);
    c.SetCV(0, P[0][i]);
    c.SetCV(1, P[0][i] + D[0][i] * k);
    c.SetCV(2, P[1][i] + D[1][i] * k);
    c.SetCV(3, P[1][i]);
    secs.push_back(c);
    mids.push_back(0.5 * (P[0][i] + P[1][i]));
  }
  ON_Brep* br = nullptr;
  try { br = SectionsSurface(secs, ChordParams(mids, false), false, tol); } catch (...) {}
  if (!br) return e.Fail("superficie non creata");
  e.AddObject(br);
  e.Out() << "  Creata superficie di raccordo (tangenza)\n";
  return true;
}

// Curva per punti (interpolata, cubica).
ON_NurbsCurve* InterpCurve(const std::vector<ON_3dPoint>& pts, double tol) {
  std::vector<ON_3dPoint> q;
  for (const auto& p : pts) if (q.empty() || q.back().DistanceTo(p) > tol) q.push_back(p);
  if (q.size() < 2) return nullptr;
  Handle(TColgp_HArray1OfPnt) arr = new TColgp_HArray1OfPnt(1, (int)q.size());
  for (size_t i = 0; i < q.size(); i++) arr->SetValue(int(i) + 1, G(q[i]));
  GeomAPI_Interpolate in(arr, Standard_False, 1e-9);
  in.Perform();
  if (!in.IsDone()) return nullptr;
  return occt::FromOcct(in.Curve());
}

bool PickCurve(Engine& e, const ON_3dPoint& p, ON_UUID& id, double& t) {
  double best = 1e300;
  for (const auto& oid : e.AllObjectIds(false)) {
    const auto* c = ON_Curve::Cast(Geo(e, oid));
    if (!c) continue;
    const double tt = ClosestParam(*c, p);
    const double d = c->PointAt(tt).DistanceTo(p);
    if (d < best) { best = d; id = oid; t = tt; }
  }
  return best < 1e299;
}

// OffsetCrvOnSrf: curva sulla superficie a distanza costante (misurata sulla superficie).
bool CmdOffsetCrvOnSrf(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto pc = e.ReadPick(ts, "curva sulla superficie");
  if (!pc) return false;
  auto ps = e.ReadPick(ts, "superficie");
  if (!ps) return false;
  opts.merge(e.ReadOptions(ts));
  double d = OptNum(opts, "distance", 1.0);
  auto side = e.ReadPoint(ts, "lato dell'offset");
  if (!side) return false;
  ON_UUID cid;
  double t0;
  if (!PickCurve(e, *pc, cid, t0)) return e.Fail("nessuna curva");
  const auto* c = ON_Curve::Cast(Geo(e, cid));
  Picked sf;
  if (!PickFace(e, *ps, sf)) return e.Fail("nessuna superficie");
  FaceSurf fs(sf.face, e.Tolerance());
  const int N = 80;
  // verso: quello dalla parte del punto indicato, nel punto della curva piu' vicino
  const double ts0 = ClosestParam(*c, *side);
  double u, v;
  fs.Project(c->PointAt(ts0), u, v);
  ON_3dVector w0 = ON_CrossProduct(fs.Normal(u, v), c->TangentAt(ts0));
  const double sgn = w0 * (*side - c->PointAt(ts0)) >= 0 ? 1 : -1;
  std::vector<ON_3dPoint> out;
  for (int i = 0; i <= N; i++) {
    const double t = c->Domain().ParameterAt(double(i) / N);
    ON_3dPoint P = c->PointAt(t);
    const ON_3dVector T = c->TangentAt(t);
    const int steps = 10;
    for (int k = 0; k < steps; k++) {  // passi piccoli per seguire la superficie
      if (!fs.Project(P, u, v, &P)) break;
      ON_3dVector w = ON_CrossProduct(fs.Normal(u, v), T);
      w = w - fs.Normal(u, v) * (w * fs.Normal(u, v));
      if (!w.Unitize()) break;
      P = P + w * (sgn * d / steps);
    }
    fs.Project(P, u, v, &P);
    out.push_back(P);
  }
  ON_NurbsCurve* r = InterpCurve(out, e.Tolerance());
  if (!r) return e.Fail("curva non creata");
  e.AddObject(r);
  e.Out() << "  Creata curva a distanza " << d << " sulla superficie\n";
  return true;
}

// ExtendCrvOnSrf: allunga la curva, dall'estremo piu' vicino al punto, fino al bordo della superficie.
bool CmdExtendCrvOnSrf(Engine& e, TokenStream& ts) {
  auto pc = e.ReadPick(ts, "curva vicino all'estremo da allungare");
  if (!pc) return false;
  auto ps = e.ReadPick(ts, "superficie");
  if (!ps) return false;
  ON_UUID cid;
  double tp;
  if (!PickCurve(e, *pc, cid, tp)) return e.Fail("nessuna curva");
  const auto* c = ON_Curve::Cast(Geo(e, cid));
  if (c->IsClosed()) return e.Fail("la curva e' chiusa");
  Picked sf;
  if (!PickFace(e, *ps, sf)) return e.Fail("nessuna superficie");
  FaceSurf fs(sf.face, e.Tolerance());
  const ON_Interval dom = c->Domain();
  const bool atEnd = std::fabs(tp - dom[1]) < std::fabs(tp - dom[0]);
  const int N = 60;
  std::vector<ON_3dPoint> pts;
  for (int i = 0; i <= N; i++) pts.push_back(c->PointAt(dom.ParameterAt(double(i) / N)));
  if (!atEnd) std::reverse(pts.begin(), pts.end());
  // direzione nel piano (u,v) dagli ultimi due punti
  double ua, va, ub, vb;
  if (!fs.Project(pts[N - 1], ua, va) || !fs.Project(pts[N], ub, vb)) return e.Fail("la curva non sta sulla superficie");
  gp_Vec2d dir(ub - ua, vb - va);
  if (dir.Magnitude() < 1e-15) return e.Fail("direzione non valida");
  double umin, umax, vmin, vmax;
  BRepTools::UVBounds(sf.face, umin, umax, vmin, vmax);
  const double stepLen = std::max(umax - umin, vmax - vmin) / 200;
  dir.Normalize();
  double u = ub, v = vb;
  int added = 0;
  for (int k = 0; k < 400; k++) {
    double nu = u + dir.X() * stepLen, nv = v + dir.Y() * stepLen;
    if (!fs.Inside(nu, nv)) {  // bordo: bisezione
      double lo = 0, hi = 1;
      for (int j = 0; j < 40; j++) { const double m = 0.5 * (lo + hi); if (fs.Inside(u + dir.X() * stepLen * m, v + dir.Y() * stepLen * m, 1e-9)) lo = m; else hi = m; }
      nu = u + dir.X() * stepLen * lo;
      nv = v + dir.Y() * stepLen * lo;
      ON_3dPoint last = O(fs.s->Value(nu, nv));
      // aggancio esatto al bordo della faccia (indipendente dalla tolleranza del classificatore)
      BRepExtrema_DistShapeShape dd(BRepBuilderAPI_MakeVertex(G(last)).Vertex(), BRepTools::OuterWire(sf.face));
      if (dd.IsDone() && dd.NbSolution() > 0 && dd.Value() < 10 * e.Tolerance()) last = O(dd.PointOnShape2(1));
      pts.push_back(last);
      added++;
      break;
    }
    u = nu;
    v = nv;
    if (k % 4 == 3) { pts.push_back(O(fs.s->Value(u, v))); added++; }
  }
  if (!added) return e.Fail("l'estremo e' gia' sul bordo");
  if (!atEnd) std::reverse(pts.begin(), pts.end());
  ON_NurbsCurve* r = InterpCurve(pts, e.Tolerance());
  if (!r) return e.Fail("curva non creata");
  e.ReplaceObject(cid, r);
  e.Out() << "  Curva allungata fino al bordo\n";
  return true;
}

// RebuildCrvNonUniform: approssimazione con nodi non uniformi entro RequestedTolerance=.
bool CmdRebuildCrvNonUniform(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("RebuildCrvNonUniform", "seleziona le curve, poi Invio");
  if (sel.empty()) return false;
  const double tol = OptNum(opts, "requestedtolerance", e.Tolerance());
  const int maxPts = (int)OptNum(opts, "maxpointcount", 1000);
  int n = 0;
  for (const auto& id : sel) {
    const auto* c = ON_Curve::Cast(Geo(e, id));
    if (!c) continue;
    const int M = 400;
    TColgp_Array1OfPnt arr(1, M + 1);
    for (int i = 0; i <= M; i++) arr.SetValue(i + 1, G(c->PointAt(c->Domain().ParameterAt(double(i) / M))));
    GeomAPI_PointsToBSpline ap(arr, 3, 3, GeomAbs_C2, tol);
    if (!ap.IsDone()) continue;
    ON_NurbsCurve* r = occt::FromOcct(ap.Curve());
    if (!r) continue;
    if (r->CVCount() > maxPts) { delete r; continue; }
    e.ReplaceObject(id, r);
    e.Out() << "  Ricostruita con " << r->CVCount() << " punti di controllo\n";
    n++;
  }
  if (!n) return e.Fail("nessuna curva ricostruita");
  return true;
}

// UnrollSrf / Smash: sviluppa nel piano xy ogni faccia (a strisce di triangoli che
// conservano le lunghezze). Esatta per superfici sviluppabili (piani, cilindri, coni);
// per le altre (Smash) e' un'approssimazione.
bool Unroll(Engine& e, TokenStream& ts, const char* name) {
  auto sel = e.RequireSelection(name, "seleziona le superfici, poi Invio");
  if (sel.empty()) return false;
  (void)ts;
  ON_BoundingBox box;
  for (const auto& id : sel) if (const auto* g = Geo(e, id)) box.Union(g->BoundingBox());
  double xoff = box.IsValid() ? box.m_max.x + 0.2 * box.Diagonal().Length() + 1 : 0;
  const double yoff = box.IsValid() ? box.m_min.y : 0;
  int made = 0;
  for (const auto& id : sel) {
    const ON_Geometry* g = Geo(e, id);
    if (!IsSurfaceLike(g)) continue;
    TopoDS_Shape s;
    try { s = occt::GeometryToShape(*g, e.Tolerance()); } catch (...) { continue; }
    for (TopExp_Explorer fx(s, TopAbs_FACE); fx.More(); fx.Next()) {
      const TopoDS_Face f = TopoDS::Face(fx.Current());
      GeomLib_IsPlanarSurface isPl(BRep_Tool::Surface(f), e.Tolerance());
      if (isPl.IsPlanar()) {
        // faccia piana (anche con tagli e fori): basta portarla sul piano xy
        const gp_Pln pln = isPl.Plan();
        gp_Trsf tr;
        tr.SetTransformation(pln.Position(), gp_Ax3());
        TopoDS_Shape flat = BRepBuilderAPI_Transform(f, tr, Standard_True).Shape();
        Bnd_Box bb;
        BRepBndLib::Add(flat, bb);
        double x0, y0, z0, x1, y1, z1;
        bb.Get(x0, y0, z0, x1, y1, z1);
        gp_Trsf mv;
        mv.SetTranslation(gp_Vec(xoff - x0, yoff - y0, 0));
        flat = BRepBuilderAPI_Transform(flat, mv, Standard_True).Shape();
        if (ON_Brep* br = occt::ShapeToBrep(flat, e.Tolerance())) {
          e.AddObject(br);
          xoff += (x1 - x0) * 1.1 + 1;
          made++;
        }
        continue;
      }
      Handle(Geom_Surface) S = BRep_Tool::Surface(f);
      double umin, umax, vmin, vmax;
      BRepTools::UVBounds(f, umin, umax, vmin, vmax);
      const int NU = 40, NV = 40;
      auto P3 = [&](int i, int j) { return O(S->Value(umin + (umax - umin) * i / NU, vmin + (vmax - vmin) * j / NV)); };
      std::vector<ON_2dPoint> P2((NU + 1) * (NV + 1));
      std::vector<bool> placed(P2.size(), false);
      auto at = [&](int i, int j) { return i * (NV + 1) + j; };
      // trilaterazione: terzo vertice dalle distanze, dalla parte opposta a "away"
      auto third = [&](int a, int b, int c, const ON_2dPoint* away, const ON_3dPoint& A3, const ON_3dPoint& B3, const ON_3dPoint& C3) {
        (void)c;
        const ON_2dPoint A = P2[a], B = P2[b];
        const double d = A.DistanceTo(B), ra = A3.DistanceTo(C3), rb = B3.DistanceTo(C3);
        if (d < 1e-14) return A;
        const double x = (ra * ra - rb * rb + d * d) / (2 * d);
        const double y = std::sqrt(std::max(0.0, ra * ra - x * x));
        const ON_2dVector ex = (B - A) / d;
        const ON_2dVector ey(-ex.y, ex.x);
        ON_2dPoint c1 = A + ex * x + ey * y, c2 = A + ex * x - ey * y;
        if (away) return c1.DistanceTo(*away) > c2.DistanceTo(*away) ? c1 : c2;
        return c1;
      };
      // prima colonna: si sviluppa la striscia i=0..1 riga per riga, poi le colonne successive
      P2[at(0, 0)] = ON_2dPoint(0, 0);
      placed[at(0, 0)] = true;
      P2[at(1, 0)] = ON_2dPoint(P3(0, 0).DistanceTo(P3(1, 0)), 0);
      placed[at(1, 0)] = true;
      for (int i = 0; i < NU; i++) {
        for (int j = 0; j < NV; j++) {
          // quadrilatero (i,j) (i+1,j) (i+1,j+1) (i,j+1): due triangoli
          const int a = at(i, j), b = at(i + 1, j), c = at(i + 1, j + 1), d = at(i, j + 1);
          if (!placed[a] || !placed[b]) continue;
          if (!placed[c]) {
            const ON_2dPoint* away = j > 0 ? &P2[at(i, j - 1)] : nullptr;
            ON_2dPoint q = third(a, b, c, away, P3(i, j), P3(i + 1, j), P3(i + 1, j + 1));
            if (!away) { // primo triangolo: verso +y
              const ON_2dVector ab = P2[b] - P2[a];
              const ON_2dVector aq = q - P2[a];
              if (ab.x * aq.y - ab.y * aq.x < 0) q = P2[a] + (P2[b] - P2[a]) * ((aq * ab) / (ab * ab)) * 2.0 - aq + (P2[a] - P2[a]);
            }
            P2[c] = q;
            placed[c] = true;
          }
          if (!placed[d]) {
            P2[d] = third(a, c, d, &P2[b], P3(i, j), P3(i + 1, j + 1), P3(i, j + 1));
            placed[d] = true;
          }
        }
        // la colonna successiva parte dal suo primo spigolo
        if (i + 2 <= NU && !placed[at(i + 2, 0)]) {
          P2[at(i + 2, 0)] = third(at(i + 1, 0), at(i + 1, 1), at(i + 2, 0), &P2[at(i, 0)], P3(i + 1, 0), P3(i + 1, 1), P3(i + 2, 0));
          placed[at(i + 2, 0)] = true;
        }
      }
      // contorno -> superficie piana
      std::vector<ON_3dPoint> side[4];
      for (int i = 0; i <= NU; i++) side[0].push_back(ON_3dPoint(P2[at(i, 0)].x, P2[at(i, 0)].y, 0));
      for (int j = 0; j <= NV; j++) side[1].push_back(ON_3dPoint(P2[at(NU, j)].x, P2[at(NU, j)].y, 0));
      for (int i = NU; i >= 0; i--) side[2].push_back(ON_3dPoint(P2[at(i, NV)].x, P2[at(i, NV)].y, 0));
      for (int j = NV; j >= 0; j--) side[3].push_back(ON_3dPoint(P2[at(0, j)].x, P2[at(0, j)].y, 0));
      ON_BoundingBox fb;
      for (auto& sd : side) for (auto& p : sd) fb.Set(p, fb.IsValid());
      const ON_3dVector shift(xoff - fb.m_min.x, yoff - fb.m_min.y, 0);
      BRepBuilderAPI_MakeWire mw;
      for (auto& sd : side) {
        for (auto& p : sd) p = p + shift;
        if (sd.front().DistanceTo(sd.back()) < e.Tolerance()) continue;  // lato degenere (apice del cono)
        bool straight = true;
        for (auto& p : sd) {
          ON_Line l(sd.front(), sd.back());
          if (l.DistanceTo(p) > e.Tolerance()) { straight = false; break; }
        }
        if (straight) { mw.Add(BRepBuilderAPI_MakeEdge(G(sd.front()), G(sd.back())).Edge()); continue; }
        std::unique_ptr<ON_NurbsCurve> nc(InterpCurve(sd, e.Tolerance() * 0.01));
        if (!nc) continue;
        mw.Add(BRepBuilderAPI_MakeEdge(occt::ToOcct(*nc)).Edge());
      }
      if (!mw.IsDone()) continue;
      BRepBuilderAPI_MakeFace mf(mw.Wire(), Standard_True);
      if (!mf.IsDone()) continue;
      ON_Brep* br = occt::ShapeToBrep(mf.Face(), e.Tolerance());
      if (!br) continue;
      e.AddObject(br);
      xoff += fb.Diagonal().Length() * 0.1 + (fb.m_max.x - fb.m_min.x) + 1;
      made++;
    }
  }
  if (!made) return e.Fail("nessuna superficie sviluppata");
  e.Out() << "  Sviluppate " << made << " facce nel piano xy\n";
  return true;
}
bool CmdUnrollSrf(Engine& e, TokenStream& ts) { return Unroll(e, ts, "UnrollSrf"); }
bool CmdSmash(Engine& e, TokenStream& ts) { return Unroll(e, ts, "Smash"); }

// Silhouette: contorni apparenti delle superfici visti da View= o Direction=x,y,z
// (verso l'osservatore). Polilinee sulle superfici, dalla mesh di calcolo.
bool CmdSilhouette(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("Silhouette", "seleziona le superfici, poi Invio");
  if (sel.empty()) return false;
  opts.merge(e.ReadOptions(ts));
  ON_3dVector dir(1, -1, 1);
  const std::string view = opts.count("view") ? opts["view"] : "";
  if (view == "top") dir = ON_3dVector(0, 0, 1);
  else if (view == "front") dir = ON_3dVector(0, -1, 0);
  else if (view == "right") dir = ON_3dVector(1, 0, 0);
  if (opts.count("direction")) {
    double x, y, z;
    if (std::sscanf(opts["direction"].c_str(), "%lf,%lf,%lf", &x, &y, &z) == 3) dir = ON_3dVector(x, y, z);
  }
  if (!dir.Unitize()) return e.Fail("direzione non valida");
  int made = 0;
  for (const auto& id : sel) {
    const ON_Geometry* g = Geo(e, id);
    if (!IsSurfaceLike(g)) continue;
    std::unique_ptr<ON_Mesh> m;
    try {
      TopoDS_Shape s = occt::GeometryToShape(*g, e.Tolerance());
      const double diag = g->BoundingBox().Diagonal().Length();
      m.reset(occt::MeshShape(s, diag / 400, 0.05, true));
    } catch (...) {
    }
    if (!m) continue;
    if (!m->HasVertexNormals()) m->ComputeVertexNormals();
    // segmenti dove n.dir cambia segno dentro i triangoli
    struct Seg { ON_3dPoint a, b; };
    std::vector<Seg> segs;
    for (int f = 0; f < m->FaceCount(); f++) {
      const ON_MeshFace& F = m->m_F[f];
      const int nv = F.IsQuad() ? 4 : 3;
      std::vector<ON_3dPoint> hits;
      for (int k = 0; k < nv; k++) {
        const int a = F.vi[k], b = F.vi[(k + 1) % nv];
        const double fa = ON_3dVector(m->m_N[a]) * dir, fb = ON_3dVector(m->m_N[b]) * dir;
        if ((fa < 0) != (fb < 0)) {
          const double t = fa / (fa - fb);
          hits.push_back(ON_3dPoint(m->m_V[a]) + t * (ON_3dPoint(m->m_V[b]) - ON_3dPoint(m->m_V[a])));
        }
      }
      if (hits.size() >= 2) segs.push_back({hits[0], hits[1]});
    }
    // concatena i segmenti in polilinee
    const double tol = std::max(e.Tolerance(), g->BoundingBox().Diagonal().Length() * 1e-7);
    std::vector<bool> used(segs.size(), false);
    for (size_t s0 = 0; s0 < segs.size(); s0++) {
      if (used[s0]) continue;
      used[s0] = true;
      std::vector<ON_3dPoint> pl{segs[s0].a, segs[s0].b};
      for (int pass = 0; pass < 2; pass++) {
        bool grew = true;
        while (grew) {
          grew = false;
          for (size_t k = 0; k < segs.size(); k++) {
            if (used[k]) continue;
            if (segs[k].a.DistanceTo(pl.back()) < tol) { pl.push_back(segs[k].b); used[k] = grew = true; }
            else if (segs[k].b.DistanceTo(pl.back()) < tol) { pl.push_back(segs[k].a); used[k] = grew = true; }
          }
        }
        std::reverse(pl.begin(), pl.end());
      }
      if (pl.size() < 3) continue;
      ON_Polyline poly;
      for (const auto& p : pl) if (poly.Count() == 0 || poly.Last()->DistanceTo(p) > tol) poly.Append(p);
      if (poly.Count() < 2) continue;
      e.AddObject(new ON_PolylineCurve(poly));
      made++;
    }
  }
  if (!made) return e.Fail("nessun contorno apparente in questa direzione");
  e.Out() << "  Creati " << made << " contorni\n";
  return true;
}

}  // namespace

void RegisterKernel5Commands(Engine& e) {
  auto reg = [&](const char* n, const char* cat, const char* d, CommandFn f) {
    CommandInfo i{n, cat, d, f};
    i.modifiesModel = true;
    e.Register(i);
  };
  reg("FilletSrf", "Superfici", "Superficie di raccordo di raggio Radius= tra due superfici", CmdFilletSrf);
  reg("VariableFilletSrf", "Superfici", "Raccordo tra due superfici con raggio variabile da Radius= a EndRadius=", CmdVariableFilletSrf);
  reg("ChamferSrf", "Superfici", "Superficie di smusso a distanza Distance= tra due superfici", CmdChamferSrf);
  reg("BlendSrf", "Superfici", "Superficie di raccordo con tangenza tra due spigoli (Bulge=)", CmdBlendSrf);
  reg("OffsetCrvOnSrf", "Curve", "Curva sulla superficie a distanza costante (Distance=)", CmdOffsetCrvOnSrf);
  reg("ExtendCrvOnSrf", "Curve", "Allunga una curva sulla superficie fino al bordo", CmdExtendCrvOnSrf);
  reg("RebuildCrvNonUniform", "Curve", "Ricostruisce le curve con nodi non uniformi (RequestedTolerance=, MaxPointCount=)", CmdRebuildCrvNonUniform);
  reg("UnrollSrf", "Superfici", "Sviluppa nel piano le superfici sviluppabili", CmdUnrollSrf);
  reg("Smash", "Superfici", "Appiattisce le superfici nel piano (approssimato)", CmdSmash);
  reg("Squish", "Superfici", "Come Smash", CmdSmash);
  reg("Silhouette", "Curve", "Contorni apparenti delle superfici (View=, Direction=)", CmdSilhouette);
}

}  // namespace cad
