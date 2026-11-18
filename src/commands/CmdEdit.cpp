// Comandi di modifica (menu Modifica di Rhino 8).
#include "commands/Engine.h"
#include <memory>
#ifdef CAD_WITH_OCCT
#include "kernel/OcctConvert.h"
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRep_Tool.hxx>
#include <ShapeFix_Solid.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Iterator.hxx>
#endif

namespace cad {
namespace {

ON_3dmObjectAttributes AttrsOf(const ON_ModelGeometryComponent* o) {
  return *o->Attributes(&ON_3dmObjectAttributes::DefaultAttributes);
}

bool CmdDelete(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("Delete");
  if (sel.empty()) return false;
  int n = 0;
  for (const auto& id : sel) n += e.DeleteObject(id) ? 1 : 0;
  e.Out() << "  Eliminati " << n << " oggetti\n";
  return true;
}

// Join di mesh: una sola mesh con tutte le facce (come Rhino, senza saldare i vertici).
int JoinMeshes(Engine& e, const std::vector<ON_UUID>& sel) {
  std::vector<ON_UUID> ids;
  for (const auto& id : sel)
    if (const auto* o = e.Object(id); o && ON_Mesh::Cast(o->Geometry(nullptr))) ids.push_back(id);
  if (ids.size() < 2) return 0;
  auto* m = new ON_Mesh(*ON_Mesh::Cast(e.Object(ids[0])->Geometry(nullptr)));
  for (size_t i = 1; i < ids.size(); i++) m->Append(*ON_Mesh::Cast(e.Object(ids[i])->Geometry(nullptr)));
  const ON_3dmObjectAttributes a = AttrsOf(e.Object(ids[0]));
  for (const auto& id : ids) e.DeleteObject(id);
  e.AddObject(m, &a);
  e.Out() << "  " << ids.size() << " mesh unite in una\n";
  return (int)ids.size();
}

#ifdef CAD_WITH_OCCT
// Join di superfici e polisuperfici: cucitura dei bordi coincidenti entro la
// tolleranza; un guscio chiuso diventa un solido.
int JoinSurfaces(Engine& e, const std::vector<ON_UUID>& sel) {
  std::vector<ON_UUID> ids;
  BRepBuilderAPI_Sewing sew(e.Tolerance());
  int faces = 0;
  for (const auto& id : sel) {
    const auto* o = e.Object(id);
    const ON_Geometry* g = o ? o->Geometry(nullptr) : nullptr;
    if (!g || !(ON_Brep::Cast(g) || ON_Extrusion::Cast(g) || ON_Surface::Cast(g))) continue;
    TopoDS_Shape s = occt::GeometryToShape(*g, e.Tolerance());
    if (s.IsNull()) continue;
    for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next()) { sew.Add(ex.Current()); faces++; }
    ids.push_back(id);
  }
  if (ids.size() < 2) return 0;
  try { sew.Perform(); } catch (...) { return 0; }
  TopoDS_Shape r = sew.SewedShape();
  std::vector<TopoDS_Shape> parts;
  if (r.ShapeType() == TopAbs_COMPOUND) for (TopoDS_Iterator it(r); it.More(); it.Next()) parts.push_back(it.Value());
  else parts.push_back(r);
  if (parts.size() >= ids.size()) return 0;  // niente si e' unito
  const ON_3dmObjectAttributes a = AttrsOf(e.Object(ids[0]));
  std::vector<ON_Brep*> out;
  for (auto& p : parts) {
    TopoDS_Shape q = p;
    if (p.ShapeType() == TopAbs_SHELL && p.Closed()) {
      BRepBuilderAPI_MakeSolid ms(TopoDS::Shell(p));
      if (ms.IsDone()) { ShapeFix_Solid fix(ms.Solid()); fix.Perform(); q = fix.Solid(); }
    }
    if (ON_Brep* b = occt::ShapeToBrep(q, e.Tolerance())) out.push_back(b);
  }
  if (out.empty()) return 0;
  for (const auto& id : ids) e.DeleteObject(id);
  for (auto* b : out) e.AddObject(b, &a);
  e.Out() << "  " << ids.size() << " superfici unite in " << out.size() << " polisuperfici\n";
  return (int)ids.size();
}
#endif

// Join: unisce le curve selezionate che si toccano entro la tolleranza; le mesh
// in una mesh; le superfici e polisuperfici con bordi coincidenti in polisuperfici.
bool CmdJoin(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("Join");
  if (sel.empty()) return false;
  int joinedOther = JoinMeshes(e, sel);
#ifdef CAD_WITH_OCCT
  joinedOther += JoinSurfaces(e, sel);
#endif
  {
    // le curve rimaste nella selezione
    std::vector<ON_UUID> rest;
    for (const auto& id : sel) if (e.Object(id)) rest.push_back(id);
    sel = rest;
  }
  ON_SimpleArray<const ON_Curve*> in;
  std::vector<ON_UUID> inIds;
  for (const auto& id : sel) {
    const auto* o = e.Object(id);
    if (const ON_Curve* c = ON_Curve::Cast(o ? o->Geometry(nullptr) : nullptr)) {
      in.Append(c);
      inIds.push_back(id);
    }
  }
  if (in.Count() < 2) return joinedOther > 0 || e.Fail("seleziona almeno 2 curve, superfici o mesh");
  ON_SimpleArray<ON_Curve*> out;
  ON_SimpleArray<int> key;
  ON_JoinCurves(in, out, e.Tolerance(), false, &key);
  if (out.Count() >= in.Count()) {
    for (int i = 0; i < out.Count(); i++) delete out[i];
    return joinedOther > 0 || e.Fail("nessuna curva unita");
  }
  // Ogni risultato prende gli attributi della prima curva che lo compone.
  std::vector<ON_3dmObjectAttributes> attrs(out.Count());
  std::vector<bool> has(out.Count(), false);
  for (int i = 0; i < in.Count(); i++) {
    int k = key[i];
    if (k >= 0 && k < out.Count() && !has[k]) { attrs[k] = AttrsOf(e.Object(inIds[i])); has[k] = true; }
  }
  for (const auto& id : inIds) e.DeleteObject(id);
  for (int i = 0; i < out.Count(); i++) e.AddObject(out[i], has[i] ? &attrs[i] : nullptr);
  e.Out() << "  " << in.Count() << " curve unite in " << out.Count() << "\n";
  return true;
}

// Explode: policurve e polilinee in segmenti, polisuperfici in superfici.
bool CmdExplode(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("Explode");
  if (sel.empty()) return false;
  int exploded = 0, parts = 0;
  for (const auto& id : sel) {
    const auto* o = e.Object(id);
    const ON_Geometry* g = o ? o->Geometry(nullptr) : nullptr;
    if (!g) continue;
    std::vector<ON_Object*> pieces;
    if (const auto* pc = ON_PolyCurve::Cast(g)) {
      for (int i = 0; i < pc->Count(); i++) pieces.push_back(pc->SegmentCurve(i)->DuplicateCurve());
    } else if (const auto* pl = ON_PolylineCurve::Cast(g)) {
      for (int i = 0; i + 1 < pl->PointCount(); i++)
        pieces.push_back(new ON_LineCurve(pl->m_pline[i], pl->m_pline[i + 1]));
    } else {
      std::unique_ptr<ON_Brep> owned;
      const ON_Brep* b = ON_Brep::Cast(g);
      if (const auto* ex = ON_Extrusion::Cast(g)) { owned.reset(ex->BrepForm()); b = owned.get(); }
      if (b && b->m_F.Count() > 1)
        for (int i = 0; i < b->m_F.Count(); i++) pieces.push_back(b->DuplicateFace(i, false));
    }
    if (pieces.size() < 2) { for (auto* p : pieces) delete p; continue; }
    ON_3dmObjectAttributes a = AttrsOf(o);
    e.DeleteObject(id);
    for (auto* p : pieces) if (p) { e.AddObject(p, &a); parts++; }
    exploded++;
  }
  if (!exploded) return e.Fail("niente da esplodere");
  e.Out() << "  Esplosi " << exploded << " oggetti in " << parts << " parti\n";
  return true;
}

// Flip: inverte la direzione di curve e normali di superfici.
bool CmdFlip(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("Flip");
  if (sel.empty()) return false;
  int n = 0;
  for (const auto& id : sel) {
    ON_Geometry* g = e.EditableGeometry(id);
    if (auto* c = ON_Curve::Cast(g)) n += c->Reverse() ? 1 : 0;
    else if (auto* b = ON_Brep::Cast(g)) { b->Flip(); n++; }
    else if (auto* m = ON_Mesh::Cast(g)) { m->Flip(); n++; }
    else if (auto* s = ON_Surface::Cast(g)) n += s->Transpose() ? 1 : 0;
  }
  e.Out() << "  Invertiti " << n << " oggetti\n";
  return n > 0 || e.Fail("niente da invertire");
}

bool SetMode(Engine& e, const std::vector<ON_UUID>& ids, ON::object_mode mode) {
  int n = 0;
  for (const auto& id : ids)
    if (auto* a = e.EditableAttributes(id)) { a->SetMode(mode); n++; }
  return n > 0;
}

bool CmdHide(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("Hide");
  if (sel.empty()) return false;
  SetMode(e, sel, ON::object_mode::hidden_object);
  e.ClearSelection();
  e.Out() << "  Nascosti " << sel.size() << " oggetti\n";
  return true;
}

bool CmdShow(Engine& e, TokenStream&) {
  std::vector<ON_UUID> hidden;
  for (const auto& id : e.AllObjectIds(true))
    if (const auto* o = e.Object(id); o && o->Attributes(nullptr)->Mode() == ON::object_mode::hidden_object)
      hidden.push_back(id);
  SetMode(e, hidden, ON::object_mode::normal_object);
  e.Out() << "  Mostrati " << hidden.size() << " oggetti\n";
  return true;
}

bool CmdLock(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("Lock");
  if (sel.empty()) return false;
  SetMode(e, sel, ON::object_mode::locked_object);
  e.ClearSelection();
  e.Out() << "  Bloccati " << sel.size() << " oggetti\n";
  return true;
}

bool CmdUnlock(Engine& e, TokenStream&) {
  std::vector<ON_UUID> locked;
  for (const auto& id : e.AllObjectIds(true))
    if (const auto* o = e.Object(id); o && o->Attributes(nullptr)->Mode() == ON::object_mode::locked_object)
      locked.push_back(id);
  SetMode(e, locked, ON::object_mode::normal_object);
  e.Out() << "  Sbloccati " << locked.size() << " oggetti\n";
  return true;
}

// DupBorder: curve dei bordi liberi (spigoli "nudi") di superfici e polisuperfici.
bool CmdDupBorder(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("DupBorder", "seleziona le superfici o polisuperfici, poi Invio");
  if (sel.empty()) return false;
  int n = 0;
  std::vector<ON_UUID> created;
  for (const auto& id : sel) {
    const auto* o = e.Object(id);
    const ON_Geometry* g = o->Geometry(nullptr);
    std::unique_ptr<ON_Brep> owned;
    const ON_Brep* b = ON_Brep::Cast(g);
    if (!b) {
      if (const auto* x = ON_Extrusion::Cast(g)) owned.reset(x->BrepForm());
      else if (const auto* sf = ON_Surface::Cast(g)) owned.reset(sf->BrepForm());
      b = owned.get();
    }
    if (!b) continue;
    ON_SimpleArray<const ON_Curve*> naked;
    std::vector<std::unique_ptr<ON_Curve>> keep;
    for (int i = 0; i < b->m_E.Count(); i++) {
      const ON_BrepEdge& edge = b->m_E[i];
      if (edge.m_ti.Count() != 1) continue;
      keep.emplace_back(edge.DuplicateCurve());
      if (keep.back()) naked.Append(keep.back().get());
    }
    if (!naked.Count()) continue;
    ON_SimpleArray<ON_Curve*> joined;
    ON_JoinCurves(naked, joined, e.Tolerance(), false, nullptr);
    ON_3dmObjectAttributes a = AttrsOf(o);
    for (int i = 0; i < joined.Count(); i++) { e.AddObject(joined[i], &a); n++; }
  }
  if (!n) return e.Fail("nessun bordo libero: gli oggetti sono chiusi");
  e.Out() << "  Create " << n << " curve di bordo\n";
  return true;
}

}  // namespace

void RegisterEditCommands(Engine& e) {
  e.Register({"Delete", "Modifica", "Elimina gli oggetti selezionati", CmdDelete});
  e.Register({"Join", "Modifica", "Unisce curve, superfici o mesh contigue", CmdJoin});
  e.Register({"Explode", "Modifica", "Esplode policurve, polilinee, polisuperfici", CmdExplode});
  e.Register({"Flip", "Modifica", "Inverte direzione di curve e normali", CmdFlip});
  e.Register({"Hide", "Visibilita'", "Nasconde gli oggetti selezionati", CmdHide});
  e.Register({"Show", "Visibilita'", "Mostra gli oggetti nascosti", CmdShow});
  e.Register({"Lock", "Visibilita'", "Blocca gli oggetti selezionati", CmdLock});
  e.Register({"Unlock", "Visibilita'", "Sblocca gli oggetti bloccati", CmdUnlock});
  e.Register({"DupBorder", "Curve", "Copia i bordi liberi di superfici e polisuperfici come curve", CmdDupBorder});
}

}  // namespace cad
