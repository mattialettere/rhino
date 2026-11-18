// Composizione delle pagine (tavole e viste) e rasterizzazione delle viste ombreggiate.
#include "print/Page.h"
#include "commands/Engine.h"
#include "ui/Scene.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace cad {

// --- tavole ---------------------------------------------------------------------------------
std::vector<int> LayoutIndices(const ONX_Model& m) {
  std::vector<int> out;
  const auto& views = m.m_settings.m_views;
  for (int i = 0; i < views.Count(); i++)
    if (views[i].m_view_type == ON::view_type::page_view_type) out.push_back(i);
  // in ordine di numero di pagina, come le schede di Rhino
  std::stable_sort(out.begin(), out.end(), [&](int a, int b) {
    return views[a].m_page_settings.m_page_number < views[b].m_page_settings.m_page_number;
  });
  return out;
}

int FindLayout(const ONX_Model& m, const std::string& name) {
  const ON_wString w(name.c_str());
  for (int i : LayoutIndices(m))
    if (ON_wString::EqualOrdinal(m.m_settings.m_views[i].m_name, w, true)) return i;
  return -1;
}

int FindLayout(const ONX_Model& m, const ON_UUID& id) {
  for (int i : LayoutIndices(m))
    if (m.m_settings.m_views[i].m_vp.ViewportId() == id) return i;
  return -1;
}

bool PaperSize(const std::string& name, double& w, double& h) {
  std::string n;
  for (char c : name) n += (char)std::tolower((unsigned char)c);
  struct P { const char* n; double w, h; };
  static const P sizes[] = {{"a0", 1189, 841}, {"a1", 841, 594}, {"a2", 594, 420}, {"a3", 420, 297}, {"a4", 297, 210},
                            {"a5", 210, 148}, {"letter", 279.4, 215.9}, {"legal", 355.6, 215.9}, {"tabloid", 431.8, 279.4}};
  for (const auto& p : sizes)
    if (n == p.n) { w = p.w; h = p.h; return true; }
  return false;
}

bool IsShadedView(const ON_3dmView& v) {
  const ON_UUID& d = v.m_display_mode_id;
  return d == ON_StandardDisplayModeId::Shaded || d == ON_StandardDisplayModeId::Rendered ||
         d == ON_StandardDisplayModeId::Ghosted || d == ON_StandardDisplayModeId::XrayShade ||
         d == ON_StandardDisplayModeId::RenderedShadows || d == ON_StandardDisplayModeId::Raytraced ||
         d == ON_StandardDisplayModeId::Artistic || d == ON_StandardDisplayModeId::Monochrome ||
         d == ON_StandardDisplayModeId::AmbientOcclusion || d == ON_StandardDisplayModeId::Technical ||
         d == ON_StandardDisplayModeId::Pen;
}

void FitView(ON_3dmView& v, double wMm, double hMm, double ratio, ON::LengthUnitSystem units, const ON_BoundingBox& box) {
  ON_Viewport& vp = v.m_vp;
  if (box.IsValid()) {
    double l, r, bt, t, n, f;
    double ang = 0.25;
    if (vp.IsPerspectiveProjection() && vp.GetFrustum(&l, &r, &bt, &t, &n, &f) && n > 0)
      ang = std::atan(std::min(r - l, t - bt) / 2 / n);
    if (!(ang > 0.01 && ang < 1.5)) ang = 0.25;
    ON_BoundingBox b = box;
    const double pad = 0.05 * std::max(1e-6, b.Diagonal().Length());
    b.m_min -= ON_3dVector(pad, pad, pad);
    b.m_max += ON_3dVector(pad, pad, pad);
    vp.Extents(ang, b);
    v.SetTargetPoint(b.Center());
  }
  if (wMm > 0 && hMm > 0) vp.SetFrustumAspect(wMm / hMm);
  if (ratio > 0 && vp.IsParallelProjection() && wMm > 0 && hMm > 0) {
    double l, r, b, t, n, f;
    vp.GetFrustum(&l, &r, &b, &t, &n, &f);
    const double s = ON::UnitScale(ON::LengthUnitSystem::Millimeters, units);  // mm della pagina -> unita' del modello
    const double cx = (l + r) / 2, cy = (b + t) / 2, hw = wMm / 2 * s / ratio, hh = hMm / 2 * s / ratio;
    vp.SetFrustum(cx - hw, cx + hw, cy - hh, cy + hh, n, f);
  }
}

ON_3dmView StandardView(const std::string& projection, const ON_BoundingBox& box, double aspect) {
  std::string p;
  for (char c : projection) p += (char)std::tolower((unsigned char)c);
  ON_3dmView v;
  ON_Viewport& vp = v.m_vp;
  ON_3dVector d(0, 0, -1), up(0, 1, 0);
  const char* name = "Top";
  if (p == "bottom") { d = ON_3dVector(0, 0, 1); up = ON_3dVector(0, 1, 0); name = "Bottom"; }
  else if (p == "front") { d = ON_3dVector(0, 1, 0); up = ON_3dVector::ZAxis; name = "Front"; }
  else if (p == "back") { d = ON_3dVector(0, -1, 0); up = ON_3dVector::ZAxis; name = "Back"; }
  else if (p == "right") { d = ON_3dVector(-1, 0, 0); up = ON_3dVector::ZAxis; name = "Right"; }
  else if (p == "left") { d = ON_3dVector(1, 0, 0); up = ON_3dVector::ZAxis; name = "Left"; }
  const bool persp = p == "perspective";
  if (persp) {
    const double yaw = -ON_PI / 4, pitch = 0.46;
    d = -ON_3dVector(std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch));
    ON_3dVector r = ON_CrossProduct(d, ON_3dVector::ZAxis);
    r.Unitize();
    up = ON_CrossProduct(r, d);
    up.Unitize();
    name = "Perspective";
    vp.ChangeToPerspectiveProjection(ON_UNSET_VALUE, true, 15.0 * ON_PI / 180);
  } else {
    vp.ChangeToParallelProjection(true);
  }
  ON_BoundingBox b = box.IsValid() ? box : ON_BoundingBox(ON_3dPoint(-50, -50, 0), ON_3dPoint(50, 50, 0));
  const double dist = std::max(10.0, b.Diagonal().Length() * 3);
  vp.SetCameraLocation(b.Center() - d * dist);
  vp.SetCameraDirection(d);
  vp.SetCameraUp(up);
  const double t = persp ? 0.1 * std::tan(15.0 * ON_PI / 180) : b.Diagonal().Length() / 2;
  vp.SetFrustum(-t * aspect, t * aspect, -t, t, persp ? 0.1 : 1e-3, dist * 10);
  v.m_name = ON_wString(name);
  v.m_display_mode_id = persp ? ON_StandardDisplayModeId::Shaded : ON_StandardDisplayModeId::Wireframe;
  ON_Plane cp = ON_Plane::World_xy;
  if (p == "front" || p == "back") cp = ON_Plane(ON_3dPoint::Origin, ON_3dVector::XAxis, ON_3dVector::ZAxis);
  else if (p == "right" || p == "left") cp = ON_Plane(ON_3dPoint::Origin, ON_3dVector::YAxis, ON_3dVector::ZAxis);
  v.m_cplane = ON_3dmConstructionPlane(cp);
  FitView(v, aspect, 1, 0, ON::LengthUnitSystem::Millimeters, b);
  return v;
}

// --- proiezione e ritaglio -------------------------------------------------------------------
namespace {

struct Projector {
  ON_Xform toCamera, toClip;
  double nearZ = 0;
  double x0, y0, x1, y1;
  bool persp = false;
  bool Init(const ON_3dmView& v, double ax0, double ay0, double ax1, double ay1) {
    x0 = ax0; y0 = ay0; x1 = ax1; y1 = ay1;
    ON_Viewport vp = v.m_vp;
    persp = vp.IsPerspectiveProjection();
    if (!vp.GetXform(ON::coordinate_system::world_cs, ON::coordinate_system::camera_cs, toCamera)) return false;
    if (!vp.GetXform(ON::coordinate_system::camera_cs, ON::coordinate_system::clip_cs, toClip)) return false;
    double l, r, b, t, n, f;
    vp.GetFrustum(&l, &r, &b, &t, &n, &f);
    nearZ = n;
    return true;
  }
  // camera: z negativa davanti all'occhio
  ON_3dPoint Cam(const ON_3dPoint& p) const { return toCamera * p; }
  ON_2dPoint Page(const ON_3dPoint& cam) const {
    const ON_3dPoint c = toClip * cam;
    return ON_2dPoint(x0 + (c.x + 1) / 2 * (x1 - x0), y0 + (c.y + 1) / 2 * (y1 - y0));
  }
  double Depth(const ON_3dPoint& cam) const { return -cam.z; }
  // Segmento in coordinate camera tagliato al piano vicino (solo prospettiva)
  bool ClipNear(ON_3dPoint& a, ON_3dPoint& b) const {
    if (!persp) return true;
    const double za = -a.z, zb = -b.z, zn = nearZ;
    if (za < zn && zb < zn) return false;
    if (za < zn) a = a + (b - a) * ((zn - za) / (zb - za));
    else if (zb < zn) b = b + (a - b) * ((zn - zb) / (za - zb));
    return true;
  }
};

// Liang-Barsky nel rettangolo
bool ClipRect(ON_2dPoint& a, ON_2dPoint& b, double x0, double y0, double x1, double y1) {
  double t0 = 0, t1 = 1;
  const double dx = b.x - a.x, dy = b.y - a.y;
  const double p[4] = {-dx, dx, -dy, dy}, q[4] = {a.x - x0, x1 - a.x, a.y - y0, y1 - a.y};
  for (int i = 0; i < 4; i++) {
    if (p[i] == 0) { if (q[i] < 0) return false; continue; }
    const double r = q[i] / p[i];
    if (p[i] < 0) { if (r > t1) return false; if (r > t0) t0 = r; }
    else { if (r < t0) return false; if (r < t1) t1 = r; }
  }
  const ON_2dPoint a0 = a;
  a = ON_2dPoint(a0.x + t0 * dx, a0.y + t0 * dy);
  b = ON_2dPoint(a0.x + t1 * dx, a0.y + t1 * dy);
  return true;
}

// Z-buffer delle viste ombreggiate: serve anche a nascondere le linee coperte
struct Raster {
  int w = 0, h = 0;
  std::vector<float> z;
  std::vector<unsigned char> rgb;
  bool Init(int W, int H) {
    if (W <= 0 || H <= 0 || (double)W * H > 40e6) return false;
    w = W; h = H;
    z.assign((size_t)w * h, std::numeric_limits<float>::infinity());
    rgb.assign((size_t)w * h * 3, 255);
    return true;
  }
  // punto pagina -> pixel (riga 0 in alto)
  double px(double x, double x0, double x1) const { return (x - x0) / (x1 - x0) * w; }
  double py(double y, double y0, double y1) const { return (y1 - y) / (y1 - y0) * h; }
  void Tri(const double* X, const double* Y, const double* Z, const double* I, const ON_Color& c) {
    const double area = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0]);
    if (std::fabs(area) < 1e-12) return;
    const int minx = std::max(0, (int)std::floor(std::min({X[0], X[1], X[2]})));
    const int maxx = std::min(w - 1, (int)std::ceil(std::max({X[0], X[1], X[2]})));
    const int miny = std::max(0, (int)std::floor(std::min({Y[0], Y[1], Y[2]})));
    const int maxy = std::min(h - 1, (int)std::ceil(std::max({Y[0], Y[1], Y[2]})));
    for (int y = miny; y <= maxy; y++)
      for (int x = minx; x <= maxx; x++) {
        const double sx = x + 0.5, sy = y + 0.5;
        double l0 = ((X[1] - sx) * (Y[2] - sy) - (X[2] - sx) * (Y[1] - sy)) / area;
        double l1 = ((X[2] - sx) * (Y[0] - sy) - (X[0] - sx) * (Y[2] - sy)) / area;
        double l2 = 1 - l0 - l1;
        if (l0 < 0 || l1 < 0 || l2 < 0) continue;
        const float zz = (float)(l0 * Z[0] + l1 * Z[1] + l2 * Z[2]);
        const size_t k = (size_t)y * w + x;
        if (zz >= z[k]) continue;
        z[k] = zz;
        const double i = std::clamp(l0 * I[0] + l1 * I[1] + l2 * I[2], 0.0, 1.0);
        rgb[k * 3 + 0] = (unsigned char)std::lround(c.Red() * i);
        rgb[k * 3 + 1] = (unsigned char)std::lround(c.Green() * i);
        rgb[k * 3 + 2] = (unsigned char)std::lround(c.Blue() * i);
      }
  }
  // Visibile se non c'e' una superficie piu' vicina (con un margine relativo)
  bool Visible(double X, double Y, double Z, double eps) const {
    const int x = (int)X, y = (int)Y;
    if (x < 0 || y < 0 || x >= w || y >= h) return true;
    float best = std::numeric_limits<float>::infinity();
    for (int dy = -1; dy <= 1; dy++)
      for (int dx = -1; dx <= 1; dx++) {
        const int xx = x + dx, yy = y + dy;
        if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
        best = std::min(best, z[(size_t)yy * w + xx]);
      }
    return Z <= best + eps;
  }
};

ON_Color Ink(const ON_Color& c, const PrintOptions& o) {
  if (o.blackWhite) return ON_Color::Black;
  // il bianco non si vede sulla carta: come Rhino, si stampa nero
  if (c.Red() > 245 && c.Green() > 245 && c.Blue() > 245) return ON_Color::Black;
  return c;
}

double LayerWidth(const ONX_Model& m, const ON_UUID& id, const PrintOptions& o) {
  const auto* obj = ON_ModelGeometryComponent::Cast(m.ComponentFromId(ON_ModelComponent::Type::ModelGeometry, id).ModelComponent());
  const ON_3dmObjectAttributes* a = obj ? obj->Attributes(nullptr) : nullptr;
  if (!a) return o.defaultWidthMm;
  if (a->PlotWeightSource() == ON::plot_weight_source::plot_weight_from_object && a->m_plot_weight_mm != 0)
    return a->m_plot_weight_mm;
  const auto* l = ON_Layer::Cast(m.ComponentFromIndex(ON_ModelComponent::Type::Layer, a->m_layer_index).ModelComponent());
  if (l && l->PlotWeight() != 0) return l->PlotWeight();
  return o.defaultWidthMm;
}

void AddText(const ui::Scene*, const AnnotationLabel& L, const Projector& P, const ON_Color& c, PageDrawing& out) {
  // testo visto di taglio: non si stampa (come Rhino, che lo mostra come una linea)
  if (!L.dot) {
    ON_3dVector n = ON_CrossProduct(L.xdir, L.ydir);
    ON_3dVector d = P.toCamera.Inverse() * ON_3dVector(0, 0, -1);  // direzione di vista nel mondo
    if (n.Unitize() && d.Unitize() && std::fabs(n * d) < 0.15) return;
  }
  ON_3dPoint a = P.Cam(L.origin), b = P.Cam(L.origin + L.ydir * L.height), x = P.Cam(L.origin + L.xdir * L.height);
  if (P.persp && (P.Depth(a) < P.nearZ || P.Depth(b) < P.nearZ)) return;
  const ON_2dPoint pa = P.Page(a), pb = P.Page(b), px = P.Page(x);
  const double hgt = pa.DistanceTo(pb);
  if (!(hgt > 0.05)) return;
  if (pa.x < P.x0 || pa.x > P.x1 || pa.y < P.y0 || pa.y > P.y1) return;
  const double ang = std::atan2(px.y - pa.y, px.x - pa.x);
  // righe multiple: una riga alla volta, distanza 1.6 x altezza (come gli stili di Rhino)
  size_t start = 0;
  int line = 0;
  const std::string& s = L.text;
  while (start <= s.size()) {
    size_t end = s.find('\n', start);
    if (end == std::string::npos) end = s.size();
    PageText t;
    t.text = s.substr(start, end - start);
    t.heightMm = hgt;
    t.angle = ang;
    t.halign = L.halign;
    t.color = c;
    const double off = line * 1.6 * hgt;
    t.pos = ON_2dPoint(pa.x + std::sin(ang) * off, pa.y - std::cos(ang) * off);
    if (!t.text.empty()) out.texts.push_back(t);
    start = end + 1;
    line++;
  }
}

}  // namespace

void DrawModelInRect(Engine& e, const ui::Scene& scene, const ON_3dmView& view, double x0, double y0, double x1,
                     double y1, double ratio, const PrintOptions& o, PageDrawing& out) {
  if (!(x1 > x0 && y1 > y0)) return;
  ON_3dmView v = view;
  FitView(v, x1 - x0, y1 - y0, ratio, e.Model().m_settings.m_ModelUnitsAndTolerances.m_unit_system.UnitSystem(),
          ON_BoundingBox::EmptyBoundingBox);
  Projector P;
  if (!P.Init(v, x0, y0, x1, y1)) return;
  const ON_3dVector viewDir = v.m_vp.CameraDirection();
  const ON_3dPoint eye = v.m_vp.CameraLocation();

  // vista ombreggiata: immagine con z-buffer
  Raster R;
  const bool shaded = IsShadedView(v);
  if (shaded) {
    const int W = std::max(8, (int)std::lround((x1 - x0) / 25.4 * o.dpi));
    const int H = std::max(8, (int)std::lround((y1 - y0) / 25.4 * o.dpi));
    if (R.Init(W, H)) {
      // luce dalla camera, come la vista ombreggiata della finestra
      ON_3dVector L = v.m_vp.CameraX() * 0.3 + v.m_vp.CameraY() * 0.5 + v.m_vp.CameraZ() * 1.0;
      L.Unitize();
      for (const auto& obj : scene.Objects()) {
        if (!obj.shaded) continue;
        const ON_Mesh& m = *obj.shaded;
        const bool hasN = m.HasVertexNormals();
        const ON_Color base = o.blackWhite ? ON_Color(255, 255, 255) : ON_Color(209, 209, 209);
        std::vector<double> X(m.m_V.Count()), Y(m.m_V.Count()), Z(m.m_V.Count()), I(m.m_V.Count());
        std::vector<char> ok(m.m_V.Count());
        for (int i = 0; i < m.m_V.Count(); i++) {
          const ON_3dPoint c = P.Cam(ON_3dPoint(m.m_V[i]));
          ok[i] = !P.persp || P.Depth(c) > P.nearZ;
          if (!ok[i]) continue;
          const ON_2dPoint pp = P.Page(c);
          X[i] = R.px(pp.x, x0, x1);
          Y[i] = R.py(pp.y, y0, y1);
          Z[i] = P.Depth(c);
          double li = 1.0;
          if (hasN) {
            ON_3dVector n(m.m_N[i]);
            li = 0.25 + 0.8 * std::fabs(n * L);  // due facce illuminate
          }
          I[i] = std::min(1.0, li);
        }
        for (int fi = 0; fi < m.m_F.Count(); fi++) {
          const ON_MeshFace& f = m.m_F[fi];
          double li = 1.0;
          if (!hasN) {
            ON_3dVector n = ON_CrossProduct(ON_3dPoint(m.m_V[f.vi[1]]) - ON_3dPoint(m.m_V[f.vi[0]]),
                                            ON_3dPoint(m.m_V[f.vi[2]]) - ON_3dPoint(m.m_V[f.vi[0]]));
            if (n.Unitize()) li = std::min(1.0, 0.25 + 0.8 * std::fabs(n * L));
          }
          const int tri[2][3] = {{f.vi[0], f.vi[1], f.vi[2]}, {f.vi[0], f.vi[2], f.vi[3]}};
          for (int t = 0; t < (f.IsQuad() ? 2 : 1); t++) {
            const int* k = tri[t];
            if (!ok[k[0]] || !ok[k[1]] || !ok[k[2]]) continue;
            const double xs[3] = {X[k[0]], X[k[1]], X[k[2]]}, ys[3] = {Y[k[0]], Y[k[1]], Y[k[2]]};
            const double zs[3] = {Z[k[0]], Z[k[1]], Z[k[2]]};
            const double is[3] = {hasN ? I[k[0]] : li, hasN ? I[k[1]] : li, hasN ? I[k[2]] : li};
            R.Tri(xs, ys, zs, is, base);
          }
        }
      }
      PageImage img;
      img.x = x0; img.y = y0; img.w = x1 - x0; img.h = y1 - y0;
      img.pw = R.w; img.ph = R.h;
      img.rgb = R.rgb;
      out.images.push_back(std::move(img));
    }
  }
  // margine di profondita' per le linee sulle superfici
  double depthEps = 1e-6;
  {
    const ON_BoundingBox b = scene.Bounds();
    if (b.IsValid()) depthEps = 2e-3 * b.Diagonal().Length();
  }

  auto addPolyline = [&](const std::vector<ON_3dPoint>& pts, const ON_Color& col, double wmm) {
    PagePolyline cur;
    cur.color = col;
    cur.widthMm = wmm;
    auto flush = [&]() {
      if (cur.pts.size() >= 2) out.lines.push_back(cur);
      cur.pts.clear();
    };
    for (size_t i = 0; i + 1 < pts.size(); i++) {
      ON_3dPoint a = P.Cam(pts[i]), b = P.Cam(pts[i + 1]);
      if (!P.ClipNear(a, b)) { flush(); continue; }
      // con lo z-buffer il segmento si spezza nelle parti visibili
      const int steps = R.w ? std::clamp((int)(P.Page(a).DistanceTo(P.Page(b)) / (x1 - x0) * R.w / 2), 1, 400) : 1;
      for (int s = 0; s < steps; s++) {
        const ON_3dPoint ca = a + (b - a) * (double(s) / steps), cb = a + (b - a) * (double(s + 1) / steps);
        ON_2dPoint pa = P.Page(ca), pb = P.Page(cb);
        if (R.w) {
          const ON_3dPoint mid = (ca + cb) * 0.5;
          const ON_2dPoint pm = P.Page(mid);
          if (!R.Visible(R.px(pm.x, x0, x1), R.py(pm.y, y0, y1), P.Depth(mid), depthEps)) { flush(); continue; }
        }
        const ON_2dPoint oa = pa;
        if (!ClipRect(pa, pb, x0, y0, x1, y1)) { flush(); continue; }
        if (cur.pts.empty() || cur.pts.back().DistanceTo(pa) > 1e-9 || oa.DistanceTo(pa) > 1e-9) {
          flush();
          cur.pts.push_back(pa);
        }
        cur.pts.push_back(pb);
        if (pb.DistanceTo(P.Page(cb)) > 1e-9) flush();  // uscito dal riquadro
      }
    }
    flush();
  };

  const ONX_Model& model = e.Model();
  for (const auto& obj : scene.Objects()) {
    const ON_Color col = Ink(obj.color, o);
    const double wmm = LayerWidth(model, obj.id, o);
    for (const auto& w : obj.wires) addPolyline(w, col, wmm);
    if (!shaded)
      for (const auto& w : obj.isocurves) addPolyline(w, col, std::min(wmm, 0.13));
    for (const auto& se : obj.silhouette) {
      const ON_3dVector dd = P.persp ? ON_3dVector(se.a - eye) : viewDir;
      if ((se.n1 * dd) * (se.n2 * dd) <= 0) addPolyline({se.a, se.b}, col, wmm);
    }
    for (const auto& p : obj.points) {
      ON_3dPoint c = P.Cam(p);
      if (P.persp && P.Depth(c) < P.nearZ) continue;
      const ON_2dPoint q = P.Page(c);
      if (q.x < x0 || q.x > x1 || q.y < y0 || q.y > y1) continue;
      const double r = 0.6;
      out.lines.push_back({{ON_2dPoint(q.x - r, q.y), ON_2dPoint(q.x + r, q.y)}, col, wmm});
      out.lines.push_back({{ON_2dPoint(q.x, q.y - r), ON_2dPoint(q.x, q.y + r)}, col, wmm});
    }
    for (size_t i = 0; i + 2 < obj.triangles.size(); i += 3) {
      PageFill f;
      f.color = col;
      bool ok = true;
      for (int k = 0; k < 3; k++) {
        ON_3dPoint c = P.Cam(obj.triangles[i + k]);
        if (P.persp && P.Depth(c) < P.nearZ) ok = false;
        const ON_2dPoint q = P.Page(c);
        if (q.x < x0 || q.x > x1 || q.y < y0 || q.y > y1) ok = false;
        f.pts.push_back(q);
      }
      if (ok) out.fills.push_back(f);
    }
    for (const auto& L : obj.labels) AddText(&scene, L, P, col, out);
  }
}

bool ComposeLayout(Engine& e, int li, const PrintOptions& o, PageDrawing& out, const ui::Scene* modelScene) {
  const ONX_Model& m = e.Model();
  if (li < 0 || li >= m.m_settings.m_views.Count()) return false;
  const ON_3dmView& L = m.m_settings.m_views[li];
  out = PageDrawing();
  out.name = ToUtf8(L.m_name);
  out.widthMm = L.m_page_settings.m_width_mm > 0 ? L.m_page_settings.m_width_mm : 297;
  out.heightMm = L.m_page_settings.m_height_mm > 0 ? L.m_page_settings.m_height_mm : 210;
  const ON_UUID lid = L.m_vp.ViewportId();
  ui::Scene own(e);
  if (!modelScene) {
    own.fixedSpace = true;
    own.Rebuild();
  }
  const ui::Scene& model = modelScene ? *modelScene : own;
  ui::Scene page(e);
  page.fixedSpace = true;
  page.space = lid;
  page.Rebuild();
  // dettagli: il modello visto dalla vista del dettaglio, ritagliato al riquadro
  for (const auto& id : e.ObjectIdsInSpace(lid)) {
    const auto* obj = e.Object(id);
    const auto* d = obj ? ON_DetailView::Cast(obj->Geometry(nullptr)) : nullptr;
    if (!d) continue;
    ON_BoundingBox bb = d->m_boundary.BoundingBox();
    if (!bb.IsValid()) continue;
    DrawModelInRect(e, model, d->m_view, bb.m_min.x, bb.m_min.y, bb.m_max.x, bb.m_max.y, d->m_page_per_model_ratio, o, out);
  }
  // oggetti della tavola (testi, cornici, bordi dei dettagli) sopra i dettagli
  ON_3dmView top = StandardView("Top", ON_BoundingBox(ON_3dPoint(0, 0, 0), ON_3dPoint(out.widthMm, out.heightMm, 0)), 1);
  {
    ON_Viewport& vp = top.m_vp;
    vp.SetCameraLocation(ON_3dPoint(out.widthMm / 2, out.heightMm / 2, 1000));
    vp.SetCameraDirection(-ON_3dVector::ZAxis);
    vp.SetCameraUp(ON_3dVector::YAxis);
    vp.SetFrustum(-out.widthMm / 2, out.widthMm / 2, -out.heightMm / 2, out.heightMm / 2, 1, 1e6);
    top.m_display_mode_id = ON_StandardDisplayModeId::Wireframe;
  }
  PrintOptions po = o;
  DrawModelInRect(e, page, top, 0, 0, out.widthMm, out.heightMm, 0, po, out);
  return true;
}

bool ComposeView(Engine& e, const ON_3dmView& view, double wMm, double hMm, const PrintOptions& o, PageDrawing& out) {
  out = PageDrawing();
  out.name = ToUtf8(view.m_name);
  out.widthMm = wMm;
  out.heightMm = hMm;
  ui::Scene model(e);
  model.fixedSpace = true;
  model.Rebuild();
  const double mg = 10;
  DrawModelInRect(e, model, view, mg, mg, wMm - mg, hMm - mg, 0, o, out);
  return true;
}

}  // namespace cad
