#include "core/Document3dm.h"
#include <cmath>

namespace cad {

void InitLibrary() { ON::Begin(); }
void ShutdownLibrary() { ON::End(); }

std::string ToUtf8(const ON_wString& s) {
  ON_String u(s);  // conversione UTF-8 di openNURBS
  return std::string(static_cast<const char*>(u));
}

const char* CategoryName(Category c) {
  switch (c) {
    case Category::Point: return "Punti";
    case Category::PointCloud: return "Nuvole di punti";
    case Category::NurbsCurve: return "Curve NURBS";
    case Category::OtherCurve: return "Altre curve (linee, archi, polilinee, polycurve)";
    case Category::NurbsSurface: return "Superfici NURBS";
    case Category::OtherSurface: return "Altre superfici";
    case Category::Brep: return "B-rep (polisuperfici)";
    case Category::Extrusion: return "Estrusioni";
    case Category::SubD: return "SubD";
    case Category::Mesh: return "Mesh";
    case Category::BlockInstance: return "Istanze di blocco";
    case Category::Annotation: return "Annotazioni";
    case Category::Hatch: return "Retini";
    case Category::Light: return "Luci";
    case Category::Other: return "Altri oggetti";
    case Category::BlockDefinition: return "Definizioni di blocco";
    case Category::Layer: return "Livelli";
    case Category::Material: return "Materiali";
    case Category::Linetype: return "Tipi di linea";
    case Category::Group: return "Gruppi";
    case Category::DimStyle: return "Stili di quota";
    case Category::HatchPattern: return "Motivi retino";
    case Category::TextureMapping: return "Mappature texture";
    case Category::Image: return "Immagini incorporate";
    case Category::NamedView: return "Viste con nome";
    case Category::NamedCPlane: return "CPlane con nome";
    case Category::Layout: return "Tavole (layout)";
    case Category::RenderContent: return "Render content (materiali/ambienti/texture Rhino 8)";
    case Category::EmbeddedFile: return "File incorporati";
    case Category::HistoryRecord: return "Record di cronologia (Rhino)";
    case Category::DocumentUserText: return "Testo utente del documento";
  }
  return "?";
}

Category ClassifyGeometry(const ON_Geometry* g) {
  if (!g) return Category::Other;
  if (ON_Point::Cast(g)) return Category::Point;
  if (ON_PointCloud::Cast(g)) return Category::PointCloud;
  if (ON_NurbsCurve::Cast(g)) return Category::NurbsCurve;
  if (ON_Curve::Cast(g)) return Category::OtherCurve;
  if (ON_Extrusion::Cast(g)) return Category::Extrusion;
  if (ON_NurbsSurface::Cast(g)) return Category::NurbsSurface;
  if (ON_Surface::Cast(g)) return Category::OtherSurface;
  if (ON_Brep::Cast(g)) return Category::Brep;
  if (ON_SubD::Cast(g)) return Category::SubD;
  if (ON_Mesh::Cast(g)) return Category::Mesh;
  if (ON_InstanceRef::Cast(g)) return Category::BlockInstance;
  if (ON_Annotation::Cast(g)) return Category::Annotation;
  if (ON_Hatch::Cast(g)) return Category::Hatch;
  if (ON_Light::Cast(g)) return Category::Light;
  return Category::Other;
}

Document3dm::Document3dm() : m_model(std::make_unique<ONX_Model>()) {}

bool Document3dm::Load(const std::string& path, std::string* log) {
  m_model = std::make_unique<ONX_Model>();
  ON_wString wlog;
  ON_TextLog textlog(wlog);
  const bool ok = m_model->Read(path.c_str(), &textlog);
  if (log) *log = ToUtf8(wlog);
  return ok;
}

namespace {

bool Near(double a, double b) { return std::fabs(a - b) <= 1e-6 * (1.0 + std::fabs(a) + std::fabs(b)); }
bool SameColor(const ON_Color& a, const ON_Color& b) {
  return a.Red() == b.Red() && a.Green() == b.Green() && a.Blue() == b.Blue();
}
bool Same4f(const ON_4fColor& a, const ON_4fColor& b) {
  return Near(a.Red(), b.Red()) && Near(a.Green(), b.Green()) && Near(a.Blue(), b.Blue()) && Near(a.Alpha(), b.Alpha());
}

// Valori del materiale come parametri del render content (nomi di openNURBS).
std::vector<std::pair<const wchar_t*, ON_XMLVariant>> MaterialParams(const ON_Material& m) {
  std::vector<std::pair<const wchar_t*, ON_XMLVariant>> p = {
      {ON_MATERIAL_DIFFUSE, ON_XMLVariant(ON_4fColor(m.Diffuse()))},
      {ON_MATERIAL_SPECULAR, ON_XMLVariant(ON_4fColor(m.Specular()))},
      {ON_MATERIAL_EMISSION, ON_XMLVariant(ON_4fColor(m.Emission()))},
      {ON_MATERIAL_AMBIENT, ON_XMLVariant(ON_4fColor(m.Ambient()))},
      {ON_MATERIAL_SHINE, ON_XMLVariant(m.Shine() / ON_Material::MaxShine)},
      {ON_MATERIAL_TRANSPARENCY_AMOUNT, ON_XMLVariant(m.Transparency())},
      {ON_MATERIAL_REFLECTIVITY_AMOUNT, ON_XMLVariant(m.Reflectivity())},
      {ON_MATERIAL_IOR, ON_XMLVariant(m.m_index_of_refraction)},
  };
  if (m.IsPhysicallyBased()) {
    auto pb = m.PhysicallyBased();
    p.push_back({ON_PBR_MATERIAL_BASE_COLOR, ON_XMLVariant(pb->BaseColor())});
    p.push_back({ON_PBR_MATERIAL_METALLIC, ON_XMLVariant(pb->Metallic())});
    p.push_back({ON_PBR_MATERIAL_ROUGHNESS, ON_XMLVariant(pb->Roughness())});
    p.push_back({ON_PBR_MATERIAL_OPACITY, ON_XMLVariant(pb->Opacity())});
    p.push_back({ON_PBR_MATERIAL_OPACITY_IOR, ON_XMLVariant(pb->OpacityIOR())});
    p.push_back({ON_PBR_MATERIAL_CLEARCOAT, ON_XMLVariant(pb->Clearcoat())});
    p.push_back({ON_PBR_MATERIAL_CLEARCOAT_ROUGHNESS, ON_XMLVariant(pb->ClearcoatRoughness())});
    p.push_back({ON_PBR_MATERIAL_SHEEN, ON_XMLVariant(pb->Sheen())});
    p.push_back({ON_PBR_MATERIAL_SPECULAR, ON_XMLVariant(pb->Specular())});
    p.push_back({ON_PBR_MATERIAL_EMISSION_COLOR, ON_XMLVariant(pb->Emission())});
    p.push_back({ON_PBR_MATERIAL_ALPHA, ON_XMLVariant(pb->Alpha())});
  }
  return p;
}

// Il materiale differisce da quello descritto dal render content?
bool MaterialChanged(const ON_Material& m, const ON_Material& rc) {
  if (!SameColor(m.Diffuse(), rc.Diffuse()) || !SameColor(m.Specular(), rc.Specular()) ||
      !SameColor(m.Emission(), rc.Emission()) || !Near(m.Shine(), rc.Shine()) ||
      !Near(m.Transparency(), rc.Transparency()) || !Near(m.Reflectivity(), rc.Reflectivity()) ||
      !Near(m.m_index_of_refraction, rc.m_index_of_refraction))
    return true;
  if (m.IsPhysicallyBased() != rc.IsPhysicallyBased()) return false;  // tipo diverso: non si tocca
  if (m.IsPhysicallyBased()) {
    auto a = m.PhysicallyBased();
    auto b = rc.PhysicallyBased();
    return !Same4f(a->BaseColor(), b->BaseColor()) || !Near(a->Metallic(), b->Metallic()) ||
           !Near(a->Roughness(), b->Roughness()) || !Near(a->Opacity(), b->Opacity()) ||
           !Near(a->Clearcoat(), b->Clearcoat()) || !Near(a->Sheen(), b->Sheen()) ||
           !Same4f(a->Emission(), b->Emission()) || !Near(a->Alpha(), b->Alpha());
  }
  return false;
}

}  // namespace

int SyncRenderMaterials(ONX_Model& model) {
  int updated = 0;
  ONX_ModelComponentIterator it(model, ON_ModelComponent::Type::RenderMaterial);
  for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent()) {
    const auto* m = ON_Material::Cast(c);
    if (!m || m->RdkMaterialInstanceIdIsNil()) continue;
    ON_ModelComponentReference ref = model.ComponentFromId(ON_ModelComponent::Type::RenderContent, m->RdkMaterialInstanceId());
    auto* rc = const_cast<ON_RenderMaterial*>(ON_RenderMaterial::Cast(ref.ModelComponent()));
    if (!rc) continue;
    const ON_Material cur = rc->ToOnMaterial();
    const bool renamed = !m->Name().IsEmpty() && m->Name() != rc->Name();
    if (!MaterialChanged(*m, cur) && !renamed) continue;
    // Si aggiornano le tre sezioni del nodo (parametri, parametri V8, simulazione)
    // ma solo per i parametri che il render content ha gia': il tipo di materiale
    // (Personalizzato, PBR, Vetro, ...) decide quali esistono.
    ON_XMLRootNode root(rc->XMLNode());  // il nodo <material ...> del render content
    ON_XMLNode* node = &root;
    const auto params = MaterialParams(*m);
    if (ON_XMLNode* sec = node->GetNamedChild(ON_RENDER_CONTENT_PARAMETERS)) {
      ON_XMLParameters p(*sec);
      for (auto& [name, v] : params)
        if (ON_XMLVariant old; p.GetParam(name, old)) p.SetParam(name, v);
    }
    if (ON_XMLNode* sec = node->GetNamedChild(ON_RENDER_CONTENT_PARAMETERS_V8)) {
      ON_XMLParametersV8 p(*sec);
      for (auto& [name, v] : params)
        if (ON_XMLVariant old; p.GetParam(name, old)) p.SetParam(name, v);
    }
    if (ON_XMLNode* sec = node->GetNamedChild(ON_RENDER_CONTENT_SIMULATION)) {
      ON_XMLParameters p(*sec);
      for (auto& [name, v] : params)
        if (ON_XMLVariant old; p.GetParam(name, old)) p.SetParam(name, v);
    }
    if (renamed) {
      ON_XMLProperty prop(ON_wString(L"instance-name"), ON_XMLVariant(m->Name()));
      node->SetProperty(prop);
    }
    if (!rc->SetXML(node->String())) continue;
    if (renamed) rc->SetName(m->Name());
    updated++;
  }
  return updated;
}

bool Document3dm::Save(const std::string& path, int version, std::string* log) const {
  SyncRenderMaterials(*m_model);
  ON_wString wlog;
  ON_TextLog textlog(wlog);
  const bool ok = m_model->Write(path.c_str(), version, &textlog);
  if (log) *log = ToUtf8(wlog);
  return ok;
}

void Document3dm::NewEmpty() {
  m_model = std::make_unique<ONX_Model>();
  auto& ut = m_model->m_settings.m_ModelUnitsAndTolerances;
  ut.m_unit_system = ON_UnitSystem(ON::LengthUnitSystem::Millimeters);
  ut.m_absolute_tolerance = 0.001;
  ut.m_angle_tolerance = ON_PI / 180.0;
  ut.m_relative_tolerance = 0.01;
  const int layer = m_model->AddDefaultLayer(L"Default", ON_Color::Black);
  m_model->m_settings.SetCurrentLayerId(
      m_model->ComponentFromIndex(ON_ModelComponent::Type::Layer, layer).ModelComponent()->Id());
  // Rhino 8 salva sempre almeno uno stile di quota; openNURBS ne aggiunge uno
  // in scrittura se manca, quindi lo creiamo subito per coerenza.
  m_model->AddDefaultDimensionStyle(nullptr, ON::LengthUnitSystem::Millimeters, 0.001);
  m_model->m_properties.m_Application.m_application_name = L"Kurva3D";
}

std::vector<unsigned char> Document3dm::ToBuffer() const {
  ON_Write3dmBufferArchive ar(0, 0, 0, ON::Version());
  if (!m_model->Write(ar, 0, nullptr)) return {};
  const auto* p = static_cast<const unsigned char*>(ar.Buffer());
  return std::vector<unsigned char>(p, p + ar.SizeOfArchive());
}

bool Document3dm::FromBuffer(const std::vector<unsigned char>& buffer) {
  if (buffer.empty()) return false;
  ON_Read3dmBufferArchive ar(buffer.size(), buffer.data(), false, 0, ON::Version());
  auto model = std::make_unique<ONX_Model>();
  if (!model->Read(ar, nullptr)) return false;
  m_model = std::move(model);
  return true;
}

ModelSummary Document3dm::Summarize() const {
  ModelSummary s;
  const ONX_Model& m = *m_model;
  s.archiveVersion = m.m_3dm_file_version;
  s.rhinoVersion = s.archiveVersion >= 50 ? s.archiveVersion / 10 : s.archiveVersion;
  s.createdBy = ToUtf8(m.m_properties.m_Application.m_application_name);
  s.lastEditedBy = ToUtf8(m.m_properties.m_RevisionHistory.m_sLastEditedBy);

  ONX_ModelComponentIterator it(m, ON_ModelComponent::Type::ModelGeometry);
  for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent()) {
    const auto* mgc = ON_ModelGeometryComponent::Cast(c);
    if (!mgc) continue;
    const ON_Geometry* g = mgc->Geometry(nullptr);
    s.counts[ClassifyGeometry(g)]++;
    if (g) s.geometryClasses[g->ClassId()->ClassName()]++;
  }
  auto table = [&](ON_ModelComponent::Type t, Category c) {
    s.counts[c] = static_cast<int>(m.ActiveComponentCount(t));
  };
  table(ON_ModelComponent::Type::InstanceDefinition, Category::BlockDefinition);
  table(ON_ModelComponent::Type::Layer, Category::Layer);
  table(ON_ModelComponent::Type::RenderMaterial, Category::Material);
  table(ON_ModelComponent::Type::LinePattern, Category::Linetype);
  table(ON_ModelComponent::Type::Group, Category::Group);
  table(ON_ModelComponent::Type::DimStyle, Category::DimStyle);
  table(ON_ModelComponent::Type::HatchPattern, Category::HatchPattern);
  table(ON_ModelComponent::Type::TextureMapping, Category::TextureMapping);
  table(ON_ModelComponent::Type::Image, Category::Image);
  s.counts[Category::NamedView] = m.m_settings.m_named_views.Count();
  s.counts[Category::NamedCPlane] = m.m_settings.m_named_cplanes.Count();
  int layouts = 0;
  for (int i = 0; i < m.m_settings.m_views.Count(); i++)
    if (m.m_settings.m_views[i].m_view_type == ON::view_type::page_view_type) layouts++;
  s.counts[Category::Layout] = layouts;
  table(ON_ModelComponent::Type::RenderContent, Category::RenderContent);
  table(ON_ModelComponent::Type::EmbeddedFile, Category::EmbeddedFile);
  table(ON_ModelComponent::Type::HistoryRecord, Category::HistoryRecord);
  ON_ClassArray<ON_UserString> us;
  s.counts[Category::DocumentUserText] = m.GetDocumentUserStrings(us);
  return s;
}

}  // namespace cad
