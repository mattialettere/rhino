// Comandi di file, annulla, selezione, livelli e informazioni.
#include <cctype>
#include <cstdio>
#include "commands/Engine.h"
#include <functional>
#include <sstream>

namespace cad {
namespace {

using Pred = std::function<bool(const ON_Geometry*)>;

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

const ON_Layer* FindLayer(Engine& e, const std::string& name) {
  ON_wString w(name.c_str());
  ONX_ModelComponentIterator it(e.Model(), ON_ModelComponent::Type::Layer);
  for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent())
    if (ON_wString::EqualOrdinal(c->Name(), w, true)) return ON_Layer::Cast(c);
  return nullptr;
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

// -Layer New <nome> | Current <nome>
bool CmdLayer(Engine& e, TokenStream& ts) {
  auto action = e.ReadString(ts, "azione (New, Current)");
  if (!action) return false;
  auto name = e.ReadString(ts, "nome del livello");
  if (!name) return false;
  const std::string a = NormalizeName(*action);
  if (a == "new") {
    if (FindLayer(e, *name)) return e.Fail("il livello esiste gia': " + *name);
    ON_wString w(name->c_str());
    if (e.Model().AddLayer(w, ON_Color::Black) < 0) return e.Fail("nome di livello non valido");
    e.Out() << "  Creato livello " << *name << "\n";
    return true;
  }
  if (a == "current") {
    const ON_Layer* l = FindLayer(e, *name);
    if (!l) return e.Fail("livello inesistente: " + *name);
    e.Model().m_settings.SetCurrentLayerId(l->Id());
    e.Out() << "  Livello corrente: " << *name << "\n";
    return true;
  }
  return e.Fail("azione di Layer non supportata: " + *action);
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
  return true;
}

bool CmdOpen(Engine& e, TokenStream& ts) {
  auto path = e.ReadString(ts, "nome del file");
  if (!path) return false;
  std::string log;
  if (!e.Doc().Load(*path, &log)) return e.Fail("impossibile aprire " + *path);
  e.ClearSelection();
  e.currentPath = *path;
  e.Out() << "  Aperto " << *path << "\n";
  return true;
}

bool SaveTo(Engine& e, const std::string& path) {
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
  reg("Layer", "Livelli", "-Layer New/Current <nome>", CmdLayer, true);
  reg("ChangeLayer", "Livelli", "Sposta la selezione su un livello", CmdChangeLayer, true);
  reg("SetObjectName", "Proprietà", "Nome degli oggetti selezionati", CmdSetObjectName, true);
  reg("SetObjectColor", "Proprietà", "Colore degli oggetti (ByLayer, ByParent, r,g,b)", CmdSetObjectColor, true);
  reg("What", "Analisi", "Descrive gli oggetti selezionati", CmdWhat, false);
}

}  // namespace cad
