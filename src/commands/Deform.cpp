#include "commands/Deform.h"
#include <algorithm>
#include <memory>
#ifdef CAD_WITH_OCCT
#include "kernel/OcctConvert.h"
#include <BRepBuilderAPI_NurbsConvert.hxx>
#endif

namespace cad {
namespace {

constexpr int kMinSpans = 16;

void MapCV(ON_NurbsCurve& c, int i, const PointMap& f) {
  ON_3dPoint p;
  c.GetCV(i, p);
  const ON_3dPoint q = f(p);
  if (c.IsRational()) {
    const double w = c.Weight(i);
    c.SetCV(i, ON_4dPoint(q.x * w, q.y * w, q.z * w, w));
  } else {
    c.SetCV(i, q);
  }
}

// Curva NURBS cubica con almeno kMinSpans campate (stesso parametro della curva originale se possibile).
bool RefinedCurve(const ON_Curve& c, ON_NurbsCurve& nc) {
  if (!c.GetNurbForm(nc)) return false;
  if (nc.Degree() < 3) nc.IncreaseDegree(3);
  const ON_Interval d = nc.Domain();
  const int spans = nc.SpanCount();
  if (spans < kMinSpans) {
    for (int k = 1; k < kMinSpans; k++) {
      const double t = d.ParameterAt(double(k) / kMinSpans);
      bool exists = false;
      for (int i = 0; i < nc.KnotCount(); i++) if (std::fabs(nc.m_knot[i] - t) < 1e-12 * (1 + d.Length())) exists = true;
      if (!exists) nc.InsertKnot(t, 1);
    }
  }
  return true;
}

ON_NurbsCurve* DeformCurve(const ON_Curve& c, const PointMap& f) {
  auto* nc = new ON_NurbsCurve();
  if (!RefinedCurve(c, *nc)) { delete nc; return nullptr; }
  for (int i = 0; i < nc->CVCount(); i++) MapCV(*nc, i, f);
  return nc;
}

bool RefinedSurface(const ON_Surface& s, ON_NurbsSurface& ns) {
  if (s.GetNurbForm(ns) != 1) return false;  // parametri diversi: i tagli non varrebbero piu'
  for (int dir = 0; dir < 2; dir++) {
    if (ns.Degree(dir) < 3) ns.IncreaseDegree(dir, 3);
    const ON_Interval d = ns.Domain(dir);
    if (ns.SpanCount(dir) < kMinSpans) {
      for (int k = 1; k < kMinSpans; k++) {
        const double t = d.ParameterAt(double(k) / kMinSpans);
        bool exists = false;
        for (int i = 0; i < ns.KnotCount(dir); i++) if (std::fabs(ns.m_knot[dir][i] - t) < 1e-12 * (1 + d.Length())) exists = true;
        if (!exists) ns.InsertKnot(dir, t, 1);
      }
    }
  }
  return true;
}

ON_Brep* DeformBrep(const ON_Brep& in, const PointMap& f) {
  auto b = std::make_unique<ON_Brep>(in);
  for (int si = 0; si < b->m_S.Count(); si++) {
    if (!b->m_S[si]) continue;
    auto* ns = new ON_NurbsSurface();
    if (!RefinedSurface(*b->m_S[si], *ns)) { delete ns; return nullptr; }
    for (int i = 0; i < ns->CVCount(0); i++)
      for (int j = 0; j < ns->CVCount(1); j++) {
        ON_3dPoint p;
        ns->GetCV(i, j, p);
        const ON_3dPoint q = f(p);
        if (ns->IsRational()) {
          const double w = ns->Weight(i, j);
          ns->SetCV(i, j, ON_4dPoint(q.x * w, q.y * w, q.z * w, w));
        } else {
          ns->SetCV(i, j, q);
        }
      }
    delete b->m_S[si];
    b->m_S[si] = ns;
  }
  for (int ci = 0; ci < b->m_C3.Count(); ci++) {
    if (!b->m_C3[ci]) continue;
    ON_NurbsCurve tmp;
    if (b->m_C3[ci]->GetNurbForm(tmp) != 1) return nullptr;
    ON_NurbsCurve* nc = DeformCurve(*b->m_C3[ci], f);
    if (!nc) return nullptr;
    nc->SetDomain(b->m_C3[ci]->Domain()[0], b->m_C3[ci]->Domain()[1]);
    delete b->m_C3[ci];
    b->m_C3[ci] = nc;
  }
  // facce e spigoli sono "proxy" delle superfici e curve sostituite: si ricollegano
  for (int fi = 0; fi < b->m_F.Count(); fi++) {
    ON_BrepFace& face = b->m_F[fi];
    if (face.m_si >= 0 && face.m_si < b->m_S.Count()) face.SetProxySurface(b->m_S[face.m_si]);
  }
  for (int ei = 0; ei < b->m_E.Count(); ei++) {
    ON_BrepEdge& edge = b->m_E[ei];
    if (edge.m_c3i < 0 || edge.m_c3i >= b->m_C3.Count()) continue;
    const ON_Interval pd = edge.ProxyCurveDomain(), td = edge.Domain();
    const bool rev = edge.ProxyCurveIsReversed();
    edge.SetProxyCurve(b->m_C3[edge.m_c3i], pd);
    if (rev) edge.ON_CurveProxy::Reverse();
    edge.SetDomain(td[0], td[1]);
  }
  for (int vi = 0; vi < b->m_V.Count(); vi++) b->m_V[vi].point = f(b->m_V[vi].point);
  // tolleranze di bordi e vertici: la deformazione dei punti di controllo e' approssimata
  for (int ei = 0; ei < b->m_E.Count(); ei++) b->m_E[ei].m_tolerance = ON_UNSET_VALUE;
  for (int vi = 0; vi < b->m_V.Count(); vi++) b->m_V[vi].m_tolerance = ON_UNSET_VALUE;
  b->SetTolerancesBoxesAndFlags(false, true, true, true, true, true, true, true);
  for (int fi = 0; fi < b->m_F.Count(); fi++) b->m_F[fi].ClearBoundingBox();
  b->ClearBoundingBox();
  for (int ei = 0; ei < b->m_E.Count(); ei++) if (!(b->m_E[ei].m_tolerance >= 0)) b->m_E[ei].m_tolerance = 0;
  for (int vi = 0; vi < b->m_V.Count(); vi++) if (!(b->m_V[vi].m_tolerance >= 0)) b->m_V[vi].m_tolerance = 0;
  b->DestroyMesh(ON::any_mesh);
  return b.release();
}

}  // namespace

ON_Geometry* DeformGeometry(const ON_Geometry* g, const PointMap& f) {
  if (!g) return nullptr;
  if (const auto* p = ON_Point::Cast(g)) return new ON_Point(f(p->point));
  if (const auto* pc = ON_PointCloud::Cast(g)) {
    auto* o = new ON_PointCloud(*pc);
    for (int i = 0; i < o->m_P.Count(); i++) o->m_P[i] = f(o->m_P[i]);
    o->InvalidateBoundingBox();
    return o;
  }
  if (const auto* c = ON_Curve::Cast(g)) return DeformCurve(*c, f);
  if (const auto* b = ON_Brep::Cast(g)) {
    if (ON_Brep* r = DeformBrep(*b, f)) return r;
#ifdef CAD_WITH_OCCT
    // superfici con parametri diversi dalla forma NURBS (rivoluzioni, estrusioni di
    // archi): prima tutto in NURBS con le curve di taglio coerenti, poi la deformazione
    try {
      BRepBuilderAPI_NurbsConvert nc(occt::BrepToShape(*b, 1e-6), Standard_True);
      std::unique_ptr<ON_Brep> nb(occt::ShapeToBrep(nc.Shape(), 1e-6));
      if (nb) return DeformBrep(*nb, f);
    } catch (...) {
    }
#endif
    return nullptr;
  }
  if (const auto* m = ON_Mesh::Cast(g)) {
    auto* o = new ON_Mesh(*m);
    o->m_dV.Destroy();
    for (int i = 0; i < o->m_V.Count(); i++) o->m_V[i] = ON_3fPoint(f(ON_3dPoint(o->m_V[i])));
    o->m_N.Destroy();
    o->m_FN.Destroy();
    o->ComputeVertexNormals();
    o->InvalidateBoundingBoxes();
    return o;
  }
  if (const auto* s = ON_SubD::Cast(g)) {
    auto* o = new ON_SubD(*s);
    ON_SubDVertexIterator it(*o);
    for (const ON_SubDVertex* v = it.FirstVertex(); v; v = it.NextVertex())
      const_cast<ON_SubDVertex*>(v)->SetControlNetPoint(f(v->ControlNetPoint()), true);
    o->ClearEvaluationCache();
    return o;
  }
  if (g->HasBrepForm()) {  // estrusioni e superfici: come B-rep
    std::unique_ptr<ON_Brep> b(g->BrepForm());
    return b ? DeformGeometry(b.get(), f) : nullptr;
  }
  return nullptr;
}

}  // namespace cad
