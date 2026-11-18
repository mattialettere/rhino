// Comandi che richiedono il motore geometrico OpenCASCADE:
// booleane, raccordi, smussi, offset, loft, tubi, sweep, intersezioni,
// divisione, mesh, aree e volumi.
#include "commands/Engine.h"
#include "kernel/OcctConvert.h"
#include "python/PythonHost.h"

#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepAlgoAPI_Section.hxx>
#include <BRepAlgoAPI_Splitter.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepGProp.hxx>
#include <BRepOffsetAPI_MakeOffset.hxx>
#include <BRepOffsetAPI_MakeOffsetShape.hxx>
#include <BRepOffsetAPI_MakePipeShell.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRep_Tool.hxx>
#include <GC_MakeCircle.hxx>
#include <GProp_GProps.hxx>
#include <ShapeAnalysis_FreeBounds.hxx>
#include <ShapeFix_Wire.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopoDS_Wire.hxx>
#include <cmath>
#include <memory>
#include <set>
#include <sstream>

namespace cad {
namespace {

using occt::GeometryToShape;
using occt::ShapeToBrep;

const ON_Geometry* Geo(Engine& e, const ON_UUID& id) {
  const auto* o = e.Object(id);
  return o ? o->Geometry(nullptr) : nullptr;
}

ON_3dmObjectAttributes Attrs(Engine& e, const ON_UUID& id) {
  return *e.Object(id)->Attributes(&ON_3dmObjectAttributes::DefaultAttributes);
}

bool IsSurfaceLike(const ON_Geometry* g) {
  return ON_Brep::Cast(g) || ON_Extrusion::Cast(g) || ON_Surface::Cast(g);
}

// Shape di tutti gli oggetti indicati (solo superfici/polisuperfici).
std::vector<TopoDS_Shape> Shapes(Engine& e, const std::vector<ON_UUID>& ids, std::vector<ON_UUID>* used = nullptr) {
  std::vector<TopoDS_Shape> out;
  for (const auto& id : ids) {
    const ON_Geometry* g = Geo(e, id);
    if (!g || !IsSurfaceLike(g)) continue;
    TopoDS_Shape s = GeometryToShape(*g, e.Tolerance());
    if (s.IsNull()) continue;
    out.push_back(s);
    if (used) used->push_back(id);
  }
  return out;
}

// Aggiunge al modello ogni pezzo "di primo livello" del risultato.
int AddShapeParts(Engine& e, const TopoDS_Shape& result, const ON_3dmObjectAttributes& a) {
  int n = 0;
  std::vector<TopoDS_Shape> parts;
  if (result.ShapeType() == TopAbs_COMPOUND) {
    for (TopoDS_Iterator it(result); it.More(); it.Next()) parts.push_back(it.Value());
  } else {
    parts.push_back(result);
  }
  for (auto& p : parts) {
    if (p.ShapeType() == TopAbs_EDGE || p.ShapeType() == TopAbs_WIRE) {
      for (ON_Curve* c : occt::ShapeToCurves(p, e.Tolerance())) { e.AddObject(c, &a); n++; }
      continue;
    }
    if (p.ShapeType() == TopAbs_VERTEX) {
      gp_Pnt q = BRep_Tool::Pnt(TopoDS::Vertex(p));
      e.AddObject(new ON_Point(ON_3dPoint(q.X(), q.Y(), q.Z())), &a);
      n++;
      continue;
    }
    if (ON_Brep* b = ShapeToBrep(p, e.Tolerance())) { e.AddObject(b, &a); n++; }
  }
  return n;
}

bool Replace(Engine& e, const std::vector<ON_UUID>& inputs, const TopoDS_Shape& result, const char* what) {
  if (result.IsNull()) return e.Fail(std::string(what) + " non riuscita");
  ON_3dmObjectAttributes a = Attrs(e, inputs.front());
  const int n = AddShapeParts(e, result, a);
  if (!n) return e.Fail(std::string(what) + ": risultato vuoto");
  for (const auto& id : inputs) e.DeleteObject(id);
  e.Out() << "  " << what << ": " << n << " oggetti risultanti\n";
  return true;
}

// --- booleane ---------------------------------------------------------
bool CmdBooleanUnion(Engine& e, TokenStream&) {
  std::vector<ON_UUID> used;
  auto shapes = Shapes(e, e.RequireSelection("BooleanUnion"), &used);
  if (shapes.size() < 2) return e.Fail("seleziona almeno 2 superfici o polisuperfici");
  TopoDS_Shape acc = shapes[0];
  for (size_t i = 1; i < shapes.size(); i++) {
    BRepAlgoAPI_Fuse op(acc, shapes[i]);
    if (!op.IsDone()) return e.Fail("unione booleana non riuscita");
    acc = op.Shape();
  }
  return Replace(e, used, acc, "Unione booleana");
}

// Il primo oggetto selezionato e' quello da cui sottrarre, gli altri sottraggono.
bool CmdBooleanDifference(Engine& e, TokenStream&) {
  std::vector<ON_UUID> used;
  auto shapes = Shapes(e, e.RequireSelection("BooleanDifference"), &used);
  if (shapes.size() < 2) return e.Fail("seleziona prima l'oggetto da cui sottrarre, poi gli oggetti da sottrarre");
  TopoDS_Shape acc = shapes[0];
  for (size_t i = 1; i < shapes.size(); i++) {
    BRepAlgoAPI_Cut op(acc, shapes[i]);
    if (!op.IsDone()) return e.Fail("differenza booleana non riuscita");
    acc = op.Shape();
  }
  return Replace(e, used, acc, "Differenza booleana");
}

bool CmdBooleanIntersection(Engine& e, TokenStream&) {
  std::vector<ON_UUID> used;
  auto shapes = Shapes(e, e.RequireSelection("BooleanIntersection"), &used);
  if (shapes.size() < 2) return e.Fail("seleziona almeno 2 oggetti");
  TopoDS_Shape acc = shapes[0];
  for (size_t i = 1; i < shapes.size(); i++) {
    BRepAlgoAPI_Common op(acc, shapes[i]);
    if (!op.IsDone()) return e.Fail("intersezione booleana non riuscita");
    acc = op.Shape();
  }
  return Replace(e, used, acc, "Intersezione booleana");
}

// --- raccordi e smussi (su tutti gli spigoli degli oggetti selezionati) ---
bool EdgeOp(Engine& e, TokenStream& ts, bool fillet) {
  const char* name = fillet ? "FilletEdge" : "ChamferEdge";
  auto sel = e.RequireSelection(name, "seleziona i solidi o le polisuperfici, poi Invio");
  if (sel.empty()) return false;
  auto r = e.ReadNumber(ts, fillet ? "raggio" : "distanza");
  if (!r) return false;
  if (*r <= e.Tolerance()) return e.Fail("valore troppo piccolo");
  // Spigoli cliccati (come Rhino); con Invio senza clic: tutti gli spigoli
  std::vector<ON_3dPoint> picks;
  while (e.MoreInput(ts)) {
    auto p = e.ReadPick(ts, "clicca gli spigoli (Invio = fine; senza clic = tutti)");
    if (!p) return false;
    picks.push_back(*p);
  }
  e.SkipEnter(ts);
  struct Obj { ON_UUID id; TopoDS_Shape shape; TopTools_IndexedMapOfShape edges; std::set<int> chosen; };
  std::vector<Obj> objs;
  for (const auto& id : sel) {
    std::vector<ON_UUID> one{id};
    auto shapes = Shapes(e, one);
    if (shapes.empty()) continue;
    Obj o{id, shapes[0], {}, {}};
    TopExp::MapShapes(o.shape, TopAbs_EDGE, o.edges);
    objs.push_back(std::move(o));
  }
  if (objs.empty()) return e.Fail("nessuna polisuperficie selezionata");
  for (const ON_3dPoint& p : picks) {
    TopoDS_Shape v = BRepBuilderAPI_MakeVertex(gp_Pnt(p.x, p.y, p.z));
    double best = 1e300;
    int bo = -1, be = -1;
    for (size_t k = 0; k < objs.size(); k++)
      for (int i = 1; i <= objs[k].edges.Extent(); i++) {
        if (BRep_Tool::Degenerated(TopoDS::Edge(objs[k].edges(i)))) continue;
        BRepExtrema_DistShapeShape d(v, objs[k].edges(i));
        if (d.IsDone() && d.Value() < best) { best = d.Value(); bo = int(k); be = i; }
      }
    if (bo >= 0) objs[bo].chosen.insert(be);
  }
  int n = 0;
  for (auto& o : objs) {
    if (!picks.empty() && o.chosen.empty()) continue;
    std::vector<ON_UUID> one{o.id};
    TopoDS_Shape result;
    auto useEdge = [&](int i) {
      return !BRep_Tool::Degenerated(TopoDS::Edge(o.edges(i))) && (picks.empty() || o.chosen.count(i));
    };
    try {
      if (fillet) {
        BRepFilletAPI_MakeFillet mk(o.shape);
        for (int i = 1; i <= o.edges.Extent(); i++) if (useEdge(i)) mk.Add(*r, TopoDS::Edge(o.edges(i)));
        mk.Build();
        if (mk.IsDone()) result = mk.Shape();
      } else {
        BRepFilletAPI_MakeChamfer mk(o.shape);
        for (int i = 1; i <= o.edges.Extent(); i++) if (useEdge(i)) mk.Add(*r, TopoDS::Edge(o.edges(i)));
        mk.Build();
        if (mk.IsDone()) result = mk.Shape();
      }
    } catch (...) {
    }
    if (result.IsNull()) return e.Fail(std::string(name) + " non riuscito (raggio troppo grande?)");
    if (!Replace(e, one, result, fillet ? "Raccordo" : "Smusso")) return false;
    n++;
  }
  return n > 0 || e.Fail("nessuno spigolo trovato vicino ai punti cliccati");
}
bool CmdFilletEdge(Engine& e, TokenStream& ts) { return EdgeOp(e, ts, true); }
bool CmdChamferEdge(Engine& e, TokenStream& ts) { return EdgeOp(e, ts, false); }

// --- offset -------------------------------------------------------------
// Wire con un edge per ogni tratto liscio della curva (spezzata agli spigoli),
// come serve agli algoritmi di offset e sweep di OpenCASCADE.
TopoDS_Wire WireOf(const ON_Curve& c, bool splitClosed = false) {
  std::vector<double> breaks{c.Domain()[0]};
  double t = c.Domain()[0];
  int hint = 0;
  while (c.GetNextDiscontinuity(ON::continuity::G1_locus_continuous, t, c.Domain()[1], &t, &hint))
    if (t > breaks.back()) breaks.push_back(t);
  if (breaks.back() < c.Domain()[1]) breaks.push_back(c.Domain()[1]);
  // Curva chiusa in un solo tratto (es. cerchio): due edge, piu' robusto per l'offset.
  if (splitClosed && breaks.size() == 2 && c.IsClosed()) breaks.insert(breaks.begin() + 1, c.Domain().ParameterAt(0.5));
  BRepBuilderAPI_MakeWire mw;
  for (size_t i = 0; i + 1 < breaks.size(); i++) {
    std::unique_ptr<ON_Curve> span(c.DuplicateCurve());
    if (!span->Trim(ON_Interval(breaks[i], breaks[i + 1]))) continue;
    TopoDS_Shape s = occt::CurveToShape(*span);
    if (!s.IsNull()) mw.Add(TopoDS::Edge(s));
  }
  return mw.IsDone() ? mw.Wire() : TopoDS_Wire();
}

double DistanceToShape(const TopoDS_Shape& s, const ON_3dPoint& p) {
  double best = 1e300;
  for (ON_Curve* c : occt::ShapeToCurves(s, 0.0)) {
    for (int i = 0; i <= 200; i++) {
      ON_3dPoint q = c->PointAt(c->Domain().ParameterAt(i / 200.0));
      best = std::min(best, q.DistanceTo(p));
    }
    delete c;
  }
  return best;
}

// Offset: distanza, poi facoltativo punto dal lato voluto (come in Rhino).
bool CmdOffset(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("Offset");
  if (sel.empty()) return false;
  auto opts = e.ReadOptions(ts);
  double dist;
  if (opts.count("distance")) dist = std::atof(opts["distance"].c_str());
  else { auto d = e.ReadNumber(ts, "distanza"); if (!d) return false; dist = *d; }
  std::optional<ON_3dPoint> side;
  if (!ts.AtEnd() && ts.Peek().find(',') != std::string::npos) side = e.ReadPoint(ts, "lato dell'offset");
  int n = 0;
  for (const auto& id : sel) {
    const ON_Curve* c = ON_Curve::Cast(Geo(e, id));
    if (!c) continue;
    ON_Plane plane;
    if (!c->IsPlanar(&plane, e.Tolerance())) { e.Out() << "  curva non planare ignorata\n"; continue; }
    TopoDS_Wire w = WireOf(*c, true);
    auto make = [&](double d) -> TopoDS_Shape {
      try {
        BRepOffsetAPI_MakeOffset mo;
        mo.Init(GeomAbs_Arc, !c->IsClosed());
        if (c->IsClosed()) {
          // Curva chiusa: offset sulla faccia piana che delimita (verso interno/esterno).
          BRepBuilderAPI_MakeFace mf(w, Standard_True);
          if (!mf.IsDone()) return TopoDS_Shape();
          mo.Init(mf.Face(), GeomAbs_Arc, Standard_False);
        } else {
          mo.AddWire(w);
        }
        mo.Perform(d);
        return mo.IsDone() ? mo.Shape() : TopoDS_Shape();
      } catch (...) { return TopoDS_Shape(); }
    };
    TopoDS_Shape r = make(std::fabs(dist));
    if (side) {
      TopoDS_Shape r2 = make(-std::fabs(dist));
      if (!r2.IsNull() && (r.IsNull() || DistanceToShape(r2, *side) < DistanceToShape(r, *side))) r = r2;
    } else if (dist < 0) {
      r = make(dist);
    }
    if (r.IsNull()) continue;
    ON_3dmObjectAttributes a = Attrs(e, id);
    std::vector<ON_Curve*> curves = occt::ShapeToCurves(r, e.Tolerance());
    // Unisce i pezzi in un'unica curva, come fa Rhino.
    ON_SimpleArray<const ON_Curve*> in;
    for (auto* cc : curves) in.Append(cc);
    ON_SimpleArray<ON_Curve*> joined;
    ON_JoinCurves(in, joined, e.Tolerance() * 10, false);
    for (auto* cc : curves) delete cc;
    for (int i = 0; i < joined.Count(); i++) { e.AddObject(joined[i], &a); n++; }
  }
  if (!n) return e.Fail("offset non riuscito");
  e.Out() << "  Create " << n << " curve di offset\n";
  return true;
}

// OffsetSrf: offset di superfici/polisuperfici; opzione Solid=Yes.
bool CmdOffsetSrf(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("OffsetSrf");
  if (sel.empty()) return false;
  auto d = e.ReadNumber(ts, "distanza");
  if (!d) return false;
  int n = 0;
  for (const auto& id : sel) {
    std::vector<ON_UUID> one{id};
    auto shapes = Shapes(e, one);
    if (shapes.empty()) continue;
    BRepOffsetAPI_MakeOffsetShape mk;
    try {
      mk.PerformByJoin(shapes[0], *d, e.Tolerance());
    } catch (...) { continue; }
    if (!mk.IsDone()) continue;
    ON_3dmObjectAttributes a = Attrs(e, id);
    n += AddShapeParts(e, mk.Shape(), a);
  }
  (void)opts;
  if (!n) return e.Fail("offset di superficie non riuscito");
  e.Out() << "  Create " << n << " superfici di offset\n";
  return true;
}

// --- loft, tubi, sweep ----------------------------------------------------
// Loft: curve nell'ordine di selezione. Opzioni Style=Straight, Closed=Yes.
bool CmdLoft(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("Loft");
  std::vector<TopoDS_Wire> wires;
  for (const auto& id : sel)
    if (const ON_Curve* c = ON_Curve::Cast(Geo(e, id))) wires.push_back(WireOf(*c));
  if (wires.size() < 2) return e.Fail("seleziona almeno 2 curve (nell'ordine del loft)");
  const bool ruled = opts.count("style") && opts["style"] == "straight";
  BRepOffsetAPI_ThruSections mk(Standard_False, ruled ? Standard_True : Standard_False, e.Tolerance());
  for (auto& w : wires) mk.AddWire(w);
  mk.CheckCompatibility(Standard_True);
  try { mk.Build(); } catch (...) {}
  if (!mk.IsDone()) return e.Fail("loft non riuscito");
  ON_Brep* b = ShapeToBrep(mk.Shape(), e.Tolerance());
  if (!b) return e.Fail("loft: conversione non riuscita");
  ON_3dmObjectAttributes a = Attrs(e, sel.front());
  e.AddObject(b, &a);
  e.Out() << "  Creata superficie di loft\n";
  return true;
}

TopoDS_Shape SweepCircle(const TopoDS_Wire& spine, double radius, bool cap) {
  // Profilo circolare perpendicolare all'inizio della guida.
  TopExp_Explorer ex(spine, TopAbs_EDGE);
  if (!ex.More()) return {};
  double f, l;
  Handle(Geom_Curve) gc = BRep_Tool::Curve(TopoDS::Edge(ex.Current()), f, l);
  gp_Pnt p; gp_Vec v;
  gc->D1(f, p, v);
  if (ex.Current().Orientation() == TopAbs_REVERSED) { gc->D1(l, p, v); v.Reverse(); }
  Handle(Geom_Circle) circ = GC_MakeCircle(gp_Ax2(p, gp_Dir(v)), radius).Value();
  TopoDS_Wire prof = BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(circ).Edge()).Wire();
  BRepOffsetAPI_MakePipeShell mk(spine);
  mk.Add(prof);
  mk.Build();
  if (!mk.IsDone()) return {};
  if (cap) mk.MakeSolid();
  return mk.Shape();
}

// Pipe: raggio costante lungo le curve selezionate; Cap=Yes (predefinito) chiude.
bool CmdPipe(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  const bool cap = !(opts.count("cap") && (opts["cap"] == "none" || opts["cap"] == "no"));
  auto sel = e.RequireSelection("Pipe");
  if (sel.empty()) return false;
  auto r = e.ReadNumber(ts, "raggio");
  if (!r) return false;
  int n = 0;
  for (const auto& id : sel) {
    const ON_Curve* c = ON_Curve::Cast(Geo(e, id));
    if (!c) continue;
    TopoDS_Shape s;
    try { s = SweepCircle(WireOf(*c), *r, cap && !c->IsClosed()); } catch (...) {}
    if (s.IsNull()) continue;
    ON_3dmObjectAttributes a = Attrs(e, id);
    n += AddShapeParts(e, s, a);
  }
  if (!n) return e.Fail("tubo non riuscito");
  e.Out() << "  Creati " << n << " tubi\n";
  return true;
}

// Sweep1: la prima curva selezionata e' il binario, le altre i profili.
bool CmdSweep1(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("Sweep1");
  if (sel.size() < 2) return e.Fail("seleziona prima il binario, poi uno o piu' profili");
  const ON_Curve* rail = ON_Curve::Cast(Geo(e, sel[0]));
  if (!rail) return e.Fail("il primo oggetto deve essere una curva");
  BRepOffsetAPI_MakePipeShell mk(WireOf(*rail));
  for (size_t i = 1; i < sel.size(); i++)
    if (const ON_Curve* p = ON_Curve::Cast(Geo(e, sel[i]))) mk.Add(WireOf(*p));
  try { mk.Build(); } catch (...) {}
  if (!mk.IsDone()) return e.Fail("sweep non riuscito");
  ON_Brep* b = ShapeToBrep(mk.Shape(), e.Tolerance());
  if (!b) return e.Fail("sweep: conversione non riuscita");
  ON_3dmObjectAttributes a = Attrs(e, sel[0]);
  e.AddObject(b, &a);
  e.Out() << "  Creata superficie di sweep\n";
  return true;
}

// --- intersezioni e divisione -----------------------------------------------
bool CmdIntersect(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("Intersect");
  if (sel.size() < 2) return e.Fail("seleziona almeno 2 oggetti");
  std::vector<TopoDS_Shape> shapes;
  for (const auto& id : sel)
    if (const ON_Geometry* g = Geo(e, id)) shapes.push_back(GeometryToShape(*g, e.Tolerance()));
  int n = 0;
  ON_3dmObjectAttributes a = Attrs(e, sel.front());
  for (size_t i = 0; i < shapes.size(); i++)
    for (size_t j = i + 1; j < shapes.size(); j++) {
      if (shapes[i].IsNull() || shapes[j].IsNull()) continue;
      BRepAlgoAPI_Section sec(shapes[i], shapes[j], Standard_False);
      sec.Approximation(Standard_True);
      sec.Build();
      if (!sec.IsDone()) continue;
      TopoDS_Shape r = sec.Shape();
      bool anyEdge = false;
      for (TopExp_Explorer ex(r, TopAbs_EDGE); ex.More(); ex.Next()) anyEdge = true;
      if (anyEdge) {
        std::vector<ON_Curve*> curves = occt::ShapeToCurves(r, e.Tolerance());
        ON_SimpleArray<const ON_Curve*> in;
        for (auto* c : curves) in.Append(c);
        ON_SimpleArray<ON_Curve*> joined;
        ON_JoinCurves(in, joined, e.Tolerance() * 10, false);
        for (auto* c : curves) delete c;
        for (int k = 0; k < joined.Count(); k++) { e.AddObject(joined[k], &a); n++; }
      } else {
        for (TopExp_Explorer ex(r, TopAbs_VERTEX); ex.More(); ex.Next()) {
          gp_Pnt q = BRep_Tool::Pnt(TopoDS::Vertex(ex.Current()));
          e.AddObject(new ON_Point(ON_3dPoint(q.X(), q.Y(), q.Z())), &a);
          n++;
        }
      }
    }
  e.Out() << "  Creati " << n << " oggetti di intersezione\n";
  return true;
}

// Split: il primo oggetto selezionato viene diviso dagli altri.
bool CmdSplit(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("Split");
  if (sel.size() < 2) return e.Fail("seleziona prima l'oggetto da dividere, poi gli oggetti di taglio");
  const ON_Geometry* g = Geo(e, sel[0]);
  if (!g) return false;
  TopoDS_Shape obj = GeometryToShape(*g, e.Tolerance());
  TopTools_ListOfShape args, tools;
  args.Append(obj);
  for (size_t i = 1; i < sel.size(); i++)
    if (const ON_Geometry* t = Geo(e, sel[i])) {
      TopoDS_Shape s = GeometryToShape(*t, e.Tolerance());
      if (!s.IsNull()) tools.Append(s);
    }
  BRepAlgoAPI_Splitter sp;
  sp.SetArguments(args);
  sp.SetTools(tools);
  sp.Build();
  if (!sp.IsDone()) return e.Fail("divisione non riuscita");
  std::vector<ON_UUID> one{sel[0]};
  // Ogni faccia (o edge) risultante diventa un oggetto, come in Rhino per le superfici.
  TopoDS_Shape r = sp.Shape();
  ON_3dmObjectAttributes a = Attrs(e, sel[0]);
  int n = 0;
  if (ON_Curve::Cast(g)) {
    for (ON_Curve* c : occt::ShapeToCurves(r, e.Tolerance())) { e.AddObject(c, &a); n++; }
  } else {
    TopAbs_ShapeEnum level = TopAbs_SOLID;
    bool hasSolid = false;
    for (TopExp_Explorer ex(r, TopAbs_SOLID); ex.More(); ex.Next()) hasSolid = true;
    if (!hasSolid) level = TopAbs_FACE;
    for (TopExp_Explorer ex(r, level); ex.More(); ex.Next())
      if (ON_Brep* b = ShapeToBrep(ex.Current(), e.Tolerance())) { e.AddObject(b, &a); n++; }
  }
  if (n < 2) return e.Fail("gli oggetti di taglio non dividono l'oggetto");
  e.DeleteObject(sel[0]);
  e.Out() << "  Diviso in " << n << " parti\n";
  return true;
}

// --- mesh, area, volume -------------------------------------------------------
bool CmdMesh(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("Mesh");
  if (sel.empty()) return false;
  const double defl = opts.count("maxdistance") ? std::atof(opts["maxdistance"].c_str()) : 0.1;
  int n = 0;
  for (const auto& id : sel) {
    const ON_Geometry* g = Geo(e, id);
    if (!g || !IsSurfaceLike(g)) continue;
    TopoDS_Shape s = GeometryToShape(*g, e.Tolerance());
    if (ON_Mesh* m = occt::MeshShape(s, defl, 20.0 * ON_PI / 180.0)) {
      ON_3dmObjectAttributes a = Attrs(e, id);
      e.AddObject(m, &a);
      n++;
    }
  }
  if (!n) return e.Fail("nessuna superficie da convertire in mesh");
  e.Out() << "  Create " << n << " mesh\n";
  return true;
}

bool Props(Engine& e, bool volume) {
  auto sel = e.RequireSelection(volume ? "Volume" : "Area");
  if (sel.empty()) return false;
  double total = 0;
  for (const auto& id : sel) {
    const ON_Geometry* g = Geo(e, id);
    if (!g) continue;
    TopoDS_Shape s = GeometryToShape(*g, e.Tolerance());
    if (s.IsNull()) continue;
    GProp_GProps p;
    // Integrazione adattiva con errore relativo 1e-9 (come la precisione di Rhino).
    if (volume) BRepGProp::VolumeProperties(s, p, 1e-9);
    else if (ON_Curve::Cast(g)) continue;
    else BRepGProp::SurfaceProperties(s, p, 1e-9);
    total += p.Mass();
  }
  std::ostringstream os;
  os.precision(10);
  os << (volume ? "  Volume = " : "  Area = ") << total << (volume ? " mm3" : " mm2") << "\n";
  e.Out() << os.str();
  return true;
}
bool CmdArea(Engine& e, TokenStream&) { return Props(e, false); }

bool MassPropsImpl(Engine& e, const ON_UUID& id, bool volume, double& value, ON_3dPoint& centroid);
bool MassProps(Engine& e, const ON_UUID& id, bool volume, double& value, ON_3dPoint& centroid) {
  try {
    return MassPropsImpl(e, id, volume, value, centroid);
  } catch (...) {
    return false;
  }
}
bool MassPropsImpl(Engine& e, const ON_UUID& id, bool volume, double& value, ON_3dPoint& centroid) {
  const ON_Geometry* g = Geo(e, id);
  if (!g) return false;
  TopoDS_Shape s = GeometryToShape(*g, e.Tolerance());
  if (s.IsNull()) return false;
  GProp_GProps p;
  if (volume) BRepGProp::VolumeProperties(s, p, 1e-9);
  else if (ON_Curve::Cast(g)) BRepGProp::LinearProperties(s, p);
  else BRepGProp::SurfaceProperties(s, p, 1e-9);
  value = p.Mass();
  const gp_Pnt c = p.CentreOfMass();
  centroid = ON_3dPoint(c.X(), c.Y(), c.Z());
  return true;
}
bool CmdVolume(Engine& e, TokenStream&) { return Props(e, true); }

}  // namespace

void RegisterKernelCommands(Engine& e) {
  auto reg = [&](const char* n, const char* cat, const char* d, CommandFn f, bool mod = true) {
    CommandInfo i{n, cat, d, f};
    i.modifiesModel = mod;
    e.Register(i);
  };
  reg("BooleanUnion", "Solidi", "Unione booleana degli oggetti selezionati", CmdBooleanUnion);
  reg("BooleanDifference", "Solidi", "Differenza: primo selezionato meno gli altri", CmdBooleanDifference);
  reg("BooleanIntersection", "Solidi", "Intersezione booleana", CmdBooleanIntersection);
  reg("FilletEdge", "Solidi", "Raccordo di tutti gli spigoli (raggio)", CmdFilletEdge);
  reg("ChamferEdge", "Solidi", "Smusso di tutti gli spigoli (distanza)", CmdChamferEdge);
  reg("Offset", "Curve", "Offset di curve planari (distanza, lato)", CmdOffset);
  reg("OffsetSrf", "Superfici", "Offset di superfici (distanza)", CmdOffsetSrf);
  MassPropertiesFn = MassProps;
  reg("Loft", "Superfici", "Loft tra curve nell'ordine di selezione", CmdLoft);
  reg("Pipe", "Solidi", "Tubo di raggio costante lungo curve", CmdPipe);
  reg("Sweep1", "Superfici", "Sweep su un binario (primo selezionato)", CmdSweep1);
  reg("Intersect", "Curve", "Curve/punti di intersezione", CmdIntersect);
  reg("Split", "Modifica", "Divide il primo oggetto con gli altri", CmdSplit);
  reg("Mesh", "Mesh", "Mesh da superfici e polisuperfici", CmdMesh);
  reg("Area", "Analisi", "Area totale degli oggetti selezionati", CmdArea, false);
  reg("Volume", "Analisi", "Volume totale dei solidi selezionati", CmdVolume, false);
}

}  // namespace cad
