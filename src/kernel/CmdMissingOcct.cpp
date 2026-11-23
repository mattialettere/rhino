// Comandi di Rhino 8 aggiunti dopo il confronto con Rhino 8, con OpenCASCADE:
// CreateSolid, IntersectTwoSets, MeshSplit, MeshTrim.
#include "commands/Engine.h"
#include "kernel/OcctConvert.h"
#include <BOPAlgo_MakerVolume.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepAlgoAPI_Section.hxx>
#include <BRepAlgoAPI_Splitter.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <GeomAPI_PointsToBSpline.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <vector>

namespace cad {

// Curve 3D degli spigoli ricalcolate dalla superficie e dalla curva di taglio del primo trim
// (dopo che la superficie e' cambiata, i tagli in parametri restano validi).
void RebuildBrepEdges(ON_Brep& b) {
  for (int ei = 0; ei < b.m_E.Count(); ei++) {
    ON_BrepEdge& E = b.m_E[ei];
    if (E.m_ti.Count() < 1) continue;
    const ON_BrepTrim& T = b.m_T[E.m_ti[0]];
    const ON_Surface* S = T.SurfaceOf();
    if (!S || !T.TrimCurveOf()) continue;
    const ON_Interval ed = E.Domain(), td = T.Domain();
    const int n = 40;
    TColgp_Array1OfPnt pts(1, n + 1);
    for (int k = 0; k <= n; k++) {
      const double s = double(k) / n;
      const ON_2dPoint uv = T.PointAt(T.m_bRev3d ? td.ParameterAt(1 - s) : td.ParameterAt(s));
      const ON_3dPoint q = S->PointAt(uv.x, uv.y);
      pts.SetValue(k + 1, gp_Pnt(q.x, q.y, q.z));
    }
    try {
      GeomAPI_PointsToBSpline fit(pts, 3, 8, GeomAbs_C2, 1e-6);
      if (!fit.IsDone()) continue;
      ON_NurbsCurve* nc = occt::FromOcct(fit.Curve());
      if (!nc) continue;
      nc->SetDomain(ed[0], ed[1]);
      const int c3 = b.AddEdgeCurve(nc);
      E.m_c3i = c3;
      E.SetProxyCurve(b.m_C3[c3], ed);
    } catch (...) {
    }
  }
  // vertici sugli estremi dei nuovi spigoli
  for (int vi = 0; vi < b.m_V.Count(); vi++) {
    ON_BrepVertex& V = b.m_V[vi];
    for (int k = 0; k < V.m_ei.Count(); k++) {
      const ON_BrepEdge& E = b.m_E[V.m_ei[k]];
      V.point = E.m_vi[0] == vi ? E.PointAtStart() : E.PointAtEnd();
      break;
    }
  }
  b.SetTolerancesBoxesAndFlags(true, true, true, true, true, true, true, true);
}

namespace {

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

bool NearestBrepEdge(Engine& e, const ON_3dPoint& p, ON_UUID& id, int& ei, double& t, const ON_UUID* only = nullptr,
                     int skipEdge = -1) {
  double best = 1e300;
  for (const auto& oid : e.AllObjectIds(false)) {
    if (only && ON_UuidCompare(*only, oid) != 0) continue;
    const auto* b = ON_Brep::Cast(e.Object(oid) ? e.Object(oid)->Geometry(nullptr) : nullptr);
    if (!b) continue;
    for (int i = 0; i < b->m_E.Count(); i++) {
      if (i == skipEdge) continue;
      if (b->m_E[i].BoundingBox().MinimumDistanceTo(p) > best) continue;
      const double tt = ClosestParam(b->m_E[i], p);
      const double d = b->m_E[i].PointAt(tt).DistanceTo(p);
      if (d < best) { best = d; id = oid; ei = i; t = tt; }
    }
  }
  return best < 1e300;
}

const ON_Geometry* Geo(Engine& e, const ON_UUID& id) {
  const auto* o = e.Object(id);
  return o ? o->Geometry(nullptr) : nullptr;
}

gp_Pnt G(const ON_3dPoint& p) { return gp_Pnt(p.x, p.y, p.z); }
ON_3dPoint O(const gp_Pnt& p) { return ON_3dPoint(p.X(), p.Y(), p.Z()); }

ON_3dmObjectAttributes Attrs(Engine& e, const ON_UUID& id) {
  const auto* o = e.Object(id);
  ON_3dmObjectAttributes a;
  if (o && o->Attributes(nullptr)) a = *o->Attributes(nullptr);
  a.m_uuid = ON_nil_uuid;
  a.RemoveFromAllGroups();
  return a;
}

// Mesh di openNURBS -> shape a facce triangolari (o quadrilatere piane) che condividono gli spigoli.
TopoDS_Shape MeshToShape(const ON_Mesh& m) {
  std::vector<TopoDS_Vertex> V(m.m_V.Count());
  for (int i = 0; i < m.m_V.Count(); i++) V[i] = BRepBuilderAPI_MakeVertex(G(ON_3dPoint(m.m_V[i])));
  std::map<std::pair<int, int>, TopoDS_Edge> E;
  auto edge = [&](int a, int b) {
    const auto key = std::make_pair(std::min(a, b), std::max(a, b));
    auto it = E.find(key);
    if (it == E.end()) it = E.emplace(key, BRepBuilderAPI_MakeEdge(V[key.first], V[key.second]).Edge()).first;
    TopoDS_Edge ed = it->second;
    if (a != key.first) ed.Reverse();
    return ed;
  };
  BRep_Builder bb;
  TopoDS_Shell sh;
  bb.MakeShell(sh);
  int n = 0;
  auto addFace = [&](const std::vector<int>& f) {
    // facce degeneri saltate
    for (size_t k = 0; k < f.size(); k++)
      if (f[k] == f[(k + 1) % f.size()] || ON_3dPoint(m.m_V[f[k]]).DistanceTo(ON_3dPoint(m.m_V[f[(k + 1) % f.size()]])) < 1e-12) return;
    BRepBuilderAPI_MakeWire w;
    for (size_t k = 0; k < f.size(); k++) w.Add(edge(f[k], f[(k + 1) % f.size()]));
    if (!w.IsDone()) return;
    BRepBuilderAPI_MakeFace mf(w.Wire(), Standard_True);
    if (!mf.IsDone()) return;
    bb.Add(sh, mf.Face());
    n++;
  };
  for (int i = 0; i < m.m_F.Count(); i++) {
    const ON_MeshFace& f = m.m_F[i];
    if (f.IsTriangle()) addFace({f.vi[0], f.vi[1], f.vi[2]});
    else {
      addFace({f.vi[0], f.vi[1], f.vi[2]});
      addFace({f.vi[0], f.vi[2], f.vi[3]});
    }
  }
  return n ? TopoDS_Shape(sh) : TopoDS_Shape();
}

TopoDS_Shape ShapeOf(Engine& e, const ON_UUID& id) {
  const ON_Geometry* g = Geo(e, id);
  if (!g) return {};
  if (const auto* m = ON_Mesh::Cast(g)) return MeshToShape(*m);
  try { return occt::GeometryToShape(*g, e.Tolerance()); } catch (...) { return {}; }
}

double DistanceTo(const TopoDS_Shape& s, const ON_3dPoint& p) {
  if (s.IsNull()) return 1e300;
  BRepExtrema_DistShapeShape d(BRepBuilderAPI_MakeVertex(G(p)).Vertex(), s);
  return d.IsDone() && d.NbSolution() ? d.Value() : 1e300;
}

// --- CreateSolid -----------------------------------------------------------------
// Solido chiuso dalle superfici e polisuperfici che racchiudono un volume (anche se si
// intersecano e sporgono: le parti fuori vengono tolte). Cells=Yes tiene separate le celle.
bool CmdCreateSolid(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts, {"cells"});
  auto sel = e.RequireSelection("CreateSolid", "seleziona le superfici che racchiudono il volume, poi Invio");
  if (sel.empty()) return false;
  opts.merge(e.ReadOptions(ts, {"cells"}));
  const bool cells = opts.count("cells") && opts["cells"] != "no";
  const double tol = e.Tolerance();
  TopTools_ListOfShape args;
  for (const auto& id : sel) {
    TopoDS_Shape s = ShapeOf(e, id);
    if (!s.IsNull()) args.Append(s);
  }
  if (args.IsEmpty()) return e.Fail("nessuna superficie");
  // primo tentativo: cucitura (superfici che si incontrano gia' sui bordi)
  std::vector<TopoDS_Shape> solids;
  {
    BRepBuilderAPI_Sewing sew(tol * 10);
    for (TopTools_ListIteratorOfListOfShape it(args); it.More(); it.Next()) sew.Add(it.Value());
    sew.Perform();
    for (TopExp_Explorer ex(sew.SewedShape(), TopAbs_SHELL); ex.More(); ex.Next()) {
      const TopoDS_Shell sh = TopoDS::Shell(ex.Current());
      if (!BRep_Tool::IsClosed(sh)) { solids.clear(); break; }
      BRepBuilderAPI_MakeSolid ms(sh);
      if (ms.IsDone()) solids.push_back(ms.Solid());
    }
  }
  if (solids.empty()) {
    BOPAlgo_MakerVolume mv;
    mv.SetArguments(args);
    mv.SetFuzzyValue(tol);
    mv.SetIntersect(Standard_True);
    mv.Perform();
    if (mv.HasErrors()) return e.Fail("le superfici non racchiudono un volume");
    for (TopExp_Explorer ex(mv.Shape(), TopAbs_SOLID); ex.More(); ex.Next()) solids.push_back(ex.Current());
    if (solids.empty()) return e.Fail("le superfici non racchiudono un volume");
    if (!cells && solids.size() > 1) {
      // celle unite in un solo solido (le pareti interne spariscono)
      TopoDS_Shape acc = solids[0];
      for (size_t i = 1; i < solids.size(); i++) {
        BRepAlgoAPI_Fuse fu(acc, solids[i]);
        if (fu.IsDone()) acc = fu.Shape();
      }
      ShapeUpgrade_UnifySameDomain un(acc, Standard_True, Standard_True, Standard_True);
      un.Build();
      solids = {un.Shape()};
    }
  }
  const ON_3dmObjectAttributes a = Attrs(e, sel.front());
  int n = 0;
  for (const auto& s : solids)
    if (ON_Brep* b = occt::ShapeToBrep(s, tol)) { e.AddObject(b, &a); n++; }
  if (!n) return e.Fail("solido non creato");
  if (!(opts.count("deleteinput") && opts["deleteinput"] == "no"))
    for (const auto& id : sel) e.DeleteObject(id);
  e.Out() << "  Creati " << n << " solidi\n";
  return true;
}

// --- IntersectTwoSets ------------------------------------------------------------
// Intersezioni solo tra gli oggetti del primo gruppo (selezione) e quelli del secondo
// (scelti con un clic ciascuno, Invio per finire).
bool CmdIntersectTwoSets(Engine& e, TokenStream& ts) {
  auto first = e.RequireSelection("IntersectTwoSets", "seleziona il primo gruppo di oggetti, poi Invio");
  if (first.empty()) return false;
  std::vector<ON_UUID> second;
  while (true) {
    auto p = e.ReadPick(ts, second.empty() ? "oggetto del secondo gruppo" : "altro oggetto del secondo gruppo (Invio per finire)");
    if (!p) { if (e.NeedsInput()) return false; break; }
    ON_UUID best = ON_nil_uuid;
    double bd = 1e300;
    for (const auto& id : e.AllObjectIds(false)) {
      if (std::find(first.begin(), first.end(), id) != first.end()) continue;
      const ON_Geometry* g = Geo(e, id);
      if (!g || !g->BoundingBox().IsValid()) continue;
      if (g->BoundingBox().MinimumDistanceTo(*p) > bd) continue;
      const double d = DistanceTo(ShapeOf(e, id), *p);
      if (d < bd) { bd = d; best = id; }
    }
    if (!ON_UuidIsNil(best) && std::find(second.begin(), second.end(), best) == second.end()) second.push_back(best);
    if (!e.MoreInput(ts)) break;
  }
  e.SkipEnter(ts);
  if (second.empty()) return e.Fail("nessun oggetto nel secondo gruppo");
  const double tol = e.Tolerance();
  int n = 0;
  const ON_3dmObjectAttributes a = Attrs(e, first.front());
  for (const auto& ia : first)
    for (const auto& ib : second) {
      TopoDS_Shape A = ShapeOf(e, ia), B = ShapeOf(e, ib);
      if (A.IsNull() || B.IsNull()) continue;
      BRepAlgoAPI_Section sec(A, B, Standard_False);
      sec.Approximation(Standard_True);
      sec.Build();
      if (!sec.IsDone()) continue;
      const TopoDS_Shape r = sec.Shape();
      bool anyEdge = false;
      for (TopExp_Explorer ex(r, TopAbs_EDGE); ex.More(); ex.Next()) anyEdge = true;
      if (anyEdge) {
        std::vector<ON_Curve*> curves = occt::ShapeToCurves(r, tol);
        ON_SimpleArray<const ON_Curve*> in;
        for (auto* c : curves) in.Append(c);
        ON_SimpleArray<ON_Curve*> joined;
        ON_JoinCurves(in, joined, tol * 10, false);
        for (auto* c : curves) delete c;
        for (int k = 0; k < joined.Count(); k++) { e.AddObject(joined[k], &a); n++; }
      } else {
        for (TopExp_Explorer ex(r, TopAbs_VERTEX); ex.More(); ex.Next()) {
          e.AddObject(new ON_Point(O(BRep_Tool::Pnt(TopoDS::Vertex(ex.Current())))), &a);
          n++;
        }
      }
    }
  e.Out() << "  Creati " << n << " oggetti di intersezione\n";
  return true;
}

// --- MeshSplit / MeshTrim --------------------------------------------------------
// La mesh diventa una shape a facce piane che condividono gli spigoli; OpenCASCADE la divide
// con gli oggetti di taglio; le parti sono gli insiemi di facce collegate da spigoli che non
// stanno sul taglio.
constexpr int kMaxSplitFaces = 20000;

struct Pieces {
  std::vector<TopoDS_Compound> parts;
};

bool SplitMesh(Engine& e, const ON_Mesh& mesh, const std::vector<ON_UUID>& cutters, Pieces& out) {
  if (mesh.m_F.Count() > kMaxSplitFaces) return e.Fail("mesh troppo grande per la divisione (massimo 20000 facce)");
  const double tol = e.Tolerance();
  TopoDS_Shape M = MeshToShape(mesh);
  if (M.IsNull()) return e.Fail("mesh vuota");
  TopTools_ListOfShape args, tools;
  args.Append(M);
  TopoDS_Compound cutAll;
  BRep_Builder bb;
  bb.MakeCompound(cutAll);
  for (const auto& id : cutters) {
    TopoDS_Shape s = ShapeOf(e, id);
    if (s.IsNull()) continue;
    tools.Append(s);
    bb.Add(cutAll, s);
  }
  if (tools.IsEmpty()) return e.Fail("nessun oggetto di taglio");
  BRepAlgoAPI_Splitter sp;
  sp.SetArguments(args);
  sp.SetTools(tools);
  sp.SetFuzzyValue(tol);
  sp.Build();
  if (!sp.IsDone()) return e.Fail("divisione non riuscita");
  const TopoDS_Shape R = sp.Shape();
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(R, TopAbs_FACE, faces);
  TopTools_IndexedDataMapOfShapeListOfShape e2f;
  TopExp::MapShapesAndAncestors(R, TopAbs_EDGE, TopAbs_FACE, e2f);
  // spigoli sul taglio: il loro punto medio sta sugli oggetti di taglio
  std::vector<bool> cutEdge(e2f.Extent() + 1, false);
  for (int i = 1; i <= e2f.Extent(); i++) {
    const TopoDS_Edge ed = TopoDS::Edge(e2f.FindKey(i));
    double f, l;
    Handle(Geom_Curve) c = BRep_Tool::Curve(ed, f, l);
    if (c.IsNull()) continue;
    const ON_3dPoint mid = O(c->Value(0.5 * (f + l)));
    cutEdge[i] = DistanceTo(cutAll, mid) <= tol * 2;
  }
  std::vector<int> comp(faces.Extent() + 1, -1);
  int nc = 0;
  for (int f0 = 1; f0 <= faces.Extent(); f0++) {
    if (comp[f0] >= 0) continue;
    std::vector<int> stack{f0};
    comp[f0] = nc;
    while (!stack.empty()) {
      const int f = stack.back();
      stack.pop_back();
      for (TopExp_Explorer ex(faces(f), TopAbs_EDGE); ex.More(); ex.Next()) {
        const int ei = e2f.FindIndex(ex.Current());
        if (ei <= 0 || cutEdge[ei]) continue;
        for (TopTools_ListIteratorOfListOfShape it(e2f(ei)); it.More(); it.Next()) {
          const int g = faces.FindIndex(it.Value());
          if (g > 0 && comp[g] < 0) { comp[g] = nc; stack.push_back(g); }
        }
      }
    }
    nc++;
  }
  out.parts.assign(nc, TopoDS_Compound());
  for (auto& c : out.parts) bb.MakeCompound(c);
  for (int f = 1; f <= faces.Extent(); f++) bb.Add(out.parts[comp[f]], faces(f));
  return true;
}

ON_Mesh* PieceMesh(const TopoDS_Shape& s, double tol) {
  ON_Mesh* m = occt::MeshShape(s, tol, 0.5, true);
  if (m) {
    m->CombineIdenticalVertices(true, true);
    m->ComputeVertexNormals();
    m->Compact();
  }
  return m;
}

// MeshSplit: divide le mesh selezionate con gli oggetti di taglio (scelti con un clic ciascuno).
bool CmdMeshSplit(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("MeshSplit", "seleziona le mesh da dividere, poi Invio");
  if (sel.empty()) return false;
  std::vector<ON_UUID> cutters;
  while (true) {
    auto p = e.ReadPick(ts, cutters.empty() ? "oggetto di taglio" : "altro oggetto di taglio (Invio per finire)");
    if (!p) { if (e.NeedsInput()) return false; break; }
    ON_UUID best = ON_nil_uuid;
    double bd = 1e300;
    for (const auto& id : e.AllObjectIds(false)) {
      if (std::find(sel.begin(), sel.end(), id) != sel.end()) continue;
      const ON_Geometry* g = Geo(e, id);
      if (!g || g->BoundingBox().MinimumDistanceTo(*p) > bd) continue;
      const double d = DistanceTo(ShapeOf(e, id), *p);
      if (d < bd) { bd = d; best = id; }
    }
    if (!ON_UuidIsNil(best) && std::find(cutters.begin(), cutters.end(), best) == cutters.end()) cutters.push_back(best);
    if (!e.MoreInput(ts)) break;
  }
  e.SkipEnter(ts);
  if (cutters.empty()) return e.Fail("nessun oggetto di taglio");
  int n = 0;
  for (const auto& id : sel) {
    const auto* m = ON_Mesh::Cast(Geo(e, id));
    if (!m) continue;
    Pieces pc;
    if (!SplitMesh(e, *m, cutters, pc)) return false;
    if (pc.parts.size() < 2) continue;
    const ON_3dmObjectAttributes a = Attrs(e, id);
    for (const auto& c : pc.parts)
      if (ON_Mesh* pm = PieceMesh(c, e.Tolerance())) { e.AddObject(pm, &a); n++; }
    e.DeleteObject(id);
  }
  if (!n) return e.Fail("gli oggetti di taglio non dividono le mesh");
  e.Out() << "  Mesh divise in " << n << " parti\n";
  return true;
}

// MeshTrim: toglie la parte di mesh cliccata, delimitata dagli oggetti di taglio selezionati.
bool CmdMeshTrim(Engine& e, TokenStream& ts) {
  auto cutters = e.RequireSelection("MeshTrim", "seleziona gli oggetti di taglio, poi Invio");
  if (cutters.empty()) return false;
  auto p = e.ReadPick(ts, "parte di mesh da togliere");
  if (!p) return false;
  // mesh piu' vicina al punto
  ON_UUID mid = ON_nil_uuid;
  double bd = 1e300;
  for (const auto& id : e.AllObjectIds(false)) {
    if (std::find(cutters.begin(), cutters.end(), id) != cutters.end()) continue;
    const auto* m = ON_Mesh::Cast(Geo(e, id));
    if (!m) continue;
    if (m->BoundingBox().MinimumDistanceTo(*p) > bd) continue;
    const double d = DistanceTo(MeshToShape(*m), *p);
    if (d < bd) { bd = d; mid = id; }
  }
  if (ON_UuidIsNil(mid)) return e.Fail("nessuna mesh");
  const auto* m = ON_Mesh::Cast(Geo(e, mid));
  Pieces pc;
  if (!SplitMesh(e, *m, cutters, pc)) return false;
  if (pc.parts.size() < 2) return e.Fail("gli oggetti di taglio non dividono la mesh");
  int drop = -1;
  double dd = 1e300;
  for (int i = 0; i < (int)pc.parts.size(); i++) {
    const double d = DistanceTo(pc.parts[i], *p);
    if (d < dd) { dd = d; drop = i; }
  }
  BRep_Builder bb;
  TopoDS_Compound keep;
  bb.MakeCompound(keep);
  for (int i = 0; i < (int)pc.parts.size(); i++)
    if (i != drop) bb.Add(keep, pc.parts[i]);
  ON_Mesh* km = PieceMesh(keep, e.Tolerance());
  if (!km || km->m_F.Count() == 0) { delete km; return e.Fail("non resta niente della mesh"); }
  e.ReplaceObject(mid, km);
  e.Out() << "  Tolta una parte della mesh\n";
  return true;
}

// --- SplitEdge / JoinEdge / RebuildEdges ----------------------------------------
// SplitEdge: lo spigolo viene diviso da un vertice nel punto scelto (stesso solido, un vertice in piu').
bool CmdSplitEdge(Engine& e, TokenStream& ts) {
  auto p = e.ReadPick(ts, "punto dello spigolo in cui dividerlo");
  if (!p) return false;
  ON_UUID id;
  int ei;
  double t;
  if (!NearestBrepEdge(e, *p, id, ei, t)) return e.Fail("nessuno spigolo");
  const auto* b = ON_Brep::Cast(Geo(e, id));
  const ON_Interval d = b->m_E[ei].Domain();
  if (t <= d.ParameterAt(0.001) || t >= d.ParameterAt(0.999)) return e.Fail("punto troppo vicino a un estremo");
  const double tol = e.Tolerance();
  TopoDS_Shape S = occt::BrepToShape(*b, tol);
  TopTools_ListOfShape args, tools;
  args.Append(S);
  tools.Append(BRepBuilderAPI_MakeVertex(G(b->m_E[ei].PointAt(t))).Vertex());
  BRepAlgoAPI_Splitter sp;
  sp.SetArguments(args);
  sp.SetTools(tools);
  sp.SetFuzzyValue(tol);
  sp.Build();
  if (!sp.IsDone()) return e.Fail("divisione dello spigolo non riuscita");
  ON_Brep* nb = occt::ShapeToBrep(sp.Shape(), tol);
  if (!nb) return e.Fail("divisione dello spigolo non riuscita");
  const int before = b->m_E.Count();
  const int after = nb->m_E.Count();
  e.ReplaceObject(id, nb);
  e.Out() << "  Spigolo diviso (" << before << " -> " << after << " spigoli)\n";
  return true;
}

// JoinEdge: unisce due spigoli nudi vicini, con la tolleranza che serve per la loro distanza.
bool CmdJoinEdge(Engine& e, TokenStream& ts) {
  auto p1 = e.ReadPick(ts, "primo spigolo nudo");
  if (!p1) return false;
  auto p2 = e.ReadPick(ts, "secondo spigolo nudo");
  if (!p2) return false;
  ON_UUID id, id2;
  int e1, e2;
  double t1, t2;
  if (!NearestBrepEdge(e, *p1, id, e1, t1)) return e.Fail("nessuno spigolo");
  if (!NearestBrepEdge(e, *p2, id2, e2, t2, &id, e1)) return e.Fail("il secondo spigolo deve stare nello stesso oggetto (usa prima Join)");
  const auto* b = ON_Brep::Cast(Geo(e, id));
  if (e1 == e2) return e.Fail("scegli due spigoli diversi");
  if (b->m_E[e1].m_ti.Count() != 1 || b->m_E[e2].m_ti.Count() != 1) return e.Fail("gli spigoli devono essere nudi");
  const ON_BrepEdge& A = b->m_E[e1];
  const ON_BrepEdge& B = b->m_E[e2];
  double gap = 0;
  for (int k = 0; k <= 10; k++) {
    const ON_3dPoint q = A.PointAt(A.Domain().ParameterAt(k / 10.0));
    gap = std::max(gap, B.PointAt(ClosestParam(B, q)).DistanceTo(q));
  }
  const double tol = e.Tolerance();
  BRepBuilderAPI_Sewing sew(std::max(gap * 1.05, tol));
  sew.Add(occt::BrepToShape(*b, tol));
  sew.Perform();
  ON_Brep* nb = occt::ShapeToBrep(sew.SewedShape(), tol);
  if (!nb) return e.Fail("spigoli non uniti");
  int naked0 = 0, naked1 = 0;
  for (int i = 0; i < b->m_E.Count(); i++) if (b->m_E[i].m_ti.Count() == 1) naked0++;
  for (int i = 0; i < nb->m_E.Count(); i++) if (nb->m_E[i].m_ti.Count() == 1) naked1++;
  if (naked1 >= naked0) { delete nb; return e.Fail("spigoli non uniti"); }
  e.ReplaceObject(id, nb);
  e.Out() << "  Spigoli uniti (distanza massima " << gap << ")\n";
  return true;
}

// RebuildEdges: ricalcola le curve 3D degli spigoli dalle superfici e dai tagli.
bool CmdRebuildEdges(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("RebuildEdges", "seleziona le superfici o polisuperfici, poi Invio");
  if (sel.empty()) return false;
  int n = 0;
  for (const auto& id : sel) {
    const auto* b0 = ON_Brep::Cast(Geo(e, id));
    if (!b0) continue;
    ON_Brep* b = b0->Duplicate();
    RebuildBrepEdges(*b);
    b->Compact();
    e.ReplaceObject(id, b);
    n++;
  }
  if (!n) return e.Fail("nessuna polisuperficie");
  e.Out() << "  Spigoli ricostruiti in " << n << " oggetti\n";
  return true;
}

}  // namespace

void RegisterMissingOcctCommands(Engine& e) {
  e.Register(CommandInfo{"SplitEdge", "Solidi", "Divide uno spigolo nel punto scelto", CmdSplitEdge});
  e.Register(CommandInfo{"JoinEdge", "Solidi", "Unisce due spigoli nudi anche oltre la tolleranza", CmdJoinEdge});
  e.Register(CommandInfo{"RebuildEdges", "Solidi", "Ricostruisce le curve 3D degli spigoli dalle facce", CmdRebuildEdges});
  auto reg = [&](const char* n, const char* cat, const char* d, CommandFn f) { e.Register(CommandInfo{n, cat, d, f}); };
  reg("CreateSolid", "Solidi", "Solido dalle superfici che racchiudono un volume (Cells=Yes per celle separate)", CmdCreateSolid);
  reg("IntersectTwoSets", "Curve", "Intersezioni tra due gruppi di oggetti", CmdIntersectTwoSets);
  reg("MeshSplit", "Mesh", "Divide le mesh con altri oggetti", CmdMeshSplit);
  reg("MeshTrim", "Mesh", "Taglia la parte di mesh scelta con gli oggetti di taglio", CmdMeshTrim);
}

}  // namespace cad
