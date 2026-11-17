#include "ui/Viewport.h"
#include <QMouseEvent>
#include <QOpenGLFunctions_1_1>
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
  }
  return "";
}

ON_3dVector Camera::Dir() const {
  switch (kind) {
    case Kind::Top: return ON_3dVector(0, 0, -1);
    case Kind::Front: return ON_3dVector(0, 1, 0);
    case Kind::Right: return ON_3dVector(-1, 0, 0);
    case Kind::Perspective: break;
  }
  return -ON_3dVector(std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch));
}

ON_3dVector Camera::Up() const {
  if (kind == Kind::Top) return ON_3dVector(0, 1, 0);
  if (kind != Kind::Perspective) return ON_3dVector(0, 0, 1);
  ON_3dVector u = ON_CrossProduct(Right(), Dir());
  u.Unitize();
  return u;
}

ON_3dVector Camera::Right() const {
  if (kind == Kind::Top || kind == Kind::Front) return ON_3dVector(1, 0, 0);
  if (kind == Kind::Right) return ON_3dVector(0, 1, 0);
  ON_3dVector r = ON_CrossProduct(Dir(), ON_3dVector::ZAxis);
  if (!r.Unitize()) r = ON_3dVector::XAxis;
  return r;
}

ON_3dPoint Camera::Eye() const {
  const double d = (kind == Kind::Perspective) ? distance : 1e5;
  return target - Dir() * d;
}

ON_Plane Camera::CPlane() const {
  switch (kind) {
    case Kind::Front: return ON_Plane(ON_3dPoint::Origin, ON_3dVector::XAxis, ON_3dVector::ZAxis);
    case Kind::Right: return ON_Plane(ON_3dPoint::Origin, ON_3dVector::YAxis, ON_3dVector::ZAxis);
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
  shaded = (kind == Camera::Kind::Perspective);
  setMouseTracking(true);
  setFocusPolicy(Qt::ClickFocus);
  setMinimumSize(120, 90);
}

void Viewport::ZoomExtents() {
  m_cam.ZoomExtents(m_scene.Bounds(), width(), height());
  update();
}

void Viewport::initializeGL() {}

namespace {
QOpenGLFunctions_1_1* GL(QOpenGLContext* ctx) {
  return QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_1_1>(ctx);
}
}  // namespace

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
    if (major) gl->glColor3ub(120, 120, 120); else gl->glColor3ub(152, 152, 152);
    ON_3dPoint a = cp.PointAt(i * s, -n * s), b = cp.PointAt(i * s, n * s);
    ON_3dPoint c = cp.PointAt(-n * s, i * s), d = cp.PointAt(n * s, i * s);
    gl->glVertex3d(a.x, a.y, a.z); gl->glVertex3d(b.x, b.y, b.z);
    gl->glVertex3d(c.x, c.y, c.z); gl->glVertex3d(d.x, d.y, d.z);
  }
  // assi del piano di costruzione: X rosso, Y verde
  gl->glColor3ub(150, 75, 75);
  ON_3dPoint a = cp.PointAt(-n * s, 0), b = cp.PointAt(n * s, 0);
  gl->glVertex3d(a.x, a.y, a.z); gl->glVertex3d(b.x, b.y, b.z);
  gl->glColor3ub(75, 150, 75);
  a = cp.PointAt(0, -n * s); b = cp.PointAt(0, n * s);
  gl->glVertex3d(a.x, a.y, a.z); gl->glVertex3d(b.x, b.y, b.z);
  gl->glEnd();
}

void Viewport::paintGL() {
  auto* gl = GL(context());
  const int w = width(), h = height();
  gl->glViewport(0, 0, w * devicePixelRatio(), h * devicePixelRatio());
  gl->glClearColor(190 / 255.f, 190 / 255.f, 190 / 255.f, 1.f);
  gl->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  // Matrici: proiezione e vista calcolate qui (le stesse usate per la selezione).
  const double aspect = double(w) / std::max(1, h);
  gl->glMatrixMode(GL_PROJECTION);
  gl->glLoadIdentity();
  const double far = (m_cam.kind == Camera::Kind::Perspective) ? m_cam.distance * 200 : 4e5;
  if (m_cam.kind == Camera::Kind::Perspective) {
    const double nearP = m_cam.distance * 0.01;
    const double t = nearP * std::tan(m_cam.fovY / 2);
    gl->glFrustum(-t * aspect, t * aspect, -t, t, nearP, far);
  } else {
    gl->glOrtho(-m_cam.halfHeight * aspect, m_cam.halfHeight * aspect, -m_cam.halfHeight, m_cam.halfHeight, 1.0, far);
  }
  gl->glMatrixMode(GL_MODELVIEW);
  const ON_3dVector r = m_cam.Right(), u = m_cam.Up(), d = m_cam.Dir();
  const ON_3dPoint e = m_cam.Eye();
  const double M[16] = {r.x, u.x, -d.x, 0, r.y, u.y, -d.y, 0, r.z, u.z, -d.z, 0,
                        -(r * ON_3dVector(e)), -(u * ON_3dVector(e)), (d * ON_3dVector(e)), 1};
  gl->glLoadMatrixd(M);

  gl->glDisable(GL_LIGHTING);
  gl->glDisable(GL_DEPTH_TEST);
  DrawGrid();
  gl->glEnable(GL_DEPTH_TEST);
  gl->glDepthFunc(GL_LEQUAL);

  // Superfici ombreggiate
  if (shaded) {
    gl->glEnable(GL_LIGHTING);
    gl->glEnable(GL_LIGHT0);
    gl->glEnable(GL_NORMALIZE);
    gl->glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, 1);
    // luce "dalla camera" come la luce predefinita di Rhino
    gl->glMatrixMode(GL_MODELVIEW);
    gl->glPushMatrix();
    gl->glLoadIdentity();
    const float lp[4] = {0.3f, 0.5f, 1.0f, 0.0f};
    gl->glLightfv(GL_LIGHT0, GL_POSITION, lp);
    gl->glPopMatrix();
    const float amb[4] = {0.25f, 0.25f, 0.25f, 1.f}, dif[4] = {0.8f, 0.8f, 0.8f, 1.f};
    gl->glLightfv(GL_LIGHT0, GL_AMBIENT, amb);
    gl->glLightfv(GL_LIGHT0, GL_DIFFUSE, dif);
    gl->glEnable(GL_POLYGON_OFFSET_FILL);
    gl->glPolygonOffset(1.f, 1.f);
    for (const auto& obj : m_scene.Objects()) {
      if (!obj.shaded) continue;
      const float col[4] = {obj.selected ? 0.95f : 0.82f, obj.selected ? 0.9f : 0.82f, obj.selected ? 0.45f : 0.82f, 1.f};
      gl->glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, col);
      const ON_Mesh& m = *obj.shaded;
      const bool hasN = m.HasVertexNormals();
      gl->glBegin(GL_TRIANGLES);
      for (int i = 0; i < m.m_F.Count(); i++) {
        const ON_MeshFace& f = m.m_F[i];
        const int tri[2][3] = {{f.vi[0], f.vi[1], f.vi[2]}, {f.vi[0], f.vi[2], f.vi[3]}};
        for (int t = 0; t < (f.IsQuad() ? 2 : 1); t++)
          for (int k = 0; k < 3; k++) {
            const int vi = tri[t][k];
            if (hasN) gl->glNormal3f(m.m_N[vi].x, m.m_N[vi].y, m.m_N[vi].z);
            gl->glVertex3f(m.m_V[vi].x, m.m_V[vi].y, m.m_V[vi].z);
          }
      }
      gl->glEnd();
    }
    gl->glDisable(GL_POLYGON_OFFSET_FILL);
    gl->glDisable(GL_LIGHTING);
  }

  // Wireframe, punti
  for (int pass = 0; pass < 2; pass++) {  // selezionati per ultimi, sopra
    for (const auto& obj : m_scene.Objects()) {
      if (obj.selected != (pass == 1)) continue;
      if (obj.selected) gl->glColor3ub(255, 255, 0);
      else if (obj.locked) gl->glColor3ub(110, 110, 110);
      else gl->glColor3ub(obj.color.Red(), obj.color.Green(), obj.color.Blue());
      gl->glLineWidth(obj.selected ? 2.0f : 1.0f);
      for (const auto& wire : obj.wires) {
        gl->glBegin(GL_LINE_STRIP);
        for (const auto& p : wire) gl->glVertex3d(p.x, p.y, p.z);
        gl->glEnd();
      }
      gl->glPointSize(5.0f);
      gl->glBegin(GL_POINTS);
      for (const auto& p : obj.points) gl->glVertex3d(p.x, p.y, p.z);
      gl->glEnd();
    }
  }

  // Elastico dall'ultimo punto al cursore durante un comando
  const Settings& S = Shared();
  gl->glDisable(GL_DEPTH_TEST);
  if (S.waitingForPoint && S.hasLastPoint && underMouse()) {
    gl->glColor3ub(0, 0, 0);
    gl->glLineWidth(1.0f);
    gl->glBegin(GL_LINES);
    gl->glVertex3d(S.lastPoint.x, S.lastPoint.y, S.lastPoint.z);
    gl->glVertex3d(m_hoverWorld.x, m_hoverWorld.y, m_hoverWorld.z);
    gl->glEnd();
  }

  // Sovrapposizioni 2D: titolo, icona assi, rettangolo di selezione, snap
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(QColor(30, 30, 30));
  QFont f = p.font(); f.setBold(true); p.setFont(f);
  QString title = KindName(m_cam.kind);
  p.drawText(8, 16, title);
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
    QPen pen(QColor(0, 0, 0), 1, crossing ? Qt::DashLine : Qt::SolidLine);
    p.setPen(pen);
    p.setBrush(crossing ? QColor(80, 200, 80, 40) : QColor(80, 80, 200, 40));
    p.drawRect(QRect(m_pressPos, m_hover).normalized());
  }
  if (m_hoverSnapped && underMouse()) {
    double sx, sy;
    if (m_cam.Project(m_hoverWorld, w, h, sx, sy)) {
      p.setPen(QPen(QColor(255, 255, 255), 2)); p.setBrush(Qt::NoBrush);
      p.drawRect(QRectF(sx - 5, sy - 5, 10, 10));
    }
  }
  if (hasFocus()) {
    p.setPen(QPen(QColor(60, 60, 60), 2)); p.setBrush(Qt::NoBrush);
    p.drawRect(rect().adjusted(1, 1, -1, -1));
  }
}

ON_3dPoint Viewport::PointUnderMouse(const QPoint& pos, bool* snapped, bool allowSnap) const {
  const Settings& S = Shared();
  if (snapped) *snapped = false;
  // Osnap: punto notevole entro 10 pixel
  if (S.osnap && allowSnap) {
    double best = 10.0;
    ON_3dPoint bestP;
    bool found = false;
    for (const auto& obj : m_scene.Objects())
      for (const auto& sp : obj.snapPoints) {
        double sx, sy;
        if (!m_cam.Project(sp, width(), height(), sx, sy)) continue;
        const double dd = std::hypot(sx - pos.x(), sy - pos.y());
        if (dd < best) { best = dd; bestP = sp; found = true; }
      }
    if (found) { if (snapped) *snapped = true; return bestP; }
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
    for (const auto& wire : obj.wires)
      for (size_t i = 0; i + 1 < wire.size(); i++) {
        double ax, ay, bx, by;
        if (!m_cam.Project(wire[i], width(), height(), ax, ay) || !m_cam.Project(wire[i + 1], width(), height(), bx, by)) continue;
        dmin = std::min(dmin, segDist(pos.x(), pos.y(), ax, ay, bx, by));
      }
    for (const auto& p : obj.points) {
      double sx, sy;
      if (m_cam.Project(p, width(), height(), sx, sy)) dmin = std::min(dmin, std::hypot(sx - pos.x(), sy - pos.y()));
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
    if (crossing ? any : (all && (any))) out.push_back(obj.id);
  }
  return out;
}

void Viewport::mousePressEvent(QMouseEvent* e) {
  setFocus();
  m_pressPos = m_lastPos = e->pos();
  m_dragButton = e->button();
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
  if (m_dragButton == Qt::LeftButton && !Shared().waitingForPoint &&
      (e->pos() - m_pressPos).manhattanLength() > 4)
    m_windowSelecting = true;
  m_hoverWorld = PointUnderMouse(e->pos(), &m_hoverSnapped);
  emit MouseWorld(m_hoverWorld);
  update();
}

void Viewport::mouseReleaseEvent(QMouseEvent* e) {
  const bool wasDrag = (e->pos() - m_pressPos).manhattanLength() > 4;
  if (m_dragButton == Qt::LeftButton) {
    const bool add = e->modifiers() & Qt::ShiftModifier, remove = e->modifiers() & Qt::ControlModifier;
    if (m_windowSelecting) {
      emit ObjectsClicked(PickWindow(m_pressPos, e->pos()), add, remove);
    } else if (Shared().waitingForPoint) {
      emit PointPicked(Shared().pickOnObjects ? PointOnObjects(e->pos()) : PointUnderMouse(e->pos()));
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

}  // namespace cad::ui
