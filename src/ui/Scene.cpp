#include "ui/Scene.h"
#ifdef CAD_WITH_OCCT
#include "core/SubDLimit.h"
#include "commands/Materials.h"
#include "kernel/OcctConvert.h"
#endif
#include <cmath>
#include <memory>
#include <set>

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

// Isocurve come nel wireframe di Rhino (densita' 1): una curva per ogni nodo
// interno della superficie in ciascuna direzione, tagliata dai bordi della faccia.
void AddIsocurves(const ON_Brep& b, DisplayObject& d) {
  for (int fi = 0; fi < b.m_F.Count(); fi++) {
    const ON_BrepFace& f = b.m_F[fi];
    const ON_Surface* srf = f.SurfaceOf();
    if (!srf) continue;
    // contorni della regione di parametri (cicli di trim campionati)
    std::vector<std::vector<ON_2dPoint>> loops;
    for (int li = 0; li < f.m_li.Count(); li++) {
      const ON_BrepLoop& L = b.m_L[f.m_li[li]];
      std::vector<ON_2dPoint> poly;
      for (int k = 0; k < L.m_ti.Count(); k++) {
        const ON_BrepTrim& t = b.m_T[L.m_ti[k]];
        const ON_Interval dom = t.Domain();
        const int n = 24;
        for (int j = 0; j < n; j++) {
          const ON_3dPoint q = t.PointAt(dom.ParameterAt(double(j) / n));
          poly.push_back(ON_2dPoint(q.x, q.y));
        }
      }
      if (poly.size() >= 3) loops.push_back(std::move(poly));
    }
    auto inside = [&](double u, double v) {
      bool in = false;
      for (const auto& poly : loops)
        for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
          if (((poly[i].y > v) != (poly[j].y > v)) &&
              (u < (poly[j].x - poly[i].x) * (v - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x))
            in = !in;
      return loops.empty() ? true : in;
    };
    for (int dir = 0; dir < 2; dir++) {
      const int spans = srf->SpanCount(dir);
      if (spans < 2 || spans > 64) continue;
      std::vector<double> sv(spans + 1);
      if (!srf->GetSpanVector(dir, sv.data())) continue;
      const ON_Interval other = srf->Domain(1 - dir);
      const int step = std::max(1, spans / 16);  // al massimo ~16 isocurve per direzione
      for (int k = 1; k < spans; k += step) {
        const double c = sv[k];
        const int n = 64;
        std::vector<ON_3dPoint> run;
        for (int j = 0; j <= n; j++) {
          const double t = other.ParameterAt(double(j) / n);
          const double u = dir == 0 ? c : t, v = dir == 0 ? t : c;
          if (inside(u, v)) run.push_back(srf->PointAt(u, v));
          else {
            if (run.size() >= 2) d.isocurves.push_back(run);
            run.clear();
          }
        }
        if (run.size() >= 2) d.isocurves.push_back(std::move(run));
      }
    }
  }
}

// Candidati al contorno apparente: spigoli interni della mesh tra facce non
// complanari (gli spigoli vivi sono gia' disegnati come bordi delle facce).
void AddSilhouetteCandidates(const ON_Mesh& m, DisplayObject& d) {
  ON_Mesh mm(m);
  if (!mm.HasFaceNormals()) mm.ComputeFaceNormals();
  const ON_MeshTopology& top = mm.Topology();
  for (int i = 0; i < top.m_tope.Count(); i++) {
    const ON_MeshTopologyEdge& e = top.m_tope[i];
    if (e.m_topf_count != 2) continue;
    const ON_3dVector n1(mm.m_FN[e.m_topfi[0]]), n2(mm.m_FN[e.m_topfi[1]]);
    if (n1 * n2 < 0.5) continue;  // spigolo vivo: non e' un contorno apparente
    const auto& v0 = top.m_topv[e.m_topvi[0]];
    const auto& v1 = top.m_topv[e.m_topvi[1]];
    if (!v0.m_v_count || !v1.m_v_count) continue;
    d.silhouette.push_back({ON_3dPoint(mm.m_V[v0.m_vi[0]]), ON_3dPoint(mm.m_V[v1.m_vi[0]]), n1, n2});
  }
}

void AddBrep(const ON_Brep& b, DisplayObject& d) {
  AddIsocurves(b, d);
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

}  // namespace

void Scene::AddShaded(DisplayObject& d, const std::shared_ptr<ON_Mesh>& m) {
  if (!m) return;
  if (!d.shaded) { d.shaded = m; return; }
  auto comb = std::make_shared<ON_Mesh>(*d.shaded);
  comb->Append(*m);
  d.shaded = comb;
}

void Scene::AddGeometry(const ON_Geometry* g, DisplayObject& d, const std::string& sig,
                        std::map<std::string, std::shared_ptr<ON_Mesh>>& newCache, int depth) {
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
    AddShaded(d, mesh);
    for (int i = 0; i < m->m_V.Count(); i++) d.snapPoints.push_back({ON_3dPoint(m->m_V[i]), SnapKind::Vertex});
    const ON_MeshTopology& top = m->Topology();
    for (int i = 0; i < top.m_tope.Count(); i++) {
      const ON_MeshTopologyEdge& e = top.m_tope[i];
      d.wires.push_back({ON_3dPoint(top.m_topv[e.m_topvi[0]].m_v_count ? m->m_V[top.m_topv[e.m_topvi[0]].m_vi[0]] : ON_3fPoint::Origin),
                         ON_3dPoint(top.m_topv[e.m_topvi[1]].m_v_count ? m->m_V[top.m_topv[e.m_topvi[1]].m_vi[0]] : ON_3fPoint::Origin)});
    }
  } else if (const auto* s = ON_SubD::Cast(g)) {
    ON_SubDVertexIterator vit(*s);
    for (const ON_SubDVertex* v = vit.FirstVertex(); v; v = vit.NextVertex())
      d.snapPoints.push_back({ON_3dPoint(v->m_P), SnapKind::Vertex});
    // superficie limite e spigoli limite (come Rhino), calcolati una volta per firma
    auto it = m_meshCache.find(sig);
    auto ie = m_subdEdges.find(sig);
    std::shared_ptr<ON_Mesh> sm;
    std::shared_ptr<EdgeList> edges;
    if (it != m_meshCache.end() && ie != m_subdEdges.end()) {
      sm = it->second;
      edges = ie->second;
    } else {
      SubDLimitDisplay lim = m_engine.subdFlat ? BuildSubDFlat(*s) : BuildSubDLimit(*s, 3);
      sm = std::shared_ptr<ON_Mesh>(lim.mesh.release());
      edges = std::make_shared<EdgeList>(std::move(lim.edges));
      if (!sm) sm = std::shared_ptr<ON_Mesh>(s->GetControlNetMesh(nullptr, ON_SubDGetControlNetMeshPriority::Geometry));
    }
    newCache[sig] = sm;
    m_newSubdEdges[sig] = edges;
    for (const auto& w : *edges) d.wires.push_back(w);
    AddShaded(d, sm);
    if (sm) AddSilhouetteCandidates(*sm, d);
  } else if (const auto* dot = ON_TextDot::Cast(g)) {
    AnnotationLabel L;
    L.origin = dot->CenterPoint();
    L.text = ToUtf8(dot->PrimaryText());
    L.dot = true;
    d.labels.push_back(L);
    d.points.push_back(dot->CenterPoint());
    d.snapPoints.push_back({dot->CenterPoint(), SnapKind::Point});
  } else if (const auto* h = ON_Hatch::Cast(g)) {
    // contorni; il riempimento (solo motivo pieno) e' una superficie piana con fori, in mesh
    ON_SimpleArray<ON_Curve*> outer;
    std::vector<ON_SimpleArray<ON_Curve*>> inner;
    for (int i = 0; i < h->LoopCount(); i++) {
      std::unique_ptr<ON_Curve> c(h->LoopCurve3d(i));
      if (!c) continue;
      std::vector<ON_3dPoint> w;
      TessellateCurve(*c, w);
      d.wires.push_back(std::move(w));
      if (h->Loop(i)->Type() == ON_HatchLoop::ltOuter && outer.Count() == 0) outer.Append(c.release());
      else { inner.emplace_back(); inner.back().Append(c.release()); }
    }
    const auto* pat = ON_HatchPattern::Cast(
        m_engine.Model().ComponentFromIndex(ON_ModelComponent::Type::HatchPattern, h->PatternIndex()).ModelComponent());
    const bool solid = !pat || h->PatternIndex() < 0 || pat->FillType() == ON_HatchPattern::HatchFillType::Solid;
#ifdef CAD_WITH_OCCT
    if (solid && outer.Count() == 1) {
      auto it = m_meshCache.find(sig);
      std::shared_ptr<ON_Mesh> hm;
      if (it != m_meshCache.end()) hm = it->second;
      else {
        std::unique_ptr<ON_Brep> b(ON_BrepTrimmedPlane(h->Plane(), *outer[0]));
        if (b) {
          for (auto& in : inner) b->NewPlanarFaceLoop(0, ON_BrepLoop::inner, in, true);
          ON_BoundingBox bb;
          b->GetTightBoundingBox(bb);
          hm.reset(occt::MeshShape(occt::BrepToShape(*b, m_engine.Tolerance()), std::max(bb.Diagonal().Length() / 400.0, 0.01), 0.3));
        }
      }
      newCache[sig] = hm;
      if (hm) {
        // il riempimento si disegna come triangoli del colore dell'oggetto (anche in wireframe)
        for (int fi = 0; fi < hm->m_F.Count(); fi++) {
          const ON_MeshFace& f = hm->m_F[fi];
          d.triangles.push_back(ON_3dPoint(hm->m_V[f.vi[0]]));
          d.triangles.push_back(ON_3dPoint(hm->m_V[f.vi[1]]));
          d.triangles.push_back(ON_3dPoint(hm->m_V[f.vi[2]]));
          if (f.IsQuad()) {
            d.triangles.push_back(ON_3dPoint(hm->m_V[f.vi[0]]));
            d.triangles.push_back(ON_3dPoint(hm->m_V[f.vi[2]]));
            d.triangles.push_back(ON_3dPoint(hm->m_V[f.vi[3]]));
          }
        }
      }
    }
#endif
    for (int i = 0; i < outer.Count(); i++) delete outer[i];
    for (auto& in : inner) for (int i = 0; i < in.Count(); i++) delete in[i];
  } else if (const auto* an = ON_Annotation::Cast(g)) {
    AnnotationDisplay ad;
    BuildAnnotationDisplay(m_engine.Model(), *an, ad);
    for (auto& w : ad.wires) {
      d.snapPoints.push_back({w.front(), SnapKind::End});
      d.wires.push_back(std::move(w));
    }
    for (auto& l : ad.labels) d.labels.push_back(std::move(l));
    d.triangles.insert(d.triangles.end(), ad.triangles.begin(), ad.triangles.end());
    d.snapPoints.push_back({an->Plane().origin, SnapKind::Point});
  } else if (const auto* mc = ON_MorphControl::Cast(g)) {
    // Gabbia di CageEdit: reticolo dei punti di controllo (visibile e selezionabile anche a punti spenti)
    if (mc->m_varient != 3) return;
    const ON_NurbsCage& c = mc->m_nurbs_cage;
    const int n[3] = {c.CVCount(0), c.CVCount(1), c.CVCount(2)};
    for (int dir = 0; dir < 3; dir++)
      for (int a = 0; a < n[(dir + 1) % 3]; a++)
        for (int b = 0; b < n[(dir + 2) % 3]; b++) {
          std::vector<ON_3dPoint> w;
          for (int k = 0; k < n[dir]; k++) {
            int idx[3];
            idx[dir] = k; idx[(dir + 1) % 3] = a; idx[(dir + 2) % 3] = b;
            ON_3dPoint q;
            c.GetCV(idx[0], idx[1], idx[2], q);
            w.push_back(q);
            if (dir == 0) d.snapPoints.push_back({q, SnapKind::Point});
          }
          d.wires.push_back(std::move(w));
        }
  } else if (const auto* ir = ON_InstanceRef::Cast(g)) {
    // Blocco: geometria della definizione trasformata (anche blocchi annidati)
    if (depth > 8) return;
    const auto* idef = ON_InstanceDefinition::Cast(
        m_engine.Model().ComponentFromId(ON_ModelComponent::Type::InstanceDefinition, ir->m_instance_definition_uuid).ModelComponent());
    if (!idef) return;
    const auto& ids = idef->InstanceGeometryIdList();
    for (int i = 0; i < ids.Count(); i++) {
      const auto* mo = ON_ModelGeometryComponent::Cast(
          m_engine.Model().ComponentFromId(ON_ModelComponent::Type::ModelGeometry, ids[i]).ModelComponent());
      const ON_Geometry* mg = mo ? mo->Geometry(nullptr) : nullptr;
      if (!mg) continue;
      std::unique_ptr<ON_Geometry> copy(static_cast<ON_Geometry*>(mg->Duplicate()));
      if (!copy || !copy->Transform(ir->m_xform)) continue;
      char mk[64];
      ON_UuidToString(ids[i], mk);
      std::string msig = sig + "/" + mk;
      for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) msig += "|" + std::to_string(ir->m_xform[r][c]);
      AddGeometry(copy.get(), d, msig, newCache, depth + 1);
    }
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
      std::shared_ptr<ON_Mesh> bm;
      if (it != m_meshCache.end()) bm = it->second;
      else {
        ON_BoundingBox bb;
        b->GetTightBoundingBox(bb);
        const double size = bb.Diagonal().Length();
        TopoDS_Shape shape = occt::BrepToShape(*b, m_engine.Tolerance());
        bm.reset(occt::MeshShape(shape, std::max(size / 400.0, 0.01), 15.0 * ON_PI / 180.0, false));
      }
      newCache[sig] = bm;
      AddShaded(d, bm);
      if (bm) AddSilhouetteCandidates(*bm, d);
#endif
    }
  }
}

std::string Scene::textureCacheDir;

void Scene::SetPreview(const std::vector<std::shared_ptr<const ON_Geometry>>& geometry) {
  m_preview.clear();
  std::map<std::string, std::shared_ptr<ON_Mesh>> newCache;
  // le mesh gia' calcolate (stessa geometria) si riusano tra un aggiornamento e l'altro
  std::swap(m_meshCache, m_previewCache);
  for (const auto& g : geometry) {
    if (!g) continue;
    DisplayObject d;
    g->GetTightBoundingBox(d.bbox);
    std::string sig = std::string("preview|") + g->ClassId()->ClassName() + "|" + std::to_string(g->DataCRC(0));
    for (int i = 0; i < 3; i++) sig += "|" + std::to_string(d.bbox.m_min[i]) + "|" + std::to_string(d.bbox.m_max[i]);
    AddGeometry(g.get(), d, sig, newCache, 0);
    d.snapPoints.clear();
    m_preview.push_back(std::move(d));
  }
  std::swap(m_meshCache, m_previewCache);
  m_previewCache = std::move(newCache);
  m_version++;
}

void Scene::Rebuild() {
  m_version++;
  m_objects.clear();
  m_clips.clear();
  std::map<std::string, std::shared_ptr<ON_Mesh>> newCache;
  ONX_Model& model = m_engine.Model();
  {  // grandezza dei simboli delle luci: dal modello
    ON_BoundingBox b;
    for (const auto& id : m_engine.ObjectIdsInSpace(ON_nil_uuid)) {
      const auto* o = m_engine.Object(id);
      const ON_Geometry* g = o ? o->Geometry(nullptr) : nullptr;
      if (g && !ON_Light::Cast(g)) { ON_BoundingBox t; if (g->GetTightBoundingBox(t)) b.Union(t); }
    }
    m_lightScale = b.IsValid() ? b.Diagonal().Length() : 100.0;
  }
  for (const auto& id : fixedSpace ? m_engine.ObjectIdsInSpace(space) : m_engine.AllObjectIds(false)) {
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
    std::string sig = std::string(key) + g->ClassId()->ClassName() + (m_engine.subdFlat ? "#flat" : "");
    for (int i = 0; i < 3; i++) sig += "|" + std::to_string(d.bbox.m_min[i]) + "|" + std::to_string(d.bbox.m_max[i]);
    // contenuto: una modifica che lascia uguale il riquadro (pieghe SubD, punti interni) rigenera la mesh
    if (const auto* sd = ON_SubD::Cast(g)) sig += "|" + std::string(static_cast<const char*>(sd->GeometryHash().ToUTF8String(false)));
    else sig += "|" + std::to_string(g->DataCRC(0));

    if (const auto* L = ON_Light::Cast(g)) {
      // luci: simbolo nella posizione (stella), direzione o cono
      const ON_3dPoint p = L->Location();
      const double s = std::max(1.0, 0.02 * std::max(1e-6, m_lightScale));
      for (const ON_3dVector& ax : {ON_3dVector::XAxis, ON_3dVector::YAxis, ON_3dVector::ZAxis})
        d.wires.push_back({p - ax * s, p + ax * s});
      if (L->IsDirectionalLight() || L->IsSpotLight()) {
        ON_3dVector dir = L->Direction();
        const double len = L->IsSpotLight() ? dir.Length() : 8 * s;
        if (dir.Unitize()) {
          const ON_3dPoint tip = p + dir * len;
          d.wires.push_back({p, tip});
          if (L->IsSpotLight()) {
            ON_3dVector a, b;
            a.PerpendicularTo(dir);
            a.Unitize();
            b = ON_CrossProduct(dir, a);
            const double r = len * std::tan(L->SpotAngleRadians());
            std::vector<ON_3dPoint> circ;
            for (int i = 0; i <= 32; i++) {
              const double t = 2 * ON_PI * i / 32;
              circ.push_back(tip + a * (r * std::cos(t)) + b * (r * std::sin(t)));
            }
            d.wires.push_back(circ);
            for (int i = 0; i < 4; i++) d.wires.push_back({p, circ[i * 8]});
          }
        }
      }
      if (L->IsRectangularLight()) {
        const ON_3dVector u = L->Length(), v = L->Width();
        d.wires.push_back({p, p + u, p + u + v, p + v, p});
      }
      d.snapPoints.push_back({p, SnapKind::Point});
      m_objects.push_back(std::move(d));
      continue;
    }
    if (const auto* dv = ON_DetailView::Cast(g)) {
      // dettaglio di una tavola: il bordo (il contenuto lo disegna la tavola)
      std::vector<ON_3dPoint> w;
      TessellateCurve(dv->m_boundary, w);
      for (const auto& q : w) d.snapPoints.push_back({q, SnapKind::End});
      d.wires.push_back(w);
      m_objects.push_back(std::move(d));
      continue;
    }
    if (const auto* cp = ON_ClippingPlaneSurface::Cast(g)) {
      // piano di sezione: rettangolo con la freccia della direzione di vista (lato visibile)
      const ON_Plane& pl = cp->m_clipping_plane.m_plane;
      const ON_Interval du = cp->Extents(0), dv = cp->Extents(1);
      std::vector<ON_3dPoint> r;
      for (auto [u, v] : {std::pair{du[0], dv[0]}, {du[1], dv[0]}, {du[1], dv[1]}, {du[0], dv[1]}, {du[0], dv[0]}})
        r.push_back(pl.PointAt(u, v));
      d.wires.push_back(r);
      const double len = 0.25 * std::min(du.Length(), dv.Length());
      const ON_3dPoint tip = pl.origin - pl.zaxis * len;
      d.wires.push_back({pl.origin, tip});
      d.wires.push_back({tip, tip + (pl.zaxis + pl.xaxis * 0.5) * len * 0.3});
      d.wires.push_back({tip, tip + (pl.zaxis - pl.xaxis * 0.5) * len * 0.3});
      for (const auto& q : r) d.snapPoints.push_back({q, SnapKind::End});
      d.unclipped = true;
      if (cp->m_clipping_plane.m_bEnabled) {
        ClipPlane c;
        c.eq.Create(pl.origin, pl.zaxis);
        for (int i = 0; i < cp->m_clipping_plane.m_viewport_ids.Count(); i++)
          c.views.push_back(cp->m_clipping_plane.m_viewport_ids.Array()[i]);
        m_clips.push_back(c);
      }
      m_objects.push_back(std::move(d));
      continue;
    }
    AddGeometry(g, d, sig, newCache, 0);
    if (d.shaded) {
      ApplyTexture(*a, d);
      int mi = a->MaterialSource() == ON::material_from_object ? a->m_material_index : (layer ? layer->RenderMaterialIndex() : -1);
      if (const auto* mat = mi >= 0 ? ON_Material::Cast(model.ComponentFromIndex(ON_ModelComponent::Type::RenderMaterial, mi).ModelComponent()) : nullptr) {
        d.material = mat->IsPhysicallyBased() ? ON_Color(mat->PhysicallyBased()->BaseColor()) : mat->Diffuse();
        d.hasMaterial = true;
      }
    }
    if (m_engine.pointsOn.count(id)) {
      GripSet gs;
      if (GetGrips(g, gs, m_engine.editPointsOn.count(id) > 0)) { d.grips = std::move(gs.grips); d.cage = std::move(gs.cage); }
    }
    m_objects.push_back(std::move(d));
  }
  m_meshCache.swap(newCache);
  m_subdEdges.swap(m_newSubdEdges);
  m_newSubdEdges.clear();
  RefreshSelection();
}

// Coordinate di texture della mesh ombreggiata secondo la mappatura dell'oggetto.
void Scene::ApplyTexture(const ON_3dmObjectAttributes& a, DisplayObject& d) {
  ObjectTexture t;
  if (!GetObjectTexture(m_engine.Model(), a, t)) return;
  d.texture = ResolveTextureFile(m_engine.Model(), t.file, textureCacheDir);
  if (d.texture.empty()) return;
  d.diffuse = t.diffuse;
  const ON_Mesh& m = *d.shaded;
  const int n = m.m_V.Count();
  const bool hasN = m.m_N.Count() == n;
  ON_TextureMapping map = t.mapping;
  const bool surface = !t.hasMapping || map.m_type == ON_TextureMapping::TYPE::srfp_mapping;
  if (surface && m.m_T.Count() != n) {
    // niente parametri di superficie (mesh, SubD): box sull'ingombro
    ON_BoundingBox bb = d.bbox;
    const ON_3dVector dv = bb.m_max - bb.m_min;
    map.SetBoxMapping(ON_Plane(bb.m_min, ON_3dVector::XAxis, ON_3dVector::YAxis), ON_Interval(0, std::max(dv.x, 1e-6)),
                      ON_Interval(0, std::max(dv.y, 1e-6)), ON_Interval(0, std::max(dv.z, 1e-6)), true);
  }
  d.uv.resize(n);
  for (int i = 0; i < n; i++) {
    ON_3dPoint T(0, 0, 0);
    if (surface && m.m_T.Count() == n) T = ON_3dPoint(m.m_T[i].x, m.m_T[i].y, 0);
    else map.Evaluate(ON_3dPoint(m.m_V[i]), hasN ? ON_3dVector(m.m_N[i]) : ON_3dVector::ZAxis, &T);
    T = t.uvw * T;
    d.uv[i] = ON_2fPoint(static_cast<float>(T.x), static_cast<float>(T.y));
  }
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
