// Analisi della continuita' e della deviazione (Rhino 8, menu Analizza):
//   GCon            continuita' tra gli estremi di due curve (G0, G1, G2)
//   EdgeContinuity  continuita' lungo gli spigoli comuni di superfici e polisuperfici
//   PointDeviation  distanza di punti e nuvole di punti da curve, superfici e mesh
// Calcoli propri: openNURBS per le curve, OpenCASCADE per spigoli e distanze.
#include "commands/Engine.h"
#include "kernel/OcctConvert.h"
#include "kernel/SurfaceAnalysis.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepLProp_SLProps.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Geom2d_Curve.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace cad {
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

std::string Num(double v, int prec = 6) {
  std::ostringstream s;
  s.precision(prec);
  s << (std::fabs(v) < 1e-12 ? 0.0 : v);
  return s.str();
}

double AngleTolDeg(Engine& e) {
  const double a = e.Model().m_settings.m_ModelUnitsAndTolerances.m_angle_tolerance;
  return (a > 0 && a < ON_PI) ? a * 180.0 / ON_PI : 1.0;
}

double Deg(const ON_3dVector& a, const ON_3dVector& b) {
  ON_3dVector u = a, v = b;
  if (!u.Unitize() || !v.Unitize()) return 0;
  return std::acos(std::clamp(u * v, -1.0, 1.0)) * 180.0 / ON_PI;
}

// --- GCon ---------------------------------------------------------------------------
// Estremi piu' vicini delle due curve (come se l'utente avesse cliccato vicino a quegli estremi).
bool CmdGCon(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("GCon", "seleziona due curve, poi Invio");
  if (sel.empty()) return false;
  std::vector<const ON_Curve*> crv;
  for (const auto& id : sel)
    if (const ON_Curve* c = ON_Curve::Cast(Geo(e, id))) crv.push_back(c);
  if (crv.size() != 2) return e.Fail("GCon: seleziona esattamente due curve");
  const ON_Curve& a = *crv[0];
  const ON_Curve& b = *crv[1];
  // la scelta dell'estremo si puo' forzare: End1=Start|End, End2=Start|End
  int ea = -1, eb = -1;
  auto endOpt = [&](const char* k) {
    auto it = opts.find(k);
    if (it == opts.end()) return -1;
    const std::string v = NormalizeName(it->second);
    return v == "start" ? 0 : v == "end" ? 1 : -1;
  };
  ea = endOpt("end1");
  eb = endOpt("end2");
  double best = ON_DBL_MAX;
  int ba = 1, bb = 0;
  for (int i = 0; i < 2; i++)
    for (int j = 0; j < 2; j++) {
      if ((ea >= 0 && i != ea) || (eb >= 0 && j != eb)) continue;
      const ON_3dPoint pa = i ? a.PointAtEnd() : a.PointAtStart();
      const ON_3dPoint pb = j ? b.PointAtEnd() : b.PointAtStart();
      const double d = pa.DistanceTo(pb);
      if (d < best - 1e-12) { best = d; ba = i; bb = j; }
    }
  const double ta = a.Domain()[ba], tb = b.Domain()[bb];
  ON_3dPoint pa, pb;
  ON_3dVector Ta, Tb, Ka, Kb;
  a.EvCurvature(ta, pa, Ta, Ka, ba ? -1 : 1);
  b.EvCurvature(tb, pb, Tb, Kb, bb ? -1 : 1);
  // tangenti orientate nel verso "dalla prima curva alla seconda"
  if (!ba) Ta = -Ta;
  if (bb) Tb = -Tb;
  const double tol = e.Tolerance(), angTol = AngleTolDeg(e);
  const double dist = pa.DistanceTo(pb);
  const double tanDeg = Deg(Ta, Tb);
  const double ka = Ka.Length(), kb = Kb.Length();
  const double ra = ka > 1e-12 ? 1.0 / ka : 0, rb = kb > 1e-12 ? 1.0 / kb : 0;
  const bool straightA = ka <= 1e-12, straightB = kb <= 1e-12;
  e.Out() << "  Differenza fra gli estremi = " << Num(dist) << " mm\n";
  if (straightA && straightB)
    e.Out() << "  Raggio di curvatura: entrambe le curve sono diritte nel punto\n";
  else if (straightA || straightB)
    e.Out() << "  Raggio di curvatura: una curva e' diritta (raggio infinito), l'altra ha raggio "
            << Num(straightA ? rb : ra) << " mm\n";
  else
    e.Out() << "  Differenza del raggio di curvatura = " << Num(std::fabs(ra - rb)) << " mm\n";
  const double kDirDeg = (straightA || straightB) ? 0 : Deg(Ka, Kb);
  if (!straightA && !straightB) e.Out() << "  Differenza della direzione di curvatura = " << Num(kDirDeg) << " gradi\n";
  e.Out() << "  Differenza di tangenza = " << Num(tanDeg) << " gradi\n";
  const double relK = std::max(ka, kb) > 1e-12 ? std::fabs(ka - kb) / std::max(ka, kb) : 0;
  const double kTol = OptNum(opts, "curvaturetolerance", 1.0) / 100.0;
  std::string g = "non sono continue (G0 mancante)";
  if (dist <= tol) {
    g = "G0";
    if (tanDeg <= angTol) {
      g = "G1";
      const bool sameK = (straightA && straightB) || (!straightA && !straightB && relK <= kTol && kDirDeg <= angTol);
      if (sameK) g = "G2";
    }
  }
  e.Out() << "  Le curve sono " << g << ".\n";
  return true;
}

// --- EdgeContinuity -------------------------------------------------------------------
gp_Pnt G(const ON_3dPoint& p) { return gp_Pnt(p.x, p.y, p.z); }

struct Side {
  bool ok = false;
  gp_Pnt p;
  gp_Vec n;    // normale della superficie (non orientata con la faccia)
  double k = 0;  // curvatura normale trasversale allo spigolo, segno rispetto a n
};

Side Evaluate(const TopoDS_Edge& E, const TopoDS_Face& F, double s, const gp_Vec& edgeTan) {
  Side r;
  double f, l;
  Handle(Geom2d_Curve) pc = BRep_Tool::CurveOnSurface(E, F, f, l);
  if (pc.IsNull()) return r;
  const gp_Pnt2d uv = pc->Value(f + s * (l - f));
  try {
    BRepAdaptor_Surface as(F, Standard_False);  // senza i limiti: si valuta la superficie
    BRepLProp_SLProps pr(as, uv.X(), uv.Y(), 2, 1e-9);
    r.p = pr.Value();
    if (!pr.IsNormalDefined()) return r;
    r.n = gp_Vec(pr.Normal());
    gp_Vec d = r.n.Crossed(edgeTan);
    if (d.Magnitude() < 1e-12) return r;
    d.Normalize();
    if (pr.IsCurvatureDefined()) {
      gp_Dir dmax, dmin;
      pr.CurvatureDirections(dmax, dmin);
      const double cmax = pr.MaxCurvature(), cmin = pr.MinCurvature();
      const double a = d.Dot(gp_Vec(dmax)), b = d.Dot(gp_Vec(dmin));
      r.k = cmax * a * a + cmin * b * b;
    }
    r.ok = true;
  } catch (...) {}
  return r;
}

bool CmdEdgeContinuity(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("EdgeContinuity", "seleziona superfici o polisuperfici, poi Invio");
  if (sel.empty()) return false;
  const double tol = e.Tolerance(), angTol = AngleTolDeg(e);
  const double joinTol = OptNum(opts, "tolerance", 10 * tol);  // distanza per considerare due bordi lo stesso spigolo
  const int samples = std::clamp((int)OptNum(opts, "samplepoints", 20), 2, 1000);
  const double kTol = OptNum(opts, "curvaturetolerance", 2.0);  // percento
  BRep_Builder bb;
  TopoDS_Compound comp;
  bb.MakeCompound(comp);
  int n = 0;
  for (const auto& id : sel) {
    const ON_Geometry* g = Geo(e, id);
    if (!g || !(ON_Brep::Cast(g) || ON_Extrusion::Cast(g) || ON_Surface::Cast(g))) continue;
    TopoDS_Shape s = occt::GeometryToShape(*g, tol);
    if (s.IsNull()) continue;
    bb.Add(comp, s);
    n++;
  }
  if (!n) return e.Fail("EdgeContinuity: nessuna superficie selezionata");
  // gli spigoli liberi di oggetti diversi che coincidono diventano comuni (le superfici non cambiano)
  BRepBuilderAPI_Sewing sew(joinTol, Standard_True, Standard_False, Standard_True, Standard_False);
  sew.Add(comp);
  TopoDS_Shape shape = comp;
  try {
    sew.Perform();
    if (!sew.SewedShape().IsNull()) shape = sew.SewedShape();
  } catch (...) {}
  TopTools_IndexedDataMapOfShapeListOfShape map;
  TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, map);
  int edges = 0, g0 = 0, g1 = 0, g2 = 0;
  double maxD = 0, maxA = 0, maxK = 0;
  for (int i = 1; i <= map.Extent(); i++) {
    const TopoDS_Edge& E = TopoDS::Edge(map.FindKey(i));
    if (BRep_Tool::Degenerated(E)) continue;
    std::vector<TopoDS_Face> faces;
    for (TopTools_ListOfShape::Iterator it(map.FindFromIndex(i)); it.More(); it.Next()) {
      const TopoDS_Face& F = TopoDS::Face(it.Value());
      if (std::none_of(faces.begin(), faces.end(), [&](const TopoDS_Face& x) { return x.IsSame(F); })) faces.push_back(F);
    }
    if (faces.size() != 2) continue;  // bordo libero o cucitura della stessa faccia
    BRepAdaptor_Curve ac(E);
    double d = 0, a = 0, k = 0;
    int valid = 0;
    for (int j = 0; j < samples; j++) {
      // estremi esclusi di poco: nei vertici la valutazione e' spesso degenere
      const double s = (j + 0.5) / samples;
      gp_Pnt q;
      gp_Vec T;
      ac.D1(ac.FirstParameter() + s * (ac.LastParameter() - ac.FirstParameter()), q, T);
      if (T.Magnitude() < 1e-12) continue;
      const Side s1 = Evaluate(E, faces[0], s, T), s2 = Evaluate(E, faces[1], s, T);
      if (!s1.ok || !s2.ok) continue;
      valid++;
      d = std::max(d, s1.p.Distance(s2.p));
      const double c = std::fabs(s1.n.Dot(s2.n)) / (s1.n.Magnitude() * s2.n.Magnitude());
      a = std::max(a, std::acos(std::clamp(c, -1.0, 1.0)) * 180.0 / ON_PI);
      const double k2 = s1.n.Dot(s2.n) < 0 ? -s2.k : s2.k;
      const double km = std::max(std::fabs(s1.k), std::fabs(k2));
      if (km > 1e-9) k = std::max(k, 100.0 * std::fabs(s1.k - k2) / km);
    }
    if (!valid) continue;
    edges++;
    const char* cls = "aperto";
    if (d <= tol) {
      g0++;
      cls = "G0";
      if (a <= angTol) {
        g1++;
        cls = "G1";
        if (k <= kTol) { g2++; cls = "G2"; }
      }
    }
    maxD = std::max(maxD, d);
    maxA = std::max(maxA, a);
    maxK = std::max(maxK, k);
    e.Out() << "  Spigolo " << edges << ": distanza " << Num(d) << " mm, angolo " << Num(a) << " gradi, curvatura "
            << Num(k, 4) << " %  -> " << cls << "\n";
  }
  if (!edges) return e.Fail("EdgeContinuity: nessuno spigolo comune tra le superfici selezionate");
  e.Out() << "  Spigoli analizzati: " << edges << " (G0 " << g0 << ", G1 " << g1 << ", G2 " << g2 << ")\n";
  e.Out() << "  Massimi: distanza " << Num(maxD) << " mm, angolo " << Num(maxA) << " gradi, curvatura " << Num(maxK, 4)
          << " %\n";
  return true;
}

// --- PointDeviation ---------------------------------------------------------------------
ON_3dPoint ClosestOnTriangle(const ON_3dPoint& p, const ON_3dPoint& a, const ON_3dPoint& b, const ON_3dPoint& c) {
  const ON_3dVector ab = b - a, ac = c - a, ap = p - a;
  const double d1 = ab * ap, d2 = ac * ap;
  if (d1 <= 0 && d2 <= 0) return a;
  const ON_3dVector bp = p - b;
  const double d3 = ab * bp, d4 = ac * bp;
  if (d3 >= 0 && d4 <= d3) return b;
  const double vc = d1 * d4 - d3 * d2;
  if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
  const ON_3dVector cp = p - c;
  const double d5 = ab * cp, d6 = ac * cp;
  if (d6 >= 0 && d5 <= d6) return c;
  const double vb = d5 * d2 - d1 * d6;
  if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
  const double va = d3 * d6 - d5 * d4;
  if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
  const double den = 1.0 / (va + vb + vc);
  return a + ab * (vb * den) + ac * (vc * den);
}

double MeshDistance(const ON_Mesh& m, const ON_3dPoint& p) {
  double best = ON_DBL_MAX;
  for (int i = 0; i < m.m_F.Count(); i++) {
    const ON_MeshFace& f = m.m_F[i];
    const ON_3dPoint A = m.Vertex(f.vi[0]), B = m.Vertex(f.vi[1]), C = m.Vertex(f.vi[2]);
    best = std::min(best, p.DistanceTo(ClosestOnTriangle(p, A, B, C)));
    if (f.IsQuad()) best = std::min(best, p.DistanceTo(ClosestOnTriangle(p, A, C, m.Vertex(f.vi[3]))));
  }
  return best;
}

bool CmdPointDeviation(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("PointDeviation", "seleziona gli oggetti da confrontare e i punti, poi Invio");
  if (sel.empty()) return false;
  const double tol = e.Tolerance();
  const double good = OptNum(opts, "goodpoint", 0.1);
  const double bad = std::max(good, OptNum(opts, "badpoint", 1.0));
  const double ignore = OptNum(opts, "ignorepoint", 10.0);
  std::vector<ON_3dPoint> pts;
  std::vector<TopoDS_Shape> shapes;
  std::vector<const ON_Mesh*> meshes;
  for (const auto& id : sel) {
    const ON_Geometry* g = Geo(e, id);
    if (!g) continue;
    if (const auto* p = ON_Point::Cast(g)) pts.push_back(p->point);
    else if (const auto* pc = ON_PointCloud::Cast(g)) for (int i = 0; i < pc->m_P.Count(); i++) pts.push_back(pc->m_P[i]);
    else if (const auto* m = ON_Mesh::Cast(g)) meshes.push_back(m);
    else {
      TopoDS_Shape s = occt::GeometryToShape(*g, tol);
      if (!s.IsNull()) shapes.push_back(s);
    }
  }
  if (pts.empty()) return e.Fail("PointDeviation: nessun punto selezionato");
  if (shapes.empty() && meshes.empty()) return e.Fail("PointDeviation: nessuna curva, superficie o mesh selezionata");
  std::vector<double> dev;
  int ignored = 0;
  for (const ON_3dPoint& p : pts) {
    double d = ON_DBL_MAX;
    for (const auto* m : meshes) d = std::min(d, MeshDistance(*m, p));
    if (!shapes.empty()) {
      TopoDS_Vertex v = BRepBuilderAPI_MakeVertex(G(p));
      for (const auto& s : shapes) {
        try {
          BRepExtrema_DistShapeShape ex(v, s);
          if (ex.IsDone() && ex.NbSolution() > 0) d = std::min(d, ex.Value());
        } catch (...) {}
      }
    }
    if (d == ON_DBL_MAX) continue;
    if (d > ignore) { ignored++; continue; }
    dev.push_back(d);
  }
  if (dev.empty()) return e.Fail("PointDeviation: tutti i punti sono oltre IgnorePoint");
  int nGood = 0, nBad = 0;
  double sum = 0, mx = 0, mn = ON_DBL_MAX;
  for (double d : dev) {
    if (d <= good) nGood++;
    if (d > bad) nBad++;
    sum += d;
    mx = std::max(mx, d);
    mn = std::min(mn, d);
  }
  const double mean = sum / dev.size();
  double var = 0;
  for (double d : dev) var += (d - mean) * (d - mean);
  std::vector<double> s = dev;
  std::sort(s.begin(), s.end());
  const double median = s.size() % 2 ? s[s.size() / 2] : 0.5 * (s[s.size() / 2 - 1] + s[s.size() / 2]);
  e.Out() << "  Punti: " << dev.size() << " confrontati, " << ignored << " ignorati (oltre " << Num(ignore) << " mm)\n";
  e.Out() << "  Buoni (<= " << Num(good) << " mm): " << nGood << "   Intermedi: " << (dev.size() - nGood - nBad)
          << "   Cattivi (> " << Num(bad) << " mm): " << nBad << "\n";
  e.Out() << "  Distanza media = " << Num(mean) << " mm   mediana = " << Num(median) << " mm   deviazione standard = "
          << Num(std::sqrt(var / dev.size())) << " mm\n";
  e.Out() << "  Distanza minima = " << Num(mn) << " mm   massima = " << Num(mx) << " mm\n";
  return true;
}


// --- CurvatureAnalysis (riga di comando) -------------------------------------------------
// Nell'interfaccia colora le superfici; qui riporta i valori e gli intervalli per lo stile.
bool CmdCurvatureAnalysis(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts, {"autorange", "maxrange"});
  auto sel = e.RequireSelection("CurvatureAnalysis", "seleziona superfici, polisuperfici o mesh, poi Invio");
  if (sel.empty()) return false;
  const CurvatureStyle st = ParseCurvatureStyle(opts.count("style") ? opts["style"] : "gaussian");
  std::vector<CurvatureData> data;
  data.reserve(sel.size());
  for (const auto& id : sel) {
    const ON_Geometry* g = Geo(e, id);
    if (!g || ON_Curve::Cast(g) || ON_Point::Cast(g)) continue;
    CurvatureData d;
    if (ComputeCurvature(*g, e.Tolerance(), d)) data.push_back(std::move(d));
  }
  if (data.empty()) return e.Fail("CurvatureAnalysis: nessuna superficie o mesh selezionata");
  std::vector<const CurvatureData*> ptr;
  for (const auto& d : data) ptr.push_back(&d);
  double alo, ahi, mlo, mhi;
  CurvatureRange(ptr, st, true, alo, ahi);
  CurvatureRange(ptr, st, false, mlo, mhi);
  size_t nv = 0;
  for (const auto& d : data) nv += d.k1.size();
  e.Out() << "  Analisi di curvatura (" << CurvatureStyleLabel(st) << ") su " << data.size() << " oggetti, " << nv
          << " punti\n";
  e.Out() << "  Intervallo massimo: " << Num(mlo) << " .. " << Num(mhi) << "\n";
  e.Out() << "  Intervallo automatico: " << Num(alo) << " .. " << Num(ahi) << "\n";
  return true;
}

}  // namespace

void RegisterAnalysisCommands(Engine& e) {
  auto reg = [&](const char* n, const char* d, CommandFn f) {
    CommandInfo i{n, "Analisi", d, f};
    i.modifiesModel = false;
    e.Register(i);
  };
  reg("GCon", "Continuita' tra gli estremi di due curve (G0, G1, G2)", CmdGCon);
  reg("EdgeContinuity", "Continuita' lungo gli spigoli comuni delle superfici (distanza, angolo, curvatura)", CmdEdgeContinuity);
  reg("CurvatureAnalysis", "Curvatura a falsi colori delle superfici (Style=Gaussian|Mean|MinRadius|MaxRadius)", CmdCurvatureAnalysis);
  reg("PointDeviation", "Distanza dei punti da curve, superfici e mesh (GoodPoint=, BadPoint=, IgnorePoint=)", CmdPointDeviation);
}

}  // namespace cad
