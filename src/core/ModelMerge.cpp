#include "core/ModelMerge.h"
#include <map>
#include <memory>
#include <set>

namespace cad {
namespace {

using T = ON_ModelComponent::Type;

struct IdLess {
  bool operator()(const ON_UUID& a, const ON_UUID& b) const { return ON_UuidCompare(a, b) < 0; }
};

const ON_ModelComponent* ByName(const ONX_Model& m, T type, const ON_wString& name, const ON_UUID& parent = ON_nil_uuid) {
  ONX_ModelComponentIterator it(m, type);
  for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent()) {
    if (c->IsDeleted() || !ON_wString::EqualOrdinal(c->Name(), name, true)) continue;
    if (type == T::Layer) {
      const auto* l = ON_Layer::Cast(c);
      if (!l || ON_UuidCompare(l->ParentLayerId(), parent) != 0) continue;
    }
    return c;
  }
  return nullptr;
}

class Merger {
 public:
  Merger(ONX_Model& dst, const ONX_Model& src, const MergeOptions& o) : d(dst), s(src), opt(o) {}

  MergeResult Run() {
    ONX_ModelComponentIterator it(s, T::ModelGeometry);
    for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent()) {
      const auto* mg = ON_ModelGeometryComponent::Cast(c);
      const ON_Geometry* g = mg ? mg->Geometry(nullptr) : nullptr;
      const ON_3dmObjectAttributes* a = mg ? mg->Attributes(nullptr) : nullptr;
      if (!g || !a) continue;
      if (a->Mode() == ON::object_mode::idef_object) continue;  // fanno parte di un blocco
      if (opt.filter && !opt.filter(a->m_uuid)) continue;
      if (a->m_space == ON::page_space) { r.skipped++; continue; }  // oggetti delle tavole
      const ON_UUID id = CopyObject(*g, *a, false);
      if (ON_UuidIsNotNil(id)) r.objects.push_back(id);
      else r.skipped++;
    }
    return r;
  }

 private:
  // --- tabelle -------------------------------------------------------------------------
  int MapLinetype(int si) {
    if (si < 0) return si;
    if (auto f = lt.find(si); f != lt.end()) return f->second;
    const auto* src = ON_Linetype::Cast(s.ComponentFromIndex(T::LinePattern, si).ModelComponent());
    int di = -1;
    if (src) {
      if (const auto* ex = ByName(d, T::LinePattern, src->Name())) di = ex->Index();
      else {
        ON_ModelComponentReference ref = d.AddModelComponent(*src, true);
        if (ref.ModelComponent()) { di = ref.ModelComponent()->Index(); r.linetypes++; }
      }
    }
    return lt[si] = di;
  }

  void CopyEmbeddedFile(const ON_wString& path) {
    if (path.IsEmpty()) return;
    auto find = [&](const ONX_Model& m) -> const ON_EmbeddedFile* {
      ONX_ModelComponentIterator it(m, T::EmbeddedFile);
      for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent())
        if (const auto* ef = ON_EmbeddedFile::Cast(c); ef && ON_wString::EqualOrdinal(ef->Filename(), path, true)) return ef;
      return nullptr;
    };
    if (find(d)) return;
    if (const ON_EmbeddedFile* ef = find(s)) {
      if (d.AddModelComponent(*ef, true).ModelComponent()) r.embeddedFiles++;
    }
  }

  int MapMaterial(int si) {
    if (si < 0) return si;
    if (auto f = mat.find(si); f != mat.end()) return f->second;
    const auto* src = ON_Material::Cast(s.ComponentFromIndex(T::RenderMaterial, si).ModelComponent());
    int di = -1;
    if (src) {
      if (const auto* ex = ByName(d, T::RenderMaterial, src->Name())) di = ex->Index();
      else {
        ON_Material copy(*src);
        // render content di Rhino 8 collegato (con i figli: texture ecc.)
        if (src->RdkMaterialInstanceIdIsNotNil()) {
          const ON_UUID rid = src->RdkMaterialInstanceId();
          if (!d.ComponentFromId(T::RenderContent, rid).ModelComponent()) {
            if (const auto* rc = ON_RenderContent::Cast(s.ComponentFromId(T::RenderContent, rid).ModelComponent())) {
              ON_ModelComponentReference rr = d.AddModelComponent(*rc, true);
              if (const ON_ModelComponent* added = rr.ModelComponent()) {
                copy.SetRdkMaterialInstanceId(added->Id());
                r.renderContents++;
              }
            }
          }
        }
        for (int i = 0; i < copy.m_textures.Count(); i++) CopyEmbeddedFile(copy.m_textures[i].m_image_file_reference.FullPath());
        ON_ModelComponentReference ref = d.AddModelComponent(copy, true);
        if (ref.ModelComponent()) { di = ref.ModelComponent()->Index(); r.materials++; }
      }
    }
    return mat[si] = di;
  }

  int MapLayer(int si) {
    if (auto f = layer.find(si); f != layer.end()) return f->second;
    const auto* src = ON_Layer::Cast(s.ComponentFromIndex(T::Layer, si).ModelComponent());
    int di = -1;
    if (!src) {
      // livello mancante: livello corrente della destinazione
      const ON_ModelComponent* cur = d.ComponentFromId(T::Layer, d.m_settings.CurrentLayerId()).ModelComponent();
      di = cur ? cur->Index() : 0;
      return layer[si] = di;
    }
    ON_UUID parent = ON_nil_uuid;
    if (ON_UuidIsNotNil(src->ParentLayerId())) {
      const ON_ModelComponent* sp = s.ComponentFromId(T::Layer, src->ParentLayerId()).ModelComponent();
      if (sp) {
        const ON_ModelComponent* dp = d.ComponentFromIndex(T::Layer, MapLayer(sp->Index())).ModelComponent();
        if (dp) parent = dp->Id();
      }
    }
    if (const auto* ex = ByName(d, T::Layer, src->Name(), parent)) di = ex->Index();
    else {
      ON_Layer copy(*src);
      copy.SetParentLayerId(parent);
      copy.SetLinetypeIndex(MapLinetype(src->LinetypeIndex()));
      copy.SetRenderMaterialIndex(MapMaterial(src->RenderMaterialIndex()));
      ON_ModelComponentReference ref = d.AddModelComponent(copy, true);
      if (ref.ModelComponent()) { di = ref.ModelComponent()->Index(); r.layers++; }
      else di = 0;
    }
    return layer[si] = di;
  }

  int MapGroup(int si) {
    if (auto f = group.find(si); f != group.end()) return f->second;
    const auto* src = ON_Group::Cast(s.ComponentFromIndex(T::Group, si).ModelComponent());
    int di = -1;
    if (src) {
      // come Rhino: i gruppi importati restano gruppi separati da quelli gia' presenti
      ON_Group g;
      ON_wString name = src->Name();
      if (!name.IsEmpty() && ByName(d, T::Group, name)) {
        for (int k = 2;; k++) {
          ON_wString n2;
          n2.Format(L"%ls (%d)", static_cast<const wchar_t*>(src->Name()), k);
          if (!ByName(d, T::Group, n2)) { name = n2; break; }
        }
      }
      g.SetName(name);
      ON_ModelComponentReference ref = d.AddModelComponent(g, true);
      if (ref.ModelComponent()) { di = ref.ModelComponent()->Index(); r.groups++; }
    }
    return group[si] = di;
  }

  ON_UUID MapDimStyle(const ON_UUID& sid) {
    if (ON_UuidIsNil(sid)) return sid;
    if (auto f = dimstyle.find(sid); f != dimstyle.end()) return f->second;
    ON_UUID did = sid;
    const auto* src = ON_DimStyle::Cast(s.ComponentFromId(T::DimStyle, sid).ModelComponent());
    if (src && !src->IsSystemComponent()) {
      if (const auto* ex = ByName(d, T::DimStyle, src->Name())) did = ex->Id();
      else if (const auto* same = d.ComponentFromId(T::DimStyle, sid).ModelComponent()) did = same->Id();
      else {
        ON_ModelComponentReference ref = d.AddModelComponent(*src, true);
        if (ref.ModelComponent()) { did = ref.ModelComponent()->Id(); r.dimStyles++; }
      }
    }
    return dimstyle[sid] = did;
  }

  int MapHatchPattern(int si) {
    if (si < 0) return si;
    if (auto f = hatch.find(si); f != hatch.end()) return f->second;
    const auto* src = ON_HatchPattern::Cast(s.ComponentFromIndex(T::HatchPattern, si).ModelComponent());
    int di = si;
    if (src) {
      if (const auto* ex = ByName(d, T::HatchPattern, src->Name())) di = ex->Index();
      else {
        ON_ModelComponentReference ref = d.AddModelComponent(*src, true);
        if (ref.ModelComponent()) { di = ref.ModelComponent()->Index(); r.hatchPatterns++; }
      }
    }
    return hatch[si] = di;
  }

  void MapTextureMappings(ON_3dmObjectAttributes& a) {
    for (int i = 0; i < a.m_rendering_attributes.m_mappings.Count(); i++) {
      ON_MappingRef& mr = a.m_rendering_attributes.m_mappings[i];
      for (int j = 0; j < mr.m_mapping_channels.Count(); j++) {
        ON_MappingChannel& ch = mr.m_mapping_channels[j];
        if (ON_UuidIsNil(ch.m_mapping_id)) continue;
        if (auto f = mapping.find(ch.m_mapping_id); f != mapping.end()) { ch.m_mapping_id = f->second; continue; }
        ON_UUID did = ch.m_mapping_id;
        if (!d.ComponentFromId(T::TextureMapping, did).ModelComponent()) {
          if (const auto* tm = ON_TextureMapping::Cast(s.ComponentFromId(T::TextureMapping, did).ModelComponent())) {
            ON_ModelComponentReference ref = d.AddModelComponent(*tm, true);
            if (ref.ModelComponent()) did = ref.ModelComponent()->Id();
          }
        }
        mapping[ch.m_mapping_id] = did;
        ch.m_mapping_id = did;
      }
    }
  }

  ON_3dmObjectAttributes MapAttributes(const ON_3dmObjectAttributes& src) {
    ON_3dmObjectAttributes a = src;
    a.m_layer_index = MapLayer(src.m_layer_index);
    a.m_linetype_index = MapLinetype(src.m_linetype_index);
    a.m_material_index = MapMaterial(src.m_material_index);
    a.RemoveFromAllGroups();
    for (int i = 0; i < src.GroupCount(); i++)
      if (int g = MapGroup(src.GroupList()[i]); g >= 0) a.AddToGroup(g);
    MapTextureMappings(a);
    a.m_space = ON::model_space;
    a.m_viewport_id = ON_nil_uuid;
    return a;
  }

  // --- blocchi --------------------------------------------------------------------------
  ON_UUID MapBlock(const ON_UUID& sid) {
    if (auto f = block.find(sid); f != block.end()) return f->second;
    const auto* src = ON_InstanceDefinition::Cast(s.ComponentFromId(T::InstanceDefinition, sid).ModelComponent());
    if (!src) return block[sid] = ON_nil_uuid;
    if (const auto* ex = ByName(d, T::InstanceDefinition, src->Name())) {
      r.blocksReused++;
      return block[sid] = ex->Id();
    }
    block[sid] = ON_nil_uuid;  // protegge dai riferimenti circolari
    ON_SimpleArray<ON_UUID> members;
    const ON_SimpleArray<ON_UUID>& ids = src->InstanceGeometryIdList();
    for (int i = 0; i < ids.Count(); i++) {
      const auto* mg = ON_ModelGeometryComponent::Cast(s.ComponentFromId(T::ModelGeometry, ids[i]).ModelComponent());
      const ON_Geometry* g = mg ? mg->Geometry(nullptr) : nullptr;
      const ON_3dmObjectAttributes* a = mg ? mg->Attributes(nullptr) : nullptr;
      if (!g || !a) continue;
      const ON_UUID nid = CopyObject(*g, *a, true);
      if (ON_UuidIsNotNil(nid)) members.Append(nid);
    }
    ON_InstanceDefinition copy(*src);
    copy.SetInstanceGeometryIdList(members);
    ON_ModelComponentReference ref = d.AddModelComponent(copy, true);
    const ON_ModelComponent* added = ref.ModelComponent();
    if (!added) return ON_nil_uuid;
    r.blocks++;
    return block[sid] = added->Id();
  }

  ON_UUID CopyObject(const ON_Geometry& g, const ON_3dmObjectAttributes& srcAttrs, bool idefMember) {
    std::unique_ptr<ON_Geometry> copy(static_cast<ON_Geometry*>(g.Duplicate()));
    if (!copy) return ON_nil_uuid;
    if (auto* ir = ON_InstanceRef::Cast(copy.get())) {
      ir->m_instance_definition_uuid = MapBlock(ir->m_instance_definition_uuid);
      if (ON_UuidIsNil(ir->m_instance_definition_uuid)) return ON_nil_uuid;
    } else if (auto* an = ON_Annotation::Cast(copy.get())) {
      const ON_UUID ds = MapDimStyle(an->DimensionStyleId());
      if (ON_UuidCompare(ds, an->DimensionStyleId()) != 0) an->SetDimensionStyleIdForExperts(ds, true);
    } else if (auto* h = ON_Hatch::Cast(copy.get())) {
      h->SetPatternIndex(MapHatchPattern(h->PatternIndex()));
    }
    ON_3dmObjectAttributes a = MapAttributes(srcAttrs);
    if (idefMember || !opt.addObject) {
      auto* pa = new ON_3dmObjectAttributes(a);
      pa->m_uuid = ON_CreateId();
      if (idefMember) pa->SetMode(ON::object_mode::idef_object);
      ON_ModelComponentReference ref = d.AddManagedModelGeometryComponent(copy.release(), pa, true);
      const auto* mg = ON_ModelGeometryComponent::Cast(ref.ModelComponent());
      return mg && mg->Attributes(nullptr) ? mg->Attributes(nullptr)->m_uuid : ON_nil_uuid;
    }
    return opt.addObject(copy.release(), a);
  }

  ONX_Model& d;
  const ONX_Model& s;
  const MergeOptions& opt;
  MergeResult r;
  std::map<int, int> layer, lt, mat, group, hatch;
  std::map<ON_UUID, ON_UUID, IdLess> block, dimstyle, mapping;
};

}  // namespace

MergeResult MergeModel(ONX_Model& dst, const ONX_Model& src, const MergeOptions& options) {
  return Merger(dst, src, options).Run();
}

}  // namespace cad
