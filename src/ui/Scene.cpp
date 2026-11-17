#include "ui/Scene.h"
#ifdef CAD_WITH_OCCT
#include "kernel/OcctConvert.h"
#endif
#include <cmath>

namespace cad::ui {

void TessellateCurve(const ON_Curve& c, std::vector<ON_3dPoint>& out) {
  if (const auto* l = ON_LineCurve::Cast(&c)) { out = {l->m_line.from, l->m_line.to}; return; }
  if (const auto* p = ON_PolylineCurve::Cast(&c)) {
    out.assign(p->m_pline.Array(), p->m_pline.Array() + p->m_pline.Count());
    return;
  }
  if (const auto* pc = ON_PolyCurve::Cast(&c)) {
    out.clear();
    for (int i = 0; i < pc->Count(); i++) {
      std::vector<ON_3dPoint> seg;
      TessellateCurve(*pc->SegmentCurve(i), seg);
      if (!out.empty() && !seg.empty()) seg.erase(seg.begin());
      out.insert(out.end(), seg.begin(), seg.end());
    }
    return;
  }
  // Curve generiche: campionamento per campata (piu' fitto per archi e gradi alti).
  const int spans = std::max(1, c.SpanCount());
  int perSpan = c.Degree() <= 1 ? 1 : 24;
  if (ON_ArcCurve::Cast(&c)) perSpan = 96 / spans + 1;
  std::vector<double> knots(spans + 1);
  c.GetSpanVector(knots.data());
  out.clear();
  for (int s = 0; s < spans; s++)
    for (int i = 0; i < perSpan; i++)
      out.push_back(c.PointAt(knots[s] + (knots[s + 1] - knots[s]) * i / perSpan));
  out.push_back(c.PointAtEnd());
}

const char* SnapName(SnapKind k) {
  switch (k) {
    case SnapKind::End: return "Fine";
    case SnapKind::Mid: return "Medio";
    case SnapKind::Center: return "Centro";
    case SnapKind::Quad: return "Quadrante";
    case SnapKind::Knot: return "Nodo";
    case SnapKind::Point: return "Punto";
    case SnapKind::Vertex: return "Vertice";
  }
  return "";
}

namespace {

ON_3dPoint ArcLengthMid(const ON_Curve& c) {
  const ON_Interval d = c.Domain();
  const int n = 200;
  std::vector<double> len(n + 1, 0.0);
  ON_3dPoint prev = c.PointAt(d[0]);
  for (int i = 1; i <= n; i++) {
    ON_3dPoint q = c.PointAt(d.ParameterAt(double(i) / n));
    len[i] = len[i - 1] + prev.DistanceTo(q);
    prev = q;
  }
  const double half = len[n] / 2;
  for (int i = 1; i <= n; i++)
    if (len[i] >= half) {
      const double f = len[i] > len[i - 1] ? (half - len[i - 1]) / (len[i] - len[i - 1]) : 0;
      return c.PointAt(d.ParameterAt((i - 1 + f) / n));
    }
  return c.PointAt(d.Mid());
}

void AddCurveSnaps(const ON_Curve& c, std::vector<SnapPoint>& snaps) {
  using K = SnapKind;
  if (const auto* pl = ON_PolylineCurve::Cast(&c)) {
    // polilinea: estremi e punto medio di ogni segmento (come Rhino)
    for (int i = 0; i < pl->PointCount(); i++) snaps.push_back({pl->m_pline[i], K::End});
    for (int i = 0; i + 1 < pl->PointCount(); i++) snaps.push_back({0.5 * (pl->m_pline[i] + pl->m_pline[i + 1]), K::Mid});
    return;
  }
  if (const auto* pc = ON_PolyCurve::Cast(&c)) {
    for (int i = 0; i < pc->Count(); i++)
      if (const ON_Curve* seg = pc->SegmentCurve(i)) AddCurveSnaps(*seg, snaps);
    return;
  }
  snaps.push_back({c.PointAtStart(), K::End});
  if (!c.IsClosed()) snaps.push_back({c.PointAtEnd(), K::End});
  snaps.push_back({ArcLengthMid(c), K::Mid});
  ON_Arc arc;
  bool isArc = false;
  if (const auto* a = ON_ArcCurve::Cast(&c)) { arc = a->m_arc; isArc = true; }
  else if (!ON_LineCurve::Cast(&c)) isArc = c.IsArc(nullptr, &arc, 1e-9);
  if (isArc) {
    snaps.push_back({arc.Center(), K::Center});
    for (int q = 0; q < 4; q++) {
      const double ang = q * ON_PI / 2;
      // quadranti dentro l'arco
      double t = ang;
      while (t < arc.DomainRadians()[0] - 1e-9) t += 2 * ON_PI;
      if (t <= arc.DomainRadians()[1] + 1e-9) snaps.push_back({arc.PointAt(t), K::Quad});
    }
  }
  if (const auto* nc = ON_NurbsCurve::Cast(&c)) {
    const int kc = nc->KnotCount();
    for (int i = nc->Order() - 1; i < kc - nc->Order() + 1; i++) {
      if (i > nc->Order() - 1 && nc->m_knot[i] == nc->m_knot[i - 1]) continue;
      const double t = nc->m_knot[i];
      if (t > nc->Domain()[0] && t < nc->Domain()[1]) snaps.push_back({nc->PointAt(t), K::Knot});
    }
  }
}

void AddBrep(const ON_Brep& b, DisplayObject& d) {
  for (int i = 0; i < b.m_E.Count(); i++) {
    std::vector<ON_3dPoint> w;
    TessellateCurve(b.m_E[i], w);
    d.wires.push_back(std::move(w));
    if (const ON_Curve* ec = b.m_E[i].EdgeCurveOf()) {
      std::unique_ptr<ON_Curve> sub(ec->DuplicateCurve());
      sub->Trim(b.m_E[i].ProxyCurveDomain());
      std::vector<SnapPoint> es;
      AddCurveSnaps(*sub, es);
      for (auto& sp : es) if (sp.kind != SnapKind::End) d.snapPoints.push_back(sp);
    }
  }
  for (int i = 0; i < b.m_V.Count(); i++) d.snapPoints.push_back({b.m_V[i].point, SnapKind::End});
}

std::shared_ptr<ON_Mesh> SubDMesh(const ON_SubD& s) {
  // Mesh della superficie limite costruita dai frammenti (griglie di quad)
  ON_Mesh* m = new ON_Mesh();
  ON_SubDMeshFragmentIterator frit(s);
  for (const ON_SubDMeshFragment* f = frit.FirstFragment(); f; f = frit.NextFragment()) {
    const unsigned n = f->m_grid.SidePointCount();
    if (n < 2) continue;
    const int base = m->m_V.Count();
    for (unsigned j = 0; j < n; j++)
      for (unsigned i = 0; i < n; i++) m->m_V.Append(ON_3fPoint(f->VertexPoint(i, j)));
    for (unsigned j = 0; j + 1 < n; j++)
      for (unsigned i = 0; i + 1 < n; i++) {
        const int a = base + int(j * n + i);
        m->SetQuad(m->m_F.Count(), a, a + 1, a + 1 + int(n), a + int(n));
      }
  }
  if (m->m_F.Count() == 0) {
    delete m;
    m = s.GetControlNetMesh(nullptr, ON_SubDGetControlNetMeshPriority::Geometry);
  }
  if (m) m->ComputeVertexNormals();
  return std::shared_ptr<ON_Mesh>(m);
}

}  // namespace

void Scene::Rebuild() {
  m_objects.clear();
  std::map<std::string, std::shared_ptr<ON_Mesh>> newCache;
  ONX_Model& model = m_engine.Model();
  for (const auto& id : m_engine.AllObjectIds(false)) {
    const auto* o = m_engine.Object(id);
    const ON_Geometry* g = o->Geometry(nullptr);
    const ON_3dmObjectAttributes* a = o->Attributes(nullptr);
    if (!g || !a) continue;
    // Livelli spenti: oggetti non visualizzati (come Rhino)
    const auto* layer = ON_Layer::Cast(model.ComponentFromIndex(ON_ModelComponent::Type::Layer, a->m_layer_index).ModelComponent());
    if (layer && !layer->IsVisible()) continue;
    DisplayObject d;
    d.id = id;
    d.color = (a->ColorSource() == ON::color_from_object) ? a->m_color : (layer ? layer->Color() : ON_Color::Black);
    d.locked = a->Mode() == ON::object_mode::locked_object;
    g->GetTightBoundingBox(d.bbox);
    char key[64];
    ON_UuidToString(id, key);
    // firma: classe + bounding box (si rigenera dopo ogni modifica geometrica)
    std::string sig = std::string(key) + g->ClassId()->ClassName();
    for (int i = 0; i < 3; i++) sig += "|" + std::to_string(d.bbox.m_min[i]) + "|" + std::to_string(d.bbox.m_max[i]);

    if (const auto* p = ON_Point::Cast(g)) {
      d.points.push_back(p->point);
      d.snapPoints.push_back({p->point, SnapKind::Point});
    } else if (const auto* pc = ON_PointCloud::Cast(g)) {
      for (int i = 0; i < pc->m_P.Count(); i++) { d.points.push_back(pc->m_P[i]); d.snapPoints.push_back({pc->m_P[i], SnapKind::Point}); }
    } else if (const auto* c = ON_Curve::Cast(g)) {
      std::vector<ON_3dPoint> w;
      TessellateCurve(*c, w);
      d.wires.push_back(std::move(w));
      AddCurveSnaps(*c, d.snapPoints);
    } else if (const auto* m = ON_Mesh::Cast(g)) {
      auto mesh = std::make_shared<ON_Mesh>(*m);
      if (!mesh->HasVertexNormals()) mesh->ComputeVertexNormals();
      d.shaded = mesh;
      for (int i = 0; i < m->m_V.Count(); i++) d.snapPoints.push_back({ON_3dPoint(m->m_V[i]), SnapKind::Vertex});
      const ON_MeshTopology& top = m->Topology();
      for (int i = 0; i < top.m_tope.Count(); i++) {
        const ON_MeshTopologyEdge& e = top.m_tope[i];
        d.wires.push_back({ON_3dPoint(top.m_topv[e.m_topvi[0]].m_v_count ? m->m_V[top.m_topv[e.m_topvi[0]].m_vi[0]] : ON_3fPoint::Origin),
                           ON_3dPoint(top.m_topv[e.m_topvi[1]].m_v_count ? m->m_V[top.m_topv[e.m_topvi[1]].m_vi[0]] : ON_3fPoint::Origin)});
      }
    } else if (const auto* s = ON_SubD::Cast(g)) {
      ON_SubDEdgeIterator eit(*s);
      for (const ON_SubDEdge* e = eit.FirstEdge(); e; e = eit.NextEdge())
        if (e->m_vertex[0] && e->m_vertex[1])
          d.wires.push_back({ON_3dPoint(e->m_vertex[0]->m_P), ON_3dPoint(e->m_vertex[1]->m_P)});
      ON_SubDVertexIterator vit(*s);
      for (const ON_SubDVertex* v = vit.FirstVertex(); v; v = vit.NextVertex())
        d.snapPoints.push_back({ON_3dPoint(v->m_P), SnapKind::Vertex});
      auto it = m_meshCache.find(sig);
      d.shaded = it != m_meshCache.end() ? it->second : SubDMesh(*s);
      newCache[sig] = d.shaded;
    } else {
      std::unique_ptr<ON_Brep> owned;
      const ON_Brep* b = ON_Brep::Cast(g);
      if (!b) {
        if (const auto* x = ON_Extrusion::Cast(g)) owned.reset(x->BrepForm());
        else if (const auto* sf = ON_Surface::Cast(g)) owned.reset(sf->BrepForm());
        b = owned.get();
      }
      if (b) {
        AddBrep(*b, d);
#ifdef CAD_WITH_OCCT
        auto it = m_meshCache.find(sig);
        if (it != m_meshCache.end()) d.shaded = it->second;
        else {
          const double size = d.bbox.Diagonal().Length();
          TopoDS_Shape shape = occt::BrepToShape(*b, m_engine.Tolerance());
          d.shaded.reset(occt::MeshShape(shape, std::max(size / 400.0, 0.01), 15.0 * ON_PI / 180.0));
        }
        newCache[sig] = d.shaded;
#endif
      }
    }
    m_objects.push_back(std::move(d));
  }
  m_meshCache.swap(newCache);
  RefreshSelection();
}

void Scene::RefreshSelection() {
  for (auto& d : m_objects) d.selected = m_engine.IsSelected(d.id);
}

ON_BoundingBox Scene::Bounds() const {
  ON_BoundingBox b;
  for (const auto& d : m_objects) b.Union(d.bbox);
  return b;
}

}  // namespace cad::ui
