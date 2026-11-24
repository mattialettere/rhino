// Modificatori della mesh di rendering e decalcomanie, come nelle proprieta' dell'oggetto di
// Rhino 8: Displacement (spostamento da un'immagine in grigio), EdgeSoftening (spigoli
// ammorbiditi), Shutlining (scanalature lungo curve), Decal (immagine proiettata su parte
// dell'oggetto). Non distruttivi: i parametri stanno in testi dell'oggetto e si applicano alla
// mesh di visualizzazione e rendering; ExtractRenderMesh restituisce la mesh modificata.
#include "commands/Engine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace cad {

bool LoadGrayImageFile(const std::string& path, int& w, int& h, std::vector<double>& g);

namespace {

const wchar_t* kDisp = L"Kurva3D.Displacement";
const wchar_t* kSoft = L"Kurva3D.EdgeSoftening";
const wchar_t* kShut = L"Kurva3D.Shutlining";
const wchar_t* kDecal = L"Kurva3D.Decal";

std::vector<std::string> Split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == sep) { out.push_back(cur); cur.clear(); } else cur += c;
  }
  out.push_back(cur);
  return out;
}

std::string Unq(std::string s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
  return s;
}

std::string GetString(const ON_3dmObjectAttributes& a, const wchar_t* key) {
  ON_wString v;
  if (!a.GetUserString(key, v) || v.IsEmpty()) return {};
  return ToUtf8(v);
}

struct GrayImage {
  int w = 0, h = 0;
  std::vector<double> g;
  double Sample(double u, double v) const {  // bilineare, ripetuta
    if (w < 1 || h < 1) return 0;
    u -= std::floor(u);
    v -= std::floor(v);
    const double x = u * (w - 1), y = (1.0 - v) * (h - 1);
    const int x0 = std::clamp((int)x, 0, w - 1), y0 = std::clamp((int)y, 0, h - 1);
    const int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    const double fx = x - x0, fy = y - y0;
    auto at = [&](int xx, int yy) { return g[size_t(yy) * w + xx]; };
    return (at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) + (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy;
  }
};

bool LoadImage(Engine& e, const std::string& path, GrayImage& img) {
  bool ok = e.imageLoader ? e.imageLoader(path, img.w, img.h, img.g) : false;
  if (!ok) ok = LoadGrayImageFile(path, img.w, img.h, img.g);
  return ok && img.w > 0 && img.h > 0;
}

// Coordinate di texture: quelle della mesh, altrimenti proiezione planare dall'alto
// sulla scatola di ingombro (come la mappatura planare predefinita).
void EnsureUV(ON_Mesh& m) {
  if (m.m_T.Count() == m.m_V.Count() && m.m_T.Count() > 0) return;
  ON_BoundingBox bb = m.BoundingBox();
  const double dx = std::max(bb.m_max.x - bb.m_min.x, 1e-12), dy = std::max(bb.m_max.y - bb.m_min.y, 1e-12);
  m.m_T.SetCount(0);
  m.m_T.Reserve(m.m_V.Count());
  for (int i = 0; i < m.m_V.Count(); i++)
    m.m_T.Append(ON_2fPoint(float((m.m_V[i].x - bb.m_min.x) / dx), float((m.m_V[i].y - bb.m_min.y) / dy)));
}

// Suddivisione 1 -> 4 dei triangoli (i quadrilateri prima in due triangoli), con le
// coordinate di texture interpolate. I vertici sui lati sono condivisi.
void Subdivide(ON_Mesh& m, int levels) {
  for (int l = 0; l < levels; l++) {
    m.ConvertQuadsToTriangles();
    const bool hasT = m.m_T.Count() == m.m_V.Count();
    ON_Mesh out;
    out.m_V = m.m_V;
    if (hasT) out.m_T = m.m_T;
    std::map<std::pair<int, int>, int> mid;
    auto midpoint = [&](int a, int b) {
      const auto key = std::make_pair(std::min(a, b), std::max(a, b));
      auto it = mid.find(key);
      if (it != mid.end()) return it->second;
      const int i = out.m_V.Count();
      out.m_V.Append(ON_3fPoint(0.5f * (m.m_V[a].x + m.m_V[b].x), 0.5f * (m.m_V[a].y + m.m_V[b].y), 0.5f * (m.m_V[a].z + m.m_V[b].z)));
      if (hasT) out.m_T.Append(ON_2fPoint(0.5f * (m.m_T[a].x + m.m_T[b].x), 0.5f * (m.m_T[a].y + m.m_T[b].y)));
      mid[key] = i;
      return i;
    };
    for (int fi = 0; fi < m.m_F.Count(); fi++) {
      const ON_MeshFace& f = m.m_F[fi];
      const int a = f.vi[0], b = f.vi[1], c = f.vi[2];
      const int ab = midpoint(a, b), bc = midpoint(b, c), ca = midpoint(c, a);
      out.SetTriangle(out.m_F.Count(), a, ab, ca);
      out.SetTriangle(out.m_F.Count(), ab, b, bc);
      out.SetTriangle(out.m_F.Count(), ca, bc, c);
      out.SetTriangle(out.m_F.Count(), ab, bc, ca);
    }
    m = out;
  }
  m.m_N.SetCount(0);
  m.ComputeVertexNormals();
}

// Normali per posizione: i vertici doppi (spigoli vivi della mesh) si spostano insieme
// e la mesh non si apre.
std::vector<ON_3dVector> WeldedNormals(const ON_Mesh& m, std::vector<int>& group) {
  std::map<std::tuple<long long, long long, long long>, int> idx;
  group.assign(m.m_V.Count(), 0);
  ON_BoundingBox bb = m.BoundingBox();
  const double q = std::max(bb.Diagonal().Length() * 1e-7, 1e-9);
  int ng = 0;
  for (int i = 0; i < m.m_V.Count(); i++) {
    const auto key = std::make_tuple((long long)std::llround(m.m_V[i].x / q), (long long)std::llround(m.m_V[i].y / q), (long long)std::llround(m.m_V[i].z / q));
    auto it = idx.find(key);
    if (it == idx.end()) it = idx.emplace(key, ng++).first;
    group[i] = it->second;
  }
  std::vector<ON_3dVector> n(ng, ON_3dVector::ZeroVector);
  for (int fi = 0; fi < m.m_F.Count(); fi++) {
    const ON_MeshFace& f = m.m_F[fi];
    const int k = f.IsQuad() ? 4 : 3;
    ON_3dVector fn = ON_CrossProduct(ON_3dPoint(m.m_V[f.vi[1]]) - ON_3dPoint(m.m_V[f.vi[0]]),
                                     ON_3dPoint(m.m_V[f.vi[k - 1]]) - ON_3dPoint(m.m_V[f.vi[0]]));
    for (int j = 0; j < k; j++) n[group[f.vi[j]]] += fn;
  }
  for (auto& v : n) v.Unitize();
  return n;
}

void ApplyDisplacement(Engine& e, const std::string& spec, ON_Mesh& m) {
  const auto p = Split(spec, '|');
  if (p.size() < 3) return;
  GrayImage img;
  if (!LoadImage(e, p[0], img)) return;
  const double amount = std::atof(p[1].c_str());
  const int levels = std::clamp(std::atoi(p[2].c_str()), 0, 5);
  const double black = p.size() > 3 ? std::atof(p[3].c_str()) : 0.0;
  const double white = p.size() > 4 ? std::atof(p[4].c_str()) : 1.0;
  EnsureUV(m);
  Subdivide(m, levels);
  std::vector<int> group;
  const auto n = WeldedNormals(m, group);
  const double span = std::max(white - black, 1e-9);
  for (int i = 0; i < m.m_V.Count(); i++) {
    const double g = std::clamp((img.Sample(m.m_T[i].x, m.m_T[i].y) - black) / span, 0.0, 1.0);
    const ON_3dVector d = n[group[i]] * (amount * g);
    m.m_V[i] = ON_3fPoint(float(m.m_V[i].x + d.x), float(m.m_V[i].y + d.y), float(m.m_V[i].z + d.z));
  }
  m.m_N.SetCount(0);
  m.ComputeVertexNormals();
}

// EdgeSoftening: le normali dei vertici entro Radius da uno spigolo vivo (angolo tra le facce
// oltre AngleThreshold) si fondono gradualmente; con Chamfer=Yes i vertici sullo spigolo
// rientrano lungo la bisettrice, come uno smusso.
void ApplyEdgeSoftening(const std::string& spec, ON_Mesh& m) {
  const auto p = Split(spec, '|');
  const double radius = std::max(std::atof(p[0].c_str()), 0.0);
  const bool chamfer = p.size() > 1 && p[1] == "1";
  const double angTh = (p.size() > 2 ? std::atof(p[2].c_str()) : 30.0) * ON_PI / 180.0;
  if (radius <= 0) return;
  std::vector<int> group;
  const auto smoothN = WeldedNormals(m, group);
  m.ComputeFaceNormals();
  // vertici su spigoli vivi: posizione del gruppo che ha facce con normali molto diverse
  std::vector<ON_3dVector> firstN(smoothN.size(), ON_3dVector::ZeroVector);
  std::vector<char> sharp(smoothN.size(), 0);
  for (int fi = 0; fi < m.m_F.Count(); fi++) {
    const ON_MeshFace& f = m.m_F[fi];
    const ON_3dVector fn(m.m_FN[fi]);
    for (int j = 0; j < (f.IsQuad() ? 4 : 3); j++) {
      const int g = group[f.vi[j]];
      if (firstN[g].IsZero()) firstN[g] = fn;
      else {
        ON_3dVector a = firstN[g], b = fn;
        a.Unitize();
        b.Unitize();
        if (std::acos(std::clamp(a * b, -1.0, 1.0)) > angTh) sharp[g] = 1;
      }
    }
  }
  std::vector<ON_3dPoint> sharpPts;
  std::vector<ON_3dPoint> gp(smoothN.size());
  for (int i = 0; i < m.m_V.Count(); i++) gp[group[i]] = ON_3dPoint(m.m_V[i]);
  for (size_t g = 0; g < sharp.size(); g++) if (sharp[g]) sharpPts.push_back(gp[g]);
  if (sharpPts.empty()) return;
  if (!m.HasVertexNormals()) m.ComputeVertexNormals();
  for (int i = 0; i < m.m_V.Count(); i++) {
    const ON_3dPoint P(m.m_V[i]);
    double d = 1e300;
    for (const auto& s : sharpPts) d = std::min(d, P.DistanceTo(s));
    if (d >= radius) continue;
    const double t = 1.0 - d / radius;  // 1 sullo spigolo, 0 a distanza Radius
    ON_3dVector nn = ON_3dVector(m.m_N[i]) * (1.0 - t) + smoothN[group[i]] * t;
    nn.Unitize();
    m.m_N[i] = ON_3fVector(nn);
    if (chamfer && sharp[group[i]]) {
      const ON_3dVector off = -smoothN[group[i]] * (radius * 0.2929);  // (1 - cos 45°) del raggio
      m.m_V[i] = ON_3fPoint(float(P.x + off.x), float(P.y + off.y), float(P.z + off.z));
    }
  }
}

// Shutlining: scanalatura a profilo circolare (o quadrato) di raggio Radius lungo le curve.
void ApplyShutlining(Engine& e, const std::string& spec, ON_Mesh& m) {
  const auto p = Split(spec, '|');
  if (p.size() < 3) return;
  const double radius = std::atof(p[0].c_str());
  const bool square = p[1] == "square";
  if (radius <= 0) return;
  std::vector<std::vector<ON_3dPoint>> polys;
  for (const auto& sid : Split(p[2], ',')) {
    const ON_UUID id = ON_UuidFromString(sid.c_str());
    const auto* o = e.Object(id);
    const ON_Curve* c = o ? ON_Curve::Cast(o->Geometry(nullptr)) : nullptr;
    if (!c) continue;
    std::vector<ON_3dPoint> pl;
    const ON_Interval dom = c->Domain();
    for (int i = 0; i <= 400; i++) pl.push_back(c->PointAt(dom.ParameterAt(i / 400.0)));
    polys.push_back(std::move(pl));
  }
  if (polys.empty()) return;
  // densita' sufficiente: lati della mesh piu' corti del raggio vicino alle curve
  ON_BoundingBox bb = m.BoundingBox();
  int levels = 0;
  double edge = 0;
  for (int fi = 0; fi < m.m_F.Count(); fi++)
    edge = std::max(edge, ON_3dPoint(m.m_V[m.m_F[fi].vi[0]]).DistanceTo(ON_3dPoint(m.m_V[m.m_F[fi].vi[1]])));
  while (edge / (1 << levels) > radius * 0.75 && levels < 4) levels++;
  if (m.m_F.Count() * (1 << (2 * levels)) > 400000) levels = std::max(0, levels - 1);
  Subdivide(m, levels);
  std::vector<int> group;
  const auto n = WeldedNormals(m, group);
  for (int i = 0; i < m.m_V.Count(); i++) {
    const ON_3dPoint P(m.m_V[i]);
    double d = 1e300;
    for (const auto& pl : polys)
      for (size_t k = 0; k + 1 < pl.size(); k++) {
        const ON_3dVector v = pl[k + 1] - pl[k];
        const double l2 = v * v;
        double t = l2 > 0 ? ((P - pl[k]) * v) / l2 : 0;
        t = std::clamp(t, 0.0, 1.0);
        d = std::min(d, P.DistanceTo(pl[k] + t * v));
      }
    if (d >= radius) continue;
    const double depth = square ? radius : std::sqrt(radius * radius - d * d);
    const ON_3dVector off = -n[group[i]] * depth;
    m.m_V[i] = ON_3fPoint(float(P.x + off.x), float(P.y + off.y), float(P.z + off.z));
  }
  (void)bb;
  m.m_N.SetCount(0);
  m.ComputeVertexNormals();
}

bool SetModifier(Engine& e, const std::vector<ON_UUID>& sel, const wchar_t* key, const std::string& value) {
  for (const auto& id : sel)
    if (auto* a = e.EditableAttributes(id)) {
      if (value.empty()) a->SetUserString(key, nullptr);
      else a->SetUserString(key, ON_wString(value.c_str()));
    }
  return true;
}

bool IsOff(const std::map<std::string, std::string>& o) {
  auto it = o.find("off");
  return o.count("off") || (o.count("enabled") && (o.at("enabled") == "no" || o.at("enabled") == "off")) ||
         (it != o.end() && it->second != "no");
}

// Displacement File=img Amount=1 Subdivisions=2 Black=0 White=1 | Off
bool CmdDisplacement(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("Displacement", "seleziona superfici, polisuperfici, mesh o SubD, poi Invio");
  if (sel.empty()) return false;
  auto o = e.ReadOptions(ts, {"off"});
  if (IsOff(o)) { SetModifier(e, sel, kDisp, ""); e.Out() << "  Displacement tolto\n"; return true; }
  std::string file = o.count("file") ? Unq(o["file"]) : "";
  if (file.empty()) {
    auto f = e.ReadString(ts, "immagine in toni di grigio (bianco = spostamento massimo)");
    if (!f) return false;
    file = Unq(*f);
    o.merge(e.ReadOptions(ts, {"off"}));
  }
  GrayImage img;
  if (!LoadImage(e, file, img)) return e.Fail("immagine non leggibile: " + file);
  const double amount = o.count("amount") ? std::atof(o["amount"].c_str()) : 1.0;
  const int sub = o.count("subdivisions") ? std::clamp(std::atoi(o["subdivisions"].c_str()), 0, 5) : 2;
  const double black = o.count("black") ? std::atof(o["black"].c_str()) : 0.0;
  const double white = o.count("white") ? std::atof(o["white"].c_str()) : 1.0;
  char buf[160];
  std::snprintf(buf, sizeof buf, "|%.17g|%d|%.17g|%.17g", amount, sub, black, white);
  SetModifier(e, sel, kDisp, file + buf);
  e.Out() << "  Displacement su " << sel.size() << " oggetti (altezza " << amount << ")\n";
  return true;
}

// EdgeSoftening Radius=1 Chamfer=No AngleThreshold=30 | Off
bool CmdEdgeSoftening(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("EdgeSoftening", "seleziona gli oggetti, poi Invio");
  if (sel.empty()) return false;
  auto o = e.ReadOptions(ts, {"off"});
  if (IsOff(o)) { SetModifier(e, sel, kSoft, ""); e.Out() << "  EdgeSoftening tolto\n"; return true; }
  double r = 0;
  if (o.count("radius")) r = std::atof(o["radius"].c_str());
  else {
    auto v = e.ReadNumber(ts, "raggio di ammorbidimento");
    if (!v) return false;
    r = *v;
    o.merge(e.ReadOptions(ts));
  }
  if (!(r > 0)) return e.Fail("il raggio deve essere positivo");
  const bool ch = o.count("chamfer") && (o["chamfer"] == "yes" || o["chamfer"] == "si");
  const double ang = o.count("anglethreshold") ? std::atof(o["anglethreshold"].c_str()) : 30.0;
  char buf[120];
  std::snprintf(buf, sizeof buf, "%.17g|%d|%.17g", r, ch ? 1 : 0, ang);
  SetModifier(e, sel, kSoft, buf);
  e.Out() << "  EdgeSoftening su " << sel.size() << " oggetti (raggio " << r << ")\n";
  return true;
}

// Shutlining: oggetto, poi curve (clic), Radius=, Profile=Round|Square | Off
bool CmdShutlining(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("Shutlining", "seleziona l'oggetto, poi Invio");
  if (sel.empty()) return false;
  auto o = e.ReadOptions(ts, {"off"});
  if (IsOff(o)) { SetModifier(e, sel, kShut, ""); e.Out() << "  Shutlining tolto\n"; return true; }
  std::vector<std::string> ids;
  while (true) {
    auto p = e.ReadPick(ts, ids.empty() ? "curva della scanalatura" : "altra curva (Invio = fine)");
    if (!p) { if (e.NeedsInput()) return false; break; }
    ON_UUID best = ON_nil_uuid;
    double bd = 1e300;
    for (const auto& id : e.AllObjectIds(false)) {
      const auto* ob = e.Object(id);
      const ON_Curve* c = ob ? ON_Curve::Cast(ob->Geometry(nullptr)) : nullptr;
      if (!c || c->BoundingBox().MinimumDistanceTo(*p) > bd) continue;
      const ON_Interval dom = c->Domain();
      for (int i = 0; i <= 200; i++) {
        const double d = c->PointAt(dom.ParameterAt(i / 200.0)).DistanceTo(*p);
        if (d < bd) { bd = d; best = id; }
      }
    }
    if (ON_UuidIsNil(best)) return e.Fail("nessuna curva vicino al punto");
    char sid[64];
    ON_UuidToString(best, sid);
    if (std::find(ids.begin(), ids.end(), std::string(sid)) == ids.end()) ids.push_back(sid);
    if (!e.MoreInput(ts)) break;
  }
  if (ids.empty()) return e.Fail("nessuna curva");
  e.SkipEnter(ts);
  o.merge(e.ReadOptions(ts));
  const double r = o.count("radius") ? std::atof(o["radius"].c_str()) : 0.5;
  if (!(r > 0)) return e.Fail("il raggio deve essere positivo");
  const std::string prof = o.count("profile") && o["profile"] == "square" ? "square" : "round";
  std::string list;
  for (auto& s : ids) list += (list.empty() ? "" : ",") + s;
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.17g|", r);
  SetModifier(e, sel, kShut, buf + prof + "|" + list);
  e.Out() << "  Shutlining con " << ids.size() << " curve (raggio " << r << ")\n";
  return true;
}

// Decal: immagine proiettata (planare) sul rettangolo indicato da due angoli sul CPlane.
bool CmdDecal(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("Decal", "seleziona l'oggetto, poi Invio");
  if (sel.empty()) return false;
  auto o = e.ReadOptions(ts, {"off"});
  if (IsOff(o)) { SetModifier(e, sel, kDecal, ""); e.Out() << "  Decalcomanie tolte\n"; return true; }
  auto f = e.ReadString(ts, "immagine della decalcomania");
  if (!f) return false;
  const std::string file = Unq(*f);
  auto a = e.ReadPoint(ts, "primo angolo della decalcomania");
  if (!a) return false;
  e.lastPoint = *a;
  auto b = e.ReadPoint(ts, "angolo opposto");
  if (!b) return false;
  ON_Plane cp = ON_Plane::World_xy;
  ON_3dmView v;
  if (e.getActiveView && e.getActiveView(v) && v.m_cplane.m_plane.IsValid()) cp = v.m_cplane.m_plane;
  double ua, va, ub, vb;
  cp.ClosestPointTo(*a, &ua, &va);
  cp.ClosestPointTo(*b, &ub, &vb);
  const double w = std::fabs(ub - ua), h = std::fabs(vb - va);
  if (w < 1e-9 || h < 1e-9) return e.Fail("rettangolo nullo");
  const ON_3dPoint org = cp.PointAt(std::min(ua, ub), std::min(va, vb));
  char buf[400];
  std::snprintf(buf, sizeof buf, "|%.17g %.17g %.17g|%.17g %.17g %.17g|%.17g %.17g %.17g|%.17g|%.17g", org.x, org.y, org.z,
                cp.xaxis.x, cp.xaxis.y, cp.xaxis.z, cp.yaxis.x, cp.yaxis.y, cp.yaxis.z, w, h);
  SetModifier(e, sel, kDecal, file + buf);
  e.Out() << "  Decalcomania \"" << file << "\" su " << sel.size() << " oggetti\n";
  return true;
}

}  // namespace

bool HasMeshModifiers(Engine& e, const ON_UUID& id, std::string* signature) {
  const auto* o = e.Object(id);
  const ON_3dmObjectAttributes* a = o ? o->Attributes(nullptr) : nullptr;
  if (!a) return false;
  const std::string s = GetString(*a, kDisp) + "#" + GetString(*a, kSoft) + "#" + GetString(*a, kShut);
  if (signature) *signature = s;
  return s != "##";
}

void ApplyMeshModifiers(Engine& e, const ON_UUID& id, ON_Mesh& m) {
  const auto* o = e.Object(id);
  const ON_3dmObjectAttributes* a = o ? o->Attributes(nullptr) : nullptr;
  if (!a) return;
  // ordine di Rhino: displacement, ammorbidimento degli spigoli, scanalature
  if (auto s = GetString(*a, kDisp); !s.empty()) ApplyDisplacement(e, s, m);
  if (auto s = GetString(*a, kSoft); !s.empty()) ApplyEdgeSoftening(s, m);
  if (auto s = GetString(*a, kShut); !s.empty()) ApplyShutlining(e, s, m);
  m.InvalidateBoundingBoxes();  // vertici spostati: l'ingombro memorizzato non vale piu'
}

bool DecalMesh(Engine& e, const ON_UUID& id, const ON_Mesh& src, ON_Mesh& out, std::string& file) {
  const auto* o = e.Object(id);
  const ON_3dmObjectAttributes* a = o ? o->Attributes(nullptr) : nullptr;
  if (!a) return false;
  const auto p = Split(GetString(*a, kDecal), '|');
  if (p.size() < 6) return false;
  file = p[0];
  ON_3dPoint org;
  ON_3dVector xa, ya;
  if (std::sscanf(p[1].c_str(), "%lf %lf %lf", &org.x, &org.y, &org.z) != 3 ||
      std::sscanf(p[2].c_str(), "%lf %lf %lf", &xa.x, &xa.y, &xa.z) != 3 ||
      std::sscanf(p[3].c_str(), "%lf %lf %lf", &ya.x, &ya.y, &ya.z) != 3)
    return false;
  const double w = std::atof(p[4].c_str()), h = std::atof(p[5].c_str());
  if (!(w > 0 && h > 0)) return false;
  const ON_3dVector za = ON_CrossProduct(xa, ya);
  ON_BoundingBox bb = src.BoundingBox();
  const double lift = std::max(bb.Diagonal().Length() * 2e-4, 1e-6);  // sopra la superficie
  ON_Mesh m(src);
  m.ConvertQuadsToTriangles();
  if (!m.HasVertexNormals()) m.ComputeVertexNormals();
  m.ComputeFaceNormals();
  out.Destroy();
  std::map<int, int> remap;
  auto uv = [&](int vi) {
    const ON_3dVector d = ON_3dPoint(m.m_V[vi]) - org;
    return ON_2dPoint((d * xa) / w, (d * ya) / h);
  };
  for (int fi = 0; fi < m.m_F.Count(); fi++) {
    const ON_MeshFace& f = m.m_F[fi];
    if (ON_3dVector(m.m_FN[fi]) * za <= 0.05) continue;  // solo le facce rivolte alla proiezione
    bool inside = false;
    for (int j = 0; j < 3; j++) {
      const ON_2dPoint t = uv(f.vi[j]);
      if (t.x >= 0 && t.x <= 1 && t.y >= 0 && t.y <= 1) inside = true;
    }
    if (!inside) continue;
    int nv[3];
    for (int j = 0; j < 3; j++) {
      auto it = remap.find(f.vi[j]);
      if (it == remap.end()) {
        const int k = out.m_V.Count();
        const ON_3dVector n(m.m_N[f.vi[j]]);
        const ON_3dPoint P = ON_3dPoint(m.m_V[f.vi[j]]) + n * lift;
        out.m_V.Append(ON_3fPoint(P));
        out.m_N.Append(m.m_N[f.vi[j]]);
        const ON_2dPoint t = uv(f.vi[j]);
        out.m_T.Append(ON_2fPoint((float)std::clamp(t.x, 0.0, 1.0), (float)std::clamp(t.y, 0.0, 1.0)));
        it = remap.emplace(f.vi[j], k).first;
      }
      nv[j] = it->second;
    }
    out.SetTriangle(out.m_F.Count(), nv[0], nv[1], nv[2]);
  }
  return out.m_F.Count() > 0;
}

void RegisterMeshModifierCommands(Engine& e) {
  auto reg = [&](const char* n, const char* cat, const char* d, CommandFn f) {
    CommandInfo i{n, cat, d, f};
    e.Register(i);
  };
  reg("Displacement", "Render", "Spostamento della mesh di rendering da un'immagine (File=, Amount=, Subdivisions=, Off)", CmdDisplacement);
  reg("EdgeSoftening", "Render", "Spigoli ammorbiditi nella mesh di rendering (Radius=, Chamfer=, AngleThreshold=, Off)", CmdEdgeSoftening);
  reg("Shutlining", "Render", "Scanalature lungo curve nella mesh di rendering (Radius=, Profile=Round|Square, Off)", CmdShutlining);
  reg("Decal", "Render", "Decalcomania: immagine proiettata su parte dell'oggetto (Off per toglierle)", CmdDecal);
}

}  // namespace cad
