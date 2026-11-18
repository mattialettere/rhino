#pragma once
#include "commands/Engine.h"
#include "print/Page.h"
#include "ui/Scene.h"
#include "ui/Theme.h"
#include "ui/Viewport.h"
#include <QMainWindow>
#include <sstream>
#include <utility>
#include <vector>

class QAction;
class QLineEdit;
class QCompleter;
class QLabel;
class QPlainTextEdit;
class QTreeWidget;
class QGridLayout;
class QToolButton;
class QComboBox;
class QPushButton;
class QStackedWidget;
class QTabBar;

namespace cad::collab {
class Session;
}

namespace cad::ui {

class AiPanel;
class TexturePanel;
class LayoutView;

class MainWindow : public QMainWindow {
  Q_OBJECT
public:
  explicit MainWindow(QWidget* parent = nullptr);
  // Esegue testo come se fosse scritto nella riga di comando.
  void Submit(const QString& text);
  void PropertyCommand(const QString& cmd);
  bool OpenFile(const QString& path);
  Viewport* View(int i) { return m_views[i]; }
  void ZoomExtentsAll();
  bool HasPending() const { return !m_pending.empty(); }
  cad::collab::Session* Collab() { return m_collab; }

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
  bool ApplyTextureToSelection(const QString& file);
  bool CaptureView(const QString& path, int width, int height, bool grid);
  bool AdoptFileViews();
  void RebuildLayoutTabs();
  void NewLayoutDialog();
  bool PrintPages(const std::vector<PageDrawing>& pages);
  void SetTheme(ThemeMode m);
  void RefreshThemeIcons();
  QWidget* BuildToolTabs();
  Viewport* Active() { return m_active ? m_active : m_views[1]; }
  void BuildStatusBar();
  void OnPointPicked(ON_3dPoint p);
  void OnObjectsClicked(std::vector<ON_UUID> ids, bool add, bool remove);
  void ToggleMaximize(Viewport* v);
  void ShowSelectionFilter();
  void ShowBlockManager();
  void ShowAbout();
  void ShowTextFile(const QString& file, const QString& title);
  bool PassesFilter(const ON_UUID& id) const;
  unsigned m_selFilter = 0xFFFF;  // tipi selezionabili (SelectionFilter)

  Document3dm m_doc;
  std::ostringstream m_out;
  Engine m_engine;
  Scene m_scene;
  std::string m_pending;   // comando in attesa di input
  QLineEdit* m_input = nullptr;
  QCompleter* m_completer = nullptr;
  QLabel* m_prompt = nullptr;
  QPlainTextEdit* m_history = nullptr;
  QTreeWidget* m_layers = nullptr;
  QPlainTextEdit* m_props = nullptr;
  QLabel* m_propType = nullptr;
  QLineEdit* m_propName = nullptr;
  QComboBox* m_propLayer = nullptr;
  QComboBox* m_propColor = nullptr;
  QPushButton* m_propSwatch = nullptr;
  QWidget* m_propForm = nullptr;
  QLabel* m_coords = nullptr;
  QGridLayout* m_grid = nullptr;
  Viewport* m_views[4] = {};
  Viewport* m_maximized = nullptr;
  Viewport* m_active = nullptr;
  AiPanel* m_ai = nullptr;
  QStackedWidget* m_stack = nullptr;
  QTabBar* m_layoutTabs = nullptr;
  LayoutView* m_layoutView = nullptr;
  bool m_tabsUpdating = false;
  TexturePanel* m_textures = nullptr;
  QToolButton* m_historyBtn = nullptr;
  class NodeEditor* m_nodes = nullptr;
  class cad::collab::Session* m_collab = nullptr;
  std::vector<std::pair<QAction*, QString>> m_iconActions;  // azioni con icona del tema
};

}  // namespace cad::ui
