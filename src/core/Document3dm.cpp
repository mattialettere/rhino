#include "core/Document3dm.h"

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

bool Document3dm::Save(const std::string& path, int version, std::string* log) const {
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
  m_model->m_properties.m_Application.m_application_name = L"Rhinoceros CAD (personale)";
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
  return s;
}

}  // namespace cad
