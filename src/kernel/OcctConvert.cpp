#include <Geom_Surface.hxx>
#include <Precision.hxx>
#include "kernel/OcctConvert.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_NurbsConvert.hxx>
#include <BRepLib.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Geom2dConvert.hxx>
#include <Geom2d_TrimmedCurve.hxx>
#include <GeomConvert.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <Poly_Triangulation.hxx>
#include <ShapeFix_Shape.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColStd_Array2OfReal.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TColgp_Array1OfPnt2d.hxx>
#include <TColgp_Array2OfPnt.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <cmath>
#include <map>
#include <tuple>
#include <vector>

namespace cad::occt {
namespace {

// Vettore di nodi "completo" (con i nodi superflui alle estremita') in
// nodi distinti + molteplicita', come li vuole OpenCASCADE.
void SplitKnots(const double* onKnots, int order, int cvCount,
                std::vector<double>& knots, std::vector<int>& mults) {
  const int n = cvCount + order;  // lunghezza completa
  std::vector<double> full(n);
  full[0] = onKnots[0];
  for (int i = 0; i < n - 2; i++) full[i + 1] = onKnots[i];
  full[n - 1] = onKnots[n - 3];
  knots.clear(); mults.clear();
  for (double k : full) {
    if (!knots.empty() && std::fabs(k - knots.back()) <= ON_ZERO_TOLERANCE * (1 + std::fabs(k))) mults.back()++;
    else { knots.push_back(k); mults.push_back(1); }
  }
}

// Ogni curva NURBS openNURBS (anche periodica) resa "clamped".
ON_NurbsCurve Clamped(const ON_NurbsCurve& c) {
  ON_NurbsCurve n(c);
  if (!n.IsClamped(2)) n.ClampEnd(2);
  return n;
}

template <class TKnots, class TMults>
void FillKnots(const std::vector<double>& k, const std::vector<int>& m, TKnots& K, TMults& M) {
  for (size_t i = 0; i < k.size(); i++) { K.SetValue(static_cast<int>(i) + 1, k[i]); M.SetValue(static_cast<int>(i) + 1, m[i]); }
}

void FlatKnots(const TColStd_Array1OfReal& knots, const TColStd_Array1OfInteger& mults, std::vector<double>& flat) {
  flat.clear();
  for (int i = knots.Lower(); i <= knots.Upper(); i++)
    for (int j = 0; j < mults(i); j++) flat.push_back(knots(i));
}

}  // namespace

Handle(Geom_BSplineCurve) ToOcct(const ON_NurbsCurve& src) {
  ON_NurbsCurve c = Clamped(src);
  std::vector<double> k; std::vector<int> m;
  SplitKnots(c.m_knot, c.m_order, c.m_cv_count, k, m);
  TColgp_Array1OfPnt poles(1, c.m_cv_count);
  TColStd_Array1OfReal weights(1, c.m_cv_count);
  for (int i = 0; i < c.m_cv_count; i++) {
    ON_3dPoint p; c.GetCV(i, p);
    poles.SetValue(i + 1, gp_Pnt(p.x, p.y, p.z));
    weights.SetValue(i + 1, c.Weight(i));
  }
  TColStd_Array1OfReal K(1, static_cast<int>(k.size()));
  TColStd_Array1OfInteger M(1, static_cast<int>(m.size()));
  FillKnots(k, m, K, M);
  if (c.IsRational()) return new Geom_BSplineCurve(poles, weights, K, M, c.Degree());
  return new Geom_BSplineCurve(poles, K, M, c.Degree());
}

Handle(Geom2d_BSplineCurve) ToOcct2d(const ON_NurbsCurve& src) {
  ON_NurbsCurve c = Clamped(src);
  std::vector<double> k; std::vector<int> m;
  SplitKnots(c.m_knot, c.m_order, c.m_cv_count, k, m);
  TColgp_Array1OfPnt2d poles(1, c.m_cv_count);
  TColStd_Array1OfReal weights(1, c.m_cv_count);
  for (int i = 0; i < c.m_cv_count; i++) {
    ON_3dPoint p; c.GetCV(i, p);
    poles.SetValue(i + 1, gp_Pnt2d(p.x, p.y));
    weights.SetValue(i + 1, c.Weight(i));
  }
  TColStd_Array1OfReal K(1, static_cast<int>(k.size()));
  TColStd_Array1OfInteger M(1, static_cast<int>(m.size()));
  FillKnots(k, m, K, M);
  if (c.IsRational()) return new Geom2d_BSplineCurve(poles, weights, K, M, c.Degree());
  return new Geom2d_BSplineCurve(poles, K, M, c.Degree());
}

Handle(Geom_BSplineSurface) ToOcct(const ON_NurbsSurface& src) {
  ON_NurbsSurface s(src);
  for (int dir = 0; dir < 2; dir++)
    if (!s.IsClamped(dir, 2)) s.ClampEnd(dir, 2);
  std::vector<double> ku, kv; std::vector<int> mu, mv;
  SplitKnots(s.m_knot[0], s.m_order[0], s.m_cv_count[0], ku, mu);
  SplitKnots(s.m_knot[1], s.m_order[1], s.m_cv_count[1], kv, mv);
  const int nu = s.m_cv_count[0], nv = s.m_cv_count[1];
  TColgp_Array2OfPnt poles(1, nu, 1, nv);
  TColStd_Array2OfReal weights(1, nu, 1, nv);
  for (int i = 0; i < nu; i++)
    for (int j = 0; j < nv; j++) {
      ON_3dPoint p; s.GetCV(i, j, p);
      poles.SetValue(i + 1, j + 1, gp_Pnt(p.x, p.y, p.z));
      weights.SetValue(i + 1, j + 1, s.Weight(i, j));
    }
  TColStd_Array1OfReal KU(1, static_cast<int>(ku.size())), KV(1, static_cast<int>(kv.size()));
  TColStd_Array1OfInteger MU(1, static_cast<int>(mu.size())), MV(1, static_cast<int>(mv.size()));
  FillKnots(ku, mu, KU, MU);
  FillKnots(kv, mv, KV, MV);
  if (s.IsRational())
    return new Geom_BSplineSurface(poles, weights, KU, KV, MU, MV, s.Degree(0), s.Degree(1));
  return new Geom_BSplineSurface(poles, KU, KV, MU, MV, s.Degree(0), s.Degree(1));
}

ON_NurbsCurve* FromOcct(const Handle(Geom_BSplineCurve)& in) {
  Handle(Geom_BSplineCurve) c = Handle(Geom_BSplineCurve)::DownCast(in->Copy());
  if (c->IsPeriodic()) c->SetNotPeriodic();
  const int n = c->NbPoles(), deg = c->Degree();
  const bool rat = c->IsRational();
  auto* out = new ON_NurbsCurve(3, rat, deg + 1, n);
  for (int i = 1; i <= n; i++) {
    gp_Pnt p = c->Pole(i);
    const double w = rat ? c->Weight(i) : 1.0;
    if (rat) out->SetCV(i - 1, ON_4dPoint(p.X() * w, p.Y() * w, p.Z() * w, w));
    else out->SetCV(i - 1, ON_3dPoint(p.X(), p.Y(), p.Z()));
  }
  std::vector<double> flat;
  TColStd_Array1OfReal K(1, c->NbKnots()); c->Knots(K);
  TColStd_Array1OfInteger M(1, c->NbKnots()); c->Multiplicities(M);
  FlatKnots(K, M, flat);
  for (int i = 1; i + 1 < static_cast<int>(flat.size()); i++) out->m_knot[i - 1] = flat[i];
  return out;
}

ON_NurbsCurve* FromOcct2d(const Handle(Geom2d_BSplineCurve)& in) {
  Handle(Geom2d_BSplineCurve) c = Handle(Geom2d_BSplineCurve)::DownCast(in->Copy());
  if (c->IsPeriodic()) c->SetNotPeriodic();
  const int n = c->NbPoles(), deg = c->Degree();
  const bool rat = c->IsRational();
  auto* out = new ON_NurbsCurve(2, rat, deg + 1, n);
  for (int i = 1; i <= n; i++) {
    gp_Pnt2d p = c->Pole(i);
    const double w = rat ? c->Weight(i) : 1.0;
    double* cv = out->CV(i - 1);
    cv[0] = p.X() * w; cv[1] = p.Y() * w;
    if (rat) cv[2] = w;
  }
  std::vector<double> flat;
  TColStd_Array1OfReal K(1, c->NbKnots()); c->Knots(K);
  TColStd_Array1OfInteger M(1, c->NbKnots()); c->Multiplicities(M);
  FlatKnots(K, M, flat);
  for (int i = 1; i + 1 < static_cast<int>(flat.size()); i++) out->m_knot[i - 1] = flat[i];
  return out;
}

ON_NurbsSurface* FromOcct(const Handle(Geom_BSplineSurface)& in) {
  Handle(Geom_BSplineSurface) s = Handle(Geom_BSplineSurface)::DownCast(in->Copy());
  if (s->IsUPeriodic()) s->SetUNotPeriodic();
  if (s->IsVPeriodic()) s->SetVNotPeriodic();
  const int nu = s->NbUPoles(), nv = s->NbVPoles();
  const bool rat = s->IsURational() || s->IsVRational();
  auto* out = new ON_NurbsSurface(3, rat, s->UDegree() + 1, s->VDegree() + 1, nu, nv);
  for (int i = 1; i <= nu; i++)
    for (int j = 1; j <= nv; j++) {
      gp_Pnt p = s->Pole(i, j);
      const double w = rat ? s->Weight(i, j) : 1.0;
      if (rat) out->SetCV(i - 1, j - 1, ON_4dPoint(p.X() * w, p.Y() * w, p.Z() * w, w));
      else out->SetCV(i - 1, j - 1, ON_3dPoint(p.X(), p.Y(), p.Z()));
    }
  std::vector<double> flat;
  TColStd_Array1OfReal KU(1, s->NbUKnots()); s->UKnots(KU);
  TColStd_Array1OfInteger MU(1, s->NbUKnots()); s->UMultiplicities(MU);
  FlatKnots(KU, MU, flat);
  for (int i = 1; i + 1 < static_cast<int>(flat.size()); i++) out->m_knot[0][i - 1] = flat[i];
  TColStd_Array1OfReal KV(1, s->NbVKnots()); s->VKnots(KV);
  TColStd_Array1OfInteger MV(1, s->NbVKnots()); s->VMultiplicities(MV);
  FlatKnots(KV, MV, flat);
  for (int i = 1; i + 1 < static_cast<int>(flat.size()); i++) out->m_knot[1][i - 1] = flat[i];
  return out;
}

TopoDS_Shape CurveToShape(const ON_Curve& c) {
  ON_NurbsCurve nc;
  if (!c.GetNurbForm(nc)) return {};
  Handle(Geom_BSplineCurve) g = ToOcct(nc);
  BRepBuilderAPI_MakeEdge me(g);
  return me.IsDone() ? TopoDS_Shape(me.Edge()) : TopoDS_Shape();
}

TopoDS_Shape BrepToShape(const ON_Brep& b, double tol) {
  BRep_Builder bb;
  std::vector<TopoDS_Vertex> verts(b.m_V.Count());
  for (int i = 0; i < b.m_V.Count(); i++) {
    const ON_3dPoint& p = b.m_V[i].point;
    bb.MakeVertex(verts[i], gp_Pnt(p.x, p.y, p.z), std::max(tol, b.m_V[i].m_tolerance > 0 && b.m_V[i].m_tolerance != ON_UNSET_VALUE ? b.m_V[i].m_tolerance : tol));
  }
  std::vector<TopoDS_Edge> edges(b.m_E.Count());
  for (int i = 0; i < b.m_E.Count(); i++) {
    const ON_BrepEdge& e = b.m_E[i];
    ON_NurbsCurve nc;
    if (!e.GetNurbForm(nc)) continue;
    Handle(Geom_BSplineCurve) g = ToOcct(nc);
    bb.MakeEdge(edges[i], g, tol);
    TopoDS_Vertex v0 = verts[e.m_vi[0]], v1 = verts[e.m_vi[1]];
    v0.Orientation(TopAbs_FORWARD);
    v1.Orientation(TopAbs_REVERSED);
    bb.Add(edges[i], v0);
    bb.Add(edges[i], v1);
    bb.Range(edges[i], g->FirstParameter(), g->LastParameter());
  }
  TopoDS_Shell shell;
  bb.MakeShell(shell);
  int faceCount = 0;
  for (int fi = 0; fi < b.m_F.Count(); fi++) {
    const ON_BrepFace& f = b.m_F[fi];
    const ON_Surface* srf = f.SurfaceOf();
    ON_NurbsSurface ns;
    if (!srf || !srf->GetNurbForm(ns)) continue;
    Handle(Geom_BSplineSurface) gs = ToOcct(ns);
    TopoDS_Face face;
    bb.MakeFace(face, gs, tol);
    // pcurve per edge in questa faccia (piu' d'una per le cuciture)
    struct PC { Handle(Geom2d_BSplineCurve) c; bool rev; };
    std::map<int, std::vector<PC>> pcurves;
    std::vector<std::pair<TopoDS_Wire, std::vector<std::pair<int, bool>>>> wires;
    for (int li = 0; li < f.m_li.Count(); li++) {
      const ON_BrepLoop& loop = b.m_L[f.m_li[li]];
      TopoDS_Wire w;
      bb.MakeWire(w);
      for (int ti = 0; ti < loop.m_ti.Count(); ti++) {
        const ON_BrepTrim& t = b.m_T[loop.m_ti[ti]];
        ON_NurbsCurve c2;
        if (!t.GetNurbForm(c2)) continue;
        if (t.m_ei < 0 || t.m_type == ON_BrepTrim::singular) {
          // edge degenere (polo di sfera, apice di cono)
          TopoDS_Edge de;
          bb.MakeEdge(de);
          Handle(Geom2d_BSplineCurve) pc = ToOcct2d(c2);
          bb.UpdateEdge(de, pc, face, tol);
          bb.Range(de, pc->FirstParameter(), pc->LastParameter());
          bb.Degenerated(de, true);
          TopoDS_Vertex v = verts[t.m_vi[0]];
          v.Orientation(TopAbs_FORWARD); bb.Add(de, v);
          v.Orientation(TopAbs_REVERSED); bb.Add(de, v);
          bb.Add(w, de);
          continue;
        }
        const ON_BrepEdge& e = b.m_E[t.m_ei];
        if (t.m_bRev3d) c2.Reverse();
        c2.SetDomain(e.Domain()[0], e.Domain()[1]);
        pcurves[t.m_ei].push_back({ToOcct2d(c2), t.m_bRev3d});
        TopoDS_Edge oe = edges[t.m_ei];
        oe.Orientation(t.m_bRev3d ? TopAbs_REVERSED : TopAbs_FORWARD);
        bb.Add(w, oe);
      }
      bb.Add(face, w);
    }
    for (auto& [ei, list] : pcurves) {
      TopoDS_Edge& oe = edges[ei];
      if (list.size() >= 2) {
        // cucitura: la prima pcurve per l'orientamento FORWARD
        const PC& fwd = !list[0].rev ? list[0] : list[1];
        const PC& rev = !list[0].rev ? list[1] : list[0];
        bb.UpdateEdge(oe, fwd.c, rev.c, face, tol);
      } else {
        bb.UpdateEdge(oe, list[0].c, face, tol);
      }
      bb.SameParameter(oe, false);
    }
    if (f.m_bRev) face.Reverse();
    bb.Add(shell, face);
    faceCount++;
  }
  if (!faceCount) return {};
  TopoDS_Shape result = shell;
  BRepLib::SameParameter(result, tol, true);
  if (b.IsSolid()) {
    TopoDS_Solid solid;
    bb.MakeSolid(solid);
    shell.Closed(true);
    bb.Add(solid, shell);
    BRepLib::OrientClosedSolid(solid);
    result = solid;
  }
  ShapeFix_Shape fix(result);
  fix.SetPrecision(tol);
  fix.Perform();
  return fix.Shape();
}

TopoDS_Shape GeometryToShape(const ON_Geometry& g, double tol) {
  if (const auto* c = ON_Curve::Cast(&g)) return CurveToShape(*c);
  if (const auto* b = ON_Brep::Cast(&g)) return BrepToShape(*b, tol);
  std::unique_ptr<ON_Brep> owned;
  if (const auto* x = ON_Extrusion::Cast(&g)) owned.reset(x->BrepForm());
  else if (const auto* s = ON_Surface::Cast(&g)) owned.reset(s->BrepForm());
  return owned ? BrepToShape(*owned, tol) : TopoDS_Shape();
}

ON_Brep* ShapeToBrep(const TopoDS_Shape& in, double tol) {
  if (in.IsNull()) return nullptr;
  BRepBuilderAPI_NurbsConvert conv(in, Standard_True);
  if (!conv.IsDone()) return nullptr;
  TopoDS_Shape shape = conv.Shape();
  auto* b = ON_Brep::New();
  TopTools_IndexedMapOfShape vmap, emap;
  TopExp::MapShapes(shape, TopAbs_VERTEX, vmap);
  TopExp::MapShapes(shape, TopAbs_EDGE, emap);
  for (int i = 1; i <= vmap.Extent(); i++) {
    const TopoDS_Vertex& v = TopoDS::Vertex(vmap(i));
    gp_Pnt p = BRep_Tool::Pnt(v);
    b->NewVertex(ON_3dPoint(p.X(), p.Y(), p.Z()), std::max(tol, BRep_Tool::Tolerance(v)));
  }
  std::vector<int> edgeIndex(emap.Extent() + 1, -1);
  for (int i = 1; i <= emap.Extent(); i++) {
    const TopoDS_Edge& e = TopoDS::Edge(emap(i));
    if (BRep_Tool::Degenerated(e)) continue;
    double f, l;
    Handle(Geom_Curve) c = BRep_Tool::Curve(e, f, l);
    if (c.IsNull()) continue;
    Handle(Geom_BSplineCurve) bs = GeomConvert::CurveToBSplineCurve(new Geom_TrimmedCurve(c, f, l));
    ON_NurbsCurve* nc = FromOcct(bs);
    const int c3i = b->AddEdgeCurve(nc);
    TopoDS_Vertex v0, v1;
    TopExp::Vertices(e, v0, v1);  // rispetto all'edge FORWARD
    ON_BrepEdge& be = b->NewEdge(b->m_V[vmap.FindIndex(v0) - 1], b->m_V[vmap.FindIndex(v1) - 1], c3i, nullptr,
                                 std::max(tol, BRep_Tool::Tolerance(e)));
    edgeIndex[i] = be.m_edge_index;
  }
  for (TopExp_Explorer fx(shape, TopAbs_FACE); fx.More(); fx.Next()) {
    const TopoDS_Face& face = TopoDS::Face(fx.Current());
    TopoDS_Face fwd = TopoDS::Face(face.Oriented(TopAbs_FORWARD));
    Handle(Geom_Surface) gs = BRep_Tool::Surface(fwd);
    Handle(Geom_BSplineSurface) bs = Handle(Geom_BSplineSurface)::DownCast(gs);
    if (bs.IsNull()) bs = GeomConvert::SurfaceToBSplineSurface(gs);
    if (bs.IsNull()) continue;
    const int si = b->AddSurface(FromOcct(bs));
    ON_BrepFace& bf = b->NewFace(si);
    bf.m_bRev = (face.Orientation() == TopAbs_REVERSED);
    const TopoDS_Wire outer = BRepTools::OuterWire(fwd);
    for (TopExp_Explorer wx(fwd, TopAbs_WIRE); wx.More(); wx.Next()) {
      const TopoDS_Wire& w = TopoDS::Wire(wx.Current());
      ON_BrepLoop& loop = b->NewLoop(w.IsSame(outer) ? ON_BrepLoop::outer : ON_BrepLoop::inner, b->m_F[bf.m_face_index]);
      const int loopIndex = loop.m_loop_index;
      for (BRepTools_WireExplorer we(w, fwd); we.More(); we.Next()) {
        const TopoDS_Edge& e = we.Current();
        double f, l;
        Handle(Geom2d_Curve) pc = BRep_Tool::CurveOnSurface(e, fwd, f, l);
        if (pc.IsNull()) continue;
        Handle(Geom2d_BSplineCurve) pbs = Geom2dConvert::CurveToBSplineCurve(new Geom2d_TrimmedCurve(pc, f, l));
        ON_NurbsCurve* c2 = FromOcct2d(pbs);
        const bool rev = (e.Orientation() == TopAbs_REVERSED);
        if (rev) c2->Reverse();
        const int c2i = b->AddTrimCurve(c2);
        const int ei = edgeIndex[emap.FindIndex(e)];
        if (ei < 0) {
          // edge degenere -> trim singolare (il flag iso lo calcola SetTrimIsoFlags)
          TopoDS_Vertex v = TopExp::FirstVertex(e);
          b->NewSingularTrim(b->m_V[vmap.FindIndex(v) - 1], b->m_L[loopIndex], ON_Surface::not_iso, c2i);
        } else {
          ON_BrepTrim& t = b->NewTrim(b->m_E[ei], rev, b->m_L[loopIndex], c2i);
          t.m_tolerance[0] = t.m_tolerance[1] = 0.0;
        }
      }
    }
  }
  b->SetTrimIsoFlags();
  b->SetTrimTypeFlags();
  b->SetTrimBoundingBoxes(false);
  if (b->m_F.Count() == 0) { delete b; return nullptr; }
  return b;
}

std::vector<ON_Curve*> ShapeToCurves(const TopoDS_Shape& shape, double) {
  std::vector<ON_Curve*> out;
  for (TopExp_Explorer ex(shape, TopAbs_EDGE); ex.More(); ex.Next()) {
    const TopoDS_Edge& e = TopoDS::Edge(ex.Current());
    if (BRep_Tool::Degenerated(e)) continue;
    double f, l;
    Handle(Geom_Curve) c = BRep_Tool::Curve(e, f, l);
    if (c.IsNull()) continue;
    Handle(Geom_BSplineCurve) bs = GeomConvert::CurveToBSplineCurve(new Geom_TrimmedCurve(c, f, l));
    ON_NurbsCurve* nc = FromOcct(bs);
    if (e.Orientation() == TopAbs_REVERSED) nc->Reverse();
    out.push_back(nc);
  }
  return out;
}

ON_Mesh* MeshShape(const TopoDS_Shape& shape, double lin, double ang, bool weld) {
  BRepMesh_IncrementalMesh mesher(shape, lin, Standard_False, ang, Standard_True);
  auto* mesh = new ON_Mesh();
  for (TopExp_Explorer fx(shape, TopAbs_FACE); fx.More(); fx.Next()) {
    const TopoDS_Face& face = TopoDS::Face(fx.Current());
    TopLoc_Location loc;
    Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
    if (tri.IsNull()) continue;
    const gp_Trsf tr = loc.Transformation();
    const int base = mesh->m_V.Count();
    for (int i = 1; i <= tri->NbNodes(); i++) {
      gp_Pnt p = tri->Node(i).Transformed(tr);
      mesh->m_V.Append(ON_3fPoint(static_cast<float>(p.X()), static_cast<float>(p.Y()), static_cast<float>(p.Z())));
    }
    if (!weld) {
      // coordinate di texture: parametri della superficie normalizzati sul dominio
      // (mappatura "superficie" di Rhino); per superfici illimitate, sui limiti della faccia
      if (mesh->m_T.Count() != base) mesh->m_T.SetCount(base);
      {
        double u0 = 0, u1 = 1, v0 = 0, v1 = 1;
        Handle(Geom_Surface) gs = BRep_Tool::Surface(face);
        if (!gs.IsNull()) gs->Bounds(u0, u1, v0, v1);
        if (gs.IsNull() || Precision::IsInfinite(u0) || Precision::IsInfinite(u1) || Precision::IsInfinite(v0) ||
            Precision::IsInfinite(v1))
          BRepTools::UVBounds(face, u0, u1, v0, v1);
        const double du = std::fabs(u1 - u0) > 1e-12 ? u1 - u0 : 1.0, dv = std::fabs(v1 - v0) > 1e-12 ? v1 - v0 : 1.0;
        for (int i = 1; i <= tri->NbNodes(); i++) {
          ON_2fPoint t(0.f, 0.f);
          if (tri->HasUVNodes()) {
            const gp_Pnt2d uv = tri->UVNode(i);
            t.Set(static_cast<float>((uv.X() - u0) / du), static_cast<float>((uv.Y() - v0) / dv));
          }
          mesh->m_T.Append(t);
        }
      }
      // normali esatte dalla superficie nei nodi (orientate come la faccia)
      if (mesh->m_N.Count() != base) mesh->m_N.SetCount(base);
      BRepGProp_Face gf(face);
      for (int i = 1; i <= tri->NbNodes(); i++) {
        gp_Vec n(0, 0, 0);
        if (tri->HasUVNodes()) {
          gp_Pnt q;
          const gp_Pnt2d uv = tri->UVNode(i);
          try { gf.Normal(uv.X(), uv.Y(), q, n); } catch (...) {}
          if (!loc.IsIdentity()) n.Transform(tr);
        }
        if (n.Magnitude() > 1e-15) n.Normalize();
        mesh->m_N.Append(ON_3fVector(static_cast<float>(n.X()), static_cast<float>(n.Y()), static_cast<float>(n.Z())));
      }
    }
    const bool rev = face.Orientation() == TopAbs_REVERSED;
    for (int i = 1; i <= tri->NbTriangles(); i++) {
      int a, bI, c;
      tri->Triangle(i).Get(a, bI, c);
      if (rev) std::swap(bI, c);
      mesh->SetTriangle(mesh->m_F.Count(), base + a - 1, base + bI - 1, base + c - 1);
    }
  }
  if (mesh->m_F.Count() == 0) { delete mesh; return nullptr; }
  if (!weld) {
    // normali mancanti (poli, nodi senza uv): dalla media delle facce della stessa faccia
    bool bad = mesh->m_N.Count() != mesh->m_V.Count();
    for (int i = 0; !bad && i < mesh->m_N.Count(); i++) if (mesh->m_N[i].LengthSquared() < 0.5f) bad = true;
    if (bad) {
      ON_3fVectorArray keep = mesh->m_N;
      mesh->m_N.Destroy();
      mesh->ComputeVertexNormals();
      if (keep.Count() == mesh->m_N.Count())
        for (int i = 0; i < keep.Count(); i++) if (keep[i].LengthSquared() >= 0.5f) mesh->m_N[i] = keep[i];
    }
    mesh->Compact();
    return mesh;
  }
  // Saldatura dei vertici coincidenti tra facce adiacenti (mesh chiusa per i solidi).
  {
    std::map<std::tuple<long long, long long, long long>, int> index;
    std::vector<int> remap(mesh->m_V.Count());
    ON_3fPointArray verts;
    const double q = 1e6;
    for (int i = 0; i < mesh->m_V.Count(); i++) {
      const ON_3fPoint& v = mesh->m_V[i];
      auto key = std::make_tuple(std::llround(v.x * q), std::llround(v.y * q), std::llround(v.z * q));
      auto it = index.find(key);
      if (it == index.end()) { it = index.emplace(key, verts.Count()).first; verts.Append(v); }
      remap[i] = it->second;
    }
    for (int i = 0; i < mesh->m_F.Count(); i++)
      for (int k = 0; k < 4; k++) mesh->m_F[i].vi[k] = remap[mesh->m_F[i].vi[k]];
    mesh->m_V = verts;
    mesh->CullDegenerateFaces();
  }
  mesh->ComputeVertexNormals();
  mesh->Compact();
  return mesh;
}

}  // namespace cad::occt
