#include "commands/Exchange.h"
#include "commands/ExchangeUtil.h"
#include "commands/Engine.h"
#include "core/Document3dm.h"
#include "core/SubDLimit.h"
#include "commands/Materials.h"
#ifdef CAD_WITH_OCCT
#include "kernel/OcctConvert.h"
#endif
#include <algorithm>
#include <cctype>
#include <functional>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <sstream>
#include <tuple>

namespace cad {

using namespace xu;
namespace {
namespace fs = std::filesystem;

// Triangoli di una mesh (i quadrilateri divisi in due).
template <class F>
void ForEachTriangle(const ON_Mesh& m, F f) {
  for (int i = 0; i < m.m_F.Count(); i++) {
    const ON_MeshFace& fc = m.m_F[i];
    f(fc.vi[0], fc.vi[1], fc.vi[2]);
    if (fc.IsQuad()) f(fc.vi[0], fc.vi[2], fc.vi[3]);
  }
}
}  // namespace

std::string UnitNameIt(ON::LengthUnitSystem u) {
  using U = ON::LengthUnitSystem;
  switch (u) {
    case U::Millimeters: return "Millimetri";
    case U::Centimeters: return "Centimetri";
    case U::Decimeters: return "Decimetri";
    case U::Meters: return "Metri";
    case U::Kilometers: return "Chilometri";
    case U::Microns: return "Micron";
    case U::Nanometers: return "Nanometri";
    case U::Inches: return "Pollici";
    case U::Feet: return "Piedi";
    case U::Yards: return "Iarde";
    case U::Miles: return "Miglia";
    case U::Mils: return "Mil";
    case U::Microinches: return "Micropollici";
    case U::None: return "Senza unita'";
    default: return ToUtf8(ON_UnitSystem(u).UnitSystemName());
  }
}

std::string FileExtension(const std::string& path) {
  std::string e = U8(fs::u8path(path).extension());
  if (!e.empty() && e[0] == '.') e.erase(0, 1);
  return Lower(e);
}

bool IsExchangeFormat(const std::string& ext) {
  return ext == "stl" || ext == "obj" || ext == "dxf" || ext == "step" || ext == "stp" || ext == "iges" || ext == "igs" ||
         ext == "gltf" || ext == "glb" || ext == "3mf" || ext == "ply";
}

std::string ExchangeFormatsDescription() { return "3dm, STEP (.stp/.step), IGES (.igs/.iges), STL, OBJ, DXF, glTF (.gltf/.glb), 3MF, PLY"; }

std::unique_ptr<ON_Mesh> ExportMesh(const ON_Geometry& g, double tolerance) {
  if (const auto* m = ON_Mesh::Cast(&g)) return std::make_unique<ON_Mesh>(*m);
  if (const auto* s = ON_SubD::Cast(&g)) {
    SubDLimitDisplay lim = BuildSubDLimit(*s, 3);
    if (lim.mesh) return std::move(lim.mesh);
    return std::unique_ptr<ON_Mesh>(s->GetControlNetMesh(nullptr, ON_SubDGetControlNetMeshPriority::Geometry));
  }
#ifdef CAD_WITH_OCCT
  std::unique_ptr<ON_Brep> owned;
  const ON_Brep* b = ON_Brep::Cast(&g);
  if (!b) {
    if (const auto* x = ON_Extrusion::Cast(&g)) owned.reset(x->BrepForm());
    else if (const auto* sf = ON_Surface::Cast(&g)) owned.reset(sf->BrepForm());
    b = owned.get();
  }
  if (b) {
    ON_BoundingBox bb;
    b->GetTightBoundingBox(bb);
    const double defl = std::max(tolerance, bb.Diagonal().Length() / 2000.0);
    TopoDS_Shape shape = occt::BrepToShape(*b, tolerance);
    if (ON_Mesh* m = occt::MeshShape(shape, defl, 10.0 * ON_PI / 180.0, true)) return std::unique_ptr<ON_Mesh>(m);
  }
#else
  (void)tolerance;
#endif
  return nullptr;
}

// --- STL --------------------------------------------------------------------------------------
bool ReadSTL(const std::string& path, std::vector<ImportedObject>& out, std::string& err) {
  std::ifstream f(fs::u8path(path), std::ios::binary);
  if (!f) { err = "impossibile aprire " + path; return false; }
  std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  const std::string base = U8(fs::u8path(path).stem());
  bool ascii = data.size() >= 5 && Lower(data.substr(0, 5)) == "solid" &&
               data.find("facet", 0) != std::string::npos && data.find("facet") < 2048;
  if (!ascii && data.size() >= 84) {
    uint32_t n = 0;
    std::memcpy(&n, data.data() + 80, 4);
    if (84 + 50ull * n != data.size()) ascii = data.find("facet") != std::string::npos;
  }
  MeshBuilder mb;
  if (ascii) {
    std::istringstream in(data);
    std::string tok, name = base;
    std::vector<int> loop;
    while (in >> tok) {
      const std::string t = Lower(tok);
      if (t == "solid") {
        std::string rest;
        std::getline(in, rest);
        if (!Trim(rest).empty()) name = Trim(rest);
      } else if (t == "vertex") {
        double x, y, z;
        in >> x >> y >> z;
        loop.push_back(mb.Vertex(ON_3dPoint(x, y, z)));
      } else if (t == "endloop") {
        if (loop.size() >= 3) mb.Face(loop[0], loop[1], loop[2]);
        loop.clear();
      } else if (t == "endsolid") {
        if (!mb.Empty()) {
          ImportedObject o;
          o.geom = mb.Take();
          o.name = name;
          out.push_back(std::move(o));
        }
        name = base;
      }
    }
  } else {
    if (data.size() < 84) { err = "file STL troppo corto"; return false; }
    uint32_t n = 0;
    std::memcpy(&n, data.data() + 80, 4);
    if (84 + 50ull * n > data.size()) { err = "file STL binario incompleto"; return false; }
    for (uint32_t i = 0; i < n; i++) {
      const char* p = data.data() + 84 + 50ull * i + 12;
      int v[3];
      for (int k = 0; k < 3; k++) {
        float c[3];
        std::memcpy(c, p + 12 * k, 12);
        v[k] = mb.Vertex(ON_3dPoint(c[0], c[1], c[2]));
      }
      mb.Face(v[0], v[1], v[2]);
    }
  }
  if (!mb.Empty()) {
    ImportedObject o;
    o.geom = mb.Take();
    o.name = base;
    out.push_back(std::move(o));
  }
  if (out.empty()) { err = "nessun triangolo nel file STL"; return false; }
  return true;
}

bool WriteSTL(const std::string& path, const std::vector<ExportItem>& items, double tol, bool binary, std::string& err) {
  std::vector<std::unique_ptr<ON_Mesh>> meshes;
  size_t tris = 0;
  for (const auto& it : items)
    if (auto m = ExportMesh(*it.geom, tol)) {
      ForEachTriangle(*m, [&](int, int, int) { tris++; });
      meshes.push_back(std::move(m));
    }
  if (meshes.empty()) { err = "nessun oggetto con superfici o mesh da esportare in STL"; return false; }
  std::ofstream f(fs::u8path(path), std::ios::binary);
  if (!f) { err = "impossibile scrivere " + path; return false; }
  auto normal = [](const ON_3fPoint& a, const ON_3fPoint& b, const ON_3fPoint& c) {
    ON_3dVector n = ON_CrossProduct(ON_3dPoint(b) - ON_3dPoint(a), ON_3dPoint(c) - ON_3dPoint(a));
    n.Unitize();
    return n;
  };
  if (binary) {
    char header[80] = {};
    std::snprintf(header, sizeof header, "Kurva3D STL");
    f.write(header, 80);
    const uint32_t n = (uint32_t)tris;
    f.write(reinterpret_cast<const char*>(&n), 4);
    for (const auto& m : meshes)
      ForEachTriangle(*m, [&](int a, int b, int c) {
        const ON_3dVector nn = normal(m->m_V[a], m->m_V[b], m->m_V[c]);
        float buf[12] = {(float)nn.x, (float)nn.y, (float)nn.z};
        const int vi[3] = {a, b, c};
        for (int k = 0; k < 3; k++) {
          buf[3 + 3 * k] = m->m_V[vi[k]].x;
          buf[4 + 3 * k] = m->m_V[vi[k]].y;
          buf[5 + 3 * k] = m->m_V[vi[k]].z;
        }
        f.write(reinterpret_cast<const char*>(buf), 48);
        const uint16_t attr = 0;
        f.write(reinterpret_cast<const char*>(&attr), 2);
      });
  } else {
    f << "solid Kurva3D\n";
    f.precision(9);
    for (const auto& m : meshes)
      ForEachTriangle(*m, [&](int a, int b, int c) {
        const ON_3dVector nn = normal(m->m_V[a], m->m_V[b], m->m_V[c]);
        f << " facet normal " << nn.x << " " << nn.y << " " << nn.z << "\n  outer loop\n";
        for (int v : {a, b, c}) f << "   vertex " << m->m_V[v].x << " " << m->m_V[v].y << " " << m->m_V[v].z << "\n";
        f << "  endloop\n endfacet\n";
      });
    f << "endsolid Kurva3D\n";
  }
  if (!f) { err = "errore di scrittura"; return false; }
  return true;
}

// --- OBJ --------------------------------------------------------------------------------------
// Materiali di un file .mtl (nome -> materiale).
void ReadMTL(const fs::path& file, std::map<std::string, std::shared_ptr<ExchangeMaterial>>& mats) {
  std::ifstream f(file);
  if (!f) return;
  std::shared_ptr<ExchangeMaterial> cur;
  auto color = [](std::istringstream& in) {
    double r = 0, g = 0, b = 0;
    in >> r >> g >> b;
    auto c = [](double v) { return static_cast<int>(std::lround(std::clamp(v, 0.0, 1.0) * 255)); };
    return ON_Color(c(r), c(g), c(b));
  };
  for (std::string line; std::getline(f, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream in(line);
    std::string t;
    if (!(in >> t)) continue;
    if (t == "newmtl") {
      std::string rest;
      std::getline(in, rest);
      cur = std::make_shared<ExchangeMaterial>();
      cur->name = Trim(rest);
      mats[cur->name] = cur;
    } else if (!cur) continue;
    else if (t == "Kd") cur->diffuse = color(in);
    else if (t == "Ks") cur->specular = color(in);
    else if (t == "Ke") cur->emission = color(in);
    else if (t == "Ns") { double v = 0; in >> v; cur->shine = std::clamp(v / 1000.0, 0.0, 1.0); }
    else if (t == "d") { double v = 1; in >> v; cur->transparency = 1.0 - std::clamp(v, 0.0, 1.0); }
    else if (t == "Tr") { double v = 0; in >> v; cur->transparency = std::clamp(v, 0.0, 1.0); }
    else if (t == "Ni") { double v = 1; in >> v; cur->ior = v; }
    else if (t == "Pm") { double v = 0; in >> v; cur->metallic = v; cur->pbr = true; }
    else if (t == "Pr") { double v = 0.5; in >> v; cur->roughness = v; cur->pbr = true; }
    else if (t == "map_Kd") {
      std::string rest;
      std::getline(in, rest);
      rest = Trim(rest);
      // le opzioni (-s, -o, ...) precedono il nome del file: si prende l'ultima parola
      // se il nome intero non esiste
      fs::path p = file.parent_path() / fs::u8path(rest);
      std::error_code ec;
      if (!fs::is_regular_file(p, ec)) {
        const size_t sp = rest.find_last_of(' ');
        if (sp != std::string::npos) p = file.parent_path() / fs::u8path(rest.substr(sp + 1));
      }
      if (fs::u8path(rest).is_absolute() && fs::is_regular_file(fs::u8path(rest), ec)) p = fs::u8path(rest);
      cur->texture = U8(fs::absolute(p, ec).lexically_normal());
    }
  }
}

bool ReadOBJ(const std::string& path, std::vector<ImportedObject>& out, std::string& err) {
  std::ifstream f(fs::u8path(path));
  if (!f) { err = "impossibile aprire " + path; return false; }
  std::map<std::string, std::shared_ptr<ExchangeMaterial>> mats;
  std::shared_ptr<ExchangeMaterial> curMat;
  std::vector<ON_3dPoint> V;
  std::vector<ON_2dPoint> T;
  std::vector<ON_3dVector> N;
  std::string name = U8(fs::u8path(path).stem());
  // vertici della mesh corrente: (v, vt, vn) -> indice
  std::map<std::tuple<int, int, int>, int> index;
  auto mesh = std::make_unique<ON_Mesh>();
  bool hasT = true, hasN = true;
  std::vector<ON_Polyline> lines;
  auto flush = [&] {
    if (mesh->m_F.Count()) {
      if (!hasT || mesh->m_T.Count() != mesh->m_V.Count()) mesh->m_T.Destroy();
      if (!hasN || mesh->m_N.Count() != mesh->m_V.Count()) { mesh->m_N.Destroy(); mesh->ComputeVertexNormals(); }
      mesh->Compact();
      ImportedObject o;
      o.geom = std::move(mesh);
      o.name = name;
      o.material = curMat;
      out.push_back(std::move(o));
    }
    for (auto& pl : lines)
      if (pl.Count() >= 2) {
        ImportedObject o;
        o.geom = std::make_unique<ON_PolylineCurve>(pl);
        o.name = name;
        out.push_back(std::move(o));
      }
    lines.clear();
    mesh = std::make_unique<ON_Mesh>();
    index.clear();
    hasT = hasN = true;
  };
  auto resolve = [](int i, size_t n) { return i > 0 ? i - 1 : (int)n + i; };
  for (std::string line; std::getline(f, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream in(line);
    std::string t;
    if (!(in >> t)) continue;
    if (t == "v") { double x = 0, y = 0, z = 0; in >> x >> y >> z; V.emplace_back(x, y, z); }
    else if (t == "vt") { double u = 0, v = 0; in >> u >> v; T.emplace_back(u, v); }
    else if (t == "vn") { double x = 0, y = 0, z = 0; in >> x >> y >> z; N.emplace_back(x, y, z); }
    else if (t == "o" || t == "g") {
      flush();
      std::string rest;
      std::getline(in, rest);
      if (!Trim(rest).empty()) name = Trim(rest);
    } else if (t == "mtllib") {
      std::string rest;
      std::getline(in, rest);
      ReadMTL(fs::u8path(path).parent_path() / fs::u8path(Trim(rest)), mats);
    } else if (t == "usemtl") {
      std::string rest;
      std::getline(in, rest);
      auto mf = mats.find(Trim(rest));
      std::shared_ptr<ExchangeMaterial> m = mf == mats.end() ? nullptr : mf->second;
      if (m != curMat) { flush(); curMat = m; }  // un oggetto per materiale
    } else if (t == "l") {
      ON_Polyline pl;
      for (std::string w; in >> w;) {
        const int vi = resolve(std::atoi(w.c_str()), V.size());
        if (vi >= 0 && vi < (int)V.size()) pl.Append(V[vi]);
      }
      lines.push_back(pl);
    } else if (t == "f") {
      std::vector<int> face;
      for (std::string w; in >> w;) {
        int vi = 0, ti = 0, ni = 0;
        const size_t s1 = w.find('/');
        vi = std::atoi(w.substr(0, s1).c_str());
        if (s1 != std::string::npos) {
          const size_t s2 = w.find('/', s1 + 1);
          const std::string ts = w.substr(s1 + 1, s2 == std::string::npos ? std::string::npos : s2 - s1 - 1);
          if (!ts.empty()) ti = std::atoi(ts.c_str());
          if (s2 != std::string::npos) ni = std::atoi(w.substr(s2 + 1).c_str());
        }
        vi = resolve(vi, V.size());
        ti = ti ? resolve(ti, T.size()) : -1;
        ni = ni ? resolve(ni, N.size()) : -1;
        if (vi < 0 || vi >= (int)V.size()) { err = "indice di vertice non valido: " + w; return false; }
        if (ti >= (int)T.size()) ti = -1;
        if (ni >= (int)N.size()) ni = -1;
        if (ti < 0) hasT = false;
        if (ni < 0) hasN = false;
        auto key = std::make_tuple(vi, ti, ni);
        auto it = index.find(key);
        int idx;
        if (it != index.end()) idx = it->second;
        else {
          idx = mesh->m_V.Count();
          mesh->m_V.Append(ON_3fPoint(V[vi]));
          if (ti >= 0) mesh->m_T.Append(ON_2fPoint(T[ti]));
          if (ni >= 0) mesh->m_N.Append(ON_3fVector(N[ni]));
          index[key] = idx;
        }
        face.push_back(idx);
      }
      if (face.size() == 3) mesh->SetTriangle(mesh->m_F.Count(), face[0], face[1], face[2]);
      else if (face.size() == 4) mesh->SetQuad(mesh->m_F.Count(), face[0], face[1], face[2], face[3]);
      else
        for (size_t k = 1; k + 1 < face.size(); k++) mesh->SetTriangle(mesh->m_F.Count(), face[0], face[k], face[k + 1]);
    }
  }
  flush();
  if (out.empty()) { err = "nessuna geometria nel file OBJ"; return false; }
  return true;
}

// File .mtl accanto all'OBJ; le immagini delle texture vengono copiate nella stessa cartella.
std::map<const ExchangeMaterial*, std::string> WriteMTL(const fs::path& mtl, const std::vector<ExportItem>& items) {
  std::map<const ExchangeMaterial*, std::string> names;
  std::map<std::string, int> used;
  std::ostringstream o;
  o << "# Kurva3D\n";
  for (const auto& it : items) {
    const ExchangeMaterial* m = it.material.get();
    if (!m || names.count(m)) continue;
    std::string n = m->name.empty() ? "materiale" : m->name;
    for (auto& c : n) if (std::isspace((unsigned char)c)) c = '_';
    if (used[n]++) n += "_" + std::to_string(used[n]);
    names[m] = n;
    auto c3 = [](const ON_Color& c) {
      std::ostringstream s;
      s << c.FractionRed() << " " << c.FractionGreen() << " " << c.FractionBlue();
      return s.str();
    };
    o << "\nnewmtl " << n << "\nKa 0 0 0\nKd " << c3(m->diffuse) << "\nKs " << c3(m->specular)
      << "\nNs " << m->shine * 1000.0 << "\nd " << 1.0 - m->transparency << "\nNi " << m->ior << "\n";
    if (m->emission.Red() || m->emission.Green() || m->emission.Blue()) o << "Ke " << c3(m->emission) << "\n";
    if (m->pbr) o << "Pm " << m->metallic << "\nPr " << m->roughness << "\n";
    o << "illum " << (m->transparency > 0 ? 4 : 2) << "\n";
    if (!m->texture.empty()) {
      std::error_code ec;
      const fs::path src = fs::u8path(m->texture);
      const fs::path dst = mtl.parent_path() / src.filename();
      if (!fs::equivalent(src, dst, ec)) fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
      o << "map_Kd " << U8(src.filename()) << "\n";
    }
  }
  if (!names.empty()) {
    std::ofstream f(mtl);
    f << o.str();
  }
  return names;
}

bool WriteOBJ(const std::string& path, const std::vector<ExportItem>& items, double tol, std::string& err) {
  std::ofstream f(fs::u8path(path));
  if (!f) { err = "impossibile scrivere " + path; return false; }
  f << "# Kurva3D\n";
  const fs::path mtlPath = fs::u8path(path).replace_extension(".mtl");
  const auto mtlNames = WriteMTL(mtlPath, items);
  if (!mtlNames.empty()) f << "mtllib " << U8(mtlPath.filename()) << "\n";
  f.precision(10);
  size_t vb = 1, tb = 1, nb = 1;
  int n = 0;
  for (size_t i = 0; i < items.size(); i++) {
    const auto& it = items[i];
    std::string nm = it.name.empty() ? "oggetto_" + std::to_string(i + 1) : it.name;
    for (auto& c : nm) if (std::isspace((unsigned char)c)) c = '_';
    if (const auto* c = ON_Curve::Cast(it.geom)) {
      // curve come polilinee (elemento "l")
      ON_SimpleArray<ON_3dPoint> pts;
      ON_Polyline pl;
      if (c->IsPolyline(&pl) < 2) {
        const ON_Interval d = c->Domain();
        const int segs = std::max(16, 8 * std::max(1, c->SpanCount()) * std::max(1, c->Degree()));
        for (int k = 0; k <= segs; k++) pl.Append(c->PointAt(d.ParameterAt((double)k / segs)));
      }
      f << "o " << nm << "\n";
      for (int k = 0; k < pl.Count(); k++) f << "v " << pl[k].x << " " << pl[k].y << " " << pl[k].z << "\n";
      f << "l";
      for (int k = 0; k < pl.Count(); k++) f << " " << vb + k;
      f << "\n";
      vb += pl.Count();
      n++;
      continue;
    }
    auto m = ExportMeshFor(it, tol);
    if (!m || m->m_F.Count() == 0) continue;
    const int nv = m->m_V.Count();
    const bool hasT = m->m_T.Count() == nv, hasN = m->m_N.Count() == nv;
    f << "o " << nm << "\n";
    if (auto mn = mtlNames.find(it.material.get()); it.material && mn != mtlNames.end()) f << "usemtl " << mn->second << "\n";
    for (int k = 0; k < nv; k++) f << "v " << m->m_V[k].x << " " << m->m_V[k].y << " " << m->m_V[k].z << "\n";
    if (hasT) for (int k = 0; k < nv; k++) f << "vt " << m->m_T[k].x << " " << m->m_T[k].y << "\n";
    if (hasN) for (int k = 0; k < nv; k++) f << "vn " << m->m_N[k].x << " " << m->m_N[k].y << " " << m->m_N[k].z << "\n";
    for (int k = 0; k < m->m_F.Count(); k++) {
      const ON_MeshFace& fc = m->m_F[k];
      f << "f";
      for (int j = 0; j < (fc.IsQuad() ? 4 : 3); j++) {
        const int v = fc.vi[j];
        f << " " << vb + v;
        if (hasT || hasN) f << "/" << (hasT ? std::to_string(tb + v) : "");
        if (hasN) f << "/" << nb + v;
      }
      f << "\n";
    }
    vb += nv;
    if (hasT) tb += nv;
    if (hasN) nb += nv;
    n++;
  }
  if (!n) { err = "nessun oggetto esportabile in OBJ"; return false; }
  return (bool)f || (err = "errore di scrittura", false);
}

// --- Comandi ----------------------------------------------------------------------------------
namespace {
// Materiale del documento per un materiale importato (riusato se c'e' gia' con lo stesso nome).
int AddExchangeMaterial(ONX_Model& model, const ExchangeMaterial& em) {
  const ON_wString wname(em.name.c_str());
  if (!em.name.empty())
    if (const ON_ModelComponent* c = model.ComponentFromName(ON_ModelComponent::Type::RenderMaterial, ON_nil_uuid, wname).ModelComponent())
      return c->Index();
  ON_Material m;
  m.SetName(wname);
  m.SetDiffuse(em.diffuse);
  m.SetSpecular(em.specular);
  m.SetEmission(em.emission);
  m.SetShine(std::clamp(em.shine, 0.0, 1.0) * ON_Material::MaxShine);
  m.SetTransparency(std::clamp(em.transparency, 0.0, 1.0));
  m.m_index_of_refraction = em.ior;
  if (em.pbr) {
    m.ToPhysicallyBased();
    auto pb = m.PhysicallyBased();
    pb->SetBaseColor(ON_4fColor(em.diffuse));
    pb->SetMetallic(std::clamp(em.metallic, 0.0, 1.0));
    pb->SetRoughness(std::clamp(em.roughness, 0.0, 1.0));
    pb->SetOpacity(1.0 - std::clamp(em.transparency, 0.0, 1.0));
    pb->SetEmission(ON_4fColor(em.emission));
  }
  if (!em.texture.empty()) {
    if (!em.textureData.empty()) EmbedImageBuffer(model, em.texture, em.textureData);
    else {
      std::error_code ec;
      if (fs::is_regular_file(fs::u8path(em.texture), ec)) EmbedImageFile(model, em.texture);
    }
    ON_Texture tex;
    tex.m_image_file_reference.SetFullPath(ON_wString(em.texture.c_str()), false);
    tex.m_type = ON_Texture::TYPE::bitmap_texture;
    tex.m_mapping_channel_id = 1;  // coordinate di texture della mesh
    m.AddTexture(tex);
    if (!em.pbr) m.SetDiffuse(ON_Color(255, 255, 255));
  }
  ON_ModelComponentReference ref = model.AddModelComponent(m, true);
  return ref.ModelComponent() ? ref.ModelComponent()->Index() : -1;
}

// Materiale di rendering di un oggetto per i formati mesh (dall'oggetto o dal livello).
std::shared_ptr<ExchangeMaterial> ExportMaterialOf(const ONX_Model& model, const ON_3dmObjectAttributes& a,
                                                   std::map<int, std::shared_ptr<ExchangeMaterial>>& cache) {
  int mi = -1;
  if (a.MaterialSource() == ON::material_from_object) mi = a.m_material_index;
  else if (const auto* l = ON_Layer::Cast(model.LayerFromIndex(a.m_layer_index).ModelComponent())) mi = l->RenderMaterialIndex();
  if (mi < 0) return nullptr;
  if (auto f = cache.find(mi); f != cache.end()) return f->second;
  const auto* m = ON_Material::Cast(model.ComponentFromIndex(ON_ModelComponent::Type::RenderMaterial, mi).ModelComponent());
  if (!m) return cache[mi] = nullptr;
  auto em = std::make_shared<ExchangeMaterial>();
  em->name = ToUtf8(m->Name()).empty() ? "Materiale " + std::to_string(mi + 1) : ToUtf8(m->Name());
  em->diffuse = m->Diffuse();
  em->specular = m->Specular();
  em->emission = m->Emission();
  em->shine = m->Shine() / ON_Material::MaxShine;
  em->transparency = m->Transparency();
  em->ior = m->m_index_of_refraction;
  if (m->IsPhysicallyBased()) {
    auto pb = m->PhysicallyBased();
    em->pbr = true;
    em->diffuse = ON_Color(pb->BaseColor());
    em->metallic = pb->Metallic();
    em->roughness = pb->Roughness();
    em->transparency = 1.0 - pb->Opacity();
  }
  const int ti = m->FindTexture(nullptr, ON_Texture::TYPE::bitmap_texture);
  if (ti >= 0 && m->m_textures[ti].m_bOn) {
    std::string file = ToUtf8(m->m_textures[ti].m_image_file_reference.FullPath());
    std::error_code ec;
    const std::string cache2 = U8(fs::temp_directory_path(ec) / "kurva3d-texture");
    const std::string res = ResolveTextureFile(model, file, cache2);
    if (!res.empty()) em->texture = res;
  }
  return cache[mi] = em;
}

int LayerFor(Engine& e, const std::string& name, const ON_Color& color) {
  if (name.empty()) return e.Model().m_settings.CurrentLayerIndex();
  ONX_ModelComponentIterator it(e.Model(), ON_ModelComponent::Type::Layer);
  const ON_wString w(name.c_str());
  for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent())
    if (const auto* l = ON_Layer::Cast(c); l && ON_wString::EqualOrdinal(l->Name(), w, true)) return l->Index();
  const int i = e.Model().AddLayer(static_cast<const wchar_t*>(w), color == ON_UNSET_COLOR ? ON_Color::Black : color);
  return i >= 0 ? i : e.Model().m_settings.CurrentLayerIndex();
}

std::string KeyOf(const std::string& s) { return Upper(Trim(s)); }

// Tabelle e oggetti di un file importato aggiunti al documento.
class ModelBuilder {
 public:
  explicit ModelBuilder(Engine& e) : m_e(e), m_m(e.Model()) {}

  void Tables(const ImportedModel& im) {
    for (const auto& lt : im.linetypes) AddLinetype(lt);
    for (const auto& l : im.layers) AddLayer(l);
    for (const auto& hp : im.hatchPatterns) AddHatchPattern(hp);
    for (const auto& ds : im.dimStyles) {
      ON_ModelComponentReference ref = m_m.AddModelComponent(*ds, true);
      if (ref.ModelComponent() && !(ref.ModelComponent()->Id() == ds->Id())) m_dimRemap[ds->Id()] = ref.ModelComponent()->Id();
    }
    for (const auto& b : im.blocks) AddBlock(b);
  }

  // Oggetto del modello (o della definizione di un blocco con idef = true). Nil se non creato.
  ON_UUID Add(ImportedObject& o, bool idef, std::map<const ExchangeMaterial*, int>& matIndex) {
    ON_3dmObjectAttributes at;
    at.m_layer_index = LayerFor(m_e, o.layer, o.color);
    if (!o.name.empty()) at.SetName(ON_wString(o.name.c_str()), true);
    using S = ImportedObject::Source;
    if (o.colorSource == S::Object) { at.SetColorSource(ON::color_from_object); at.m_color = o.objectColor; }
    else if (o.colorSource == S::Parent) at.SetColorSource(ON::color_from_parent);
    if (o.linetypeSource == S::Object) {
      const int lt = LinetypeIndex(o.linetype);
      at.SetLinetypeSource(ON::linetype_from_object);
      at.m_linetype_index = lt;
    } else if (o.linetypeSource == S::Parent) at.SetLinetypeSource(ON::linetype_from_parent);
    if (o.plotWeightSource == S::Object) { at.SetPlotWeightSource(ON::plot_weight_from_object); at.m_plot_weight_mm = o.plotWeight; }
    else if (o.plotWeightSource == S::Parent) at.SetPlotWeightSource(ON::plot_weight_from_parent);
    if (o.hidden) at.SetVisible(false);
    for (const auto& g : o.groups) at.AddToGroup(GroupIndex(g));
    for (const auto& [k, v] : o.userText) at.SetUserString(ON_wString(k.c_str()), ON_wString(v.c_str()));
    if (o.material) {
      auto f = matIndex.find(o.material.get());
      const int mi = f != matIndex.end() ? f->second : (matIndex[o.material.get()] = AddExchangeMaterial(m_m, *o.material));
      if (mi >= 0) {
        at.m_material_index = mi;
        at.SetMaterialSource(ON::material_from_object);
      }
    }
    if (idef) at.SetMode(ON::object_mode::idef_object);
    if (o.pageSpace && !idef) {
      at.m_space = ON::page_space;
      at.m_viewport_id = PageId();
    }
    ON_Geometry* g = o.geom.release();
    if (!o.block.empty()) {
      auto f = m_blocks.find(KeyOf(o.block));
      if (f == m_blocks.end()) return ON_nil_uuid;
      auto* ir = new ON_InstanceRef();
      ir->m_instance_definition_uuid = f->second.id;
      ir->m_xform = o.xform;
      if (f->second.box.IsValid()) {
        ON_BoundingBox bb = f->second.box;
        bb.Transform(o.xform);
        ir->m_bbox = bb;
      }
      g = ir;
    }
    if (!g) return ON_nil_uuid;
    if (auto* an = ON_Annotation::Cast(g)) {
      auto f = m_dimRemap.find(an->DimensionStyleId());
      if (f != m_dimRemap.end()) an->SetDimensionStyleId(f->second);
      if (auto* d = ON_Dimension::Cast(an)) {
        const auto* ds = ON_DimStyle::Cast(m_m.DimensionStyleFromId(d->DimensionStyleId()).ModelComponent());
        const ON_DimStyle& parent = ds ? *ds : ON_DimStyle::Default;
        if (auto* ang = ON_DimAngular::Cast(d)) ang->UpdateDimensionText(&d->DimensionStyle(parent));
        else d->UpdateDimensionText(m_m.m_settings.m_ModelUnitsAndTolerances.m_unit_system.UnitSystem(), &d->DimensionStyle(parent));
      }
    }
    if (auto* h = ON_Hatch::Cast(g)) h->SetPatternIndex(HatchPatternIndex(o.hatchPattern));
    return m_e.AddObject(g, &at);
  }

  bool HasPage() const { return !ON_UuidIsNil(m_pageId); }
  void FinishPage(const ON_BoundingBox& bb) {
    if (!HasPage()) return;
    // foglio con gli oggetti dello spazio carta (minimo A4 orizzontale), origine in basso a sinistra
    double w = 297, h = 210;
    if (bb.IsValid()) { w = std::max(w, std::ceil(bb.m_max.x + 10)); h = std::max(h, std::ceil(bb.m_max.y + 10)); }
    for (int i = 0; i < m_m.m_settings.m_views.Count(); i++) {
      ON_3dmView& v = m_m.m_settings.m_views[i];
      if (!(v.m_vp.ViewportId() == m_pageId)) continue;
      v.m_page_settings.m_width_mm = w;
      v.m_page_settings.m_height_mm = h;
      v.m_vp.SetCameraLocation(ON_3dPoint(w / 2, h / 2, 100));
      v.m_vp.SetFrustum(-w / 2, w / 2, -h / 2, h / 2, 1, 1000);
    }
    if (m_e.layoutsChanged) m_e.layoutsChanged();
  }

 private:
  struct BlockInfo { ON_UUID id; ON_BoundingBox box; };
  Engine& m_e;
  ONX_Model& m_m;
  std::map<std::string, int> m_linetypes, m_hatchPatterns, m_groups;
  std::map<std::string, BlockInfo> m_blocks;
  std::map<ON_UUID, ON_UUID, UuidLess> m_dimRemap;
  ON_UUID m_pageId = ON_nil_uuid;

  ON_UUID PageId() {
    if (!ON_UuidIsNil(m_pageId)) return m_pageId;
    ON_3dmView v;
    v.m_view_type = ON::view_type::page_view_type;
    std::string name = "Spazio carta";
    v.m_name = ON_wString(name.c_str());
    v.m_page_settings.m_width_mm = 297;
    v.m_page_settings.m_height_mm = 210;
    int maxPage = -1;
    for (int i = 0; i < m_m.m_settings.m_views.Count(); i++)
      if (m_m.m_settings.m_views[i].m_view_type == ON::view_type::page_view_type)
        maxPage = std::max(maxPage, m_m.m_settings.m_views[i].m_page_settings.m_page_number);
    v.m_page_settings.m_page_number = maxPage + 1;
    ON_Viewport& vp = v.m_vp;
    vp.ChangeToParallelProjection(true);
    vp.SetCameraLocation(ON_3dPoint(297 / 2.0, 105, 100));
    vp.SetCameraDirection(-ON_3dVector::ZAxis);
    vp.SetCameraUp(ON_3dVector::YAxis);
    vp.SetFrustum(-297 / 2.0, 297 / 2.0, -105, 105, 1, 1000);
    ON_CreateUuid(m_pageId);
    vp.SetViewportId(m_pageId);
    v.m_display_mode_id = ON_StandardDisplayModeId::Wireframe;
    m_m.m_settings.m_views.Append(v);
    return m_pageId;
  }

  int FindComponent(ON_ModelComponent::Type type, const std::string& name) const {
    ONX_ModelComponentIterator it(m_m, type);
    const ON_wString w(name.c_str());
    for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent())
      if (!c->IsDeleted() && ON_wString::EqualOrdinal(c->Name(), w, true)) return c->Index();
    return ON_UNSET_INT_INDEX;
  }

  void AddLinetype(const ImportedLinetype& lt) {
    const std::string k = KeyOf(lt.name);
    if (m_linetypes.count(k)) return;
    int idx = FindComponent(ON_ModelComponent::Type::LinePattern, lt.name);
    if (idx == ON_UNSET_INT_INDEX) {
      ON_Linetype l;
      l.SetName(ON_wString(lt.name.c_str()));
      for (double d : lt.pattern) {
        ON_LinetypeSegment sg;
        sg.m_length = std::fabs(d);
        sg.m_seg_type = d < 0 ? ON_LinetypeSegment::eSegType::stSpace : ON_LinetypeSegment::eSegType::stLine;
        l.AppendSegment(sg);
      }
      // il motivo deve iniziare con un tratto e alternare tratto/spazio
      ON_ModelComponentReference ref = m_m.AddModelComponent(l, true);
      idx = ref.ModelComponent() ? ref.ModelComponent()->Index() : -1;
    }
    m_linetypes[k] = idx;
  }
  int LinetypeIndex(const std::string& name) {
    const std::string k = KeyOf(name);
    if (k.empty() || k == "CONTINUOUS") return -1;
    if (auto f = m_linetypes.find(k); f != m_linetypes.end()) return f->second;
    const int idx = FindComponent(ON_ModelComponent::Type::LinePattern, name);
    return m_linetypes[k] = (idx == ON_UNSET_INT_INDEX ? -1 : idx);
  }

  void AddLayer(const ImportedLayer& il) {
    const int idx = LayerFor(m_e, il.name, il.color);
    auto* l = const_cast<ON_Layer*>(ON_Layer::Cast(m_m.LayerFromIndex(idx).ModelComponent()));
    if (!l) return;
    l->SetColor(il.color);
    l->SetVisible(il.visible);
    l->SetLocked(il.locked);
    l->SetLinetypeIndex(LinetypeIndex(il.linetype));
    if (il.plotWeight > 0) l->SetPlotWeight(il.plotWeight);
  }

  void AddHatchPattern(const ImportedHatchPattern& hp) {
    const std::string k = KeyOf(hp.name);
    if (m_hatchPatterns.count(k)) return;
    int idx = FindComponent(ON_ModelComponent::Type::HatchPattern, hp.name);
    if (idx == ON_UNSET_INT_INDEX) {
      ON_HatchPattern p(hp.solid ? ON_HatchPattern::Solid : ON_HatchPattern::Hatch1);
      p.ClearId();
      p.ClearIndex();
      p.SetName(ON_wString(hp.name.c_str()));
      if (!hp.solid) {
        p.SetFillType(ON_HatchPattern::HatchFillType::Lines);
        ON_ClassArray<ON_HatchLine> lines;
        for (const auto& l : hp.lines) lines.Append(l);
        p.SetHatchLines(lines);
      }
      ON_ModelComponentReference ref = m_m.AddModelComponent(p, true);
      idx = ref.ModelComponent() ? ref.ModelComponent()->Index() : -1;
    }
    m_hatchPatterns[k] = idx;
  }
  int HatchPatternIndex(const std::string& name) {
    if (auto f = m_hatchPatterns.find(KeyOf(name)); f != m_hatchPatterns.end() && f->second >= 0) return f->second;
    ImportedHatchPattern solid;
    solid.name = "Solid";
    solid.solid = true;
    AddHatchPattern(solid);
    return m_hatchPatterns[KeyOf("Solid")];
  }

  int GroupIndex(const std::string& name) {
    const std::string k = KeyOf(name);
    if (auto f = m_groups.find(k); f != m_groups.end()) return f->second;
    ON_Group g;
    g.SetName(ON_wString(name.c_str()));
    ON_ModelComponentReference ref = m_m.AddModelComponent(g, true);
    return m_groups[k] = ref.ModelComponent() ? ref.ModelComponent()->Index() : -1;
  }

  void AddBlock(const ImportedBlock& b) {
    std::map<const ExchangeMaterial*, int> mats;
    ON_SimpleArray<ON_UUID> members;
    ON_BoundingBox box;
    for (const auto& src : b.objects) {
      // copia: gli oggetti del blocco restano nella definizione importata
      ImportedObject o;
      o.geom.reset(src.geom ? static_cast<ON_Geometry*>(src.geom->Duplicate()) : nullptr);
      o.layer = src.layer; o.name = src.name; o.color = src.color; o.material = src.material;
      o.colorSource = src.colorSource; o.objectColor = src.objectColor;
      o.linetypeSource = src.linetypeSource; o.linetype = src.linetype;
      o.plotWeightSource = src.plotWeightSource; o.plotWeight = src.plotWeight;
      o.hidden = src.hidden; o.hatchPattern = src.hatchPattern; o.block = src.block; o.xform = src.xform;
      o.userText = src.userText;
      const ON_UUID id = Add(o, true, mats);
      if (ON_UuidIsNil(id)) continue;
      members.Append(id);
      if (const auto* mg = m_e.Object(id))
        if (const ON_Geometry* g = mg->Geometry(nullptr)) { ON_BoundingBox t; if (g->GetTightBoundingBox(t)) box.Union(t); }
    }
    if (!members.Count()) return;
    auto* idef = new ON_InstanceDefinition();
    std::string name = b.name.empty() ? "Blocco" : b.name;
    for (int k = 2; FindComponent(ON_ModelComponent::Type::InstanceDefinition, name) != ON_UNSET_INT_INDEX; k++)
      name = b.name + " (" + std::to_string(k) + ")";
    idef->SetName(ON_wString(name.c_str()));
    if (!b.description.empty()) idef->SetDescription(ON_wString(b.description.c_str()));
    idef->SetInstanceGeometryIdList(members);
    if (box.IsValid()) idef->SetBoundingBox(box);
    ON_ModelComponentReference ref = m_m.AddManagedModelComponent(idef, true);
    if (!ref.ModelComponent()) return;
    m_blocks[KeyOf(b.name)] = {ref.ModelComponent()->Id(), box};
  }
};
}  // namespace

bool ImportExchange(Engine& e, const std::string& path, const std::map<std::string, std::string>& opts) {
  const bool open = opts.count("open") > 0;
  const std::string ext = FileExtension(path);
  ImportedModel im;
  std::string err;
  ON::LengthUnitSystem units = e.Model().m_settings.m_ModelUnitsAndTolerances.m_unit_system.UnitSystem();
  // Apri: le coordinate restano nelle unita' del file, che diventano quelle del documento
  const ON::LengthUnitSystem readUnits = open ? ON::LengthUnitSystem::None : units;
  bool ok = false;
  if (ext == "stl") ok = ReadSTL(path, im.objects, err);
  else if (ext == "obj") ok = ReadOBJ(path, im.objects, err);
  else if (ext == "dxf") ok = ReadDXF(path, readUnits, im, err);
  else if (ext == "step" || ext == "stp" || ext == "iges" || ext == "igs") ok = ReadStepIges(path, readUnits, e.Tolerance(), im, err);
  else if (ext == "gltf" || ext == "glb") ok = ReadGLTF(path, units, im.objects, err);
  else if (ext == "3mf") ok = Read3MF(path, units, im.objects, err);
  else if (ext == "ply") ok = ReadPLY(path, im.objects, err);
  else err = "formato non supportato: " + ext;
  if (!ok) return e.Fail(err);
  if (!im.note.empty()) err = im.note;
  if (open && im.units != ON::LengthUnitSystem::None && im.units != units) {
    e.Model().m_settings.m_ModelUnitsAndTolerances.m_unit_system = ON_UnitSystem(im.units);
    e.Model().m_settings.m_PageUnitsAndTolerances.m_unit_system = ON_UnitSystem(ON::LengthUnitSystem::Millimeters);
    e.Out() << "  Unita' del documento: " << UnitNameIt(im.units) << " (dal file)\n";
  }
  e.ClearSelection();
  ModelBuilder mb(e);
  mb.Tables(im);
  int n = 0;
  std::map<const ExchangeMaterial*, int> matIndex;
  ON_BoundingBox pageBox;
  for (auto& o : im.objects) {
    const bool page = o.pageSpace;
    const ON_UUID id = mb.Add(o, false, matIndex);
    if (ON_UuidIsNil(id)) continue;
    n++;
    if (page) {
      if (const auto* mg = e.Object(id))
        if (const ON_Geometry* g = mg->Geometry(nullptr)) { ON_BoundingBox t; if (g->GetTightBoundingBox(t)) pageBox.Union(t); }
    } else e.Select(id);
  }
  mb.FinishPage(pageBox);
  e.Out() << "  Importati " << n << " oggetti da " << U8(fs::u8path(path).filename());
  if (!im.blocks.empty()) e.Out() << " (" << im.blocks.size() << " definizioni di blocco)";
  e.Out() << "\n";
  if (!err.empty()) e.Out() << "  Nota: " << err << "\n";
  return n > 0 || e.Fail("nessun oggetto importato");
}

bool ExportExchange(Engine& e, const std::vector<ON_UUID>& ids, const std::string& path,
                    const std::map<std::string, std::string>& opts) {
  const std::string ext = FileExtension(path);
  std::vector<ExportItem> items;
  std::map<int, std::shared_ptr<ExchangeMaterial>> materials;
  for (const auto& id : ids) {
    const auto* o = e.Object(id);
    if (!o || !o->Geometry(nullptr)) continue;
    ExportItem it;
    it.geom = o->Geometry(nullptr);
    if (const auto* a = o->Attributes(nullptr)) {
      it.name = ToUtf8(a->Name());
      if (const auto* l = ON_Layer::Cast(e.Model().LayerFromIndex(a->m_layer_index).ModelComponent())) {
        it.layer = ToUtf8(l->Name());
        it.color = a->ColorSource() == ON::color_from_object ? a->m_color : l->Color();
      }
      it.material = ExportMaterialOf(e.Model(), *a, materials);
      ObjectTexture tex;
      if (GetObjectTexture(e.Model(), *a, tex) && tex.hasMapping) {
        it.mapping = tex.mapping;
        it.hasMapping = true;
      }
    }
    it.attributes = o->Attributes(nullptr);
    items.push_back(it);
  }
  std::string err;
  const ON::LengthUnitSystem units = e.Model().m_settings.m_ModelUnitsAndTolerances.m_unit_system.UnitSystem();
  bool ok = false;
  if (ext == "stl") {
    auto it = opts.find("binary");
    const bool binary = !opts.count("ascii") && (it == opts.end() || it->second != "no");
    ok = WriteSTL(path, items, e.Tolerance(), binary, err);
  } else if (ext == "obj") ok = WriteOBJ(path, items, e.Tolerance(), err);
  else if (ext == "dxf") ok = WriteDXF(path, items, units, e.Tolerance(), err, &e.Model());
  else if (ext == "step" || ext == "stp" || ext == "iges" || ext == "igs") ok = WriteStepIges(path, items, units, e.Tolerance(), err);
  else if (ext == "gltf" || ext == "glb") ok = WriteGLTF(path, items, units, e.Tolerance(), ext == "glb", err);
  else if (ext == "3mf") ok = Write3MF(path, items, units, e.Tolerance(), err);
  else if (ext == "ply") {
    auto it = opts.find("binary");
    const bool binary = !opts.count("ascii") && (it == opts.end() || it->second != "no");
    ok = WritePLY(path, items, e.Tolerance(), binary, err);
  } else err = "formato non supportato: " + ext;
  if (!ok) return e.Fail(err);
  e.Out() << "  Esportati " << items.size() << " oggetti in " << U8(fs::u8path(path).filename()) << "\n";
  if (!err.empty()) e.Out() << "  Nota: " << err << "\n";
  return true;
}

namespace {
bool ViewCapture(Engine& e, TokenStream& ts, bool toFile) {
  auto opts = e.ReadOptions(ts);
  std::string path;
  if (toFile) {
    if (opts.count("file")) path = opts["file"];
    else {
      auto p = e.ReadString(ts, "file dell'immagine (.png, .jpg, .bmp)");
      if (!p) return false;
      path = *p;
    }
    if (path.size() >= 2 && path.front() == '"' && path.back() == '"') path = path.substr(1, path.size() - 2);
    opts.merge(e.ReadOptions(ts));
  }
  if (!e.viewCapture) return e.Fail("le immagini delle viste si salvano solo dalla finestra di Kurva3D");
  int w = 0, h = 0;
  try {
    if (opts.count("width")) w = std::stoi(opts["width"]);
    if (opts.count("height")) h = std::stoi(opts["height"]);
  } catch (...) { return e.Fail("larghezza o altezza non valida"); }
  const bool grid = !opts.count("drawgrid") || opts["drawgrid"] != "no";
  if (!e.viewCapture(path, std::max(0, w), std::max(0, h), grid)) return e.Fail("immagine della vista non salvata");
  e.Out() << (toFile ? "  Vista salvata in " + path + "\n" : std::string("  Vista copiata negli appunti\n"));
  return true;
}
}  // namespace

void RegisterExchangeCommands(Engine& e) {
  auto reg = [&](const char* n, const char* d, CommandFn f) {
    CommandInfo i{n, "File", d, f};
    i.modifiesModel = false;
    e.Register(i);
  };
  reg("ViewCaptureToFile", "Salva la vista attiva come immagine (Width=, Height=, DrawGrid=)",
      [](Engine& e, TokenStream& ts) { return ViewCapture(e, ts, true); });
  reg("ViewCaptureToClipboard", "Copia la vista attiva negli appunti come immagine",
      [](Engine& e, TokenStream& ts) { return ViewCapture(e, ts, false); });
}

#ifndef CAD_WITH_OCCT_DE
bool ReadStepIges(const std::string&, ON::LengthUnitSystem, double, ImportedModel&, std::string& err) {
  err = "STEP e IGES richiedono il modulo di scambio dati di OpenCASCADE, non presente in questa build";
  return false;
}
bool WriteStepIges(const std::string&, const std::vector<ExportItem>&, ON::LengthUnitSystem, double, std::string& err) {
  err = "STEP e IGES richiedono il modulo di scambio dati di OpenCASCADE, non presente in questa build";
  return false;
}
#endif

}  // namespace cad
