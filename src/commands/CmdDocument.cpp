// Comandi di file, annulla, selezione, livelli e informazioni.
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include "commands/Engine.h"
#include "commands/Exchange.h"
#include <functional>
#include <sstream>

namespace cad {
namespace {

using Pred = std::function<bool(const ON_Geometry*)>;

std::string Unquote(std::string s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
  return s;
}

int FaceCount(const ON_Geometry* g) {
  if (const auto* b = ON_Brep::Cast(g)) return b->m_F.Count();
  if (const auto* x = ON_Extrusion::Cast(g)) return x->IsCapped() ? 3 : 1;  // >1 = polisuperficie
  return 0;
}

bool SelectWhere(Engine& e, const Pred& p, const char* what) {
  int n = 0;
  for (const auto& id : e.AllObjectIds()) {
    const auto* o = e.Object(id);
    if (o->Attributes(nullptr)->Mode() == ON::object_mode::locked_object) continue;
    if (p(o->Geometry(nullptr))) { e.Select(id); n++; }
  }
  e.Out() << "  Selezionati " << n << " " << what << "\n";
  return true;
}

bool CmdSelAll(Engine& e, TokenStream&) { return SelectWhere(e, [](auto) { return true; }, "oggetti"); }
bool CmdSelNone(Engine& e, TokenStream&) { e.ClearSelection(); return true; }
bool CmdSelLast(Engine& e, TokenStream&) {
  int n = 0;
  for (const auto& id : e.LastCreated())
    if (e.Object(id)) { e.Select(id); n++; }
  e.Out() << "  Selezionati " << n << " oggetti\n";
  return true;
}
bool CmdSelCrv(Engine& e, TokenStream&) {
  return SelectWhere(e, [](const ON_Geometry* g) { return ON_Curve::Cast(g) != nullptr; }, "curve");
}
bool CmdSelPt(Engine& e, TokenStream&) {
  return SelectWhere(e, [](const ON_Geometry* g) { return ON_Point::Cast(g) != nullptr; }, "punti");
}
bool CmdSelSrf(Engine& e, TokenStream&) {
  return SelectWhere(e, [](const ON_Geometry* g) {
    return ON_Surface::Cast(g) || (FaceCount(g) == 1);
  }, "superfici");
}
bool CmdSelPolysrf(Engine& e, TokenStream&) {
  return SelectWhere(e, [](const ON_Geometry* g) { return FaceCount(g) > 1; }, "polisuperfici");
}
bool CmdSelMesh(Engine& e, TokenStream&) {
  return SelectWhere(e, [](const ON_Geometry* g) { return ON_Mesh::Cast(g) != nullptr; }, "mesh");
}
bool CmdSelSubD(Engine& e, TokenStream&) {
  return SelectWhere(e, [](const ON_Geometry* g) { return ON_SubD::Cast(g) != nullptr; }, "SubD");
}
bool CmdInvert(Engine& e, TokenStream&) {
  auto sel = e.Selection();
  std::set<ON_UUID, UuidLess> old(sel.begin(), sel.end());
  e.ClearSelection();
  int n = 0;
  for (const auto& id : e.AllObjectIds())
    if (!old.count(id)) { e.Select(id); n++; }
  e.Out() << "  Selezionati " << n << " oggetti\n";
  return true;
}

// Percorso completo di un livello ("Padre::Figlio").
std::string LayerPath(const ONX_Model& m, const ON_Layer* l) {
  std::string path = ToUtf8(l->Name());
  ON_UUID parent = l->ParentLayerId();
  for (int d = 0; d < 64 && ON_UuidIsNotNil(parent); d++) {
    const auto* pl = ON_Layer::Cast(m.ComponentFromId(ON_ModelComponent::Type::Layer, parent).ModelComponent());
    if (!pl) break;
    path = ToUtf8(pl->Name()) + "::" + path;
    parent = pl->ParentLayerId();
  }
  return path;
}

// Livello per nome o per percorso completo ("Padre::Figlio"), senza distinguere maiuscole.
const ON_Layer* FindLayer(Engine& e, const std::string& name) {
  const std::string want = Unquote(name);
  const bool byPath = want.find("::") != std::string::npos;
  ON_wString w(want.c_str());
  const ON_Layer* found = nullptr;
  ONX_ModelComponentIterator it(e.Model(), ON_ModelComponent::Type::Layer);
  for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent()) {
    const auto* l = ON_Layer::Cast(c);
    if (!l || l->IsDeleted()) continue;
    if (byPath ? ON_wString::EqualOrdinal(ON_wString(LayerPath(e.Model(), l).c_str()), w, true)
               : ON_wString::EqualOrdinal(c->Name(), w, true)) {
      if (!byPath && ON_UuidIsNil(l->ParentLayerId())) return l;  // a parita' di nome vince quello di primo livello
      if (!found) found = l;
    }
  }
  return found;
}

ON_Layer* EditableLayer(Engine& e, const ON_Layer* l) {
  return l ? const_cast<ON_Layer*>(l) : nullptr;
}

bool ParseRgb(const std::string& v, ON_Color& c) {
  int r, g, b;
  if (std::sscanf(Unquote(v).c_str(), "%d,%d,%d", &r, &g, &b) != 3 || r < 0 || g < 0 || b < 0 || r > 255 || g > 255 || b > 255)
    return false;
  c = ON_Color(r, g, b);
  return true;
}

// Tipo di linea per nome: -1 = Continuous (predefinito), -2 = inesistente.
int FindLinetype(Engine& e, const std::string& name) {
  const std::string n = Unquote(name);
  ON_wString w(n.c_str());
  if (ON_wString::EqualOrdinal(w, L"Continuous", true) || ON_wString::EqualOrdinal(w, L"Continua", true)) return -1;
  ONX_ModelComponentIterator it(e.Model(), ON_ModelComponent::Type::LinePattern);
  for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent())
    if (!c->IsDeleted() && ON_wString::EqualOrdinal(c->Name(), w, true)) return c->Index();
  return -2;
}

int FindMaterial(Engine& e, const std::string& name) {
  ON_wString w(Unquote(name).c_str());
  ONX_ModelComponentIterator it(e.Model(), ON_ModelComponent::Type::RenderMaterial);
  for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent())
    if (!c->IsDeleted() && ON_wString::EqualOrdinal(c->Name(), w, true)) return c->Index();
  return -2;
}

bool CmdSelLayer(Engine& e, TokenStream& ts) {
  auto name = e.ReadString(ts, "nome del livello");
  if (!name) return false;
  const ON_Layer* l = FindLayer(e, *name);
  if (!l) return e.Fail("livello inesistente: " + *name);
  const int idx = l->Index();
  int n = 0;
  for (const auto& id : e.AllObjectIds())
    if (e.Object(id)->Attributes(nullptr)->m_layer_index == idx) { e.Select(id); n++; }
  e.Out() << "  Selezionati " << n << " oggetti\n";
  return true;
}

// -Layer: come la versione a riga di comando di Rhino 8.
//   New <nome|Padre::Figlio> (crea anche i padri mancanti), Current <nome>,
//   Color <nome> r,g,b, PrintColor <nome> r,g,b, PrintWidth <nome> <mm> (0 = predefinito),
//   Linetype <nome> <tipo di linea>, Material <nome> <materiale|None>,
//   On/Off/Lock/Unlock <nome>, Rename <nome> <nuovo>, Delete <nome> (solo se vuoto).
bool CmdLayer(Engine& e, TokenStream& ts) {
  auto action = e.ReadString(ts, "azione (New, Current, Color, PrintColor, PrintWidth, Linetype, Material, On, Off, Lock, Unlock, Rename, Delete)");
  if (!action) return false;
  const std::string a = NormalizeName(*action);
  auto name = e.ReadString(ts, "nome del livello");
  if (!name) return false;
  ONX_Model& m = e.Model();
  if (a == "new") {
    const std::string full = Unquote(*name);
    if (FindLayer(e, full) && (full.find("::") != std::string::npos || FindLayer(e, full)->ParentLayerId() == ON_nil_uuid))
      return e.Fail("il livello esiste gia': " + full);
    // crea i livelli del percorso che mancano
    ON_UUID parent = ON_nil_uuid;
    std::string path;
    size_t start = 0;
    while (true) {
      const size_t sep = full.find("::", start);
      const std::string part = full.substr(start, sep == std::string::npos ? std::string::npos : sep - start);
      if (part.empty()) return e.Fail("nome di livello non valido");
      path = path.empty() ? part : path + "::" + part;
      const ON_Layer* l = FindLayer(e, path.find("::") != std::string::npos ? path : path);
      if (l && path.find("::") == std::string::npos && ON_UuidIsNotNil(l->ParentLayerId())) l = nullptr;
      if (!l) {
        ON_Layer nl;
        nl.SetName(ON_wString(part.c_str()));
        nl.SetColor(ON_Color::Black);
        nl.SetParentLayerId(parent);
        ON_ModelComponentReference ref = m.AddModelComponent(nl, true);
        l = ON_Layer::Cast(ref.ModelComponent());
        if (!l) return e.Fail("nome di livello non valido");
        e.Out() << "  Creato livello " << path << "\n";
      }
      parent = l->Id();
      if (sep == std::string::npos) break;
      start = sep + 2;
    }
    return true;
  }
  const ON_Layer* cl = FindLayer(e, *name);
  if (!cl) return e.Fail("livello inesistente: " + *name);
  ON_Layer* l = EditableLayer(e, cl);
  const std::string label = LayerPath(m, l);
  if (a == "current") {
    m.m_settings.SetCurrentLayerId(l->Id());
    e.Out() << "  Livello corrente: " << label << "\n";
    return true;
  }
  if (a == "color" || a == "printcolor") {
    auto v = e.ReadString(ts, "colore r,g,b");
    if (!v) return false;
    ON_Color c;
    if (!ParseRgb(*v, c)) return e.Fail("colore non valido: " + *v);
    if (a == "color") l->SetColor(c); else l->SetPlotColor(c);
    e.Out() << "  Livello " << label << ": colore " << (a == "color" ? "" : "di stampa ") << *v << "\n";
    return true;
  }
  if (a == "printwidth") {
    auto v = e.ReadNumber(ts, "spessore di stampa in mm (0 = predefinito, -1 = non stampare)");
    if (!v) return false;
    l->SetPlotWeight(*v);
    e.Out() << "  Livello " << label << ": spessore di stampa " << *v << " mm\n";
    return true;
  }
  if (a == "linetype") {
    auto v = e.ReadString(ts, "tipo di linea");
    if (!v) return false;
    const int lt = FindLinetype(e, *v);
    if (lt == -2) return e.Fail("tipo di linea inesistente: " + *v);
    l->SetLinetypeIndex(lt);
    e.Out() << "  Livello " << label << ": tipo di linea " << Unquote(*v) << "\n";
    return true;
  }
  if (a == "material") {
    auto v = e.ReadString(ts, "materiale (None per nessuno)");
    if (!v) return false;
    int mi = -1;
    if (NormalizeName(*v) != "none") {
      mi = FindMaterial(e, *v);
      if (mi == -2) return e.Fail("materiale inesistente: " + *v);
    }
    l->SetRenderMaterialIndex(mi);
    e.Out() << "  Livello " << label << ": materiale " << Unquote(*v) << "\n";
    return true;
  }
  if (a == "on" || a == "off") {
    if (a == "off" && l->Id() == m.m_settings.CurrentLayerId()) return e.Fail("non si puo' spegnere il livello corrente");
    l->SetVisible(a == "on");
    l->SetPersistentVisibility(a == "on");
    return true;
  }
  if (a == "lock" || a == "unlock") {
    if (a == "lock" && l->Id() == m.m_settings.CurrentLayerId()) return e.Fail("non si puo' bloccare il livello corrente");
    l->SetLocked(a == "lock");
    l->SetPersistentLocking(a == "lock");
    return true;
  }
  if (a == "rename") {
    auto v = e.ReadString(ts, "nuovo nome");
    if (!v) return false;
    const std::string nn = Unquote(*v);
    if (nn.empty() || nn.find("::") != std::string::npos) return e.Fail("nome non valido");
    l->SetName(ON_wString(nn.c_str()));
    e.Out() << "  Livello " << label << " rinominato in " << nn << "\n";
    return true;
  }
  if (a == "delete") {
    if (l->Id() == m.m_settings.CurrentLayerId()) return e.Fail("non si puo' eliminare il livello corrente");
    for (const auto& id : e.AllObjectIds(true))
      if (e.Object(id)->Attributes(nullptr)->m_layer_index == l->Index()) return e.Fail("il livello contiene oggetti");
    ONX_ModelComponentIterator it(m, ON_ModelComponent::Type::Layer);
    for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent())
      if (const auto* ch = ON_Layer::Cast(c); ch && !ch->IsDeleted() && ch->ParentLayerId() == l->Id())
        return e.Fail("il livello ha sottolivelli");
    m.RemoveModelComponent(ON_ModelComponent::Type::Layer, l->Id());
    e.Out() << "  Eliminato livello " << label << "\n";
    return true;
  }
  return e.Fail("azione di Layer non supportata: " + *action);
}

// Linetypes New <nome> <motivo> | Delete <nome> | List
//   motivo: lunghezze in mm separate da virgole, positive = tratto, negative = spazio
//   (0 = punto), es. "5,-2" tratteggio, "0,-1.5" punti, "8,-2,0,-2" tratto e punto.
bool CmdLinetypes(Engine& e, TokenStream& ts) {
  auto action = e.ReadString(ts, "azione (New, Edit, Rename, Delete, List)");
  if (!action) return false;
  const std::string a = NormalizeName(*action);
  ONX_Model& m = e.Model();
  if (a == "list") {
    e.Out() << "  Continuous\n";
    ONX_ModelComponentIterator it(m, ON_ModelComponent::Type::LinePattern);
    for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent())
      if (const auto* lt = ON_Linetype::Cast(c); lt && !lt->IsDeleted()) {
        e.Out() << "  " << ToUtf8(lt->Name()) << ":";
        for (int i = 0; i < lt->SegmentCount(); i++) {
          const ON_LinetypeSegment sg = lt->Segment(i);
          e.Out() << " " << (sg.m_seg_type == ON_LinetypeSegment::eSegType::stSpace ? -sg.m_length : sg.m_length);
        }
        e.Out() << "\n";
      }
    return true;
  }
  auto name = e.ReadString(ts, "nome del tipo di linea");
  if (!name) return false;
  const std::string n = Unquote(*name);
  // motivo "5,-2,0,-2" -> segmenti
  auto parsePattern = [&](const std::string& pat, ON_SimpleArray<ON_LinetypeSegment>& segs) {
    std::stringstream ss(Unquote(pat));
    std::string tok;
    while (std::getline(ss, tok, ',')) {
      char* end = nullptr;
      const double v = std::strtod(tok.c_str(), &end);
      if (end == tok.c_str()) return false;
      segs.Append(ON_LinetypeSegment(std::fabs(v), v < 0 || (v == 0 && tok.find('-') != std::string::npos)
                                                       ? ON_LinetypeSegment::eSegType::stSpace
                                                       : ON_LinetypeSegment::eSegType::stLine));
    }
    return segs.Count() >= 2;
  };
  if (a == "edit" || a == "rename") {
    // Edit <nome> <motivo> | Rename <nome> <nuovo nome>
    const int idx = FindLinetype(e, n);
    if (idx < 0) return e.Fail("tipo di linea inesistente o predefinito: " + n);
    auto v = e.ReadString(ts, a == "edit" ? "nuovo motivo (es. 5,-2)" : "nuovo nome");
    if (!v) return false;
    const ON_ModelComponent* c = m.ComponentFromIndex(ON_ModelComponent::Type::LinePattern, idx).ModelComponent();
    auto* lt = const_cast<ON_Linetype*>(ON_Linetype::Cast(c));
    if (!lt) return e.Fail("tipo di linea non trovato");
    if (a == "edit") {
      ON_SimpleArray<ON_LinetypeSegment> segs;
      if (!parsePattern(*v, segs)) return e.Fail("motivo non valido: " + *v);
      if (lt->IsLocked()) return e.Fail("tipo di linea bloccato");
      lt->ClearPattern();
      for (int i = 0; i < segs.Count(); i++) lt->AppendSegment(segs[i]);
      e.Out() << "  Motivo di " << n << " cambiato\n";
    } else {
      const std::string nn = Unquote(*v);
      if (nn.empty() || FindLinetype(e, nn) != -2) return e.Fail("nome non valido o gia' usato: " + nn);
      lt->SetName(ON_wString(nn.c_str()));
      e.Out() << "  Tipo di linea " << n << " rinominato in " << nn << "\n";
    }
    return true;
  }
  if (a == "new") {
    if (n.empty()) return e.Fail("nome non valido");
    if (FindLinetype(e, n) != -2) return e.Fail("il tipo di linea esiste gia': " + n);
    auto pat = e.ReadString(ts, "motivo (es. 5,-2: tratto 5 mm, spazio 2 mm)");
    if (!pat) return false;
    ON_Linetype lt;
    lt.SetName(ON_wString(n.c_str()));
    ON_SimpleArray<ON_LinetypeSegment> segs;
    if (!parsePattern(*pat, segs)) return e.Fail("motivo non valido (servono almeno un tratto e uno spazio): " + *pat);
    for (int i = 0; i < segs.Count(); i++) lt.AppendSegment(segs[i]);
    ON_ModelComponentReference ref = m.AddModelComponent(lt, true);
    if (!ref.ModelComponent()) return e.Fail("tipo di linea non creato");
    e.Out() << "  Creato tipo di linea " << n << "\n";
    return true;
  }
  if (a == "delete") {
    const int idx = FindLinetype(e, n);
    if (idx < 0) return e.Fail("tipo di linea inesistente o predefinito: " + n);
    for (const auto& id : e.AllObjectIds(true))
      if (e.Object(id)->Attributes(nullptr)->m_linetype_index == idx) return e.Fail("tipo di linea usato da oggetti");
    ONX_ModelComponentIterator it(m, ON_ModelComponent::Type::Layer);
    for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent())
      if (const auto* l = ON_Layer::Cast(c); l && !l->IsDeleted() && l->LinetypeIndex() == idx)
        return e.Fail("tipo di linea usato dal livello " + ToUtf8(l->Name()));
    const ON_ModelComponent* c = m.ComponentFromIndex(ON_ModelComponent::Type::LinePattern, idx).ModelComponent();
    if (c) m.RemoveModelComponent(ON_ModelComponent::Type::LinePattern, c->Id());
    e.Out() << "  Eliminato tipo di linea " << n << "\n";
    return true;
  }
  return e.Fail("azione di Linetypes non supportata: " + *action);
}

// SetObjectLinetype ByLayer | ByParent | <nome>
bool CmdSetObjectLinetype(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("SetObjectLinetype");
  if (sel.empty()) return false;
  auto v = e.ReadString(ts, "tipo di linea: ByLayer, ByParent oppure il nome");
  if (!v) return false;
  const std::string s = NormalizeName(*v);
  ON::object_linetype_source src = ON::linetype_from_object;
  int idx = -1;
  if (s == "bylayer") src = ON::linetype_from_layer;
  else if (s == "byparent") src = ON::linetype_from_parent;
  else if ((idx = FindLinetype(e, *v)) == -2) return e.Fail("tipo di linea inesistente: " + *v);
  for (const auto& id : sel)
    if (auto* at = e.EditableAttributes(id)) {
      at->SetLinetypeSource(src);
      at->m_linetype_index = src == ON::linetype_from_object ? idx : -1;
    }
  e.Out() << "  Tipo di linea impostato su " << sel.size() << " oggetti\n";
  return true;
}

// SetObjectPrintWidth ByLayer | ByParent | <mm>   e   SetObjectPrintColor ByLayer | ByParent | r,g,b
bool CmdSetObjectPrintWidth(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("SetObjectPrintWidth");
  if (sel.empty()) return false;
  auto v = e.ReadString(ts, "spessore di stampa: ByLayer, ByParent oppure mm");
  if (!v) return false;
  const std::string s = NormalizeName(*v);
  ON::plot_weight_source src = ON::plot_weight_from_object;
  double w = 0;
  if (s == "bylayer") src = ON::plot_weight_from_layer;
  else if (s == "byparent") src = ON::plot_weight_from_parent;
  else {
    char* end = nullptr;
    w = std::strtod(Unquote(*v).c_str(), &end);
    if (end == Unquote(*v).c_str()) return e.Fail("spessore non valido: " + *v);
  }
  for (const auto& id : sel)
    if (auto* at = e.EditableAttributes(id)) {
      at->SetPlotWeightSource(src);
      at->m_plot_weight_mm = w;
    }
  e.Out() << "  Spessore di stampa impostato su " << sel.size() << " oggetti\n";
  return true;
}

bool CmdSetObjectPrintColor(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("SetObjectPrintColor");
  if (sel.empty()) return false;
  auto v = e.ReadString(ts, "colore di stampa: ByLayer, ByParent, ByDisplay oppure r,g,b");
  if (!v) return false;
  const std::string s = NormalizeName(*v);
  ON::plot_color_source src = ON::plot_color_from_object;
  ON_Color c = ON_Color::Black;
  if (s == "bylayer") src = ON::plot_color_from_layer;
  else if (s == "byparent") src = ON::plot_color_from_parent;
  else if (s == "bydisplay") src = ON::plot_color_from_display;
  else if (!ParseRgb(*v, c)) return e.Fail("colore non valido: " + *v);
  for (const auto& id : sel)
    if (auto* at = e.EditableAttributes(id)) {
      at->SetPlotColorSource(src);
      at->m_plot_color = c;
    }
  e.Out() << "  Colore di stampa impostato su " << sel.size() << " oggetti\n";
  return true;
}

// ChangeLayer <nome>: sposta gli oggetti selezionati sul livello.
bool CmdChangeLayer(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("ChangeLayer");
  if (sel.empty()) return false;
  auto name = e.ReadString(ts, "nome del livello");
  if (!name) return false;
  const ON_Layer* l = FindLayer(e, *name);
  if (!l) return e.Fail("livello inesistente: " + *name);
  for (const auto& id : sel)
    if (auto* a = e.EditableAttributes(id)) a->m_layer_index = l->Index();
  e.Out() << "  " << sel.size() << " oggetti spostati sul livello " << *name << "\n";
  return true;
}

bool CmdNew(Engine& e, TokenStream&) {
  e.Doc().NewEmpty();
  e.ClearSelection();
  e.currentPath.clear();
  e.activeLayoutId = ON_nil_uuid;
  if (e.layoutsChanged) e.layoutsChanged();
  return true;
}

bool CmdOpen(Engine& e, TokenStream& ts) {
  auto path = e.ReadString(ts, "nome del file");
  if (!path) return false;
  std::string log;
  if (IsExchangeFormat(FileExtension(*path))) {
    // STEP, IGES, STL, OBJ, DXF: nuovo documento senza nome con gli oggetti importati (come Rhino)
    e.Doc().NewEmpty();
    e.ClearSelection();
    e.currentPath.clear();
    e.activeLayoutId = ON_nil_uuid;
    if (!ImportExchange(e, *path, {})) return false;
    e.ClearSelection();
    return true;
  }
  if (!e.Doc().Load(*path, &log)) return e.Fail("impossibile aprire " + *path);
  e.ClearSelection();
  e.activeLayoutId = ON_nil_uuid;
  if (e.layoutsChanged) e.layoutsChanged();
  e.currentPath = *path;
  e.Out() << "  Aperto " << *path << "\n";
  return true;
}

bool SaveTo(Engine& e, const std::string& path) {
  if (e.beforeSave) e.beforeSave();
  std::string log;
  if (!e.Doc().Save(path, kRhino8ArchiveVersion, &log)) return e.Fail("impossibile salvare " + path);
  e.currentPath = path;
  e.Out() << "  Salvato " << path << " (formato Rhino 8)\n";
  return true;
}

bool CmdSave(Engine& e, TokenStream& ts) {
  if (e.currentPath.empty() || (!ts.AtEnd() && !IsEnter(ts.Peek()))) {
    auto path = e.ReadString(ts, "nome del file");
    if (!path) return false;
    return SaveTo(e, *path);
  }
  return SaveTo(e, e.currentPath);
}

bool CmdSaveAs(Engine& e, TokenStream& ts) {
  auto path = e.ReadString(ts, "nome del file");
  if (!path) return false;
  return SaveTo(e, *path);
}

bool CmdUndo(Engine& e, TokenStream&) { return e.Undo(); }
bool CmdRedo(Engine& e, TokenStream&) { return e.Redo(); }

// SelName: aggiunge alla selezione gli oggetti con i nomi dati (Invio per finire),
// nell'ordine dei nomi: conta per BooleanDifference (prima l'oggetto di base).
bool CmdSelName(Engine& e, TokenStream& ts) {
  int n = 0, names = 0;
  while (!ts.AtEnd() && !IsEnter(ts.Peek())) {
    const std::string name = ts.Next();
    names++;
    for (const auto& id : e.AllObjectIds(false)) {
      const auto* a = e.Object(id)->Attributes(nullptr);
      if (a && ON_wString::EqualOrdinal(a->m_name, ON_wString(name.c_str()), true) && !e.IsSelected(id)) { e.Select(id); n++; }
    }
  }
  e.SkipEnter(ts);
  if (!names) return e.Fail("scrivi i nomi degli oggetti da selezionare");
  e.Out() << "  Selezionati " << n << " oggetti per nome\n";
  return true;
}

// Nome dell'oggetto (campo Nome del pannello Proprieta'). "" toglie il nome.
bool CmdSetObjectName(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("SetObjectName");
  if (sel.empty()) return false;
  auto name = e.ReadString(ts, "nome dell'oggetto (\"\" per nessuno)");
  if (!name) return false;
  std::string n = *name == "\"\"" ? std::string() : *name;
  for (const auto& id : sel)
    if (auto* a = e.EditableAttributes(id)) a->SetName(ON_wString(n.c_str()), true);
  e.Out() << "  Nome assegnato a " << sel.size() << " oggetti\n";
  return true;
}

// Colore di visualizzazione (pannello Proprieta'): ByLayer, ByParent o r,g,b.
bool CmdSetObjectColor(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("SetObjectColor");
  if (sel.empty()) return false;
  auto v = e.ReadString(ts, "colore: ByLayer, ByParent oppure r,g,b");
  if (!v) return false;
  std::string s = *v;
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  while (!s.empty() && (s[0] == '_' || s[0] == '-')) s.erase(0, 1);
  ON::object_color_source src = ON::color_from_object;
  ON_Color col = ON_Color::Black;
  if (s == "bylayer") src = ON::color_from_layer;
  else if (s == "byparent") src = ON::color_from_parent;
  else {
    int r, g, b;
    if (std::sscanf(s.c_str(), "%d,%d,%d", &r, &g, &b) != 3 || r < 0 || g < 0 || b < 0 || r > 255 || g > 255 || b > 255)
      return e.Fail("colore non valido: " + *v);
    col = ON_Color(r, g, b);
  }
  for (const auto& id : sel)
    if (auto* a = e.EditableAttributes(id)) {
      a->SetColorSource(src);
      if (src == ON::color_from_object) a->m_color = col;
    }
  e.Out() << "  Colore impostato su " << sel.size() << " oggetti\n";
  return true;
}

bool CmdWhat(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("What");
  if (sel.empty()) return false;
  for (const auto& id : sel) {
    const auto* o = e.Object(id);
    char buf[64];
    ON_UuidToString(id, buf);
    const auto* a = o->Attributes(nullptr);
    const ON_ModelComponent* layer = e.Model().ComponentFromIndex(ON_ModelComponent::Type::Layer, a->m_layer_index).ModelComponent();
    e.Out() << "  " << DescribeObject(o->Geometry(nullptr)) << "  livello: "
            << (layer ? ToUtf8(layer->Name()) : std::string("?")) << "  id: " << buf << "\n";
  }
  return true;
}

}  // namespace

std::string DescribeObject(const ON_Geometry* g) {
  std::ostringstream s;
  if (!g) return "oggetto vuoto";
  if (const auto* p = ON_Point::Cast(g)) s << "punto (" << p->point.x << ", " << p->point.y << ", " << p->point.z << ")";
  else if (const auto* l = ON_LineCurve::Cast(g)) s << "linea, lunghezza " << l->m_line.Length();
  else if (const auto* a = ON_ArcCurve::Cast(g)) s << (a->IsCircle() ? "cerchio" : "arco") << ", raggio " << a->Radius();
  else if (const auto* pl = ON_PolylineCurve::Cast(g)) s << (pl->IsClosed() ? "polilinea chiusa" : "polilinea") << ", " << pl->PointCount() << " punti";
  else if (const auto* pc = ON_PolyCurve::Cast(g)) s << (pc->IsClosed() ? "policurva chiusa" : "policurva") << ", " << pc->Count() << " segmenti";
  else if (const auto* n = ON_NurbsCurve::Cast(g)) s << "curva NURBS, grado " << n->Degree() << ", " << n->CVCount() << " punti di controllo" << (n->IsClosed() ? ", chiusa" : "");
  else if (const auto* x = ON_Extrusion::Cast(g)) s << (x->IsCapped() ? "estrusione chiusa" : "estrusione");
  else if (const auto* b = ON_Brep::Cast(g)) {
    if (b->m_F.Count() == 1) s << (b->IsSolid() ? "superficie chiusa" : "superficie");
    else s << (b->IsSolid() ? "polisuperficie chiusa" : "polisuperficie aperta") << ", " << b->m_F.Count() << " superfici";
  }
  else if (const auto* m = ON_Mesh::Cast(g)) s << "mesh, " << m->VertexCount() << " vertici, " << m->FaceCount() << " facce";
  else if (const auto* sd = ON_SubD::Cast(g)) s << "SubD, " << sd->FaceCount() << " facce";
  else s << g->ClassId()->ClassName();
  return s.str();
}

void RegisterDocumentCommands(Engine& e) {
  auto reg = [&](const char* n, const char* cat, const char* d, CommandFn f, bool mod) {
    CommandInfo i{n, cat, d, f};
    i.modifiesModel = mod;
    e.Register(i);
  };
  reg("New", "File", "Nuovo modello (mm, tolleranza 0.001)", CmdNew, true);
  reg("Open", "File", "Apre un file .3dm", CmdOpen, true);
  reg("Save", "File", "Salva in formato Rhino 8", CmdSave, false);
  reg("SaveAs", "File", "Salva con nome in formato Rhino 8", CmdSaveAs, false);
  reg("Undo", "Modifica", "Annulla l'ultimo comando", CmdUndo, false);
  reg("Redo", "Modifica", "Ripete il comando annullato", CmdRedo, false);
  reg("SelAll", "Seleziona", "Seleziona tutto", CmdSelAll, false);
  reg("SelNone", "Seleziona", "Deseleziona tutto", CmdSelNone, false);
  reg("SelLast", "Seleziona", "Seleziona gli ultimi oggetti creati", CmdSelLast, false);
  reg("SelCrv", "Seleziona", "Seleziona le curve", CmdSelCrv, false);
  reg("SelPt", "Seleziona", "Seleziona i punti", CmdSelPt, false);
  reg("SelSrf", "Seleziona", "Seleziona le superfici", CmdSelSrf, false);
  reg("SelPolysrf", "Seleziona", "Seleziona le polisuperfici", CmdSelPolysrf, false);
  reg("SelMesh", "Seleziona", "Seleziona le mesh", CmdSelMesh, false);
  reg("SelSubD", "Seleziona", "Seleziona i SubD", CmdSelSubD, false);
  reg("SelLayer", "Seleziona", "Seleziona gli oggetti di un livello", CmdSelLayer, false);
  reg("Invert", "Seleziona", "Inverte la selezione", CmdInvert, false);
  reg("Layer", "Livelli", "-Layer New/Current/Color/PrintColor/PrintWidth/Linetype/Material/On/Off/Lock/Unlock/Rename/Delete <nome>", CmdLayer, true);
  reg("Linetypes", "Livelli", "Tipi di linea: New <nome> <motivo>, Delete <nome>, List", CmdLinetypes, true);
  reg("SetObjectLinetype", "Proprietà", "Tipo di linea degli oggetti (ByLayer, ByParent, nome)", CmdSetObjectLinetype, true);
  reg("SetObjectPrintWidth", "Proprietà", "Spessore di stampa degli oggetti (ByLayer, ByParent, mm)", CmdSetObjectPrintWidth, true);
  reg("SetObjectPrintColor", "Proprietà", "Colore di stampa degli oggetti (ByLayer, ByParent, ByDisplay, r,g,b)", CmdSetObjectPrintColor, true);
  reg("ChangeLayer", "Livelli", "Sposta la selezione su un livello", CmdChangeLayer, true);
  reg("SelName", "Seleziona", "Seleziona gli oggetti per nome", CmdSelName, false);
  reg("SetObjectName", "Proprietà", "Nome degli oggetti selezionati", CmdSetObjectName, true);
  reg("SetObjectColor", "Proprietà", "Colore degli oggetti (ByLayer, ByParent, r,g,b)", CmdSetObjectColor, true);
  reg("What", "Analisi", "Descrive gli oggetti selezionati", CmdWhat, false);
}

}  // namespace cad
