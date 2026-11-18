#include <cstdio>
#include <cstdlib>
#include "core/ModelCompare.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <regex>
#include <sstream>

namespace cad {
namespace {

std::string IdStr(const ON_UUID& id) {
  char buf[64];
  ON_UuidToString(id, buf);
  return buf;
}


// Impronta della geometria: SHA-1 dei byte che openNURBS scrive per l'oggetto
// in formato Rhino 8. Non usiamo ON_Object::DataCRC perche' per alcune classi
// (ON_PolyCurve, ON_ClippingPlaneSurface) include indirizzi di memoria e non e'
// deterministico tra due letture dello stesso file.
std::string GeometryHash(const ON_Geometry& g) {
  // SubD: la serializzazione contiene un contatore di runtime che cambia a
  // ogni lettura; si usa l'hash geometrico fornito da openNURBS.
  if (const ON_SubD* subd = ON_SubD::Cast(&g)) {
    return "subd:" + std::string(static_cast<const char*>(subd->GeometryHash().ToUTF8String(false))) +
           ":" + std::string(static_cast<const char*>(subd->TopologyHash().ToUTF8String(false)));
  }
  // I dati utente (plug-in, metadati) sono confrontati a parte: includono la
  // versione d'archivio in cui furono scritti, che cambia legittimamente.
  std::unique_ptr<ON_Object> copy(g.Duplicate());
  if (!copy) return "errore-copia";
  copy->PurgeUserData();
  // ON_Brep salva in archivio un valore in cache "e' solido?" (0 = non
  // calcolato): lo calcoliamo sempre, cosi' la cache non conta nel confronto.
  if (const ON_Brep* b = ON_Brep::Cast(copy.get())) (void)b->IsSolid();
  ON_Write3dmBufferArchive ar(0, 0, 80, ON::Version());
  if (!ar.WriteObject(*copy)) return "errore-serializzazione";
  ON_SHA1 sha;
  sha.AccumulateBytes(ar.Buffer(), ar.SizeOfArchive());
  std::string h = static_cast<const char*>(sha.Hash().ToUTF8String(false));
  if (const char* dir = std::getenv("CAD_HASH_DUMP")) {  // diagnostica: byte serializzati per hash
    if (FILE* f = std::fopen((std::string(dir) + "/" + h + ".bin").c_str(), "wb")) {
      std::fwrite(ar.Buffer(), 1, ar.SizeOfArchive(), f);
      std::fclose(f);
    }
  }
  return h;
}

// Elenco dei dati utente attaccati a un oggetto (id del tipo di dato).
std::string UserDataList(const ON_Object& o) {
  std::string out;
  for (const ON_UserData* ud = o.FirstUserData(); ud; ud = ud->Next()) {
    if (!out.empty()) out += ",";
    out += IdStr(ud->m_userdata_uuid);
  }
  return out;
}

// Chiave stabile di una voce di tabella. Gli id delle voci predefinite (es.
// stili di quota di sistema) vengono generati a ogni lettura, quindi le voci
// con nome si identificano per nome (per i livelli: percorso completo).
std::string ComponentKey(const ONX_Model& m, const ON_ModelComponent* c) {
  if (!c) return "-";
  std::string name = ToUtf8(c->Name());
  if (name.empty()) return "id:" + IdStr(c->Id());
  if (const auto* l = ON_Layer::Cast(c)) {
    ON_UUID parent = l->ParentLayerId();
    for (int depth = 0; depth < 64 && ON_UuidIsNotNil(parent); depth++) {
      const auto* pl = ON_Layer::Cast(m.ComponentFromId(ON_ModelComponent::Type::Layer, parent).ModelComponent());
      if (!pl) break;
      name = ToUtf8(pl->Name()) + "::" + name;
      parent = pl->ParentLayerId();
    }
  }
  return name;
}

std::string RefId(const ONX_Model& m, ON_ModelComponent::Type t, int index) {
  if (index < 0) return "-";
  ON_ModelComponentReference r = m.ComponentFromIndex(t, index);
  const ON_ModelComponent* c = r.ModelComponent();
  return c ? ComponentKey(m, c) : ("idx" + std::to_string(index));
}

std::string Sha1Of(const void* data, size_t n) {
  ON_SHA1 sha;
  sha.AccumulateBytes(data, n);
  return static_cast<const char*>(sha.Hash().ToUTF8String(false));
}

std::string Sha1Of(const ON_wString& w) {
  const std::string u = ToUtf8(w);
  return Sha1Of(u.data(), u.size());
}

// XML dei render content con gli attributi di ogni tag in ordine alfabetico:
// openNURBS li riscrive in ordine inverso a ogni salvataggio, senza cambiarne il senso.
// Vedi docs/DIFFERENZE.md D-068.
std::string CanonicalXml(const ON_wString& w) {
  const std::string x = ToUtf8(w);
  static const std::regex tag(R"(<([A-Za-z_][\w.-]*)((?:\s+[\w.:-]+="[^"]*")*)\s*(/?)>)");
  static const std::regex attr(R"([\w.:-]+="[^"]*")");
  std::string out;
  auto last = x.cbegin();
  for (std::sregex_iterator it(x.begin(), x.end(), tag), end; it != end; ++it) {
    const std::smatch& m = *it;
    out.append(last, m[0].first);
    std::vector<std::string> attrs;
    const std::string a = m[2].str();
    // instance-name: il nome si confronta a parte (openNURBS da' un nome a quelli vuoti)
    for (std::sregex_iterator jt(a.begin(), a.end(), attr); jt != std::sregex_iterator(); ++jt)
      if (jt->str().rfind("instance-name=", 0) != 0) attrs.push_back(jt->str());
    std::sort(attrs.begin(), attrs.end());
    out += "<" + m[1].str();
    for (auto& s2 : attrs) out += " " + s2;
    out += m[3].str() + ">";
    last = m[0].second;
  }
  out.append(last, x.cend());
  return out;
}

std::string Num(double v) {
  char b[48];
  std::snprintf(b, sizeof b, "%.9g", v);
  return b;
}

std::string Pt(const ON_3dPoint& p) { return Num(p.x) + "," + Num(p.y) + "," + Num(p.z); }

std::string UserStrings(const ON_Object& o) {
  ON_ClassArray<ON_UserString> us;
  o.GetUserStrings(us);
  std::map<std::string, std::string> kv;
  for (int i = 0; i < us.Count(); i++) kv[ToUtf8(us[i].m_key)] = ToUtf8(us[i].m_string_value);
  std::string out;
  for (auto& [k, v] : kv) out += k + "=" + v + ";";
  return out;
}

std::map<std::string, ObjectSnapshot> CollectObjects(const ONX_Model& m) {
  std::map<std::string, ObjectSnapshot> out;
  ONX_ModelComponentIterator it(m, ON_ModelComponent::Type::ModelGeometry);
  for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent()) {
    const auto* mgc = ON_ModelGeometryComponent::Cast(c);
    if (!mgc) continue;
    const ON_Geometry* g = mgc->Geometry(nullptr);
    const ON_3dmObjectAttributes* at = mgc->Attributes(nullptr);
    ObjectSnapshot k;
    if (g) {
      k.cls = g->ClassId()->ClassName();
      // Bounding box "stretta" ricalcolata dalla geometria: i file di Rhino 1-2
      // contengono box in cache larghe (dal poligono di controllo) che in
      // scrittura vengono sostituite da quelle esatte. Vedi docs/DIFFERENZE.md.
      if (!g->GetTightBoundingBox(k.bbox)) k.bbox = g->BoundingBox();
      // ON_DimOrdinate calcola la box con lo stile predefinito e, su Windows con i
      // font reali, puo' rifare il testo in cache in modo diverso prima e dopo la
      // lettura: si confrontano i punti di definizione (i dati salvati nel file).
      // Testo, quote e leader: la box dipende dal testo impaginato con i caratteri del
      // sistema (cache di runtime, non salvata): un modello appena creato da riga di comando
      // e lo stesso riletto possono differire. I dati salvati si confrontano con l'hash.
      if (ON_Annotation::Cast(g)) k.bbox = ON_BoundingBox();
      if (const auto* dor = ON_DimOrdinate::Cast(g)) {
        ON_3dPoint pts[4];
        if (dor->Get3dPoints(nullptr, &pts[0], &pts[1], &pts[2], &pts[3])) { k.bbox = ON_BoundingBox(); for (auto& q : pts) k.bbox.Set(q, true); }
      }
      k.hash = GeometryHash(*g);
      k.userData = UserDataList(*g);
    }
    ON_UUID id = at ? at->m_uuid : c->Id();
    if (at) {
      k.name = ToUtf8(at->m_name);
      k.layer = RefId(m, ON_ModelComponent::Type::Layer, at->m_layer_index);
      k.color = static_cast<unsigned int>(at->m_color);
      k.colorSource = static_cast<int>(at->ColorSource());
      k.mode = static_cast<int>(at->Mode());
      k.linetype = RefId(m, ON_ModelComponent::Type::LinePattern, at->m_linetype_index);
      k.materialSource = static_cast<int>(at->MaterialSource());
      // Il materiale conta solo se l'oggetto lo usa (sorgente = oggetto).
      if (at->MaterialSource() == ON::material_from_object)
        k.material = RefId(m, ON_ModelComponent::Type::RenderMaterial, at->m_material_index);
      k.visible = at->IsVisible();
      k.userText = UserStrings(*at);
      std::vector<std::string> gs;
      for (int i = 0; i < at->GroupCount(); i++)
        gs.push_back(RefId(m, ON_ModelComponent::Type::Group, at->GroupList()[i]));
      std::sort(gs.begin(), gs.end());
      for (auto& g2 : gs) k.groups += g2 + ";";
      k.print = std::to_string(static_cast<unsigned int>(at->m_plot_color)) + "/" +
                std::to_string(static_cast<int>(at->PlotColorSource())) + "|" + Num(at->m_plot_weight_mm) + "/" +
                std::to_string(static_cast<int>(at->PlotWeightSource()));
      k.space = at->m_space == ON::page_space ? "pagina:" + IdStr(at->m_viewport_id) : "modello";
    }
    out[IdStr(id)] = k;
  }
  return out;
}

// Massimo scostamento tra due bounding box (infinito se una sola e' valida).
double BoxDelta(const ON_BoundingBox& a, const ON_BoundingBox& b) {
  if (a.IsValid() != b.IsValid()) return ON_DBL_MAX;
  if (!a.IsValid()) return 0.0;
  double d = 0.0;
  for (int i = 0; i < 3; i++) {
    d = std::max(d, std::fabs(a.m_min[i] - b.m_min[i]));
    d = std::max(d, std::fabs(a.m_max[i] - b.m_max[i]));
  }
  return d;
}

// Firma testuale di una voce di tabella (livello, materiale, ...)
std::string TableSig(const ONX_Model& m, const ON_ModelComponent* c) {
  std::ostringstream s;
  s << ToUtf8(c->Name());
  if (const auto* l = ON_Layer::Cast(c)) {
    s << "|col=" << static_cast<unsigned int>(l->Color())
      << "|plotcol=" << static_cast<unsigned int>(l->PlotColor())
      << "|vis=" << l->IsVisible() << "|lock=" << l->IsLocked()
      << "|lt=" << RefId(m, ON_ModelComponent::Type::LinePattern, l->LinetypeIndex())
      << "|pw=" << l->PlotWeight()
      << "|mat=" << l->RenderMaterialIndex() << "|us=" << UserStrings(*l);
  } else if (const auto* mat = ON_Material::Cast(c)) {
    s << "|diff=" << static_cast<unsigned int>(mat->Diffuse())
      << "|spec=" << static_cast<unsigned int>(mat->Specular())
      << "|shine=" << mat->Shine() << "|transp=" << mat->Transparency()
      << "|tex=" << mat->m_textures.Count()
      << "|refl=" << mat->Reflectivity() << "|ior=" << mat->m_index_of_refraction
      << "|emis=" << static_cast<unsigned int>(mat->Emission())
      << "|rdk=" << IdStr(mat->RdkMaterialInstanceId());
    for (int i = 0; i < mat->m_textures.Count(); i++)
      s << "|t" << i << "=" << static_cast<int>(mat->m_textures[i].m_type) << ":"
        << ToUtf8(mat->m_textures[i].m_image_file_reference.FullPath()) << ":"
        << mat->m_textures[i].m_mapping_channel_id;
    if (mat->IsPhysicallyBased()) {
      auto pb = mat->PhysicallyBased();
      const ON_4fColor bc = pb->BaseColor();
      s << "|pbr=" << bc.Red() << "," << bc.Green() << "," << bc.Blue() << "," << bc.Alpha()
        << "|metal=" << pb->Metallic() << "|rough=" << pb->Roughness() << "|opac=" << pb->Opacity()
        << "|ccoat=" << pb->Clearcoat() << "|sheen=" << pb->Sheen() << "|alpha=" << pb->Alpha();
    }
  } else if (const auto* idef = ON_InstanceDefinition::Cast(c)) {
    s << "|n=" << idef->InstanceGeometryIdList().Count()
      << "|type=" << static_cast<int>(idef->InstanceDefinitionType());
  } else if (const auto* lt = ON_Linetype::Cast(c)) {
    s << "|segs=" << lt->SegmentCount() << "|len=" << lt->PatternLength();
    for (int i = 0; i < lt->SegmentCount(); i++) {
      const ON_LinetypeSegment sg = lt->Segment(i);
      s << "|" << (sg.m_seg_type == ON_LinetypeSegment::eSegType::stLine ? "L" : "S") << sg.m_length;
    }
  } else if (const auto* ds = ON_DimStyle::Cast(c)) {
    s << "|txth=" << ds->TextHeight() << "|arrow=" << ds->ArrowSize()
      << "|scale=" << ds->DimScale();
  }
  return s.str();
}

std::map<std::string, std::string> CollectTable(const ONX_Model& m, ON_ModelComponent::Type t) {
  std::map<std::string, std::string> out;
  ONX_ModelComponentIterator it(m, t);
  for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent())
    out[ComponentKey(m, c)] = TableSig(m, c);
  return out;
}

const std::pair<ON_ModelComponent::Type, const char*> kTables[] = {
  {ON_ModelComponent::Type::Layer, "Livello"},
  {ON_ModelComponent::Type::RenderMaterial, "Materiale"},
  {ON_ModelComponent::Type::InstanceDefinition, "Definizione di blocco"},
  {ON_ModelComponent::Type::LinePattern, "Tipo di linea"},
  {ON_ModelComponent::Type::Group, "Gruppo"},
  {ON_ModelComponent::Type::DimStyle, "Stile di quota"},
  {ON_ModelComponent::Type::HatchPattern, "Motivo retino"},
  {ON_ModelComponent::Type::TextureMapping, "Mappatura texture"},
};

// Dati del documento confrontati come "tabelle" aggiuntive (chiave -> firma).
void CollectDocumentData(const ONX_Model& m, ModelSnapshot& s) {
  // testo utente del documento (Rhino: SetDocumentUserText)
  {
    auto& t = s.tables["Testo utente del documento"];
    ON_ClassArray<ON_UserString> us;
    m.GetDocumentUserStrings(us);
    for (int i = 0; i < us.Count(); i++) t[ToUtf8(us[i].m_key)] = ToUtf8(us[i].m_string_value);
  }
  // render content di Rhino 8 (materiali, ambienti e texture dell'editor dei materiali)
  {
    auto& t = s.tables["Render content"];
    ONX_ModelComponentIterator it(m, ON_ModelComponent::Type::RenderContent);
    for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent()) {
      const auto* rc = ON_RenderContent::Cast(c);
      if (!rc) continue;
      const char* kind = ON_RenderMaterial::Cast(rc) ? "materiale" : ON_RenderEnvironment::Cast(rc) ? "ambiente"
                         : ON_RenderTexture::Cast(rc)  ? "texture" : "altro";
      t[IdStr(rc->Id())] = std::string(kind) + "|" + ToUtf8(rc->Name()) + "|" + ToUtf8(rc->TypeName()) + "|" +
                           IdStr(rc->TypeId()) + "|xml=" + [&] { const std::string c = CanonicalXml(rc->XML(true)); return Sha1Of(c.data(), c.size()); }();
    }
  }
  // file incorporati (immagini delle texture, ...)
  {
    auto& t = s.tables["File incorporato"];
    ONX_ModelComponentIterator it(m, ON_ModelComponent::Type::EmbeddedFile);
    for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent()) {
      const auto* ef = ON_EmbeddedFile::Cast(c);
      if (!ef) continue;
      ON_Buffer buf;
      std::string sha = "?";
      if (ef->SaveToBuffer(buf)) {
        std::vector<unsigned char> bytes(static_cast<size_t>(buf.Size()));
        buf.SeekFromStart(0);
        if (!bytes.empty()) buf.Read(bytes.size(), bytes.data());
        sha = Sha1Of(bytes.data(), bytes.size());
      }
      t[ToUtf8(ef->Filename())] = std::to_string(ef->Length()) + "|" + sha;
    }
  }
  // record di cronologia di Rhino (ON_HistoryRecord): conservati cosi' come sono
  {
    auto& t = s.tables["Record di cronologia"];
    ONX_ModelComponentIterator it(m, ON_ModelComponent::Type::HistoryRecord);
    for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent()) {
      ON_Write3dmBufferArchive ar(0, 0, 80, ON::Version());
      std::string h = "?";
      if (ar.WriteObject(*c)) h = Sha1Of(ar.Buffer(), ar.SizeOfArchive());
      t[IdStr(c->Id())] = h;
    }
  }
  // viste con nome, CPlane con nome, tavole
  {
    auto& nv = s.tables["Vista con nome"];
    for (int i = 0; i < m.m_settings.m_named_views.Count(); i++) {
      const ON_3dmView& v = m.m_settings.m_named_views[i];
      nv[ToUtf8(v.m_name) + "#" + std::to_string(i)] =
          Pt(v.m_vp.CameraLocation()) + "|" + Pt(ON_3dPoint(v.m_vp.CameraDirection())) + "|" +
          std::to_string(v.m_vp.IsPerspectiveProjection());
    }
    auto& nc = s.tables["CPlane con nome"];
    for (int i = 0; i < m.m_settings.m_named_cplanes.Count(); i++) {
      const ON_3dmConstructionPlane& cp = m.m_settings.m_named_cplanes[i];
      nc[ToUtf8(cp.m_name) + "#" + std::to_string(i)] = Pt(cp.m_plane.origin) + "|" + Pt(ON_3dPoint(cp.m_plane.zaxis));
    }
    auto& lay = s.tables["Tavola"];
    for (int i = 0; i < m.m_settings.m_views.Count(); i++) {
      const ON_3dmView& v = m.m_settings.m_views[i];
      if (v.m_view_type != ON::view_type::page_view_type) continue;
      lay[IdStr(v.m_vp.ViewportId())] = ToUtf8(v.m_name) + "|" + Num(v.m_page_settings.m_width_mm) + "x" +
                                        Num(v.m_page_settings.m_height_mm);
    }
  }
  // sole, cielo, piano di terra (impostazioni di rendering di Rhino 8)
  {
    auto& t = s.tables["Impostazioni di rendering"];
    const ON_3dmRenderSettings& rs = m.m_settings.m_RenderSettings;
    const ON_Sun& sun = rs.Sun();
    t["sole"] = std::to_string(sun.EnableOn()) + "|" + std::to_string(sun.ManualControlOn()) + "|" +
                Num(sun.Azimuth()) + "|" + Num(sun.Altitude());
    const ON_GroundPlane& gp = rs.GroundPlane();
    t["piano di terra"] = std::to_string(gp.Enabled()) + "|" + Num(gp.Altitude()) + "|" + std::to_string(gp.AutoAltitude()) +
                          "|" + std::to_string(gp.ShadowOnly());
    t["cielo"] = std::to_string(rs.Skylight().Enabled());
  }
}

}  // namespace

ModelSnapshot TakeSnapshot(const ONX_Model& m) {
  ModelSnapshot s;
  s.objects = CollectObjects(m);
  for (auto& [t, label] : kTables) s.tables[label] = CollectTable(m, t);
  CollectDocumentData(m, s);
  s.namedViews = m.m_settings.m_named_views.Count();
  s.unitSystem = static_cast<int>(m.m_settings.m_ModelUnitsAndTolerances.m_unit_system.UnitSystem());
  s.absTolerance = m.m_settings.m_ModelUnitsAndTolerances.m_absolute_tolerance;
  return s;
}

CompareResult CompareSnapshots(const ModelSnapshot& a, const ModelSnapshot& b, double tol) {
  CompareResult r;
  const auto& oa = a.objects;
  const auto& ob = b.objects;
  for (auto& [id, ka] : oa) {
    auto f = ob.find(id);
    if (f == ob.end()) { r.differences.push_back("Oggetto mancante: " + id + " (" + ka.cls + ")"); continue; }
    const ObjectSnapshot& kb = f->second;
    r.objectsCompared++;
    std::string p = "Oggetto " + id + " (" + ka.cls + "): ";
    if (ka.cls != kb.cls) r.differences.push_back(p + "classe " + kb.cls);
    if (double d = BoxDelta(ka.bbox, kb.bbox); d > tol)
      r.differences.push_back(p + "bounding box diversa (scostamento " + std::to_string(d) + ")");
    if (ka.hash != kb.hash) r.differences.push_back(p + "dati geometrici diversi");
    if (ka.userData != kb.userData) r.differences.push_back(p + "dati utente diversi [" + ka.userData + "] -> [" + kb.userData + "]");
    if (ka.name != kb.name) r.differences.push_back(p + "nome diverso");
    if (ka.layer != kb.layer) r.differences.push_back(p + "livello diverso");
    if (ka.color != kb.color || ka.colorSource != kb.colorSource) r.differences.push_back(p + "colore diverso");
    if (ka.mode != kb.mode || ka.visible != kb.visible) r.differences.push_back(p + "stato (nascosto/bloccato) diverso");
    if (ka.linetype != kb.linetype) r.differences.push_back(p + "tipo di linea diverso");
    if (ka.material != kb.material || ka.materialSource != kb.materialSource) r.differences.push_back(p + "materiale diverso");
    if (ka.userText != kb.userText) r.differences.push_back(p + "testo utente diverso [" + ka.userText + "] -> [" + kb.userText + "]");
    if (ka.groups != kb.groups) r.differences.push_back(p + "gruppi diversi");
    if (ka.print != kb.print) r.differences.push_back(p + "colore o spessore di stampa diverso");
    if (ka.space != kb.space) r.differences.push_back(p + "spazio (modello/pagina) diverso");
  }
  for (auto& [id, kb] : ob)
    if (!oa.count(id)) r.differences.push_back("Oggetto in piu': " + id + " (" + kb.cls + ")");

  for (auto& [label, ta] : a.tables) {
    auto it = b.tables.find(label);
    static const std::map<std::string, std::string> empty;
    const auto& tb = it == b.tables.end() ? empty : it->second;
    for (auto& [key, sig] : ta) {
      auto f = tb.find(key);
      if (f == tb.end()) r.differences.push_back(label + " mancante dopo il salvataggio: " + sig);
      else if (f->second != sig) r.differences.push_back(label + " diverso: [" + sig + "] -> [" + f->second + "]");
    }
    for (auto& [key, sig] : tb)
      if (!ta.count(key)) r.differences.push_back(label + " in piu' dopo il salvataggio: " + sig);
  }
  if (a.namedViews != b.namedViews) r.differences.push_back("Numero di viste con nome diverso");
  if (a.unitSystem != b.unitSystem) r.differences.push_back("Unita' del modello diverse");
  if (a.absTolerance != b.absTolerance) r.differences.push_back("Tolleranza assoluta diversa");
  return r;
}

CompareResult CompareModels(const ONX_Model& a, const ONX_Model& b, double tol) {
  return CompareSnapshots(TakeSnapshot(a), TakeSnapshot(b), tol);
}

}  // namespace cad
