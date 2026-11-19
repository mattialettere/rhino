#include "commands/Exchange.h"
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

namespace {
namespace fs = std::filesystem;

std::string U8(const fs::path& p) {
  const auto u = p.u8string();
  return std::string(u.begin(), u.end());
}
std::string Lower(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}
std::string Trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace((unsigned char)s[a])) a++;
  while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
  return s.substr(a, b - a);
}

// Triangoli di una mesh (i quadrilateri divisi in due).
template <class F>
void ForEachTriangle(const ON_Mesh& m, F f) {
  for (int i = 0; i < m.m_F.Count(); i++) {
    const ON_MeshFace& fc = m.m_F[i];
    f(fc.vi[0], fc.vi[1], fc.vi[2]);
    if (fc.IsQuad()) f(fc.vi[0], fc.vi[2], fc.vi[3]);
  }
}

// Mesh con i vertici saldati per posizione esatta.
class MeshBuilder {
 public:
  int Vertex(const ON_3dPoint& p) {
    auto key = std::make_tuple(p.x, p.y, p.z);
    auto it = m_index.find(key);
    if (it != m_index.end()) return it->second;
    const int i = m_mesh->m_V.Count();
    m_mesh->m_V.Append(ON_3fPoint(p));
    m_index[key] = i;
    return i;
  }
  void Face(int a, int b, int c, int d = -1) {
    if (a == b || b == c || a == c) return;
    if (d < 0 || d == c || d == a) m_mesh->SetTriangle(m_mesh->m_F.Count(), a, b, c);
    else m_mesh->SetQuad(m_mesh->m_F.Count(), a, b, c, d);
  }
  bool Empty() const { return m_mesh->m_F.Count() == 0; }
  std::unique_ptr<ON_Mesh> Take() {
    auto m = std::move(m_mesh);
    m_mesh = std::make_unique<ON_Mesh>();
    m_index.clear();
    m->ComputeVertexNormals();
    m->Compact();
    return m;
  }

 private:
  std::unique_ptr<ON_Mesh> m_mesh = std::make_unique<ON_Mesh>();
  std::map<std::tuple<double, double, double>, int> m_index;
};

// --- Colori ACI (AutoCAD Color Index) ---------------------------------------------------------
ON_Color AciColor(int aci) {
  static const ON_Color base[10] = {ON_Color(0, 0, 0),       ON_Color(255, 0, 0),   ON_Color(255, 255, 0),
                                    ON_Color(0, 255, 0),     ON_Color(0, 255, 255), ON_Color(0, 0, 255),
                                    ON_Color(255, 0, 255),   ON_Color(0, 0, 0),     ON_Color(128, 128, 128),
                                    ON_Color(192, 192, 192)};
  aci = std::abs(aci);
  if (aci < 10) return base[aci];
  if (aci >= 250) {
    const int g = 51 + (aci - 250) * 41;
    return ON_Color(g, g, g);
  }
  const double hue = ((aci / 10) - 1) * 15.0;
  const int k = aci % 10;
  static const double val[5] = {1.0, 0.8, 0.6, 0.5, 0.3};
  const double v = val[k / 2], s = (k % 2) ? 0.5 : 1.0;
  ON_Color c;
  c.SetHSV(hue * ON_PI / 180.0, s, v);
  return c;
}

int NearestAci(const ON_Color& c) {
  if (c.Red() == c.Green() && c.Green() == c.Blue() && (c.Red() < 30 || c.Red() > 225)) return 7;
  int best = 7;
  double bd = 1e18;
  for (int i = 1; i < 256; i++) {
    if (i == 7) continue;
    const ON_Color a = AciColor(i);
    const double d = std::pow(a.Red() - c.Red(), 2) + std::pow(a.Green() - c.Green(), 2) + std::pow(a.Blue() - c.Blue(), 2);
    if (d < bd) { bd = d; best = i; }
  }
  return best;
}

// Algoritmo dell'asse arbitrario di AutoCAD (sistema OCS dalla direzione di estrusione).
ON_Plane OcsPlane(const ON_3dVector& nIn) {
  ON_3dVector n = nIn;
  if (!n.Unitize()) n = ON_3dVector::ZAxis;
  ON_3dVector ax = (std::fabs(n.x) < 1.0 / 64 && std::fabs(n.y) < 1.0 / 64) ? ON_CrossProduct(ON_3dVector::YAxis, n)
                                                                          : ON_CrossProduct(ON_3dVector::ZAxis, n);
  ax.Unitize();
  ON_3dVector ay = ON_CrossProduct(n, ax);
  ay.Unitize();
  return ON_Plane(ON_3dPoint::Origin, ax, ay);
}

ON_3dPoint OcsToWcs(const ON_Plane& p, const ON_3dPoint& q) {
  return ON_3dPoint::Origin + q.x * p.xaxis + q.y * p.yaxis + q.z * p.zaxis;
}
}  // namespace

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

// --- DXF --------------------------------------------------------------------------------------
namespace {
struct DxfPair { int code; std::string value; };

bool ReadPairs(const std::string& path, std::vector<DxfPair>& pairs, std::string& err) {
  std::ifstream f(fs::u8path(path), std::ios::binary);
  if (!f) { err = "impossibile aprire " + path; return false; }
  std::string head(22, '\0');
  f.read(head.data(), 22);
  if (head.rfind("AutoCAD Binary DXF", 0) == 0) { err = "DXF binario non supportato (salvalo come DXF di testo)"; return false; }
  f.seekg(0);
  std::string a, b;
  while (std::getline(f, a) && std::getline(f, b)) {
    if (!b.empty() && b.back() == '\r') b.pop_back();
    try { pairs.push_back({std::stoi(Trim(a)), b}); } catch (...) { err = "file DXF non valido"; return false; }
  }
  return true;
}

double Num(const std::string& s) { try { return std::stod(Trim(s)); } catch (...) { return 0; } }
int Int(const std::string& s) { try { return std::stoi(Trim(s)); } catch (...) { return 0; } }

// Un'entita' DXF: tutte le coppie fino al prossimo codice 0.
struct Entity {
  std::string type;
  std::vector<DxfPair> p;
  double D(int code, double def = 0) const {
    for (const auto& q : p) if (q.code == code) return Num(q.value);
    return def;
  }
  int I(int code, int def = 0) const {
    for (const auto& q : p) if (q.code == code) return Int(q.value);
    return def;
  }
  std::string S(int code) const {
    for (const auto& q : p) if (q.code == code) return q.value;
    return "";
  }
  std::vector<double> All(int code) const {
    std::vector<double> v;
    for (const auto& q : p) if (q.code == code) v.push_back(Num(q.value));
    return v;
  }
  ON_3dVector Extrusion() const { return ON_3dVector(D(210, 0), D(220, 0), D(230, 1)); }
};

ON::LengthUnitSystem InsUnits(int v) {
  switch (v) {
    case 1: return ON::LengthUnitSystem::Inches;
    case 2: return ON::LengthUnitSystem::Feet;
    case 4: return ON::LengthUnitSystem::Millimeters;
    case 5: return ON::LengthUnitSystem::Centimeters;
    case 6: return ON::LengthUnitSystem::Meters;
    case 7: return ON::LengthUnitSystem::Kilometers;
    case 8: return ON::LengthUnitSystem::Microinches;
    case 9: return ON::LengthUnitSystem::Mils;
    case 10: return ON::LengthUnitSystem::Yards;
    case 13: return ON::LengthUnitSystem::Microns;
    case 14: return ON::LengthUnitSystem::Decimeters;
    default: return ON::LengthUnitSystem::None;
  }
}

int InsUnitsCode(ON::LengthUnitSystem u) {
  for (int v = 1; v <= 14; v++) if (InsUnits(v) == u) return v;
  return 0;
}

// Segmento di polilinea con rigonfiamento (bulge) -> arco.
ON_Curve* BulgeSegment(const ON_3dPoint& a, const ON_3dPoint& b, double bulge, const ON_Plane& ocs) {
  const ON_3dPoint A = OcsToWcs(ocs, a), B = OcsToWcs(ocs, b);
  if (std::fabs(bulge) < 1e-12 || a.DistanceTo(b) < 1e-12) return new ON_LineCurve(A, B);
  // punto medio dell'arco nel piano OCS
  const ON_2dVector chord(b.x - a.x, b.y - a.y);
  const ON_2dVector perp(chord.y, -chord.x);  // a destra della corda
  const double sagitta = bulge * chord.Length() / 2.0;
  ON_2dVector pu = perp;
  pu.Unitize();
  const ON_3dPoint mid((a.x + b.x) / 2 + pu.x * sagitta, (a.y + b.y) / 2 + pu.y * sagitta, a.z);
  ON_Arc arc;
  if (!arc.Create(A, OcsToWcs(ocs, mid), B)) return new ON_LineCurve(A, B);
  return new ON_ArcCurve(arc);
}

ON_Curve* JoinSegments(std::vector<ON_Curve*>& segs) {
  if (segs.empty()) return nullptr;
  if (segs.size() == 1) { ON_Curve* c = segs[0]; segs.clear(); return c; }
  auto* pc = new ON_PolyCurve();
  for (ON_Curve* c : segs) pc->Append(c);
  segs.clear();
  return pc;
}
}  // namespace

// Da coordinate OCS a coordinate del mondo.
static ON_Xform OcsXform(const ON_Plane& ocs) {
  ON_Xform x(ON_Xform::IdentityTransformation);
  for (int r = 0; r < 3; r++) { x.m_xform[r][0] = ocs.xaxis[r]; x.m_xform[r][1] = ocs.yaxis[r]; x.m_xform[r][2] = ocs.zaxis[r]; }
  return x;
}

// Testo MTEXT/TEXT senza i codici di formattazione (\P a capo, {\fArial;...}, %%d gradi...).
static std::string DxfPlainText(const std::string& in) {
  std::string o;
  for (size_t i = 0; i < in.size(); i++) {
    const char c = in[i];
    if (c == '\\' && i + 1 < in.size()) {
      const char k = in[i + 1];
      if (k == 'P' || k == 'n') { o += '\n'; i++; }
      else if (k == '~') { o += ' '; i++; }
      else if (k == '\\' || k == '{' || k == '}') { o += k; i++; }
      else if (std::strchr("LlOoKk", k)) { i++; }
      else if (k == 'S') {  // frazioni impilate \S1^2; -> 1/2
        size_t e = in.find(';', i + 2);
        if (e == std::string::npos) e = in.size();
        std::string f = in.substr(i + 2, e - i - 2);
        for (auto& ch : f) if (ch == '^' || ch == '#') ch = '/';
        o += f;
        i = e;
      } else if (std::strchr("fFHCcAWQTp", k)) {  // codici con parametro fino a ';'
        const size_t e = in.find(';', i + 2);
        i = e == std::string::npos ? in.size() : e;
      } else { o += c; }
    } else if (c == '{' || c == '}') {
    } else if (c == '%' && i + 2 < in.size() && in[i + 1] == '%') {
      const char k = (char)std::tolower((unsigned char)in[i + 2]);
      if (k == 'd') o += "\xC2\xB0";        // gradi
      else if (k == 'p') o += "\xC2\xB1";   // piu' o meno
      else if (k == 'c') o += "\xC3\x98";   // diametro
      else if (k == '%') o += '%';
      else if (k == 'u' || k == 'o') {}    // sottolineato/sopralineato: ignorati
      else { o += in.substr(i, 3); }
      i += 2;
    } else {
      o += c;
    }
  }
  return o;
}

bool ReadDXF(const std::string& path, ON::LengthUnitSystem docUnits, std::vector<ImportedObject>& out, std::string& err) {
  std::vector<DxfPair> pairs;
  if (!ReadPairs(path, pairs, err)) return false;
  // sezioni
  std::map<std::string, ON_Color> layerColors;
  ON::LengthUnitSystem fileUnits = ON::LengthUnitSystem::None;
  std::vector<Entity> ents;
  struct Block { ON_3dPoint base; std::vector<Entity> ents; };
  std::map<std::string, Block> blocks;
  Block* curBlock = nullptr;
  std::string section;
  for (size_t i = 0; i < pairs.size(); i++) {
    const auto& q = pairs[i];
    if (q.code == 0 && q.value == "SECTION" && i + 1 < pairs.size()) { section = pairs[i + 1].value; i++; continue; }
    if (q.code == 0 && q.value == "ENDSEC") { section.clear(); curBlock = nullptr; continue; }
    if (section == "HEADER" && q.code == 9 && q.value == "$INSUNITS" && i + 1 < pairs.size()) fileUnits = InsUnits(Int(pairs[i + 1].value));
    if ((section == "TABLES" || section == "ENTITIES" || section == "BLOCKS") && q.code == 0) {
      Entity e;
      e.type = q.value;
      size_t j = i + 1;
      for (; j < pairs.size() && pairs[j].code != 0; j++) e.p.push_back(pairs[j]);
      i = j - 1;
      if (section == "TABLES" && e.type == "LAYER") layerColors[e.S(2)] = AciColor(e.I(62, 7));
      else if (section == "ENTITIES") ents.push_back(std::move(e));
      else if (section == "BLOCKS") {
        if (e.type == "BLOCK") {
          curBlock = &blocks[e.S(2)];
          curBlock->ents.clear();
          curBlock->base = ON_3dPoint(e.D(10), e.D(20), e.D(30));
        } else if (e.type == "ENDBLK") {
          curBlock = nullptr;
        } else if (curBlock) {
          curBlock->ents.push_back(std::move(e));
        }
      }
    }
  }
  const double scale = (fileUnits != ON::LengthUnitSystem::None && docUnits != ON::LengthUnitSystem::None)
                           ? ON::UnitScale(fileUnits, docUnits) : 1.0;
  std::map<std::string, MeshBuilder> faces;  // 3DFACE per livello
  int skipped = 0;
  // Entita' di un blocco (INSERT) o di una quota (DIMENSION): trasformate ed esplose nel modello.
  std::function<void(const std::vector<Entity>&, const ON_Xform&, const std::string&, int)> convert;
  convert = [&](const std::vector<Entity>& list, const ON_Xform& xf, const std::string& parentLayer, int depth) {
    // livello "0" dentro un blocco = livello dell'inserimento (regola DXF)
    auto layerOf = [&](const Entity& e) {
      std::string l = e.S(8);
      if (l.empty() || (l == "0" && !parentLayer.empty())) l = parentLayer.empty() ? "0" : parentLayer;
      return l;
    };
    auto add = [&](ON_Geometry* g, const Entity& e) {
      if (!g) return;
      if (!xf.IsIdentity()) g->Transform(xf);
      ImportedObject o;
      o.geom.reset(g);
      o.layer = layerOf(e);
      auto lc = layerColors.find(o.layer);
      o.color = lc != layerColors.end() ? lc->second : ON_Color::Black;
      out.push_back(std::move(o));
    };
    auto P = [](const Entity& e, int c) { return ON_3dPoint(e.D(c), e.D(c + 10), e.D(c + 20)); };
    for (size_t i = 0; i < list.size(); i++) {
      const Entity& e = list[i];
      const ON_Plane ocs = OcsPlane(e.Extrusion());
      if (e.type == "LINE") add(new ON_LineCurve(P(e, 10), P(e, 11)), e);
      else if (e.type == "POINT") add(new ON_Point(P(e, 10)), e);
      else if (e.type == "CIRCLE" || e.type == "ARC") {
        ON_Plane pl = ocs;
        pl.origin = OcsToWcs(ocs, P(e, 10));
        pl.UpdateEquation();
        ON_Circle circ(pl, e.D(40));
        if (e.type == "CIRCLE") add(new ON_ArcCurve(circ), e);
        else {
          double a0 = e.D(50) * ON_PI / 180, a1 = e.D(51) * ON_PI / 180;
          while (a1 <= a0) a1 += 2 * ON_PI;
          add(new ON_ArcCurve(ON_Arc(circ, ON_Interval(a0, a1))), e);
        }
      } else if (e.type == "ELLIPSE") {
        const ON_3dPoint c = P(e, 10);
        const ON_3dVector major(e.D(11), e.D(21), e.D(31));
        ON_3dVector n = e.Extrusion();
        n.Unitize();
        ON_3dVector minor = ON_CrossProduct(n, major);
        minor.Unitize();
        minor *= major.Length() * e.D(40, 1);
        ON_NurbsCurve nc;
        ON_Ellipse el(ON_Plane(c, major, minor), major.Length(), minor.Length());
        el.GetNurbForm(nc);
        double t0 = e.D(41, 0), t1 = e.D(42, 2 * ON_PI);
        auto* crv = new ON_NurbsCurve(nc);
        if (std::fabs(t1 - t0 - 2 * ON_PI) > 1e-9) {
          while (t1 <= t0) t1 += 2 * ON_PI;
          // dominio di GetNurbForm: 0..2pi
          if (t1 > 2 * ON_PI + 1e-12) {
            ON_Curve* a = crv->DuplicateCurve();
            a->Trim(ON_Interval(t0, 2 * ON_PI));
            ON_Curve* b = crv->DuplicateCurve();
            b->Trim(ON_Interval(0, t1 - 2 * ON_PI));
            delete crv;
            std::vector<ON_Curve*> segs{a, b};
            add(JoinSegments(segs), e);
            continue;
          }
          crv->Trim(ON_Interval(t0, t1));
        }
        add(crv, e);
      } else if (e.type == "LWPOLYLINE") {
        std::vector<ON_3dPoint> pts;
        std::vector<double> bulges;
        const double elev = e.D(38);
        double x = 0;
        bool haveX = false;
        for (const auto& q : e.p) {
          if (q.code == 10) { x = Num(q.value); haveX = true; }
          else if (q.code == 20 && haveX) { pts.emplace_back(x, Num(q.value), elev); bulges.push_back(0); haveX = false; }
          else if (q.code == 42 && !bulges.empty()) bulges.back() = Num(q.value);
        }
        const bool closed = e.I(70) & 1;
        std::vector<ON_Curve*> segs;
        for (size_t k = 0; k + 1 < pts.size() + (closed ? 1 : 0); k++)
          segs.push_back(BulgeSegment(pts[k], pts[(k + 1) % pts.size()], bulges[k], ocs));
        add(JoinSegments(segs), e);
      } else if (e.type == "POLYLINE") {
        const int flags = e.I(70);
        std::vector<Entity> verts;
        size_t j = i + 1;
        for (; j < list.size() && list[j].type == "VERTEX"; j++) verts.push_back(list[j]);
        i = (j < list.size() && list[j].type == "SEQEND") ? j : j - 1;
        if (flags & 64) {  // polyface mesh
          MeshBuilder mb;
          std::vector<int> idx;
          for (const auto& v : verts)
            if (v.I(70) & 64) idx.push_back(mb.Vertex((xf * P(v, 10)) * scale));
          for (const auto& v : verts)
            if ((v.I(70) & 128) && !(v.I(70) & 64)) {
              int f[4] = {std::abs(v.I(71)) - 1, std::abs(v.I(72)) - 1, std::abs(v.I(73)) - 1, std::abs(v.I(74)) - 1};
              auto ok = [&](int k) { return k >= 0 && k < (int)idx.size(); };
              if (ok(f[0]) && ok(f[1]) && ok(f[2])) mb.Face(idx[f[0]], idx[f[1]], idx[f[2]], ok(f[3]) ? idx[f[3]] : -1);
            }
          if (!mb.Empty()) {
            ImportedObject o;
            o.geom = mb.Take();
            o.layer = layerOf(e);
            o.color = layerColors.count(o.layer) ? layerColors[o.layer] : ON_Color::Black;
            out.push_back(std::move(o));
          }
          continue;
        }
        if (flags & 16) { skipped++; continue; }  // mesh poligonale MxN: non gestita
        const bool is3d = flags & 8;
        std::vector<ON_3dPoint> pts;
        std::vector<double> bulges;
        for (const auto& v : verts) { pts.push_back(P(v, 10)); bulges.push_back(is3d ? 0 : v.D(42)); }
        if (!is3d) for (auto& p : pts) p.z = e.D(30, p.z);
        const bool closed = flags & 1;
        std::vector<ON_Curve*> segs;
        const ON_Plane frame = is3d ? ON_Plane(ON_3dPoint::Origin, ON_3dVector::XAxis, ON_3dVector::YAxis) : ocs;
        for (size_t k = 0; pts.size() >= 2 && k + 1 < pts.size() + (closed ? 1 : 0); k++)
          segs.push_back(BulgeSegment(pts[k], pts[(k + 1) % pts.size()], bulges[k], frame));
        add(JoinSegments(segs), e);
      } else if (e.type == "SPLINE") {
        const int degree = e.I(71, 3);
        const auto knots = e.All(40);
        const auto weights = e.All(41);
        std::vector<ON_3dPoint> cps, fits;
        {
          double x = 0, y = 0;
          for (const auto& q : e.p) {
            if (q.code == 10) x = Num(q.value);
            else if (q.code == 20) y = Num(q.value);
            else if (q.code == 30) cps.emplace_back(x, y, Num(q.value));
            else if (q.code == 11) x = Num(q.value);
            else if (q.code == 21) y = Num(q.value);
            else if (q.code == 31) fits.emplace_back(x, y, Num(q.value));
          }
        }
        const int n = (int)cps.size();
        if (n >= degree + 1 && (int)knots.size() == n + degree + 1) {
          const bool rational = (int)weights.size() == n;
          auto* nc = new ON_NurbsCurve(3, rational, degree + 1, n);
          for (int k = 0; k < n; k++) {
            if (rational) nc->SetCV(k, ON_4dPoint(cps[k].x * weights[k], cps[k].y * weights[k], cps[k].z * weights[k], weights[k]));
            else nc->SetCV(k, cps[k]);
          }
          // DXF: n+degree+1 nodi; openNURBS: senza il primo e l'ultimo
          for (int k = 1; k + 1 < (int)knots.size(); k++) nc->m_knot[k - 1] = knots[k];
          if (nc->IsValid()) add(nc, e); else { delete nc; skipped++; }
        } else if (fits.size() >= 2) {
          // curva per i punti di passaggio: approssimata con una NURBS uniforme sui punti
          ON_3dPointArray fp;
          for (auto& p : fits) fp.Append(p);
          auto* nc = new ON_NurbsCurve();
          const int order = std::min(4, (int)fits.size());
          if (nc->CreateClampedUniformNurbs(3, order, fp.Count(), fp.Array())) add(nc, e);
          else { delete nc; add(new ON_PolylineCurve(fp), e); }
        } else skipped++;
      } else if (e.type == "3DFACE" || e.type == "SOLID") {
        MeshBuilder& mb = faces[layerOf(e)];
        ON_3dPoint q[4] = {P(e, 10), P(e, 11), P(e, 12), P(e, 13)};
        if (e.type == "SOLID") { std::swap(q[2], q[3]); for (auto& p : q) p = OcsToWcs(ocs, p); }
        int v[4];
        for (int k = 0; k < 4; k++) v[k] = mb.Vertex((xf * q[k]) * scale);
        mb.Face(v[0], v[1], v[2], v[3] == v[2] ? -1 : v[3]);
      } else if (e.type == "TEXT" || e.type == "MTEXT") {
        // testo come punto di testo (le annotazioni DXF non sono convertite)
        std::string t;
        for (const auto& q : e.p) if (q.code == 3) t += q.value;
        t = DxfPlainText(t + e.S(1));
        if (t.empty()) continue;
        auto* dot = new ON_TextDot(OcsToWcs(e.type == "TEXT" ? ocs : OcsPlane(ON_3dVector::ZAxis), P(e, 10)),
                                   ON_wString(t.c_str()), nullptr);
        add(dot, e);
      } else if (e.type == "INSERT" || e.type == "DIMENSION") {
        // INSERT: blocco esploso; DIMENSION: le linee, frecce e testo del suo blocco anonimo (*D..)
        auto bl = blocks.find(e.S(2));
        if (bl == blocks.end() || depth > 16) { skipped++; continue; }
        const std::string layer = layerOf(e);
        if (e.type == "DIMENSION") {
          // la geometria della quota e' gia' nella posizione finale (OCS della quota)
          convert(bl->second.ents, xf * OcsXform(ocs), layer, depth + 1);
          continue;
        }
        const ON_3dPoint ins = P(e, 10);
        const double sx = e.D(41, 1), sy = e.D(42, 1), sz = e.D(43, 1), rot = e.D(50) * ON_PI / 180;
        const int cols = std::max(1, e.I(70, 1)), rows = std::max(1, e.I(71, 1));
        const double dc = e.D(44), dr = e.D(45);
        const ON_Xform ocsXf = OcsXform(ocs);
        ON_Xform R;
        R.Rotation(rot, ON_3dVector::ZAxis, ON_3dPoint::Origin);
        const ON_Xform S = ON_Xform::DiagonalTransformation(sx, sy, sz);
        const ON_Xform B = ON_Xform::TranslationTransformation(ON_3dPoint::Origin - bl->second.base);
        for (int r = 0; r < rows; r++)
          for (int c = 0; c < cols; c++) {
            const ON_3dVector off = R * ON_3dVector(c * dc, r * dr, 0);
            const ON_Xform T = ON_Xform::TranslationTransformation(ON_3dVector(ins.x, ins.y, ins.z) + off);
            convert(bl->second.ents, xf * ocsXf * T * R * S * B, layer, depth + 1);
          }
      } else if (e.type != "SEQEND" && e.type != "VERTEX" && e.type != "ATTRIB" && e.type != "ATTDEF") {
        skipped++;
      }
    }
  };
  convert(ents, ON_Xform::IdentityTransformation, std::string(), 0);
  // scala delle unita' (le mesh di 3DFACE sono gia' scalate)
  if (std::fabs(scale - 1) > 1e-12)
    for (auto& o : out) if (!ON_Mesh::Cast(o.geom.get())) o.geom->Scale(scale);
  for (auto& [layer, mb] : faces) {
    if (mb.Empty()) continue;
    ImportedObject o;
    o.geom = mb.Take();
    o.layer = layer;
    o.color = layerColors.count(layer) ? layerColors[layer] : ON_Color::Black;
    out.push_back(std::move(o));
  }
  if (out.empty()) { err = skipped ? "nessuna entita' DXF supportata nel file" : "il file DXF non contiene entita'"; return false; }
  if (skipped) err = std::to_string(skipped) + " entita' non supportate (retini, immagini, regioni...) saltate";
  return true;
}

bool WriteDXF(const std::string& path, const std::vector<ExportItem>& items, ON::LengthUnitSystem docUnits, double tol,
              std::string& err) {
  std::ostringstream ents;
  ents.precision(12);
  std::map<std::string, ON_Color> layers{{"0", ON_Color::Black}};
  auto layerName = [&](const ExportItem& it) {
    std::string l = it.layer.empty() ? "0" : it.layer;
    for (auto& c : l) if (c == ' ' || c == '<' || c == '>' || c == '/' || c == '\\' || c == ':' || c == '"' || c == ';' || c == '?' || c == '*' || c == '|' || c == '=' || c == ',') c = '_';
    if (!layers.count(l)) layers[l] = it.color;
    return l;
  };
  auto pt = [&](int code, const ON_3dPoint& p) {
    ents << code << "\n" << p.x << "\n" << code + 10 << "\n" << p.y << "\n" << code + 20 << "\n" << p.z << "\n";
  };
  auto polyline = [&](const std::string& l, const ON_Polyline& pl, bool closed) {
    ents << "0\nPOLYLINE\n8\n" << l << "\n66\n1\n10\n0\n20\n0\n30\n0\n70\n" << (8 | (closed ? 1 : 0)) << "\n";
    for (int k = 0; k < pl.Count() - (closed ? 1 : 0); k++) {
      ents << "0\nVERTEX\n8\n" << l << "\n";
      pt(10, pl[k]);
      ents << "70\n32\n";
    }
    ents << "0\nSEQEND\n8\n" << l << "\n";
  };
  int n = 0;
  for (const auto& it : items) {
    const std::string l = layerName(it);
    const ON_Geometry* g = it.geom;
    if (const auto* p = ON_Point::Cast(g)) { ents << "0\nPOINT\n8\n" << l << "\n"; pt(10, p->point); n++; }
    else if (const auto* c = ON_Curve::Cast(g)) {
      ON_Arc arc;
      const bool planarZ = c->IsArc(nullptr, &arc, tol) && std::fabs(std::fabs(arc.plane.zaxis.z) - 1) < 1e-9;
      if (const auto* lc = ON_LineCurve::Cast(c)) {
        ents << "0\nLINE\n8\n" << l << "\n";
        pt(10, lc->m_line.from);
        pt(11, lc->m_line.to);
      } else if (planarZ) {
        // archi e cerchi orizzontali (normale +Z o -Z: OCS con estrusione)
        const ON_3dVector nz = arc.plane.zaxis.z > 0 ? ON_3dVector::ZAxis : -ON_3dVector::ZAxis;
        const ON_Plane ocs = OcsPlane(nz);
        ON_3dPoint cc(arc.Center() - ON_3dPoint::Origin);
        const ON_3dPoint oc(cc * ocs.xaxis, cc * ocs.yaxis, cc * ocs.zaxis);
        ents << "0\n" << (arc.IsCircle() ? "CIRCLE" : "ARC") << "\n8\n" << l << "\n";
        pt(10, oc);
        ents << "40\n" << arc.Radius() << "\n";
        if (nz.z < 0) ents << "210\n0\n220\n0\n230\n-1\n";
        if (!arc.IsCircle()) {
          auto ang = [&](const ON_3dPoint& p) {
            const ON_3dVector v = p - arc.Center();
            double a = std::atan2(v * ocs.yaxis, v * ocs.xaxis) * 180 / ON_PI;
            return a < 0 ? a + 360 : a;
          };
          ON_3dPoint s = arc.StartPoint(), e = arc.EndPoint();
          // gli archi DXF vanno in senso antiorario attorno all'estrusione
          if (ON_CrossProduct(arc.plane.xaxis, arc.plane.yaxis) * nz < 0) std::swap(s, e);
          ents << "50\n" << ang(s) << "\n51\n" << ang(e) << "\n";
        }
      } else {
        ON_Polyline pl;
        if (c->IsPolyline(&pl) < 2) {
          pl.Empty();
          const ON_Interval d = c->Domain();
          const int segs = std::max(16, 8 * std::max(1, c->SpanCount()) * std::max(1, c->Degree()));
          for (int k = 0; k <= segs; k++) pl.Append(c->PointAt(d.ParameterAt((double)k / segs)));
        }
        polyline(l, pl, c->IsClosed());
      }
      n++;
    } else if (const auto* dot = ON_TextDot::Cast(g)) {
      ents << "0\nTEXT\n8\n" << l << "\n";
      pt(10, dot->CenterPoint());
      ents << "40\n" << 1.0 << "\n1\n" << ToUtf8(dot->PrimaryText()) << "\n";
      n++;
    } else if (auto m = ExportMesh(*g, tol)) {
      for (int k = 0; k < m->m_F.Count(); k++) {
        const ON_MeshFace& f = m->m_F[k];
        ents << "0\n3DFACE\n8\n" << l << "\n";
        for (int j = 0; j < 4; j++) pt(10 + j, ON_3dPoint(m->m_V[f.vi[j]]));
      }
      n++;
    }
  }
  if (!n) { err = "nessun oggetto esportabile in DXF"; return false; }
  std::ofstream f(fs::u8path(path), std::ios::binary);
  if (!f) { err = "impossibile scrivere " + path; return false; }
  f << "0\nSECTION\n2\nHEADER\n9\n$ACADVER\n1\nAC1009\n9\n$INSUNITS\n70\n" << InsUnitsCode(docUnits) << "\n0\nENDSEC\n";
  f << "0\nSECTION\n2\nTABLES\n0\nTABLE\n2\nLAYER\n70\n" << layers.size() << "\n";
  for (const auto& [name, col] : layers)
    f << "0\nLAYER\n2\n" << name << "\n70\n0\n62\n" << NearestAci(col) << "\n6\nCONTINUOUS\n";
  f << "0\nENDTAB\n0\nENDSEC\n";
  f << "0\nSECTION\n2\nENTITIES\n" << ents.str() << "0\nENDSEC\n0\nEOF\n";
  if (!f) { err = "errore di scrittura"; return false; }
  return true;
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
}  // namespace

bool ImportExchange(Engine& e, const std::string& path, const std::map<std::string, std::string>& opts) {
  (void)opts;
  const std::string ext = FileExtension(path);
  std::vector<ImportedObject> objs;
  std::string err;
  const ON::LengthUnitSystem units = e.Model().m_settings.m_ModelUnitsAndTolerances.m_unit_system.UnitSystem();
  bool ok = false;
  if (ext == "stl") ok = ReadSTL(path, objs, err);
  else if (ext == "obj") ok = ReadOBJ(path, objs, err);
  else if (ext == "dxf") ok = ReadDXF(path, units, objs, err);
  else if (ext == "step" || ext == "stp" || ext == "iges" || ext == "igs") ok = ReadStepIges(path, units, e.Tolerance(), objs, err);
  else if (ext == "gltf" || ext == "glb") ok = ReadGLTF(path, units, objs, err);
  else if (ext == "3mf") ok = Read3MF(path, units, objs, err);
  else if (ext == "ply") ok = ReadPLY(path, objs, err);
  else err = "formato non supportato: " + ext;
  if (!ok) return e.Fail(err);
  e.ClearSelection();
  int n = 0;
  std::map<const ExchangeMaterial*, int> matIndex;
  for (auto& o : objs) {
    ON_3dmObjectAttributes at;
    at.m_layer_index = LayerFor(e, o.layer, o.color);
    if (!o.name.empty()) at.SetName(ON_wString(o.name.c_str()), true);
    if (o.material) {
      auto f = matIndex.find(o.material.get());
      const int mi = f != matIndex.end() ? f->second : (matIndex[o.material.get()] = AddExchangeMaterial(e.Model(), *o.material));
      if (mi >= 0) {
        at.m_material_index = mi;
        at.SetMaterialSource(ON::material_from_object);
      }
    }
    const ON_UUID id = e.AddObject(o.geom.release(), &at);
    if (!ON_UuidIsNil(id)) { e.Select(id); n++; }
  }
  e.Out() << "  Importati " << n << " oggetti da " << U8(fs::u8path(path).filename()) << "\n";
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
  else if (ext == "dxf") ok = WriteDXF(path, items, units, e.Tolerance(), err);
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
bool ReadStepIges(const std::string&, ON::LengthUnitSystem, double, std::vector<ImportedObject>&, std::string& err) {
  err = "STEP e IGES richiedono il modulo di scambio dati di OpenCASCADE, non presente in questa build";
  return false;
}
bool WriteStepIges(const std::string&, const std::vector<ExportItem>&, ON::LengthUnitSystem, double, std::string& err) {
  err = "STEP e IGES richiedono il modulo di scambio dati di OpenCASCADE, non presente in questa build";
  return false;
}
#endif

}  // namespace cad
