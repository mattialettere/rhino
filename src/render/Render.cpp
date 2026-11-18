// Path tracer su CPU: BVH dei triangoli, materiali diffusi/lucidi/metallici/vetro,
// luci del modello con ombre, sole, cielo e piano di terra.
#include "render/Render.h"
#include "commands/Engine.h"
#include "ui/Scene.h"
#include <algorithm>
#include <cmath>
#include <thread>

namespace cad {

std::function<bool(const std::string&, int&, int&, std::vector<unsigned char>&)> RenderScene::imageLoader;

namespace {

inline float ToLinear(int c) {
  const float x = c / 255.0f;
  return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
}
inline ON_3fVector Lin(const ON_Color& c) { return ON_3fVector(ToLinear(c.Red()), ToLinear(c.Green()), ToLinear(c.Blue())); }
inline ON_3fVector Mul(const ON_3fVector& a, const ON_3fVector& b) { return ON_3fVector(a.x * b.x, a.y * b.y, a.z * b.z); }
inline float MaxC(const ON_3fVector& a) { return std::max(a.x, std::max(a.y, a.z)); }

// generatore pseudo-casuale per pixel
inline float Rnd(unsigned& s) {
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return (s & 0xFFFFFF) / 16777216.0f;
}

void Basis(const ON_3dVector& n, ON_3dVector& t, ON_3dVector& b) {
  t = std::fabs(n.x) > 0.5 ? ON_CrossProduct(n, ON_3dVector::YAxis) : ON_CrossProduct(n, ON_3dVector::XAxis);
  t.Unitize();
  b = ON_CrossProduct(n, t);
}

ON_3dVector CosineSample(const ON_3dVector& n, unsigned& s) {
  const double r1 = Rnd(s), r2 = Rnd(s);
  const double phi = 2 * ON_PI * r1, r = std::sqrt(r2);
  ON_3dVector t, b;
  Basis(n, t, b);
  ON_3dVector d = t * (r * std::cos(phi)) + b * (r * std::sin(phi)) + n * std::sqrt(std::max(0.0, 1 - r2));
  d.Unitize();
  return d;
}

// lobo lucido attorno alla direzione speculare (Phong, esponente dalla rugosita')
ON_3dVector GlossySample(const ON_3dVector& m, float rough, unsigned& s) {
  if (rough < 0.01f) return m;
  const double e = 2.0 / std::max(1e-4, (double)rough * rough) - 2.0;
  const double r1 = Rnd(s), r2 = Rnd(s);
  const double ct = std::pow(r1, 1.0 / (e + 1)), st = std::sqrt(std::max(0.0, 1 - ct * ct)), phi = 2 * ON_PI * r2;
  ON_3dVector t, b;
  Basis(m, t, b);
  ON_3dVector d = t * (st * std::cos(phi)) + b * (st * std::sin(phi)) + m * ct;
  d.Unitize();
  return d;
}

ON_3dVector Reflect(const ON_3dVector& d, const ON_3dVector& n) { return d - 2 * (d * n) * n; }

int MaterialIndex(const ONX_Model& model, const ON_3dmObjectAttributes& a) {
  if (a.MaterialSource() == ON::material_from_object) return a.m_material_index;
  if (const auto* l = ON_Layer::Cast(model.ComponentFromIndex(ON_ModelComponent::Type::Layer, a.m_layer_index).ModelComponent()))
    return l->RenderMaterialIndex();
  return -1;
}

}  // namespace

ON_3fVector RenderTexture::Sample(float u, float v) const {
  if (w <= 0 || h <= 0) return ON_3fVector(1, 1, 1);
  u -= std::floor(u);
  v -= std::floor(v);
  const int x = std::min(w - 1, (int)(u * w)), y = std::min(h - 1, (int)((1 - v) * h));
  const size_t k = ((size_t)y * w + x) * 3;
  return ON_3fVector(rgb[k], rgb[k + 1], rgb[k + 2]);
}

void RenderScene::Build(Engine& e, const ui::Scene& scene, RenderOptions& opt) {
  m_tris.clear();
  m_mats.clear();
  m_textures.clear();
  m_lights.clear();
  m_box = ON_BoundingBox();
  const ONX_Model& model = e.Model();
  std::map<int, int> matOf;            // materiale del documento -> indice
  std::map<std::string, int> texOf;
  RenderMaterial def;                  // materiale predefinito di Rhino: bianco opaco
  def.albedo = ON_3fVector(0.8f, 0.8f, 0.8f);
  m_mats.push_back(def);
  for (const auto& obj : scene.Objects()) {
    if (!obj.shaded) continue;
    const auto* o = e.Object(obj.id);
    const ON_3dmObjectAttributes* a = o ? o->Attributes(nullptr) : nullptr;
    int mi = 0;
    const int dm = a ? MaterialIndex(model, *a) : -1;
    if (dm >= 0) {
      auto it = matOf.find(dm);
      if (it != matOf.end()) mi = it->second;
      else if (const auto* m = ON_Material::Cast(model.ComponentFromIndex(ON_ModelComponent::Type::RenderMaterial, dm).ModelComponent())) {
        RenderMaterial rm;
        if (m->IsPhysicallyBased()) {
          auto pb = m->PhysicallyBased();
          const ON_4fColor bc = pb->BaseColor();
          rm.albedo = ON_3fVector(bc.Red(), bc.Green(), bc.Blue());
          rm.roughness = (float)pb->Roughness();
          rm.metallic = (float)pb->Metallic();
          rm.transmission = (float)(1.0 - pb->Opacity());
          rm.ior = (float)pb->OpacityIOR();
          const ON_4fColor em = pb->Emission();
          rm.emission = ON_3fVector(em.Red(), em.Green(), em.Blue());
        } else {
          rm.albedo = Lin(m->Diffuse());
          // lucentezza 0..255 -> rugosita'; riflettivita' come metallo/specchio colorato dallo speculare
          const double shine = m->Shine() / ON_Material::MaxShine;
          rm.roughness = (float)std::clamp(1.0 - shine, 0.02, 1.0);
          rm.specular = (float)(0.04 + 0.2 * shine);
          rm.metallic = (float)std::clamp(m->Reflectivity(), 0.0, 1.0);
          rm.transmission = (float)std::clamp(m->Transparency(), 0.0, 1.0);
          rm.ior = (float)(m->m_index_of_refraction > 1.0 ? m->m_index_of_refraction : 1.5);
          rm.emission = m->Emission() != ON_Color::Black ? Lin(m->Emission()) : ON_3fVector(0, 0, 0);
        }
        mi = (int)m_mats.size();
        m_mats.push_back(rm);
        matOf[dm] = mi;
      }
    }
    // texture dell'oggetto (gia' risolta e con le coordinate calcolate dalla scena)
    if (!obj.texture.empty() && (int)obj.uv.size() == obj.shaded->m_V.Count() && imageLoader) {
      int ti;
      auto it = texOf.find(obj.texture);
      if (it != texOf.end()) ti = it->second;
      else {
        RenderTexture t;
        std::vector<unsigned char> px;
        ti = -1;
        if (imageLoader(obj.texture, t.w, t.h, px) && (int)px.size() == t.w * t.h * 3) {
          t.rgb.resize(px.size());
          for (size_t k = 0; k < px.size(); k++) t.rgb[k] = ToLinear(px[k]);
          ti = (int)m_textures.size();
          m_textures.push_back(std::move(t));
        }
        texOf[obj.texture] = ti;
      }
      if (ti >= 0) {
        RenderMaterial rm = m_mats[mi];
        rm.texture = ti;
        rm.albedo = Lin(obj.diffuse);
        mi = (int)m_mats.size();
        m_mats.push_back(rm);
      }
    }
    const ON_Mesh& m = *obj.shaded;
    const bool hasN = m.HasVertexNormals();
    const bool hasUv = (int)obj.uv.size() == m.m_V.Count();
    for (int fi = 0; fi < m.m_F.Count(); fi++) {
      const ON_MeshFace& f = m.m_F[fi];
      const int tri[2][3] = {{f.vi[0], f.vi[1], f.vi[2]}, {f.vi[0], f.vi[2], f.vi[3]}};
      for (int t = 0; t < (f.IsQuad() ? 2 : 1); t++) {
        Tri T;
        T.mat = mi;
        T.hasUv = hasUv;
        for (int k = 0; k < 3; k++) {
          const int vi = tri[t][k];
          T.p[k] = m.m_V[vi];
          m_box.Set(ON_3dPoint(T.p[k]), true);
          if (hasUv) T.uv[k] = obj.uv[vi];
        }
        ON_3fVector fn = ON_CrossProduct(T.p[1] - T.p[0], T.p[2] - T.p[0]);
        if (!fn.Unitize()) continue;
        for (int k = 0; k < 3; k++) T.n[k] = hasN ? m.m_N[tri[t][k]] : fn;
        m_tris.push_back(T);
      }
    }
  }
  // luci del modello
  for (const auto& id : e.ObjectIdsInSpace(ON_nil_uuid)) {
    const auto* o = e.Object(id);
    const auto* L = o ? ON_Light::Cast(o->Geometry(nullptr)) : nullptr;
    if (!L || !L->IsEnabled()) continue;
    RenderLight rl;
    rl.color = Lin(L->Diffuse()) * (float)L->Intensity();
    rl.pos = L->Location();
    rl.dir = L->Direction();
    rl.dir.Unitize();
    if (L->IsDirectionalLight()) rl.kind = RenderLight::Kind::Directional;
    else if (L->IsSpotLight()) {
      rl.kind = RenderLight::Kind::Spot;
      rl.cosOuter = std::cos(L->SpotAngleRadians());
      rl.cosInner = std::cos(L->SpotAngleRadians() * std::clamp(L->HotSpot(), 0.0, 1.0));
    } else if (L->IsRectangularLight()) {
      rl.kind = RenderLight::Kind::Rect;
      rl.u = L->Length();
      rl.v = L->Width();
      rl.dir = ON_CrossProduct(rl.u, rl.v);
      rl.dir.Unitize();
    }
    // le luci puntiformi di Rhino sono pensate per la scala del modello: intensita' relativa al
    // modello (1 = illumina come una luce a distanza pari alla dimensione del modello)
    if (rl.kind != RenderLight::Kind::Directional) {
      const double sz = std::max(1e-6, m_box.IsValid() ? m_box.Diagonal().Length() : 1.0);
      rl.color = rl.color * (float)(sz * sz * 0.5);
      rl.radius = 0.005 * sz;
    }
    m_lights.push_back(rl);
  }
  // sole (impostazioni di rendering)
  const ON_3dmRenderSettings& rs = model.m_settings.m_RenderSettings;
  if (rs.Sun().EnableOn()) {
    RenderLight sun;
    sun.kind = RenderLight::Kind::Directional;
    const double alt = rs.Sun().Altitude() * ON_PI / 180, az = rs.Sun().Azimuth() * ON_PI / 180, north = rs.Sun().North() * ON_PI / 180;
    const ON_3dVector toSun(std::cos(alt) * std::cos(north - az), std::cos(alt) * std::sin(north - az), std::sin(alt));
    sun.dir = -toSun;
    sun.color = ON_3fVector(1.0f, 0.96f, 0.88f) * (float)(1.3 * rs.Sun().Intensity());
    sun.radius = 0.0047;  // raggio angolare del sole
    m_lights.push_back(sun);
  }
  opt.skylight = rs.Skylight().Enabled() || m_lights.empty();
  if (m_lights.empty()) {
    // illuminazione predefinita: luce dall'alto a sinistra dietro la camera (impostata da chi chiama)
    RenderLight d;
    d.kind = RenderLight::Kind::Directional;
    d.dir = ON_3dVector(0.4, 0.6, -1);
    d.dir.Unitize();
    d.color = ON_3fVector(1.1f, 1.1f, 1.1f);
    d.radius = 0.02;
    m_lights.push_back(d);
  }
  const ON_GroundPlane& gp = rs.GroundPlane();
  if (gp.Enabled()) {
    opt.ground = true;
    opt.groundShadowOnly = gp.ShadowOnly();
    opt.groundZ = gp.AutoAltitude() && m_box.IsValid() ? m_box.m_min.z : gp.Altitude();
  }
  opt.background = Lin(rs.m_background_color);
  BuildBvh();
}

// --- BVH -----------------------------------------------------------------------------------
void RenderScene::BuildBvh() {
  m_nodes.clear();
  m_index.resize(m_tris.size());
  std::vector<ON_BoundingBox> boxes(m_tris.size());
  std::vector<ON_3dPoint> centers(m_tris.size());
  for (size_t i = 0; i < m_tris.size(); i++) {
    m_index[i] = (int)i;
    for (int k = 0; k < 3; k++) boxes[i].Set(ON_3dPoint(m_tris[i].p[k]), k > 0);
    centers[i] = boxes[i].Center();
  }
  if (!m_tris.empty()) BuildNode(0, (int)m_tris.size(), boxes, centers, 0);
}

int RenderScene::BuildNode(int first, int count, std::vector<ON_BoundingBox>& boxes, std::vector<ON_3dPoint>& centers, int depth) {
  const int ni = (int)m_nodes.size();
  m_nodes.emplace_back();
  ON_BoundingBox b, cb;
  for (int i = first; i < first + count; i++) {
    b.Union(boxes[m_index[i]]);
    cb.Set(centers[m_index[i]], true);
  }
  for (int k = 0; k < 3; k++) {
    m_nodes[ni].bmin[k] = (float)b.m_min[k] - 1e-5f;
    m_nodes[ni].bmax[k] = (float)b.m_max[k] + 1e-5f;
  }
  if (count <= 4 || depth > 40) {
    m_nodes[ni].first = first;
    m_nodes[ni].count = count;
    return ni;
  }
  const ON_3dVector ext = cb.m_max - cb.m_min;
  const int axis = ext.x > ext.y ? (ext.x > ext.z ? 0 : 2) : (ext.y > ext.z ? 1 : 2);
  const int mid = first + count / 2;
  std::nth_element(m_index.begin() + first, m_index.begin() + mid, m_index.begin() + first + count,
                   [&](int a, int c) { return centers[a][axis] < centers[c][axis]; });
  const int l = BuildNode(first, mid - first, boxes, centers, depth + 1);
  const int r = BuildNode(mid, first + count - mid, boxes, centers, depth + 1);
  m_nodes[ni].left = l;
  m_nodes[ni].right = r;
  return ni;
}

namespace {
inline bool HitBox(const float* bmin, const float* bmax, const double* o, const double* inv, double tmax) {
  double t0 = 0, t1 = tmax;
  for (int k = 0; k < 3; k++) {
    double a = (bmin[k] - o[k]) * inv[k], b = (bmax[k] - o[k]) * inv[k];
    if (a > b) std::swap(a, b);
    t0 = std::max(t0, a);
    t1 = std::min(t1, b);
    if (t0 > t1) return false;
  }
  return true;
}
}  // namespace

bool RenderScene::Intersect(const Ray& r, double tmax, double& tBest, int& triBest, double& bu, double& bv) const {
  if (m_nodes.empty()) return false;
  const double o[3] = {r.o.x, r.o.y, r.o.z};
  const double inv[3] = {1.0 / (r.d.x != 0 ? r.d.x : 1e-300), 1.0 / (r.d.y != 0 ? r.d.y : 1e-300),
                         1.0 / (r.d.z != 0 ? r.d.z : 1e-300)};
  int stack[128];
  int sp = 0;
  stack[sp++] = 0;
  bool hit = false;
  tBest = tmax;
  while (sp) {
    const Node& n = m_nodes[stack[--sp]];
    if (!HitBox(n.bmin, n.bmax, o, inv, tBest)) continue;
    if (n.left < 0) {
      for (int i = n.first; i < n.first + n.count; i++) {
        const Tri& T = m_tris[m_index[i]];
        const ON_3dVector e1 = ON_3dPoint(T.p[1]) - ON_3dPoint(T.p[0]), e2 = ON_3dPoint(T.p[2]) - ON_3dPoint(T.p[0]);
        const ON_3dVector p = ON_CrossProduct(r.d, e2);
        const double det = e1 * p;
        if (std::fabs(det) < 1e-14) continue;
        const double id = 1.0 / det;
        const ON_3dVector s = r.o - ON_3dPoint(T.p[0]);
        const double u = (s * p) * id;
        if (u < 0 || u > 1) continue;
        const ON_3dVector q = ON_CrossProduct(s, e1);
        const double v = (r.d * q) * id;
        if (v < 0 || u + v > 1) continue;
        const double t = (e2 * q) * id;
        if (t > 1e-9 && t < tBest) { tBest = t; triBest = m_index[i]; bu = u; bv = v; hit = true; }
      }
    } else if (sp < 126) {
      stack[sp++] = n.left;
      stack[sp++] = n.right;
    }
  }
  return hit;
}

bool RenderScene::Occluded(const Ray& r, double tmax) const {
  double t, u, v;
  int tri;
  return Intersect(r, tmax, t, tri, u, v);
}

// --- camera --------------------------------------------------------------------------------
bool RenderCamera::Init(const ON_3dmView& view, double aspect) {
  ON_Viewport vp = view.m_vp;
  if (aspect > 0) vp.SetFrustumAspect(aspect);
  double f;
  if (!vp.GetFrustum(&l, &r, &b, &t, &n, &f)) return false;
  loc = vp.CameraLocation();
  X = vp.CameraX();
  Y = vp.CameraY();
  Z = vp.CameraZ();
  persp = vp.IsPerspectiveProjection();
  return true;
}

Ray RenderCamera::Generate(double u, double v) const {
  const double x = l + u * (r - l), y = b + v * (t - b);
  Ray ray;
  if (persp) {
    ray.o = loc;
    ray.d = X * x + Y * y - Z * n;
  } else {
    ray.o = loc + X * x + Y * y;
    ray.d = -Z;
  }
  ray.d.Unitize();
  return ray;
}

// --- path tracer ---------------------------------------------------------------------------
PathTracer::PathTracer(const RenderScene& s, const RenderCamera& c, const RenderOptions& o) : m_scene(s), m_cam(c), m_opt(o) {
  m_accum.assign((size_t)std::max(1, o.width) * std::max(1, o.height) * 3, 0.0f);
}

ON_3fVector PathTracer::Sky(const ON_3dVector& d) const {
  const float t = (float)std::clamp(d.z * 0.5 + 0.5, 0.0, 1.0);
  return (m_opt.skyBottom * (1 - t) + m_opt.skyTop * t) * m_opt.skyIntensity;
}

ON_3fVector PathTracer::Trace(Ray ray, unsigned& seed) const {
  ON_3fVector L(0, 0, 0), thr(1, 1, 1);
  const double scale = std::max(1e-6, m_scene.m_box.IsValid() ? m_scene.m_box.Diagonal().Length() : 1.0);
  const double eps = 1e-5 * scale;
  bool diffuseSeen = false;
  for (int depth = 0; depth <= m_opt.maxBounces; depth++) {
    double t = 1e30, bu = 0, bv = 0;
    int ti = -1;
    const bool hitTri = m_scene.Intersect(ray, 1e30, t, ti, bu, bv);
    // piano di terra
    bool hitGround = false;
    if (m_opt.ground && std::fabs(ray.d.z) > 1e-9) {
      const double tg = (m_opt.groundZ - ray.o.z) / ray.d.z;
      if (tg > eps && tg < t) { t = tg; hitGround = true; }
    }
    if (!hitTri && !hitGround) {
      // sfondo visto direttamente o riflesso/rifratto; il cielo illumina solo se acceso
      if (depth == 0 || !diffuseSeen) L = L + Mul(thr, m_opt.backgroundIsSky ? Sky(ray.d) : m_opt.background);
      else if (m_opt.skylight) L = L + Mul(thr, Sky(ray.d));
      break;
    }
    const ON_3dPoint P = ray.o + ray.d * t;
    ON_3dVector N, Ng;
    RenderMaterial mat;
    if (hitGround) {
      N = Ng = ON_3dVector(0, 0, ray.d.z < 0 ? 1 : -1);
      mat.albedo = ON_3fVector(0.7f, 0.7f, 0.7f);
      if (m_opt.groundShadowOnly && depth == 0) mat.albedo = m_opt.background;
    } else {
      const auto& T = m_scene.m_tris[ti];
      const double w0 = 1 - bu - bv;
      N = ON_3dVector(T.n[0]) * w0 + ON_3dVector(T.n[1]) * bu + ON_3dVector(T.n[2]) * bv;
      Ng = ON_CrossProduct(ON_3dPoint(T.p[1]) - ON_3dPoint(T.p[0]), ON_3dPoint(T.p[2]) - ON_3dPoint(T.p[0]));
      Ng.Unitize();
      if (!N.Unitize()) N = Ng;
      if (N * Ng < 0) N = -N;  // normali della mesh girate rispetto all'ordine dei vertici
      mat = m_scene.m_mats[T.mat];
      if (mat.texture >= 0 && T.hasUv) {
        const float u = (float)(T.uv[0].x * w0 + T.uv[1].x * bu + T.uv[2].x * bv);
        const float v = (float)(T.uv[0].y * w0 + T.uv[1].y * bu + T.uv[2].y * bv);
        mat.albedo = Mul(mat.albedo, m_scene.m_textures[mat.texture].Sample(u, v));
      }
      if (MaxC(mat.emission) > 0) L = L + Mul(thr, mat.emission);
    }
    const bool entering = ray.d * Ng < 0;
    if (!entering) { N = -N; Ng = -Ng; }
    const ON_3dVector V = -ray.d;
    // scelta del lobo: vetro, riflessione speculare/metallo, diffuso
    const double cosi = std::max(0.0, N * V);
    const double F0 = mat.metallic > 0 ? 1.0 : mat.specular;
    const double fres = F0 + (1 - F0) * std::pow(1 - cosi, 5);
    const float r = Rnd(seed);
    if (mat.transmission > 0 && r < mat.transmission) {
      // dielettrico: Fresnel tra riflessione e rifrazione
      const double eta = entering ? 1.0 / mat.ior : mat.ior;
      const double c = cosi, k = 1 - eta * eta * (1 - c * c);
      const double R0 = std::pow((1 - mat.ior) / (1 + mat.ior), 2), Fr = R0 + (1 - R0) * std::pow(1 - c, 5);
      ON_3dVector d;
      if (k < 0 || Rnd(seed) < Fr) d = Reflect(ray.d, N);
      else {
        d = ray.d * eta + N * (eta * c - std::sqrt(k));
        d.Unitize();
        thr = Mul(thr, ON_3fVector(0.9f + 0.1f * mat.albedo.x, 0.9f + 0.1f * mat.albedo.y, 0.9f + 0.1f * mat.albedo.z));
      }
      ray.o = P + d * eps * 10;
      ray.d = d;
      continue;
    }
    const double pSpec = std::clamp(std::max(fres, (double)mat.metallic), 0.0, 1.0);
    if (Rnd(seed) < pSpec) {
      ON_3dVector d = GlossySample(Reflect(ray.d, N), mat.roughness, seed);
      if (d * Ng <= 0) break;
      if (mat.metallic > 0) thr = Mul(thr, mat.albedo * mat.metallic + ON_3fVector(1, 1, 1) * (1 - mat.metallic));
      ray.o = P + Ng * eps;
      ray.d = d;
      continue;
    }
    // diffuso: luci campionate direttamente (ombre) + rimbalzo coseno
    diffuseSeen = true;
    const ON_3fVector alb = mat.albedo;
    for (const auto& Lt : m_scene.Lights()) {
      ON_3dVector toL;
      double dist = 1e30;
      ON_3fVector rad = Lt.color;
      if (Lt.kind == RenderLight::Kind::Directional) {
        toL = -Lt.dir;
        if (Lt.radius > 0) {  // sole o luce morbida: direzione perturbata
          ON_3dVector a, b;
          Basis(toL, a, b);
          const double ang = Lt.radius * std::sqrt(Rnd(seed)), ph = 2 * ON_PI * Rnd(seed);
          toL = toL + a * (ang * std::cos(ph)) + b * (ang * std::sin(ph));
          toL.Unitize();
        }
      } else {
        ON_3dPoint lp = Lt.pos;
        if (Lt.kind == RenderLight::Kind::Rect) lp = Lt.pos + Lt.u * Rnd(seed) + Lt.v * Rnd(seed);
        else if (Lt.radius > 0) lp = lp + ON_3dVector(Rnd(seed) - 0.5, Rnd(seed) - 0.5, Rnd(seed) - 0.5) * (2 * Lt.radius);
        toL = lp - P;
        dist = toL.Length();
        if (dist < 1e-12) continue;
        toL = toL / dist;
        rad = rad * (float)(1.0 / (dist * dist));
        if (Lt.kind == RenderLight::Kind::Spot) {
          const double c = -(toL * Lt.dir);
          if (c < Lt.cosOuter) continue;
          if (c < Lt.cosInner && Lt.cosInner > Lt.cosOuter) rad = rad * (float)((c - Lt.cosOuter) / (Lt.cosInner - Lt.cosOuter));
        } else if (Lt.kind == RenderLight::Kind::Rect) {
          const double c = -(toL * Lt.dir);
          if (c <= 0) continue;
          rad = rad * (float)c;
        }
      }
      const double cl = N * toL;
      if (cl <= 0 || Ng * toL <= 0) continue;
      // ombra: gli oggetti trasparenti lasciano passare la luce (attenuata)
      Ray sr{P + Ng * eps, toL};
      double left = dist - 2 * eps;
      float pass = 1.0f;
      for (int k = 0; k < 8 && pass > 0.01f; k++) {
        double th, u2, v2;
        int t2;
        if (!m_scene.Intersect(sr, left, th, t2, u2, v2)) break;
        const float tr = m_scene.m_mats[m_scene.m_tris[t2].mat].transmission;
        pass *= tr * 0.92f;
        sr.o = sr.o + sr.d * (th + eps);
        left -= th + eps;
      }
      if (pass <= 0.01f) continue;
      rad = rad * pass;
      if (m_opt.ground && Lt.kind == RenderLight::Kind::Directional && toL.z < 0 && P.z > m_opt.groundZ + eps) continue;
      L = L + Mul(thr, Mul(alb, rad)) * (float)cl;
    }
    // il piano "solo ombre" non riflette il resto della scena
    if (hitGround && m_opt.groundShadowOnly) break;
    ray.o = P + Ng * eps;
    ray.d = CosineSample(N, seed);
    thr = Mul(thr, alb);
    if (depth >= 3) {  // roulette russa
      const float q = std::clamp(MaxC(thr), 0.05f, 0.95f);
      if (Rnd(seed) > q) break;
      thr = thr * (1.0f / q);
    }
  }
  // niente lucciole
  const float m = MaxC(L);
  if (m > 20) L = L * (20 / m);
  return L;
}

bool PathTracer::Pass(const std::atomic<bool>* cancel) {
  const int W = m_opt.width, H = m_opt.height;
  int nt = m_opt.threads > 0 ? m_opt.threads : (int)std::thread::hardware_concurrency();
  nt = std::clamp(nt, 1, 64);
  std::atomic<int> nextRow{0};
  std::atomic<bool> stopped{false};
  const int pass = m_passes;
  auto work = [&]() {
    for (;;) {
      const int y = nextRow++;
      if (y >= H) return;
      if (cancel && cancel->load()) { stopped = true; return; }
      for (int x = 0; x < W; x++) {
        // hash di Wang: semi indipendenti per pixel e passata (niente righe nel rumore)
        unsigned seed = (unsigned)((y * W + x) * 9781u + (unsigned)pass * 6271u + 1u);
        seed = (seed ^ 61u) ^ (seed >> 16);
        seed *= 9u;
        seed ^= seed >> 4;
        seed *= 0x27d4eb2du;
        seed ^= seed >> 15;
        if (!seed) seed = 1;
        const double u = (x + Rnd(seed)) / W, v = 1.0 - (y + Rnd(seed)) / H;
        const ON_3fVector c = Trace(m_cam.Generate(u, v), seed);
        float* a = &m_accum[((size_t)y * W + x) * 3];
        a[0] += c.x;
        a[1] += c.y;
        a[2] += c.z;
      }
    }
  };
  std::vector<std::thread> th;
  for (int i = 1; i < nt; i++) th.emplace_back(work);
  work();
  for (auto& t : th) t.join();
  if (stopped) return false;
  m_passes++;
  return true;
}

void PathTracer::Image(std::vector<unsigned char>& rgb) const {
  rgb.assign((size_t)m_opt.width * m_opt.height * 3, 255);
  if (!m_passes) return;
  const float k = m_opt.exposure / m_passes;
  for (size_t i = 0; i < rgb.size(); i++) {
    float v = m_accum[i] * k;
    v = v * (1 + v / 4) / (1 + v);  // compressione morbida delle luci alte (Reinhard esteso, bianco = 2)
    v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1 / 2.4f) - 0.055f;
    rgb[i] = (unsigned char)std::lround(std::clamp(v, 0.0f, 1.0f) * 255);
  }
}

}  // namespace cad
