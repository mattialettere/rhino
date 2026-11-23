#include "ui/Viewport.h"
#include "render/Render.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <algorithm>
#include <memory>
#include <QMenu>
#include <QMouseEvent>
#include <QTouchEvent>
#include <QPainterPath>
#include <QPointingDevice>
#include <QOpenGLFunctions_1_1>
#include <QImage>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include "ui/TexturePanel.h"
#include <QOpenGLVersionFunctionsFactory>
#include <QPainter>
#include <QWheelEvent>
#include <cmath>

namespace cad::ui {

// ------------------------------------------------------------------ camera
const char* KindName(Camera::Kind k) {
  switch (k) {
    case Camera::Kind::Top: return "Top";
    case Camera::Kind::Front: return "Front";
    case Camera::Kind::Right: return "Right";
    case Camera::Kind::Perspective: return "Perspective";
    case Camera::Kind::Bottom: return "Bottom";
    case Camera::Kind::Left: return "Left";
    case Camera::Kind::Back: return "Back";
  }
  return "";
}

ON_3dVector Camera::Dir() const {
  switch (kind) {
    case Kind::Top: return ON_3dVector(0, 0, -1);
    case Kind::Front: return ON_3dVector(0, 1, 0);
    case Kind::Right: return ON_3dVector(-1, 0, 0);
    case Kind::Bottom: return ON_3dVector(0, 0, 1);
    case Kind::Left: return ON_3dVector(1, 0, 0);
    case Kind::Back: return ON_3dVector(0, -1, 0);
    case Kind::Perspective: break;
  }
  return -ON_3dVector(std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch));
}

ON_3dVector Camera::Up() const {
  if (kind == Kind::Top || kind == Kind::Bottom) return ON_3dVector(0, 1, 0);
  if (kind != Kind::Perspective) return ON_3dVector(0, 0, 1);
  ON_3dVector u = ON_CrossProduct(Right(), Dir());
  u.Unitize();
  return u;
}

ON_3dVector Camera::Right() const {
  if (kind == Kind::Top || kind == Kind::Front) return ON_3dVector(1, 0, 0);
  if (kind == Kind::Bottom || kind == Kind::Back) return ON_3dVector(-1, 0, 0);
  if (kind == Kind::Right) return ON_3dVector(0, 1, 0);
  if (kind == Kind::Left) return ON_3dVector(0, -1, 0);
  ON_3dVector r = ON_CrossProduct(Dir(), ON_3dVector::ZAxis);
  if (!r.Unitize()) r = ON_3dVector::XAxis;
  return r;
}

ON_3dPoint Camera::Eye() const {
  const double d = (kind == Kind::Perspective) ? distance : 1e5;
  return target - Dir() * d;
}

ON_Plane Camera::CPlane() const {
  if (customCPlane) return cplane;
  switch (kind) {
    case Kind::Front: case Kind::Back: return ON_Plane(ON_3dPoint::Origin, ON_3dVector::XAxis, ON_3dVector::ZAxis);
    case Kind::Right: case Kind::Left: return ON_Plane(ON_3dPoint::Origin, ON_3dVector::YAxis, ON_3dVector::ZAxis);
    default: return ON_Plane::World_xy;
  }
}

double Camera::UnitsPerPixel(int h) const {
  if (kind == Kind::Perspective) return 2.0 * distance * std::tan(fovY / 2.0) / std::max(1, h);
  return 2.0 * halfHeight / std::max(1, h);
}

bool Camera::Project(const ON_3dPoint& p, int w, int h, double& sx, double& sy) const {
  const ON_3dVector v = p - Eye();
  const double x = v * Right(), y = v * Up(), z = v * Dir();
  const double aspect = double(w) / std::max(1, h);
  double nx, ny;
  if (kind == Kind::Perspective) {
    if (z <= distance * 1e-3) return false;
    const double t = std::tan(fovY / 2.0);
    nx = x / (z * t * aspect);
    ny = y / (z * t);
  } else {
    nx = x / (halfHeight * aspect);
    ny = y / halfHeight;
  }
  sx = (nx + 1.0) * 0.5 * w;
  sy = (1.0 - ny) * 0.5 * h;
  return true;
}

void Camera::Ray(double sx, double sy, int w, int h, ON_3dPoint& origin, ON_3dVector& dir) const {
  const double aspect = double(w) / std::max(1, h);
  const double nx = 2.0 * sx / w - 1.0, ny = 1.0 - 2.0 * sy / h;
  if (kind == Kind::Perspective) {
    const double t = std::tan(fovY / 2.0);
    origin = Eye();
    dir = Dir() + Right() * (nx * t * aspect) + Up() * (ny * t);
    dir.Unitize();
  } else {
    origin = Eye() + Right() * (nx * halfHeight * aspect) + Up() * (ny * halfHeight);
    dir = Dir();
  }
}

void Camera::ZoomExtents(const ON_BoundingBox& b, int w, int h) {
  if (!b.IsValid()) return;
  target = b.Center();
  const double r = std::max(b.Diagonal().Length() / 2.0, 1.0);
  if (kind == Kind::Perspective) {
    distance = r / std::sin(fovY / 2.0) * 1.1;
  } else {
    // dimensioni proiettate nella vista
    double maxX = 0, maxY = 0;
    for (int i = 0; i < 8; i++) {
      ON_3dVector v = b.Corner(i & 1, (i >> 1) & 1, (i >> 2) & 1) - target;
      maxX = std::max(maxX, std::fabs(v * Right()));
      maxY = std::max(maxY, std::fabs(v * Up()));
    }
    const double aspect = double(w) / std::max(1, h);
    halfHeight = std::max({maxY, maxX / aspect, 1.0}) * 1.15;
  }
}

// ------------------------------------------------------------------ viewport
Viewport::Settings& Viewport::Shared() { static Settings s; return s; }

Viewport::Viewport(Scene& scene, Camera::Kind kind, QWidget* parent) : QOpenGLWidget(parent), m_scene(scene) {
  m_cam.kind = kind;
  mode = (kind == Camera::Kind::Perspective) ? DisplayMode::Shaded : DisplayMode::Wireframe;
  setMouseTracking(true);
  setFocusPolicy(Qt::ClickFocus);
  setMinimumSize(120, 90);
  setAcceptDrops(true);
  setAttribute(Qt::WA_AcceptTouchEvents);  // schermi touch: vedi TouchEvent
}

namespace {
QString DroppedTexture(const QMimeData* md) {
  if (md->hasFormat(TexturePanel::MimeType())) return QString::fromUtf8(md->data(TexturePanel::MimeType()));
  for (const QUrl& u : md->urls())
    if (u.isLocalFile() && TexturePanel::IsImageFile(u.toLocalFile())) return u.toLocalFile();
  return {};
}
}  // namespace

void Viewport::dragEnterEvent(QDragEnterEvent* e) {
  if (!DroppedTexture(e->mimeData()).isEmpty()) e->acceptProposedAction();
}

void Viewport::dragMoveEvent(QDragMoveEvent* e) {
  // si accetta solo sopra un oggetto
  if (!PickAt(e->position().toPoint()).empty()) e->acceptProposedAction();
  else e->ignore();
}

void Viewport::dropEvent(QDropEvent* e) {
  const QString f = DroppedTexture(e->mimeData());
  const auto ids = PickAt(e->position().toPoint());
  if (f.isEmpty() || ids.empty()) return;
  e->acceptProposedAction();
  emit TextureDropped(f, ids);
}

void Viewport::ZoomBox(const ON_BoundingBox& b) {
  m_cam.ZoomExtents(b, width(), height());
  update();
}

void Viewport::ZoomExtents() {
  m_cam.ZoomExtents(m_scene.Bounds(), width(), height());
  update();
}

void Viewport::SetView(Camera::Kind k) {
  m_cam.kind = k;
  ZoomExtents();
}

// Menu del titolo della vista (come Rhino 8: clic sul nome della vista)
void Viewport::ShowViewMenu(const QPoint& at) {
  QMenu menu(this);
  auto* view = menu.addMenu("Imposta vista");
  const std::pair<const char*, Camera::Kind> views[] = {
      {"Superiore (Top)", Camera::Kind::Top}, {"Inferiore (Bottom)", Camera::Kind::Bottom},
      {"Sinistra (Left)", Camera::Kind::Left}, {"Destra (Right)", Camera::Kind::Right},
      {"Frontale (Front)", Camera::Kind::Front}, {"Posteriore (Back)", Camera::Kind::Back},
      {"Prospettica (Perspective)", Camera::Kind::Perspective}};
  for (auto& [label, k] : views) {
    auto* a = view->addAction(label);
    a->setCheckable(true);
    a->setChecked(m_cam.kind == k);
    connect(a, &QAction::triggered, this, [this, k = k] { SetView(k); });
  }
  menu.addSeparator();
  const std::pair<const char*, DisplayMode> modes[] = {
      {"Wireframe", DisplayMode::Wireframe}, {"Ombreggiato (Shaded)", DisplayMode::Shaded},
      {"Fantasma (Ghosted)", DisplayMode::Ghosted}, {"Raggi X (X-Ray)", DisplayMode::XRay},
      {"Renderizzato (Rendered)", DisplayMode::Rendered}, {"Raytraced", DisplayMode::Raytraced}};
  for (auto& [label, m] : modes) {
    auto* a = menu.addAction(label);
    a->setCheckable(true);
    a->setChecked(mode == m);
    connect(a, &QAction::triggered, this, [this, m = m] { mode = m; update(); });
  }
  menu.addSeparator();
  connect(menu.addAction("Zoom estensione"), &QAction::triggered, this, [this] { ZoomExtents(); });
  connect(menu.addAction("Ingrandisci / ripristina"), &QAction::triggered, this, [this] { emit ToggleMaximize(this); });
  menu.exec(mapToGlobal(at));
}

namespace {
QOpenGLFunctions_1_1* GL(QOpenGLContext* ctx);
}

// --- Raytraced -------------------------------------------------------------------------------
struct Viewport::Raytrace {
  std::thread th;
  std::atomic<bool> cancel{false};
  std::mutex mx;
  std::vector<unsigned char> img;
  int w = 0, h = 0, passes = 0;
  std::string sig;
  ~Raytrace() {
    cancel = true;
    if (th.joinable()) th.join();
  }
};

Viewport::~Viewport() { StopRaytrace(); }

void Viewport::StopRaytrace() {
  if (!m_rt) return;
  m_rt->cancel = true;
  if (m_rt->th.joinable()) m_rt->th.join();
  m_rt.reset();
}

void Viewport::UpdateRaytrace() {
  const int W = std::max(8, width()), H = std::max(8, height());
  const ON_3dmView v = ToView();
  char buf[512];
  const ON_3dPoint e = v.m_vp.CameraLocation();
  const ON_3dVector d = v.m_vp.CameraDirection();
  double l, r, b, t, n, f;
  v.m_vp.GetFrustum(&l, &r, &b, &t, &n, &f);
  std::snprintf(buf, sizeof buf, "%d %d %d|%.9g %.9g %.9g|%.9g %.9g %.9g|%.9g %.9g", m_scene.Version(), W, H, e.x, e.y, e.z, d.x,
                d.y, d.z, r - l, t - b);
  if (m_rt && m_rt->sig == buf) return;
  auto rt = std::make_shared<Raytrace>();
  rt->sig = buf;
  rt->w = W;
  rt->h = H;
  if (m_rt && m_rt->w == W && m_rt->h == H) {  // l'immagine precedente resta finche' arriva la nuova
    std::lock_guard<std::mutex> lk(m_rt->mx);
    rt->img = m_rt->img;
  }
  StopRaytrace();
  auto scene = std::make_shared<RenderScene>();
  RenderOptions ro;
  ro.width = W;
  ro.height = H;
  ro.threads = std::max(1, (int)std::thread::hardware_concurrency() - 1);  // un core resta all'interfaccia
  scene->Build(m_scene.GetEngine(), m_scene, ro);
  RenderCamera cam;
  cam.Init(v, double(W) / H);
  Viewport* self = this;
  Raytrace* R = rt.get();
  rt->th = std::thread([R, scene, cam, ro, self]() {
    PathTracer pt(*scene, cam, ro);
    for (int i = 0; i < 1000 && !R->cancel; i++) {
      if (!pt.Pass(&R->cancel)) break;
      std::vector<unsigned char> img;
      pt.Image(img);
      {
        std::lock_guard<std::mutex> lk(R->mx);
        R->img.swap(img);
        R->passes = pt.Passes();
      }
      QMetaObject::invokeMethod(self, [self] { self->update(); }, Qt::QueuedConnection);
    }
  });
  m_rt = rt;
}

// --- Renderizzato: luci del documento (o la luce predefinita) -----------------------------------
namespace {
struct DocLights {
  std::vector<const ON_Light*> lights;
  bool sun = false;
  ON_3dVector sunDir;
  double sunIntensity = 1;
};
DocLights CollectLights(Engine& e) {
  DocLights L;
  for (const auto& id : e.ObjectIdsInSpace(ON_nil_uuid))
    if (const auto* o = e.Object(id))
      if (const auto* l = ON_Light::Cast(o->Geometry(nullptr)); l && l->IsEnabled()) L.lights.push_back(l);
  const auto& rs = e.Model().m_settings.m_RenderSettings;
  if (rs.Sun().EnableOn()) {
    const double alt = rs.Sun().Altitude() * ON_PI / 180, az = rs.Sun().Azimuth() * ON_PI / 180, north = rs.Sun().North() * ON_PI / 180;
    L.sun = true;
    L.sunDir = -ON_3dVector(std::cos(alt) * std::cos(north - az), std::cos(alt) * std::sin(north - az), std::sin(alt));
    L.sunIntensity = rs.Sun().Intensity();
  }
  return L;
}
}  // namespace

void Viewport::SetupRenderedLights() {
  auto* gl = GL(context());
  const DocLights L = CollectLights(m_scene.GetEngine());
  for (int i = 0; i < 8; i++) gl->glDisable(GL_LIGHT0 + i);
  const float amb[4] = {0.18f, 0.18f, 0.18f, 1.f};
  gl->glLightModelfv(GL_LIGHT_MODEL_AMBIENT, amb);
  int n = 0;
  auto setColor = [&](int i, const ON_Color& c, double k) {
    const float dif[4] = {float(c.FractionRed() * k), float(c.FractionGreen() * k), float(c.FractionBlue() * k), 1.f};
    const float zero[4] = {0, 0, 0, 1}, spec[4] = {0.35f, 0.35f, 0.35f, 1.f};
    gl->glLightfv(GL_LIGHT0 + i, GL_DIFFUSE, dif);
    gl->glLightfv(GL_LIGHT0 + i, GL_AMBIENT, zero);
    gl->glLightfv(GL_LIGHT0 + i, GL_SPECULAR, spec);
    gl->glLightf(GL_LIGHT0 + i, GL_SPOT_CUTOFF, 180.f);
    gl->glEnable(GL_LIGHT0 + i);
  };
  if (L.sun) {
    const float pos[4] = {float(-L.sunDir.x), float(-L.sunDir.y), float(-L.sunDir.z), 0.f};
    gl->glLightfv(GL_LIGHT0 + n, GL_POSITION, pos);
    setColor(n++, ON_Color(255, 246, 225), 0.9 * L.sunIntensity);
  }
  for (const ON_Light* l : L.lights) {
    if (n >= 8) break;
    const double k = std::min(1.5, 0.8 * l->Intensity());
    if (l->IsDirectionalLight()) {
      const ON_3dVector d = l->Direction();
      const float pos[4] = {float(-d.x), float(-d.y), float(-d.z), 0.f};
      gl->glLightfv(GL_LIGHT0 + n, GL_POSITION, pos);
      setColor(n++, l->Diffuse(), k);
    } else {
      const ON_3dPoint p = l->Location();
      const float pos[4] = {float(p.x), float(p.y), float(p.z), 1.f};
      gl->glLightfv(GL_LIGHT0 + n, GL_POSITION, pos);
      setColor(n, l->Diffuse(), k);
      if (l->IsSpotLight()) {
        const ON_3dVector d = l->Direction();
        const float dir[3] = {float(d.x), float(d.y), float(d.z)};
        gl->glLightfv(GL_LIGHT0 + n, GL_SPOT_DIRECTION, dir);
        gl->glLightf(GL_LIGHT0 + n, GL_SPOT_CUTOFF, float(std::min(90.0, l->SpotAngleDegrees())));
        gl->glLightf(GL_LIGHT0 + n, GL_SPOT_EXPONENT, 8.f);
      }
      n++;
    }
  }
  if (n == 0) {  // luce predefinita di Rhino: dalla camera, in alto a sinistra
    gl->glMatrixMode(GL_MODELVIEW);
    gl->glPushMatrix();
    gl->glLoadIdentity();
    const float lp[4] = {-0.4f, 0.5f, 1.0f, 0.0f};
    gl->glLightfv(GL_LIGHT0, GL_POSITION, lp);
    gl->glPopMatrix();
    setColor(0, ON_Color(255, 255, 255), 0.85);
  }
}

void Viewport::DrawGroundShadows() {
  Engine& e = m_scene.GetEngine();
  const auto& gp = e.Model().m_settings.m_RenderSettings.GroundPlane();
  if (!gp.Enabled()) return;
  const DocLights L = CollectLights(e);
  ON_3dVector d(0.4, 0.6, -1);  // stessa luce del rendering predefinito
  if (L.sun) d = L.sunDir;
  else
    for (const ON_Light* l : L.lights)
      if (l->IsDirectionalLight()) { d = l->Direction(); break; }
  if (!d.Unitize() || d.z > -0.05) return;
  const ON_BoundingBox b = m_scene.Bounds();
  if (!b.IsValid()) return;
  const double g = gp.AutoAltitude() ? b.m_min.z : gp.Altitude();
  auto* gl = GL(context());
  const double kx = d.x / d.z, ky = d.y / d.z;
  const double M[16] = {1, 0, 0, 0, 0, 1, 0, 0, -kx, -ky, 0, 0, kx * g, ky * g, g, 1};
  gl->glPushMatrix();
  gl->glMultMatrixd(M);
  gl->glDisable(GL_LIGHTING);
  gl->glEnable(GL_BLEND);
  gl->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  gl->glDepthFunc(GL_LESS);  // ombre sovrapposte: un solo strato
  gl->glEnable(GL_POLYGON_OFFSET_FILL);
  gl->glPolygonOffset(-1.f, -1.f);
  gl->glColor4f(0.f, 0.f, 0.f, 0.32f);
  for (const auto& obj : m_scene.Objects()) {
    if (!obj.shaded) continue;
    const ON_Mesh& m = *obj.shaded;
    gl->glBegin(GL_TRIANGLES);
    for (int i = 0; i < m.m_F.Count(); i++) {
      const ON_MeshFace& f = m.m_F[i];
      const int tri[2][3] = {{f.vi[0], f.vi[1], f.vi[2]}, {f.vi[0], f.vi[2], f.vi[3]}};
      for (int t = 0; t < (f.IsQuad() ? 2 : 1); t++)
        for (int k = 0; k < 3; k++) gl->glVertex3f(m.m_V[tri[t][k]].x, m.m_V[tri[t][k]].y, m.m_V[tri[t][k]].z);
    }
    gl->glEnd();
  }
  gl->glDisable(GL_POLYGON_OFFSET_FILL);
  gl->glDepthFunc(GL_LEQUAL);
  gl->glDisable(GL_BLEND);
  gl->glPopMatrix();
}

void Viewport::initializeGL() {}

QString Viewport::Title() const { return QString::fromUtf8(KindName(m_cam.kind)); }

ON_3dmView Viewport::ToView() const {
  ON_3dmView v;
  ON_Viewport& vp = v.m_vp;
  const double aspect = double(std::max(1, width())) / std::max(1, height());
  if (m_cam.kind == Camera::Kind::Perspective) {
    vp.ChangeToPerspectiveProjection(ON_UNSET_VALUE, true, m_cam.fovY / 2);
    const double n = std::max(1e-3, m_cam.distance * 0.01), t = n * std::tan(m_cam.fovY / 2);
    vp.SetCameraLocation(m_cam.Eye());
    vp.SetCameraDirection(m_cam.Dir());
    vp.SetCameraUp(m_cam.Up());
    vp.SetFrustum(-t * aspect, t * aspect, -t, t, n, m_cam.distance * 100);
  } else {
    vp.ChangeToParallelProjection(true);
    vp.SetCameraLocation(m_cam.target - m_cam.Dir() * 1000.0 * m_cam.halfHeight);
    vp.SetCameraDirection(m_cam.Dir());
    vp.SetCameraUp(m_cam.Up());
    const double h = m_cam.halfHeight;
    vp.SetFrustum(-h * aspect, h * aspect, -h, h, 1e-3, 2000.0 * h);
  }
  vp.SetScreenPort(0, width(), height(), 0, 0, 0xff);
  vp.SetViewportId(id);
  v.SetTargetPoint(m_cam.target);
  v.m_cplane = ON_3dmConstructionPlane(m_cam.CPlane());
  v.m_name = ON_wString(KindName(m_cam.kind));
  v.m_display_mode_id = mode == DisplayMode::Shaded ? ON_StandardDisplayModeId::Shaded
                        : mode == DisplayMode::Ghosted ? ON_StandardDisplayModeId::Ghosted
                        : mode == DisplayMode::XRay ? ON_StandardDisplayModeId::XrayShade
                        : mode == DisplayMode::Rendered ? ON_StandardDisplayModeId::Rendered
                        : mode == DisplayMode::Raytraced ? ON_StandardDisplayModeId::Raytraced
                                                    : ON_StandardDisplayModeId::Wireframe;
  return v;
}

void Viewport::FromView(const ON_3dmView& v) {
  const ON_Viewport& vp = v.m_vp;
  ON_3dVector d = vp.CameraDirection();
  if (!d.Unitize()) return;
  ON_3dPoint tg = v.TargetPoint();
  double l, r, b, t, n, f;
  vp.GetFrustum(&l, &r, &b, &t, &n, &f);
  if (!tg.IsValid()) tg = vp.CameraLocation() + d * (vp.IsPerspectiveProjection() ? (n + f) / 2 : 100.0);
  m_cam.target = tg;
  if (vp.IsParallelProjection()) {
    // le viste piane di Kurva3D sono le sei standard: si cerca quella con la stessa direzione
    const std::pair<ON_3dVector, Camera::Kind> std6[] = {
        {ON_3dVector(0, 0, -1), Camera::Kind::Top}, {ON_3dVector(0, 1, 0), Camera::Kind::Front},
        {ON_3dVector(-1, 0, 0), Camera::Kind::Right}, {ON_3dVector(0, 0, 1), Camera::Kind::Bottom},
        {ON_3dVector(1, 0, 0), Camera::Kind::Left}, {ON_3dVector(0, -1, 0), Camera::Kind::Back}};
    for (const auto& [dir, k] : std6)
      if (dir * d > 0.9999) {
        m_cam.kind = k;
        m_cam.halfHeight = std::max(1e-6, (t - b) / 2);
        goto cplane;
      }
  }
  m_cam.kind = Camera::Kind::Perspective;
  m_cam.pitch = std::asin(std::max(-1.0, std::min(1.0, -d.z)));
  m_cam.yaw = std::atan2(-d.y, -d.x);
  m_cam.distance = std::max(1e-6, vp.CameraLocation().DistanceTo(tg));
  if (vp.IsPerspectiveProjection() && n > 0) m_cam.fovY = 2 * std::atan((t - b) / 2 / n);
cplane:
  {
    Camera def = m_cam;
    def.customCPlane = false;
    const ON_Plane& p = v.m_cplane.m_plane;
    const ON_Plane dp = def.CPlane();
    m_cam.customCPlane = p.IsValid() && (p.origin.DistanceTo(dp.origin) > 1e-9 || p.xaxis * dp.xaxis < 1 - 1e-12 ||
                                         p.yaxis * dp.yaxis < 1 - 1e-12);
    if (m_cam.customCPlane) m_cam.cplane = p;
  }
  update();
}


namespace {
QOpenGLFunctions_1_1* GL(QOpenGLContext* ctx) {
  return QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_1_1>(ctx);
}
}  // namespace

unsigned Viewport::TextureId(const std::string& file) {
  auto it = m_textures.find(file);
  if (it != m_textures.end()) return it->second;
  unsigned id = 0;
  QImage img(QString::fromStdString(file));
  if (!img.isNull()) {
    // OpenGL 1.1: lati potenza di due, al massimo 1024
    auto pot = [](int v) { int p = 1; while (p < v && p < 1024) p <<= 1; return p; };
    img = img.convertToFormat(QImage::Format_RGBA8888)
              .scaled(pot(img.width()), pot(img.height()), Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
              .mirrored();  // la riga 0 di OpenGL e' in basso (v = 0 come in Rhino)
    auto* gl = GL(context());
    GLuint t = 0;
    gl->glGenTextures(1, &t);
    gl->glBindTexture(GL_TEXTURE_2D, t);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.width(), img.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, img.constBits());
    gl->glBindTexture(GL_TEXTURE_2D, 0);
    id = t;
  }
  m_textures[file] = id;
  return id;
}

void Viewport::DrawGrid() {
  auto* gl = GL(context());
  const ON_Plane cp = m_cam.CPlane();
  const double s = Shared().gridSpacing;
  const int n = 100;  // linee per lato (griglia di Rhino: 100 x 1 mm, maggiori ogni 10)
  gl->glLineWidth(1.0f);
  gl->glBegin(GL_LINES);
  for (int i = -n; i <= n; i++) {
    if (i == 0) continue;
    const bool major = (i % 10) == 0;
    const QColor& gc = major ? Shared().colors.gridMajor : Shared().colors.gridMinor;
    gl->glColor3ub(gc.red(), gc.green(), gc.blue());
    ON_3dPoint a = cp.PointAt(i * s, -n * s), b = cp.PointAt(i * s, n * s);
    ON_3dPoint c = cp.PointAt(-n * s, i * s), d = cp.PointAt(n * s, i * s);
    gl->glVertex3d(a.x, a.y, a.z); gl->glVertex3d(b.x, b.y, b.z);
    gl->glVertex3d(c.x, c.y, c.z); gl->glVertex3d(d.x, d.y, d.z);
  }
  // assi del piano di costruzione: X rosso, Y verde
  gl->glColor3ub(Shared().colors.axisX.red(), Shared().colors.axisX.green(), Shared().colors.axisX.blue());
  ON_3dPoint a = cp.PointAt(-n * s, 0), b = cp.PointAt(n * s, 0);
  gl->glVertex3d(a.x, a.y, a.z); gl->glVertex3d(b.x, b.y, b.z);
  gl->glColor3ub(Shared().colors.axisY.red(), Shared().colors.axisY.green(), Shared().colors.axisY.blue());
  a = cp.PointAt(0, -n * s); b = cp.PointAt(0, n * s);
  gl->glVertex3d(a.x, a.y, a.z); gl->glVertex3d(b.x, b.y, b.z);
  gl->glEnd();
}

namespace {
bool InList(const std::vector<ON_UUID>& v, const ON_UUID& id) {
  for (const auto& x : v) if (ON_UuidCompare(x, id) == 0) return true;
  return false;
}
// rosso -> verde -> blu al crescere di f in [0,1]
void Ramp(double f, float c[3]) {
  f = std::clamp(f, 0.0, 1.0);
  if (f < 0.5) { c[0] = float(1 - 2 * f); c[1] = float(2 * f); c[2] = 0; }
  else { c[0] = 0; c[1] = float(2 - 2 * f); c[2] = float(2 * f - 1); }
}
}  // namespace

std::shared_ptr<cad::CurvatureData> Viewport::CurvatureOf(const Scene& scene, const DisplayObject& obj) {
  auto& cache = Shared().curvCache;
  const std::string key = [&] { char b[40]; ON_UuidToString(obj.id, b); return std::string(b); }();
  const ON_Geometry* g = scene.Geometry(obj.id);
  const void* src = obj.shaded ? static_cast<const void*>(obj.shaded.get()) : static_cast<const void*>(g);
  auto it = cache.find(key);
  if (it != cache.end() && it->second.source == src) return it->second.data;
  std::shared_ptr<cad::CurvatureData> d;
#ifdef CAD_WITH_OCCT
  if (g) {
    auto nd = std::make_shared<cad::CurvatureData>();
    if (cad::ComputeCurvature(*g, scene.GetEngine().Tolerance(), *nd, obj.shaded.get())) d = nd;
  }
#endif
  cache[key] = {src, d};
  return d;
}

// Analisi come in Rhino: strisce di zebra (riflessione di strisce verticali o orizzontali),
// angolo di sformo rispetto a z del mondo (intervallo impostabile, rosso -> blu), curvatura a
// falsi colori, mappa d'ambiente (EMap), grafico della curvatura delle curve e spigoli nudi.
void Viewport::DrawAnalysis() {
  const Settings& S = Shared();
  const auto& A = S.analysis;
  if (S.zebra.empty() && S.draft.empty() && S.curvature.empty() && S.edges.empty() && S.curvatureAnalysis.empty() &&
      S.emap.empty())
    return;
  auto* gl = GL(context());
  const bool persp = m_cam.kind == Camera::Kind::Perspective;
  const ON_3dPoint eye = m_cam.Eye();
  ON_3dVector vd = m_cam.Dir();
  vd.Unitize();
  ON_3dVector camRight = m_cam.Right(), camUp = m_cam.Up();
  camRight.Unitize();
  camUp.Unitize();
  if (!S.zebra.empty() && (!m_zebraTex || m_zebraSize != A.zebraSize)) {
    GLuint t = m_zebraTex;
    if (!t) gl->glGenTextures(1, &t);
    m_zebraTex = t;
    m_zebraSize = A.zebraSize;
    gl->glBindTexture(GL_TEXTURE_1D, t);
    unsigned char px[64 * 3];
    for (int i = 0; i < 64; i++) { const unsigned char v = i < 32 ? 20 : 245; px[3 * i] = px[3 * i + 1] = px[3 * i + 2] = v; }
    gl->glTexImage1D(GL_TEXTURE_1D, 0, GL_RGB, 64, 0, GL_RGB, GL_UNSIGNED_BYTE, px);
    gl->glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  }
  if (!S.emap.empty() && (!m_emapTex || m_emapRevision != A.emapRevision)) {
    GLuint t = m_emapTex;
    if (!t) gl->glGenTextures(1, &t);
    m_emapTex = t;
    m_emapRevision = A.emapRevision;
    QImage img;
    if (!A.emapFile.empty()) img.load(QString::fromStdString(A.emapFile));
    if (img.isNull()) {
      // ambiente da studio: cielo chiaro in alto, due luci morbide, pavimento scuro (vista come "sfera cromata")
      const int N = 256;
      img = QImage(N, N, QImage::Format_RGB888);
      for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
          const double nx = (x + 0.5) / N * 2 - 1, ny = 1 - (y + 0.5) / N * 2;
          const double r2 = nx * nx + ny * ny;
          double v;
          if (r2 > 1) v = 0.2;
          else {
            const double el = ny;  // -1 basso, +1 alto
            v = el > 0 ? 0.55 + 0.4 * el : 0.12 + 0.25 * (1 + el);
            const double l1 = std::exp(-((nx + 0.45) * (nx + 0.45) + (ny - 0.45) * (ny - 0.45)) / 0.02);
            const double l2 = std::exp(-((nx - 0.55) * (nx - 0.55) + (ny - 0.2) * (ny - 0.2)) / 0.01);
            const double horizon = std::exp(-(ny * ny) / 0.002) * 0.35;
            v = std::min(1.0, v + 0.6 * l1 + 0.5 * l2 + horizon);
          }
          const int c = int(std::clamp(v, 0.0, 1.0) * 255);
          img.setPixel(x, y, qRgb(int(c * 0.96), int(c * 0.98), c));
        }
    }
    img = img.convertToFormat(QImage::Format_RGB888).scaled(256, 256, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).mirrored();
    gl->glBindTexture(GL_TEXTURE_2D, t);
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 256, 256, 0, GL_RGB, GL_UNSIGNED_BYTE, img.constBits());
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
  }
  const double zebraFreq = A.zebraSize == 0 ? 10.0 : A.zebraSize == 2 ? 3.0 : 6.0;
  const double draftSpan = std::max(1e-6, A.draftMax - A.draftMin);
  const auto style = static_cast<cad::CurvatureStyle>(std::clamp(A.curvStyle, 0, 3));
  gl->glDisable(GL_LIGHTING);
  gl->glEnable(GL_POLYGON_OFFSET_FILL);
  gl->glPolygonOffset(1.f, 1.f);
  for (const auto& obj : m_scene.Objects()) {
    // curvatura a falsi colori: mesh di analisi propria, colore per vertice
    if (InList(S.curvatureAnalysis, obj.id)) {
      if (auto cd = CurvatureOf(m_scene, obj)) {
        const ON_Mesh& m = cd->mesh;
        gl->glBegin(GL_TRIANGLES);
        for (int i = 0; i < m.m_F.Count(); i++) {
          const ON_MeshFace& f = m.m_F[i];
          const int tri[2][3] = {{f.vi[0], f.vi[1], f.vi[2]}, {f.vi[0], f.vi[2], f.vi[3]}};
          for (int t = 0; t < (f.IsQuad() ? 2 : 1); t++)
            for (int k = 0; k < 3; k++) {
              const int vi = tri[t][k];
              float c[3];
              cad::CurvatureColor(style, cad::CurvatureValue(style, cd->k1[vi], cd->k2[vi]), A.curvMin, A.curvMax, c);
              gl->glColor3fv(c);
              gl->glVertex3f(m.m_V[vi].x, m.m_V[vi].y, m.m_V[vi].z);
            }
        }
        gl->glEnd();
      }
    }
    const bool z = InList(S.zebra, obj.id), d = InList(S.draft, obj.id), em = InList(S.emap, obj.id);
    if ((z || d || em) && obj.shaded) {
      const ON_Mesh& m = *obj.shaded;
      if (!m.HasVertexNormals()) continue;
      if (z) {
        gl->glEnable(GL_TEXTURE_1D);
        gl->glBindTexture(GL_TEXTURE_1D, m_zebraTex);
        gl->glColor3f(1, 1, 1);
      } else if (em) {
        gl->glEnable(GL_TEXTURE_2D);
        gl->glBindTexture(GL_TEXTURE_2D, m_emapTex);
        gl->glColor3f(1, 1, 1);
      }
      gl->glBegin(GL_TRIANGLES);
      for (int i = 0; i < m.m_F.Count(); i++) {
        const ON_MeshFace& f = m.m_F[i];
        const int tri[2][3] = {{f.vi[0], f.vi[1], f.vi[2]}, {f.vi[0], f.vi[2], f.vi[3]}};
        for (int t = 0; t < (f.IsQuad() ? 2 : 1); t++)
          for (int k = 0; k < 3; k++) {
            const int vi = tri[t][k];
            const ON_3dPoint p(m.m_V[vi]);
            ON_3dVector n(m.m_N[vi]);
            if (z || em) {
              ON_3dVector dir = persp ? ON_3dVector(p - eye) : vd;
              dir.Unitize();
              if (n * dir > 0) n = -n;  // lato visto
              if (z) {
                const ON_3dVector r = dir - 2.0 * (dir * n) * n;
                const double s = A.zebraHorizontal ? std::asin(std::clamp(r.z, -1.0, 1.0)) : std::atan2(r.y, r.x);
                gl->glTexCoord1d(s / ON_PI * zebraFreq);
              } else {
                // mappa sferica nello spazio della vista (come una sfera cromata fotografata)
                const ON_3dVector r = dir - 2.0 * (dir * n) * n;
                ON_3dVector h(r * camRight, r * camUp, -(r * vd) + 1.0);
                const double len = h.Length();
                if (len > 1e-9) h = h / len;
                gl->glTexCoord2d(0.5 + 0.5 * h.x, 0.5 + 0.5 * h.y);
              }
            } else {
              float c[3];
              const double ang = std::asin(std::clamp(n.z, -1.0, 1.0)) * 180.0 / ON_PI;
              Ramp((ang - A.draftMin) / draftSpan, c);
              gl->glColor3fv(c);
            }
            gl->glVertex3d(p.x, p.y, p.z);
          }
      }
      gl->glEnd();
      if (z) gl->glDisable(GL_TEXTURE_1D);
      if (em && !z) gl->glDisable(GL_TEXTURE_2D);
    }
    if (InList(S.curvature, obj.id)) {
      const auto* c = ON_Curve::Cast(m_scene.Geometry(obj.id));
      if (c) {
        const ON_Interval dom = c->Domain();
        const int n = 200;
        std::vector<ON_3dPoint> P(n + 1);
        std::vector<ON_3dVector> K(n + 1);
        double maxK = 0, len = 0;
        for (int i = 0; i <= n; i++) {
          const double t = dom.ParameterAt(double(i) / n);
          P[i] = c->PointAt(t);
          K[i] = c->CurvatureAt(t);
          maxK = std::max(maxK, K[i].Length());
          if (i) len += P[i].DistanceTo(P[i - 1]);
        }
        if (maxK > 1e-12) {
          const double s = 0.25 * len / maxK;
          gl->glColor3ub(200, 0, 200);
          gl->glLineWidth(1.f);
          gl->glBegin(GL_LINES);
          for (int i = 0; i <= n; i += 2) {
            const ON_3dPoint q = P[i] - K[i] * s;
            gl->glVertex3d(P[i].x, P[i].y, P[i].z);
            gl->glVertex3d(q.x, q.y, q.z);
          }
          gl->glEnd();
          gl->glBegin(GL_LINE_STRIP);
          for (int i = 0; i <= n; i++) { const ON_3dPoint q = P[i] - K[i] * s; gl->glVertex3d(q.x, q.y, q.z); }
          gl->glEnd();
        }
      }
    }
    if (InList(S.edges, obj.id)) {
      const ON_Geometry* g = m_scene.Geometry(obj.id);
      std::unique_ptr<ON_Brep> owned;
      const ON_Brep* b = ON_Brep::Cast(g);
      if (!b && g && g->HasBrepForm()) { owned.reset(g->BrepForm()); b = owned.get(); }
      if (b) {
        gl->glColor3ub(255, 0, 255);
        gl->glLineWidth(3.f);
        for (int ei = 0; ei < b->m_E.Count(); ei++) {
          const ON_BrepEdge& e = b->m_E[ei];
          if (e.m_ti.Count() != 1) continue;
          gl->glBegin(GL_LINE_STRIP);
          for (int i = 0; i <= 32; i++) {
            const ON_3dPoint q = e.PointAt(e.Domain().ParameterAt(i / 32.0));
            gl->glVertex3d(q.x, q.y, q.z);
          }
          gl->glEnd();
        }
        gl->glLineWidth(1.f);
      }
    }
  }
  gl->glDisable(GL_POLYGON_OFFSET_FILL);
}

void Viewport::paintGL() {
  auto* gl = GL(context());
  const int w = width(), h = height();
  gl->glViewport(0, 0, w * devicePixelRatio(), h * devicePixelRatio());
  {  // sfondo sfumato (dall'alto in basso) con i colori del tema
    const auto& C = Shared().colors;
    gl->glClearColor(C.bgBottom.redF(), C.bgBottom.greenF(), C.bgBottom.blueF(), 1.f);
    gl->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    gl->glMatrixMode(GL_PROJECTION);
    gl->glLoadIdentity();
    gl->glMatrixMode(GL_MODELVIEW);
    gl->glLoadIdentity();
    gl->glDisable(GL_DEPTH_TEST);
    gl->glDisable(GL_LIGHTING);
    gl->glBegin(GL_QUADS);
    gl->glColor3f(C.bgBottom.redF(), C.bgBottom.greenF(), C.bgBottom.blueF());
    gl->glVertex2f(-1, -1);
    gl->glVertex2f(1, -1);
    gl->glColor3f(C.bgTop.redF(), C.bgTop.greenF(), C.bgTop.blueF());
    gl->glVertex2f(1, 1);
    gl->glVertex2f(-1, 1);
    gl->glEnd();
    gl->glEnable(GL_DEPTH_TEST);
  }
  // Raytraced: immagine del path tracer (progressiva) al posto della scena ombreggiata
  bool rtImage = false;
  if (mode == DisplayMode::Raytraced) {
    UpdateRaytrace();
    std::lock_guard<std::mutex> lk(m_rt->mx);
    if ((int)m_rt->img.size() == m_rt->w * m_rt->h * 3 && m_rt->w == std::max(8, w) && m_rt->h == std::max(8, h)) {
      gl->glDisable(GL_DEPTH_TEST);
      gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      // posizione raster nel centro (sempre valida) e poi spostata all'angolo in alto a sinistra con
      // glBitmap: una posizione proprio sul bordo (-1, 1) puo' risultare fuori dal volume di vista su
      // alcuni driver e glDrawPixels allora non disegna niente (vista Raytraced vuota)
      gl->glRasterPos2f(0.f, 0.f);
      const int vw = int(w * devicePixelRatio()), vh = int(h * devicePixelRatio());  // come glViewport
      gl->glBitmap(0, 0, 0.f, 0.f, -0.5f * vw, 0.5f * vh, nullptr);
      gl->glPixelZoom((float)devicePixelRatioF(), -(float)devicePixelRatioF());
      gl->glDrawPixels(m_rt->w, m_rt->h, GL_RGB, GL_UNSIGNED_BYTE, m_rt->img.data());
      gl->glPixelZoom(1.f, 1.f);
      gl->glEnable(GL_DEPTH_TEST);
      rtImage = true;
    }
  } else if (m_rt) {
    StopRaytrace();
  }
  const bool rendered = mode == DisplayMode::Rendered || mode == DisplayMode::Raytraced;

  // Matrici: proiezione e vista calcolate qui (le stesse usate per la selezione).
  const double aspect = double(w) / std::max(1, h);
  gl->glMatrixMode(GL_PROJECTION);
  gl->glLoadIdentity();
  const double farP = (m_cam.kind == Camera::Kind::Perspective) ? m_cam.distance * 200 : 4e5;
  if (m_cam.kind == Camera::Kind::Perspective) {
    const double nearP = m_cam.distance * 0.01;
    const double t = nearP * std::tan(m_cam.fovY / 2);
    gl->glFrustum(-t * aspect, t * aspect, -t, t, nearP, farP);
  } else {
    gl->glOrtho(-m_cam.halfHeight * aspect, m_cam.halfHeight * aspect, -m_cam.halfHeight, m_cam.halfHeight, -farP, farP);  // niente taglio davanti alla camera nelle viste piane
  }
  gl->glMatrixMode(GL_MODELVIEW);
  const ON_3dVector r = m_cam.Right(), u = m_cam.Up(), d = m_cam.Dir();
  const ON_3dPoint e = m_cam.Eye();
  const double M[16] = {r.x, u.x, -d.x, 0, r.y, u.y, -d.y, 0, r.z, u.z, -d.z, 0,
                        -(r * ON_3dVector(e)), -(u * ON_3dVector(e)), (d * ON_3dVector(e)), 1};
  gl->glLoadMatrixd(M);

  gl->glDisable(GL_LIGHTING);
  gl->glDisable(GL_DEPTH_TEST);
  if (showGrid && !rtImage) DrawGrid();
  gl->glEnable(GL_DEPTH_TEST);
  gl->glDepthFunc(GL_LEQUAL);
  // Piani di sezione attivi in questa vista (Rhino: visibile dove l'equazione <= 0;
  // OpenGL tiene dove l'equazione >= 0, quindi si cambia segno)
  int clips = 0;
  for (const auto& c : m_scene.Clips()) {
    if (clips >= 6 || std::find(c.views.begin(), c.views.end(), id) == c.views.end()) continue;
    const double eq[4] = {-c.eq.x, -c.eq.y, -c.eq.z, -c.eq.d};
    gl->glClipPlane(GL_CLIP_PLANE0 + clips, eq);
    gl->glEnable(GL_CLIP_PLANE0 + clips);
    clips++;
  }

  // Superfici ombreggiate (Shaded), trasparenti (Ghosted) o trasparenti con spigoli sempre visibili (X-Ray)
  const bool shaded = mode != DisplayMode::Wireframe && !rtImage;
  const bool transparent = mode == DisplayMode::Ghosted || mode == DisplayMode::XRay;
  if (shaded) {
    if (transparent) {
      gl->glEnable(GL_BLEND);
      gl->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      gl->glDepthMask(GL_FALSE);
    }
    gl->glEnable(GL_LIGHTING);
    gl->glEnable(GL_LIGHT0);
    gl->glEnable(GL_NORMALIZE);
    gl->glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, 1);
    // luce "dalla camera" come la luce predefinita di Rhino
    if (rendered) {
      SetupRenderedLights();
      const float spec[4] = {0.25f, 0.25f, 0.25f, 1.f};
      gl->glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, spec);
      gl->glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 40.f);
    } else {
      for (int i = 1; i < 8; i++) gl->glDisable(GL_LIGHT0 + i);
      const float zero[4] = {0, 0, 0, 1}, gamb[4] = {0.2f, 0.2f, 0.2f, 1.f};
      gl->glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, zero);
      gl->glLightModelfv(GL_LIGHT_MODEL_AMBIENT, gamb);
      gl->glMatrixMode(GL_MODELVIEW);
      gl->glPushMatrix();
      gl->glLoadIdentity();
      const float lp[4] = {0.3f, 0.5f, 1.0f, 0.0f};
      gl->glLightfv(GL_LIGHT0, GL_POSITION, lp);
      gl->glPopMatrix();
      const float amb[4] = {0.25f, 0.25f, 0.25f, 1.f}, dif[4] = {0.8f, 0.8f, 0.8f, 1.f};
      gl->glLightfv(GL_LIGHT0, GL_AMBIENT, amb);
      gl->glLightfv(GL_LIGHT0, GL_DIFFUSE, dif);
      gl->glLightf(GL_LIGHT0, GL_SPOT_CUTOFF, 180.f);
    }
    gl->glEnable(GL_POLYGON_OFFSET_FILL);
    gl->glPolygonOffset(1.f, 1.f);
    for (const auto& obj : m_scene.Objects()) {
      if (!obj.shaded) continue;
      float col[4] = {obj.selected ? 0.95f : 0.82f, obj.selected ? 0.9f : 0.82f, obj.selected ? 0.45f : 0.82f,
                      transparent ? 0.35f : 1.f};
      if (rendered && !obj.selected) {  // colore del materiale (bianco se non c'e')
        const ON_Color mc = obj.hasMaterial ? obj.material : ON_Color(235, 235, 235);
        col[0] = (float)mc.FractionRed();
        col[1] = (float)mc.FractionGreen();
        col[2] = (float)mc.FractionBlue();
      }
      const ON_Mesh& m = *obj.shaded;
      // texture del materiale (solo in Shaded, come la modalita' Rendered semplificata)
      const bool tex = (mode == DisplayMode::Shaded || rendered) && !obj.texture.empty() && (int)obj.uv.size() == m.m_V.Count();
      const unsigned tid = tex ? TextureId(obj.texture) : 0;
      if (tid) {
        const float s = obj.selected ? 0.75f : 1.f;
        const float tc[4] = {(float)obj.diffuse.FractionRed() * s, (float)obj.diffuse.FractionGreen() * s,
                             (float)obj.diffuse.FractionBlue() * (obj.selected ? 0.5f : 1.f), 1.f};
        gl->glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, tc);
        gl->glEnable(GL_TEXTURE_2D);
        gl->glBindTexture(GL_TEXTURE_2D, tid);
        gl->glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
      } else {
        gl->glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, col);
      }
      const bool hasN = m.HasVertexNormals();
      gl->glBegin(GL_TRIANGLES);
      for (int i = 0; i < m.m_F.Count(); i++) {
        const ON_MeshFace& f = m.m_F[i];
        const int tri[2][3] = {{f.vi[0], f.vi[1], f.vi[2]}, {f.vi[0], f.vi[2], f.vi[3]}};
        for (int t = 0; t < (f.IsQuad() ? 2 : 1); t++)
          for (int k = 0; k < 3; k++) {
            const int vi = tri[t][k];
            if (hasN) gl->glNormal3f(m.m_N[vi].x, m.m_N[vi].y, m.m_N[vi].z);
            if (tid) gl->glTexCoord2f(obj.uv[vi].x, obj.uv[vi].y);
            gl->glVertex3f(m.m_V[vi].x, m.m_V[vi].y, m.m_V[vi].z);
          }
      }
      gl->glEnd();
      if (tid) gl->glDisable(GL_TEXTURE_2D);
    }
    gl->glDisable(GL_POLYGON_OFFSET_FILL);
    gl->glDisable(GL_LIGHTING);
    if (transparent) { gl->glDepthMask(GL_TRUE); gl->glDisable(GL_BLEND); }
    if (rendered) DrawGroundShadows();
  }
  DrawAnalysis();
  if (mode == DisplayMode::XRay) gl->glDisable(GL_DEPTH_TEST);

  // Wireframe, punti
  for (int pass = 0; pass < 2; pass++) {  // selezionati per ultimi, sopra
    for (const auto& obj : m_scene.Objects()) {
      if (obj.selected != (pass == 1)) continue;
      // Renderizzato/Raytraced: solo le curve, i punti e gli oggetti selezionati
      if (rendered && obj.shaded && !obj.selected) continue;
      if (rtImage && !obj.selected && obj.shaded) continue;
      {
        const auto& SC = Shared();
        const QColor oc = obj.selected ? SC.colors.selected : obj.locked ? QColor(128, 128, 128) : SC.ObjectColor(obj.color);
        gl->glColor3ub(oc.red(), oc.green(), oc.blue());
      }
      gl->glLineWidth(obj.selected ? 2.0f : 1.0f);
      if (obj.unclipped) for (int k = 0; k < clips; k++) gl->glDisable(GL_CLIP_PLANE0 + k);
      if (mode == DisplayMode::Wireframe || mode == DisplayMode::XRay || mode == DisplayMode::Ghosted) {
        // contorno apparente: spigoli dove una faccia guarda la camera e l'altra no
        const bool persp = m_cam.kind == Camera::Kind::Perspective;
        const ON_3dVector vd = m_cam.Dir();
        const ON_3dPoint eye = m_cam.Eye();
        gl->glBegin(GL_LINES);
        for (const auto& se : obj.silhouette) {
          const ON_3dVector dd = persp ? ON_3dVector(se.a - eye) : vd;
          if ((se.n1 * dd) * (se.n2 * dd) <= 0) {
            gl->glVertex3d(se.a.x, se.a.y, se.a.z);
            gl->glVertex3d(se.b.x, se.b.y, se.b.z);
          }
        }
        gl->glEnd();
      }
      for (const auto& wire : obj.isocurves) {
        gl->glBegin(GL_LINE_STRIP);
        for (const auto& p : wire) gl->glVertex3d(p.x, p.y, p.z);
        gl->glEnd();
      }
      for (const auto& wire : obj.wires) {
        gl->glBegin(GL_LINE_STRIP);
        for (const auto& p : wire) gl->glVertex3d(p.x, p.y, p.z);
        gl->glEnd();
      }
      if (!obj.triangles.empty()) {
        gl->glBegin(GL_TRIANGLES);
        for (const auto& p : obj.triangles) gl->glVertex3d(p.x, p.y, p.z);
        gl->glEnd();
      }
      gl->glPointSize(5.0f);
      gl->glBegin(GL_POINTS);
      for (const auto& p : obj.points) gl->glVertex3d(p.x, p.y, p.z);
      gl->glEnd();
      if (obj.unclipped) for (int k = 0; k < clips; k++) gl->glEnable(GL_CLIP_PLANE0 + k);
    }
  }

  // poligoni e reti di controllo degli oggetti con i punti attivi
  gl->glLineWidth(1.0f);
  for (const auto& obj : m_scene.Objects()) {
    if (obj.cage.empty()) continue;
    gl->glColor3ub(Shared().colors.cage.red(), Shared().colors.cage.green(), Shared().colors.cage.blue());
    for (const auto& poly : obj.cage) {
      gl->glBegin(GL_LINE_STRIP);
      for (const auto& q : poly) gl->glVertex3d(q.x, q.y, q.z);
      gl->glEnd();
    }
  }
  if (m_gripDrag.active) {  // anteprima del punto trascinato
    gl->glDisable(GL_DEPTH_TEST);
    gl->glColor3ub(Shared().colors.cage.red(), Shared().colors.cage.green(), Shared().colors.cage.blue());
    if (const ON_Geometry* g = m_scene.Geometry(m_gripDrag.id)) {
      std::unique_ptr<ON_Geometry> moved(MoveGrip(g, m_gripDrag.i, m_gripDrag.j, m_gripDrag.cur));
      if (const auto* c = ON_Curve::Cast(moved.get())) {
        std::vector<ON_3dPoint> w;
        TessellateCurve(*c, w);
        gl->glBegin(GL_LINE_STRIP);
        for (const auto& q : w) gl->glVertex3d(q.x, q.y, q.z);
        gl->glEnd();
      }
      GripSet gs;
      if (moved && GetGrips(moved.get(), gs, m_gripDrag.j == -2))
        for (const auto& poly : gs.cage) {
          gl->glBegin(GL_LINE_STRIP);
          for (const auto& q : poly) gl->glVertex3d(q.x, q.y, q.z);
          gl->glEnd();
        }
    }
    gl->glEnable(GL_DEPTH_TEST);
  }

  if (m_gbDrag.mode) {  // anteprima del Gumball: gli oggetti selezionati trasformati
    const ON_Xform x = GumballXform();
    double m[16];
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) m[c * 4 + r] = x[r][c];
    gl->glPushMatrix();
    gl->glMultMatrixd(m);
    gl->glColor3ub(Shared().colors.cage.red(), Shared().colors.cage.green(), Shared().colors.cage.blue());
    gl->glLineWidth(1.0f);
    for (const auto& obj : m_scene.Objects()) {
      if (!obj.selected) continue;
      for (const auto* list : {&obj.wires, &obj.isocurves})
        for (const auto& wire : *list) {
          gl->glBegin(GL_LINE_STRIP);
          for (const auto& q : wire) gl->glVertex3d(q.x, q.y, q.z);
          gl->glEnd();
        }
    }
    gl->glPopMatrix();
  }

  // Anteprima dell'editor a nodi: superfici trasparenti e curve nel colore dell'anteprima
  if (!m_scene.Preview().empty()) {
    const QColor pc(200, 60, 40);
    gl->glDisable(GL_LIGHTING);
    if (shaded) {
      gl->glEnable(GL_BLEND);
      gl->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      gl->glDepthMask(GL_FALSE);
      gl->glColor4f(pc.redF(), pc.greenF(), pc.blueF(), 0.35f);
      for (const auto& obj : m_scene.Preview()) {
        if (!obj.shaded) continue;
        const ON_Mesh& m = *obj.shaded;
        gl->glBegin(GL_TRIANGLES);
        for (int i = 0; i < m.m_F.Count(); i++) {
          const ON_MeshFace& f = m.m_F[i];
          const int tri[2][3] = {{f.vi[0], f.vi[1], f.vi[2]}, {f.vi[0], f.vi[2], f.vi[3]}};
          for (int t = 0; t < (f.IsQuad() ? 2 : 1); t++)
            for (int k = 0; k < 3; k++) gl->glVertex3fv(&m.m_V[tri[t][k]].x);
        }
        gl->glEnd();
      }
      gl->glDepthMask(GL_TRUE);
      gl->glDisable(GL_BLEND);
    }
    gl->glColor3ub(pc.red(), pc.green(), pc.blue());
    gl->glLineWidth(1.5f);
    for (const auto& obj : m_scene.Preview()) {
      for (const auto* list : {&obj.wires, &obj.isocurves})
        for (const auto& wire : *list) {
          gl->glBegin(GL_LINE_STRIP);
          for (const auto& q : wire) gl->glVertex3d(q.x, q.y, q.z);
          gl->glEnd();
        }
      if (!obj.points.empty()) {
        gl->glPointSize(5.0f);
        gl->glBegin(GL_POINTS);
        for (const auto& q : obj.points) gl->glVertex3d(q.x, q.y, q.z);
        gl->glEnd();
      }
    }
    gl->glLineWidth(1.0f);
  }

  for (int k = 0; k < clips; k++) gl->glDisable(GL_CLIP_PLANE0 + k);

  // Elastico dall'ultimo punto al cursore durante un comando
  const Settings& S = Shared();
  gl->glDisable(GL_DEPTH_TEST);
  m_cursorInside = underMouse() || m_touch.pointMode;
  if (S.DynActive() && m_cursorInside) {
    m_hoverSnapLabel.clear();
    m_hoverWorld = InputPoint(m_hover, &m_hoverSnapped, &m_hoverSnapLabel);
  }
  // (col rettangolo dell'input dinamico l'elastico diagonale non serve)
  if (S.waitingForPoint && S.hasLastPoint && m_cursorInside && !(S.DynActive() && S.dyn.mode == cad::DynMode::Delta)) {
    gl->glColor3ub(S.colors.overlay.red(), S.colors.overlay.green(), S.colors.overlay.blue());
    gl->glLineWidth(1.0f);
    gl->glBegin(GL_LINES);
    gl->glVertex3d(S.lastPoint.x, S.lastPoint.y, S.lastPoint.z);
    gl->glVertex3d(m_hoverWorld.x, m_hoverWorld.y, m_hoverWorld.z);
    gl->glEnd();
  }

  // Sovrapposizioni 2D: titolo, icona assi, rettangolo di selezione, snap
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  DrawLabels(p);
  DrawGrips(p);
  DrawGumball(p);
  p.setPen(Shared().colors.overlay);
  QFont f = p.font(); f.setBold(true); p.setFont(f);
  QString title = KindName(m_cam.kind);
  p.drawText(8, 16, title);
  {  // freccia del menu della vista
    const int tx = 8 + p.fontMetrics().horizontalAdvance(title) + 8;
    QPolygonF tri({QPointF(tx, 9), QPointF(tx + 8, 9), QPointF(tx + 4, 14)});
    p.setBrush(Shared().colors.overlay);
    p.drawPolygon(tri);
    p.setBrush(Qt::NoBrush);
  }
  // icona assi del mondo in basso a sinistra
  const QPointF o(28, h - 28);
  auto axis = [&](ON_3dVector v, QColor c, const char* name) {
    const double x = v * m_cam.Right(), y = v * m_cam.Up();
    if (std::fabs(x) < 1e-6 && std::fabs(y) < 1e-6) return;
    QPointF tip = o + QPointF(x * 18, -y * 18);
    p.setPen(QPen(c, 2)); p.drawLine(o, tip);
    p.drawText(tip + QPointF(2, 0), name);
  };
  axis(ON_3dVector::XAxis, QColor(200, 0, 0), "x");
  axis(ON_3dVector::YAxis, QColor(0, 150, 0), "y");
  axis(ON_3dVector::ZAxis, QColor(0, 0, 200), "z");
  if (m_windowSelecting) {
    const bool crossing = m_hover.x() < m_pressPos.x();
    QPen pen(Shared().colors.overlay, 1, crossing ? Qt::DashLine : Qt::SolidLine);
    p.setPen(pen);
    p.setBrush(crossing ? QColor(80, 200, 80, 40) : QColor(80, 80, 200, 40));
    p.drawRect(QRect(m_pressPos, m_hover).normalized());
  }
  DrawDynamicInput(p);
  if (m_hoverSnapped && m_cursorInside) {
    double sx, sy;
    if (m_cam.Project(m_hoverWorld, w, h, sx, sy)) {
      p.setPen(QPen(QColor(255, 255, 255), 2)); p.setBrush(Qt::NoBrush);
      p.drawRect(QRectF(sx - 5, sy - 5, 10, 10));
      if (!m_hoverSnapLabel.isEmpty()) {
        QFont lf = p.font(); lf.setBold(false); p.setFont(lf);
        const QRectF tr(sx + 10, sy + 6, p.fontMetrics().horizontalAdvance(m_hoverSnapLabel) + 8, 16);
        p.setPen(QColor(40, 40, 40)); p.setBrush(QColor(255, 255, 225));
        p.drawRect(tr);
        p.drawText(tr, Qt::AlignCenter, m_hoverSnapLabel);
      }
    }
  }
  if (hasFocus()) {
    QColor fc = Shared().colors.selected.isValid() ? QColor(59, 130, 246) : Shared().colors.overlay;
    p.setPen(QPen(fc, 2)); p.setBrush(Qt::NoBrush);
    p.drawRect(rect().adjusted(1, 1, -1, -1));
  }
}

// Testi di testi, quote e direttrici: ogni riga e' disegnata con una
// trasformazione affine che porta il sistema del testo (base, direzione x/y)
// sullo schermo, cosi' ruota e scala con la vista come in Rhino.
void Viewport::DrawLabels(QPainter& p) const {
  const int W = width(), H = height();
  for (const auto& obj : m_scene.Objects()) {
    if (obj.labels.empty()) continue;
    QColor c = obj.selected ? Shared().colors.selected : obj.locked ? QColor(128, 128, 128) : Shared().ObjectColor(obj.color);
    for (const auto& L : obj.labels) {
      if (L.dot) {  // punto di testo: riquadro pieno del colore dell'oggetto, testo bianco
        double sx, sy;
        if (!m_cam.Project(L.origin, W, H, sx, sy)) continue;
        QFont df = p.font();
        df.setPixelSize(12);
        df.setBold(false);
        const QFontMetrics fm(df);
        const QString t = QString::fromUtf8(L.text.c_str());
        const QRectF r(sx - fm.horizontalAdvance(t) / 2.0 - 4, sy - fm.height() / 2.0 - 2, fm.horizontalAdvance(t) + 8, fm.height() + 4);
        p.save();
        p.setFont(df);
        p.setPen(Qt::NoPen);
        p.setBrush(obj.selected ? QColor(255, 255, 0) : (c == QColor(0, 0, 0) ? QColor(0, 0, 0) : c));
        p.drawRoundedRect(r, 3, 3);
        p.setPen(obj.selected ? QColor(0, 0, 0) : QColor(255, 255, 255));
        p.drawText(r, Qt::AlignCenter, t);
        p.restore();
        continue;
      }
      const QStringList lines = QString::fromUtf8(L.text.c_str()).split('\n');
      for (int i = 0; i < lines.size(); i++) {
        const ON_3dPoint o = L.origin - L.ydir * (i * 1.6 * L.height);
        double ox, oy, xx, xy, yx, yy;
        if (!m_cam.Project(o, W, H, ox, oy) || !m_cam.Project(o + L.xdir * L.height, W, H, xx, xy) ||
            !m_cam.Project(o + L.ydir * L.height, W, H, yx, yy)) continue;
        const double px = std::hypot(yx - ox, yy - oy);
        if (px < 2.0 || px > 4000) continue;  // troppo piccolo o enorme: non leggibile
        const double F = 100.0;  // corpo del carattere di riferimento, in pixel
        QFont font(L.font.empty() ? QStringLiteral("Arial") : QString::fromUtf8(L.font.c_str()));
        font.setPixelSize(int(F));
        font.setBold(L.bold);
        font.setItalic(L.italic);
        const QFontMetricsF fm(font);
        const double capH = fm.capHeight() > 0 ? fm.capHeight() : 0.7 * F;  // altezza testo Rhino = maiuscole
        const double k = 1.0 / capH;
        QTransform t((xx - ox) * k, (xy - oy) * k, -(yx - ox) * k, -(yy - oy) * k, ox, oy);
        const double wdt = fm.horizontalAdvance(lines[i]);
        const double x0 = L.halign == 1 ? -wdt / 2 : L.halign == 2 ? -wdt : 0;
        p.save();
        p.setTransform(t);
        p.setFont(font);
        p.setPen(c);
        p.drawText(QPointF(x0, 0), lines[i]);
        p.restore();
      }
    }
  }
}


// ---------------------------------------------------------------- Gumball
// Come il Gumball di Rhino 8: frecce (sposta), archi (ruota) e quadratini
// (scala in una direzione) nel centro della scatola degli oggetti selezionati.
// Il trascinamento mostra l'anteprima; al rilascio si esegue il comando
// equivalente, cosi' l'operazione si annulla con Undo come le altre.
// (valori letterali: le costanti di openNURBS potrebbero non essere ancora inizializzate)
static const ON_3dVector kAxes[3] = {ON_3dVector(1, 0, 0), ON_3dVector(0, 1, 0), ON_3dVector(0, 0, 1)};

bool Viewport::GumballFrame(ON_3dPoint& center, double& size) const {
  const Settings& S = Shared();
  if (!S.gumball || S.waitingForPoint) return false;
  ON_BoundingBox bb;
  for (const auto& o : m_scene.Objects())
    if (o.selected && o.bbox.IsValid()) bb.Union(o.bbox);
  if (!bb.IsValid()) return false;
  center = bb.Center();
  // dimensione costante sullo schermo: ~90 pixel
  double upp = m_cam.UnitsPerPixel(height());
  if (m_cam.kind == Camera::Kind::Perspective) {
    const double z = (center - m_cam.Eye()) * m_cam.Dir();
    if (z <= 0) return false;
    upp = 2.0 * z * std::tan(m_cam.fovY / 2.0) / std::max(1, height());
  }
  size = 90.0 * upp;
  return true;
}

// Assi del Gumball: mondo, CPlane della vista o oggetto (un solo oggetto selezionato: piano
// della curva piana, profilo dell'estrusione, prima faccia piana, trasformazione del blocco).
void Viewport::GumballAxes(ON_3dVector ax[3]) const {
  for (int i = 0; i < 3; i++) ax[i] = kAxes[i];
  const Settings& S = Shared();
  ON_Plane pl;
  bool ok = false;
  if (S.gumballAlign == 1) {
    pl = m_cam.CPlane();
    ok = pl.IsValid();
  } else if (S.gumballAlign == 2) {
    const DisplayObject* only = nullptr;
    int n = 0;
    for (const auto& o : m_scene.Objects())
      if (o.selected) { only = &o; n++; }
    const ON_Geometry* g = (n == 1 && only) ? m_scene.Geometry(only->id) : nullptr;
    if (const auto* ir = ON_InstanceRef::Cast(g)) {
      ON_3dVector x(ir->m_xform[0][0], ir->m_xform[1][0], ir->m_xform[2][0]);
      ON_3dVector y(ir->m_xform[0][1], ir->m_xform[1][1], ir->m_xform[2][1]);
      if (x.Unitize() && y.Unitize()) { pl = ON_Plane(ON_3dPoint::Origin, x, y); ok = pl.IsValid(); }
    } else if (const auto* x = ON_Extrusion::Cast(g)) {
      ON_3dVector z = x->m_path.Direction(), y = x->m_up;
      if (z.Unitize() && y.Unitize()) { pl = ON_Plane(ON_3dPoint::Origin, ON_CrossProduct(y, z), y); ok = pl.IsValid(); }
    } else if (const auto* c = ON_Curve::Cast(g)) {
      if (c->IsPlanar(&pl, 1e-6) && !c->IsLinear(1e-6)) {
        // asse x lungo la tangente iniziale (nel piano della curva)
        ON_3dVector t = c->TangentAt(c->Domain()[0]);
        t = t - pl.zaxis * (t * pl.zaxis);
        if (t.Unitize()) pl = ON_Plane(pl.origin, t, ON_CrossProduct(pl.zaxis, t));
        ok = pl.IsValid();
      } else if (c->IsLinear(1e-6)) {
        ON_3dVector t = c->PointAtEnd() - c->PointAtStart();
        if (t.Unitize()) {
          ON_3dVector y = std::fabs(t.z) < 0.9 ? ON_CrossProduct(ON_3dVector::ZAxis, t) : ON_CrossProduct(t, ON_3dVector::XAxis);
          if (y.Unitize()) { pl = ON_Plane(ON_3dPoint::Origin, t, y); ok = pl.IsValid(); }
        }
      }
    } else if (const auto* b = ON_Brep::Cast(g)) {
      for (int i = 0; i < b->m_F.Count() && !ok; i++) {
        ON_Plane fp;
        if (b->m_F[i].IsPlanar(&fp, 1e-6)) {
          pl = fp;
          if (b->m_F[i].m_bRev) pl.Flip();
          ok = pl.IsValid();
        }
      }
    } else if (const auto* an = ON_Annotation::Cast(g)) {
      pl = an->Plane();
      ok = pl.IsValid();
    }
  }
  if (ok) {
    ax[0] = pl.xaxis;
    ax[1] = pl.yaxis;
    ax[2] = pl.zaxis;
  }
}

static double SegDist(double px, double py, double ax, double ay, double bx, double by) {
  const double dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
  double t = l2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0;
  t = std::clamp(t, 0.0, 1.0);
  return std::hypot(px - (ax + t * dx), py - (ay + t * dy));
}

// Arco di rotazione attorno all'asse i: quarto di cerchio tra gli altri due assi.
static std::vector<ON_3dPoint> GumballArc(const ON_3dPoint& c, int i, double r, const ON_3dVector* ax) {
  const ON_3dVector u = ax[(i + 1) % 3], v = ax[(i + 2) % 3];
  std::vector<ON_3dPoint> pts;
  for (int k = 0; k <= 16; k++) {
    const double a = 0.5 * ON_PI * k / 16;
    pts.push_back(c + u * (r * std::cos(a)) + v * (r * std::sin(a)));
  }
  return pts;
}

Viewport::GumballHit Viewport::GumballPick(const QPoint& pos) const {
  ON_3dPoint c;
  double L;
  GumballHit best;
  if (!GumballFrame(c, L)) return best;
  ON_3dVector kAx[3];
  GumballAxes(kAx);
  const int W = width(), H = height();
  double cx, cy;
  if (!m_cam.Project(c, W, H, cx, cy)) return best;
  double bestD = 7.0;
  for (int i = 0; i < 3; i++) {
    double tx, ty, sx, sy;
    if (!m_cam.Project(c + kAx[i] * L, W, H, tx, ty)) continue;
    if (std::hypot(tx - cx, ty - cy) < 12) continue;  // asse visto di punta: non utilizzabile
    // quadratino di scala a 0.6 della freccia
    if (m_cam.Project(c + kAx[i] * (0.6 * L), W, H, sx, sy)) {
      const double d = std::max(std::fabs(sx - pos.x()), std::fabs(sy - pos.y()));
      if (d <= 6 && d < bestD) { bestD = d; best = {3, i}; }
    }
    const double d = SegDist(pos.x(), pos.y(), cx + (tx - cx) * 0.7, cy + (ty - cy) * 0.7, tx, ty);
    if (d < bestD) { bestD = d; best = {1, i}; }
  }
  if (best.mode) return best;
  for (int i = 0; i < 3; i++) {
    const auto arc = GumballArc(c, i, 0.8 * L, kAx);
    for (size_t k = 0; k + 1 < arc.size(); k++) {
      double ax, ay, bx, by;
      if (!m_cam.Project(arc[k], W, H, ax, ay) || !m_cam.Project(arc[k + 1], W, H, bx, by)) continue;
      const double d = SegDist(pos.x(), pos.y(), ax, ay, bx, by);
      if (d < bestD) { bestD = d; best = {2, i}; }
    }
  }
  return best;
}

bool Viewport::GumballValue(const QPoint& pos, double& value) const {
  ON_3dPoint o;
  ON_3dVector d;
  m_cam.Ray(pos.x(), pos.y(), width(), height(), o, d);
  const ON_3dVector a = m_gbAxes[m_gbDrag.axis];
  if (m_gbDrag.mode == 2) {
    // angolo nel piano perpendicolare all'asse
    const double den = d * a;
    if (std::fabs(den) < 1e-6) return false;
    const double t = ((m_gbCenter - o) * a) / den;
    const ON_3dVector v = (o + d * t) - m_gbCenter;
    const ON_3dVector u1 = m_gbAxes[(m_gbDrag.axis + 1) % 3], u2 = m_gbAxes[(m_gbDrag.axis + 2) % 3];
    value = std::atan2(v * u2, v * u1);
    return true;
  }
  // punto dell'asse piu' vicino al raggio del mouse
  const ON_3dVector w0 = m_gbCenter - o;
  const double b = a * d, dd = d * w0, e = a * w0;
  const double den = 1.0 - b * b;
  if (std::fabs(den) < 1e-9) return false;
  value = (b * dd - e) / den;
  return true;
}

ON_Xform Viewport::GumballXform() const {
  const ON_3dVector a = m_gbAxes[std::max(0, m_gbDrag.axis)];
  if (m_gbDrag.mode == 1) return ON_Xform::TranslationTransformation(a * (m_gbCurrent - m_gbStart));
  if (m_gbDrag.mode == 2) {
    ON_Xform r;
    r.Rotation(m_gbCurrent - m_gbStart, a, m_gbCenter);
    return r;
  }
  if (m_gbDrag.mode == 3 && std::fabs(m_gbStart) > 1e-12) {
    // scala lungo l'asse a (qualsiasi direzione): X = c + (I + (f-1) a a^T)(P - c)
    const double f = m_gbCurrent / m_gbStart;
    ON_Xform x = ON_Xform::IdentityTransformation;
    const double v[3] = {a.x, a.y, a.z};
    for (int r = 0; r < 3; r++)
      for (int k = 0; k < 3; k++) x[r][k] += (f - 1) * v[r] * v[k];
    const ON_3dPoint c = m_gbCenter;
    const ON_3dPoint xc = x * c;
    x[0][3] = c.x - xc.x;
    x[1][3] = c.y - xc.y;
    x[2][3] = c.z - xc.z;
    return x;
  }
  return ON_Xform::IdentityTransformation;
}

void Viewport::DrawGumball(QPainter& p) const {
  ON_3dPoint c;
  double L;
  if (!GumballFrame(c, L)) return;
  ON_3dVector kAx[3];
  if (m_gbDrag.mode) for (int i = 0; i < 3; i++) kAx[i] = m_gbAxes[i];
  else GumballAxes(kAx);
  const int W = width(), H = height();
  double cx, cy;
  if (!m_cam.Project(c, W, H, cx, cy)) return;
  const QColor col[3] = {QColor(220, 40, 40), QColor(40, 170, 40), QColor(40, 70, 220)};
  const GumballHit hov = m_gbDrag.mode ? m_gbDrag : GumballPick(m_hover);
  p.save();
  for (int i = 0; i < 3; i++) {
    const bool hot = hov.axis == i;
    // archi di rotazione
    const auto arc = GumballArc(c, i, 0.8 * L, kAx);
    QPolygonF poly;
    for (const auto& q : arc) { double x, y; if (m_cam.Project(q, W, H, x, y)) poly << QPointF(x, y); }
    p.setPen(QPen(col[i], (hot && hov.mode == 2) ? 4 : 2));
    p.setBrush(Qt::NoBrush);
    p.drawPolyline(poly);
    double tx, ty;
    if (!m_cam.Project(c + kAx[i] * L, W, H, tx, ty) || std::hypot(tx - cx, ty - cy) < 12) continue;
    // freccia
    p.setPen(QPen(col[i], (hot && hov.mode == 1) ? 4 : 2));
    p.drawLine(QPointF(cx, cy), QPointF(tx, ty));
    QPointF dir(tx - cx, ty - cy);
    const double len = std::hypot(dir.x(), dir.y());
    dir /= len;
    const QPointF nrm(-dir.y(), dir.x());
    const QPointF tip(tx, ty), base = tip - dir * 12;
    p.setBrush(col[i]);
    p.drawPolygon(QPolygonF({tip + dir * 4, base + nrm * 5, base - nrm * 5}));
    // quadratino di scala
    double sx, sy;
    if (m_cam.Project(c + kAx[i] * (0.6 * L), W, H, sx, sy)) {
      p.setPen(Qt::NoPen);
      const double r = (hot && hov.mode == 3) ? 6 : 4;
      p.drawRect(QRectF(sx - r, sy - r, 2 * r, 2 * r));
    }
  }
  p.setPen(Qt::NoPen);
  p.setBrush(QColor(255, 255, 255));
  p.drawEllipse(QPointF(cx, cy), 4, 4);
  if (m_gbDrag.mode) {  // valore durante il trascinamento, come Rhino
    QString t;
    if (m_gbDrag.mode == 1) t = QString::number(m_gbCurrent - m_gbStart, 'f', 3);
    else if (m_gbDrag.mode == 2) t = QString::number((m_gbCurrent - m_gbStart) * 180.0 / ON_PI, 'f', 2) + "°";
    else if (std::fabs(m_gbStart) > 1e-12) t = "× " + QString::number(m_gbCurrent / m_gbStart, 'f', 3);
    p.setPen(Shared().colors.ink);
    p.drawText(QPointF(m_hover.x() + 14, m_hover.y() - 8), t);
  }
  p.restore();
}

// Osnap persistenti, come la barra Osnap di Rhino 8. Raggio di cattura 10 pixel.
// Priorita': il candidato piu' vicino al cursore; "Vicino" solo se non c'e' altro.
bool Viewport::FindOsnap(const QPoint& pos, ON_3dPoint& out, QString& label) const {
  const Settings& S = Shared();
  const unsigned m = S.osnapMask;
  const double R = m_snapRadius;
  const int W = width(), H = height();
  double best = R;
  bool found = false;
  auto consider = [&](const ON_3dPoint& p, const char* name, double extra = 0) {
    double sx, sy;
    if (!m_cam.Project(p, W, H, sx, sy)) return;
    const double dd = std::hypot(sx - pos.x(), sy - pos.y()) + extra;
    if (dd < best) { best = dd; out = p; label = name; found = true; }
  };
  auto kindBit = [](SnapKind k) -> unsigned {
    switch (k) {
      case SnapKind::End: return End;
      case SnapKind::Mid: return Mid;
      case SnapKind::Center: return Cen;
      case SnapKind::Quad: return Quad;
      case SnapKind::Knot: return Knot;
      case SnapKind::Point: return Point;
      case SnapKind::Vertex: return Vertex;
    }
    return 0;
  };
  for (const auto& obj : m_scene.Objects())
    for (const auto& sp : obj.snapPoints)
      if (m & kindBit(sp.kind)) consider(sp.p, SnapName(sp.kind));

  // Segmenti dei wire vicini al cursore (per Int, Perp, Near)
  struct Seg { ON_3dPoint a, b; double ax, ay, bx, by; size_t obj, wire; };
  std::vector<Seg> nearSegs;
  if (m & (Int | Perp | Near)) {
    for (size_t oi = 0; oi < m_scene.Objects().size(); oi++) {
      const auto& obj = m_scene.Objects()[oi];
      for (size_t wi = 0; wi < obj.wires.size(); wi++) {
        const auto& w = obj.wires[wi];
        for (size_t i = 0; i + 1 < w.size(); i++) {
          double ax, ay, bx, by;
          if (!m_cam.Project(w[i], W, H, ax, ay) || !m_cam.Project(w[i + 1], W, H, bx, by)) continue;
          const double dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
          double t = l2 > 0 ? ((pos.x() - ax) * dx + (pos.y() - ay) * dy) / l2 : 0;
          t = std::clamp(t, 0.0, 1.0);
          if (std::hypot(pos.x() - (ax + t * dx), pos.y() - (ay + t * dy)) <= R)
            nearSegs.push_back({w[i], w[i + 1], ax, ay, bx, by, oi, wi});
        }
      }
    }
  }
  if (m & Int) {
    // intersezione (apparente nella vista) tra segmenti di curve diverse
    for (size_t i = 0; i < nearSegs.size(); i++)
      for (size_t j = i + 1; j < nearSegs.size(); j++) {
        const Seg &A = nearSegs[i], &B = nearSegs[j];
        if (A.obj == B.obj && A.wire == B.wire) continue;
        const double rx = A.bx - A.ax, ry = A.by - A.ay, qx = B.bx - B.ax, qy = B.by - B.ay;
        const double den = rx * qy - ry * qx;
        if (std::fabs(den) < 1e-9) continue;
        const double t = ((B.ax - A.ax) * qy - (B.ay - A.ay) * qx) / den;
        const double u = ((B.ax - A.ax) * ry - (B.ay - A.ay) * rx) / den;
        if (t < -1e-6 || t > 1 + 1e-6 || u < -1e-6 || u > 1 + 1e-6) continue;
        const ON_3dPoint pa = A.a + t * (A.b - A.a), pb = B.a + u * (B.b - B.a);
        consider(0.5 * (pa + pb), "Intersezione");
      }
  }
  if ((m & Perp) && S.hasLastPoint) {
    for (const Seg& sg : nearSegs) {
      const ON_3dVector v = sg.b - sg.a;
      const double l2 = v * v;
      if (l2 <= 0) continue;
      const double t = ((S.lastPoint - sg.a) * v) / l2;
      if (t < 0 || t > 1) continue;
      consider(sg.a + t * v, "Perpendicolare");
    }
  }
  if (!found && (m & Near)) {
    for (const Seg& sg : nearSegs) {
      const double dx = sg.bx - sg.ax, dy = sg.by - sg.ay, l2 = dx * dx + dy * dy;
      double t = l2 > 0 ? ((pos.x() - sg.ax) * dx + (pos.y() - sg.ay) * dy) / l2 : 0;
      t = std::clamp(t, 0.0, 1.0);
      consider(sg.a + t * (sg.b - sg.a), "Vicino");
    }
  }
  return found;
}

ON_3dPoint Viewport::PointUnderMouse(const QPoint& pos, bool* snapped, bool allowSnap, QString* label) const {
  const Settings& S = Shared();
  if (snapped) *snapped = false;
  if (S.osnap && allowSnap) {
    ON_3dPoint sp;
    QString name;
    if (FindOsnap(pos, sp, name)) {
      if (snapped) *snapped = true;
      if (label) *label = name;
      return sp;
    }
  }
  ON_3dPoint o; ON_3dVector d;
  m_cam.Ray(pos.x(), pos.y(), width(), height(), o, d);
  const ON_Plane cp = m_cam.CPlane();
  const double den = d * cp.zaxis;
  ON_3dPoint p = cp.origin;
  if (std::fabs(den) > 1e-12) p = o + d * (((cp.origin - o) * cp.zaxis) / den);
  double u = (p - cp.origin) * cp.xaxis, v = (p - cp.origin) * cp.yaxis;
  if (S.ortho && S.hasLastPoint) {
    const double lu = (S.lastPoint - cp.origin) * cp.xaxis, lv = (S.lastPoint - cp.origin) * cp.yaxis;
    if (std::fabs(u - lu) > std::fabs(v - lv)) v = lv; else u = lu;
  }
  if (S.gridSnap && S.gridSpacing > 0) {
    u = std::round(u / S.gridSpacing) * S.gridSpacing;
    v = std::round(v / S.gridSpacing) * S.gridSpacing;
  }
  return cp.PointAt(u, v);
}

ON_3dPoint Viewport::PointOnObjects(const QPoint& pos, bool* hit) const {
  if (hit) *hit = false;
  // 1) curve e spigoli entro 8 pixel: punto sul segmento piu' vicino
  double best = 8.0;
  ON_3dPoint bestP;
  bool found = false;
  for (const auto& obj : m_scene.Objects()) {
    if (obj.locked) continue;
    for (const auto& wire : obj.wires)
      for (size_t i = 0; i + 1 < wire.size(); i++) {
        double ax, ay, bx, by;
        if (!m_cam.Project(wire[i], width(), height(), ax, ay) || !m_cam.Project(wire[i + 1], width(), height(), bx, by)) continue;
        const double dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
        double t = l2 > 0 ? ((pos.x() - ax) * dx + (pos.y() - ay) * dy) / l2 : 0;
        t = std::clamp(t, 0.0, 1.0);
        const double d = std::hypot(pos.x() - (ax + t * dx), pos.y() - (ay + t * dy));
        if (d < best) { best = d; bestP = wire[i] + t * (wire[i + 1] - wire[i]); found = true; }
      }
  }
  if (found) { if (hit) *hit = true; return bestP; }
  // 2) superfici: primo triangolo colpito dal raggio (Moller-Trumbore)
  ON_3dPoint o; ON_3dVector d;
  m_cam.Ray(pos.x(), pos.y(), width(), height(), o, d);
  double tBest = 1e300;
  for (const auto& obj : m_scene.Objects()) {
    if (obj.locked || !obj.shaded) continue;
    const ON_Mesh& m = *obj.shaded;
    auto tri = [&](int a, int b, int c) {
      const ON_3dPoint A(m.m_V[a]), B(m.m_V[b]), C(m.m_V[c]);
      const ON_3dVector e1 = B - A, e2 = C - A, p = ON_CrossProduct(d, e2);
      const double det = e1 * p;
      if (std::fabs(det) < 1e-14) return;
      const ON_3dVector s = o - A;
      const double u = (s * p) / det;
      if (u < 0 || u > 1) return;
      const ON_3dVector q = ON_CrossProduct(s, e1);
      const double v = (d * q) / det;
      if (v < 0 || u + v > 1) return;
      const double t = (e2 * q) / det;
      if (t > 0 && t < tBest) tBest = t;
    };
    for (int i = 0; i < m.m_F.Count(); i++) {
      const ON_MeshFace& f = m.m_F[i];
      tri(f.vi[0], f.vi[1], f.vi[2]);
      if (f.IsQuad()) tri(f.vi[0], f.vi[2], f.vi[3]);
    }
  }
  if (tBest < 1e300) { if (hit) *hit = true; return o + d * tBest; }
  return PointUnderMouse(pos, nullptr, false);
}

std::vector<ON_UUID> Viewport::PickAt(const QPoint& pos) const {
  double best = 6.0;
  ON_UUID bestId = ON_nil_uuid;
  auto segDist = [](double px, double py, double ax, double ay, double bx, double by) {
    const double dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
    double t = l2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0;
    t = std::clamp(t, 0.0, 1.0);
    return std::hypot(px - (ax + t * dx), py - (ay + t * dy));
  };
  for (const auto& obj : m_scene.Objects()) {
    if (obj.locked) continue;
    double dmin = 1e9;
    for (const auto* list : {&obj.wires, &obj.isocurves})
      for (const auto& wire : *list)
        for (size_t i = 0; i + 1 < wire.size(); i++) {
          double ax, ay, bx, by;
          if (!m_cam.Project(wire[i], width(), height(), ax, ay) || !m_cam.Project(wire[i + 1], width(), height(), bx, by)) continue;
          dmin = std::min(dmin, segDist(pos.x(), pos.y(), ax, ay, bx, by));
        }
    for (const auto& p : obj.points) {
      double sx, sy;
      if (m_cam.Project(p, width(), height(), sx, sy)) dmin = std::min(dmin, std::hypot(sx - pos.x(), sy - pos.y()));
    }
    for (const auto& L : obj.labels) {  // testo: clic dentro il riquadro stimato
      const double wd = 0.75 * L.height * std::max<size_t>(1, L.text.size());
      const double x0 = L.halign == 1 ? -wd / 2 : L.halign == 2 ? -wd : 0;
      for (double f = 0; f <= 1.0; f += 0.5) {
        const ON_3dPoint a = L.origin + L.xdir * x0 + L.ydir * (f * L.height), b = a + L.xdir * wd;
        double ax, ay, bx, by;
        if (m_cam.Project(a, width(), height(), ax, ay) && m_cam.Project(b, width(), height(), bx, by))
          dmin = std::min(dmin, segDist(pos.x(), pos.y(), ax, ay, bx, by));
      }
    }
    if (dmin < best) { best = dmin; bestId = obj.id; }
  }
  if (ON_UuidIsNil(bestId)) return {};
  return {bestId};
}

// Finestra (sinistra->destra): oggetti interamente dentro.
// Intersezione (destra->sinistra): oggetti che toccano il rettangolo.
std::vector<ON_UUID> Viewport::PickWindow(const QPoint& a, const QPoint& b) const {
  const QRect r = QRect(a, b).normalized();
  const bool crossing = b.x() < a.x();
  std::vector<ON_UUID> out;
  for (const auto& obj : m_scene.Objects()) {
    if (obj.locked) continue;
    bool any = false, all = true;
    auto test = [&](const ON_3dPoint& p) {
      double sx, sy;
      if (!m_cam.Project(p, width(), height(), sx, sy)) { all = false; return; }
      if (r.contains(QPoint(int(sx), int(sy)))) any = true; else all = false;
    };
    for (const auto& w : obj.wires) for (const auto& p : w) test(p);
    for (const auto& p : obj.points) test(p);
    for (const auto& L : obj.labels) test(L.origin);
    if (crossing ? any : (all && (any))) out.push_back(obj.id);
  }
  return out;
}

bool Viewport::GripPick(const QPoint& pos, GripDrag& out) const {
  double best = 7.0;
  bool found = false;
  for (const auto& obj : m_scene.Objects())
    for (const auto& g : obj.grips) {
      double sx, sy;
      if (!m_cam.Project(g.p, width(), height(), sx, sy)) continue;
      const double d = std::hypot(sx - pos.x(), sy - pos.y());
      if (d < best) {
        best = d;
        out.active = true;
        out.id = obj.id;
        out.i = g.i;
        out.j = g.j;
        out.start = out.cur = g.p;
        found = true;
      }
    }
  return found;
}

// Posizione del punto trascinato: osnap se presente, altrimenti sul piano
// parallelo al piano di costruzione che passa per il punto di partenza.
ON_3dPoint Viewport::GripDragPoint(const QPoint& pos) const {
  ON_3dPoint snapped;
  QString label;
  if (Shared().osnap && FindOsnap(pos, snapped, label)) return snapped;
  ON_3dPoint o;
  ON_3dVector dir;
  m_cam.Ray(pos.x(), pos.y(), width(), height(), o, dir);
  const ON_Plane cp = m_cam.CPlane();
  ON_Plane pl(m_gripDrag.start, cp.xaxis, cp.yaxis);
  const double den = dir * pl.zaxis;
  if (std::fabs(den) < 1e-12) return m_gripDrag.cur;
  ON_3dPoint p = o + dir * (((pl.origin - o) * pl.zaxis) / den);
  if (Shared().gridSnap && Shared().gridSpacing > 0) {
    const double g = Shared().gridSpacing;
    const ON_3dVector v = p - cp.origin;
    const double u = std::round((v * cp.xaxis) / g) * g, w = std::round((v * cp.yaxis) / g) * g;
    const double h = (m_gripDrag.start - cp.origin) * cp.zaxis;
    p = cp.origin + cp.xaxis * u + cp.yaxis * w + cp.zaxis * h;
  }
  return p;
}

void Viewport::DrawGrips(QPainter& p) const {
  p.setRenderHint(QPainter::Antialiasing, false);
  for (const auto& obj : m_scene.Objects())
    for (const auto& g : obj.grips) {
      double sx, sy;
      if (!m_cam.Project(g.p, width(), height(), sx, sy)) continue;
      p.setPen(QPen(QColor(0, 0, 0), 1));
      p.setBrush(QColor(255, 255, 255));
      p.drawRect(QRectF(std::round(sx) - 3, std::round(sy) - 3, 6, 6));
    }
  if (m_gripDrag.active) {
    double sx, sy;
    if (m_cam.Project(m_gripDrag.cur, width(), height(), sx, sy)) {
      p.setPen(QPen(QColor(0, 0, 0), 1));
      p.setBrush(QColor(255, 255, 0));
      p.drawRect(QRectF(std::round(sx) - 3, std::round(sy) - 3, 6, 6));
    }
  }
  p.setBrush(Qt::NoBrush);
  p.setRenderHint(QPainter::Antialiasing, true);
}

void Viewport::mousePressEvent(QMouseEvent* e) {
  setFocus();
  emit Activated(this);
  // menu della vista: tasto destro sul titolo, oppure clic sulla freccia accanto al titolo
  // (il doppio clic sul titolo resta per ingrandire, come Rhino)
  QFont bf = font(); bf.setBold(true);
  const int titleW = QFontMetrics(bf).horizontalAdvance(KindName(m_cam.kind));
  const bool onTitle = e->pos().y() < 22 && e->pos().x() < 8 + titleW + 4;
  const bool onArrow = e->pos().y() < 22 && e->pos().x() >= 8 + titleW + 4 && e->pos().x() < 8 + titleW + 22;
  if ((onTitle && e->button() == Qt::RightButton) || (onArrow && e->button() == Qt::LeftButton)) {
    m_dragButton = Qt::NoButton;
    ShowViewMenu(e->pos());
    return;
  }
  m_pressPos = m_lastPos = e->pos();
  m_dragButton = e->button();
  if (e->button() == Qt::LeftButton && !Shared().waitingForPoint) {
    GripDrag gd;
    if (GripPick(e->pos(), gd)) { m_gripDrag = gd; return; }
  }
  if (e->button() == Qt::LeftButton) {
    const GumballHit h = GumballPick(e->pos());
    double size;
    if (h.mode && GumballFrame(m_gbCenter, size)) {
      m_gbDrag = h;
      m_gbSize = size;
      GumballAxes(m_gbAxes);
      if (!GumballValue(e->pos(), m_gbStart)) m_gbDrag = {};
      m_gbCurrent = m_gbStart;
    }
  }
}

void Viewport::mouseMoveEvent(QMouseEvent* e) {
  const QPoint delta = e->pos() - m_lastPos;
  m_lastPos = e->pos();
  m_hover = e->pos();
  if (m_dragButton == Qt::RightButton) {
    const bool pan = m_cam.kind != Camera::Kind::Perspective || (e->modifiers() & Qt::ShiftModifier);
    if (e->modifiers() & Qt::ControlModifier) {
      const double f = std::pow(1.01, delta.y());
      m_cam.halfHeight *= f; m_cam.distance *= f;
    } else if (pan) {
      const double upp = m_cam.UnitsPerPixel(height());
      m_cam.target = m_cam.target - m_cam.Right() * (delta.x() * upp) + m_cam.Up() * (delta.y() * upp);
    } else {
      m_cam.yaw -= delta.x() * 0.01;
      m_cam.pitch = std::clamp(m_cam.pitch + delta.y() * 0.01, -1.55, 1.55);
    }
    update();
    return;
  }
  if (m_gripDrag.active) {
    m_gripDrag.cur = GripDragPoint(e->pos());
    m_hoverWorld = m_gripDrag.cur;
    emit MouseWorld(m_hoverWorld);
    update();
    return;
  }
  if (m_gbDrag.mode) {
    double v;
    if (GumballValue(e->pos(), v)) m_gbCurrent = v;
    // con lo snap alla griglia lo spostamento procede a passi della griglia
    if (m_gbDrag.mode == 1 && Shared().gridSnap && Shared().gridSpacing > 0) {
      const double g = Shared().gridSpacing;
      m_gbCurrent = m_gbStart + std::round((m_gbCurrent - m_gbStart) / g) * g;
    }
    if (m_gbDrag.mode == 2 && (e->modifiers() & Qt::ShiftModifier)) {  // Shift: passi di 15 gradi
      const double st = ON_PI / 12;
      m_gbCurrent = m_gbStart + std::round((m_gbCurrent - m_gbStart) / st) * st;
    }
    update();
    return;
  }
  if (m_dragButton == Qt::LeftButton && !Shared().waitingForPoint &&
      (e->pos() - m_pressPos).manhattanLength() > 4)
    m_windowSelecting = true;
  m_hoverSnapLabel.clear();
  m_hoverWorld = InputPoint(e->pos(), &m_hoverSnapped, &m_hoverSnapLabel);
  emit MouseWorld(m_hoverWorld);
  update();
}

void Viewport::mouseReleaseEvent(QMouseEvent* e) {
  const bool wasDrag = (e->pos() - m_pressPos).manhattanLength() > 4;
  if (m_gripDrag.active) {
    const GripDrag gd = m_gripDrag;
    m_gripDrag = {};
    m_dragButton = Qt::NoButton;
    if (wasDrag && gd.cur.DistanceTo(gd.start) > 1e-12) {
      char sid[64];
      ON_UuidToString(gd.id, sid);
      emit GumballCommand(QString("-_GripMove %1 %2 %3 %4,%5,%6").arg(sid).arg(gd.i).arg(gd.j)
                              .arg(gd.cur.x, 0, 'g', 12).arg(gd.cur.y, 0, 'g', 12).arg(gd.cur.z, 0, 'g', 12));
    }
    update();
    return;
  }
  if (m_gbDrag.mode) {
    const GumballHit h = m_gbDrag;
    m_gbDrag = {};
    m_dragButton = Qt::NoButton;
    const double dv = m_gbCurrent - m_gbStart;
    auto P = [](const ON_3dPoint& q) { return QString("%1,%2,%3").arg(q.x, 0, 'g', 12).arg(q.y, 0, 'g', 12).arg(q.z, 0, 'g', 12); };
    const ON_3dVector a = m_gbAxes[h.axis];
    if (wasDrag) {
      if (h.mode == 1 && std::fabs(dv) > 1e-12)
        emit GumballCommand("-_Move " + P(ON_3dPoint::Origin) + " " + P(ON_3dPoint::Origin + a * dv));
      else if (h.mode == 2 && std::fabs(dv) > 1e-12)
        emit GumballCommand("-_Rotate3D " + P(m_gbCenter) + " " + P(m_gbCenter + a) + " " + QString::number(dv * 180.0 / ON_PI, 'g', 12));
      else if (h.mode == 3 && std::fabs(m_gbStart) > 1e-12 && std::fabs(m_gbCurrent / m_gbStart) > 1e-9)
        emit GumballCommand("-_Scale1D " + P(m_gbCenter) + " " + QString::number(m_gbCurrent / m_gbStart, 'g', 12) + " " + P(m_gbCenter + a));
    }
    update();
    return;
  }
  if (m_dragButton == Qt::LeftButton) {
    const bool add = e->modifiers() & Qt::ShiftModifier, remove = e->modifiers() & Qt::ControlModifier;
    if (m_windowSelecting) {
      emit ObjectsClicked(PickWindow(m_pressPos, e->pos()), add, remove);
    } else if (Shared().waitingForPoint) {
      const int field = DynamicFieldAt(e->pos());
      if (field >= 0) emit DynamicFieldClicked(field);  // clic su una misura: si scrive in quel campo
      else emit PointPicked(Shared().pickOnObjects ? PointOnObjects(e->pos()) : InputPoint(e->pos()));
    } else if (!wasDrag) {
      emit ObjectsClicked(PickAt(e->pos()), add, remove);
    }
  }
  m_windowSelecting = false;
  m_dragButton = Qt::NoButton;
  update();
}

void Viewport::mouseDoubleClickEvent(QMouseEvent* e) {
  // Doppio clic sul titolo: ingrandisce/ripristina la vista (come Rhino).
  if (e->pos().y() < 22 && e->pos().x() < 120) emit ToggleMaximize(this);
}

void Viewport::wheelEvent(QWheelEvent* e) {
  const double f = e->angleDelta().y() > 0 ? 1.0 / 1.15 : 1.15;
  // zoom verso il cursore
  ON_3dPoint before = PointUnderMouse(e->position().toPoint(), nullptr, false);
  m_cam.halfHeight *= f;
  m_cam.distance *= f;
  if (m_cam.kind != Camera::Kind::Perspective) {
    ON_3dPoint after = PointUnderMouse(e->position().toPoint(), nullptr, false);
    m_cam.target = m_cam.target + (before - after);
  }
  update();
}


// ---------------------------------------------------------------------------
// Input dinamico: misure accanto al cursore (lunghezza/angolo o larghezza/altezza),
// tracciamento polare e vincoli digitati. La matematica e' in core/DynamicInput.
// ---------------------------------------------------------------------------
ON_3dPoint Viewport::InputPoint(const QPoint& pos, bool* snapped, QString* label) {
  Settings& S = Shared();
  bool sn = false;
  const ON_3dPoint raw = PointUnderMouse(pos, &sn, true, label);
  if (snapped) *snapped = sn;
  if (!S.DynActive()) return raw;
  ON_Plane pl = m_cam.CPlane();
  pl.origin = S.lastPoint;
  pl.UpdateEquation();
  S.dyn.opts.ortho = S.ortho;
  S.dyn.hasCursor = true;
  S.dyn.cursor = raw;
  S.dyn.cursorSnapped = sn;
  S.dyn.plane = pl;
  // il valore che si sta scrivendo si vede gia' nell'anteprima (come AutoCAD), senza bloccarlo
  cad::DynFields preview = S.dyn.fields;
  double typed = 0;
  const cad::DynTyped tk = cad::ParseDynamicText(S.dyn.typing.toStdString(), typed);
  if (tk == cad::DynTyped::Number) { preview.locked[preview.active] = true; preview.value[preview.active] = typed; }
  else if (tk == cad::DynTyped::Angle && S.dyn.mode == cad::DynMode::Polar) { preview.locked[1] = true; preview.value[1] = typed; }
  S.dyn.result = cad::SolveDynamicPoint(pl, S.lastPoint, raw, S.dyn.mode, preview, S.dyn.opts, sn);
  // con un vincolo il punto non e' piu' quello dell'osnap
  if (snapped && preview.AnyLocked()) *snapped = false;
  return S.dyn.result.point;
}

int Viewport::DynamicFieldAt(const QPoint& pos) const {
  if (!Shared().DynActive() || !m_cursorInside) return -1;
  for (int i = 0; i < 2; i++)
    if (!m_dynRects[i].isEmpty() && m_dynRects[i].adjusted(-4, -4, 4, 4).contains(pos)) return i;
  return -1;
}

void Viewport::DrawDynamicInput(QPainter& p) {
  m_dynRects[0] = m_dynRects[1] = QRectF();
  const Settings& S = Shared();
  if (!S.DynActive() || !m_cursorInside) return;
  const auto& D = S.dyn;
  const int W = width(), H = height();
  auto scr = [&](const ON_3dPoint& q, QPointF& out) {
    double x, y;
    if (!m_cam.Project(q, W, H, x, y)) return false;
    out = QPointF(x, y);
    return true;
  };
  const ON_Plane& pl = D.plane;
  const ON_3dPoint A3 = S.lastPoint, P3 = D.result.point;
  QPointF A, P;
  if (!scr(A3, A) || !scr(P3, P)) return;
  p.save();
  p.setRenderHint(QPainter::Antialiasing);
  QFont f = p.font();
  f.setBold(false);
  f.setPixelSize(12);
  p.setFont(f);
  const QFontMetrics fm(f);
  const QColor guide = S.colors.dark ? QColor(150, 190, 255) : QColor(30, 90, 200);
  const QColor accent(59, 130, 246);
  auto fmt = [](double v, int dec = 2) {
    QString t = QString::number(v, 'f', dec);
    if (t.contains('.')) { while (t.endsWith('0')) t.chop(1); if (t.endsWith('.')) t.chop(1); }
    if (t == "-0") t = "0";
    return t;
  };
  // campo: valore attuale, testo che si sta scrivendo (campo attivo) o valore bloccato
  auto field = [&](int i, const QString& label, double live, const QString& unit, QPointF at) {
    const bool locked = D.fields.locked[i];
    const bool active = D.fields.active == i;
    QString text;
    if (active && !D.typing.isEmpty()) text = D.typing + "|";
    else text = fmt(locked ? D.fields.value[i] : live) + unit;
    const QString full = label.isEmpty() ? text : label + " " + text;
    const double wd = fm.horizontalAdvance(full) + (locked ? 22 : 12), ht = fm.height() + 6;
    QRectF r(at.x() - wd / 2, at.y() - ht / 2, wd, ht);
    r.moveLeft(std::clamp(r.left(), 2.0, std::max(2.0, W - wd - 2)));
    r.moveTop(std::clamp(r.top(), 2.0, std::max(2.0, H - ht - 2)));
    m_dynRects[i] = r;
    p.setPen(QPen(active ? accent : QColor(120, 120, 120), active ? 2 : 1));
    p.setBrush(locked ? accent : (S.colors.dark ? QColor(40, 42, 46, 235) : QColor(255, 255, 255, 235)));
    p.drawRoundedRect(r, 3, 3);
    QRectF tr = r;
    if (locked) {  // lucchetto: valore bloccato
      const QPointF c(r.left() + 9, r.center().y() + 1);
      p.setPen(QPen(Qt::white, 1.4));
      p.setBrush(Qt::NoBrush);
      p.drawArc(QRectF(c.x() - 3, c.y() - 7, 6, 7), 0, 180 * 16);
      p.setBrush(Qt::white);
      p.drawRect(QRectF(c.x() - 4, c.y() - 3, 8, 6));
      tr.setLeft(r.left() + 14);
    }
    p.setPen(locked ? QColor(Qt::white) : (S.colors.dark ? QColor(230, 230, 230) : QColor(20, 20, 20)));
    p.drawText(tr, Qt::AlignCenter, full);
  };
  QPen dimPen(guide, 1);
  const double upp = m_cam.UnitsPerPixel(H);
  if (D.mode == cad::DynMode::Polar) {
    // tracciamento polare: linea tratteggiata lungo la direzione agganciata
    if (D.result.tracked && !D.fields.locked[1]) {
      const double ar = D.result.trackAngle * ON_PI / 180.0;
      const ON_3dVector dir = pl.xaxis * std::cos(ar) + pl.yaxis * std::sin(ar);
      QPointF far;
      if (scr(A3 + dir * (upp * 4.0 * (W + H)), far)) {
        p.setPen(QPen(guide, 1, Qt::DashLine));
        p.drawLine(A, far);
      }
    }
    // linea di quota parallela all'elastico, spostata di 22 pixel
    const QPointF v = P - A;
    const double l = std::hypot(v.x(), v.y());
    if (l > 8) {
      QPointF nrm(-v.y() / l, v.x() / l);
      if (nrm.y() > 0) nrm = -nrm;  // sopra (sullo schermo)
      const QPointF a = A + nrm * 22, b = P + nrm * 22;
      p.setPen(dimPen);
      p.drawLine(A + nrm * 4, A + nrm * 27);
      p.drawLine(P + nrm * 4, P + nrm * 27);
      p.drawLine(a, b);
      auto tick = [&](QPointF c) { p.drawLine(c - (nrm + v / l) * 4, c + (nrm + v / l) * 4); };
      tick(a);
      tick(b);
      field(0, QString::fromLatin1(cad::DynamicLengthLabel(D.command)), D.result.length, QString(), (a + b) / 2);
      // arco dell'angolo dall'asse x del CPlane, raggio 46 pixel
      const double rW = 46 * upp;
      const double ang = D.result.angle;
      QPointF ref;
      if (scr(A3 + pl.xaxis * (rW * 1.35), ref)) {
        p.setPen(QPen(guide, 1, Qt::DotLine));
        p.drawLine(A, ref);
      }
      QPainterPath arc;
      const int steps = std::max(2, int(std::fabs(ang) / 5));
      for (int i = 0; i <= steps; i++) {
        const double t = ang * i / steps * ON_PI / 180.0;
        QPointF q;
        if (!scr(A3 + (pl.xaxis * std::cos(t) + pl.yaxis * std::sin(t)) * rW, q)) continue;
        if (arc.elementCount() == 0) arc.moveTo(q); else arc.lineTo(q);
      }
      p.setPen(dimPen);
      p.setBrush(Qt::NoBrush);
      p.drawPath(arc);
      const double tm = ang * 0.5 * ON_PI / 180.0;
      QPointF am;
      if (scr(A3 + (pl.xaxis * std::cos(tm) + pl.yaxis * std::sin(tm)) * (rW + 22 * upp), am))
        field(1, QString(), ang, QString::fromUtf8("°"), am);
    }
  } else {
    // Delta: quota della larghezza sotto/sopra il lato orizzontale, dell'altezza accanto al lato verticale
    const ON_3dPoint C3 = A3 + pl.xaxis * D.result.dx;
    QPointF C, E;
    if (scr(C3, C) && scr(A3 + pl.yaxis * D.result.dy, E)) {
      // anteprima del rettangolo
      p.setPen(QPen(S.colors.overlay, 1));
      p.setBrush(Qt::NoBrush);
      p.drawPolygon(QPolygonF({A, C, P, E}));
      p.setPen(dimPen);
      const double sy = (C.y() >= P.y()) ? 1 : -1;  // dalla parte opposta al rettangolo
      const QPointF off(0, 20 * sy);
      p.drawLine(A + off, C + off);
      if (std::fabs(C.x() - A.x()) > 1) field(0, "X", D.result.dx, QString(), (A + C) / 2 + off);
      const double sx = (C.x() >= A.x()) ? 1 : -1;
      const QPointF offx(20 * sx, 0);
      p.setPen(dimPen);
      p.drawLine(C + offx, P + offx);
      if (std::fabs(C.y() - P.y()) > 1) field(1, "Y", D.result.dy, QString(), (C + P) / 2 + offx + QPointF(18 * sx, 0));
      if (m_dynRects[0].isEmpty()) field(0, "X", D.result.dx, QString(), (A + C) / 2 + off);
      if (m_dynRects[1].isEmpty()) field(1, "Y", D.result.dy, QString(), (C + P) / 2 + offx + QPointF(18 * sx, 0));
    }
  }
  // suggerimento accanto al cursore: richiesta del comando (sopra) e tasti (sotto, in grigio)
  {
    const QPointF c = m_hover;
    QString tip = D.prompt;
    if (D.result.tracked && !D.fields.locked[1]) tip = QString("Polare %1°  -  ").arg(fmt(D.result.trackAngle)) + tip;
    const QString keys = D.fields.AnyLocked() ? QString("Invio conferma   Esc sblocca")
                                              : QString("Scrivi un valore   Tab campo successivo");
    QFont tf = f;
    tf.setPixelSize(11);
    p.setFont(tf);
    const QFontMetrics tm(tf);
    const double wd = std::max(tm.horizontalAdvance(tip), tm.horizontalAdvance(keys)) + 12;
    const double ht = 2 * tm.height() + 6;
    // prima posizione libera tra: sotto a destra, sotto a sinistra, sopra a destra, sopra a sinistra, piu' in basso o in alto
    const QPointF cand[] = {{c.x() + 20, c.y() + 20}, {c.x() - 20 - wd, c.y() + 20}, {c.x() + 20, c.y() - 20 - ht},
                            {c.x() - 20 - wd, c.y() - 20 - ht}, {c.x() - wd / 2, c.y() + 60}, {c.x() - wd / 2, c.y() - 60 - ht},
                            {W - wd - 8, H - ht - 8}, {60.0, H - ht - 8}};  // ultimi: angoli in basso della vista
    QRectF r(cand[0], QSizeF(wd, ht));
    for (const QPointF& q : cand) {
      const QRectF t(q, QSizeF(wd, ht));
      if (t.left() < 2 || t.top() < 2 || t.right() > W - 2 || t.bottom() > H - 2) continue;
      if (t.intersects(m_dynRects[0]) || t.intersects(m_dynRects[1])) continue;
      r = t;
      break;
    }
    r.moveLeft(std::clamp(r.left(), 2.0, std::max(2.0, W - wd - 2)));
    r.moveTop(std::clamp(r.top(), 2.0, std::max(2.0, H - ht - 2)));
    p.setPen(QColor(110, 110, 110));
    p.setBrush(S.colors.dark ? QColor(40, 42, 46, 225) : QColor(255, 255, 225, 235));
    p.drawRect(r);
    p.setPen(S.colors.dark ? QColor(225, 225, 225) : QColor(30, 30, 30));
    p.drawText(QRectF(r.left() + 6, r.top() + 2, wd, tm.height()), Qt::AlignLeft | Qt::AlignVCenter, tip);
    p.setPen(QColor(120, 120, 120));
    p.drawText(QRectF(r.left() + 6, r.top() + 3 + tm.height(), wd, tm.height()), Qt::AlignLeft | Qt::AlignVCenter, keys);
  }
  // tocco: mirino sul punto e linea verso il dito (il dito non copre il punto)
  if (m_touch.pointMode) {
    const QPointF c = m_hover;
    p.setPen(QPen(accent, 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawLine(c + QPointF(-14, 0), c + QPointF(14, 0));
    p.drawLine(c + QPointF(0, -14), c + QPointF(0, 14));
    p.setPen(QPen(accent, 1, Qt::DotLine));
    p.drawLine(c, m_touch.finger);
    p.drawEllipse(m_touch.finger, 18, 18);
  }
  p.restore();
}

// ---------------------------------------------------------------------------
// Tocco e Tab
// ---------------------------------------------------------------------------
bool Viewport::event(QEvent* e) {
  switch (e->type()) {
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::TouchCancel:
      return TouchEvent(static_cast<QTouchEvent*>(e));
    case QEvent::KeyPress: {
      auto* k = static_cast<QKeyEvent*>(e);
      if ((k->key() == Qt::Key_Tab || k->key() == Qt::Key_Backtab) && Shared().DynActive()) {
        emit DynamicTab(k->key() == Qt::Key_Backtab);
        return true;
      }
      break;
    }
    default:
      break;
  }
  return QOpenGLWidget::event(e);
}

// Il mirino sta 70 pixel sopra il dito: si vede dove cade il punto e l'osnap ha un raggio
// di cattura piu' grande. Alzando il dito il punto viene dato al comando (come un clic).
bool Viewport::TouchEvent(QTouchEvent* e) {
  if (e->device() && e->device()->type() != QInputDevice::DeviceType::TouchScreen)
    return QOpenGLWidget::event(e);  // touchpad: gestito come mouse da Qt
  const auto& pts = e->points();
  // posizione nella vista dalla posizione globale (vale anche per eventi costruiti a mano)
  auto Local = [this](const QEventPoint& tp) { return QPointF(mapFromGlobal(tp.globalPosition())); };
  int down = 0;
  QPointF center;
  for (const auto& tp : pts)
    if (tp.state() != QEventPoint::State::Released) { down++; center += Local(tp); }
  if (down) center /= down;
  const Qt::KeyboardModifiers mods = Qt::NoModifier;
  auto mouse = [&](QEvent::Type t, const QPointF& at, Qt::MouseButtons buttons) {
    QMouseEvent me(t, at, mapToGlobal(at), Qt::LeftButton, buttons, mods);
    if (t == QEvent::MouseButtonPress) mousePressEvent(&me);
    else if (t == QEvent::MouseMove) mouseMoveEvent(&me);
    else mouseReleaseEvent(&me);
  };
  const QPointF offset(0, -70);
  const bool finished = e->type() == QEvent::TouchEnd || e->type() == QEvent::TouchCancel;

  if (e->type() == QEvent::TouchBegin) {
    m_touch = Touch();
    setFocus();
    emit Activated(this);
    m_touch.finger = pts.isEmpty() ? QPointF() : Local(pts.first());
    m_touch.fingers = down;
    if (Shared().waitingForPoint) {
      m_touch.field = DynamicFieldAt(m_touch.finger.toPoint());
      m_touch.pointMode = m_touch.field < 0;
      m_snapRadius = 22.0;
      m_hover = (m_touch.finger + offset).toPoint();
      m_hoverSnapLabel.clear();
      m_hoverWorld = InputPoint(m_hover, &m_hoverSnapped, &m_hoverSnapLabel);
      emit MouseWorld(m_hoverWorld);
    } else {
      m_touch.mouse = true;
      mouse(QEvent::MouseButtonPress, m_touch.finger, Qt::LeftButton);
    }
    update();
    e->accept();
    return true;
  }
  if (!finished && down >= 2) {
    double dist = 0;
    for (const auto& tp : pts)
      if (tp.state() != QEventPoint::State::Released) dist += QLineF(Local(tp), center).length();
    dist /= down;
    if (!m_touch.gesture) {
      // passa ai gesti: annulla selezione a finestra o punto in corso
      m_touch.gesture = true;
      m_touch.pointMode = false;
      m_touch.mouse = false;
      m_windowSelecting = false;
      m_dragButton = Qt::NoButton;
      m_gripDrag = {};
      m_gbDrag = {};
      m_touch.lastCenter = center;
      m_touch.lastDist = dist;
      m_touch.fingers = down;
    } else {
      const QPointF d = center - m_touch.lastCenter;
      if (down >= 3 && m_cam.kind == Camera::Kind::Perspective) {
        m_cam.yaw -= d.x() * 0.01;
        m_cam.pitch = std::clamp(m_cam.pitch + d.y() * 0.01, -1.55, 1.55);
      } else {
        if (m_touch.lastDist > 1 && dist > 1) {
          const double f = m_touch.lastDist / dist;
          m_cam.halfHeight *= f;
          m_cam.distance *= f;
        }
        const double upp = m_cam.UnitsPerPixel(height());
        m_cam.target = m_cam.target - m_cam.Right() * (d.x() * upp) + m_cam.Up() * (d.y() * upp);
      }
      m_touch.lastCenter = center;
      m_touch.lastDist = dist;
    }
    update();
    e->accept();
    return true;
  }
  const QPointF pos = pts.isEmpty() ? m_touch.finger : Local(pts.first());
  if (!finished) {
    if (m_touch.pointMode) {
      m_touch.finger = pos;
      m_hover = (pos + offset).toPoint();
      m_hoverSnapLabel.clear();
      m_hoverWorld = InputPoint(m_hover, &m_hoverSnapped, &m_hoverSnapLabel);
      emit MouseWorld(m_hoverWorld);
    } else if (m_touch.mouse) {
      mouse(QEvent::MouseMove, pos, Qt::LeftButton);
    }
    update();
    e->accept();
    return true;
  }
  // dito alzato
  if (m_touch.field >= 0 && !m_touch.gesture && e->type() == QEvent::TouchEnd) {
    emit DynamicFieldClicked(m_touch.field);
  } else if (m_touch.pointMode && e->type() == QEvent::TouchEnd && Shared().waitingForPoint) {
    const ON_3dPoint pt = Shared().pickOnObjects ? PointOnObjects(m_hover) : InputPoint(m_hover);
    m_touch.pointMode = false;
    emit PointPicked(pt);
  } else if (m_touch.mouse) {
    mouse(QEvent::MouseButtonRelease, pos, Qt::NoButton);
  }
  m_touch = Touch();
  m_snapRadius = 10.0;
  update();
  e->accept();
  return true;
}

}  // namespace cad::ui
