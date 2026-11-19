// Funzioni comuni ai lettori e scrittori dei formati di scambio (DXF, STL, OBJ, ...).
#pragma once
#include "opennurbs.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <tuple>

namespace cad {
namespace xu {

inline std::string U8(const std::filesystem::path& p) {
  const auto u = p.u8string();
  return std::string(u.begin(), u.end());
}
inline std::string Lower(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}
inline std::string Upper(std::string s) {
  for (auto& c : s) c = (char)std::toupper((unsigned char)c);
  return s;
}
inline std::string Trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace((unsigned char)s[a])) a++;
  while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
  return s.substr(a, b - a);
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
inline ON_Color AciColor(int aci) {
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

inline int NearestAci(const ON_Color& c) {
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
inline ON_Plane OcsPlane(const ON_3dVector& nIn) {
  ON_3dVector n = nIn;
  if (!n.Unitize()) n = ON_3dVector::ZAxis;
  ON_3dVector ax = (std::fabs(n.x) < 1.0 / 64 && std::fabs(n.y) < 1.0 / 64) ? ON_CrossProduct(ON_3dVector::YAxis, n)
                                                                          : ON_CrossProduct(ON_3dVector::ZAxis, n);
  ax.Unitize();
  ON_3dVector ay = ON_CrossProduct(n, ax);
  ay.Unitize();
  return ON_Plane(ON_3dPoint::Origin, ax, ay);
}

inline ON_3dPoint OcsToWcs(const ON_Plane& p, const ON_3dPoint& q) {
  return ON_3dPoint::Origin + q.x * p.xaxis + q.y * p.yaxis + q.z * p.zaxis;
}

}  // namespace xu
}  // namespace cad
