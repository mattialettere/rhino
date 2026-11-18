// Finestra dell'editor visuale a nodi (comando NodeEditor): tavolozza dei componenti per
// categoria, area di lavoro con nodi e fili, anteprima rossa nelle viste e Bake nel modello.
#pragma once
#include "nodes/Graph.h"
#include <QPointF>
#include <QString>
#include <QWidget>
#include <functional>
#include <set>

class QLabel;
class QLineEdit;
class QListWidget;
class QTabWidget;
class QTimer;

namespace cad {
class Engine;
}

namespace cad::ui {

class NodeCanvas : public QWidget {
  Q_OBJECT
 public:
  explicit NodeCanvas(nodes::Graph& g, QWidget* parent = nullptr);
  void AddNodeAt(const std::string& type, QPointF screen);
  void AddNodeCentered(const std::string& type);
  void ZoomAll();
  int SelectedNode() const { return m_selected; }
  // azioni del menu del nodo (fornite dalla finestra)
  std::function<void(int)> bake;
  std::function<void(int)> setFromSelection;

 signals:
  void Changed();         // struttura o parametri: si rivaluta
  void Message(QString);  // testo per la barra in basso

 protected:
  void paintEvent(QPaintEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void mouseReleaseEvent(QMouseEvent*) override;
  void mouseDoubleClickEvent(QMouseEvent*) override;
  void wheelEvent(QWheelEvent*) override;
  void keyPressEvent(QKeyEvent*) override;
  void contextMenuEvent(QContextMenuEvent*) override;

 private:
  struct Hit {
    int node = 0;
    int port = -1;
    bool output = false;
    bool slider = false;
    bool toggle = false;
  };
  QRectF NodeRect(const nodes::Node& n) const;  // coordinate del grafo
  QPointF PortPos(const nodes::Node& n, int port, bool output) const;
  QRectF SliderTrack(const nodes::Node& n) const;
  Hit HitTest(QPointF graph) const;
  QPointF ToGraph(QPointF screen) const { return (screen - m_offset) / m_zoom; }
  QPointF ToScreen(QPointF graph) const { return graph * m_zoom + m_offset; }
  void SetSlider(nodes::Node& n, double graphX);
  void EditNode(int id);
  void ShowSearch(QPointF screen);

  nodes::Graph& m_graph;
  QPointF m_offset{40, 40};
  double m_zoom = 1.0;
  int m_selected = 0;
  // trascinamenti
  enum class Drag { None, Pan, Move, Wire, Slider } m_drag = Drag::None;
  QPointF m_press, m_last;
  int m_dragNode = 0, m_dragPort = 0;
  QPointF m_wireEnd;
};

class NodeEditor : public QWidget {
  Q_OBJECT
 public:
  // preview: geometrie da mostrare nelle viste; afterBake: aggiornamento della finestra
  NodeEditor(Engine& e, std::function<void(const std::vector<std::shared_ptr<const ON_Geometry>>&)> preview,
             std::function<void()> afterBake, QWidget* parent = nullptr);
  // Il documento e' cambiato (comando eseguito): si rivalutano i riferimenti al modello
  void DocumentChanged();
  // Il grafo vive anche nel .3dm (testo utente del documento)
  void SaveToDocument();
  void LoadFromDocument();
  void Evaluate();
  bool LoadFile(const QString& path);
  void ZoomAll() { m_canvas->ZoomAll(); }

 protected:
  void closeEvent(QCloseEvent*) override;

 private:
  void Open();
  bool SaveAs();
  void UpdateStatus();
  Engine& m_engine;
  nodes::Graph m_graph;
  NodeCanvas* m_canvas = nullptr;
  QLabel* m_status = nullptr;
  QTimer* m_timer = nullptr;
  QString m_path;
  bool m_previewAll = true;
  std::function<void(const std::vector<std::shared_ptr<const ON_Geometry>>&)> m_preview;
  std::function<void()> m_afterBake;
};

}  // namespace cad::ui
