// Comandi di modellazione di Rhino 8 (0.13): MergeSrf, SplitFace, ShrinkWrap, OffsetMesh,
// MeshRepair, CullDegenerateMeshFaces, RebuildMeshNormals, ExtractRenderMesh.
// Algoritmi propri su openNURBS; OpenCASCADE per le divisioni delle facce e la rimozione dei nodi.
#include "commands/Engine.h"
#include "core/SubDLimit.h"
#include "kernel/OcctConvert.h"

#include <BRepAlgoAPI_Splitter.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepAlgoAPI_Section.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <Geom_BSplineSurface.hxx>
#include <Geom_Surface.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace cad {
namespace {

const ON_Geometry* Geo(Engine& e, const ON_UUID& id) {
  const auto* o = e.Object(id);
  return o ? o->Geometry(nullptr) : nullptr;
}

ON_3dmObjectAttributes Attrs(Engine& e, const ON_UUID& id) {
  const auto* o = e.Object(id);
  ON_3dmObjectAttributes a = o && o->Attributes(nullptr) ? *o->Attributes(nullptr) : ON_3dmObjectAttributes::DefaultAttributes;
  a.m_uuid = ON_nil_uuid;
  a.SetMode(ON::object_mode::normal_object);
  return a;
}

double OptNum(const std::map<std::string, std::string>& o, const char* k, double def) {
  auto it = o.find(k);
  if (it == o.end()) return def;
  try { return std::stod(it->second); } catch (...) { return def; }
}

bool OptBool(const std::map<std::string, std::string>& o, const char* k, bool def) {
  auto it = o.find(k);
  if (it == o.end()) return def;
  return it->second == "yes" || it->second == "si" || it->second == "true" || it->second == "1";
}

// --- MergeSrf ------------------------------------------------------------------------------
// Superficie NURBS non tagliata di un oggetto (superficie, estrusione o polisuperficie di una
// faccia non tagliata), come richiede MergeSrf in Rhino.
bool UntrimmedNurbs(const ON_Geometry* g, ON_NurbsSurface& out) {
  if (!g) return false;
  if (const auto* s = ON_Surface::Cast(g)) return s->GetNurbForm(out) > 0;
  std::unique_ptr<ON_Brep> owned;
  const ON_Brep* b = ON_Brep::Cast(g);
  if (!b) {
    if (const auto* x = ON_Extrusion::Cast(g)) {
      if (x->IsCapped()) return false;
      owned.reset(x->BrepForm());
      b = owned.get();
    }
  }
  if (!b || b->m_F.Count() != 1 || !b->FaceIsSurface(0)) return false;
  if (!b->m_F[0].GetNurbForm(out)) return false;
  if (b->m_F[0].m_bRev) out.Reverse(0);  // la normale segue la faccia
  return true;
}

// Bordo di un lato: 0 = u min, 1 = u max, 2 = v min, 3 = v max (punti campionati)
std::vector<ON_3dPoint> SideSamples(const ON_NurbsSurface& s, int side, int n = 9) {
  std::vector<ON_3dPoint> p;
  const ON_Interval du = s.Domain(0), dv = s.Domain(1);
  for (int i = 0; i < n; i++) {
    const double t = double(i) / (n - 1);
    if (side < 2) p.push_back(s.PointAt(side == 0 ? du[0] : du[1], dv.ParameterAt(t)));
    else p.push_back(s.PointAt(du.ParameterAt(t), side == 2 ? dv[0] : dv[1]));
  }
  return p;
}

// Porta il lato "side" in u max (atEnd) o in u min.
void MoveSideTo(ON_NurbsSurface& s, int side, bool atEnd) {
  if (side >= 2) { s.Transpose(); side -= 2; }
  if ((side == 1) != atEnd) s.Reverse(0);
}

std::vector<std::pair<double, int>> Knots(const ON_NurbsSurface& s, int dir) {
  std::vector<std::pair<double, int>> k;
  const int n = s.KnotCount(dir);
  for (int i = 0; i < n; i++) {
    const double v = s.Knot(dir, i);
    if (!k.empty() && std::fabs(k.back().first - v) < 1e-10) k.back().second++;
    else k.push_back({v, 1});
  }
  return k;
}

// Stesso grado e stessi nodi nella direzione dir (domini gia' uguali).
void MakeCompatible(ON_NurbsSurface& a, ON_NurbsSurface& b, int dir) {
  const int deg = std::max(a.Degree(dir), b.Degree(dir));
  if (a.Degree(dir) < deg) a.IncreaseDegree(dir, deg);
  if (b.Degree(dir) < deg) b.IncreaseDegree(dir, deg);
  for (int pass = 0; pass < 2; pass++) {
    ON_NurbsSurface& src = pass == 0 ? a : b;
    ON_NurbsSurface& dst = pass == 0 ? b : a;
    const auto ks = Knots(src, dir);
    for (size_t i = 1; i + 1 < ks.size(); i++) {
      const auto kd = Knots(dst, dir);
      int m = 0;
      for (const auto& [v, mult] : kd) if (std::fabs(v - ks[i].first) < 1e-10) m = mult;
      if (ks[i].second > m) dst.InsertKnot(dir, ks[i].first, ks[i].second);
    }
  }
}

bool CmdMergeSrf(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts, {"smooth"});
  auto sel = e.RequireSelection("MergeSrf", "seleziona due superfici non tagliate con un bordo in comune, poi Invio");
  if (sel.empty()) return false;
  if (sel.size() != 2) return e.Fail("MergeSrf: seleziona esattamente due superfici");
  ON_NurbsSurface A, B;
  if (!UntrimmedNurbs(Geo(e, sel[0]), A) || !UntrimmedNurbs(Geo(e, sel[1]), B))
    return e.Fail("MergeSrf: servono superfici non tagliate (usa ShrinkTrimmedSrf o UntrimAll)");
  const double tol = std::max(e.Tolerance(), OptNum(opts, "tolerance", e.Tolerance()));
  // lati coincidenti (nello stesso verso o opposto)
  int sa = -1, sb = -1;
  bool flip = false;
  double best = ON_DBL_MAX;
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++) {
      const auto pa = SideSamples(A, i), pb = SideSamples(B, j);
      for (int f = 0; f < 2; f++) {
        double d = 0;
        for (size_t k = 0; k < pa.size(); k++) d = std::max(d, pa[k].DistanceTo(pb[f ? pb.size() - 1 - k : k]));
        if (d < best) { best = d; sa = i; sb = j; flip = f == 1; }
      }
    }
  if (best > 10 * tol) return e.Fail("MergeSrf: le superfici non hanno un bordo in comune (distanza " + std::to_string(best) + ")");
  MoveSideTo(A, sa, true);
  MoveSideTo(B, sb, false);
  // verso lungo il bordo: A(umax, vmin) deve coincidere con B(umin, vmin)
  if (A.PointAt(A.Domain(0)[1], A.Domain(1)[0]).DistanceTo(B.PointAt(B.Domain(0)[0], B.Domain(1)[0])) >
      A.PointAt(A.Domain(0)[1], A.Domain(1)[0]).DistanceTo(B.PointAt(B.Domain(0)[0], B.Domain(1)[1])))
    B.Reverse(1);
  (void)flip;
  // normali concordi: se opposte si inverte B in u e lo si rimette in coda ad A (impossibile senza
  // cambiare lato), quindi si inverte B in v e si ripete l'allineamento
  {
    const ON_3dVector na = A.NormalAt(A.Domain(0)[1], A.Domain(1).Mid());
    const ON_3dVector nb = B.NormalAt(B.Domain(0)[0], B.Domain(1).Mid());
    if (na * nb < 0) {
      // l'unica orientazione coerente con il bordo comune ha le normali opposte: si inverte
      // la direzione v di entrambe (la superficie unita mantiene la normale di A)
      B.Transpose();
      B.Reverse(1);
      B.Transpose();
    }
  }
  A.SetDomain(1, 0.0, 1.0);
  B.SetDomain(1, 0.0, 1.0);
  MakeCompatible(A, B, 1);
  const int du = std::max(A.Degree(0), B.Degree(0));
  if (A.Degree(0) < du) A.IncreaseDegree(0, du);
  if (B.Degree(0) < du) B.IncreaseDegree(0, du);
  // parametro u proporzionale alle dimensioni (velocita' simile ai due lati del bordo)
  auto spanLen = [](const ON_NurbsSurface& s) {
    const double v = s.Domain(1).Mid();
    double L = 0;
    ON_3dPoint prev = s.PointAt(s.Domain(0)[0], v);
    for (int i = 1; i <= 16; i++) {
      const ON_3dPoint p = s.PointAt(s.Domain(0).ParameterAt(i / 16.0), v);
      L += p.DistanceTo(prev);
      prev = p;
    }
    return std::max(L, 1e-9);
  };
  const double la = spanLen(A), lb = spanLen(B);
  A.SetDomain(0, 0.0, la);
  B.SetDomain(0, la, la + lb);
  if (A.IsRational() || B.IsRational()) { A.MakeRational(); B.MakeRational(); }
  const bool rational = A.IsRational();
  if (rational) {
    // pesi uguali sul bordo comune: si scala B (stessa superficie, coordinate omogenee)
    const int nb0 = B.CVCount(0);
    double ratio = A.Weight(A.CVCount(0) - 1, 0) / B.Weight(0, 0);
    for (int j = 0; j < B.CVCount(1); j++)
      if (std::fabs(A.Weight(A.CVCount(0) - 1, j) / B.Weight(0, j) - ratio) > 1e-9)
        return e.Fail("MergeSrf: pesi incompatibili sul bordo comune");
    for (int i = 0; i < nb0; i++)
      for (int j = 0; j < B.CVCount(1); j++) {
        double* cv = B.CV(i, j);
        for (int k = 0; k < 4; k++) cv[k] *= ratio;
      }
  }
  const int na = A.CVCount(0), nb = B.CVCount(0), nv = A.CVCount(1);
  auto* M = new ON_NurbsSurface(3, rational, du + 1, A.Order(1), na + nb - 1, nv);
  for (int i = 0; i < A.KnotCount(0); i++) M->SetKnot(0, i, A.Knot(0, i));
  for (int i = du; i < B.KnotCount(0); i++) M->SetKnot(0, A.KnotCount(0) + i - du, B.Knot(0, i));
  for (int i = 0; i < A.KnotCount(1); i++) M->SetKnot(1, i, A.Knot(1, i));
  for (int i = 0; i < na + nb - 1; i++)
    for (int j = 0; j < nv; j++) {
      const double* cv = i < na ? A.CV(i, j) : B.CV(i - na + 1, j);
      if (i == na - 1) {
        // punto medio del bordo comune (chiude piccole fessure entro la tolleranza)
        const double* cb = B.CV(0, j);
        double m[4];
        for (int k = 0; k < (rational ? 4 : 3); k++) m[k] = 0.5 * (cv[k] + cb[k]);
        M->SetCV(i, j, rational ? ON::homogeneous_rational : ON::not_rational, m);
      } else {
        M->SetCV(i, j, rational ? ON::homogeneous_rational : ON::not_rational, cv);
      }
    }
  if (!M->IsValid()) { delete M; return e.Fail("MergeSrf: superficie unita non valida"); }
  // Smooth=Yes: si toglie il nodo multiplo del bordo dove la forma non cambia oltre la tolleranza
  int removed = 0;
  if (OptBool(opts, "smooth", true)) {
    Handle(Geom_BSplineSurface) g = occt::ToOcct(*M);
    for (int k = 1; k <= g->NbUKnots(); k++)
      if (std::fabs(g->UKnot(k) - la) < 1e-9) {
        const int mult = g->UMultiplicity(k);
        for (int target = mult - 1; target >= 0; target--) {
          if (!g->RemoveUKnot(k, target, tol)) break;
          removed++;
          if (target == 0) break;
        }
        break;
      }
    if (removed) {
      if (ON_NurbsSurface* s = occt::FromOcct(g)) { delete M; M = s; }
    }
  }
  ON_Brep* brep = ON_Brep::New();
  brep->Create(M);
  const ON_3dmObjectAttributes at = Attrs(e, sel[0]);
  e.AddObject(brep, &at);
  e.DeleteObject(sel[0]);
  e.DeleteObject(sel[1]);
  e.Out() << "  Superfici unite" << (removed ? " (bordo levigato)" : "") << "\n";
  return true;
}

// --- SplitFace -------------------------------------------------------------------------------
// Divide le facce di una polisuperficie con curve (proiettate lungo z del CPlane del mondo se
// non giacciono gia' sulla faccia) o con un'isocurva per un punto (Isocurve, Direction=U|V);
// il risultato resta una polisuperficie, come in Rhino.
bool CmdSplitFace(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts, {"isocurve"});
  auto sel = e.RequireSelection("SplitFace", "seleziona la polisuperficie e le curve di taglio, poi Invio");
  if (sel.empty()) return false;
  const double tol = e.Tolerance();
  ON_UUID target = ON_nil_uuid;
  std::vector<const ON_Curve*> curves;
  for (const auto& id : sel) {
    const ON_Geometry* g = Geo(e, id);
    if (const auto* c = ON_Curve::Cast(g)) curves.push_back(c);
    else if (g && ON_UuidIsNil(target) && (ON_Brep::Cast(g) || ON_Extrusion::Cast(g) || ON_Surface::Cast(g))) target = id;
  }
  if (ON_UuidIsNil(target)) return e.Fail("SplitFace: seleziona una superficie o polisuperficie");
  TopoDS_Shape shape = occt::GeometryToShape(*Geo(e, target), tol);
  if (shape.IsNull()) return e.Fail("SplitFace: oggetto non convertibile");
  TopTools_ListOfShape tools;
  const bool iso = opts.count("isocurve") > 0;
  if (iso) {
    // punto sulla faccia e direzione dell'isocurva
    auto p = e.ReadPick(ts, "punto sulla faccia per l'isocurva");
    if (!p) return false;
    const bool dirV = opts.count("direction") && opts["direction"] == "v";
    TopoDS_Vertex v = BRepBuilderAPI_MakeVertex(gp_Pnt(p->x, p->y, p->z));
    TopoDS_Face bestF;
    double best = ON_DBL_MAX;
    for (TopExp_Explorer fx(shape, TopAbs_FACE); fx.More(); fx.Next()) {
      BRepExtrema_DistShapeShape d(v, fx.Current());
      if (d.IsDone() && d.Value() < best) { best = d.Value(); bestF = TopoDS::Face(fx.Current()); }
    }
    if (bestF.IsNull() || best > 10 * tol + 1e-3) return e.Fail("SplitFace: il punto non e' su una faccia");
    Handle(Geom_Surface) gs = BRep_Tool::Surface(bestF);
    GeomAPI_ProjectPointOnSurf pr(gp_Pnt(p->x, p->y, p->z), gs);
    if (!pr.NbPoints()) return e.Fail("SplitFace: proiezione non riuscita");
    double u, w;
    pr.LowerDistanceParameters(u, w);
    double u0, u1, v0, v1;
    BRepTools::UVBounds(bestF, u0, u1, v0, v1);
    // Direction=U: isocurva a u costante (corre lungo v), come la direzione mostrata da Rhino
    Handle(Geom_Curve) c = dirV ? gs->VIso(w) : gs->UIso(u);
    const double a = dirV ? u0 : v0, b = dirV ? u1 : v1;
    tools.Append(BRepBuilderAPI_MakeEdge(new Geom_TrimmedCurve(c, a, b)).Edge());
  } else {
    if (curves.empty()) return e.Fail("SplitFace: seleziona anche le curve di taglio (o usa Isocurve)");
    for (const ON_Curve* c : curves) {
      TopoDS_Shape cs = occt::CurveToShape(*c);
      if (cs.IsNull()) continue;
      BRepExtrema_DistShapeShape d(cs, shape);
      ON_BoundingBox cb;
      c->GetTightBoundingBox(cb);
      // gia' sulla superficie: si usa cosi' com'e'
      bool onSurface = false;
      if (d.IsDone() && d.Value() < tol) {
        onSurface = true;
        for (int i = 0; i <= 8 && onSurface; i++) {
          const ON_3dPoint q = c->PointAt(c->Domain().ParameterAt(i / 8.0));
          BRepExtrema_DistShapeShape dq(BRepBuilderAPI_MakeVertex(gp_Pnt(q.x, q.y, q.z)).Vertex(), shape);
          onSurface = dq.IsDone() && dq.Value() < tol;
        }
      }
      if (onSurface) { tools.Append(cs); continue; }
      // proiezione lungo z: sezione delle facce con la "tenda" della curva estrusa in z
      ON_BoundingBox sb;
      Geo(e, target)->GetTightBoundingBox(sb);
      const double zlo = std::min(sb.m_min.z, cb.m_min.z) - 1.0, zhi = std::max(sb.m_max.z, cb.m_max.z) + 1.0;
      std::unique_ptr<ON_Curve> low(c->DuplicateCurve());
      ON_Xform down = ON_Xform::TranslationTransformation(0, 0, zlo - cb.m_min.z);
      low->Transform(down);
      TopoDS_Shape lowShape = occt::CurveToShape(*low);
      try {
        TopoDS_Shape curtain = BRepPrimAPI_MakePrism(lowShape, gp_Vec(0, 0, zhi - zlo + (cb.m_max.z - cb.m_min.z))).Shape();
        BRepAlgoAPI_Section sec(shape, curtain, Standard_False);
        sec.Approximation(Standard_True);
        sec.ComputePCurveOn1(Standard_True);
        sec.Build();
        int k = 0;
        if (sec.IsDone())
          for (TopExp_Explorer ee(sec.Shape(), TopAbs_EDGE); ee.More(); ee.Next()) {
            // le pareti parallele alla proiezione non si dividono (spigoli verticali della sezione)
            BRepAdaptor_Curve ac(TopoDS::Edge(ee.Current()));
            gp_Pnt mp;
            gp_Vec tv;
            ac.D1(0.5 * (ac.FirstParameter() + ac.LastParameter()), mp, tv);
            if (tv.Magnitude() > 1e-12 && std::fabs(tv.Normalized().Z()) > 0.99) continue;
            tools.Append(ee.Current());
            k++;
          }
        if (!k) e.Out() << "  curva che non cade sulle facce: ignorata\n";
      } catch (const Standard_Failure& ex) { e.Out() << "  errore di OpenCASCADE: " << ex.GetMessageString() << "\n"; } catch (...) { e.Out() << "  errore\n"; }
    }
  }
  if (tools.IsEmpty()) return e.Fail("SplitFace: nessuna curva di taglio sulle facce");
  TopTools_IndexedMapOfShape before;
  TopExp::MapShapes(shape, TopAbs_FACE, before);
  BRepAlgoAPI_Splitter sp;
  TopTools_ListOfShape args;
  args.Append(shape);
  sp.SetArguments(args);
  sp.SetTools(tools);
  sp.SetFuzzyValue(tol);
  sp.Build();
  if (!sp.IsDone()) return e.Fail("SplitFace: divisione non riuscita");
  TopoDS_Shape r = sp.Shape();
  TopTools_IndexedMapOfShape after;
  TopExp::MapShapes(r, TopAbs_FACE, after);
  if (after.Extent() <= before.Extent()) return e.Fail("SplitFace: le curve non dividono nessuna faccia");
  ON_Brep* b = occt::ShapeToBrep(r, tol);
  if (!b) return e.Fail("SplitFace: risultato non valido");
  e.ReplaceObject(target, b);
  e.Out() << "  Facce: " << before.Extent() << " -> " << after.Extent() << "\n";
  return true;
}

// --- triangoli degli oggetti (ShrinkWrap) --------------------------------------------------
struct Tri { ON_3dPoint a, b, c; };

void AppendMesh(const ON_Mesh& m, std::vector<Tri>& out) {
  for (int i = 0; i < m.m_F.Count(); i++) {
    const ON_MeshFace& f = m.m_F[i];
    out.push_back({m.Vertex(f.vi[0]), m.Vertex(f.vi[1]), m.Vertex(f.vi[2])});
    if (f.IsQuad()) out.push_back({m.Vertex(f.vi[0]), m.Vertex(f.vi[2]), m.Vertex(f.vi[3])});
  }
}

bool Triangles(Engine& e, const ON_Geometry* g, std::vector<Tri>& out, std::vector<ON_3dPoint>& pts) {
  if (!g) return false;
  if (const auto* m = ON_Mesh::Cast(g)) { AppendMesh(*m, out); return true; }
  if (const auto* p = ON_Point::Cast(g)) { pts.push_back(p->point); return true; }
  if (const auto* pc = ON_PointCloud::Cast(g)) { for (int i = 0; i < pc->m_P.Count(); i++) pts.push_back(pc->m_P[i]); return true; }
  if (const auto* sd = ON_SubD::Cast(g)) {
    auto d = BuildSubDLimit(*sd, 2);
    if (d.mesh) { AppendMesh(*d.mesh, out); return true; }
    return false;
  }
  if (ON_Brep::Cast(g) || ON_Extrusion::Cast(g) || ON_Surface::Cast(g)) {
    ON_BoundingBox bb;
    g->GetTightBoundingBox(bb);
    std::unique_ptr<ON_Mesh> m(occt::MeshShape(occt::GeometryToShape(*g, e.Tolerance()), std::max(bb.Diagonal().Length() / 300.0, 0.01), 0.3));
    if (m) { AppendMesh(*m, out); return true; }
  }
  if (const auto* c = ON_Curve::Cast(g)) {
    // le curve contano come punti fitti lungo la curva
    for (int i = 0; i <= 200; i++) pts.push_back(c->PointAt(c->Domain().ParameterAt(i / 200.0)));
    return true;
  }
  return false;
}

ON_3dPoint ClosestOnTri(const ON_3dPoint& p, const ON_3dPoint& a, const ON_3dPoint& b, const ON_3dPoint& c) {
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

double SignedVolume(const ON_Mesh& m) {
  double v = 0;
  for (int i = 0; i < m.m_F.Count(); i++) {
    const ON_MeshFace& f = m.m_F[i];
    const ON_3dVector a(m.Vertex(f.vi[0])), b(m.Vertex(f.vi[1])), c(m.Vertex(f.vi[2]));
    v += a * ON_CrossProduct(b, c);
    if (f.IsQuad()) { const ON_3dVector d(m.Vertex(f.vi[3])); v += a * ON_CrossProduct(c, d); }
  }
  return v / 6.0;
}

void FlipFaces(ON_Mesh& m) {
  for (int i = 0; i < m.m_F.Count(); i++) {
    ON_MeshFace& f = m.m_F[i];
    if (f.IsTriangle()) { std::swap(f.vi[1], f.vi[2]); f.vi[3] = f.vi[2]; }
    else std::swap(f.vi[1], f.vi[3]);
  }
}

// --- ShrinkWrap -------------------------------------------------------------------------------
// Mesh chiusa che avvolge gli oggetti: campo di distanza su una griglia, riempimento
// dall'esterno (le cavita' interne spariscono), superficie a distanza Offset estratta con
// "surface nets" (quad) e levigata (Taubin, senza restringimento).
bool CmdShrinkWrap(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("ShrinkWrap", "seleziona mesh, superfici, SubD o punti da avvolgere, poi Invio");
  if (sel.empty()) return false;
  std::vector<Tri> tris;
  std::vector<ON_3dPoint> pts;
  ON_3dmObjectAttributes at = Attrs(e, sel[0]);
  for (const auto& id : sel) Triangles(e, Geo(e, id), tris, pts);
  if (tris.empty() && pts.empty()) return e.Fail("ShrinkWrap: niente da avvolgere");
  ON_BoundingBox bb;
  for (const auto& t : tris) { bb.Set(t.a, true); bb.Set(t.b, true); bb.Set(t.c, true); }
  for (const auto& p : pts) bb.Set(p, true);
  const double diag = std::max(bb.Diagonal().Length(), 1e-6);
  double h = OptNum(opts, "targetedgelength", diag / 80.0);
  const double maxCells = 200;
  const double maxSide = std::max({bb.Diagonal().x, bb.Diagonal().y, bb.Diagonal().z});
  if (maxSide / h > maxCells) {
    h = maxSide / maxCells;
    e.Out() << "  TargetEdgeLength aumentata a " << h << " (griglia massima " << int(maxCells) << ")\n";
  }
  // almeno poco piu' di mezza cella: il riempimento dall'esterno non deve passare tra i nodi
  const double offset = std::max(OptNum(opts, "offset", 0.0), 0.55 * h);
  const int smooth = std::clamp((int)OptNum(opts, "smoothingiterations", 3), 0, 100);
  const double pad = offset + 2 * h;
  const ON_3dPoint o = bb.m_min - ON_3dVector(pad, pad, pad);
  const int nx = int(std::ceil((bb.m_max.x - bb.m_min.x + 2 * pad) / h)) + 1;
  const int ny = int(std::ceil((bb.m_max.y - bb.m_min.y + 2 * pad) / h)) + 1;
  const int nz = int(std::ceil((bb.m_max.z - bb.m_min.z + 2 * pad) / h)) + 1;
  auto idx = [&](int i, int j, int k) { return (size_t(k) * ny + j) * nx + i; };
  auto node = [&](int i, int j, int k) { return ON_3dPoint(o.x + i * h, o.y + j * h, o.z + k * h); };
  const float BIG = 1e30f;
  std::vector<float> d(size_t(nx) * ny * nz, BIG);
  const double band = offset + 1.8 * h;
  auto range = [&](double lo, double hi, double org, int n, int& a, int& b) {
    a = std::max(0, int(std::floor((lo - band - org) / h)));
    b = std::min(n - 1, int(std::ceil((hi + band - org) / h)));
  };
  for (const auto& t : tris) {
    ON_BoundingBox tb;
    tb.Set(t.a, false); tb.Set(t.b, true); tb.Set(t.c, true);
    int i0, i1, j0, j1, k0, k1;
    range(tb.m_min.x, tb.m_max.x, o.x, nx, i0, i1);
    range(tb.m_min.y, tb.m_max.y, o.y, ny, j0, j1);
    range(tb.m_min.z, tb.m_max.z, o.z, nz, k0, k1);
    for (int k = k0; k <= k1; k++)
      for (int j = j0; j <= j1; j++)
        for (int i = i0; i <= i1; i++) {
          const ON_3dPoint p = node(i, j, k);
          const float dist = float(p.DistanceTo(ClosestOnTri(p, t.a, t.b, t.c)));
          float& cur = d[idx(i, j, k)];
          if (dist < cur) cur = dist;
        }
  }
  for (const auto& q : pts) {
    int i0, i1, j0, j1, k0, k1;
    range(q.x, q.x, o.x, nx, i0, i1);
    range(q.y, q.y, o.y, ny, j0, j1);
    range(q.z, q.z, o.z, nz, k0, k1);
    for (int k = k0; k <= k1; k++)
      for (int j = j0; j <= j1; j++)
        for (int i = i0; i <= i1; i++) {
          const float dist = float(node(i, j, k).DistanceTo(q));
          float& cur = d[idx(i, j, k)];
          if (dist < cur) cur = dist;
        }
  }
  // esterno: nodi lontani piu' di offset raggiungibili dal bordo della griglia
  std::vector<char> outside(d.size(), 0);
  std::queue<std::array<int, 3>> q;
  for (int k = 0; k < nz; k++)
    for (int j = 0; j < ny; j++)
      for (int i = 0; i < nx; i++)
        if (i == 0 || j == 0 || k == 0 || i == nx - 1 || j == ny - 1 || k == nz - 1) {
          outside[idx(i, j, k)] = 1;
          q.push({i, j, k});
        }
  const int nb[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  while (!q.empty()) {
    auto [i, j, k] = q.front();
    q.pop();
    for (const auto& s : nb) {
      const int a = i + s[0], b = j + s[1], c = k + s[2];
      if (a < 0 || b < 0 || c < 0 || a >= nx || b >= ny || c >= nz) continue;
      const size_t n = idx(a, b, c);
      if (outside[n] || d[n] <= offset) continue;
      outside[n] = 1;
      q.push({a, b, c});
    }
  }
  std::vector<float> f(d.size());
  const float eps = float(1e-4 * h);
  for (size_t n = 0; n < d.size(); n++) {
    f[n] = outside[n] ? float(d[n] - offset) : (d[n] > offset ? float(-0.5 * h) : float(d[n] - offset));
    if (std::fabs(f[n]) < eps) f[n] = outside[n] ? eps : -eps;  // nessun nodo esattamente sulla superficie
  }
  // surface nets: un vertice per cella attraversata
  std::vector<int> cellVertex(size_t(nx - 1) * (ny - 1) * (nz - 1), -1);
  auto cidx = [&](int i, int j, int k) { return (size_t(k) * (ny - 1) + j) * (nx - 1) + i; };
  auto* mesh = new ON_Mesh();
  const int corner[8][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}};
  const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
  for (int k = 0; k + 1 < nz; k++)
    for (int j = 0; j + 1 < ny; j++)
      for (int i = 0; i + 1 < nx; i++) {
        float v[8];
        int neg = 0;
        for (int c = 0; c < 8; c++) {
          v[c] = f[idx(i + corner[c][0], j + corner[c][1], k + corner[c][2])];
          if (v[c] < 0) neg++;
        }
        if (neg == 0 || neg == 8) continue;
        ON_3dPoint sum(0, 0, 0);
        int cnt = 0;
        for (const auto& ed : edges) {
          const float a = v[ed[0]], b = v[ed[1]];
          if ((a < 0) == (b < 0)) continue;
          const double t = a / (a - b);
          const ON_3dPoint pa = node(i + corner[ed[0]][0], j + corner[ed[0]][1], k + corner[ed[0]][2]);
          const ON_3dPoint pb = node(i + corner[ed[1]][0], j + corner[ed[1]][1], k + corner[ed[1]][2]);
          sum += pa + (pb - pa) * t;
          cnt++;
        }
        cellVertex[cidx(i, j, k)] = mesh->m_V.Count();
        mesh->m_V.Append(ON_3fPoint(sum / double(cnt)));
      }
  // un quad per ogni lato della griglia attraversato
  auto quad = [&](int c0, int c1, int c2, int c3, bool flip) {
    if (c0 < 0 || c1 < 0 || c2 < 0 || c3 < 0) return;
    if (flip) mesh->SetQuad(mesh->m_F.Count(), c0, c3, c2, c1);
    else mesh->SetQuad(mesh->m_F.Count(), c0, c1, c2, c3);
  };
  for (int k = 1; k + 1 < nz; k++)
    for (int j = 1; j + 1 < ny; j++)
      for (int i = 1; i + 1 < nx; i++) {
        const float f0 = f[idx(i, j, k)];
        if (i + 1 < nx) {
          const float f1 = f[idx(i + 1, j, k)];
          if ((f0 < 0) != (f1 < 0))
            quad(cellVertex[cidx(i, j - 1, k - 1)], cellVertex[cidx(i, j, k - 1)], cellVertex[cidx(i, j, k)],
                 cellVertex[cidx(i, j - 1, k)], f0 > 0);
        }
        if (j + 1 < ny) {
          const float f1 = f[idx(i, j + 1, k)];
          if ((f0 < 0) != (f1 < 0))
            quad(cellVertex[cidx(i - 1, j, k - 1)], cellVertex[cidx(i - 1, j, k)], cellVertex[cidx(i, j, k)],
                 cellVertex[cidx(i, j, k - 1)], f0 > 0);
        }
        if (k + 1 < nz) {
          const float f1 = f[idx(i, j, k + 1)];
          if ((f0 < 0) != (f1 < 0))
            quad(cellVertex[cidx(i - 1, j - 1, k)], cellVertex[cidx(i, j - 1, k)], cellVertex[cidx(i, j, k)],
                 cellVertex[cidx(i - 1, j, k)], f0 > 0);
        }
      }
  if (mesh->m_F.Count() == 0) { delete mesh; return e.Fail("ShrinkWrap: nessuna superficie ottenuta"); }
  mesh->CullUnusedVertices();
  // levigatura di Taubin (lambda/mu) sui vicini lungo gli spigoli
  if (smooth > 0) {
    const int nv = mesh->m_V.Count();
    std::vector<std::set<int>> nbr(nv);
    for (int fi = 0; fi < mesh->m_F.Count(); fi++) {
      const ON_MeshFace& F = mesh->m_F[fi];
      const int n = F.IsQuad() ? 4 : 3;
      for (int c = 0; c < n; c++) { nbr[F.vi[c]].insert(F.vi[(c + 1) % n]); nbr[F.vi[(c + 1) % n]].insert(F.vi[c]); }
    }
    std::vector<ON_3dPoint> P(nv), Q(nv);
    for (int i = 0; i < nv; i++) P[i] = ON_3dPoint(mesh->m_V[i]);
    for (int it = 0; it < smooth; it++)
      for (double lam : {0.5, -0.53}) {
        for (int i = 0; i < nv; i++) {
          if (nbr[i].empty()) { Q[i] = P[i]; continue; }
          ON_3dPoint c(0, 0, 0);
          for (int n : nbr[i]) c += P[n];
          c = c / double(nbr[i].size());
          Q[i] = P[i] + (c - P[i]) * lam;
        }
        std::swap(P, Q);
      }
    for (int i = 0; i < nv; i++) mesh->m_V[i] = ON_3fPoint(P[i]);
  }
  if (SignedVolume(*mesh) < 0) FlipFaces(*mesh);
  mesh->ComputeVertexNormals();
  mesh->Compact();
  e.AddObject(mesh, &at);
  e.Out() << "  ShrinkWrap: mesh chiusa con " << mesh->m_F.Count() << " facce (griglia " << nx << "x" << ny << "x" << nz
          << ", Offset " << offset << ")\n";
  return true;
}

// --- OffsetMesh -----------------------------------------------------------------------------
bool CmdOffsetMesh(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts, {"solid"});
  auto sel = e.RequireSelection("OffsetMesh", "seleziona le mesh, poi Invio");
  if (sel.empty()) return false;
  double dist;
  if (opts.count("distance")) dist = OptNum(opts, "distance", 1.0);
  else {
    auto dd = e.ReadNumber(ts, "distanza di offset");
    if (!dd) return false;
    dist = *dd;
    for (auto& [k, v] : e.ReadOptions(ts, {"solid"})) opts[k] = v;
  }
  if (std::fabs(dist) < e.Tolerance()) return e.Fail("OffsetMesh: distanza troppo piccola");
  const bool solid = OptBool(opts, "solid", false);
  const bool keep = OptBool(opts, "deleteinput", false) == false;
  int n = 0;
  for (const auto& id : sel) {
    const auto* src = ON_Mesh::Cast(Geo(e, id));
    if (!src) continue;
    ON_Mesh m(*src);
    m.DestroyTopology();
    const ON_MeshTopology& top = m.Topology();
    // normale per vertice topologico (media delle normali delle facce pesata con l'area)
    std::vector<ON_3dVector> tn(top.m_topv.Count(), ON_3dVector::ZeroVector);
    for (int fi = 0; fi < m.m_F.Count(); fi++) {
      const ON_MeshFace& F = m.m_F[fi];
      const ON_3dPoint a = m.Vertex(F.vi[0]), b = m.Vertex(F.vi[1]), c = m.Vertex(F.vi[2]), dd = m.Vertex(F.vi[3]);
      const ON_3dVector fn = ON_CrossProduct(c - a, dd - b);  // diagonali: area e verso per tri e quad
      const int nc = F.IsQuad() ? 4 : 3;
      for (int k = 0; k < nc; k++) tn[top.m_topv_map[F.vi[k]]] += fn;
    }
    for (auto& v : tn) v.Unitize();
    // distanza costante dai piani delle facce (spigoli e angoli vivi): si allunga lo spostamento
    std::vector<double> minDot(tn.size(), 1.0);
    for (int fi = 0; fi < m.m_F.Count(); fi++) {
      const ON_MeshFace& F = m.m_F[fi];
      ON_3dVector fn = ON_CrossProduct(m.Vertex(F.vi[2]) - m.Vertex(F.vi[0]), m.Vertex(F.vi[3]) - m.Vertex(F.vi[1]));
      if (!fn.Unitize()) continue;
      const int nc = F.IsQuad() ? 4 : 3;
      for (int k = 0; k < nc; k++) {
        const int t = top.m_topv_map[F.vi[k]];
        minDot[t] = std::min(minDot[t], tn[t] * fn);
      }
    }
    for (size_t i = 0; i < tn.size(); i++) tn[i] = tn[i] / std::clamp(minDot[i], 0.25, 1.0);
    ON_Mesh off(m);
    off.m_N.Destroy();
    off.m_FN.Destroy();
    for (int i = 0; i < off.m_V.Count(); i++)
      off.m_V[i] = ON_3fPoint(ON_3dPoint(m.m_V[i]) + tn[top.m_topv_map[i]] * dist);
    ON_Mesh* out = nullptr;
    if (!solid) {
      out = new ON_Mesh(off);
    } else {
      out = new ON_Mesh();
      const int nv = m.m_V.Count();
      for (int i = 0; i < nv; i++) out->m_V.Append(m.m_V[i]);
      for (int i = 0; i < nv; i++) out->m_V.Append(off.m_V[i]);
      for (int fi = 0; fi < m.m_F.Count(); fi++) {
        const ON_MeshFace& F = m.m_F[fi];
        // originale rovesciata, offset con il verso originale
        if (F.IsQuad()) {
          out->SetQuad(out->m_F.Count(), F.vi[0], F.vi[3], F.vi[2], F.vi[1]);
          out->SetQuad(out->m_F.Count(), F.vi[0] + nv, F.vi[1] + nv, F.vi[2] + nv, F.vi[3] + nv);
        } else {
          out->SetTriangle(out->m_F.Count(), F.vi[0], F.vi[2], F.vi[1]);
          out->SetTriangle(out->m_F.Count(), F.vi[0] + nv, F.vi[1] + nv, F.vi[2] + nv);
        }
      }
      // pareti lungo i bordi nudi: lo spigolo a -> b come lo percorre la sua faccia
      for (int ei = 0; ei < top.m_tope.Count(); ei++) {
        const ON_MeshTopologyEdge& te = top.m_tope[ei];
        if (te.m_topf_count != 1) continue;
        const int fi = te.m_topfi[0];
        const ON_MeshFace& F = m.m_F[fi];
        const int nc = F.IsQuad() ? 4 : 3;
        for (int k = 0; k < nc; k++) {
          const int a = F.vi[k], b = F.vi[(k + 1) % nc];
          const int ta = top.m_topv_map[a], tb = top.m_topv_map[b];
          if ((ta == te.m_topvi[0] && tb == te.m_topvi[1]) || (ta == te.m_topvi[1] && tb == te.m_topvi[0])) {
            out->SetQuad(out->m_F.Count(), a, b, b + nv, a + nv);
            break;
          }
        }
      }
      if (SignedVolume(*out) < 0) FlipFaces(*out);
    }
    out->CombineIdenticalVertices(true, true);
    out->CullUnusedVertices();
    out->ComputeVertexNormals();
    out->Compact();
    const ON_3dmObjectAttributes at = Attrs(e, id);
    e.AddObject(out, &at);
    if (!keep) e.DeleteObject(id);
    n++;
  }
  if (!n) return e.Fail("OffsetMesh: nessuna mesh selezionata");
  e.Out() << "  Offset di " << n << " mesh" << (solid ? " (solide)" : "") << "\n";
  return true;
}

// --- riparazione delle mesh ---------------------------------------------------------------------
struct MeshReport {
  int faces = 0, vertices = 0, degenerate = 0, duplicate = 0, naked = 0, nonManifold = 0, pieces = 0, unused = 0;
};

int CountDegenerate(const ON_Mesh& m) {
  int n = 0;
  for (int i = 0; i < m.m_F.Count(); i++) {
    const ON_MeshFace& f = m.m_F[i];
    const ON_3dPoint a = m.Vertex(f.vi[0]), b = m.Vertex(f.vi[1]), c = m.Vertex(f.vi[2]);
    const double area = ON_CrossProduct(b - a, c - a).Length();
    const bool sameIdx = f.vi[0] == f.vi[1] || f.vi[1] == f.vi[2] || f.vi[0] == f.vi[2];
    if (sameIdx || area <= 1e-14 * std::max(1.0, (b - a).LengthSquared())) n++;
  }
  return n;
}

std::vector<int> DuplicateFaces(const ON_Mesh& m, const ON_MeshTopology& top) {
  std::map<std::vector<int>, int> seen;
  std::vector<int> dup;
  for (int i = 0; i < m.m_F.Count(); i++) {
    const ON_MeshFace& f = m.m_F[i];
    std::vector<int> key;
    for (int k = 0; k < (f.IsQuad() ? 4 : 3); k++) key.push_back(top.m_topv_map[f.vi[k]]);
    std::sort(key.begin(), key.end());
    if (!seen.emplace(key, i).second) dup.push_back(i);
  }
  return dup;
}

MeshReport Inspect(const ON_Mesh& src) {
  MeshReport r;
  ON_Mesh m(src);
  r.faces = m.m_F.Count();
  r.vertices = m.m_V.Count();
  r.degenerate = CountDegenerate(m);
  const ON_MeshTopology& top = m.Topology();
  r.duplicate = (int)DuplicateFaces(m, top).size();
  for (int ei = 0; ei < top.m_tope.Count(); ei++) {
    if (top.m_tope[ei].m_topf_count == 1) r.naked++;
    else if (top.m_tope[ei].m_topf_count > 2) r.nonManifold++;
  }
  std::vector<char> used(m.m_V.Count(), 0);
  for (int i = 0; i < m.m_F.Count(); i++)
    for (int k = 0; k < 4; k++) if (m.m_F[i].vi[k] >= 0 && m.m_F[i].vi[k] < m.m_V.Count()) used[m.m_F[i].vi[k]] = 1;
  for (char u : used) if (!u) r.unused++;
  ON_SimpleArray<int> ids;
  r.pieces = m.GetConnectedComponents(false, true, ids);
  return r;
}

void PrintReport(Engine& e, const MeshReport& r) {
  e.Out() << "    facce " << r.faces << ", vertici " << r.vertices << ", parti " << r.pieces << "\n";
  e.Out() << "    facce degeneri " << r.degenerate << ", facce doppie " << r.duplicate << ", vertici non usati " << r.unused
          << ", spigoli nudi " << r.naked << ", spigoli non-manifold " << r.nonManifold << "\n";
}

// Toglie facce degeneri e doppie e vertici non usati, ricalcola le normali.
int RepairMesh(ON_Mesh& m) {
  int fixed = 0;
  const int before = m.m_F.Count();
  // facce con indici ripetuti (triangoli scritti come quad degeneri compresi)
  m.CullDegenerateFaces();
  // facce di area nulla
  for (int i = m.m_F.Count() - 1; i >= 0; i--) {
    const ON_MeshFace& f = m.m_F[i];
    const ON_3dPoint a = m.Vertex(f.vi[0]), b = m.Vertex(f.vi[1]), c = m.Vertex(f.vi[2]);
    double area = ON_CrossProduct(b - a, c - a).Length();
    if (f.IsQuad()) area += ON_CrossProduct(c - a, m.Vertex(f.vi[3]) - a).Length();
    if (area <= 1e-14 * std::max(1.0, (b - a).LengthSquared())) m.DeleteFace(i);
  }
  m.DestroyTopology();
  {
    const ON_MeshTopology& top = m.Topology();
    auto dup = DuplicateFaces(m, top);
    std::sort(dup.rbegin(), dup.rend());
    m.DestroyTopology();
    for (int i : dup) m.DeleteFace(i);
  }
  fixed += before - m.m_F.Count();
  const int vb = m.m_V.Count();
  m.CullUnusedVertices();
  fixed += vb - m.m_V.Count();
  m.m_N.Destroy();
  m.m_FN.Destroy();
  m.ComputeVertexNormals();
  m.Compact();
  return fixed;
}

bool MeshFix(Engine& e, const char* name, bool reportOnly, bool normalsOnly) {
  auto sel = e.RequireSelection(name, "seleziona le mesh, poi Invio");
  if (sel.empty()) return false;
  int n = 0, fixedTotal = 0;
  for (const auto& id : sel) {
    const auto* src = ON_Mesh::Cast(Geo(e, id));
    if (!src) continue;
    n++;
    if (reportOnly) {
      e.Out() << "  Mesh " << n << ":\n";
      PrintReport(e, Inspect(*src));
      continue;
    }
    auto* m = new ON_Mesh(*src);
    if (normalsOnly) {
      m->m_N.Destroy();
      m->m_FN.Destroy();
      m->ComputeVertexNormals();
    } else {
      fixedTotal += RepairMesh(*m);
    }
    e.ReplaceObject(id, m);
  }
  if (!n) return e.Fail("nessuna mesh selezionata");
  if (!reportOnly && !normalsOnly) e.Out() << "  Elementi tolti: " << fixedTotal << "\n";
  if (normalsOnly) e.Out() << "  Normali ricalcolate su " << n << " mesh\n";
  return true;
}

// MeshRepair: controllo e riparazione (Repair=No solo controllo). Per orientare le facce e
// chiudere i fori restano UnifyMeshNormals e FillMeshHoles, come i passi della procedura di Rhino.
bool CmdMeshRepair(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  const bool repair = OptBool(opts, "repair", true);
  auto sel = e.RequireSelection("MeshRepair", "seleziona le mesh, poi Invio");
  if (sel.empty()) return false;
  int n = 0;
  for (const auto& id : sel) {
    const auto* src = ON_Mesh::Cast(Geo(e, id));
    if (!src) continue;
    n++;
    const MeshReport before = Inspect(*src);
    e.Out() << "  Mesh " << n << (repair ? " prima:\n" : ":\n");
    PrintReport(e, before);
    if (!repair) continue;
    auto* m = new ON_Mesh(*src);
    RepairMesh(*m);
    const MeshReport after = Inspect(*m);
    e.ReplaceObject(id, m);
    e.Out() << "  Mesh " << n << " dopo:\n";
    PrintReport(e, after);
    if (after.naked) e.Out() << "    restano spigoli nudi: FillMeshHoles per chiudere i fori\n";
  }
  if (!n) return e.Fail("nessuna mesh selezionata");
  return true;
}

bool CmdCullDegenerateMeshFaces(Engine& e, TokenStream&) { return MeshFix(e, "CullDegenerateMeshFaces", false, false); }
bool CmdRebuildMeshNormals(Engine& e, TokenStream&) { return MeshFix(e, "RebuildMeshNormals", false, true); }
bool CmdCheckMesh(Engine& e, TokenStream&) { return MeshFix(e, "CheckMesh", true, false); }

// --- ExtractRenderMesh -----------------------------------------------------------------------
// Mesh di rendering degli oggetti come oggetti mesh: quella salvata nel file (Rhino) se c'e',
// altrimenti calcolata come quella della vista.
bool CmdExtractRenderMesh(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("ExtractRenderMesh", "seleziona superfici, polisuperfici, estrusioni o SubD, poi Invio");
  if (sel.empty()) return false;
  int n = 0, cached = 0;
  for (const auto& id : sel) {
    const ON_Geometry* g = Geo(e, id);
    if (!g) continue;
    ON_Mesh* m = nullptr;
    if (const auto* mm = ON_Mesh::Cast(g)) {
      if (!HasMeshModifiers(e, id)) continue;  // una mesh senza modificatori e' gia' la sua mesh di rendering
      m = new ON_Mesh(*mm);
    } else if (const auto* b = ON_Brep::Cast(g)) {
      bool all = b->m_F.Count() > 0;
      for (int i = 0; i < b->m_F.Count() && all; i++) all = b->m_F[i].Mesh(ON::render_mesh) != nullptr;
      if (all) {
        m = new ON_Mesh();
        for (int i = 0; i < b->m_F.Count(); i++) m->Append(*b->m_F[i].Mesh(ON::render_mesh));
        cached++;
      }
    } else if (const auto* x = ON_Extrusion::Cast(g)) {
      if (const ON_Mesh* xm = x->Mesh(ON::render_mesh)) { m = new ON_Mesh(*xm); cached++; }
    }
    if (!m) {
      if (const auto* sd = ON_SubD::Cast(g)) {
        auto d = BuildSubDLimit(*sd, 3);
        if (d.mesh) m = d.mesh.release();
      } else if (ON_Brep::Cast(g) || ON_Extrusion::Cast(g) || ON_Surface::Cast(g)) {
        ON_BoundingBox bb;
        g->GetTightBoundingBox(bb);
        m = occt::MeshShape(occt::GeometryToShape(*g, e.Tolerance()), std::max(bb.Diagonal().Length() / 400.0, 0.01),
                            15.0 * ON_PI / 180.0, true);
      }
    }
    if (!m) continue;
    if (!m->HasVertexNormals()) m->ComputeVertexNormals();
    ApplyMeshModifiers(e, id, *m);  // Displacement, EdgeSoftening, Shutlining
    const ON_3dmObjectAttributes at = Attrs(e, id);
    e.AddObject(m, &at);
    n++;
  }
  if (!n) return e.Fail("ExtractRenderMesh: nessun oggetto con mesh di rendering");
  e.Out() << "  Estratte " << n << " mesh di rendering" << (cached ? " (" + std::to_string(cached) + " dal file)" : std::string()) << "\n";
  return true;
}

}  // namespace

void RegisterModelingCommands(Engine& e) {
  auto reg = [&](const char* n, const char* cat, const char* d, CommandFn f, bool mod = true) {
    CommandInfo i{n, cat, d, f};
    i.modifiesModel = mod;
    e.Register(i);
  };
  reg("MergeSrf", "Superfici", "Unisce due superfici non tagliate lungo il bordo comune (Smooth=Yes|No)", CmdMergeSrf);
  reg("SplitFace", "Solidi", "Divide le facce di una polisuperficie con curve o un'isocurva (Isocurve, Direction=U|V)", CmdSplitFace);
  reg("ShrinkWrap", "Mesh", "Mesh chiusa che avvolge gli oggetti (TargetEdgeLength=, Offset=, SmoothingIterations=)", CmdShrinkWrap);
  reg("OffsetMesh", "Mesh", "Offset delle mesh lungo le normali (distanza; Solid=Yes per lo spessore)", CmdOffsetMesh);
  reg("MeshRepair", "Mesh", "Controlla e ripara le mesh (facce degeneri e doppie, vertici non usati; Repair=No solo controllo)", CmdMeshRepair);
  reg("CheckMesh", "Mesh", "Rapporto sullo stato delle mesh", CmdCheckMesh, false);
  reg("CullDegenerateMeshFaces", "Mesh", "Toglie le facce degeneri e doppie delle mesh", CmdCullDegenerateMeshFaces);
  reg("RebuildMeshNormals", "Mesh", "Ricalcola le normali dei vertici delle mesh", CmdRebuildMeshNormals);
  reg("ExtractRenderMesh", "Mesh", "Copia la mesh di rendering degli oggetti come oggetto mesh", CmdExtractRenderMesh);
}

}  // namespace cad
