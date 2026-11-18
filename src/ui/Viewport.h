#pragma once
#include <QColor>
#include <map>
#include <string>
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
  bool customCPlane = false;  // CPlane impostato con CPlane/NamedCPlane
  ON_Plane cplane = ON_Plane::World_xy;

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
  ON_UUID id = ON_nil_uuid;  // id della vista (piani di sezione, viste salvate nel .3dm)
  QString Title() const;
  // Vista come ON_3dmView del .3dm (camera, CPlane, nome) e ripristino da una vista salvata
  ON_3dmView ToView() const;
  void FromView(const ON_3dmView& v);
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
    // Analisi attive (Zebra, DraftAngleAnalysis, CurvatureGraph, ShowEdges) per oggetto
    std::vector<ON_UUID> zebra, draft, curvature, edges;
    // Colori delle viste (tema chiaro o scuro)
    struct Colors {
      bool dark = false;
      QColor bgTop{204, 207, 212}, bgBottom{184, 187, 192};
      QColor gridMinor{168, 171, 176}, gridMajor{140, 143, 148};
      QColor axisX{176, 70, 70}, axisY{70, 150, 80};
      QColor ink{0, 0, 0};          // oggetti neri (colore predefinito dei livelli)
      QColor overlay{32, 34, 38};   // titolo della vista, cornice, elastico
      QColor cage{70, 70, 70};
      QColor selected{255, 214, 0};
    } colors;
    // Colore con cui disegnare un oggetto: il nero diventa l'inchiostro del tema
    QColor ObjectColor(const ON_Color& c) const {
      if (c.Red() == 0 && c.Green() == 0 && c.Blue() == 0) return colors.ink;
      return QColor(c.Red(), c.Green(), c.Blue());
    }
  };
  enum OsnapBit : unsigned { End = 1, Near = 2, Point = 4, Mid = 8, Cen = 16, Int = 32, Perp = 64,
                             Tan = 128, Quad = 256, Knot = 512, Vertex = 1024 };
  static Settings& Shared();
  void SetView(Camera::Kind k);
  void ZoomExtents();
  void ZoomBox(const ON_BoundingBox& b);

signals:
  // texture trascinata dal pannello (o un'immagine da Esplora file) sugli oggetti in quel punto
  void TextureDropped(QString file, std::vector<ON_UUID> ids);
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
  void dragEnterEvent(QDragEnterEvent* e) override;
  void dragMoveEvent(QDragMoveEvent* e) override;
  void dropEvent(QDropEvent* e) override;

public:
  bool showGrid = true;  // griglia e assi (ViewCaptureToFile DrawGrid=No)

private:
  // texture caricate nel contesto OpenGL di questa vista (percorso -> nome GL)
  unsigned TextureId(const std::string& file);
  std::map<std::string, unsigned> m_textures;
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
  void DrawAnalysis();
  unsigned m_zebraTex = 0;
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
