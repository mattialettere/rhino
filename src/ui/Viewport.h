#pragma once
// Viewport stile Rhino 8: Top, Front, Right (paralleli) e Perspective.
// Rendering OpenGL 1.1 (funziona su qualsiasi scheda/driver Windows).
#include "ui/Scene.h"
#include <QOpenGLWidget>
#include <QPainter>
#include <QPoint>
#include <functional>

namespace cad::ui {

struct Camera {
  enum class Kind { Top, Front, Right, Perspective, Bottom, Left, Back } kind = Kind::Top;
  ON_3dPoint target = ON_3dPoint::Origin;
  double halfHeight = 30.0;   // viste parallele: meta' altezza visibile (unita' del modello)
  double distance = 120.0;    // prospettiva: distanza occhio-bersaglio
  double yaw = -ON_PI / 4.0, pitch = 0.46;  // prospettiva: direzione dell'occhio
  double fovY = 30.0 * ON_PI / 180.0;

  ON_3dVector Dir() const;    // dall'occhio verso il bersaglio
  ON_3dVector Up() const;
  ON_3dVector Right() const;
  ON_3dPoint Eye() const;
  ON_Plane CPlane() const;    // piano di costruzione della vista
  // Coordinate schermo (pixel) di un punto; false se dietro la camera.
  bool Project(const ON_3dPoint& p, int w, int h, double& sx, double& sy) const;
  void Ray(double sx, double sy, int w, int h, ON_3dPoint& origin, ON_3dVector& dir) const;
  // Unita' del modello per pixel vicino al bersaglio.
  double UnitsPerPixel(int h) const;
  void ZoomExtents(const ON_BoundingBox& b, int w, int h);
};

const char* KindName(Camera::Kind k);

class Viewport : public QOpenGLWidget {
  Q_OBJECT
public:
  Viewport(Scene& scene, Camera::Kind kind, QWidget* parent = nullptr);
  Camera& Cam() { return m_cam; }
  // Modalita' di visualizzazione (sottoinsieme di quelle di Rhino 8)
  enum class DisplayMode { Wireframe, Shaded, Ghosted, XRay };
  DisplayMode mode = DisplayMode::Wireframe;
  // Impostazioni condivise dalla finestra principale
  struct Settings {
    bool gridSnap = false, ortho = false, osnap = true;
    // Osnap persistenti attivi (bit OsnapBit)
    unsigned osnapMask = 0;
    double gridSpacing = 1.0;
    bool waitingForPoint = false;
    bool pickOnObjects = false;   // il comando vuole un clic SU un oggetto (Trim, Fillet, Shell...)
    ON_3dPoint lastPoint = ON_3dPoint::Origin;
    bool hasLastPoint = false;
    bool gumball = true;          // Gumball sugli oggetti selezionati (barra di stato)
  };
  enum OsnapBit : unsigned { End = 1, Near = 2, Point = 4, Mid = 8, Cen = 16, Int = 32, Perp = 64,
                             Tan = 128, Quad = 256, Knot = 512, Vertex = 1024 };
  static Settings& Shared();
  void SetView(Camera::Kind k);
  void ZoomExtents();
  void ZoomBox(const ON_BoundingBox& b);

signals:
  void PointPicked(ON_3dPoint p);
  void ObjectsClicked(std::vector<ON_UUID> ids, bool add, bool remove);
  void MouseWorld(ON_3dPoint p);
  void ToggleMaximize(Viewport* v);
  void Activated(Viewport* v);
  // Trascinamento del Gumball concluso: comando da eseguire (Move, Rotate3D, Scale1D)
  void GumballCommand(QString command);

protected:
  void initializeGL() override;
  void paintGL() override;
  void mousePressEvent(QMouseEvent* e) override;
  void mouseMoveEvent(QMouseEvent* e) override;
  void mouseReleaseEvent(QMouseEvent* e) override;
  void mouseDoubleClickEvent(QMouseEvent* e) override;
  void wheelEvent(QWheelEvent* e) override;

private:
  ON_3dPoint PointUnderMouse(const QPoint& pos, bool* snapped = nullptr, bool allowSnap = true, QString* label = nullptr) const;
  bool FindOsnap(const QPoint& pos, ON_3dPoint& out, QString& label) const;
  void DrawLabels(QPainter& p) const;
  void ShowViewMenu(const QPoint& at);
  std::vector<ON_UUID> PickAt(const QPoint& pos) const;
  // Punto sull'oggetto sotto il mouse (curva entro 8 pixel, oppure superficie colpita dal raggio).
  ON_3dPoint PointOnObjects(const QPoint& pos, bool* hit = nullptr) const;
  std::vector<ON_UUID> PickWindow(const QPoint& a, const QPoint& b) const;
  void DrawGrid();
  // --- Gumball ---
  struct GumballHit { int mode = 0; int axis = -1; };  // mode: 1 sposta, 2 ruota, 3 scala
  bool GumballFrame(ON_3dPoint& center, double& size) const;
  GumballHit GumballPick(const QPoint& pos) const;
  void DrawGumball(QPainter& p) const;
  bool GumballValue(const QPoint& pos, double& value) const;  // parametro sull'asse o angolo
  ON_Xform GumballXform() const;
  // --- punti di controllo (PointsOn) ---
  struct GripDrag { bool active = false; ON_UUID id = ON_nil_uuid; int i = 0, j = -1; ON_3dPoint start, cur; };
  bool GripPick(const QPoint& pos, GripDrag& out) const;
  ON_3dPoint GripDragPoint(const QPoint& pos) const;
  void DrawGrips(QPainter& p) const;
  GripDrag m_gripDrag;
  GumballHit m_gbDrag;
  double m_gbStart = 0, m_gbCurrent = 0;
  ON_3dPoint m_gbCenter;
  double m_gbSize = 1;
  Scene& m_scene;
  Camera m_cam;
  QPoint m_pressPos, m_lastPos, m_hover;
  Qt::MouseButton m_dragButton = Qt::NoButton;
  bool m_windowSelecting = false;
  ON_3dPoint m_hoverWorld;
  bool m_hoverSnapped = false;
  QString m_hoverSnapLabel;
};

}  // namespace cad::ui
