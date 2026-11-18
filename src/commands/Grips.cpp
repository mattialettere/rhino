#include "commands/Grips.h"
#include <memory>

namespace cad {
namespace {

// Superficie di un B-rep a faccia singola non tagliata (unico caso modificabile).
const ON_Surface* SingleUntrimmedSurface(const ON_Brep* b) {
  if (!b || b->m_F.Count() != 1 || b->m_L.Count() != 1) return nullptr;
  const ON_BrepFace& f = b->m_F[0];
  const ON_Surface* s = f.SurfaceOf();
  if (!s) return nullptr;
  // bordo esterno = rettangolo del dominio: 4 lati iso (o meno per superfici chiuse/singolari)
  const ON_BrepLoop& l = b->m_L[0];
  for (int ti = 0; ti < l.m_ti.Count(); ti++) {
    const ON_BrepTrim& t = b->m_T[l.m_ti[ti]];
    if (t.m_iso == ON_Surface::not_iso) return nullptr;
  }
  return s;
}

}  // namespace

bool SupportsGrips(const ON_Geometry* g) {
  GripSet tmp;
  return GetGrips(g, tmp);
}

bool GetGrips(const ON_Geometry* g, GripSet& out) {
  out = {};
  if (const auto* pl = ON_PolylineCurve::Cast(g)) {
    const int n = pl->PointCount() - (pl->IsClosed() ? 1 : 0);
    for (int i = 0; i < n; i++) out.grips.push_back({pl->m_pline[i], i});
    return n > 0;
  }
  if (const auto* ln = ON_LineCurve::Cast(g)) {
    out.grips.push_back({ln->m_line.from, 0});
    out.grips.push_back({ln->m_line.to, 1});
    return true;
  }
  if (const auto* c = ON_Curve::Cast(g)) {
    ON_NurbsCurve nc;
    if (!c->GetNurbForm(nc)) return false;
    const int n = nc.CVCount() - (nc.IsPeriodic() ? nc.Degree() : 0);
    std::vector<ON_3dPoint> poly;
    for (int i = 0; i < nc.CVCount(); i++) {
      ON_3dPoint p;
      nc.GetCV(i, p);
      poly.push_back(p);
      if (i < n) out.grips.push_back({p, i});
    }
    out.cage.push_back(poly);
    return true;
  }
  if (const auto* b = ON_Brep::Cast(g)) {
    const ON_Surface* s = SingleUntrimmedSurface(b);
    ON_NurbsSurface ns;
    if (!s || !s->GetNurbForm(ns)) return false;
    for (int i = 0; i < ns.CVCount(0); i++) {
      std::vector<ON_3dPoint> row;
      for (int j = 0; j < ns.CVCount(1); j++) {
        ON_3dPoint p;
        ns.GetCV(i, j, p);
        out.grips.push_back({p, i, j});
        row.push_back(p);
      }
      out.cage.push_back(row);
    }
    for (int j = 0; j < ns.CVCount(1); j++) {
      std::vector<ON_3dPoint> col;
      for (int i = 0; i < ns.CVCount(0); i++) { ON_3dPoint p; ns.GetCV(i, j, p); col.push_back(p); }
      out.cage.push_back(col);
    }
    return true;
  }
  if (const auto* m = ON_Mesh::Cast(g)) {
    // un punto per vertice topologico (i = primo vertice della mesh)
    const ON_MeshTopology& top = m->Topology();
    for (int i = 0; i < top.m_topv.Count(); i++)
      if (top.m_topv[i].m_v_count > 0) out.grips.push_back({ON_3dPoint(m->m_V[top.m_topv[i].m_vi[0]]), top.m_topv[i].m_vi[0]});
    return !out.grips.empty();
  }
  if (const auto* s = ON_SubD::Cast(g)) {
    ON_SubDVertexIterator vit(*s);
    for (const ON_SubDVertex* v = vit.FirstVertex(); v; v = vit.NextVertex())
      out.grips.push_back({v->ControlNetPoint(), (int)v->m_id});
    ON_SubDEdgeIterator eit(*s);
    for (const ON_SubDEdge* e = eit.FirstEdge(); e; e = eit.NextEdge())
      if (e->m_vertex[0] && e->m_vertex[1]) out.cage.push_back({e->m_vertex[0]->ControlNetPoint(), e->m_vertex[1]->ControlNetPoint()});
    return !out.grips.empty();
  }
  return false;
}

ON_Geometry* MoveGrip(const ON_Geometry* g, int i, int j, const ON_3dPoint& p) {
  if (const auto* pl = ON_PolylineCurve::Cast(g)) {
    auto* c = static_cast<ON_PolylineCurve*>(pl->Duplicate());
    const int n = c->PointCount();
    if (i < 0 || i >= n) { delete c; return nullptr; }
    c->m_pline[i] = p;
    if (pl->IsClosed() && i == 0) c->m_pline[n - 1] = p;  // il primo punto chiude la polilinea
    return c;
  }
  if (const auto* ln = ON_LineCurve::Cast(g)) {
    if (i < 0 || i > 1) return nullptr;
    ON_Line l = ln->m_line;
    (i == 0 ? l.from : l.to) = p;
    if (l.Length() <= ON_ZERO_TOLERANCE) return nullptr;
    return new ON_LineCurve(l);
  }
  if (const auto* c = ON_Curve::Cast(g)) {
    auto* nc = new ON_NurbsCurve();
    if (!c->GetNurbForm(*nc) || i < 0 || i >= nc->CVCount()) { delete nc; return nullptr; }
    auto set = [&](int k) {
      const double w = nc->Weight(k);
      if (nc->IsRational()) nc->SetCV(k, ON_4dPoint(p.x * w, p.y * w, p.z * w, w));
      else nc->SetCV(k, p);
    };
    set(i);
    // curva periodica: i primi "grado" punti sono ripetuti in fondo
    if (nc->IsPeriodic() && i < nc->Degree()) set(nc->CVCount() - nc->Degree() + i);
    if (!nc->IsValid()) { delete nc; return nullptr; }
    return nc;
  }
  if (const auto* b = ON_Brep::Cast(g)) {
    const ON_Surface* s = SingleUntrimmedSurface(b);
    auto* ns = new ON_NurbsSurface();
    if (!s || !s->GetNurbForm(*ns) || i < 0 || j < 0 || i >= ns->CVCount(0) || j >= ns->CVCount(1)) { delete ns; return nullptr; }
    if (b->m_F[0].m_bRev) ns->Reverse(0);  // il nuovo B-rep conserva il verso della normale
    if (b->m_F[0].m_bRev) i = ns->CVCount(0) - 1 - i;
    const double w = ns->Weight(i, j);
    if (ns->IsRational()) ns->SetCV(i, j, ON_4dPoint(p.x * w, p.y * w, p.z * w, w));
    else ns->SetCV(i, j, p);
    ON_Surface* sp = ns;
    auto* nb = ON_Brep::New();
    if (!nb->Create(sp)) { delete nb; delete sp; return nullptr; }
    return nb;
  }
  if (const auto* m = ON_Mesh::Cast(g)) {
    if (i < 0 || i >= m->m_V.Count()) return nullptr;
    auto* nm = new ON_Mesh(*m);
    const ON_3fPoint old = m->m_V[i];
    for (int k = 0; k < nm->m_V.Count(); k++)
      if (nm->m_V[k] == old) nm->m_V[k] = ON_3fPoint(p);
    nm->m_dV.Destroy();
    nm->DestroyTopology();
    nm->ComputeVertexNormals();
    return nm;
  }
  if (const auto* s = ON_SubD::Cast(g)) {
    auto* ns = new ON_SubD(*s);
    const ON_SubDVertex* v = ns->VertexFromId((unsigned)i);
    if (!v || !const_cast<ON_SubDVertex*>(v)->SetControlNetPoint(p, true)) { delete ns; return nullptr; }
    ns->ClearEvaluationCache();
    return ns;
  }
  return nullptr;
}

}  // namespace cad
