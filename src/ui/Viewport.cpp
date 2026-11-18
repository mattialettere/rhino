#include "ui/Viewport.h"
#include <QMenu>
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
      {"Fantasma (Ghosted)", DisplayMode::Ghosted}, {"Raggi X (X-Ray)", DisplayMode::XRay}};
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
  DrawGrid();
  gl->glEnable(GL_DEPTH_TEST);
  gl->glDepthFunc(GL_LEQUAL);

  // Superfici ombreggiate (Shaded), trasparenti (Ghosted) o trasparenti con spigoli sempre visibili (X-Ray)
  const bool shaded = mode != DisplayMode::Wireframe;
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
      const float col[4] = {obj.selected ? 0.95f : 0.82f, obj.selected ? 0.9f : 0.82f, obj.selected ? 0.45f : 0.82f,
                            transparent ? 0.35f : 1.f};
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
    if (transparent) { gl->glDepthMask(GL_TRUE); gl->glDisable(GL_BLEND); }
  }
  if (mode == DisplayMode::XRay) gl->glDisable(GL_DEPTH_TEST);

  // Wireframe, punti
  for (int pass = 0; pass < 2; pass++) {  // selezionati per ultimi, sopra
    for (const auto& obj : m_scene.Objects()) {
      if (obj.selected != (pass == 1)) continue;
      if (obj.selected) gl->glColor3ub(255, 255, 0);
      else if (obj.locked) gl->glColor3ub(110, 110, 110);
      else gl->glColor3ub(obj.color.Red(), obj.color.Green(), obj.color.Blue());
      gl->glLineWidth(obj.selected ? 2.0f : 1.0f);
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
    }
  }

  if (m_gbDrag.mode) {  // anteprima del Gumball: gli oggetti selezionati trasformati
    const ON_Xform x = GumballXform();
    double m[16];
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) m[c * 4 + r] = x[r][c];
    gl->glPushMatrix();
    gl->glMultMatrixd(m);
    gl->glColor3ub(80, 80, 80);
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
  DrawLabels(p);
  DrawGumball(p);
  p.setPen(QColor(30, 30, 30));
  QFont f = p.font(); f.setBold(true); p.setFont(f);
  QString title = KindName(m_cam.kind);
  p.drawText(8, 16, title);
  {  // freccia del menu della vista
    const int tx = 8 + p.fontMetrics().horizontalAdvance(title) + 8;
    QPolygonF tri({QPointF(tx, 9), QPointF(tx + 8, 9), QPointF(tx + 4, 14)});
    p.setBrush(QColor(30, 30, 30));
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
    p.setPen(QPen(QColor(60, 60, 60), 2)); p.setBrush(Qt::NoBrush);
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
    QColor c = obj.selected ? QColor(255, 255, 0) : obj.locked ? QColor(110, 110, 110)
                                                              : QColor(obj.color.Red(), obj.color.Green(), obj.color.Blue());
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
        QFont font("Arial");
        font.setPixelSize(int(F));
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

static double SegDist(double px, double py, double ax, double ay, double bx, double by) {
  const double dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
  double t = l2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0;
  t = std::clamp(t, 0.0, 1.0);
  return std::hypot(px - (ax + t * dx), py - (ay + t * dy));
}

// Arco di rotazione attorno all'asse i: quarto di cerchio tra gli altri due assi.
static std::vector<ON_3dPoint> GumballArc(const ON_3dPoint& c, int i, double r) {
  const ON_3dVector u = kAxes[(i + 1) % 3], v = kAxes[(i + 2) % 3];
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
  const int W = width(), H = height();
  double cx, cy;
  if (!m_cam.Project(c, W, H, cx, cy)) return best;
  double bestD = 7.0;
  for (int i = 0; i < 3; i++) {
    double tx, ty, sx, sy;
    if (!m_cam.Project(c + kAxes[i] * L, W, H, tx, ty)) continue;
    if (std::hypot(tx - cx, ty - cy) < 12) continue;  // asse visto di punta: non utilizzabile
    // quadratino di scala a 0.6 della freccia
    if (m_cam.Project(c + kAxes[i] * (0.6 * L), W, H, sx, sy)) {
      const double d = std::max(std::fabs(sx - pos.x()), std::fabs(sy - pos.y()));
      if (d <= 6 && d < bestD) { bestD = d; best = {3, i}; }
    }
    const double d = SegDist(pos.x(), pos.y(), cx + (tx - cx) * 0.7, cy + (ty - cy) * 0.7, tx, ty);
    if (d < bestD) { bestD = d; best = {1, i}; }
  }
  if (best.mode) return best;
  for (int i = 0; i < 3; i++) {
    const auto arc = GumballArc(c, i, 0.8 * L);
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
  const ON_3dVector a = kAxes[m_gbDrag.axis];
  if (m_gbDrag.mode == 2) {
    // angolo nel piano perpendicolare all'asse
    const double den = d * a;
    if (std::fabs(den) < 1e-6) return false;
    const double t = ((m_gbCenter - o) * a) / den;
    const ON_3dVector v = (o + d * t) - m_gbCenter;
    const ON_3dVector u1 = kAxes[(m_gbDrag.axis + 1) % 3], u2 = kAxes[(m_gbDrag.axis + 2) % 3];
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
  const ON_3dVector a = kAxes[std::max(0, m_gbDrag.axis)];
  if (m_gbDrag.mode == 1) return ON_Xform::TranslationTransformation(a * (m_gbCurrent - m_gbStart));
  if (m_gbDrag.mode == 2) {
    ON_Xform r;
    r.Rotation(m_gbCurrent - m_gbStart, a, m_gbCenter);
    return r;
  }
  if (m_gbDrag.mode == 3 && std::fabs(m_gbStart) > 1e-12) {
    const double f = m_gbCurrent / m_gbStart;
    double s[3] = {1, 1, 1};
    s[m_gbDrag.axis] = f;
    return ON_Xform::ScaleTransformation(m_gbCenter, s[0], s[1], s[2]);
  }
  return ON_Xform::IdentityTransformation;
}

void Viewport::DrawGumball(QPainter& p) const {
  ON_3dPoint c;
  double L;
  if (!GumballFrame(c, L)) return;
  const int W = width(), H = height();
  double cx, cy;
  if (!m_cam.Project(c, W, H, cx, cy)) return;
  const QColor col[3] = {QColor(220, 40, 40), QColor(40, 170, 40), QColor(40, 70, 220)};
  const GumballHit hov = m_gbDrag.mode ? m_gbDrag : GumballPick(m_hover);
  p.save();
  for (int i = 0; i < 3; i++) {
    const bool hot = hov.axis == i;
    // archi di rotazione
    const auto arc = GumballArc(c, i, 0.8 * L);
    QPolygonF poly;
    for (const auto& q : arc) { double x, y; if (m_cam.Project(q, W, H, x, y)) poly << QPointF(x, y); }
    p.setPen(QPen(col[i], (hot && hov.mode == 2) ? 4 : 2));
    p.setBrush(Qt::NoBrush);
    p.drawPolyline(poly);
    double tx, ty;
    if (!m_cam.Project(c + kAxes[i] * L, W, H, tx, ty) || std::hypot(tx - cx, ty - cy) < 12) continue;
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
    if (m_cam.Project(c + kAxes[i] * (0.6 * L), W, H, sx, sy)) {
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
    p.setPen(QColor(0, 0, 0));
    p.drawText(QPointF(m_hover.x() + 14, m_hover.y() - 8), t);
  }
  p.restore();
}

// Osnap persistenti, come la barra Osnap di Rhino 8. Raggio di cattura 10 pixel.
// Priorita': il candidato piu' vicino al cursore; "Vicino" solo se non c'e' altro.
bool Viewport::FindOsnap(const QPoint& pos, ON_3dPoint& out, QString& label) const {
  const Settings& S = Shared();
  const unsigned m = S.osnapMask;
  const double R = 10.0;
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
  if (e->button() == Qt::LeftButton) {
    const GumballHit h = GumballPick(e->pos());
    double size;
    if (h.mode && GumballFrame(m_gbCenter, size)) {
      m_gbDrag = h;
      m_gbSize = size;
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
  m_hoverWorld = PointUnderMouse(e->pos(), &m_hoverSnapped, true, &m_hoverSnapLabel);
  emit MouseWorld(m_hoverWorld);
  update();
}

void Viewport::mouseReleaseEvent(QMouseEvent* e) {
  const bool wasDrag = (e->pos() - m_pressPos).manhattanLength() > 4;
  if (m_gbDrag.mode) {
    const GumballHit h = m_gbDrag;
    m_gbDrag = {};
    m_dragButton = Qt::NoButton;
    const double dv = m_gbCurrent - m_gbStart;
    auto P = [](const ON_3dPoint& q) { return QString("%1,%2,%3").arg(q.x, 0, 'g', 12).arg(q.y, 0, 'g', 12).arg(q.z, 0, 'g', 12); };
    const ON_3dVector a = kAxes[h.axis];
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
