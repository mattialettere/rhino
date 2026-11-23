// Viste e piani: CPlane, NamedCPlane, NamedView, ClippingPlane, EnableClippingPlane,
// DisableClippingPlane e Snapshots. Viste con nome e CPlane con nome stanno nelle
// impostazioni del .3dm (come Rhino 8); le istantanee in una stringa del documento.
#include "commands/Engine.h"
#include "core/Document3dm.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace cad {

namespace {

std::string Unq(std::string s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
  return s;
}

std::string Action(const std::string& tok) {
  std::string t = NormalizeName(tok);
  return t;
}

bool SameName(const ON_wString& a, const std::string& b) { return ON_wString::EqualOrdinal(a, ON_wString(b.c_str()), true); }

// --- CPlane ---------------------------------------------------------------------------------
bool ActiveCPlane(Engine& e, ON_Plane& p) {
  ON_3dmView v;
  if (e.getActiveView && e.getActiveView(v)) { p = v.m_cplane.m_plane; return p.IsValid(); }
  p = ON_Plane::World_xy;
  return true;
}

bool SetCPlane(Engine& e, const ON_Plane* p) {
  if (!e.setActiveCPlane) return e.Fail("i piani di costruzione si cambiano solo dalla finestra di Kurva3D");
  if (!e.setActiveCPlane(p)) return e.Fail("piano di costruzione non valido");
  return true;
}

bool CmdCPlane(Engine& e, TokenStream& ts) {
  auto a = e.ReadString(ts, "CPlane: World, Origin, 3Point, Elevation o Reset");
  if (!a) return false;
  const std::string act = Action(*a);
  ON_Plane cur;
  ActiveCPlane(e, cur);
  if (act == "world") {
    auto w = e.ReadString(ts, "Top, Bottom, Front, Back, Right o Left");
    if (!w) return false;
    const std::string k = Action(*w);
    ON_Plane p;
    if (k == "top") p = ON_Plane::World_xy;
    else if (k == "bottom") p = ON_Plane(ON_3dPoint::Origin, -ON_3dVector::XAxis, ON_3dVector::YAxis);
    else if (k == "front") p = ON_Plane(ON_3dPoint::Origin, ON_3dVector::XAxis, ON_3dVector::ZAxis);
    else if (k == "back") p = ON_Plane(ON_3dPoint::Origin, -ON_3dVector::XAxis, ON_3dVector::ZAxis);
    else if (k == "right") p = ON_Plane(ON_3dPoint::Origin, ON_3dVector::YAxis, ON_3dVector::ZAxis);
    else if (k == "left") p = ON_Plane(ON_3dPoint::Origin, -ON_3dVector::YAxis, ON_3dVector::ZAxis);
    else return e.Fail("vista sconosciuta: " + *w);
    return SetCPlane(e, &p);
  }
  if (act == "reset") return SetCPlane(e, nullptr);
  if (act == "origin") {
    auto o = e.ReadPoint(ts, "nuova origine del piano di costruzione");
    if (!o) return false;
    ON_Plane p = cur;
    p.origin = *o;
    p.UpdateEquation();
    return SetCPlane(e, &p);
  }
  if (act == "elevation") {
    auto z = e.ReadNumber(ts, "altezza lungo la normale del piano");
    if (!z) return false;
    ON_Plane p = cur;
    p.origin = p.origin + *z * p.zaxis;
    p.UpdateEquation();
    return SetCPlane(e, &p);
  }
  if (act == "3point") {
    auto o = e.ReadPoint(ts, "origine");
    if (!o) return false;
    auto x = e.ReadPoint(ts, "direzione dell'asse X");
    if (!x) return false;
    auto y = e.ReadPoint(ts, "orientamento del piano (direzione Y)");
    if (!y) return false;
    ON_Plane p(*o, *x, *y);
    if (!p.IsValid()) return e.Fail("i tre punti sono allineati");
    return SetCPlane(e, &p);
  }
  return e.Fail("opzione di CPlane sconosciuta: " + *a);
}

// --- Viste e CPlane con nome -------------------------------------------------------------------
template <class T>
int FindByName(const ON_ClassArray<T>& arr, const std::string& name, ON_wString T::*field) {
  for (int i = 0; i < arr.Count(); i++)
    if (SameName(arr[i].*field, name)) return i;
  return -1;
}

bool CmdNamedView(Engine& e, TokenStream& ts) {
  auto a = e.ReadString(ts, "NamedView: Save, Restore, Delete, Rename o List");
  if (!a) return false;
  const std::string act = Action(*a);
  auto& views = e.Model().m_settings.m_named_views;
  if (act == "list") {
    e.Out() << "  Viste con nome: " << views.Count() << "\n";
    for (int i = 0; i < views.Count(); i++) e.Out() << "    " << ToUtf8(views[i].m_name) << "\n";
    return true;
  }
  auto n = e.ReadString(ts, "nome della vista");
  if (!n) return false;
  const std::string name = Unq(*n);
  const int i = FindByName(views, name, &ON_3dmView::m_name);
  if (act == "save") {
    if (!e.getActiveView) return e.Fail("le viste si salvano solo dalla finestra di Kurva3D");
    ON_3dmView v;
    if (!e.getActiveView(v)) return e.Fail("vista attiva non disponibile");
    v.m_name = ON_wString(name.c_str());
    // una vista con nome ha un id suo (non quello della finestra)
    ON_UUID id;
    ON_CreateUuid(id);
    v.m_vp.SetViewportId(id);
    if (i >= 0) views[i] = v; else views.Append(v);
    e.Out() << "  Vista \"" << name << "\" salvata\n";
    return true;
  }
  if (i < 0) return e.Fail("vista con nome inesistente: " + name);
  if (act == "restore") {
    if (!e.setActiveView || !e.setActiveView(views[i])) return e.Fail("vista non ripristinata");
    e.Out() << "  Vista \"" << name << "\" ripristinata\n";
    return true;
  }
  if (act == "delete") { views.Remove(i); e.Out() << "  Vista \"" << name << "\" eliminata\n"; return true; }
  if (act == "rename") {
    auto nn = e.ReadString(ts, "nuovo nome");
    if (!nn) return false;
    views[i].m_name = ON_wString(Unq(*nn).c_str());
    return true;
  }
  return e.Fail("opzione di NamedView sconosciuta: " + *a);
}

bool CmdNamedCPlane(Engine& e, TokenStream& ts) {
  auto a = e.ReadString(ts, "NamedCPlane: Save, Restore, Delete, Rename o List");
  if (!a) return false;
  const std::string act = Action(*a);
  auto& cps = e.Model().m_settings.m_named_cplanes;
  if (act == "list") {
    e.Out() << "  Piani di costruzione con nome: " << cps.Count() << "\n";
    for (int i = 0; i < cps.Count(); i++) e.Out() << "    " << ToUtf8(cps[i].m_name) << "\n";
    return true;
  }
  auto n = e.ReadString(ts, "nome del piano di costruzione");
  if (!n) return false;
  const std::string name = Unq(*n);
  const int i = FindByName(cps, name, &ON_3dmConstructionPlane::m_name);
  if (act == "save") {
    ON_Plane p;
    ActiveCPlane(e, p);
    ON_3dmConstructionPlane cp(p);
    cp.m_name = ON_wString(name.c_str());
    if (i >= 0) cps[i] = cp; else cps.Append(cp);
    e.Out() << "  Piano \"" << name << "\" salvato\n";
    return true;
  }
  if (i < 0) return e.Fail("piano di costruzione con nome inesistente: " + name);
  if (act == "restore") return SetCPlane(e, &cps[i].m_plane);
  if (act == "delete") { cps.Remove(i); return true; }
  if (act == "rename") {
    auto nn = e.ReadString(ts, "nuovo nome");
    if (!nn) return false;
    cps[i].m_name = ON_wString(Unq(*nn).c_str());
    return true;
  }
  return e.Fail("opzione di NamedCPlane sconosciuta: " + *a);
}

// --- Piani di sezione ------------------------------------------------------------------------
bool CmdClippingPlane(Engine& e, TokenStream& ts) {
  ON_Plane cp;
  ActiveCPlane(e, cp);
  auto p0 = e.ReadPoint(ts, "primo angolo del piano di sezione");
  if (!p0) return false;
  auto p1 = e.ReadPoint(ts, "angolo opposto");
  if (!p1) return false;
  double u0, v0, u1, v1;
  cp.ClosestPointTo(*p0, &u0, &v0);
  cp.ClosestPointTo(*p1, &u1, &v1);
  if (std::fabs(u1 - u0) < 1e-9 || std::fabs(v1 - v0) < 1e-9) return e.Fail("rettangolo nullo");
  // piano centrato nel rettangolo, normale come il CPlane: nasconde cio' che sta dalla parte della
  // normale (in Top: quello che sta sopra, come una pianta)
  ON_Plane pl = cp;
  pl.origin = cp.PointAt((u0 + u1) / 2, (v0 + v1) / 2);
  pl.UpdateEquation();
  const double hu = std::fabs(u1 - u0) / 2, hv = std::fabs(v1 - v0) / 2;
  auto* s = new ON_ClippingPlaneSurface(pl);
  s->SetExtents(0, ON_Interval(-hu, hu));
  s->SetExtents(1, ON_Interval(-hv, hv));
  s->SetDomain(0, -hu, hu);
  s->SetDomain(1, -hv, hv);
  s->m_clipping_plane.m_plane = pl;
  ON_CreateUuid(s->m_clipping_plane.m_plane_id);
  s->m_clipping_plane.m_bEnabled = true;
  if (!ON_UuidIsNil(e.activeViewportId)) s->m_clipping_plane.m_viewport_ids.AddUuid(e.activeViewportId);
  const ON_UUID id = e.AddObject(s);
  if (ON_UuidIsNil(id)) return e.Fail("piano di sezione non creato");
  e.ClearSelection();
  e.Select(id);
  e.Out() << "  Piano di sezione creato (attivo nella vista corrente)\n";
  return true;
}

ON_ClippingPlaneSurface* EditableClip(Engine& e, const ON_UUID& id) {
  return ON_ClippingPlaneSurface::Cast(e.EditableGeometry(id));
}

bool ClipViews(Engine& e, TokenStream& ts, bool enable) {
  auto opts = e.ReadOptions(ts, {"all"});
  auto sel = e.RequireSelection(enable ? "EnableClippingPlane" : "DisableClippingPlane", "seleziona i piani di sezione, poi Invio");
  if (sel.empty()) return false;
  opts.merge(e.ReadOptions(ts, {"all"}));
  std::vector<ON_UUID> views;
  if (opts.count("all")) views = e.viewportIds;
  else if (!ON_UuidIsNil(e.activeViewportId)) views.push_back(e.activeViewportId);
  int n = 0;
  for (const auto& id : sel)
    if (auto* c = EditableClip(e, id)) {
      for (const auto& v : views) {
        if (enable) c->m_clipping_plane.m_viewport_ids.AddUuid(v, true);
        else c->m_clipping_plane.m_viewport_ids.RemoveUuid(v);
      }
      c->m_clipping_plane.m_bEnabled = enable || c->m_clipping_plane.m_viewport_ids.Count() > 0;
      n++;
    }
  if (!n) return e.Fail("nessun piano di sezione selezionato");
  e.Out() << "  " << n << " piani di sezione " << (enable ? "attivati" : "disattivati") << "\n";
  return true;
}

// --- Istantanee ------------------------------------------------------------------------------
// Formato testo: una riga per voce ("view ...", "layer <indice> <visibile> <bloccato> r g b",
// "obj <uuid> <modo> <sorgente colore> r g b <materiale> <sorgente materiale>").
const wchar_t* kSnapPrefix = L"Kurva3D.Snapshot:";

std::string ViewToText(const ON_3dmView& v) {
  std::ostringstream o;
  o.precision(17);
  const ON_Viewport& vp = v.m_vp;
  double l, r, b, t, n, f;
  vp.GetFrustum(&l, &r, &b, &t, &n, &f);
  const ON_3dPoint loc = vp.CameraLocation(), tg = v.TargetPoint();
  const ON_3dVector d = vp.CameraDirection(), u = vp.CameraUp();
  const ON_Plane& c = v.m_cplane.m_plane;
  o << "view " << (vp.IsPerspectiveProjection() ? 1 : 0) << " " << loc.x << " " << loc.y << " " << loc.z << " " << d.x << " "
    << d.y << " " << d.z << " " << u.x << " " << u.y << " " << u.z << " " << l << " " << r << " " << b << " " << t << " "
    << n << " " << f << " " << (tg.IsValid() ? tg.x : 0) << " " << (tg.IsValid() ? tg.y : 0) << " "
    << (tg.IsValid() ? tg.z : 0) << " " << c.origin.x << " " << c.origin.y << " " << c.origin.z << " " << c.xaxis.x << " "
    << c.xaxis.y << " " << c.xaxis.z << " " << c.yaxis.x << " " << c.yaxis.y << " " << c.yaxis.z;
  return o.str();
}

bool TextToView(std::istringstream& in, ON_3dmView& v) {
  int persp;
  double x[27];
  if (!(in >> persp)) return false;
  for (double& d : x) if (!(in >> d)) return false;
  ON_Viewport& vp = v.m_vp;
  if (persp) vp.ChangeToPerspectiveProjection(ON_UNSET_VALUE, true, ON_PI / 6);
  else vp.ChangeToParallelProjection(true);
  vp.SetCameraLocation(ON_3dPoint(x[0], x[1], x[2]));
  vp.SetCameraDirection(ON_3dVector(x[3], x[4], x[5]));
  vp.SetCameraUp(ON_3dVector(x[6], x[7], x[8]));
  vp.SetFrustum(x[9], x[10], x[11], x[12], x[13], x[14]);
  v.SetTargetPoint(ON_3dPoint(x[15], x[16], x[17]));
  v.m_cplane.m_plane = ON_Plane(ON_3dPoint(x[18], x[19], x[20]), ON_3dVector(x[21], x[22], x[23]), ON_3dVector(x[24], x[25], x[26]));
  return true;
}

std::string SnapshotText(Engine& e) {
  std::ostringstream o;
  o.precision(17);
  ON_3dmView v;
  if (e.getActiveView && e.getActiveView(v)) o << ViewToText(v) << "\n";
  ONX_ModelComponentIterator it(e.Model(), ON_ModelComponent::Type::Layer);
  for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent())
    if (const auto* l = ON_Layer::Cast(c))
      o << "layer " << l->Index() << " " << l->IsVisible() << " " << l->IsLocked() << " " << l->Color().Red() << " "
        << l->Color().Green() << " " << l->Color().Blue() << "\n";
  for (const auto& id : e.AllObjectIds(true)) {
    const auto* a = e.Object(id)->Attributes(nullptr);
    if (!a) continue;
    char s[64];
    ON_UuidToString(id, s);
    o << "obj " << s << " " << (int)a->Mode() << " " << (int)a->ColorSource() << " " << a->m_color.Red() << " "
      << a->m_color.Green() << " " << a->m_color.Blue() << " " << a->m_material_index << " " << (int)a->MaterialSource() << "\n";
  }
  return o.str();
}

ON_Layer* EditLayer(Engine& e, int index) {
  return const_cast<ON_Layer*>(ON_Layer::Cast(e.Model().ComponentFromIndex(ON_ModelComponent::Type::Layer, index).ModelComponent()));
}

bool RestoreSnapshot(Engine& e, const std::string& text) {
  std::istringstream lines(text);
  for (std::string line; std::getline(lines, line);) {
    std::istringstream in(line);
    std::string kind;
    in >> kind;
    if (kind == "view") {
      ON_3dmView v;
      if (TextToView(in, v) && e.setActiveView) e.setActiveView(v);
    } else if (kind == "layer") {
      int i, vis, lock, r, g, b;
      if (!(in >> i >> vis >> lock >> r >> g >> b)) continue;
      if (ON_Layer* l = EditLayer(e, i)) {
        l->SetVisible(vis != 0);
        l->SetLocked(lock != 0);
        l->SetColor(ON_Color(r, g, b));
      }
    } else if (kind == "obj") {
      std::string s;
      int mode, csrc, r, g, b, mat, msrc;
      if (!(in >> s >> mode >> csrc >> r >> g >> b >> mat >> msrc)) continue;
      const ON_UUID id = ON_UuidFromString(s.c_str());
      if (!e.Object(id)) continue;  // oggetto eliminato dopo l'istantanea
      if (auto* a = e.EditableAttributes(id)) {
        a->SetMode(ON::ObjectMode(mode));
        a->SetColorSource(ON::ObjectColorSource(csrc));
        a->m_color = ON_Color(r, g, b);
        a->m_material_index = mat;
        a->SetMaterialSource(ON::ObjectMaterialSource(msrc));
      }
    }
  }
  return true;
}

bool CmdSnapshots(Engine& e, TokenStream& ts) {
  auto a = e.ReadString(ts, "Snapshots: Save, Restore, Delete o List");
  if (!a) return false;
  const std::string act = Action(*a);
  ONX_Model& m = e.Model();
  if (act == "list") {
    ON_ClassArray<ON_UserString> us;
    m.GetDocumentUserStrings(us);
    int n = 0;
    for (int i = 0; i < us.Count(); i++)
      if (us[i].m_key.Left((int)wcslen(kSnapPrefix)) == kSnapPrefix) {
        e.Out() << "    " << ToUtf8(us[i].m_key.Mid((int)wcslen(kSnapPrefix))) << "\n";
        n++;
      }
    e.Out() << "  Istantanee: " << n << "\n";
    return true;
  }
  auto n = e.ReadString(ts, "nome dell'istantanea");
  if (!n) return false;
  const std::string name = Unq(*n);
  const ON_wString key = ON_wString(kSnapPrefix) + ON_wString(name.c_str());
  ON_wString value;
  const bool exists = m.GetDocumentUserString(key, value) && !value.IsEmpty();
  if (act == "save") {
    m.SetDocumentUserString(key, ON_wString(SnapshotText(e).c_str()));
    e.Out() << "  Istantanea \"" << name << "\" salvata\n";
    return true;
  }
  if (!exists) return e.Fail("istantanea inesistente: " + name);
  if (act == "restore") {
    RestoreSnapshot(e, ToUtf8(value));
    e.Out() << "  Istantanea \"" << name << "\" ripristinata\n";
    return true;
  }
  if (act == "delete") { m.SetDocumentUserString(key, nullptr); return true; }
  return e.Fail("opzione di Snapshots sconosciuta: " + *a);
}

}  // namespace

void RegisterViewCommands(Engine& e) {
  auto reg = [&](const char* n, const char* cat, const char* d, CommandFn f, bool modifies) {
    CommandInfo i{n, cat, d, f};
    i.modifiesModel = modifies;
    e.Register(i);
  };
  reg("CPlane", "Viste", "Piano di costruzione della vista attiva (World, Origin, 3Point, Elevation, Reset)", CmdCPlane, false);
  reg("NamedCPlane", "Viste", "Piani di costruzione con nome (Save, Restore, Delete, Rename, List)", CmdNamedCPlane, true);
  reg("NamedView", "Viste", "Viste con nome (Save, Restore, Delete, Rename, List)", CmdNamedView, true);
  reg("ClippingPlane", "Viste", "Piano di sezione che taglia la vista attiva", CmdClippingPlane, true);
  reg("EnableClippingPlane", "Viste", "Attiva i piani di sezione nella vista attiva (All per tutte)",
      [](Engine& e, TokenStream& ts) { return ClipViews(e, ts, true); }, true);
  reg("DisableClippingPlane", "Viste", "Disattiva i piani di sezione nella vista attiva (All per tutte)",
      [](Engine& e, TokenStream& ts) { return ClipViews(e, ts, false); }, true);
  reg("Snapshots", "Viste", "Istantanee di vista, livelli e oggetti (Save, Restore, Delete, List)", CmdSnapshots, true);
}

std::vector<std::string> NamedItems(Engine& e, int kind) {
  std::vector<std::string> out;
  ONX_Model& m = e.Model();
  if (kind == 0)
    for (int i = 0; i < m.m_settings.m_named_views.Count(); i++) out.push_back(ToUtf8(m.m_settings.m_named_views[i].m_name));
  else if (kind == 1)
    for (int i = 0; i < m.m_settings.m_named_cplanes.Count(); i++) out.push_back(ToUtf8(m.m_settings.m_named_cplanes[i].m_name));
  else {
    const wchar_t* prefix = kind == 2 ? kSnapPrefix : kind == 3 ? L"Kurva3D.NamedSelection." : L"Kurva3D.LayerState.";
    ON_ClassArray<ON_UserString> us;
    m.GetDocumentUserStrings(us);
    const int pl = (int)wcslen(prefix);
    for (int i = 0; i < us.Count(); i++)
      if (us[i].m_key.Left(pl) == prefix && !us[i].m_string_value.IsEmpty()) out.push_back(ToUtf8(us[i].m_key.Mid(pl)));
  }
  return out;
}

}  // namespace cad
