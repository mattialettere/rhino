#include "collab/Patch.h"
#include <memory>

namespace cad::collab {
namespace {

using T = ON_ModelComponent::Type;
constexpr int kMagic = 0x4B335031;  // "K3P1"

// Tabelle sincronizzate, in ordine di dipendenza (i livelli usano tipi di linea e materiali)
const T kTables[] = {T::LinePattern, T::RenderMaterial, T::HatchPattern, T::DimStyle,
                     T::Group, T::Layer, T::InstanceDefinition};

std::string Sha(const void* p, size_t n) {
  ON_SHA1 sha;
  sha.AccumulateBytes(p, n);
  return static_cast<const char*>(sha.Hash().ToUTF8String(false));
}

std::string HashObject(const ON_Object* a, const ON_Object* b = nullptr) {
  if (const ON_Brep* br = ON_Brep::Cast(a)) (void)br->IsSolid();  // valore in cache: serializzazione stabile
  ON_Write3dmBufferArchive ar(0, 0, 80, ON::Version());
  if (a && !ar.WriteObject(*a)) return "-";
  if (b && !ar.WriteObject(*b)) return "-";
  return Sha(ar.Buffer(), ar.SizeOfArchive());
}

bool IsUser(const ON_ModelComponent* c) {
  return c && !c->IsSystemComponent() && c->Index() >= 0 && !ON_UuidIsNil(c->Id());
}

template <class F>
void ForEach(const ONX_Model& m, T type, F f) {
  ONX_ModelComponentIterator it(m, type);
  for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent()) f(c);
}

// id del componente a un indice (nil se non esiste o indice di sistema/negativo)
ON_UUID IdAt(const ONX_Model& m, T type, int index) {
  if (index < 0) return ON_nil_uuid;
  const ON_ModelComponent* c = m.ComponentFromIndex(type, index).ModelComponent();
  return c ? c->Id() : ON_nil_uuid;
}

// indice nel modello di destinazione: per id se presente, altrimenti l'indice originale se negativo
// (componenti di sistema: Continuo, PerLivello...), altrimenti "fallback"
int IndexOf(const ONX_Model& m, T type, const ON_UUID& id, int raw, int fallback) {
  if (raw < 0) return raw;
  if (!ON_UuidIsNil(id)) {
    const ON_ModelComponent* c = m.ComponentFromId(type, id).ModelComponent();
    if (c) return c->Index();
  }
  return fallback;
}

const ON_ModelGeometryComponent* ObjectById(const ONX_Model& m, const ON_UUID& id) {
  const auto* g = ON_ModelGeometryComponent::Cast(m.ComponentFromId(T::ModelGeometry, id).ModelComponent());
  if (g) return g;
  return ON_ModelGeometryComponent::Cast(m.ComponentFromId(T::RenderLight, id).ModelComponent());
}

void WriteRef(ON_BinaryArchive& ar, const ONX_Model& m, T type, int index) {
  ar.WriteInt(index);
  ar.WriteUuid(IdAt(m, type, index));
}
struct Ref { int raw = -1; ON_UUID id = ON_nil_uuid; };
bool ReadRef(ON_BinaryArchive& ar, Ref& r) { return ar.ReadInt(&r.raw) && ar.ReadUuid(r.id); }

// Copia "src" su "dst" conservando indice, id e numero di serie del modello di destinazione
template <class C>
bool AssignKeep(ON_ModelComponent* dst, const ON_Object* src) {
  C* d = C::Cast(dst);
  const C* s = C::Cast(src);
  if (!d || !s) return false;
  const int idx = d->Index();
  const ON_UUID id = d->Id();
  const unsigned int sn = d->ModelSerialNumber(), rsn = d->ReferenceModelSerialNumber(),
                     isn = d->InstanceDefinitionModelSerialNumber();
  *d = *s;
  if (d->Index() != idx) d->SetIndex(idx);
  if (ON_UuidCompare(d->Id(), id) != 0) d->SetId(id);
  if (d->ModelSerialNumber() != sn) d->SetModelSerialNumber(sn, rsn, isn);
  return true;
}

bool AssignTable(T type, ON_ModelComponent* dst, const ON_Object* src) {
  switch (type) {
    case T::Layer: return AssignKeep<ON_Layer>(dst, src);
    case T::RenderMaterial: return AssignKeep<ON_Material>(dst, src);
    case T::LinePattern: return AssignKeep<ON_Linetype>(dst, src);
    case T::DimStyle: return AssignKeep<ON_DimStyle>(dst, src);
    case T::Group: return AssignKeep<ON_Group>(dst, src);
    case T::InstanceDefinition: return AssignKeep<ON_InstanceDefinition>(dst, src);
    case T::HatchPattern: return AssignKeep<ON_HatchPattern>(dst, src);
    default: return false;
  }
}

std::map<std::wstring, std::wstring> UserStrings(const ONX_Model& m) {
  std::map<std::wstring, std::wstring> out;
  ON_ClassArray<ON_UserString> list;
  m.GetDocumentUserStrings(list);
  for (int i = 0; i < list.Count(); i++)
    out[static_cast<const wchar_t*>(list[i].m_key)] = static_cast<const wchar_t*>(list[i].m_string_value);
  return out;
}

}  // namespace

Snapshot TakeSnapshot(const ONX_Model& m) {
  Snapshot s;
  for (T type : {T::ModelGeometry, T::RenderLight})
    ForEach(m, type, [&](const ON_ModelComponent* c) {
      const auto* g = ON_ModelGeometryComponent::Cast(c);
      const ON_3dmObjectAttributes* a = g ? g->Attributes(nullptr) : nullptr;
      if (!g || !a || ON_UuidIsNil(a->m_uuid)) return;
      s.objects[a->m_uuid] = HashObject(g->Geometry(nullptr), a);
    });
  for (T type : kTables)
    ForEach(m, type, [&](const ON_ModelComponent* c) {
      if (IsUser(c)) s.tables[{static_cast<int>(type), c->Id()}] = HashObject(c);
    });
  s.userStrings = UserStrings(m);
  return s;
}

std::string MakePatch(const ONX_Model& m, const Snapshot& base, PatchInfo* info, Snapshot* nowOut) {
  PatchInfo pi;
  Snapshot now = TakeSnapshot(m);
  ON_Write3dmBufferArchive ar(0, 0, 80, ON::Version());
  ar.WriteInt(kMagic);

  // --- tabelle: prima le modifiche (nell'ordine delle dipendenze), poi le eliminazioni
  std::vector<std::pair<T, const ON_ModelComponent*>> ups;
  std::vector<std::pair<T, ON_UUID>> dels;
  for (T type : kTables)
    ForEach(m, type, [&](const ON_ModelComponent* c) {
      if (!IsUser(c)) return;
      auto it = base.tables.find({static_cast<int>(type), c->Id()});
      if (it == base.tables.end() || it->second != now.tables.at({static_cast<int>(type), c->Id()}))
        ups.push_back({type, c});
    });
  for (const auto& [key, h] : base.tables)
    if (!now.tables.count(key)) dels.push_back({static_cast<T>(key.first), key.second});
  ar.WriteInt(static_cast<int>(ups.size() + dels.size()));
  for (const auto& [type, c] : ups) {
    ar.WriteInt(static_cast<int>(type));
    ar.WriteUuid(c->Id());
    ar.WriteInt(1);
    ar.WriteObject(*c);
    const ON_Layer* l = ON_Layer::Cast(c);
    WriteRef(ar, m, T::LinePattern, l ? l->LinetypeIndex() : -1);
    WriteRef(ar, m, T::RenderMaterial, l ? l->RenderMaterialIndex() : -1);
  }
  for (const auto& [type, id] : dels) {
    ar.WriteInt(static_cast<int>(type));
    ar.WriteUuid(id);
    ar.WriteInt(0);
  }
  pi.tables = static_cast<int>(ups.size() + dels.size());

  // --- oggetti
  std::vector<const ON_ModelGeometryComponent*> objs;
  std::vector<ON_UUID> gone;
  for (const auto& [id, h] : now.objects) {
    auto it = base.objects.find(id);
    if (it == base.objects.end() || it->second != h) objs.push_back(ObjectById(m, id));
  }
  for (const auto& [id, h] : base.objects)
    if (!now.objects.count(id)) gone.push_back(id);
  ar.WriteInt(static_cast<int>(objs.size() + gone.size()));
  for (const auto* g : objs) {
    const ON_3dmObjectAttributes* a = g->Attributes(nullptr);
    ar.WriteUuid(a->m_uuid);
    ar.WriteInt(1);
    ar.WriteObject(*g->Geometry(nullptr));
    ar.WriteObject(*a);
    WriteRef(ar, m, T::Layer, a->m_layer_index);
    WriteRef(ar, m, T::LinePattern, a->m_linetype_index);
    WriteRef(ar, m, T::RenderMaterial, a->m_material_index);
    ON_SimpleArray<int> groups;
    a->GetGroupList(groups);
    ar.WriteInt(groups.Count());
    for (int i = 0; i < groups.Count(); i++) WriteRef(ar, m, T::Group, groups[i]);
    const ON_Hatch* h = ON_Hatch::Cast(g->Geometry(nullptr));
    WriteRef(ar, m, T::HatchPattern, h ? h->PatternIndex() : -1);
  }
  for (const auto& id : gone) {
    ar.WriteUuid(id);
    ar.WriteInt(0);
  }
  pi.upserted = static_cast<int>(objs.size());
  pi.deleted = static_cast<int>(gone.size());

  // --- testo utente del documento (storia, grafo dei nodi, ...)
  std::vector<std::pair<std::wstring, const std::wstring*>> strs;
  for (const auto& [k, v] : now.userStrings) {
    auto it = base.userStrings.find(k);
    if (it == base.userStrings.end() || it->second != v) strs.push_back({k, &v});
  }
  for (const auto& [k, v] : base.userStrings)
    if (!now.userStrings.count(k)) strs.push_back({k, nullptr});
  ar.WriteInt(static_cast<int>(strs.size()));
  for (const auto& [k, v] : strs) {
    ar.WriteString(ON_wString(k.c_str()));
    ar.WriteInt(v ? 1 : 0);
    if (v) ar.WriteString(ON_wString(v->c_str()));
  }
  pi.strings = static_cast<int>(strs.size());
  if (info) *info = pi;
  std::string out(static_cast<const char*>(ar.Buffer()), ar.SizeOfArchive());
  if (nowOut) *nowOut = std::move(now);
  return out;
}

bool ApplyPatch(ONX_Model& m, const std::string& patch, PatchInfo* info, std::string* err) {
  auto fail = [&](const char* msg) {
    if (err) *err = msg;
    return false;
  };
  if (patch.empty()) return fail("patch vuota");
  ON_Read3dmBufferArchive ar(patch.size(), patch.data(), false, 80, ON::Version());
  int magic = 0;
  if (!ar.ReadInt(&magic) || magic != kMagic) return fail("formato della patch non riconosciuto");
  PatchInfo pi;

  int n = 0;
  if (!ar.ReadInt(&n) || n < 0) return fail("tabelle illeggibili");
  for (int i = 0; i < n; i++) {
    int type = 0, op = 0;
    ON_UUID id;
    if (!ar.ReadInt(&type) || !ar.ReadUuid(id) || !ar.ReadInt(&op)) return fail("tabelle illeggibili");
    const T t = static_cast<T>(type);
    if (!op) {
      m.RemoveModelComponent(t, id);
      pi.tables++;
      continue;
    }
    ON_Object* raw = nullptr;
    Ref lt, mat;
    if (!ar.ReadObject(&raw) || !ReadRef(ar, lt) || !ReadRef(ar, mat)) {
      delete raw;
      return fail("voce di tabella illeggibile");
    }
    std::unique_ptr<ON_Object> obj(raw);
    auto* comp = ON_ModelComponent::Cast(obj.get());
    if (!comp || comp->ComponentType() != t) continue;
    if (ON_Layer* l = ON_Layer::Cast(comp)) {
      l->SetLinetypeIndex(IndexOf(m, T::LinePattern, lt.id, lt.raw, -1));
      l->SetRenderMaterialIndex(IndexOf(m, T::RenderMaterial, mat.id, mat.raw, -1));
    }
    const ON_ModelComponent* cur = m.ComponentFromId(t, id).ModelComponent();
    if (cur) AssignTable(t, const_cast<ON_ModelComponent*>(cur), comp);
    else {
      comp->SetId(id);
      comp->ClearIndex();
      // nome gia' usato da un'altra voce (es. "Gruppo 01" creato su entrambi i computer)
      const ON_wString base = comp->Name();
      for (int k = 2; !base.IsEmpty() && !m.ComponentFromName(t, comp->ParentId(), comp->Name()).IsEmpty() && k < 1000; k++)
        comp->SetName(base + ON_wString(L" (") + ON_wString::FormatToString(L"%d", k) + ON_wString(L")"));
      m.AddModelComponent(*comp, true);
    }
    pi.tables++;
  }

  if (!ar.ReadInt(&n) || n < 0) return fail("oggetti illeggibili");
  const int defaultLayer = [&] {
    const ON_ModelComponent* c = m.ComponentFromId(T::Layer, m.m_settings.CurrentLayerId()).ModelComponent();
    return c ? c->Index() : 0;
  }();
  for (int i = 0; i < n; i++) {
    ON_UUID id;
    int op = 0;
    if (!ar.ReadUuid(id) || !ar.ReadInt(&op)) return fail("oggetti illeggibili");
    if (!op) {
      if (m.RemoveModelComponent(T::ModelGeometry, id).IsEmpty()) m.RemoveModelComponent(T::RenderLight, id);
      pi.deleted++;
      continue;
    }
    ON_Object *rg = nullptr, *ra = nullptr;
    Ref layer, lt, mat, hatch;
    int ng = 0;
    bool ok = ar.ReadObject(&rg) && ar.ReadObject(&ra) && ReadRef(ar, layer) && ReadRef(ar, lt) &&
              ReadRef(ar, mat) && ar.ReadInt(&ng) && ng >= 0;
    std::unique_ptr<ON_Object> geo(rg), att(ra);
    std::vector<Ref> groups(ok ? ng : 0);
    for (auto& g : groups) ok = ok && ReadRef(ar, g);
    ok = ok && ReadRef(ar, hatch);
    if (!ok) return fail("oggetto illeggibile");
    auto* a = ON_3dmObjectAttributes::Cast(att.get());
    if (!ON_Geometry::Cast(geo.get()) || !a) continue;
    a->m_uuid = id;
    a->m_layer_index = IndexOf(m, T::Layer, layer.id, layer.raw, defaultLayer);
    a->m_linetype_index = IndexOf(m, T::LinePattern, lt.id, lt.raw, -1);
    a->m_material_index = IndexOf(m, T::RenderMaterial, mat.id, mat.raw, -1);
    a->RemoveFromAllGroups();
    for (const auto& g : groups) {
      const int gi = IndexOf(m, T::Group, g.id, g.raw, -1);
      if (gi >= 0) a->AddToGroup(gi);
    }
    if (ON_Hatch* h = ON_Hatch::Cast(geo.get())) h->SetPatternIndex(IndexOf(m, T::HatchPattern, hatch.id, hatch.raw, 0));
    if (m.RemoveModelComponent(T::ModelGeometry, id).IsEmpty()) m.RemoveModelComponent(T::RenderLight, id);
    auto* ga = new ON_3dmObjectAttributes(*a);
    if (!m.AddManagedModelGeometryComponent(static_cast<ON_Geometry*>(geo.release()), ga, true).IsEmpty()) pi.upserted++;
  }

  if (!ar.ReadInt(&n) || n < 0) return fail("testi illeggibili");
  for (int i = 0; i < n; i++) {
    ON_wString k, v;
    int has = 0;
    if (!ar.ReadString(k) || !ar.ReadInt(&has) || (has && !ar.ReadString(v))) return fail("testi illeggibili");
    m.SetDocumentUserString(static_cast<const wchar_t*>(k), has ? static_cast<const wchar_t*>(v) : nullptr);
    pi.strings++;
  }
  if (info) *info = pi;
  return true;
}

}  // namespace cad::collab

#include "commands/Engine.h"
namespace cad {
void RegisterCollabCommands(Engine& e) {
  e.Register({"Collaborate", "Collaborazione",
              "Lavoro in piu' persone sullo stesso progetto in rete locale (estensione di Kurva3D)",
              [](Engine& en, TokenStream&) {
                if (!en.showCollaboration) return en.Fail("la collaborazione e' disponibile solo nella finestra di Kurva3D");
                return en.showCollaboration();
              },
              false});
}
}  // namespace cad
