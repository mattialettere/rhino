// Scheda di una tavola (layout): il foglio con dettagli e oggetti di pagina, come le
// schede delle tavole di Rhino 8. I clic danno punti in millimetri sul foglio.
#pragma once
#include "print/Page.h"
#include "ui/Scene.h"
#include <QWidget>

class QPainter;
class QTimer;

namespace cad::ui {

// Disegna una pagina composta: "s" = pixel per millimetro, (ox, oy) = angolo in alto a sinistra.
void PaintPage(QPainter& p, const PageDrawing& pg, double s, double ox, double oy);

class LayoutView : public QWidget {
  Q_OBJECT
public:
  LayoutView(Engine& e, const Scene& modelScene, QWidget* parent = nullptr);
  void SetLayout(const ON_UUID& id);   // tavola mostrata
  const ON_UUID& LayoutId() const { return m_layout; }
  void Refresh();                      // dopo ogni comando
  void FitPage();

signals:
  void PointPicked(ON_3dPoint p);
  void ObjectsClicked(std::vector<ON_UUID> ids, bool add, bool remove);
  void MouseWorld(ON_3dPoint p);

protected:
  void paintEvent(QPaintEvent*) override;
  void resizeEvent(QResizeEvent*) override;
  void mousePressEvent(QMouseEvent* e) override;
  void mouseMoveEvent(QMouseEvent* e) override;
  void mouseReleaseEvent(QMouseEvent* e) override;
  void wheelEvent(QWheelEvent* e) override;

private:
  ON_3dPoint ToPage(const QPointF& p) const;
  QPointF ToScreen(const ON_3dPoint& p) const;
  ON_3dPoint Snapped(const QPointF& p, bool* snapped = nullptr) const;
  std::vector<ON_UUID> PickAt(const QPointF& p) const;
  std::vector<ON_UUID> PickWindow(const QPointF& a, const QPointF& b) const;
  Engine& m_engine;
  const Scene& m_model;
  Scene m_page;
  ON_UUID m_layout = ON_nil_uuid;
  PageDrawing m_drawing;
  double m_scale = 1, m_ox = 0, m_oy = 0;  // pixel per mm e origine (angolo in alto a sinistra del foglio)
  bool m_fitted = false;
  QPointF m_press, m_last, m_hover;
  Qt::MouseButton m_button = Qt::NoButton;
  bool m_window = false;
  bool m_hoverSnapped = false;
  QTimer* m_refresh = nullptr;
};

}  // namespace cad::ui
