// Comandi di rendering: Render, luci (PointLight, DirectionalLight, SpotLight,
// RectangularLight), Sun, Skylight, GroundPlane e SetMaterial (in piu' di Rhino).
#include "commands/Engine.h"
#include "print/Page.h"
#include "render/Render.h"
#include "ui/Scene.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include "zlib/zlib.h"  // zlib di openNURBS (nomi con prefisso z_)

namespace cad {

bool WritePng(const std::string& path, int w, int h, const std::vector<unsigned char>& rgb, std::string& err) {
  if (w <= 0 || h <= 0 || (int)rgb.size() != w * h * 3) { err = "immagine vuota"; return false; }
  std::string raw;
  raw.reserve((size_t)(w * 3 + 1) * h);
  for (int y = 0; y < h; y++) {
    raw += '\0';
    raw.append(reinterpret_cast<const char*>(&rgb[(size_t)y * w * 3]), (size_t)w * 3);
  }
  uLongf n = compressBound((uLong)raw.size());
  std::string z(n, '\0');
  if (compress2(reinterpret_cast<Bytef*>(&z[0]), &n, reinterpret_cast<const Bytef*>(raw.data()), (uLong)raw.size(), 6) != Z_OK) {
    err = "compressione non riuscita";
    return false;
  }
  z.resize(n);
  std::string out = "\x89PNG\r\n\x1a\n";
  auto be32 = [](std::string& s, unsigned v) {
    s += (char)(v >> 24); s += (char)(v >> 16); s += (char)(v >> 8); s += (char)v;
  };
  auto chunk = [&](const char* type, const std::string& data) {
    be32(out, (unsigned)data.size());
    std::string td = std::string(type, 4) + data;
    out += td;
    be32(out, (unsigned)crc32(0, reinterpret_cast<const Bytef*>(td.data()), (uInt)td.size()));
  };
  std::string ihdr;
  be32(ihdr, (unsigned)w);
  be32(ihdr, (unsigned)h);
  ihdr += '\x08'; ihdr += '\x02'; ihdr += '\0'; ihdr += '\0'; ihdr += '\0';
  chunk("IHDR", ihdr);
  chunk("IDAT", z);
  chunk("IEND", "");
  std::ofstream f(std::filesystem::u8path(path), std::ios::binary);
  if (!f) { err = "impossibile scrivere " + path; return false; }
  f.write(out.data(), (std::streamsize)out.size());
  return (bool)f;
}

namespace {

std::string Unq(std::string s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
  return s;
}

bool YesNo(const std::string& v) {
  const std::string n = NormalizeName(v);
  return n == "yes" || n == "on" || n == "si" || n == "true" || n == "1";
}

ON_UUID AddLight(Engine& e, ON_Light* l) {
  l->Enable(true);
  l->SetDiffuse(ON_Color(255, 255, 255));
  l->SetIntensity(1.0);
  const ON_UUID id = e.AddObject(l);
  if (!ON_UuidIsNil(id)) { e.ClearSelection(); e.Select(id); }
  return id;
}

bool CmdPointLight(Engine& e, TokenStream& ts) {
  auto p = e.ReadPoint(ts, "posizione della luce");
  if (!p) return false;
  auto* l = new ON_Light();
  l->SetStyle(ON::world_point_light);
  l->SetLocation(*p);
  AddLight(e, l);
  e.Out() << "  Luce puntiforme creata\n";
  return true;
}

bool CmdDirectionalLight(Engine& e, TokenStream& ts) {
  auto a = e.ReadPoint(ts, "fine della luce (bersaglio)");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "inizio della luce (direzione da cui arriva)");
  if (!b) return false;
  ON_3dVector d = *a - *b;
  if (!d.Unitize()) return e.Fail("direzione nulla");
  auto* l = new ON_Light();
  l->SetStyle(ON::world_directional_light);
  l->SetLocation(*b);
  l->SetDirection(d);
  AddLight(e, l);
  e.Out() << "  Luce direzionale creata\n";
  return true;
}

// SpotLight: centro della base del cono, raggio, vertice (posizione della luce) - come Rhino
bool CmdSpotLight(Engine& e, TokenStream& ts) {
  auto base = e.ReadPoint(ts, "centro della base del cono");
  if (!base) return false;
  auto rad = e.ReadNumber(ts, "raggio del cono");
  if (!rad) return false;
  auto apex = e.ReadPoint(ts, "posizione della luce (vertice del cono)");
  if (!apex) return false;
  ON_3dVector d = *base - *apex;
  const double h = d.Length();
  if (h < 1e-9 || *rad <= 0) return e.Fail("cono non valido");
  d.Unitize();
  auto* l = new ON_Light();
  l->SetStyle(ON::world_spot_light);
  l->SetLocation(*apex);
  l->SetDirection(d * h);
  l->SetSpotAngleRadians(std::atan(*rad / h));
  l->SetHotSpot(0.5);
  AddLight(e, l);
  e.Out() << "  Faretto creato\n";
  return true;
}

bool CmdRectangularLight(Engine& e, TokenStream& ts) {
  auto p0 = e.ReadPoint(ts, "primo angolo");
  if (!p0) return false;
  auto p1 = e.ReadPoint(ts, "lunghezza (secondo angolo)");
  if (!p1) return false;
  auto p2 = e.ReadPoint(ts, "larghezza");
  if (!p2) return false;
  const ON_3dVector len = *p1 - *p0;
  ON_3dVector wid = *p2 - *p1;
  wid = wid - (wid * len) / std::max(1e-30, len * len) * len;  // perpendicolare
  if (len.Length() < 1e-9 || wid.Length() < 1e-9) return e.Fail("rettangolo nullo");
  auto* l = new ON_Light();
  l->SetStyle(ON::world_rectangular_light);
  l->SetLocation(*p0);
  l->SetLength(len);
  l->SetWidth(wid);
  ON_3dVector n = ON_CrossProduct(len, wid);
  n.Unitize();
  l->SetDirection(n);
  AddLight(e, l);
  e.Out() << "  Luce rettangolare creata\n";
  return true;
}

// -Sun On|Off Azimuth=gradi Altitude=gradi Intensity=
bool CmdSun(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts, {"on", "off"});
  if (!ts.AtEnd() && !IsEnter(ts.Peek())) {
    const std::string w = NormalizeName(ts.Peek());
    if (w == "on" || w == "off") { ts.Next(); opts[w] = "1"; }
    opts.merge(e.ReadOptions(ts));
  }
  ON_Sun& s = e.Model().m_settings.m_RenderSettings.Sun();
  s.SetEnableAllowed(true);
  if (opts.count("off")) s.SetEnableOn(false);
  else s.SetEnableOn(true);
  s.SetManualControlAllowed(true);
  s.SetManualControlOn(true);
  if (auto it = opts.find("azimuth"); it != opts.end()) s.SetAzimuth(std::atof(it->second.c_str()));
  if (auto it = opts.find("altitude"); it != opts.end()) s.SetAltitude(std::clamp(std::atof(it->second.c_str()), -90.0, 90.0));
  if (auto it = opts.find("intensity"); it != opts.end()) s.SetIntensity(std::max(0.0, std::atof(it->second.c_str())));
  e.Out() << "  Sole " << (s.EnableOn() ? "acceso" : "spento") << " (azimut " << s.Azimuth() << ", altezza " << s.Altitude() << ")\n";
  return true;
}

bool OnOff(Engine& e, TokenStream& ts, bool& on, std::map<std::string, std::string>& opts) {
  opts = e.ReadOptions(ts, {"on", "off"});
  if (!ts.AtEnd() && !IsEnter(ts.Peek())) {
    const std::string w = NormalizeName(ts.Peek());
    if (w == "on" || w == "off") { ts.Next(); opts[w] = "1"; }
    opts.merge(e.ReadOptions(ts));
  }
  on = !opts.count("off");
  return true;
}

bool CmdSkylight(Engine& e, TokenStream& ts) {
  bool on;
  std::map<std::string, std::string> opts;
  OnOff(e, ts, on, opts);
  e.Model().m_settings.m_RenderSettings.Skylight().SetEnabled(on);
  e.Out() << "  Luce del cielo " << (on ? "accesa" : "spenta") << "\n";
  return true;
}

bool CmdGroundPlane(Engine& e, TokenStream& ts) {
  bool on;
  std::map<std::string, std::string> opts;
  OnOff(e, ts, on, opts);
  ON_GroundPlane& g = e.Model().m_settings.m_RenderSettings.GroundPlane();
  g.SetEnabled(on);
  if (auto it = opts.find("altitude"); it != opts.end()) {
    if (NormalizeName(it->second) == "auto") g.SetAutoAltitude(true);
    else { g.SetAutoAltitude(false); g.SetAltitude(std::atof(it->second.c_str())); }
  }
  if (auto it = opts.find("shadowonly"); it != opts.end()) g.SetShadowOnly(YesNo(it->second));
  e.Out() << "  Piano di terra " << (on ? "acceso" : "spento") << "\n";
  return true;
}

bool ParseColor(const std::string& s, ON_Color& c) {
  int r, g, b;
  if (std::sscanf(s.c_str(), "%d,%d,%d", &r, &g, &b) != 3) return false;
  c = ON_Color(std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255));
  return true;
}

// SetMaterial Color=r,g,b Gloss=0..1 Reflectivity=0..1 Transparency=0..1 IOR=1.5 Emission=r,g,b Name=
bool CmdSetMaterial(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("SetMaterial", "seleziona gli oggetti, poi Invio");
  if (sel.empty()) return false;
  auto opts = e.ReadOptions(ts);
  ON_Material m;
  m.SetDiffuse(ON_Color(204, 204, 204));
  if (auto it = opts.find("color"); it != opts.end()) {
    ON_Color c;
    if (!ParseColor(it->second, c)) return e.Fail("colore: r,g,b");
    m.SetDiffuse(c);
  }
  const double gloss = opts.count("gloss") ? std::clamp(std::atof(opts["gloss"].c_str()), 0.0, 1.0) : 0.0;
  m.SetShine(gloss * ON_Material::MaxShine);
  m.SetSpecular(ON_Color(255, 255, 255));
  if (opts.count("reflectivity")) m.SetReflectivity(std::clamp(std::atof(opts["reflectivity"].c_str()), 0.0, 1.0));
  if (opts.count("transparency")) m.SetTransparency(std::clamp(std::atof(opts["transparency"].c_str()), 0.0, 1.0));
  if (opts.count("ior")) m.m_index_of_refraction = std::clamp(std::atof(opts["ior"].c_str()), 1.0, 4.0);
  if (auto it = opts.find("emission"); it != opts.end()) {
    ON_Color c;
    if (!ParseColor(it->second, c)) return e.Fail("emissione: r,g,b");
    m.SetEmission(c);
  }
  // PBR=Yes: materiale fisico (come i materiali "Physically Based" di Rhino 8)
  if (opts.count("pbr") && (opts["pbr"] == "yes" || opts["pbr"] == "si" || opts["pbr"] == "true")) {
    m.ToPhysicallyBased();
    auto pb = m.PhysicallyBased();
    if (!pb) return e.Fail("materiale PBR non disponibile");
    ON_Color base = m.Diffuse();
    if (auto it = opts.find("basecolor"); it != opts.end() && !ParseColor(it->second, base)) return e.Fail("colore base: r,g,b");
    pb->SetBaseColor(ON_4fColor(base));
    auto num = [&](const char* k, double def, double lo, double hi) {
      return opts.count(k) ? std::clamp(std::atof(opts[k].c_str()), lo, hi) : def;
    };
    pb->SetMetallic(num("metallic", 0.0, 0, 1));
    pb->SetRoughness(num("roughness", 0.5, 0, 1));
    pb->SetSpecular(num("specular", 0.5, 0, 1));
    pb->SetClearcoat(num("clearcoat", 0.0, 0, 1));
    pb->SetClearcoatRoughness(num("clearcoatroughness", 0.0, 0, 1));
    pb->SetSheen(num("sheen", 0.0, 0, 1));
    pb->SetOpacity(num("opacity", 1.0, 0, 1));
    pb->SetOpacityIOR(num("ior", 1.5, 1, 4));
    if (auto it = opts.find("emission"); it != opts.end()) {
      ON_Color c;
      if (!ParseColor(it->second, c)) return e.Fail("emissione: r,g,b");
      pb->SetEmission(ON_4fColor(c));
    }
  }
  std::string name = opts.count("name") ? Unq(opts["name"]) : "";
  ONX_Model& model = e.Model();
  if (name.empty())
    for (int i = 1;; i++) {
      name = "Materiale " + std::to_string(i);
      bool used = false;
      ONX_ModelComponentIterator it(model, ON_ModelComponent::Type::RenderMaterial);
      for (auto* c = it.FirstComponent(); c; c = it.NextComponent())
        if (ToUtf8(c->Name()) == name) used = true;
      if (!used) break;
    }
  m.SetName(ON_wString(name.c_str()));
  ON_ModelComponentReference ref = model.AddModelComponent(m, true);
  const auto* added = ON_Material::Cast(ref.ModelComponent());
  if (!added) return e.Fail("materiale non creato");
  for (const auto& id : sel)
    if (auto* a = e.EditableAttributes(id)) {
      a->m_material_index = added->Index();
      a->SetMaterialSource(ON::material_from_object);
    }
  e.Out() << "  Materiale \"" << ToUtf8(added->Name()) << "\" assegnato a " << sel.size() << " oggetti\n";
  return true;
}

// Render [Width= Height= Samples= File="x.png" View=nome]: con File scrive l'immagine,
// altrimenti apre la finestra di rendering (progressiva).
bool CmdRender(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  RenderOptions ro;
  ON_3dmView v;
  bool have = false;
  if (!opts.count("view") && e.getActiveView) have = e.getActiveView(v);
  int w = 0, h = 0;
  if (have && v.m_vp.ScreenPortWidth() > 0 && v.m_vp.ScreenPortHeight() > 0) {
    w = v.m_vp.ScreenPortWidth();
    h = std::abs(v.m_vp.ScreenPortHeight());
  }
  ro.width = opts.count("width") ? std::atoi(opts["width"].c_str()) : (w > 0 ? w : 800);
  ro.height = opts.count("height") ? std::atoi(opts["height"].c_str()) : (h > 0 ? h : 600);
  ro.samples = opts.count("samples") ? std::atoi(opts["samples"].c_str()) : 64;
  if (ro.width < 8 || ro.height < 8 || ro.width > 8000 || ro.height > 8000) return e.Fail("dimensioni dell'immagine non valide");
  ro.samples = std::clamp(ro.samples, 1, 10000);
  ui::Scene scene(e);
  scene.fixedSpace = true;
  scene.Rebuild();
  // come Rhino 8: si renderizzano superfici, solidi, SubD e mesh; curve, testi e quote no
  bool any = false;
  for (const auto& o : scene.Objects())
    if (o.shaded) { any = true; break; }
  if (!any)
    return e.Fail("niente da renderizzare: il rendering mostra solo superfici, solidi, SubD e mesh "
                  "(curve, testi e quote non compaiono, come in Rhino 8). Per un disegno 2D usa Stampa o ViewCaptureToFile");
  if (!have) {
    const std::string proj = opts.count("view") ? Unq(opts["view"]) : "Perspective";
    v = StandardView(proj, scene.Bounds(), double(ro.width) / ro.height);
    FitView(v, ro.width, ro.height, 0, ON::LengthUnitSystem::Millimeters, scene.Bounds());
  }
  const std::string file = opts.count("file") ? Unq(opts["file"]) : "";
  if (file.empty() && e.renderWindow) return e.renderWindow(v, ro.width, ro.height, ro.samples) || e.Fail("rendering annullato");
  if (file.empty()) return e.Fail("indica il file dell'immagine: File=\"render.png\"");
  RenderScene rs;
  rs.Build(e, scene, ro);
  RenderCamera cam;
  if (!cam.Init(v, double(ro.width) / ro.height)) return e.Fail("vista non valida");
  PathTracer pt(rs, cam, ro);
  for (int i = 0; i < ro.samples; i++) pt.Pass();
  std::vector<unsigned char> rgb;
  pt.Image(rgb);
  std::string err;
  if (!WritePng(file, ro.width, ro.height, rgb, err)) return e.Fail(err);
  e.Out() << "  Rendering " << ro.width << " x " << ro.height << ", " << ro.samples << " campioni, " << rs.TriangleCount()
          << " triangoli: " << file << "\n";
  return true;
}

// SetDisplayMode Mode=Rendered (anche solo il nome): modalita' della vista attiva
bool CmdSetDisplayMode(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  std::string m;
  if (auto it = opts.find("mode"); it != opts.end()) m = it->second;
  else {
    auto s = e.ReadString(ts, "modalita' (Wireframe, Shaded, Ghosted, XRay, Rendered, Raytraced)");
    if (!s) return false;
    m = *s;
  }
  std::string n = NormalizeName(m);
  n.erase(std::remove(n.begin(), n.end(), '-'), n.end());
  if (!e.setDisplayMode) return e.Fail("le modalita' di visualizzazione si cambiano nella finestra di Kurva3D");
  if (!e.setDisplayMode(n)) return e.Fail("modalita' sconosciuta: " + m);
  return true;
}

}  // namespace

void RegisterRenderCommands(Engine& e) {
  auto reg = [&](const char* n, const char* d, CommandFn f, bool modifies) {
    CommandInfo i{n, "Rendering", d, f};
    i.modifiesModel = modifies;
    e.Register(i);
  };
  reg("Render", "Rendering della vista attiva (path tracing; File= per salvare l'immagine)", CmdRender, false);
  reg("PointLight", "Luce puntiforme", CmdPointLight, true);
  reg("DirectionalLight", "Luce direzionale", CmdDirectionalLight, true);
  reg("SpotLight", "Faretto (base del cono, raggio, posizione)", CmdSpotLight, true);
  reg("RectangularLight", "Luce rettangolare", CmdRectangularLight, true);
  reg("Sun", "Sole: On/Off, Azimuth, Altitude, Intensity", CmdSun, true);
  reg("Skylight", "Luce del cielo On/Off", CmdSkylight, true);
  reg("GroundPlane", "Piano di terra On/Off, Altitude, ShadowOnly", CmdGroundPlane, true);
  reg("SetDisplayMode", "Modalita' di visualizzazione della vista attiva", CmdSetDisplayMode, false);
  reg("SetMaterial", "Materiale degli oggetti selezionati (Color, Gloss, Reflectivity, Transparency, IOR; PBR=Yes con BaseColor, Metallic, Roughness, Specular, Clearcoat, Sheen, Opacity, Emission)", CmdSetMaterial, true);
}

}  // namespace cad
