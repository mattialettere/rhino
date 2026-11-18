// Texture e mappature: ApplyTexture (Kurva3D) assegna agli oggetti un materiale con
// un'immagine; ApplyBoxMapping, ApplyPlanarMapping, ApplyCylindricalMapping,
// ApplySphericalMapping e ApplySurfaceMapping (Rhino 8) scelgono come l'immagine
// si avvolge sull'oggetto; RemoveTexture toglie il materiale.
#include "commands/Engine.h"
#include "commands/Materials.h"
#include "core/Document3dm.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace cad {

const ON_UUID& MappingPlugInId() { return ON_UniversalRenderEngineId; }

namespace {

constexpr int kChannel = 1;  // canale di mappatura delle texture dei materiali
const wchar_t* kTextureKey = L"Kurva3D.Texture";

std::string FileName(const std::string& path) {
  const size_t s = path.find_last_of("/\\");
  return s == std::string::npos ? path : path.substr(s + 1);
}

std::string Stem(const std::string& path) {
  std::string n = FileName(path);
  const size_t d = n.find_last_of('.');
  return d == std::string::npos ? n : n.substr(0, d);
}

bool SameFile(const std::string& a, const std::string& b) {
  std::string x = a, y = b;
  for (auto* s : {&x, &y})
    for (auto& c : *s) c = c == '\\' ? '/' : (char)std::tolower((unsigned char)c);
  return x == y;
}

// Immagine incorporata nel documento (una sola copia per file).
void EmbedFile(ONX_Model& model, const std::string& path) {
  ONX_ModelComponentIterator it(model, ON_ModelComponent::Type::EmbeddedFile);
  for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent())
    if (const auto* ef = ON_EmbeddedFile::Cast(c))
      if (SameFile(ToUtf8(ef->Filename()), path)) return;
  auto* ef = new ON_EmbeddedFile();
  if (!ef->LoadFromFile(ON_wString(path.c_str()))) { delete ef; return; }
  model.AddManagedModelComponent(ef, true);
}

// Materiale con la texture (riusato se esiste gia' con la stessa immagine e ripetizione).
int TextureMaterial(ONX_Model& model, const std::string& path, double repeat) {
  char key[64];
  std::snprintf(key, sizeof key, "|%g", repeat);
  const ON_wString tag = ON_wString((path + key).c_str());
  ONX_ModelComponentIterator it(model, ON_ModelComponent::Type::RenderMaterial);
  for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent())
    if (const auto* m = ON_Material::Cast(c))
      if (ON_wString v; m->GetUserString(kTextureKey, v) && v == tag) return m->Index();
  ON_Material mat;
  // nome unico dal nome del file
  std::string name = Stem(path);
  for (int k = 2; model.ComponentFromName(ON_ModelComponent::Type::RenderMaterial, ON_nil_uuid,
                                          ON_wString(name.c_str())).ModelComponent(); k++)
    name = Stem(path) + " (" + std::to_string(k) + ")";
  mat.SetName(ON_wString(name.c_str()));
  mat.SetDiffuse(ON_Color(255, 255, 255));
  ON_Texture tex;
  tex.m_image_file_reference.SetFullPath(ON_wString(path.c_str()), false);
  tex.m_type = ON_Texture::TYPE::bitmap_texture;
  tex.m_mapping_channel_id = kChannel;
  tex.m_uvw = ON_Xform::DiagonalTransformation(repeat, repeat, 1.0);
  mat.AddTexture(tex);
  mat.SetUserString(kTextureKey, tag);
  ON_ModelComponentReference ref = model.AddModelComponent(mat, true);
  return ref.ModelComponent() ? ref.ModelComponent()->Index() : -1;
}

ON_BoundingBox ObjectsBox(Engine& e, const std::vector<ON_UUID>& ids) {
  ON_BoundingBox bb;
  for (const auto& id : ids)
    if (const auto* o = e.Object(id))
      if (const ON_Geometry* g = o->Geometry(nullptr)) g->GetTightBoundingBox(bb, true);
  return bb;
}

// Mappatura adattata all'ingombro (come l'opzione "BoundingBox" di Rhino).
bool MakeMapping(const std::string& kind, const ON_BoundingBox& bbIn, ON_TextureMapping& tm) {
  if (kind == "surface") return tm.SetSurfaceParameterMapping();
  if (!bbIn.IsValid()) return false;
  ON_BoundingBox bb = bbIn;
  const double big = std::max({bb.m_max.x - bb.m_min.x, bb.m_max.y - bb.m_min.y, bb.m_max.z - bb.m_min.z, 1e-6});
  for (int i = 0; i < 3; i++)  // lati piatti: spessore minimo per non dividere per zero
    if (bb.m_max[i] - bb.m_min[i] < big * 1e-3) { bb.m_min[i] -= big * 5e-4; bb.m_max[i] += big * 5e-4; }
  const ON_3dVector d = bb.m_max - bb.m_min;
  if (kind == "planar")
    return tm.SetPlaneMapping(ON_Plane(bb.m_min, ON_3dVector::XAxis, ON_3dVector::YAxis), ON_Interval(0, d.x),
                              ON_Interval(0, d.y), ON_Interval(0, d.z));
  if (kind == "box")
    return tm.SetBoxMapping(ON_Plane(bb.m_min, ON_3dVector::XAxis, ON_3dVector::YAxis), ON_Interval(0, d.x),
                            ON_Interval(0, d.y), ON_Interval(0, d.z), true);
  if (kind == "cylindrical") {
    const ON_3dPoint base((bb.m_min.x + bb.m_max.x) / 2, (bb.m_min.y + bb.m_max.y) / 2, bb.m_min.z);
    ON_Cylinder cyl(ON_Circle(ON_Plane(base, ON_3dVector::ZAxis), std::max(d.x, d.y) / 2), d.z);
    return tm.SetCylinderMapping(cyl, true);
  }
  if (kind == "spherical") return tm.SetSphereMapping(ON_Sphere(bb.Center(), d.Length() / 2));
  return false;
}

bool SetMapping(Engine& e, const std::vector<ON_UUID>& ids, const std::string& kind) {
  ON_TextureMapping tm;
  if (!MakeMapping(kind, ObjectsBox(e, ids), tm)) return e.Fail("mappatura non valida: " + kind);
  tm.SetName(ON_wString((std::string("Kurva3D ") + kind).c_str()));
  ON_ModelComponentReference ref = e.Model().AddModelComponent(tm, true);
  if (!ref.ModelComponent()) return e.Fail("mappatura non aggiunta");
  const ON_UUID mid = ref.ModelComponent()->Id();
  for (const auto& id : ids)
    if (auto* a = e.EditableAttributes(id)) {
      a->m_rendering_attributes.DeleteMappingChannel(MappingPlugInId(), kChannel);
      a->m_rendering_attributes.AddMappingChannel(MappingPlugInId(), kChannel, mid);
    }
  return true;
}

// Mappatura predefinita: parametri della superficie per superfici e solidi NURBS,
// box per mesh, SubD e il resto.
std::string DefaultMapping(Engine& e, const std::vector<ON_UUID>& ids) {
  for (const auto& id : ids) {
    const auto* o = e.Object(id);
    const ON_Geometry* g = o ? o->Geometry(nullptr) : nullptr;
    if (!g || !(ON_Brep::Cast(g) || ON_Surface::Cast(g) || ON_Extrusion::Cast(g))) return "box";
  }
  return "surface";
}

std::string MappingOption(std::string v) {
  if (v == "plane" || v == "planar") return "planar";
  if (v == "cylinder" || v == "cylindrical") return "cylindrical";
  if (v == "sphere" || v == "spherical") return "spherical";
  if (v == "surface" || v == "box") return v;
  return "";
}

bool CmdApplyTexture(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("ApplyTexture", "seleziona gli oggetti, poi Invio");
  if (sel.empty()) return false;
  opts.merge(e.ReadOptions(ts));
  std::string file = opts.count("file") ? opts["file"] : "";
  if (file.empty()) {
    auto f = e.ReadString(ts, "file dell'immagine (PNG, JPG, BMP)");
    if (!f) return false;
    file = *f;
  }
  if (file.size() >= 2 && file.front() == '"' && file.back() == '"') file = file.substr(1, file.size() - 2);
  std::error_code ec;
  if (!std::filesystem::is_regular_file(std::filesystem::u8path(file), ec)) return e.Fail("immagine non trovata: " + file);
  // percorso assoluto: Rhino cerca l'immagine (o il file incorporato) con il percorso completo
  if (auto abs = std::filesystem::absolute(std::filesystem::u8path(file), ec); !ec)
  {
    const auto u = abs.lexically_normal().u8string();
    file.assign(u.begin(), u.end());
  }
  double repeat = 1.0;
  if (opts.count("repeat")) try { repeat = std::stod(opts["repeat"]); } catch (...) {}
  if (!(repeat > 0)) repeat = 1.0;
  std::string kind = opts.count("mapping") ? MappingOption(opts["mapping"]) : "";
  if (opts.count("mapping") && kind.empty()) return e.Fail("mappatura sconosciuta: " + opts["mapping"]);
  ONX_Model& model = e.Model();
  if (!opts.count("embed") || opts["embed"] != "no") EmbedFile(model, file);
  const int mi = TextureMaterial(model, file, repeat);
  if (mi < 0) return e.Fail("materiale non creato");
  for (const auto& id : sel)
    if (auto* a = e.EditableAttributes(id)) {
      a->m_material_index = mi;
      a->SetMaterialSource(ON::material_from_object);
    }
  // la mappatura si imposta se richiesta o se l'oggetto non ne ha ancora una
  std::vector<ON_UUID> need;
  for (const auto& id : sel) {
    const auto* o = e.Object(id);
    const auto* a = o ? o->Attributes(nullptr) : nullptr;
    if (!kind.empty() || !a || !a->m_rendering_attributes.MappingChannel(MappingPlugInId(), kChannel)) need.push_back(id);
  }
  // ogni oggetto ha la sua mappatura adattata al proprio ingombro
  for (const auto& id : need)
    if (!SetMapping(e, {id}, kind.empty() ? DefaultMapping(e, {id}) : kind)) return false;
  e.Out() << "  Texture " << FileName(file) << " applicata a " << sel.size() << " oggetti\n";
  return true;
}

bool MappingCommand(Engine& e, TokenStream& ts, const char* name, const char* kind) {
  auto sel = e.RequireSelection(name, "seleziona gli oggetti, poi Invio");
  if (sel.empty()) return false;
  if (!SetMapping(e, sel, kind)) return false;
  e.Out() << "  Mappatura " << kind << " su " << sel.size() << " oggetti\n";
  return true;
}

bool CmdRemoveTexture(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("RemoveTexture", "seleziona gli oggetti, poi Invio");
  if (sel.empty()) return false;
  for (const auto& id : sel)
    if (auto* a = e.EditableAttributes(id)) {
      a->m_material_index = -1;
      a->SetMaterialSource(ON::material_from_layer);
      a->m_rendering_attributes.DeleteMappingChannel(MappingPlugInId(), kChannel);
    }
  e.Out() << "  Texture tolta da " << sel.size() << " oggetti\n";
  return true;
}

}  // namespace

bool GetObjectTexture(const ONX_Model& model, const ON_3dmObjectAttributes& a, ObjectTexture& out) {
  int mi = -1;
  if (a.MaterialSource() == ON::material_from_object) mi = a.m_material_index;
  else if (a.MaterialSource() == ON::material_from_layer) {
    if (const auto* l = ON_Layer::Cast(model.ComponentFromIndex(ON_ModelComponent::Type::Layer, a.m_layer_index).ModelComponent()))
      mi = l->RenderMaterialIndex();
  }
  if (mi < 0) return false;
  const auto* m = ON_Material::Cast(model.ComponentFromIndex(ON_ModelComponent::Type::RenderMaterial, mi).ModelComponent());
  if (!m) return false;
  const int ti = m->FindTexture(nullptr, ON_Texture::TYPE::bitmap_texture);
  if (ti < 0 || !m->m_textures[ti].m_bOn) return false;
  const ON_Texture& t = m->m_textures[ti];
  out.file = ToUtf8(t.m_image_file_reference.FullPath());
  if (out.file.empty()) out.file = ToUtf8(t.m_image_file_reference.RelativePath());
  if (out.file.empty()) return false;
  out.uvw = t.m_uvw;
  out.diffuse = m->Diffuse();
  out.hasMapping = false;
  if (const ON_MappingChannel* ch = a.m_rendering_attributes.MappingChannel(MappingPlugInId(), (int)t.m_mapping_channel_id))
    if (const auto* tm = ON_TextureMapping::Cast(
            model.ComponentFromId(ON_ModelComponent::Type::TextureMapping, ch->m_mapping_id).ModelComponent())) {
      out.mapping = *tm;
      out.hasMapping = true;
    }
  return true;
}

std::string ResolveTextureFile(const ONX_Model& model, const std::string& file, const std::string& cacheDir) {
  namespace fs = std::filesystem;
  std::error_code ec;
  if (fs::is_regular_file(fs::u8path(file), ec)) return file;
  const std::string name = FileName(file);
  ONX_ModelComponentIterator it(model, ON_ModelComponent::Type::EmbeddedFile);
  for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent()) {
    const auto* ef = ON_EmbeddedFile::Cast(c);
    if (!ef || !SameFile(FileName(ToUtf8(ef->Filename())), name)) continue;
    if (cacheDir.empty()) return "";
    fs::create_directories(fs::u8path(cacheDir), ec);
    char tag[16];
    std::snprintf(tag, sizeof tag, "%08x-", (unsigned)ef->Length());
    const std::string out = cacheDir + "/" + tag + name;
    if (fs::is_regular_file(fs::u8path(out), ec) || ef->SaveToFile(ON_wString(out.c_str()))) return out;
    return "";
  }
  return "";
}

void RegisterMaterialCommands(Engine& e) {
  auto reg = [&](const char* n, const char* d, CommandFn f) {
    CommandInfo i{n, "Rendering", d, f};
    i.modifiesModel = true;
    e.Register(i);
  };
  reg("ApplyTexture", "Assegna agli oggetti un materiale con un'immagine (File=, Mapping=, Repeat=)", CmdApplyTexture);
  reg("RemoveTexture", "Toglie materiale e mappatura della texture dagli oggetti", CmdRemoveTexture);
  reg("ApplyBoxMapping", "Mappatura della texture a box (ingombro degli oggetti)",
      [](Engine& e, TokenStream& ts) { return MappingCommand(e, ts, "ApplyBoxMapping", "box"); });
  reg("ApplyPlanarMapping", "Mappatura della texture piana (dall'alto)",
      [](Engine& e, TokenStream& ts) { return MappingCommand(e, ts, "ApplyPlanarMapping", "planar"); });
  reg("ApplyCylindricalMapping", "Mappatura della texture cilindrica (asse Z)",
      [](Engine& e, TokenStream& ts) { return MappingCommand(e, ts, "ApplyCylindricalMapping", "cylindrical"); });
  reg("ApplySphericalMapping", "Mappatura della texture sferica",
      [](Engine& e, TokenStream& ts) { return MappingCommand(e, ts, "ApplySphericalMapping", "spherical"); });
  reg("ApplySurfaceMapping", "Mappatura della texture sui parametri della superficie",
      [](Engine& e, TokenStream& ts) { return MappingCommand(e, ts, "ApplySurfaceMapping", "surface"); });
}

}  // namespace cad
