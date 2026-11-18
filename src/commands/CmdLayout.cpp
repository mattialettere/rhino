// Tavole (layout), dettagli e stampa: Layout, CopyLayout, Layouts, Detail, Print.
// Le tavole sono viste di tipo pagina nelle impostazioni del .3dm, i dettagli sono
// oggetti ON_DetailView nello spazio della tavola: gli stessi dati di Rhino 8.
#include "commands/Engine.h"
#include "print/Page.h"
#include "ui/Scene.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace cad {

namespace {

std::string Unq(std::string s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
  return s;
}

ON::LengthUnitSystem ModelUnits(Engine& e) {
  return e.Model().m_settings.m_ModelUnitsAndTolerances.m_unit_system.UnitSystem();
}

ON_BoundingBox ModelBox(Engine& e) {
  ON_BoundingBox b;
  for (const auto& id : e.ObjectIdsInSpace(ON_nil_uuid)) {
    const auto* o = e.Object(id);
    const ON_Geometry* g = o ? o->Geometry(nullptr) : nullptr;
    if (!g || ON_ClippingPlaneSurface::Cast(g)) continue;
    ON_BoundingBox t;
    if (g->GetTightBoundingBox(t)) b.Union(t);
  }
  return b;
}

void Changed(Engine& e) {
  if (e.layoutsChanged) e.layoutsChanged();
}

// Dettaglio con la vista "projection" nel riquadro (x0,y0)-(x1,y1) della tavola attiva
ON_UUID AddDetail(Engine& e, const std::string& projection, double x0, double y0, double x1, double y1) {
  auto* d = new ON_DetailView();
  const double w = std::fabs(x1 - x0), h = std::fabs(y1 - y0);
  const ON_BoundingBox box = ModelBox(e);
  d->m_view = StandardView(projection, box, w / h);
  d->m_view.m_view_type = ON::view_type::nested_view_type;
  ON_UUID vid;
  ON_CreateUuid(vid);
  d->m_view.m_vp.SetViewportId(vid);
  FitView(d->m_view, w, h, 0, ModelUnits(e), box);
  d->m_page_per_model_ratio = 0;
  ON_Polyline pl;
  const double ax = std::min(x0, x1), ay = std::min(y0, y1);
  pl.Append(ON_3dPoint(ax, ay, 0));
  pl.Append(ON_3dPoint(ax + w, ay, 0));
  pl.Append(ON_3dPoint(ax + w, ay + h, 0));
  pl.Append(ON_3dPoint(ax, ay + h, 0));
  pl.Append(ON_3dPoint(ax, ay, 0));
  ON_PolylineCurve pc(pl);
  pc.GetNurbForm(d->m_boundary);
  ON_3dmObjectAttributes a;
  const ON_UUID layerId = e.Model().m_settings.CurrentLayerId();
  const ON_ModelComponent* layer = e.Model().ComponentFromId(ON_ModelComponent::Type::Layer, layerId).ModelComponent();
  a.m_layer_index = layer ? layer->Index() : 0;
  a.m_space = ON::page_space;
  a.m_viewport_id = e.activeLayoutId;
  a.m_name = d->m_view.m_name;
  return e.AddObject(d, &a);
}

bool ReadDetails(const std::map<std::string, std::string>& opts, int& n) {
  n = 1;
  auto it = opts.find("details");
  if (it == opts.end()) it = opts.find("initialdetailcount");
  if (it != opts.end()) n = std::atoi(it->second.c_str());
  return n == 0 || n == 1 || n == 4;
}

// Layout ["nome"] Size=A3 Orientation=Landscape|Portrait Width= Height= Details=0|1|4
bool CmdLayout(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  std::string name;
  if (!ts.AtEnd() && !IsEnter(ts.Peek()) && ts.Peek().find('=') == std::string::npos) {
    name = Unq(ts.Next());
    opts.merge(e.ReadOptions(ts));
  }
  ONX_Model& m = e.Model();
  if (name.empty()) {
    for (int i = 1;; i++) {
      name = "Pagina " + std::to_string(i);
      if (FindLayout(m, name) < 0) break;
    }
  }
  if (FindLayout(m, name) >= 0) return e.Fail("esiste gia' una tavola con questo nome: " + name);
  double w = 297, h = 210;
  if (auto it = opts.find("size"); it != opts.end() && !PaperSize(it->second, w, h))
    return e.Fail("formato sconosciuto: " + it->second + " (A0-A5, Letter, Legal, Tabloid)");
  if (auto it = opts.find("width"); it != opts.end()) w = std::atof(it->second.c_str());
  if (auto it = opts.find("height"); it != opts.end()) h = std::atof(it->second.c_str());
  if (auto it = opts.find("orientation"); it != opts.end()) {
    const std::string o = NormalizeName(it->second);
    if (o == "portrait" || o == "verticale") { if (w > h) std::swap(w, h); }
    else if (w < h) std::swap(w, h);
  }
  if (!(w > 1 && h > 1 && w < 10000 && h < 10000)) return e.Fail("dimensioni del foglio non valide");
  int details = 1;
  if (!ReadDetails(opts, details)) return e.Fail("numero di dettagli iniziali: 0, 1 o 4");
  ON_3dmView v;
  v.m_view_type = ON::view_type::page_view_type;
  v.m_name = ON_wString(name.c_str());
  v.m_page_settings.m_width_mm = w;
  v.m_page_settings.m_height_mm = h;
  int maxPage = -1;
  for (int i : LayoutIndices(m)) maxPage = std::max(maxPage, m.m_settings.m_views[i].m_page_settings.m_page_number);
  v.m_page_settings.m_page_number = maxPage + 1;
  // vista della tavola: dall'alto sul foglio
  ON_Viewport& vp = v.m_vp;
  vp.ChangeToParallelProjection(true);
  vp.SetCameraLocation(ON_3dPoint(w / 2, h / 2, 100));
  vp.SetCameraDirection(-ON_3dVector::ZAxis);
  vp.SetCameraUp(ON_3dVector::YAxis);
  vp.SetFrustum(-w / 2, w / 2, -h / 2, h / 2, 1, 1000);
  ON_UUID id;
  ON_CreateUuid(id);
  vp.SetViewportId(id);
  v.m_display_mode_id = ON_StandardDisplayModeId::Wireframe;
  m.m_settings.m_views.Append(v);
  e.activeLayoutId = id;
  // dettagli iniziali dentro i margini (10 mm)
  const double mg = 10, gap = 5;
  if (details == 1) AddDetail(e, "Top", mg, mg, w - mg, h - mg);
  else if (details == 4) {
    const double cw = (w - 2 * mg - gap) / 2, ch = (h - 2 * mg - gap) / 2;
    AddDetail(e, "Top", mg, mg + ch + gap, mg + cw, h - mg);
    AddDetail(e, "Perspective", mg + cw + gap, mg + ch + gap, w - mg, h - mg);
    AddDetail(e, "Front", mg, mg, mg + cw, mg + ch);
    AddDetail(e, "Right", mg + cw + gap, mg, w - mg, mg + ch);
  }
  e.ClearSelection();
  e.Out() << "  Tavola \"" << name << "\" " << w << " x " << h << " mm con " << details << " dettagli\n";
  Changed(e);
  return true;
}

int ActiveLayoutIndex(Engine& e) { return ON_UuidIsNil(e.activeLayoutId) ? -1 : FindLayout(e.Model(), e.activeLayoutId); }

// Copia degli oggetti di una tavola in un'altra
void CopyPageObjects(Engine& e, const ON_UUID& from, const ON_UUID& to) {
  for (const auto& id : e.ObjectIdsInSpace(from, true)) {
    const auto* o = e.Object(id);
    if (!o || !o->Geometry(nullptr)) continue;
    ON_3dmObjectAttributes a = *o->Attributes(nullptr);
    a.m_viewport_id = to;
    ON_Object* g = o->Geometry(nullptr)->Duplicate();
    if (auto* d = ON_DetailView::Cast(g)) {
      ON_UUID vid;
      ON_CreateUuid(vid);
      d->m_view.m_vp.SetViewportId(vid);
    }
    e.AddObject(g, &a);
  }
}

// CopyLayout ["nuovo nome"]: copia la tavola attiva con i suoi oggetti
bool CmdCopyLayout(Engine& e, TokenStream& ts) {
  const int li = ActiveLayoutIndex(e);
  if (li < 0) return e.Fail("nessuna tavola attiva da copiare");
  ONX_Model& m = e.Model();
  std::string name;
  if (!ts.AtEnd() && !IsEnter(ts.Peek())) name = Unq(ts.Next());
  const std::string base = ToUtf8(m.m_settings.m_views[li].m_name);
  if (name.empty())
    for (int i = 1;; i++) {
      name = base + " (" + std::to_string(i) + ")";
      if (FindLayout(m, name) < 0) break;
    }
  if (FindLayout(m, name) >= 0) return e.Fail("esiste gia' una tavola con questo nome: " + name);
  ON_3dmView v = m.m_settings.m_views[li];
  v.m_name = ON_wString(name.c_str());
  ON_UUID id;
  ON_CreateUuid(id);
  v.m_vp.SetViewportId(id);
  int maxPage = -1;
  for (int i : LayoutIndices(m)) maxPage = std::max(maxPage, m.m_settings.m_views[i].m_page_settings.m_page_number);
  v.m_page_settings.m_page_number = maxPage + 1;
  const ON_UUID from = e.activeLayoutId;
  m.m_settings.m_views.Append(v);
  CopyPageObjects(e, from, id);
  e.activeLayoutId = id;
  e.ClearSelection();
  e.Out() << "  Tavola \"" << name << "\" creata\n";
  Changed(e);
  return true;
}

// Layouts List | Activate <nome|Model> | Rename <nome> <nuovo> | Delete <nome>
bool CmdLayouts(Engine& e, TokenStream& ts) {
  auto a = e.ReadString(ts, "Layouts: List, Activate, Rename o Delete");
  if (!a) return false;
  const std::string act = NormalizeName(*a);
  ONX_Model& m = e.Model();
  if (act == "list") {
    const auto idx = LayoutIndices(m);
    e.Out() << "  Tavole: " << idx.size() << "\n";
    for (int i : idx) {
      const auto& v = m.m_settings.m_views[i];
      e.Out() << "    " << ToUtf8(v.m_name) << " (" << v.m_page_settings.m_width_mm << " x " << v.m_page_settings.m_height_mm
              << " mm)" << (v.m_vp.ViewportId() == e.activeLayoutId ? " attiva" : "") << "\n";
    }
    return true;
  }
  auto n = e.ReadString(ts, "nome della tavola (Model = spazio del modello)");
  if (!n) return false;
  const std::string name = Unq(*n);
  if (act == "activate" && (NormalizeName(name) == "model" || NormalizeName(name) == "modello")) {
    e.activeLayoutId = ON_nil_uuid;
    e.ClearSelection();
    Changed(e);
    return true;
  }
  const int li = FindLayout(m, name);
  if (li < 0) return e.Fail("tavola inesistente: " + name);
  const ON_UUID lid = m.m_settings.m_views[li].m_vp.ViewportId();
  if (act == "activate") {
    e.activeLayoutId = lid;
    e.ClearSelection();
    Changed(e);
    return true;
  }
  if (act == "rename") {
    auto nn = e.ReadString(ts, "nuovo nome");
    if (!nn) return false;
    if (FindLayout(m, Unq(*nn)) >= 0) return e.Fail("esiste gia' una tavola con questo nome");
    m.m_settings.m_views[li].m_name = ON_wString(Unq(*nn).c_str());
    Changed(e);
    return true;
  }
  if (act == "delete") {
    for (const auto& id : e.ObjectIdsInSpace(lid, true)) e.DeleteObject(id);
    m.m_settings.m_views.Remove(li);
    if (e.activeLayoutId == lid) e.activeLayoutId = ON_nil_uuid;
    e.ClearSelection();
    e.Out() << "  Tavola \"" << name << "\" eliminata\n";
    Changed(e);
    return true;
  }
  return e.Fail("opzione di Layouts sconosciuta: " + *a);
}

// Scala "1:50", "2:1" o numero (pagina/modello)
bool ParseScale(const std::string& s, double& r) {
  const size_t c = s.find(':');
  if (c != std::string::npos) {
    const double a = std::atof(s.substr(0, c).c_str()), b = std::atof(s.substr(c + 1).c_str());
    if (!(a > 0 && b > 0)) return false;
    r = a / b;
    return true;
  }
  r = std::atof(s.c_str());
  return r >= 0;
}

std::vector<ON_DetailView*> SelectedDetails(Engine& e, std::vector<ON_UUID>* ids = nullptr) {
  std::vector<ON_DetailView*> out;
  for (const auto& id : e.Selection())
    if (auto* d = ON_DetailView::Cast(e.EditableGeometry(id))) {
      out.push_back(d);
      if (ids) ids->push_back(id);
    }
  return out;
}

// Detail Add <angolo> <angolo> [Projection=Top] | Scale <1:50> | Projection <nome> | ZoomExtents
bool CmdDetail(Engine& e, TokenStream& ts) {
  auto a = e.ReadString(ts, "Detail: Add, Scale, Projection o ZoomExtents");
  if (!a) return false;
  const std::string act = NormalizeName(*a);
  if (ON_UuidIsNil(e.activeLayoutId) || ActiveLayoutIndex(e) < 0)
    return e.Fail("i dettagli si creano e si modificano in una tavola (comando Layout)");
  if (act == "add") {
    auto p0 = e.ReadPoint(ts, "primo angolo del dettaglio");
    if (!p0) return false;
    auto p1 = e.ReadPoint(ts, "angolo opposto");
    if (!p1) return false;
    auto opts = e.ReadOptions(ts);
    if (std::fabs(p1->x - p0->x) < 1 || std::fabs(p1->y - p0->y) < 1) return e.Fail("dettaglio troppo piccolo");
    std::string proj = "Top";
    if (auto it = opts.find("projection"); it != opts.end()) proj = it->second;
    const ON_UUID id = AddDetail(e, proj, p0->x, p0->y, p1->x, p1->y);
    if (ON_UuidIsNil(id)) return e.Fail("dettaglio non creato");
    e.ClearSelection();
    e.Select(id);
    e.Out() << "  Dettaglio " << proj << " creato\n";
    return true;
  }
  std::vector<ON_UUID> ids;
  auto det = SelectedDetails(e, &ids);
  if (det.empty()) {
    if (e.interactive && e.RequireSelection("Detail", "seleziona i dettagli, poi Invio").empty()) return false;
    det = SelectedDetails(e, &ids);
  }
  if (det.empty()) return e.Fail("seleziona un dettaglio");
  const ON::LengthUnitSystem units = ModelUnits(e);
  if (act == "scale") {
    auto s = e.ReadString(ts, "scala (es. 1:50, pagina:modello)");
    if (!s) return false;
    double r;
    if (!ParseScale(*s, r)) return e.Fail("scala non valida: " + *s);
    for (auto* d : det) {
      if (!d->m_view.m_vp.IsParallelProjection()) return e.Fail("la scala si imposta solo nei dettagli paralleli");
      d->m_page_per_model_ratio = r;
      const ON_BoundingBox b = d->m_boundary.BoundingBox();
      FitView(d->m_view, b.m_max.x - b.m_min.x, b.m_max.y - b.m_min.y, r, units, ON_BoundingBox::EmptyBoundingBox);
    }
    e.Out() << "  Scala " << *s << " su " << det.size() << " dettagli\n";
    return true;
  }
  if (act == "projection" || act == "zoomextents") {
    std::string proj;
    if (act == "projection") {
      auto p = e.ReadString(ts, "Top, Bottom, Front, Back, Right, Left o Perspective");
      if (!p) return false;
      proj = *p;
    }
    const ON_BoundingBox box = ModelBox(e);
    for (auto* d : det) {
      const ON_BoundingBox b = d->m_boundary.BoundingBox();
      const double w = b.m_max.x - b.m_min.x, h = b.m_max.y - b.m_min.y;
      if (!proj.empty()) {
        const ON_UUID vid = d->m_view.m_vp.ViewportId();
        d->m_view = StandardView(proj, box, w / h);
        d->m_view.m_view_type = ON::view_type::nested_view_type;
        d->m_view.m_vp.SetViewportId(vid);
        d->m_page_per_model_ratio = 0;
      }
      FitView(d->m_view, w, h, 0, units, box);
      if (d->m_page_per_model_ratio > 0) FitView(d->m_view, w, h, d->m_page_per_model_ratio, units, ON_BoundingBox::EmptyBoundingBox);
    }
    for (const auto& id : ids)
      if (auto* at = e.EditableAttributes(id); at && !proj.empty())
        if (auto* d = ON_DetailView::Cast(e.EditableGeometry(id))) at->m_name = d->m_view.m_name;
    return true;
  }
  if (act == "displaymode") {
    auto p = e.ReadString(ts, "Wireframe o Shaded");
    if (!p) return false;
    const std::string k = NormalizeName(*p);
    for (auto* d : det)
      d->m_view.m_display_mode_id = (k == "shaded" || k == "ombreggiato") ? ON_StandardDisplayModeId::Shaded
                                                                           : ON_StandardDisplayModeId::Wireframe;
    return true;
  }
  return e.Fail("opzione di Detail sconosciuta: " + *a);
}

// Pagine da stampare: "View" = nome di tavola, "All" (tutte le tavole), nome di vista
// (Top, Perspective...) o vuoto (tavola attiva, altrimenti la vista attiva).
bool CollectPages(Engine& e, const std::map<std::string, std::string>& opts, std::vector<PageDrawing>& pages) {
  PrintOptions po;
  if (auto it = opts.find("dpi"); it != opts.end()) po.dpi = std::clamp(std::atof(it->second.c_str()), 36.0, 600.0);
  if (auto it = opts.find("color"); it != opts.end()) po.blackWhite = NormalizeName(it->second) == "blackwhite";
  if (opts.count("blackwhite")) po.blackWhite = true;
  double w = 297, h = 210;
  if (auto it = opts.find("size"); it != opts.end() && !PaperSize(it->second, w, h)) return e.Fail("formato sconosciuto: " + it->second);
  if (auto it = opts.find("orientation"); it != opts.end() && NormalizeName(it->second) == "portrait" && w > h) std::swap(w, h);
  ONX_Model& m = e.Model();
  std::string view;
  if (auto it = opts.find("view"); it != opts.end()) view = Unq(it->second);
  if (NormalizeName(view) == "all") {
    for (int li : LayoutIndices(m)) {
      PageDrawing pg;
      if (ComposeLayout(e, li, po, pg)) pages.push_back(std::move(pg));
    }
    if (pages.empty()) return e.Fail("il documento non ha tavole");
    return true;
  }
  int li = view.empty() ? ActiveLayoutIndex(e) : FindLayout(m, view);
  if (li >= 0) {
    PageDrawing pg;
    ComposeLayout(e, li, po, pg);
    pages.push_back(std::move(pg));
    return true;
  }
  ON_3dmView v;
  bool have = false;
  if (view.empty() && e.getActiveView) have = e.getActiveView(v);
  if (!have) {
    // vista salvata nel file con quel nome, altrimenti vista standard
    for (int i = 0; i < m.m_settings.m_views.Count() && !have; i++)
      if (m.m_settings.m_views[i].m_view_type != ON::view_type::page_view_type &&
          ON_wString::EqualOrdinal(m.m_settings.m_views[i].m_name, ON_wString(view.c_str()), true)) {
        v = m.m_settings.m_views[i];
        have = true;
      }
    for (int i = 0; i < m.m_settings.m_named_views.Count() && !have; i++)
      if (ON_wString::EqualOrdinal(m.m_settings.m_named_views[i].m_name, ON_wString(view.c_str()), true)) {
        v = m.m_settings.m_named_views[i];
        have = true;
      }
  }
  if (!have) {
    const std::string p = NormalizeName(view.empty() ? std::string("perspective") : view);
    static const char* std7[] = {"top", "bottom", "front", "back", "right", "left", "perspective"};
    if (std::find_if(std::begin(std7), std::end(std7), [&](const char* s) { return p == s; }) == std::end(std7))
      return e.Fail("vista o tavola inesistente: " + view);
    v = StandardView(p, ModelBox(e), (w - 20) / (h - 20));
    have = true;
  }
  if (opts.count("wireframe")) v.m_display_mode_id = ON_StandardDisplayModeId::Wireframe;
  if (opts.count("shaded")) v.m_display_mode_id = ON_StandardDisplayModeId::Shaded;
  // estensione: inquadra tutto il modello nel foglio
  if (opts.count("extents")) FitView(v, w - 20, h - 20, 0, ModelUnits(e), ModelBox(e));
  PageDrawing pg;
  ComposeView(e, v, w, h, po, pg);
  pages.push_back(std::move(pg));
  return true;
}

// Print [Destination=PDF|Printer] [File="x.pdf"] [View=...] [Size=A4] [Orientation=Portrait]
//       [Color=Display|BlackWhite] [Dpi=150] [Extents] [Wireframe|Shaded]
bool CmdPrint(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts, {"extents", "blackwhite", "wireframe", "shaded"});
  std::string dest = "printer";
  if (auto it = opts.find("destination"); it != opts.end()) dest = NormalizeName(it->second);
  std::string file;
  if (auto it = opts.find("file"); it != opts.end()) file = Unq(it->second);
  if (!file.empty() && dest == "printer") dest = "pdf";
  std::vector<PageDrawing> pages;
  if (!CollectPages(e, opts, pages)) return false;
  if (dest == "pdf") {
    if (file.empty()) return e.Fail("indica il file PDF: File=\"percorso.pdf\"");
    std::string err;
    if (!WritePdf(file, pages, err)) return e.Fail(err);
    e.Out() << "  " << pages.size() << " pagine scritte in " << file << "\n";
    return true;
  }
  if (!e.printPages) return e.Fail("la stampante si usa dalla finestra di Kurva3D (per un file: Destination=PDF File=...)");
  if (!e.printPages(pages)) return e.Fail("stampa annullata");
  e.Out() << "  " << pages.size() << " pagine inviate alla stampante\n";
  return true;
}

}  // namespace

bool ExportPdf(Engine& e, const std::string& path, const std::map<std::string, std::string>& opts) {
  std::vector<PageDrawing> pages;
  if (!CollectPages(e, opts, pages)) return false;
  std::string err;
  if (!WritePdf(path, pages, err)) return e.Fail(err);
  e.Out() << "  PDF scritto: " << path << "\n";
  return true;
}

void RegisterLayoutCommands(Engine& e) {
  auto reg = [&](const char* n, const char* d, CommandFn f, bool modifies) {
    CommandInfo i{n, "Tavole", d, f};
    i.modifiesModel = modifies;
    e.Register(i);
  };
  reg("Layout", "Nuova tavola (Size=A4, Orientation, Details=0/1/4)", CmdLayout, true);
  reg("CopyLayout", "Copia la tavola attiva con i suoi oggetti", CmdCopyLayout, true);
  reg("Layouts", "Tavole: List, Activate (anche Model), Rename, Delete", CmdLayouts, true);
  reg("Detail", "Dettagli della tavola: Add, Scale, Projection, ZoomExtents, DisplayMode", CmdDetail, true);
  reg("Print", "Stampa o PDF della tavola o della vista (Destination=PDF File=...)", CmdPrint, false);
}

}  // namespace cad
