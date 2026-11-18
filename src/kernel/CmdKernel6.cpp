// Comandi di Rhino 8 con OpenCASCADE, gruppo 6: Sketch, MatchSrf, DevLoft, QuadRemesh.
#include "commands/Engine.h"
#include "core/SubDLimit.h"
#include "kernel/OcctConvert.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <GeomAPI_PointsToBSpline.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <Geom_Surface.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cad {
ON_Brep* SectionsSurface(const std::vector<ON_NurbsCurve>& secs, const std::vector<double>& v, bool periodic, double tol);
double CurveLengthOf(const ON_Curve& c);
double ParamAtLengthOf(const ON_Curve& c, double s);

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

gp_Pnt G(const ON_3dPoint& p) { return gp_Pnt(p.x, p.y, p.z); }
ON_3dPoint O(const gp_Pnt& p) { return ON_3dPoint(p.X(), p.Y(), p.Z()); }
ON_3dVector O(const gp_Vec& v) { return ON_3dVector(v.X(), v.Y(), v.Z()); }

bool IsSurfaceLike(const ON_Geometry* g) {
  return g && (ON_Brep::Cast(g) || ON_Extrusion::Cast(g) || ON_Surface::Cast(g));
}

// Sketch: curva a mano libera per i punti dati (approssimata e levigata).
bool CmdSketch(Engine& e, TokenStream& ts) {
  std::vector<ON_3dPoint> pts;
  while (true) {
    auto p = e.ReadPoint(ts, pts.size() < 2 ? "punto dello schizzo" : "punto dello schizzo (Invio per finire)");
    if (!p) { if (e.NeedsInput()) return false; break; }
    if (pts.empty() || pts.back().DistanceTo(*p) > e.Tolerance()) pts.push_back(*p);
    if (!e.MoreInput(ts)) break;
  }
  if (pts.size() < 2) return e.Fail("servono almeno 2 punti");
  double L = 0;
  for (size_t i = 1; i < pts.size(); i++) L += pts[i].DistanceTo(pts[i - 1]);
  if (pts.size() == 2) {
    e.AddObject(new ON_LineCurve(pts[0], pts[1]));
    return true;
  }
  TColgp_Array1OfPnt arr(1, (int)pts.size());
  for (size_t i = 0; i < pts.size(); i++) arr.SetValue(int(i) + 1, G(pts[i]));
  GeomAPI_PointsToBSpline ap(arr, 3, 3, GeomAbs_C2, std::max(e.Tolerance(), L * 0.005));
  if (!ap.IsDone()) return e.Fail("curva non creata");
  ON_NurbsCurve* c = occt::FromOcct(ap.Curve());
  if (!c) return e.Fail("curva non creata");
  e.AddObject(c);
  e.Out() << "  Creato schizzo con " << c->CVCount() << " punti di controllo\n";
  return true;
}

// Spigolo del modello piu' vicino al punto, con la sua faccia (esclusa una superficie).
bool PickEdge(Engine& e, const ON_3dPoint& p, TopoDS_Edge& edge, TopoDS_Face& face, const ON_UUID* exclude, ON_UUID* idOut) {
  double best = 1e300;
  const TopoDS_Shape v = BRepBuilderAPI_MakeVertex(G(p)).Vertex();
  for (const auto& oid : e.AllObjectIds(false)) {
    if (exclude && ON_UuidCompare(*exclude, oid) == 0) continue;
    const ON_Geometry* g = Geo(e, oid);
    if (!IsSurfaceLike(g)) continue;
    try {
      TopoDS_Shape s = occt::GeometryToShape(*g, e.Tolerance());
      for (TopExp_Explorer fx(s, TopAbs_FACE); fx.More(); fx.Next())
        for (TopExp_Explorer ex(fx.Current(), TopAbs_EDGE); ex.More(); ex.Next()) {
          if (BRep_Tool::Degenerated(TopoDS::Edge(ex.Current()))) continue;
          BRepExtrema_DistShapeShape d(v, ex.Current());
          if (d.IsDone() && d.Value() < best - 1e-12) {
            best = d.Value();
            edge = TopoDS::Edge(ex.Current());
            face = TopoDS::Face(fx.Current());
            if (idOut) *idOut = oid;
          }
        }
    } catch (...) {
    }
  }
  return !edge.IsNull();
}

// Curva con gli stessi nodi che passa per Q[k] ai parametri t[k] (punti di controllo nuovi).
bool InterpSameKnots(ON_NurbsCurve& nc, const std::vector<double>& t, const std::vector<ON_3dPoint>& Q) {
  const int n = nc.CVCount();
  std::vector<std::vector<double>> A(n, std::vector<double>(n + 3, 0.0));
  ON_NurbsCurve basis(nc);
  for (int j = 0; j < n; j++) {
    for (int k = 0; k < n; k++) basis.SetCV(k, ON_3dPoint(k == j ? 1.0 : 0.0, 0, 0));
    for (int r = 0; r < n; r++) A[r][j] = basis.PointAt(t[r]).x;
  }
  for (int r = 0; r < n; r++) { A[r][n] = Q[r].x; A[r][n + 1] = Q[r].y; A[r][n + 2] = Q[r].z; }
  for (int col = 0; col < n; col++) {
    int piv = col;
    for (int r = col + 1; r < n; r++) if (std::fabs(A[r][col]) > std::fabs(A[piv][col])) piv = r;
    if (std::fabs(A[piv][col]) < 1e-14) return false;
    std::swap(A[piv], A[col]);
    for (int r = 0; r < n; r++) {
      if (r == col) continue;
      const double f = A[r][col] / A[col][col];
      for (int k = col; k < n + 3; k++) A[r][k] -= f * A[col][k];
    }
  }
  for (int j = 0; j < n; j++) nc.SetCV(j, ON_3dPoint(A[j][n] / A[j][j], A[j][n + 1] / A[j][j], A[j][n + 2] / A[j][j]));
  return true;
}

// MatchSrf: porta il bordo di una superficie (NURBS non tagliata) sullo spigolo di un'altra,
// con continuita' di posizione o di tangenza (Continuity=Position/Tangency, default Tangency).
bool CmdMatchSrf(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto p1 = e.ReadPick(ts, "bordo della superficie da modificare");
  if (!p1) return false;
  auto p2 = e.ReadPick(ts, "spigolo a cui raccordarla");
  if (!p2) return false;
  opts.merge(e.ReadOptions(ts));
  const bool tangency = !(opts.count("continuity") && opts["continuity"] == "position");
  // superficie da modificare: la piu' vicina al primo punto
  ON_UUID sid;
  TopoDS_Edge e1;
  TopoDS_Face f1;
  if (!PickEdge(e, *p1, e1, f1, nullptr, &sid)) return e.Fail("nessuna superficie");
  const auto* b = ON_Brep::Cast(Geo(e, sid));
  std::unique_ptr<ON_Brep> owned;
  if (!b) { if (const ON_Geometry* g = Geo(e, sid); g && g->HasBrepForm()) { owned.reset(g->BrepForm()); b = owned.get(); } }
  if (!b || b->m_F.Count() != 1 || b->m_L.Count() != 1) return e.Fail("la superficie da modificare deve essere una superficie singola non tagliata");
  ON_NurbsSurface ns;
  if (!b->m_F[0].GetNurbForm(ns) || ns.IsRational()) return e.Fail("superficie non modificabile (razionale)");
  TopoDS_Edge e2;
  TopoDS_Face f2;
  if (!PickEdge(e, *p2, e2, f2, &sid, nullptr)) return e.Fail("nessuno spigolo di destinazione");
  // lato della superficie piu' vicino al primo punto: 0=u min, 1=u max, 2=v min, 3=v max
  int side = 0;
  double bd = 1e300;
  for (int s = 0; s < 4; s++) {
    const int dir = s < 2 ? 0 : 1;
    const double fixed = ns.Domain(dir)[s % 2];
    const ON_Interval other = ns.Domain(1 - dir);
    for (int k = 0; k <= 50; k++) {
      const double t = other.ParameterAt(k / 50.0);
      const ON_3dPoint q = dir == 0 ? ns.PointAt(fixed, t) : ns.PointAt(t, fixed);
      if (q.DistanceTo(*p1) < bd) { bd = q.DistanceTo(*p1); side = s; }
    }
  }
  const int dir = side < 2 ? 0 : 1;          // direzione in cui il lato e' costante
  const int along = 1 - dir;
  const int n = ns.CVCount(along), m = ns.CVCount(dir);
  const int r0 = side % 2 == 0 ? 0 : m - 1, r1 = side % 2 == 0 ? 1 : m - 2;
  auto cv = [&](int k, int r) { ON_3dPoint q; if (dir == 0) ns.GetCV(r, k, q); else ns.GetCV(k, r, q); return q; };
  auto setcv = [&](int k, int r, const ON_3dPoint& q) { if (dir == 0) ns.SetCV(r, k, q); else ns.SetCV(k, r, q); };
  // curva di bordo (stessi nodi della superficie nella direzione "along")
  ON_NurbsCurve bc(3, false, ns.Order(along), n);
  for (int i = 0; i < bc.KnotCount(); i++) bc.m_knot[i] = ns.m_knot[along][i];
  for (int k = 0; k < n; k++) bc.SetCV(k, cv(k, r0));
  std::vector<double> gt(n);
  if (!bc.GetGrevilleAbcissae(gt.data())) return e.Fail("parametri del bordo non calcolabili");
  // destinazione: spigolo campionato, verso concorde con il bordo
  BRepAdaptor_Curve ac(e2);
  const double a = ac.FirstParameter(), bpar = ac.LastParameter();
  const int S = 400;
  std::vector<ON_3dPoint> tp(S + 1);
  for (int i = 0; i <= S; i++) tp[i] = O(ac.Value(a + (bpar - a) * i / S));
  if (bc.PointAtStart().DistanceTo(tp.front()) + bc.PointAtEnd().DistanceTo(tp.back()) >
      bc.PointAtStart().DistanceTo(tp.back()) + bc.PointAtEnd().DistanceTo(tp.front()))
    std::reverse(tp.begin(), tp.end());
  std::vector<double> cum{0};
  for (int i = 1; i <= S; i++) cum.push_back(cum.back() + tp[i].DistanceTo(tp[i - 1]));
  auto targetAt = [&](double frac) {
    const double s = frac * cum.back();
    int i = int(std::lower_bound(cum.begin(), cum.end(), s) - cum.begin());
    i = std::clamp(i, 1, S);
    const double t = (s - cum[i - 1]) / std::max(1e-15, cum[i] - cum[i - 1]);
    return (1 - t) * tp[i - 1] + t * tp[i];
  };
  const double Lb = CurveLengthOf(bc);
  std::vector<ON_3dPoint> Q(n);
  for (int k = 0; k < n; k++) {
    // posizione relativa (per lunghezza) del punto di Greville sul bordo
    double frac = 0;
    if (k == 0) frac = 0; else if (k == n - 1) frac = 1;
    else { ON_Interval d(bc.Domain()[0], gt[k]); ON_NurbsCurve sub(bc); sub.Trim(d); frac = CurveLengthOf(sub) / Lb; }
    Q[k] = targetAt(frac);
  }
  ON_NurbsCurve nb(bc);
  if (!InterpSameKnots(nb, gt, Q)) return e.Fail("bordo non calcolabile");
  // posizione: la seconda fila si sposta come la prima (la tangente trasversale resta)
  std::vector<ON_3dPoint> oldR0(n), oldR1(n);
  for (int k = 0; k < n; k++) { oldR0[k] = cv(k, r0); oldR1[k] = cv(k, r1); }
  for (int k = 0; k < n; k++) {
    ON_3dPoint q;
    nb.GetCV(k, q);
    setcv(k, r0, q);
    if (m > 2) setcv(k, r1, oldR1[k] + (q - oldR0[k]));
  }
  if (tangency && m > 2) {
    // tangenza: la seconda fila lungo la direzione che prosegue la superficie di destinazione
    Handle(Geom_Surface) S2 = BRep_Tool::Surface(f2);
    BRepGProp_Face gf(f2);
    for (int k = 0; k < n; k++) {
      ON_3dPoint q;
      nb.GetCV(k, q);
      const ON_3dPoint onEdge = nb.PointAt(gt[k]);
      GeomAPI_ProjectPointOnSurf pr(G(onEdge), S2);
      if (!pr.NbPoints()) continue;
      double u, v;
      pr.LowerDistanceParameters(u, v);
      gp_Pnt pp;
      gp_Vec nn;
      gf.Normal(u, v, pp, nn);
      ON_3dVector N = O(nn);
      N.Unitize();
      ON_3dVector T = nb.TangentAt(gt[k]);
      ON_3dVector d = ON_CrossProduct(N, T);
      if (!d.Unitize()) continue;
      // verso: fuori dalla superficie di destinazione (dalla parte della superficie da modificare)
      const ON_3dPoint inside = O(S2->Value(u, v));
      const ON_3dVector toOld = oldR1[k] - oldR0[k];
      if (d * toOld < 0) d = -d;
      (void)inside;
      const double len = std::max(toOld.Length(), 1e-6);
      setcv(k, r1, q + d * len);
    }
  }
  auto* nsurf = new ON_NurbsSurface(ns);
  ON_Brep* nbrep = ON_Brep::New();
  ON_Surface* sp = nsurf;
  if (!nbrep->Create(sp)) { delete nbrep; delete nsurf; return e.Fail("superficie non ricostruita"); }
  e.ReplaceObject(sid, nbrep);
  e.Out() << "  Superficie raccordata allo spigolo (" << (tangency ? "tangenza" : "posizione") << ")\n";
  return true;
}

// DevLoft: superficie rigata sviluppabile tra due curve. Le generatrici collegano punti
// in cui le tangenti delle due curve stanno sullo stesso piano (ricerca con programmazione dinamica).
bool CmdDevLoft(Engine& e, TokenStream& ts) {
  e.ReadOptions(ts);
  auto sel = e.RequireSelection("DevLoft", "seleziona le due curve, poi Invio");
  if (sel.empty()) return false;
  std::vector<const ON_Curve*> cs;
  for (const auto& id : sel) if (const auto* c = ON_Curve::Cast(Geo(e, id))) cs.push_back(c);
  if (cs.size() != 2) return e.Fail("seleziona due curve");
  const int N = 80;
  std::vector<ON_3dPoint> P[2];
  std::vector<ON_3dVector> T[2];
  for (int k = 0; k < 2; k++) {
    const double L = CurveLengthOf(*cs[k]);
    for (int i = 0; i < N; i++) {
      const double t = i == 0 ? cs[k]->Domain()[0] : ParamAtLengthOf(*cs[k], L * i / (N - 1));
      P[k].push_back(cs[k]->PointAt(t));
      T[k].push_back(cs[k]->TangentAt(t));
    }
  }
  if (P[0].front().DistanceTo(P[1].back()) + P[0].back().DistanceTo(P[1].front()) <
      P[0].front().DistanceTo(P[1].front()) + P[0].back().DistanceTo(P[1].back())) {
    std::reverse(P[1].begin(), P[1].end());
    std::reverse(T[1].begin(), T[1].end());
  }
  auto cost = [&](int i, int j) {
    ON_3dVector r = P[1][j] - P[0][i];
    if (!r.Unitize()) return 1.0;
    return std::fabs(ON_TripleProduct(T[0][i], T[1][j], r));
  };
  std::vector<std::vector<double>> D(N, std::vector<double>(N, 1e300));
  std::vector<std::vector<int>> from(N, std::vector<int>(N, -1));
  D[0][0] = cost(0, 0);
  for (int i = 0; i < N; i++)
    for (int j = 0; j < N; j++) {
      if (i == 0 && j == 0) continue;
      const double c = cost(i, j);
      if (i > 0 && D[i - 1][j] + c < D[i][j]) { D[i][j] = D[i - 1][j] + c; from[i][j] = 0; }
      if (j > 0 && D[i][j - 1] + c < D[i][j]) { D[i][j] = D[i][j - 1] + c; from[i][j] = 1; }
      if (i > 0 && j > 0 && D[i - 1][j - 1] + c * 0.999 < D[i][j]) { D[i][j] = D[i - 1][j - 1] + c * 0.999; from[i][j] = 2; }
    }
  std::vector<std::pair<int, int>> path;
  for (int i = N - 1, j = N - 1;;) {
    path.push_back({i, j});
    if (i == 0 && j == 0) break;
    const int f = from[i][j];
    if (f == 0) i--; else if (f == 1) j--; else { i--; j--; }
  }
  std::reverse(path.begin(), path.end());
  // le due curve come funzioni del parametro v lungo il percorso (minimi quadrati,
  // stessi nodi): la superficie e' rigata, di grado 1 tra le curve e 3 lungo v
  const int K = (int)path.size();
  std::vector<double> v{0};
  for (int k = 1; k < K; k++) {
    const auto [i0, j0] = path[k - 1];
    const auto [i1, j1] = path[k];
    v.push_back(v.back() + 0.5 * (P[0][i1].DistanceTo(P[0][i0]) + P[1][j1].DistanceTo(P[1][j0])) + 1e-12);
  }
  for (auto& x : v) x /= v.back();
  const int n = 14, ord = 4;
  ON_NurbsCurve tmpl(1, false, ord, n);
  tmpl.MakeClampedUniformKnotVector(1.0 / (n - ord + 1));
  std::vector<std::vector<double>> B(K, std::vector<double>(n));
  ON_NurbsCurve basis(tmpl);
  for (int c = 0; c < n; c++) {
    for (int k = 0; k < n; k++) basis.SetCV(k, ON_3dPoint(k == c ? 1.0 : 0.0, 0, 0));
    for (int r = 0; r < K; r++) B[r][c] = basis.PointAt(v[r] * basis.Domain().Length() + basis.Domain()[0]).x;
  }
  auto fit = [&](int which, std::vector<ON_3dPoint>& cvs) {
    std::vector<std::vector<double>> A(n, std::vector<double>(n + 3, 0.0));
    for (int r = 0; r < K; r++) {
      const ON_3dPoint q = P[which][which == 0 ? path[r].first : path[r].second];
      for (int a = 0; a < n; a++) {
        for (int b = 0; b < n; b++) A[a][b] += B[r][a] * B[r][b];
        A[a][n] += B[r][a] * q.x; A[a][n + 1] += B[r][a] * q.y; A[a][n + 2] += B[r][a] * q.z;
      }
    }
    for (int col = 0; col < n; col++) {
      int piv = col;
      for (int r = col + 1; r < n; r++) if (std::fabs(A[r][col]) > std::fabs(A[piv][col])) piv = r;
      if (std::fabs(A[piv][col]) < 1e-14) return false;
      std::swap(A[piv], A[col]);
      for (int r = 0; r < n; r++) {
        if (r == col) continue;
        const double f = A[r][col] / A[col][col];
        for (int k = col; k < n + 3; k++) A[r][k] -= f * A[col][k];
      }
    }
    cvs.resize(n);
    for (int a = 0; a < n; a++) cvs[a] = ON_3dPoint(A[a][n] / A[a][a], A[a][n + 1] / A[a][a], A[a][n + 2] / A[a][a]);
    cvs.front() = P[which].front();
    cvs.back() = P[which].back();
    return true;
  };
  std::vector<ON_3dPoint> ca, cb;
  if (!fit(0, ca) || !fit(1, cb)) return e.Fail("superficie non creata");
  auto* ns = new ON_NurbsSurface(3, false, 2, ord, 2, n);
  ns->m_knot[0][0] = 0;
  ns->m_knot[0][1] = 1;
  for (int k = 0; k < tmpl.KnotCount(); k++) ns->m_knot[1][k] = tmpl.m_knot[k];
  for (int c = 0; c < n; c++) { ns->SetCV(0, c, ca[c]); ns->SetCV(1, c, cb[c]); }
  ON_Brep* br = ON_Brep::New();
  ON_Surface* sp = ns;
  if (!br->Create(sp)) { delete br; delete ns; return e.Fail("superficie non creata"); }
  e.AddObject(br);
  e.Out() << "  Creata superficie sviluppabile (" << K << " generatrici calcolate)\n";
  return true;
}

// QuadRemesh: mesh a quadrilateri di superfici, polisuperfici e SubD (TargetQuadCount=).
// Superfici: griglia sui parametri di ogni faccia proporzionata all'area; SubD: superficie limite.
bool CmdQuadRemesh(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("QuadRemesh", "seleziona superfici, polisuperfici o SubD, poi Invio");
  if (sel.empty()) return false;
  opts.merge(e.ReadOptions(ts));
  const double target = std::max(4.0, OptNum(opts, "targetquadcount", 2000));
  int made = 0;
  for (const auto& id : sel) {
    const ON_Geometry* g = Geo(e, id);
    if (const auto* sd = ON_SubD::Cast(g)) {
      const int faces = std::max(1, (int)sd->FaceCount());
      int level = 1;
      while (level < 5 && faces * std::pow(4.0, level) < target) level++;
      auto lim = BuildSubDLimit(*sd, level);
      if (!lim.mesh) continue;
      ON_Mesh* m = lim.mesh.release();
      m->ComputeVertexNormals();
      e.AddObject(m);
      made++;
      continue;
    }
    if (!IsSurfaceLike(g)) continue;
    TopoDS_Shape s;
    try { s = occt::GeometryToShape(*g, e.Tolerance()); } catch (...) { continue; }
    double total = 0;
    std::vector<std::pair<TopoDS_Face, double>> faces;
    for (TopExp_Explorer fx(s, TopAbs_FACE); fx.More(); fx.Next()) {
      GProp_GProps gp;
      BRepGProp::SurfaceProperties(fx.Current(), gp);
      faces.push_back({TopoDS::Face(fx.Current()), gp.Mass()});
      total += gp.Mass();
    }
    if (total <= 0) continue;
    auto* m = new ON_Mesh;
    for (const auto& [f, area] : faces) {
      Handle(Geom_Surface) S = BRep_Tool::Surface(f);
      double u0, u1, v0, v1;
      BRepTools::UVBounds(f, u0, u1, v0, v1);
      // proporzioni della faccia: lunghezze delle isocurve centrali
      auto isoLen = [&](bool alongU) {
        double L = 0;
        gp_Pnt prev;
        for (int k = 0; k <= 32; k++) {
          const double t = double(k) / 32;
          gp_Pnt q = alongU ? S->Value(u0 + t * (u1 - u0), 0.5 * (v0 + v1)) : S->Value(0.5 * (u0 + u1), v0 + t * (v1 - v0));
          if (k) L += q.Distance(prev);
          prev = q;
        }
        return std::max(L, 1e-9);
      };
      const double lu = isoLen(true), lv = isoLen(false);
      const double quads = std::max(1.0, target * area / total);
      int nu = std::max(1, (int)std::round(std::sqrt(quads * lu / lv)));
      int nv = std::max(1, (int)std::round(quads / nu));
      const int base = m->m_V.Count();
      const bool rev = f.Orientation() == TopAbs_REVERSED;
      for (int i = 0; i <= nu; i++)
        for (int j = 0; j <= nv; j++) m->m_V.Append(ON_3fPoint(O(S->Value(u0 + (u1 - u0) * i / nu, v0 + (v1 - v0) * j / nv))));
      auto at = [&](int i, int j) { return base + i * (nv + 1) + j; };
      for (int i = 0; i < nu; i++)
        for (int j = 0; j < nv; j++) {
          // solo i quadrilateri che cadono dentro la faccia (facce tagliate)
          const double uc = u0 + (u1 - u0) * (i + 0.5) / nu, vc = v0 + (v1 - v0) * (j + 0.5) / nv;
          BRepClass_FaceClassifier cl(f, gp_Pnt2d(uc, vc), e.Tolerance());
          if (cl.State() != TopAbs_IN && cl.State() != TopAbs_ON) continue;
          if (rev) m->SetQuad(m->m_F.Count(), at(i, j), at(i, j + 1), at(i + 1, j + 1), at(i + 1, j));
          else m->SetQuad(m->m_F.Count(), at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1));
        }
    }
    m->CombineIdenticalVertices(true, true);
    m->Compact();
    m->ComputeVertexNormals();
    if (m->FaceCount() == 0) { delete m; continue; }
    e.AddObject(m);
    made++;
  }
  if (!made) return e.Fail("nessun oggetto convertito (le mesh in ingresso non sono ancora supportate)");
  e.Out() << "  Create " << made << " mesh a quadrilateri\n";
  return true;
}

}  // namespace

void RegisterKernel6Commands(Engine& e) {
  auto reg = [&](const char* n, const char* cat, const char* d, CommandFn f) {
    CommandInfo i{n, cat, d, f};
    i.modifiesModel = true;
    e.Register(i);
  };
  reg("Sketch", "Curve", "Curva a mano libera per punti, levigata", CmdSketch);
  reg("MatchSrf", "Superfici", "Raccorda il bordo di una superficie allo spigolo di un'altra (Continuity=Position/Tangency)", CmdMatchSrf);
  reg("DevLoft", "Superfici", "Superficie rigata sviluppabile tra due curve", CmdDevLoft);
  reg("QuadRemesh", "Mesh", "Mesh a quadrilateri da superfici, polisuperfici e SubD (TargetQuadCount=)", CmdQuadRemesh);
}

}  // namespace cad
