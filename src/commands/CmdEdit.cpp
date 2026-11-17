// Comandi di modifica (menu Modifica di Rhino 8).
#include "commands/Engine.h"
#include <memory>

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

// Join: unisce le curve selezionate che si toccano entro la tolleranza.
bool CmdJoin(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("Join");
  if (sel.empty()) return false;
  ON_SimpleArray<const ON_Curve*> in;
  std::vector<ON_UUID> inIds;
  for (const auto& id : sel) {
    const auto* o = e.Object(id);
    if (const ON_Curve* c = ON_Curve::Cast(o ? o->Geometry(nullptr) : nullptr)) {
      in.Append(c);
      inIds.push_back(id);
    }
  }
  if (in.Count() < 2) return e.Fail("seleziona almeno 2 curve");
  ON_SimpleArray<ON_Curve*> out;
  ON_SimpleArray<int> key;
  ON_JoinCurves(in, out, e.Tolerance(), false, &key);
  if (out.Count() >= in.Count()) {
    for (int i = 0; i < out.Count(); i++) delete out[i];
    return e.Fail("nessuna curva unita");
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
  e.Register({"Join", "Modifica", "Unisce curve contigue", CmdJoin});
  e.Register({"Explode", "Modifica", "Esplode policurve, polilinee, polisuperfici", CmdExplode});
  e.Register({"Flip", "Modifica", "Inverte direzione di curve e normali", CmdFlip});
  e.Register({"Hide", "Visibilita'", "Nasconde gli oggetti selezionati", CmdHide});
  e.Register({"Show", "Visibilita'", "Mostra gli oggetti nascosti", CmdShow});
  e.Register({"Lock", "Visibilita'", "Blocca gli oggetti selezionati", CmdLock});
  e.Register({"Unlock", "Visibilita'", "Sblocca gli oggetti bloccati", CmdUnlock});
  e.Register({"DupBorder", "Curve", "Copia i bordi liberi di superfici e polisuperfici come curve", CmdDupBorder});
}

}  // namespace cad
