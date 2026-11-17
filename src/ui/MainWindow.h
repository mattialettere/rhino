#pragma once
#include "commands/Engine.h"
#include "ui/Scene.h"
#include "ui/Viewport.h"
#include <QMainWindow>
#include <sstream>

class QLineEdit;
class QLabel;
class QPlainTextEdit;
class QTreeWidget;
class QGridLayout;
class QToolButton;

namespace cad::ui {

class MainWindow : public QMainWindow {
  Q_OBJECT
public:
  explicit MainWindow(QWidget* parent = nullptr);
  // Esegue testo come se fosse scritto nella riga di comando.
  void Submit(const QString& text);
  bool OpenFile(const QString& path);
  Viewport* View(int i) { return m_views[i]; }
  void ZoomExtentsAll();
  bool HasPending() const { return !m_pending.empty(); }

protected:
  void keyPressEvent(QKeyEvent* e) override;
  bool eventFilter(QObject* o, QEvent* e) override;

private:
  void Run(const std::string& script);
  void Cancel();
  void AfterCommand();
  void UpdatePrompt();
  void RefreshPanels();
  void Log(const QString& s);
  void BuildMenus();
  void BuildSidebar();
  void BuildStatusBar();
  void OnPointPicked(ON_3dPoint p);
  void OnObjectsClicked(std::vector<ON_UUID> ids, bool add, bool remove);
  void ToggleMaximize(Viewport* v);

  Document3dm m_doc;
  std::ostringstream m_out;
  Engine m_engine;
  Scene m_scene;
  std::string m_pending;   // comando in attesa di input
  QLineEdit* m_input = nullptr;
  QLabel* m_prompt = nullptr;
  QPlainTextEdit* m_history = nullptr;
  QTreeWidget* m_layers = nullptr;
  QPlainTextEdit* m_props = nullptr;
  QLabel* m_coords = nullptr;
  QGridLayout* m_grid = nullptr;
  Viewport* m_views[4] = {};
  Viewport* m_maximized = nullptr;
};

}  // namespace cad::ui
