#include "ui/MainWindow.h"
#include "ui/AiPanel.h"
#include "ui/CollabPanel.h"
#include "collab/Session.h"
#include "ui/TexturePanel.h"
#include "ui/ScriptEditor.h"
#include "ui/NodeEditor.h"
#include "ui/Theme.h"
#include "ui/LayoutView.h"
#include "ui/RenderWindow.h"
#include <QActionGroup>
#include <cstring>
#include <QPrintDialog>
#include <QPrinter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTabBar>
#include <QStyleHints>
#include <QGuiApplication>
#include <QAbstractItemView>
#include <QAction>
#include <QColorDialog>
#include <QComboBox>
#include <QFormLayout>
#include <QPushButton>
#include <QCompleter>
#include <QStringListModel>
#include <QTabWidget>
#include <QApplication>
#include <QCheckBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QFile>
#include <QClipboard>
#include <QFileInfo>
#include <QRegularExpression>
#include <QDir>
#include <QIcon>
#include <QPixmap>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QDialog>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QPainterPath>
#include <set>

namespace cad::ui {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), m_engine(m_doc, m_out), m_scene(m_engine) {
  m_engine.interactive = true;
  m_scene.fixedSpace = true;  // le viste mostrano sempre il modello; le tavole hanno la loro scheda
  Scene::textureCacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation).toStdString() + "/textures";
  // Heightfield: immagini in qualsiasi formato letto da Qt (PNG, JPG, BMP...)
  m_engine.imageLoader = [](const std::string& path, int& w, int& h, std::vector<double>& gray) {
    QImage img(QString::fromStdString(path));
    if (img.isNull()) return false;
    img = img.convertToFormat(QImage::Format_Grayscale8);
    w = img.width();
    h = img.height();
    gray.assign((size_t)w * h, 0.0);
    for (int y = 0; y < h; y++) {
      const uchar* row = img.constScanLine(y);
      for (int x = 0; x < w; x++) gray[(size_t)y * w + x] = row[x] / 255.0;
    }
    return true;
  };
  // Viste: id fissi delle quattro viste (sostituiti da quelli del file aperto), vista attiva,
  // viste con nome, CPlane e viste scritte nel .3dm al salvataggio
  m_engine.getActiveView = [this](ON_3dmView& v) { v = Active()->ToView(); return true; };
  m_engine.setActiveView = [this](const ON_3dmView& v) { Active()->FromView(v); return true; };
  m_engine.setActiveCPlane = [this](const ON_Plane* p) {
    Camera& c = Active()->Cam();
    c.customCPlane = p != nullptr;
    if (p) c.cplane = *p;
    Active()->update();
    return true;
  };
  m_engine.beforeSave = [this] {
    if (m_nodes) m_nodes->SaveToDocument();  // il grafo dell'editor a nodi resta nel .3dm
    // viste delle finestre al posto di quelle lette dal file; le tavole restano
    auto& views = m_engine.Model().m_settings.m_views;
    ON_ClassArray<ON_3dmView> pages;
    for (int i = 0; i < views.Count(); i++)
      if (views[i].m_view_type == ON::view_type::page_view_type) pages.Append(views[i]);
    views.Empty();
    for (auto* v : m_views) views.Append(v->ToView());
    for (int i = 0; i < pages.Count(); i++) views.Append(pages[i]);
  };
  // Tavole: schede sotto le viste; stampa con la finestra di stampa del sistema
  m_engine.layoutsChanged = [this] { RebuildLayoutTabs(); };
  // Render: finestra con l'immagine progressiva; SetDisplayMode: modalita' della vista attiva
  m_engine.renderWindow = [this](const ON_3dmView& v, int w, int h, int samples) {
    auto* win = new RenderWindow(m_engine, m_scene, v, w, h, samples, this);
    win->show();
    return true;
  };
  m_engine.setDisplayMode = [this](const std::string& name) {
    const std::pair<const char*, Viewport::DisplayMode> modes[] = {
        {"wireframe", Viewport::DisplayMode::Wireframe}, {"shaded", Viewport::DisplayMode::Shaded},
        {"ghosted", Viewport::DisplayMode::Ghosted}, {"xray", Viewport::DisplayMode::XRay},
        {"rendered", Viewport::DisplayMode::Rendered}, {"raytraced", Viewport::DisplayMode::Raytraced}};
    for (auto& [n, m] : modes)
      if (name == n) { Active()->mode = m; Active()->update(); return true; }
    return false;
  };
  RenderScene::imageLoader = [](const std::string& path, int& w, int& h, std::vector<unsigned char>& rgb) {
    QImage img(QString::fromStdString(path));
    if (img.isNull()) return false;
    img = img.convertToFormat(QImage::Format_RGB888);
    w = img.width();
    h = img.height();
    rgb.resize((size_t)w * h * 3);
    for (int y = 0; y < h; y++) std::memcpy(&rgb[(size_t)y * w * 3], img.constScanLine(y), (size_t)w * 3);
    return true;
  };
  m_engine.printPages = [this](const std::vector<PageDrawing>& pages) { return PrintPages(pages); };
  m_engine.viewCapture = [this](const std::string& path, int w, int h, bool grid) {
    return CaptureView(QString::fromStdString(path), w, h, grid);
  };
  // TextObject: contorni dei caratteri di sistema; l'altezza e' quella delle maiuscole
  m_engine.textOutliner = [](const std::string& text, const std::string& family, double height,
                             std::vector<std::vector<std::vector<ON_3dPoint>>>& contours) {
    QFont font(QString::fromStdString(family));
    font.setPixelSize(1000);
    QPainterPath path;
    path.addText(0, 0, font, QString::fromStdString(text));
    double cap = QFontMetricsF(font).capHeight();
    if (cap <= 0) cap = 700;
    const double k = height / cap;
    auto pt = [&](const QPointF& q) { return ON_3dPoint(q.x() * k, -q.y() * k, 0); };
    std::vector<std::vector<ON_3dPoint>>* cur = nullptr;
    ON_3dPoint last;
    for (int i = 0; i < path.elementCount(); i++) {
      const auto el = path.elementAt(i);
      if (el.isMoveTo()) {
        contours.emplace_back();
        cur = &contours.back();
        last = pt(el);
      } else if (el.isLineTo() && cur) {
        cur->push_back({last, pt(el)});
        last = pt(el);
      } else if (el.isCurveTo() && cur && i + 2 < path.elementCount()) {
        const ON_3dPoint c1 = pt(el), c2 = pt(path.elementAt(i + 1)), end = pt(path.elementAt(i + 2));
        cur->push_back({last, c1, c2, end});
        last = end;
        i += 2;
      }
    }
    std::erase_if(contours, [](const auto& c) { return c.empty(); });
    return !contours.empty();
  };
  setWindowTitle("Senza titolo - Kurva3D");
  resize(1400, 900);

  // Area comandi in alto (come Rhino 8): cronologia + riga "Comando:"
  auto* central = new QWidget;
  auto* vbox = new QVBoxLayout(central);
  vbox->setContentsMargins(6, 4, 6, 4);
  vbox->setSpacing(4);
  auto* cmdBar = new QWidget;
  cmdBar->setObjectName("commandBar");
  cmdBar->setAttribute(Qt::WA_StyledBackground);
  auto* cmdBox = new QVBoxLayout(cmdBar);
  cmdBox->setContentsMargins(8, 4, 8, 4);
  cmdBox->setSpacing(0);
  m_history = new QPlainTextEdit;
  m_history->setReadOnly(true);
  m_history->setMaximumHeight(70);
  m_history->setFont(MonoFont(9));
  m_history->setFrameShape(QFrame::NoFrame);
  cmdBox->addWidget(m_history);
  auto* line = new QHBoxLayout;
  m_prompt = new QLabel("Comando:");
  m_prompt->setObjectName("panelTitle");
  m_input = new QLineEdit;
  m_input->setFont(MonoFont(10));
  m_input->installEventFilter(this);
  {  // completamento automatico dei nomi dei comandi (come la riga di comando di Rhino 8)
    QStringList names;
    for (auto& [k, c] : m_engine.Commands()) names << QString::fromStdString(c.name);
    for (const auto& [it, en] : ItalianCommandNames())
      if (m_engine.Aliases().count(NormalizeName(it))) names << QString::fromUtf8(it);
    names.sort(Qt::CaseInsensitive);
    auto* comp = new QCompleter(names, this);
    comp->setCaseSensitivity(Qt::CaseInsensitive);
    comp->setCompletionMode(QCompleter::PopupCompletion);
    m_input->setCompleter(comp);
    m_completer = comp;
  }
  m_options = new QLabel;
  m_options->setTextFormat(Qt::RichText);
  m_options->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
  m_options->setToolTip("Clic su un'opzione per cambiarla");
  connect(m_options, &QLabel::linkActivated, this, [this](const QString& l) { OptionClicked(l); });
  line->addWidget(m_prompt);
  line->addWidget(m_options);
  line->addWidget(m_input, 1);
  cmdBox->addLayout(line);
  vbox->addWidget(cmdBar);
  vbox->addWidget(BuildToolTabs());

  // Quattro viewport
  auto* views = new QWidget;
  m_grid = new QGridLayout(views);
  m_grid->setContentsMargins(0, 0, 0, 0);
  m_grid->setSpacing(3);
  const Camera::Kind kinds[4] = {Camera::Kind::Top, Camera::Kind::Perspective, Camera::Kind::Front, Camera::Kind::Right};
  for (int i = 0; i < 4; i++) {
    m_views[i] = new Viewport(m_scene, kinds[i]);
    m_grid->addWidget(m_views[i], i / 2, i % 2);
    connect(m_views[i], &Viewport::PointPicked, this, &MainWindow::OnPointPicked);
    connect(m_views[i], &Viewport::ObjectsClicked, this, &MainWindow::OnObjectsClicked);
    connect(m_views[i], &Viewport::GumballCommand, this, [this](QString c) { PropertyCommand(c); });
    connect(m_views[i], &Viewport::ToggleMaximize, this, &MainWindow::ToggleMaximize);
    connect(m_views[i], &Viewport::Activated, this, [this](Viewport* v) { m_active = v; m_engine.activeViewportId = v->id; });
    connect(m_views[i], &Viewport::MouseWorld, this, [this](ON_3dPoint p) {
      m_coords->setText(QString("x %1   y %2   z %3").arg(p.x, 0, 'f', 3).arg(p.y, 0, 'f', 3).arg(p.z, 0, 'f', 3));
    });
  }
  // viste del modello e tavole, con le schede in basso come in Rhino 8
  m_stack = new QStackedWidget;
  m_stack->addWidget(views);
  m_layoutView = new LayoutView(m_engine, m_scene);
  m_stack->addWidget(m_layoutView);
  connect(m_layoutView, &LayoutView::PointPicked, this, &MainWindow::OnPointPicked);
  connect(m_layoutView, &LayoutView::ObjectsClicked, this, &MainWindow::OnObjectsClicked);
  connect(m_layoutView, &LayoutView::MouseWorld, this, [this](ON_3dPoint p) {
    m_coords->setText(QString("x %1   y %2 mm (tavola)").arg(p.x, 0, 'f', 2).arg(p.y, 0, 'f', 2));
  });
  vbox->addWidget(m_stack, 1);
  m_layoutTabs = new QTabBar;
  m_layoutTabs->setDrawBase(false);
  m_layoutTabs->setExpanding(false);
  m_layoutTabs->setObjectName("layoutTabs");
  connect(m_layoutTabs, &QTabBar::currentChanged, this, [this](int i) {
    if (m_tabsUpdating || i < 0) return;
    const QString name = i == 0 ? QString("Model") : m_layoutTabs->tabText(i);
    Submit("_Layouts Activate \"" + name + "\"");
  });
  {
    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(m_layoutTabs);
    auto* add = new QToolButton;
    add->setText("+");
    add->setToolTip("Nuova tavola (Layout)");
    add->setAutoRaise(true);
    connect(add, &QToolButton::clicked, this, [this] { NewLayoutDialog(); });
    row->addWidget(add);
    row->addStretch(1);
    vbox->addLayout(row);
  }
  RebuildLayoutTabs();
  {
    const char* ids[4] = {"4b0f1a10-6c21-4d7e-9a3e-0d1c2b3a4f01", "4b0f1a10-6c21-4d7e-9a3e-0d1c2b3a4f02",
                          "4b0f1a10-6c21-4d7e-9a3e-0d1c2b3a4f03", "4b0f1a10-6c21-4d7e-9a3e-0d1c2b3a4f04"};
    for (int i = 0; i < 4; i++) {
      m_views[i]->id = ON_UuidFromString(ids[i]);
      m_engine.viewportIds.push_back(m_views[i]->id);
    }
    m_engine.activeViewportId = m_views[1]->id;
  }
  setCentralWidget(central);

  // Pannelli a destra: Livelli e Proprieta'
  auto* layersDock = new QDockWidget("Livelli", this);
  m_layers = new QTreeWidget;
  m_layers->setHeaderLabels({"Nome", "Corrente", "Visibile"});
  m_layers->header()->setStretchLastSection(false);
  m_layers->setRootIsDecorated(false);
  layersDock->setWidget(m_layers);
  addDockWidget(Qt::RightDockWidgetArea, layersDock);
  connect(m_layers, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* it) {
    Submit("-_Layer _Current \"" + it->text(0) + "\"");
  });
  // Pannello Proprieta' come in Rhino 8: tipo, nome, livello, colore di
  // visualizzazione modificabili; le modifiche passano dai comandi (annullabili).
  auto* propsDock = new QDockWidget("Proprietà", this);
  auto* propsW = new QWidget;
  auto* pv = new QVBoxLayout(propsW);
  pv->setContentsMargins(4, 4, 4, 4);
  m_propForm = new QWidget;
  auto* form = new QFormLayout(m_propForm);
  form->setContentsMargins(0, 0, 0, 0);
  m_propType = new QLabel;
  m_propName = new QLineEdit;
  m_propLayer = new QComboBox;
  m_propColor = new QComboBox;
  m_propColor->addItems({"Da livello", "Da padre", "Personalizzato"});
  m_propSwatch = new QPushButton;
  m_propSwatch->setFixedSize(40, 20);
  auto* colorRow = new QWidget;
  auto* ch = new QHBoxLayout(colorRow);
  ch->setContentsMargins(0, 0, 0, 0);
  ch->addWidget(m_propColor, 1);
  ch->addWidget(m_propSwatch);
  form->addRow("Tipo", m_propType);
  form->addRow("Nome", m_propName);
  form->addRow("Livello", m_propLayer);
  form->addRow("Colore", colorRow);
  pv->addWidget(m_propForm);
  m_props = new QPlainTextEdit;
  m_props->setReadOnly(true);
  pv->addWidget(m_props, 1);
  propsDock->setWidget(propsW);
  connect(m_propName, &QLineEdit::editingFinished, this, [this] {
    if (m_engine.Selection().empty() || !m_propName->isModified()) return;
    m_propName->setModified(false);
    const QString n = m_propName->text();
    PropertyCommand("-_SetObjectName \"" + n + "\"");
  });
  connect(m_propLayer, &QComboBox::activated, this, [this](int) {
    if (!m_engine.Selection().empty()) PropertyCommand("-_ChangeLayer \"" + m_propLayer->currentText() + "\"");
  });
  auto pickColor = [this] {
    QColor c = QColorDialog::getColor(m_propSwatch->palette().button().color(), this, "Colore dell'oggetto");
    if (c.isValid()) PropertyCommand(QString("-_SetObjectColor %1,%2,%3").arg(c.red()).arg(c.green()).arg(c.blue()));
    else RefreshPanels();
  };
  connect(m_propColor, &QComboBox::activated, this, [this, pickColor](int i) {
    if (m_engine.Selection().empty()) return;
    if (i == 0) PropertyCommand("-_SetObjectColor ByLayer");
    else if (i == 1) PropertyCommand("-_SetObjectColor ByParent");
    else pickColor();
  });
  connect(m_propSwatch, &QPushButton::clicked, this, [this, pickColor] {
    if (!m_engine.Selection().empty()) pickColor();
  });
  addDockWidget(Qt::RightDockWidgetArea, propsDock);

  // Assistente AI (modello locale): accanto al pannello Proprieta'
  auto* aiDock = new QDockWidget("Assistente AI", this);
  aiDock->setObjectName("aiDock");
  m_ai = new AiPanel(m_engine, [this] { AfterCommand(); }, aiDock);
  aiDock->setWidget(m_ai);
  addDockWidget(Qt::RightDockWidgetArea, aiDock);
  tabifyDockWidget(propsDock, aiDock);
  // Collaborazione in rete locale: sessione, ricerca degli altri computer, chat e compiti per l'AI
  m_collab = new collab::Session(m_engine, KURVA3D_VERSION, this);
  auto* collabDock = new QDockWidget("Collabora", this);
  collabDock->setObjectName("collabDock");
  auto* collabPanel = new CollabPanel(*m_collab, [this] {
    return m_engine.currentPath.empty() ? QString("Senza titolo") : QFileInfo(QString::fromStdString(m_engine.currentPath)).fileName();
  }, [this](const QString& from, const QString& text, std::function<void(QString)> done) {
    for (auto* d : findChildren<QDockWidget*>())
      if (d->objectName() == "aiDock") d->show();
    m_ai->RunTask(from, text, std::move(done));
  }, collabDock);
  collabDock->setWidget(collabPanel);
  addDockWidget(Qt::RightDockWidgetArea, collabDock);
  tabifyDockWidget(propsDock, collabDock);
  connect(m_collab, &collab::Session::ModelChanged, this, [this] { AfterCommand(); });
  connect(m_collab, &collab::Session::DocumentReplaced, this, [this] {
    if (!m_pending.empty()) Cancel();
    m_engine.currentPath.clear();
    if (m_nodes) m_nodes->LoadFromDocument();
    AfterCommand();
    ZoomExtentsAll();
  });
  m_engine.showCollaboration = [collabDock] {
    collabDock->show();
    collabDock->raise();
    return true;
  };
  // Texture: libreria di immagini da una cartella, da applicare agli oggetti
  auto* texDock = new QDockWidget("Texture", this);
  texDock->setObjectName("textureDock");
  m_textures = new TexturePanel([this](const QString& f) { return ApplyTextureToSelection(f); },
                                [this] { if (!m_engine.Selection().empty()) Submit("_RemoveTexture"); }, texDock);
  texDock->setWidget(m_textures);
  addDockWidget(Qt::RightDockWidgetArea, texDock);
  tabifyDockWidget(propsDock, texDock);
  // Editor visuale a nodi (NodeEditor): finestra separata, anteprima rossa nelle viste
  m_nodes = new NodeEditor(m_engine, [this](const std::vector<std::shared_ptr<const ON_Geometry>>& g) {
    m_scene.SetPreview(g);
    for (auto* v : m_views) v->update();
  }, [this] { AfterCommand(); }, this);
  m_engine.showNodeEditor = [this] {
    m_nodes->show();
    m_nodes->raise();
    m_nodes->activateWindow();
    m_nodes->Evaluate();
    return true;
  };
  // Editor di script Python (ScriptEditor): in basso, nascosto finche' non serve
  auto* scriptDock = new QDockWidget("Editor di script", this);
  scriptDock->setObjectName("scriptDock");
  auto* scriptEd = new ScriptEditor([this](const QString& f) {
    QString path = f;
    path.replace('"', "");
    Run(("_RunPythonScript \"" + path + "\"").toStdString());
  }, scriptDock);
  scriptDock->setWidget(scriptEd);
  addDockWidget(Qt::BottomDockWidgetArea, scriptDock);
  scriptDock->hide();
  m_engine.showScriptEditor = [scriptDock, scriptEd](const std::string& file) {
    if (!file.empty() && !scriptEd->OpenFile(QString::fromStdString(file))) return false;
    scriptDock->show();
    scriptDock->raise();
    return true;
  };
  propsDock->raise();
  for (auto* v : m_views)
    connect(v, &Viewport::TextureDropped, this, [this](QString file, std::vector<ON_UUID> ids) {
      // trascinata su un oggetto selezionato: tutta la selezione; altrimenti solo quell'oggetto
      if (!m_engine.IsSelected(ids.front())) {
        m_engine.ClearSelection();
        m_engine.Select(ids.front());
      }
      ApplyTextureToSelection(file);
    });

  BuildMenus();
  BuildSidebar();
  RefreshThemeIcons();
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  // In modo automatico segue il cambio chiaro/scuro di Windows anche a programma aperto
  connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
    if (SavedThemeMode() == ThemeMode::Auto) SetTheme(ThemeMode::Auto);
  });
#endif
  BuildStatusBar();
  Log("Kurva3D - modello nuovo in millimetri. Scrivi un comando, ? per l'elenco.");
  AfterCommand();
}

void MainWindow::Log(const QString& s) {
  m_history->appendPlainText(s);
  m_history->verticalScrollBar()->setValue(m_history->verticalScrollBar()->maximum());
}

namespace {
const char* kOpenFilter =
    "Tutti i formati (*.3dm *.stp *.step *.igs *.iges *.stl *.obj *.dxf *.gltf *.glb *.3mf *.ply);;File 3DM (*.3dm);;"
    "STEP (*.stp *.step);;IGES (*.igs *.iges);;STL (*.stl);;OBJ (*.obj);;DXF (*.dxf);;glTF (*.gltf *.glb);;3MF (*.3mf);;PLY (*.ply)";
const char* kExportFilter =
    "File 3DM, versione 8 (*.3dm);;STEP (*.stp);;IGES (*.igs);;STL binario (*.stl);;OBJ con materiali (*.obj);;DXF R12 (*.dxf);;"
    "glTF binario (*.glb);;glTF (*.gltf);;3MF (*.3mf);;PLY binario (*.ply)";
}  // namespace

void MainWindow::BuildMenus() {
  auto* file = menuBar()->addMenu("&File");
  file->addAction("Nuovo", QKeySequence::New, this, [this] { Submit("_New"); });
  file->addAction("Apri...", QKeySequence::Open, this, [this] {
    QString p = QFileDialog::getOpenFileName(this, "Apri", QString(), kOpenFilter);
    if (!p.isEmpty()) OpenFile(p);
  });
  file->addAction("Salva", QKeySequence::Save, this, [this] {
    if (m_engine.currentPath.empty()) {
      QString p = QFileDialog::getSaveFileName(this, "Salva", QString(), "File 3DM, versione 8 (*.3dm)");
      if (p.isEmpty()) return;
      Submit("_SaveAs \"" + p + "\"");
    } else {
      Submit("_Save");
    }
  });
  file->addAction("Salva con nome...", QKeySequence::SaveAs, this, [this] {
    QString p = QFileDialog::getSaveFileName(this, "Salva con nome", QString(), "File 3DM, versione 8 (*.3dm)");
    if (!p.isEmpty()) Submit("_SaveAs \"" + p + "\"");
  });
  file->addSeparator();
  file->addAction("Importa...", this, [this] {
    QString p = QFileDialog::getOpenFileName(this, "Importa", QString(), kOpenFilter);
    if (!p.isEmpty()) { Submit("_Import \"" + p + "\""); ZoomExtentsAll(); }
  });
  file->addAction("Esporta selezione...", this, [this] {
    if (m_engine.Selection().empty()) { Log("Seleziona prima gli oggetti da esportare."); return; }
    QString sel;
    QString p = QFileDialog::getSaveFileName(this, "Esporta selezione", QString(), kExportFilter, &sel);
    if (p.isEmpty()) return;
    if (QFileInfo(p).suffix().isEmpty()) {  // estensione dal filtro scelto
      const int a = sel.indexOf("*."), b = sel.indexOf(QRegularExpression("[ )]"), a);
      if (a >= 0) p += sel.mid(a + 1, b - a - 1);
    }
    Submit("_Export \"" + p + "\"");
  });
  file->addAction("Stampa...", this, [this] { Submit("_Print"); });
  file->addAction("Esporta PDF...", this, [this] {
    QString p = QFileDialog::getSaveFileName(this, "Esporta PDF (tavola o vista attiva)", QString(), "PDF (*.pdf)");
    if (p.isEmpty()) return;
    if (!p.endsWith(".pdf", Qt::CaseInsensitive)) p += ".pdf";
    Submit("_Print Destination=PDF File=\"" + p + "\"");
  });
  file->addAction("Salva vista come immagine...", this, [this] {
    QString p = QFileDialog::getSaveFileName(this, "Salva vista come immagine", QString(), "Immagine PNG (*.png);;Immagine JPEG (*.jpg)");
    if (!p.isEmpty()) Submit("_ViewCaptureToFile \"" + p + "\"");
  });
  file->addSeparator();
  file->addAction("Esci", QKeySequence::Quit, qApp, &QApplication::quit);

  auto* edit = menuBar()->addMenu("&Modifica");
  edit->addAction("Annulla", QKeySequence::Undo, this, [this] { Submit("_Undo"); });
  edit->addAction("Ripeti", QKeySequence::Redo, this, [this] { Submit("_Redo"); });
  edit->addSeparator();
  edit->addAction("Seleziona tutto", QKeySequence::SelectAll, this, [this] { Submit("_SelAll"); });
  edit->addAction("Elimina", QKeySequence::Delete, this, [this] { Submit("_Delete"); });
  edit->addAction("Taglia (Trim)", this, [this] { Submit("_Trim"); });
  edit->addAction("Estendi", this, [this] { Submit("_Extend"); });
  edit->addAction("Dividi", this, [this] { Submit("_Split"); });
  edit->addAction("Unisci", this, [this] { Submit("_Join"); });
  edit->addAction("Esplodi", this, [this] { Submit("_Explode"); });
  edit->addSeparator();
  edit->addAction("Punti di controllo attivi", QKeySequence(Qt::Key_F10), this, [this] { Submit("_PointsOn"); });
  edit->addAction("Punti di controllo disattivi", QKeySequence(Qt::Key_F11), this, [this] { Submit("_PointsOff"); });

  auto* view = menuBar()->addMenu("&Vista");
  {
    auto* theme = view->addMenu("Tema");
    auto* group = new QActionGroup(theme);
    const std::pair<const char*, ThemeMode> modes[] = {
        {"Automatico (come il sistema)", ThemeMode::Auto}, {"Chiaro", ThemeMode::Light}, {"Scuro", ThemeMode::Dark}};
    const ThemeMode cur = SavedThemeMode();
    for (auto& [label, m] : modes) {
      QAction* a = theme->addAction(label, this, [this, m = m] { SetTheme(m); });
      a->setCheckable(true);
      a->setChecked(m == cur);
      group->addAction(a);
    }
    view->addSeparator();
  }
  view->addAction("Zoom estensione (tutte le viste)", this, [this] { ZoomExtentsAll(); });
  view->addAction("Ombreggiato / Wireframe (vista attiva)", this, [this] {
    for (auto* v : m_views)
      if (v->hasFocus()) {
        v->mode = v->mode == Viewport::DisplayMode::Wireframe ? Viewport::DisplayMode::Shaded : Viewport::DisplayMode::Wireframe;
        v->update();
      }
  });
  view->addAction("Ingrandisci / ripristina vista attiva", this, [this] {
    for (auto* v : m_views) if (v->hasFocus()) ToggleMaximize(v);
  });
  {
    view->addSeparator();
    auto sub = [&](QMenu* m, std::initializer_list<std::pair<const char*, const char*>> items) {
      for (auto& [label, cmd] : items) {
        const QString c = cmd;
        m->addAction(label, this, [this, c] { Submit(c); });
      }
    };
    sub(view->addMenu("Piano di costruzione"),
        {{"Superiore (Top)", "_CPlane World Top"}, {"Frontale (Front)", "_CPlane World Front"},
         {"Destro (Right)", "_CPlane World Right"}, {"Origine", "_CPlane Origin"}, {"Altezza", "_CPlane Elevation"},
         {"Da 3 punti", "_CPlane 3Point"}, {"Ripristina", "_CPlane Reset"}});
    sub(view->addMenu("Viste con nome"),
        {{"Salva vista", "_NamedView Save"}, {"Ripristina vista", "_NamedView Restore"},
         {"Elimina vista", "_NamedView Delete"}, {"Elenco", "_NamedView List"}});
    sub(view->addMenu("Piani di costruzione con nome"),
        {{"Salva piano", "_NamedCPlane Save"}, {"Ripristina piano", "_NamedCPlane Restore"},
         {"Elimina piano", "_NamedCPlane Delete"}, {"Elenco", "_NamedCPlane List"}});
    sub(view->addMenu("Piani di sezione"),
        {{"Nuovo piano di sezione", "_ClippingPlane"}, {"Attiva nella vista", "_EnableClippingPlane"},
         {"Attiva in tutte le viste", "_EnableClippingPlane All"}, {"Disattiva nella vista", "_DisableClippingPlane"},
         {"Disattiva in tutte le viste", "_DisableClippingPlane All"}});
    {
      auto* lm = view->addMenu("Tavole (layout)");
      lm->addAction("Nuova tavola...", this, [this] { NewLayoutDialog(); });
      sub(lm, {{"Copia tavola attiva", "_CopyLayout"}, {"Aggiungi dettaglio", "_Detail Add"},
               {"Scala del dettaglio", "_Detail Scale"}, {"Vista del dettaglio", "_Detail Projection"},
               {"Dettaglio ombreggiato / wireframe", "_Detail DisplayMode"}, {"Inquadra tutto nel dettaglio", "_Detail ZoomExtents"},
               {"Elenco tavole", "_Layouts List"}});
      lm->addAction("Elimina tavola attiva", this, [this] {
        const int li = FindLayout(m_engine.Model(), m_engine.activeLayoutId);
        if (li < 0) return;
        Submit("_Layouts Delete \"" + QString::fromStdString(ToUtf8(m_engine.Model().m_settings.m_views[li].m_name)) + "\"");
      });
    }
    sub(view->addMenu("Istantanee"),
        {{"Salva istantanea", "_Snapshots Save"}, {"Ripristina istantanea", "_Snapshots Restore"},
         {"Elimina istantanea", "_Snapshots Delete"}, {"Elenco", "_Snapshots List"}});
  }

  auto addCmds = [&](const char* title, std::initializer_list<std::pair<const char*, const char*>> items) {
    auto* m = menuBar()->addMenu(title);
    for (auto& [label, cmd] : items) {
      QString c = cmd;
      m->addAction(label, this, [this, c] { Submit(c); });
    }
  };
  addCmds("&Curve", {{"Punto", "_Point"}, {"Linea", "_Line"}, {"Polilinea", "_Polyline"}, {"Rettangolo", "_Rectangle"},
                     {"Poligono", "_Polygon"}, {"Cerchio", "_Circle"}, {"Arco", "_Arc"}, {"Ellisse", "_Ellipse"},
                     {"Curva per punti di controllo", "_Curve"}, {"Curva interpolata", "_InterpCrv"}, {"Curva a maniglie", "_HandleCurve"}, {"Schizzo (Sketch)", "_Sketch"},
                     {"Offset curva", "_Offset"}, {"Raccordo curve", "_Fillet"}, {"Smusso curve", "_Chamfer"},
                     {"Intersezione", "_Intersect"}, {"Copia bordi", "_DupBorder"},
                     {"Dividi in punti", "_Divide"}, {"Ricostruisci", "_Rebuild"}, {"Cambia grado", "_ChangeDegree"},
                     {"Elica", "_Helix"}, {"Spirale", "_Spiral"}, {"Linee singole", "_Lines"},
                     {"Rettangolo arrotondato", "_RoundedRectangle"}, {"Parabola", "_Parabola"}, {"Iperbole", "_Hyperbola"},
                     {"Bisettrice", "_Bisector"}, {"Curva per punti", "_CurveThroughPt"}, {"Leviga", "_Smooth"},
                     {"Conica", "_ConicCurve"}, {"Raccordo curve (Blend)", "_BlendCrv"}, {"Raccorda angoli", "_FilletCorners"}, {"Converti in polilinea", "_ConvertToPolyline"}, {"Tira su superficie (Pull)", "_Pull"}, {"Linea tangente", "_Tangent"}, {"Cerchio tangente a 3 curve", "_CircleTangent"}, {"Arco tangente a 2 curve", "_ArcTangent"}, {"Polilinea su mesh", "_PolylineOnMesh"}, {"Disegno 2D (Make2D)", "_Make2D"}, {"Linea perpendicolare", "_Perpendicular"}, {"Normale a superficie", "_LineSrfNormal"}, {"Curva su superficie", "_InterpCrvOnSrf"}, {"Raccorda a curva (Match)", "_Match"}, {"Semplifica", "_SimplifyCrv"}, {"Riadatta (FitCrv)", "_FitCrv"}, {"Estrai isocurva", "_ExtractIsocurve"}, {"Estrai wireframe", "_ExtractWireframe"}, {"Contorno apparente (Silhouette)", "_Silhouette"}, {"Offset curva su superficie", "_OffsetCrvOnSrf"}, {"Estendi curva su superficie", "_ExtendCrvOnSrf"}, {"Leviga (Fair)", "_Fair"}, {"Ricostruisci non uniforme", "_RebuildCrvNonUniform"}, {"Rendi periodica", "_MakePeriodic"}, {"Sposta giunzione (Seam)", "_Seam"}, {"Aggiungi punto di controllo", "_InsertControlPoint"}, {"Misura (punti a distanza)", "_Measure"}});
  addCmds("&Superfici", {{"Piano", "_Plane"}, {"Da curve planari", "_PlanarSrf"}, {"Estrudi curva", "_ExtrudeCrv"},
                         {"Rivoluzione", "_Revolve"}, {"Loft", "_Loft"}, {"Sweep 1 binario", "_Sweep1"}, {"Sweep 2 binari", "_Sweep2"}, {"Da rete di curve", "_NetworkSrf"}, {"Rivoluzione su binario", "_RailRevolve"}, {"Estrudi rastremato", "_ExtrudeCrvTapered"}, {"Estendi superficie", "_ExtendSrf"}, {"Unisci superfici (MergeSrf)", "_MergeSrf"},
                         {"Offset superficie", "_OffsetSrf"}, {"Da 3 o 4 punti", "_SrfPt"},
                         {"Da 2, 3 o 4 curve di bordo", "_EdgeSrf"}, {"Patch", "_Patch"},
                         {"Piano da 3 punti", "_Plane3Pt"}, {"Piano verticale", "_PlaneVertical"},
                         {"Estrudi fino a un punto", "_ExtrudeCrvToPoint"}, {"Estrudi lungo una curva", "_ExtrudeCrvAlongCrv"},
                         {"Rimuovi tutti i tagli", "_UntrimAll"}, {"Riduci superficie tagliata", "_ShrinkTrimmedSrf"}, {"Raccordo tra superfici", "_FilletSrf"}, {"Smusso tra superfici", "_ChamferSrf"}, {"Raccordo con tangenza (BlendSrf)", "_BlendSrf"}, {"Sviluppa (UnrollSrf)", "_UnrollSrf"}, {"Appiattisci (Smash)", "_Smash"}, {"Converti in NURBS", "_ToNURBS"}, {"Adatta bordo (MatchSrf)", "_MatchSrf"}, {"Loft sviluppabile (DevLoft)", "_DevLoft"}, {"Da immagine (Heightfield)", "_Heightfield"}, {"Punti di modifica (EditPtOn)", "_EditPtOn"}});
  addCmds("S&olidi", {{"Parallelepipedo", "_Box"}, {"Sfera", "_Sphere"}, {"Sfera da 3 punti", "_Sphere3Pt"}, {"Box da 3 punti", "_Box3Pt"}, {"Cilindro", "_Cylinder"}, {"Cono", "_Cone"},
                      {"Toro", "_Torus"}, {"Tubo", "_Pipe"}, {"Piramide", "_Pyramid"}, {"Cuneo", "_Wedge"}, {"Lastra (Slab)", "_Slab"}, {"Sposta faccia", "_MoveFace"},
                      {"Tronco di cono", "_TruncatedCone"}, {"Ellissoide", "_Ellipsoid"}, {"Unione booleana", "_BooleanUnion"},
                      {"Differenza booleana", "_BooleanDifference"}, {"Intersezione booleana", "_BooleanIntersection"},
                      {"Divisione booleana", "_BooleanSplit"}, {"Raccorda spigoli", "_FilletEdge"}, {"Dividi faccia (SplitFace)", "_SplitFace"},
                      {"Smussa spigoli", "_ChamferEdge"}, {"Chiudi fori piani", "_Cap"}, {"Guscio", "_Shell"}});
  addCmds("Su&bD", {{"Parallelepipedo SubD", "_SubDBox"}, {"Sfera SubD", "_SubDSphere"}, {"Cilindro SubD", "_SubDCylinder"}, {"Cono SubD", "_SubDCone"}, {"Converti in SubD", "_ToSubD"},
                    {"Toro SubD", "_SubDTorus"}, {"Piano SubD", "_SubDPlane"}, {"SubD da mesh", "_SubDFromMesh"},
                    {"Piega spigoli", "_Crease"}, {"Togli piega", "_Uncrease"}, {"Liscio / piatto", "_SubDDisplayToggle"}, {"Loft SubD", "_SubDLoft"}, {"Chiudi fori SubD (Fill)", "_Fill"}, {"Rifletti (Reflect)", "_Reflect"}, {"Sweep SubD", "_SubDSweep"}, {"Estrudi faccia SubD", "_SubDExtrude"}, {"Ponte (Bridge)", "_Bridge"}, {"Inserisci anello di spigoli", "_InsertEdge"}, {"Cuci (Stitch)", "_Stitch"}, {"Aggiungi faccia (Append)", "_Append"}, {"Quad remesh", "_QuadRemesh"}, {"Smussa spigoli SubD (Bevel)", "_Bevel"}, {"Tubi SubD (MultiPipe)", "_MultiPipe"}});
  addCmds("&Mesh", {{"Mesh da NURBS", "_Mesh"}, {"Piano mesh", "_MeshPlane"}, {"Box mesh", "_MeshBox"},
                    {"Sfera mesh", "_MeshSphere"}, {"Cilindro mesh", "_MeshCylinder"}, {"Cono mesh", "_MeshCone"},
                    {"Toro mesh", "_MeshTorus"}, {"Mesh da polilinea", "_MeshPolyline"},
                    {"Salda", "_Weld"}, {"Unifica normali", "_UnifyMeshNormals"}, {"Chiudi fori", "_FillMeshHoles"}, {"Riduci", "_Reduce"}, {"Estrai parte", "_ExtractMeshPart"}, {"Separa vertici (Unweld)", "_Unweld"}, {"Separa spigoli", "_UnweldEdge"}, {"Unione mesh", "_MeshBooleanUnion"}, {"Differenza mesh", "_MeshBooleanDifference"}, {"Intersezione mesh", "_MeshBooleanIntersection"}, {"Mesh in NURBS", "_MeshToNURB"}, {"Mesh da punti e curve (MeshPatch)", "_MeshPatch"},
                    {"Avvolgi (ShrinkWrap)", "_ShrinkWrap"}, {"Offset mesh", "_OffsetMesh"}, {"Ripara mesh", "_MeshRepair"}, {"Controlla mesh", "_CheckMesh"}, {"Togli facce degeneri", "_CullDegenerateMeshFaces"}, {"Ricalcola normali", "_RebuildMeshNormals"}, {"Estrai mesh di rendering", "_ExtractRenderMesh"}});
  addCmds("&Quota", {{"Quota lineare", "_Dim"}, {"Quota allineata", "_DimAligned"}, {"Quota ruotata", "_DimRotated"},
                      {"Quota del raggio", "_DimRadius"}, {"Quota del diametro", "_DimDiameter"},
                      {"Quota angolare", "_DimAngle"}, {"Direttrice", "_Leader"}, {"Testo", "_Text"},
                      {"Segno del centro", "_Centermark"}, {"Quota coordinata", "_DimOrdinate"}, {"Lunghezza d'arco (DimArc)", "_DimArc"}, {"Lunghezza curva (DimCurveLength)", "_DimCurveLength"}, {"Stile di quota (DimStyle)", "_DimStyle"}, {"Testo in curve (TextObject)", "_TextObject"}});
  addCmds("&Trasforma", {{"Sposta", "_Move"}, {"Copia", "_Copy"}, {"Ruota", "_Rotate"}, {"Scala", "_Scale"},
                         {"Scala 2D", "_Scale2D"}, {"Specchia", "_Mirror"}, {"Serie", "_Array"},
                         {"Serie lineare", "_ArrayLinear"}, {"Serie polare", "_ArrayPolar"}, {"Inclina", "_Shear"}, {"Torci", "_Twist"}, {"Rastrema", "_Taper"}, {"Piega", "_Bend"}, {"Serie lungo curva", "_ArrayCrv"}, {"Serie su superficie", "_ArraySrf"}, {"Avvolgi su superficie (Splop)", "_Splop"}, {"Gabbia di controllo (CageEdit)", "_CageEdit"}, {"Scorri lungo curva (Flow)", "_Flow"}, {"Scorri su superficie", "_FlowAlongSrf"}, {"Orienta su superficie", "_OrientOnSrf"},
                         {"Allinea", "_Align"}, {"Imposta punti (SetPt)", "_SetPt"}});
  addCmds("&Analizza", {{"Cos'è (What)", "_What"}, {"Direzione (Dir)", "_Dir"}, {"Segna curvatura", "_MarkCurvature"}, {"Lunghezza", "_Length"}, {"Distanza", "_Distance"},
                         {"Angolo", "_Angle"}, {"Raggio", "_Radius"}, {"Area", "_Area"}, {"Volume", "_Volume"},
                         {"Scatola di ingombro", "_BoundingBox"}, {"Zebra", "_Zebra"}, {"Zebra spento", "_ZebraOff"},
                         {"Angolo di sformo", "_DraftAngleAnalysis"}, {"Angolo di sformo spento", "_DraftAngleAnalysisOff"},
                         {"Grafico di curvatura", "_CurvatureGraph"}, {"Grafico di curvatura spento", "_CurvatureGraphOff"},
                         {"Spigoli nudi", "_ShowEdges"}, {"Spigoli nudi spenti", "_ShowEdgesOff"},
                         {"Analisi di curvatura (falsi colori)", "_CurvatureAnalysis"}, {"Analisi di curvatura spenta", "_CurvatureAnalysisOff"},
                         {"Mappa d'ambiente (EMap)", "_EMap"}, {"Mappa d'ambiente spenta", "_EMapOff"}, {"Opzioni analisi...", "_AnalysisOptions"},
                         {"Continuità tra curve (GCon)", "_GCon"}, {"Continuità degli spigoli", "_EdgeContinuity"},
                         {"Deviazione dei punti", "_PointDeviation"}});
  addCmds("&Blocchi", {{"Crea blocco", "_Block"}, {"Inserisci blocco", "_Insert"}, {"Esplodi blocco", "_ExplodeBlock"}, {"Sostituisci blocco", "_ReplaceBlock"}, {"Rinomina blocco", "_RenameBlock"}, {"Gestione blocchi", "_BlockManager"}, {"Modifica blocco (BlockEdit)", "_BlockEdit"},
                       {"Raggruppa", "_Group"}, {"Separa gruppo", "_Ungroup"}});
  {
    auto* render = menuBar()->addMenu("&Render");
    render->addAction("Pannello Texture", this, [this] {
      for (auto* d : findChildren<QDockWidget*>())
        if (d->objectName() == "textureDock") { d->show(); d->raise(); }
    });
    render->addAction("Togli texture", this, [this] { Submit("_RemoveTexture"); });
    render->addSeparator();
    const std::pair<const char*, const char*> rcmds[] = {
        {"Rendering (Render)", "_Render"}, {"Materiale degli oggetti...", "_SetMaterial"},
        {"Luce puntiforme", "_PointLight"}, {"Luce direzionale", "_DirectionalLight"}, {"Faretto", "_SpotLight"},
        {"Luce rettangolare", "_RectangularLight"}, {"Sole acceso", "_Sun On"}, {"Sole spento", "_Sun Off"},
        {"Luce del cielo accesa", "_Skylight On"}, {"Luce del cielo spenta", "_Skylight Off"},
        {"Piano di terra acceso", "_GroundPlane On"}, {"Piano di terra spento", "_GroundPlane Off"},
        {"Vista renderizzata", "_SetDisplayMode Rendered"}, {"Vista raytraced", "_SetDisplayMode Raytraced"}};
    for (auto& [label, cmd] : rcmds) {
      const QString c = cmd;
      render->addAction(label, this, [this, c] { Submit(c); });
    }
    render->addSeparator();
    const std::pair<const char*, const char*> maps[] = {
        {"Mappatura box", "_ApplyBoxMapping"}, {"Mappatura piana", "_ApplyPlanarMapping"},
        {"Mappatura cilindrica", "_ApplyCylindricalMapping"}, {"Mappatura sferica", "_ApplySphericalMapping"},
        {"Mappatura superficie", "_ApplySurfaceMapping"}};
    for (auto& [label, cmd] : maps) {
      const QString c = cmd;
      render->addAction(label, this, [this, c] { Submit(c); });
    }
  }
  auto* scripts = menuBar()->addMenu("&Script");
  scripts->addAction("Editor di script", this, [this] { Submit("_ScriptEditor"); });
  scripts->addAction("Editor a nodi", this, [this] { Submit("_NodeEditor"); });
  scripts->addSeparator();
  scripts->addAction("Esegui script Python…", this, [this] {
    const QString f = QFileDialog::getOpenFileName(this, "Esegui script Python", QString(), "Script Python (*.py)");
    if (!f.isEmpty()) Run(("_RunPythonScript \"" + f + "\"").toStdString());
  });
  auto* collabMenu = menuBar()->addMenu("&Collabora");
  collabMenu->addAction("Pannello Collabora", this, [this] { Submit("_Collaborate"); });
  auto* aiMenu = menuBar()->addMenu("&AI");
  aiMenu->addAction("Assistente AI", this, [this] {
    for (auto* d : findChildren<QDockWidget*>())
      if (d->objectName() == "aiDock") { d->show(); d->raise(); }
  });
  aiMenu->addAction("Modello dell'assistente…", this, [this] { m_ai->ShowModelDialog(); });
  auto* help = menuBar()->addMenu("&?");
  help->addAction("Elenco comandi", this, [this] { Submit("?"); });
  help->addAction("Licenza d'uso", this, [this] { ShowTextFile("EULA.txt", "Licenza d'uso di Kurva3D"); });
  help->addAction("Componenti di terze parti", this, [this] { ShowTextFile("THIRD_PARTY_NOTICES.txt", "Componenti di terze parti"); });
  help->addSeparator();
  help->addAction("Informazioni su Kurva3D", this, [this] { ShowAbout(); });
}

// Informazioni su Kurva3D: versione e librerie di terze parti con le loro licenze
// (il testo completo e' in THIRD_PARTY_NOTICES.txt accanto all'eseguibile).
void MainWindow::ShowAbout() {
  QMessageBox box(this);
  box.setWindowTitle("Informazioni su Kurva3D");
  box.setTextFormat(Qt::RichText);
  box.setIconPixmap(QIcon(":/kurva3d.png").pixmap(64, 64));
  box.setText(QString("<h3>Kurva3D %1</h3><p>Copyright &copy; 2026 %3. Tutti i diritti riservati.</p>"
      "<p>Modellatore 3D NURBS indipendente, compatibile con i file .3dm.</p>"
      "<p><b>Librerie di terze parti</b></p><ul>"
      "<li>openNURBS 8 (Robert McNeel &amp; Associates) - licenza openNURBS (permissiva, tipo MIT)</li>"
      "<li>Open CASCADE Technology 7 - GNU LGPL 2.1 con eccezione aggiuntiva</li>"
      "<li>Qt %2 (The Qt Company) - GNU LGPL 3</li></ul>"
      "<p>Le librerie LGPL sono collegate dinamicamente (DLL) e si possono sostituire. "
      "Testi completi delle licenze e indirizzi dei sorgenti: THIRD_PARTY_NOTICES.txt.</p>"
      "<p>Rhinoceros e Rhino sono marchi di Robert McNeel &amp; Associates; "
      "Kurva3D non e' affiliato a McNeel.</p>")
      .arg(KURVA3D_VERSION).arg(qVersion()).arg(KURVA3D_PUBLISHER));
  box.exec();
}

// Testo (licenza d'uso, avvisi di terze parti) dalla cartella del programma.
void MainWindow::ShowTextFile(const QString& file, const QString& title) {
  QString text;
  for (const QString& dir : {QCoreApplication::applicationDirPath(), QCoreApplication::applicationDirPath() + "/../Resources", QCoreApplication::applicationDirPath() + "/..", QString(".")}) {
    QFile f(dir + "/" + file);
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) { text = QString::fromUtf8(f.readAll()); break; }
  }
  if (text.isEmpty()) text = "File " + file + " non trovato nella cartella del programma.";
  QDialog dlg(this);
  dlg.setWindowTitle(title);
  dlg.resize(720, 560);
  auto* lay = new QVBoxLayout(&dlg);
  auto* view = new QPlainTextEdit(text);
  view->setReadOnly(true);
  lay->addWidget(view);
  auto* bb = new QDialogButtonBox(QDialogButtonBox::Close);
  connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  lay->addWidget(bb);
  dlg.exec();
}

// Barre strumenti a schede sopra le viste, come Rhino 8 (pulsanti con testo:
// le icone di Rhino non si copiano).
QWidget* MainWindow::BuildToolTabs() {
  auto* tabs = new QTabWidget;
  tabs->setDocumentMode(true);
  tabs->setMaximumHeight(70);
  using Btn = std::pair<const char*, const char*>;
  auto addTab = [&](const char* title, std::initializer_list<Btn> buttons) {
    auto* tb = new QToolBar;
    tb->setIconSize(QSize(18, 18));
    tb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    for (auto& [label, cmd] : buttons) {
      QString c = cmd;
      QAction* a = tb->addAction(label, this, [this, c] { Submit(c); });
      a->setToolTip(QString("%1  (%2)").arg(label, c.mid(c.indexOf('_') + 1)));
      m_iconActions.push_back({a, c});
    }
    tabs->addTab(tb, title);
    return tb;
  };
  addTab("Standard", {{"Nuovo", "_New"}, {"Annulla", "_Undo"}, {"Ripeti", "_Redo"}, {"Seleziona tutto", "_SelAll"},
                      {"Elimina", "_Delete"}, {"Zoom estensione", "_Zoom"}, {"Cos'è", "_What"}, {"Livello", "-_Layer"}});
  // Imposta vista e Visualizzazione agiscono sulla vista attiva
  auto* views = new QToolBar;
  views->setToolButtonStyle(Qt::ToolButtonTextOnly);
  const std::pair<const char*, Camera::Kind> vk[] = {
      {"Top", Camera::Kind::Top}, {"Bottom", Camera::Kind::Bottom}, {"Left", Camera::Kind::Left},
      {"Right", Camera::Kind::Right}, {"Front", Camera::Kind::Front}, {"Back", Camera::Kind::Back},
      {"Perspective", Camera::Kind::Perspective}};
  for (auto& [label, k] : vk) views->addAction(label, this, [this, k = k] { Active()->SetView(k); });
  tabs->addTab(views, "Imposta vista");
  auto* disp = new QToolBar;
  disp->setToolButtonStyle(Qt::ToolButtonTextOnly);
  const std::pair<const char*, Viewport::DisplayMode> dm[] = {
      {"Wireframe", Viewport::DisplayMode::Wireframe}, {"Ombreggiato", Viewport::DisplayMode::Shaded},
      {"Fantasma", Viewport::DisplayMode::Ghosted}, {"Raggi X", Viewport::DisplayMode::XRay},
      {"Renderizzato", Viewport::DisplayMode::Rendered}, {"Raytraced", Viewport::DisplayMode::Raytraced}};
  for (auto& [label, m] : dm) disp->addAction(label, this, [this, m = m] { Active()->mode = m; Active()->update(); });
  tabs->addTab(disp, "Visualizzazione");
  addTab("Seleziona", {{"Tutto", "_SelAll"}, {"Niente", "_SelNone"}, {"Inverti", "_Invert"}, {"Ultimo", "_SelLast"},
                       {"Curve", "_SelCrv"}, {"Punti", "_SelPt"}, {"Superfici", "_SelSrf"}, {"Polisuperfici", "_SelPolysrf"},
                       {"Mesh", "_SelMesh"}, {"SubD", "_SelSubD"}, {"Per livello", "_SelLayer"}});
  addTab("Visibilità", {{"Nascondi", "_Hide"}, {"Mostra", "_Show"}, {"Blocca", "_Lock"}, {"Sblocca", "_Unlock"}});
  addTab("Trasforma", {{"Sposta", "_Move"}, {"Copia", "_Copy"}, {"Ruota", "_Rotate"}, {"Scala", "_Scale"},
                       {"Scala 2D", "_Scale2D"}, {"Specchia", "_Mirror"}, {"Serie", "_Array"},
                       {"Serie lineare", "_ArrayLinear"}, {"Serie polare", "_ArrayPolar"}});
  addTab("Strumenti curve", {{"Raccordo", "_Fillet"}, {"Smusso", "_Chamfer"}, {"Offset", "_Offset"}, {"Taglia", "_Trim"},
                             {"Estendi", "_Extend"}, {"Dividi", "_Split"}, {"Unisci", "_Join"}, {"Esplodi", "_Explode"},
                             {"Inverti dir.", "_Flip"}, {"Intersezione", "_Intersect"}, {"Copia bordi", "_DupBorder"},
                             {"Dividi in punti", "_Divide"}, {"Ricostruisci", "_Rebuild"}, {"Cambia grado", "_ChangeDegree"},
                             {"Elica", "_Helix"}, {"Spirale", "_Spiral"}, {"Proietta su CPlane", "_ProjectToCPlane"},
                             {"Proietta", "_Project"}, {"Curve di livello", "_Contour"}, {"Sezione", "_Section"},
                             {"Copia spigoli", "_DupEdge"}});
  addTab("Strumenti superfici", {{"Piano", "_Plane"}, {"Da curve planari", "_PlanarSrf"}, {"Estrudi", "_ExtrudeCrv"},
                                 {"Rivoluzione", "_Revolve"}, {"Loft", "_Loft"}, {"Sweep 1", "_Sweep1"}, {"Sweep 2", "_Sweep2"}, {"Rete di curve", "_NetworkSrf"}, {"Rivoluzione su binario", "_RailRevolve"}, {"Estrudi rastremato", "_ExtrudeCrvTapered"}, {"Estendi", "_ExtendSrf"},
                                 {"Offset superficie", "_OffsetSrf"}, {"Dividi", "_Split"}, {"Estrai superficie", "_ExtractSrf"},
                                 {"Da punti", "_SrfPt"}, {"Da bordi", "_EdgeSrf"}, {"Patch", "_Patch"}, {"Rimuovi tagli", "_UntrimAll"}});
  addTab("Strumenti solidi", {{"Box", "_Box"}, {"Sfera", "_Sphere"}, {"Cilindro", "_Cylinder"}, {"Cono", "_Cone"},
                              {"Toro", "_Torus"}, {"Tubo", "_Pipe"}, {"Piramide", "_Pyramid"}, {"Cuneo", "_Wedge"}, {"Unione", "_BooleanUnion"},
                              {"Differenza", "_BooleanDifference"}, {"Intersezione", "_BooleanIntersection"},
                              {"Divisione", "_BooleanSplit"}, {"Raccorda spigoli", "_FilletEdge"},
                              {"Smussa spigoli", "_ChamferEdge"}, {"Chiudi fori", "_Cap"}, {"Guscio", "_Shell"},
                              {"Estrudi superficie", "_ExtrudeSrf"}});
  addTab("Strumenti SubD", {{"Box SubD", "_SubDBox"}, {"Sfera", "_SubDSphere"}, {"Cilindro", "_SubDCylinder"}, {"Cono", "_SubDCone"},
                            {"Toro", "_SubDTorus"}, {"Piano", "_SubDPlane"}, {"Da mesh", "_SubDFromMesh"}});
  addTab("Strumenti mesh", {{"Mesh da NURBS", "_Mesh"}, {"Piano", "_MeshPlane"}, {"Box", "_MeshBox"},
                            {"Sfera", "_MeshSphere"}, {"Cilindro", "_MeshCylinder"}, {"Toro", "_MeshTorus"}});
  addTab("Analizza", {{"Cos'è", "_What"}, {"Lunghezza", "_Length"}, {"Distanza", "_Distance"}, {"Angolo", "_Angle"},
                      {"Raggio", "_Radius"}, {"Area", "_Area"}, {"Volume", "_Volume"}, {"Ingombro", "_BoundingBox"},
                      {"Zebra", "_Zebra"}, {"Sformo", "_DraftAngleAnalysis"}, {"Curvatura", "_CurvatureGraph"}, {"Spigoli", "_ShowEdges"},
                      {"Falsi colori", "_CurvatureAnalysis"}, {"EMap", "_EMap"}, {"GCon", "_GCon"}, {"Continuità spigoli", "_EdgeContinuity"},
                      {"Deviazione punti", "_PointDeviation"}});
  addTab("Disegno", {{"Lineare", "_Dim"}, {"Allineata", "_DimAligned"}, {"Ruotata", "_DimRotated"},
                     {"Raggio", "_DimRadius"}, {"Diametro", "_DimDiameter"}, {"Angolare", "_DimAngle"},
                     {"Direttrice", "_Leader"}, {"Testo", "_Text"}, {"Segno del centro", "_Centermark"}});
  addTab("Blocchi", {{"Crea blocco", "_Block"}, {"Inserisci", "_Insert"}, {"Esplodi blocco", "_ExplodeBlock"},
                     {"Raggruppa", "_Group"}, {"Separa gruppo", "_Ungroup"}});
  return tabs;
}

// Immagine della vista attiva (ViewCaptureToFile / ViewCaptureToClipboard).
bool MainWindow::CaptureView(const QString& path, int width, int height, bool grid) {
  Viewport* v = Active();
  const bool old = v->showGrid;
  v->showGrid = grid;
  QImage img = v->grabFramebuffer();
  v->showGrid = old;
  v->update();
  if (img.isNull()) return false;
  if (width > 0 || height > 0) {
    const QSize s(width > 0 ? width : img.width() * height / std::max(1, img.height()),
                  height > 0 ? height : img.height() * width / std::max(1, img.width()));
    img = img.scaled(s, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
  }
  if (path.isEmpty()) {
    QGuiApplication::clipboard()->setImage(img);
    return true;
  }
  return img.save(path);
}

bool MainWindow::ApplyTextureToSelection(const QString& file) {
  if (m_engine.Selection().empty()) return false;
  Submit("_ApplyTexture File= \"" + QDir::fromNativeSeparators(file) + "\"" + m_textures->CommandOptions());
  return true;
}

void MainWindow::BuildSidebar() {
  // Barra laterale a due colonne con i comandi piu' usati: icone originali di
  // Kurva3D (disegnate dal programma), nome e comando nel suggerimento.
  auto* tb = new QToolBar("Comandi");
  tb->setOrientation(Qt::Vertical);
  tb->setMovable(false);
  addToolBar(Qt::LeftToolBarArea, tb);
  auto* panel = new QWidget;
  panel->setObjectName("sidebar");
  auto* grid = new QGridLayout(panel);
  grid->setContentsMargins(2, 2, 2, 2);
  grid->setSpacing(2);
  const std::pair<const char*, const char*> items[] = {
    {"Linea", "_Line"}, {"Polilinea", "_Polyline"}, {"Cerchio", "_Circle"}, {"Arco", "_Arc"},
    {"Rettangolo", "_Rectangle"}, {"Curva", "_Curve"}, {"Interp", "_InterpCrv"}, {"Box", "_Box"},
    {"Sfera", "_Sphere"}, {"Cilindro", "_Cylinder"}, {"Estrudi", "_ExtrudeCrv"}, {"Unione", "_BooleanUnion"},
    {"Differenza", "_BooleanDifference"}, {"Raccordo", "_FilletEdge"}, {"Offset", "_Offset"},
    {"Taglia", "_Trim"}, {"Estendi", "_Extend"}, {"Racc. curve", "_Fillet"},
    {"Sposta", "_Move"}, {"Copia", "_Copy"}, {"Ruota", "_Rotate"}, {"Scala", "_Scale"},
    {"Specchia", "_Mirror"}, {"Unisci", "_Join"}, {"Esplodi", "_Explode"}, {"Dividi", "_Split"},
    {"Elimina", "_Delete"}};
  int n = 0;
  for (auto& [label, cmd] : items) {
    const QString c = cmd;
    auto* b = new QToolButton;
    auto* a = new QAction(label, b);
    a->setToolTip(QString("%1  (%2)").arg(label, c.mid(1)));
    connect(a, &QAction::triggered, this, [this, c] { Submit(c); });
    b->setDefaultAction(a);
    b->setIconSize(QSize(24, 24));
    b->setFixedSize(36, 36);
    b->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_iconActions.push_back({a, c});
    grid->addWidget(b, n / 2, n % 2);
    n++;
  }
  grid->setRowStretch(n / 2 + 1, 1);
  tb->addWidget(panel);
}

// Tema dell'interfaccia: tavolozza, icone e colori delle viste.
void MainWindow::SetTheme(ThemeMode m) {
  SaveThemeMode(m);
  ApplyTheme(m);
  RefreshThemeIcons();
}

void MainWindow::RefreshThemeIcons() {
  for (auto& [a, c] : m_iconActions) a->setIcon(CommandIcon(c));
  for (auto* v : m_views)
    if (v) v->update();
}

void MainWindow::BuildStatusBar() {
  auto& S = Viewport::Shared();
  // Barra Osnap (in basso, come Rhino 8)
  auto* osnapBar = new QToolBar("Osnap");
  osnapBar->setMovable(false);
  addToolBar(Qt::BottomToolBarArea, osnapBar);
  struct Item { const char* label; unsigned bit; bool on; };
  const Item items[] = {{"Fine", Viewport::End, true},       {"Vicino", Viewport::Near, false},
                        {"Punto", Viewport::Point, true},    {"Medio", Viewport::Mid, true},
                        {"Centro", Viewport::Cen, true},     {"Intersezione", Viewport::Int, true},
                        {"Perpendicolare", Viewport::Perp, false}, {"Quadrante", Viewport::Quad, false},
                        {"Nodo", Viewport::Knot, false},     {"Vertice", Viewport::Vertex, false}};
  for (const Item& it : items) {
    auto* cb = new QCheckBox(it.label);
    cb->setChecked(it.on);
    if (it.on) S.osnapMask |= it.bit;
    const unsigned bit = it.bit;
    connect(cb, &QCheckBox::toggled, this, [bit](bool on) {
      auto& St = Viewport::Shared();
      if (on) St.osnapMask |= bit; else St.osnapMask &= ~bit;
    });
    osnapBar->addWidget(cb);
  }
  auto* disable = new QCheckBox("Disabilita");
  connect(disable, &QCheckBox::toggled, this, [](bool on) { Viewport::Shared().osnap = !on; });
  osnapBar->addSeparator();
  osnapBar->addWidget(disable);

  m_coords = new QLabel("x 0.000   y 0.000   z 0.000");
  m_coords->setMinimumWidth(260);
  statusBar()->addWidget(m_coords);
  statusBar()->addWidget(new QLabel("Millimetri"));
  auto toggle = [&](const char* label, bool& flag) {
    auto* b = new QToolButton;
    b->setText(label);
    b->setCheckable(true);
    b->setChecked(flag);
    connect(b, &QToolButton::toggled, this, [&flag](bool on) { flag = on; });
    statusBar()->addPermanentWidget(b);
  };
  toggle("Snap griglia", S.gridSnap);
  toggle("Orto", S.ortho);
  toggle("Gumball", S.gumball);
  {  // clic destro sul Gumball: allineamento (come il menu del Gumball di Rhino 8)
    auto* gb = statusBar()->findChildren<QToolButton*>().last();
    gb->setToolTip("Gumball (clic destro: allinea al mondo, al CPlane o all'oggetto)");
    gb->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(gb, &QToolButton::customContextMenuRequested, this, [this, gb](const QPoint& pos) {
      QMenu m;
      const char* names[3] = {"Allinea al mondo", "Allinea al CPlane", "Allinea all'oggetto"};
      const char* cmds[3] = {"GumballAlignment World", "GumballAlignment CPlane", "GumballAlignment Object"};
      for (int i = 0; i < 3; i++) {
        QAction* a = m.addAction(names[i]);
        a->setCheckable(true);
        a->setChecked(Viewport::Shared().gumballAlign == i);
        connect(a, &QAction::triggered, this, [this, i, cmds] { Submit(cmds[i]); });
      }
      m.exec(gb->mapToGlobal(pos));
    });
  }
  toggle("Osnap", S.osnap);
  // Registra storia (History Record): i comandi che la supportano ricordano gli ingressi
  m_historyBtn = new QToolButton;
  m_historyBtn->setText("Registra storia");
  m_historyBtn->setToolTip("Storia di costruzione: gli oggetti creati si aggiornano quando cambiano gli ingressi (History)");
  m_historyBtn->setCheckable(true);
  connect(m_historyBtn, &QToolButton::toggled, this, [this](bool on) { m_engine.recordHistory = on; });
  statusBar()->addPermanentWidget(m_historyBtn);
  auto* filt = new QToolButton;
  filt->setText("Filtro");
  filt->setToolTip("Filtro di selezione (SelectionFilter)");
  connect(filt, &QToolButton::clicked, this, [this] { ShowSelectionFilter(); });
  statusBar()->addPermanentWidget(filt);
}

namespace {
// Tipi del filtro di selezione, come la barra SelectionFilter di Rhino 8.
enum FilterBit : unsigned { FPoint = 1, FCurve = 2, FSurface = 4, FPolysurface = 8, FMesh = 16, FSubD = 32,
                            FAnnotation = 64, FBlock = 128, FOther = 256 };
unsigned FilterBitOf(const ON_Geometry* g) {
  if (!g) return FOther;
  if (ON_Point::Cast(g) || ON_PointCloud::Cast(g)) return FPoint;
  if (ON_Curve::Cast(g)) return FCurve;
  if (ON_Mesh::Cast(g)) return FMesh;
  if (ON_SubD::Cast(g)) return FSubD;
  if (ON_Annotation::Cast(g) || ON_TextDot::Cast(g) || ON_Hatch::Cast(g)) return FAnnotation;
  if (ON_InstanceRef::Cast(g)) return FBlock;
  if (const auto* b = ON_Brep::Cast(g)) return b->m_F.Count() > 1 ? FPolysurface : FSurface;
  if (const auto* x = ON_Extrusion::Cast(g)) return x->IsCapped() ? FPolysurface : FSurface;
  if (ON_Surface::Cast(g)) return FSurface;
  return FOther;
}
}  // namespace

bool MainWindow::PassesFilter(const ON_UUID& id) const {
  const auto* o = m_engine.Object(id);
  return (FilterBitOf(o ? o->Geometry(nullptr) : nullptr) & m_selFilter) != 0;
}

void MainWindow::ShowSelectionFilter() {
  QDialog dlg(this);
  dlg.setWindowTitle("Filtro di selezione");
  auto* lay = new QVBoxLayout(&dlg);
  const std::pair<const char*, unsigned> items[] = {{"Punti", FPoint}, {"Curve", FCurve}, {"Superfici", FSurface},
                                                     {"Polisuperfici", FPolysurface}, {"Mesh", FMesh}, {"SubD", FSubD},
                                                     {"Annotazioni", FAnnotation}, {"Blocchi", FBlock}, {"Altri", FOther}};
  std::vector<std::pair<QCheckBox*, unsigned>> boxes;
  for (const auto& [label, bit] : items) {
    auto* cb = new QCheckBox(label);
    cb->setChecked((m_selFilter & bit) != 0);
    lay->addWidget(cb);
    boxes.push_back({cb, bit});
  }
  auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Reset);
  connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  connect(bb->button(QDialogButtonBox::Reset), &QPushButton::clicked, &dlg, [&] { for (auto& b : boxes) b.first->setChecked(true); });
  lay->addWidget(bb);
  if (dlg.exec() != QDialog::Accepted) return;
  m_selFilter = 0;
  for (auto& [cb, bit] : boxes) if (cb->isChecked()) m_selFilter |= bit;
  Log(m_selFilter == 0xFFFF || m_selFilter == 511 ? "Filtro di selezione: tutti i tipi" : "Filtro di selezione attivo");
}

// BlockManager: elenco dei blocchi con il numero di istanze; rinomina,
// selezione delle istanze ed eliminazione dei blocchi non usati (Purge).
void MainWindow::ShowBlockManager() {
  QDialog dlg(this);
  dlg.setWindowTitle("Gestione blocchi");
  dlg.resize(420, 320);
  auto* lay = new QVBoxLayout(&dlg);
  auto* table = new QTableWidget(&dlg);
  table->setColumnCount(2);
  table->setHorizontalHeaderLabels({"Nome", "Istanze"});
  table->horizontalHeader()->setStretchLastSection(true);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  auto fill = [&] {
    std::map<std::string, std::pair<ON_UUID, int>> blocks;
    ONX_ModelComponentIterator it(m_engine.Model(), ON_ModelComponent::Type::InstanceDefinition);
    for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent())
      blocks[ToUtf8(c->Name())] = {c->Id(), 0};
    for (const auto& id : m_engine.AllObjectIds(true))
      if (const auto* ir = ON_InstanceRef::Cast(m_engine.Object(id)->Geometry(nullptr)))
        for (auto& [n, v] : blocks) if (v.first == ir->m_instance_definition_uuid) v.second++;
    table->setRowCount((int)blocks.size());
    int r = 0;
    for (auto& [n, v] : blocks) {
      table->setItem(r, 0, new QTableWidgetItem(QString::fromStdString(n)));
      table->setItem(r, 1, new QTableWidgetItem(QString::number(v.second)));
      r++;
    }
  };
  fill();
  lay->addWidget(table);
  auto* row = new QHBoxLayout;
  auto* bRename = new QPushButton("Rinomina...");
  auto* bSelect = new QPushButton("Seleziona istanze");
  auto* bPurge = new QPushButton("Elimina non usati");
  auto* bClose = new QPushButton("Chiudi");
  row->addWidget(bRename);
  row->addWidget(bSelect);
  row->addWidget(bPurge);
  row->addStretch();
  row->addWidget(bClose);
  lay->addLayout(row);
  auto current = [&]() -> QString { auto* it = table->item(table->currentRow(), 0); return it ? it->text() : QString(); };
  connect(bRename, &QPushButton::clicked, &dlg, [&] {
    const QString n = current();
    if (n.isEmpty()) return;
    bool ok = false;
    const QString nn = QInputDialog::getText(&dlg, "Rinomina blocco", "Nuovo nome:", QLineEdit::Normal, n, &ok);
    if (!ok || nn.trimmed().isEmpty() || nn == n) return;
    Submit(QString("_RenameBlock \"%1\" \"%2\"").arg(n, nn.trimmed()));
    fill();
  });
  connect(bSelect, &QPushButton::clicked, &dlg, [&] {
    const QString n = current();
    if (n.isEmpty()) return;
    ON_UUID did = ON_nil_uuid;
    ONX_ModelComponentIterator it(m_engine.Model(), ON_ModelComponent::Type::InstanceDefinition);
    for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent())
      if (QString::fromStdString(ToUtf8(c->Name())) == n) did = c->Id();
    m_engine.ClearSelection();
    for (const auto& id : m_engine.AllObjectIds())
      if (const auto* ir = ON_InstanceRef::Cast(m_engine.Object(id)->Geometry(nullptr)))
        if (ir->m_instance_definition_uuid == did) m_engine.Select(id);
    m_scene.RefreshSelection();
    RefreshPanels();
    for (auto* v : m_views) v->update();
  });
  connect(bPurge, &QPushButton::clicked, &dlg, [&] { Submit("_Purge"); fill(); });
  connect(bClose, &QPushButton::clicked, &dlg, &QDialog::accept);
  dlg.exec();
}

// Viste del file aperto: le viste con lo stesso nome prendono camera e id (cosi' i piani di
// sezione del file restano attivi nelle viste giuste).
bool MainWindow::AdoptFileViews() {
  bool any = false;
  m_engine.viewportIds.clear();
  const auto& views = m_engine.Model().m_settings.m_views;
  for (auto* v : m_views) {
    for (int i = 0; i < views.Count(); i++)
      if (views[i].m_name.CompareOrdinal(ON_wString(v->Title().toStdWString().c_str()), true) == 0 &&
          !ON_UuidIsNil(views[i].m_vp.ViewportId())) {
        v->id = views[i].m_vp.ViewportId();
        v->FromView(views[i]);
        any = true;
      }
    m_engine.viewportIds.push_back(v->id);
  }
  m_engine.activeViewportId = Active()->id;
  m_scene.Rebuild();
  for (auto* v : m_views) v->update();
  return any;
}

bool MainWindow::OpenFile(const QString& path) {
  Submit("_Open \"" + path + "\"");
  const bool hasViews = AdoptFileViews();
  const bool is3dm = QFileInfo(path).suffix().compare("3dm", Qt::CaseInsensitive) == 0;
  if (is3dm && m_engine.currentPath.empty()) return false;
  if (is3dm && m_nodes) m_nodes->LoadFromDocument();
  if (!hasViews) ZoomExtentsAll();
  return true;
}

void MainWindow::ZoomExtentsAll() {
  for (auto* v : m_views) v->ZoomExtents();
}

// Modifica dal pannello Proprieta': annulla un eventuale comando in sospeso
// (altrimenti il testo diventerebbe la sua risposta) e agisce sulla selezione.
void MainWindow::PropertyCommand(const QString& cmd) {
  if (!m_pending.empty()) Cancel();
  Submit(cmd);
}

void MainWindow::Submit(const QString& text) {
  std::string t = text.trimmed().toStdString();
  if (t == "?") {
    std::map<std::string, std::vector<std::string>> byCat;
    for (auto& [k, c] : m_engine.Commands()) byCat[c.category].push_back(c.name);
    for (auto& [cat, list] : byCat) {
      QString l = QString::fromStdString(cat) + ": ";
      for (auto& n : list) l += QString::fromStdString(n) + " ";
      Log(l);
    }
    return;
  }
  // Heightfield senza argomenti: come Rhino, il file immagine si sceglie da una finestra
  if (m_pending.empty() && NormalizeName(t) == "heightfield") {
    const QString f = QFileDialog::getOpenFileName(this, "Immagine per Heightfield", QString(),
                                                   "Immagini (*.png *.jpg *.jpeg *.bmp *.tif *.tiff *.pgm);;Tutti i file (*)");
    if (f.isEmpty()) return;
    t = "_Heightfield \"" + f.toStdString() + "\"";
  }
  // Come Rhino: un nome di comando incompleto viene completato con il primo comando che inizia cosi'
  if (m_pending.empty() && !t.empty() && t.find(' ') == std::string::npos && t.find(',') == std::string::npos) {
    const std::string n = NormalizeName(t);
    if (!n.empty() && !m_engine.IsCommand(t)) {
      bool done = false;
      for (auto& [k, c] : m_engine.Commands())
        if (k.rfind(n, 0) == 0) { t = (t[0] == '-' ? "-_" : "_") + c.name; done = true; break; }
      if (!done)
        for (auto& [k, en] : m_engine.Aliases())
          if (k.rfind(n, 0) == 0) { t = (t[0] == '-' ? "-_" : "_") + m_engine.Commands().at(en).name; break; }
    }
  }
  const std::string norm = NormalizeName(t);
  if (norm == "zoom" || norm == "zea" || norm == "zoomextents") { ZoomExtentsAll(); return; }
  if (norm == "zoomselected" || norm == "zs") {
    ON_BoundingBox bb;
    for (const auto& obj : m_scene.Objects()) if (obj.selected) bb.Union(obj.bbox);
    if (bb.IsValid()) for (auto* v : m_views) v->ZoomBox(bb);
    return;
  }
  if (HandleAnalysisCommand(t)) return;
  if (m_pending.empty() && NormalizeName(t.substr(0, t.find(' '))) == "gumballalignment") {
    // GumballAlignment World|CPlane|Object (senza valore: si passa al successivo)
    auto& G = Viewport::Shared();
    const std::string v = t.find(' ') == std::string::npos ? std::string() : NormalizeName(t.substr(t.find(' ') + 1));
    if (v == "world" || v == "mondo") G.gumballAlign = 0;
    else if (v == "cplane") G.gumballAlign = 1;
    else if (v == "object" || v == "oggetto") G.gumballAlign = 2;
    else if (v.empty()) G.gumballAlign = (G.gumballAlign + 1) % 3;
    else { Log("GumballAlignment: World, CPlane o Object"); return; }
    const char* names[3] = {"mondo", "CPlane", "oggetto"};
    Log(QString("Gumball allineato: %1").arg(names[G.gumballAlign]));
    for (auto* vw : m_views) vw->update();
    return;
  }
  if (norm == "selectionfilter") { ShowSelectionFilter(); return; }
  if (norm == "blockmanager") { ShowBlockManager(); return; }
  if (norm == "about") { ShowAbout(); return; }
  if (norm == "properties") {  // pannello Proprieta'
    for (auto* d : findChildren<QDockWidget*>())
      if (d->windowTitle() == "Proprietà") { d->show(); d->raise(); }
    Log("Pannello Proprietà");
    return;
  }
  if (norm == "properties" || (norm == "layer" && t.find(' ') == std::string::npos)) {
    const QString title = norm == "properties" ? "Proprietà" : "Livelli";
    for (auto* d : findChildren<QDockWidget*>())
      if (d->windowTitle() == title) { d->show(); d->raise(); }
    return;
  }
  std::string script;
  if (m_pending.empty()) {
    if (t.empty()) {
      // Invio a vuoto ripete l'ultimo comando (come Rhino)
      if (m_engine.LastCommandName().empty()) return;
      t = "_" + m_engine.LastCommandName();
    }
    script = t;
  } else if (m_engine.PendingInputKind() == Engine::InputKind::Objects) {
    // "Seleziona oggetti ... poi Invio": Invio fa ripartire il comando con la selezione fatta
    script = t.empty() ? m_pending : m_pending + " " + t;
  } else {
    script = m_pending + " " + (t.empty() ? std::string("_Enter") : t);
  }
  Run(script);
}

void MainWindow::Run(const std::string& script) {
  m_out.str("");
  if (m_pending.empty()) Log(QString("Comando: ") + QString::fromStdString(script));
  const auto r = m_engine.RunInteractive(script, &m_pending);
  const QString out = QString::fromStdString(m_out.str()).trimmed();
  // nella cronologia solo i messaggi utili (gli errori "manca ..." sono le richieste)
  for (const QString& l : out.split('\n'))
    if (!l.trimmed().isEmpty() && !(r == Engine::Result::NeedsInput && l.contains("Errore:"))) Log(l);
  if (r != Engine::Result::NeedsInput) m_pending.clear();
  AfterCommand();
}

void MainWindow::Cancel() {
  if (!m_pending.empty()) Log("Comando annullato");
  m_pending.clear();
  AfterCommand();
}

void MainWindow::UpdatePrompt() {
  auto& S = Viewport::Shared();
  const auto kind = m_engine.PendingInputKind();
  const bool pending = !m_pending.empty();
  // in attesa di oggetti i clic selezionano; in attesa di punti i clic danno punti
  S.waitingForPoint = pending && (kind == Engine::InputKind::Point || kind == Engine::InputKind::Pick);
  S.pickOnObjects = pending && kind == Engine::InputKind::Pick;
  S.lastPoint = m_engine.lastPoint;
  // l'elastico parte dall'ultimo punto solo se il comando ne ha gia' ricevuto uno
  S.hasLastPoint = pending && kind == Engine::InputKind::Point && m_pending.find(',') != std::string::npos;
  if (m_pending.empty()) {
    m_prompt->setText("Comando:");
  } else {
    std::string name = NormalizeName(m_pending.substr(0, m_pending.find(' ')));
    if (const CommandInfo* ci = m_engine.FindCommand(m_pending.substr(0, m_pending.find(' ')))) name = ci->name;
    m_prompt->setText(QString::fromStdString(name + " - " + m_engine.Prompt() +
                                             " (Invio = fine, Esc = annulla):"));
  }
  // opzioni cliccabili (come "( Solid=No )" nella riga di comando di Rhino)
  QString opts;
  if (!m_pending.empty())
    if (const CommandInfo* ci = m_engine.FindCommand(m_pending.substr(0, m_pending.find(' '))))
      for (const std::string& spec : ci->options) {
        const std::string key = spec.substr(0, spec.find('='));
        std::string def = spec.substr(spec.find('=') + 1);
        if (!def.empty() && def[0] == '#') def = def.size() > 1 ? def.substr(1) : "auto";
        else def = def.substr(0, def.find('|'));
        std::string cur = def;
        std::istringstream in(m_pending);
        std::string tok;
        while (in >> tok) {
          const auto eq = tok.find('=');
          if (eq != std::string::npos && NormalizeName(tok.substr(0, eq)) == NormalizeName(key)) cur = tok.substr(eq + 1);
        }
        opts += QString(" <a href=\"%1\">%1=%2</a>").arg(QString::fromStdString(key), QString::fromStdString(cur).toHtmlEscaped());
      }
  m_options->setText(opts.isEmpty() ? QString() : "(" + opts + " )");
  m_options->setVisible(!opts.isEmpty());
}

// Clic su un'opzione: le scelte si alternano, i numeri si chiedono; l'opzione va subito dopo
// il nome del comando e il comando riparte con l'input gia' dato.
void MainWindow::OptionClicked(const QString& name) {
  if (m_pending.empty()) return;
  const CommandInfo* ci = m_engine.FindCommand(m_pending.substr(0, m_pending.find(' ')));
  if (!ci) return;
  const std::string key = name.toStdString();
  std::string spec;
  for (const auto& s : ci->options)
    if (s.substr(0, s.find('=')) == key) spec = s.substr(s.find('=') + 1);
  // token del comando senza l'opzione, e valore attuale
  std::vector<std::string> toks;
  {
    std::istringstream in(m_pending);
    std::string t;
    while (in >> t) toks.push_back(t);
  }
  std::string cur;
  for (size_t i = 1; i < toks.size();) {
    const auto eq = toks[i].find('=');
    if (eq != std::string::npos && NormalizeName(toks[i].substr(0, eq)) == NormalizeName(key)) {
      cur = toks[i].substr(eq + 1);
      toks.erase(toks.begin() + long(i));
    } else {
      i++;
    }
  }
  std::string value;
  if (!spec.empty() && spec[0] == '#') {
    bool ok = false;
    const double start = cur.empty() ? (spec.size() > 1 ? std::atof(spec.c_str() + 1) : 1.0) : std::atof(cur.c_str());
    const double v = QInputDialog::getDouble(this, QString::fromStdString(ci->name), QString::fromStdString(key), start, -1e9, 1e9, 4, &ok);
    if (!ok) return;
    value = QString::number(v, 'g', 10).toStdString();
  } else {
    std::vector<std::string> vals;
    std::istringstream in(spec);
    std::string v;
    while (std::getline(in, v, '|')) vals.push_back(v);
    if (vals.empty()) return;
    size_t i = 0;
    for (size_t k = 0; k < vals.size(); k++)
      if (NormalizeName(vals[k]) == NormalizeName(cur)) i = (k + 1) % vals.size();
    if (cur.empty()) i = vals.size() > 1 ? 1 : 0;  // dal predefinito al valore successivo
    value = vals[i];
  }
  toks.insert(toks.begin() + 1, key + "=" + value);
  std::string script;
  for (const auto& t : toks) script += (script.empty() ? "" : " ") + t;
  Log(QString("%1=%2").arg(QString::fromStdString(key), QString::fromStdString(value)));
  Run(script);
}

void MainWindow::AfterCommand() {
  if (m_collab) m_collab->LocalChanged();  // in una sessione le modifiche partono subito
  if (m_nodes && m_nodes->isVisible()) m_nodes->DocumentChanged();
  if (m_historyBtn && m_historyBtn->isChecked() != m_engine.recordHistory) {
    const QSignalBlocker block(m_historyBtn);
    m_historyBtn->setChecked(m_engine.recordHistory);
  }
  m_scene.Rebuild();
  if (!ON_UuidIsNil(m_engine.activeLayoutId)) m_layoutView->Refresh();
  UpdatePrompt();
  RefreshPanels();
  QString title = m_engine.currentPath.empty() ? "Senza titolo" : QString::fromStdString(m_engine.currentPath);
  setWindowTitle(title + " - Kurva3D");
  for (auto* v : m_views) v->update();
}

void MainWindow::RefreshPanels() {
  m_layers->clear();
  ONX_Model& m = m_engine.Model();
  const ON_UUID current = m.m_settings.CurrentLayerId();
  ONX_ModelComponentIterator it(m, ON_ModelComponent::Type::Layer);
  for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent()) {
    const auto* l = ON_Layer::Cast(c);
    auto* item = new QTreeWidgetItem(m_layers);
    item->setText(0, QString::fromStdString(ToUtf8(l->Name())));
    item->setText(1, ON_UuidCompare(l->Id(), current) == 0 ? "✓" : "");
    item->setText(2, l->IsVisible() ? "sì" : "no");
    const ON_Color col = l->Color();
    QPixmap sw(12, 12);  // colore del livello come campione accanto al nome
    sw.fill(QColor(col.Red(), col.Green(), col.Blue()));
    item->setIcon(0, QIcon(sw));
  }
  m_layers->resizeColumnToContents(0);
  // Proprieta' della selezione
  QString props;
  auto sel = m_engine.Selection();
  if (sel.empty()) props = "Nessun oggetto selezionato";
  m_propForm->setEnabled(!sel.empty());
  {
    const QSignalBlocker b1(m_propName), b2(m_propLayer), b3(m_propColor);
    m_propLayer->clear();
    ONX_ModelComponentIterator lit(m, ON_ModelComponent::Type::Layer);
    for (const ON_ModelComponent* c = lit.FirstComponent(); c; c = lit.NextComponent())
      m_propLayer->addItem(QString::fromStdString(ToUtf8(ON_Layer::Cast(c)->Name())), ON_Layer::Cast(c)->Index());
    // come Rhino: con valori diversi tra gli oggetti selezionati il campo resta vuoto ("varie")
    QString type, name, layer;
    int colorSrc = -1;
    ON_Color color = ON_Color::Black;
    bool first = true;
    for (const auto& id : sel) {
      const auto* o = m_engine.Object(id);
      const ON_3dmObjectAttributes* a = o->Attributes(nullptr);
      const ON_Geometry* g = o->Geometry(nullptr);
      QString t = QString::fromStdString(DescribeObject(g)).section(',', 0, 0);
      QString n = a ? QString::fromStdString(ToUtf8(a->Name())) : QString();
      int li = a ? a->m_layer_index : 0;
      const auto* lay = ON_Layer::Cast(m.ComponentFromIndex(ON_ModelComponent::Type::Layer, li).ModelComponent());
      QString ln = lay ? QString::fromStdString(ToUtf8(lay->Name())) : QString();
      int cs = !a ? 0 : a->ColorSource() == ON::color_from_layer ? 0 : a->ColorSource() == ON::color_from_parent ? 1 : 2;
      ON_Color col = cs == 2 ? a->m_color : (lay ? lay->Color() : ON_Color::Black);
      if (first) { type = t; name = n; layer = ln; colorSrc = cs; color = col; first = false; continue; }
      if (t != type) type = "varie";
      if (n != name) name.clear();
      if (ln != layer) layer.clear();
      if (cs != colorSrc) colorSrc = -1;
      if (col != color) color = ON_Color::UnsetColor;
    }
    if (sel.size() > 1) type = QString("%1 oggetti: %2").arg(sel.size()).arg(type);
    m_propType->setText(type);
    m_propName->setText(name);
    m_propName->setModified(false);
    m_propLayer->setCurrentIndex(layer.isEmpty() ? -1 : m_propLayer->findText(layer));
    m_propColor->setCurrentIndex(colorSrc);
    const QColor qc = color == ON_Color::UnsetColor ? QColor(Qt::transparent) : QColor(color.Red(), color.Green(), color.Blue());
    m_propSwatch->setStyleSheet(QString("background-color: %1; border: 1px solid #555;").arg(qc.name(QColor::HexArgb)));
    m_propSwatch->setPalette(QPalette(qc));
  }
  for (const auto& id : sel) {
    const auto* o = m_engine.Object(id);
    props += QString::fromStdString(DescribeObject(o->Geometry(nullptr))) + "\n";
  }
  m_props->setPlainText(props);
}

void MainWindow::OnPointPicked(ON_3dPoint p) {
  if (m_pending.empty()) return;
  char buf[128];
  std::snprintf(buf, sizeof(buf), "%.6g,%.6g,%.6g", p.x, p.y, p.z);
  Run(m_pending + " " + buf);
}

void MainWindow::OnObjectsClicked(std::vector<ON_UUID> ids, bool add, bool remove) {
  ids.erase(std::remove_if(ids.begin(), ids.end(), [&](const ON_UUID& id) { return !PassesFilter(id); }), ids.end());
  if (!add && !remove) m_engine.ClearSelection();
  {  // come Rhino: cliccando un oggetto di un gruppo si seleziona tutto il gruppo
    std::set<int> groups;
    for (const auto& id : ids)
      if (const auto* o = m_engine.Object(id)) {
        ON_SimpleArray<int> gl;
        o->Attributes(nullptr)->GetGroupList(gl);
        for (int i = 0; i < gl.Count(); i++) groups.insert(gl[i]);
      }
    if (!groups.empty())
      for (const auto& id : m_engine.AllObjectIds()) {
        ON_SimpleArray<int> gl;
        m_engine.Object(id)->Attributes(nullptr)->GetGroupList(gl);
        for (int i = 0; i < gl.Count(); i++)
          if (groups.count(gl[i])) { ids.push_back(id); break; }
      }
  }
  for (const auto& id : ids) {
    if (remove) m_engine.Deselect(id);
    else m_engine.Select(id);
  }
  m_scene.RefreshSelection();
  RefreshPanels();
  for (auto* v : m_views) v->update();
}

void MainWindow::ToggleMaximize(Viewport* v) {
  if (m_maximized) {
    for (auto* w : m_views) w->show();
    m_maximized = nullptr;
  } else {
    for (auto* w : m_views) if (w != v) w->hide();
    m_maximized = v;
  }
}

void MainWindow::keyPressEvent(QKeyEvent* e) {
  if (e->key() == Qt::Key_Escape) {
    if (!m_pending.empty()) Cancel();
    else if (m_engine.Selection().empty() && !m_engine.pointsOn.empty()) Submit("_PointsOff");  // come Rhino: Esc spegne i punti
    else { m_engine.ClearSelection(); m_scene.RefreshSelection(); RefreshPanels(); for (auto* v : m_views) v->update(); }
    return;
  }
  // I campi del pannello Proprieta' gestiscono da soli i tasti (Invio compreso).
  if (QWidget* f = QApplication::focusWidget();
      f && f != m_input && ((m_propForm && m_propForm->isAncestorOf(f)) || qobject_cast<QLineEdit*>(f) ||
                            qobject_cast<QComboBox*>(f) || qobject_cast<QAbstractItemView*>(f))) {
    QMainWindow::keyPressEvent(e);
    return;
  }
  // Come in Rhino: si scrive un comando senza cliccare sulla riga di comando.
  if (!e->text().isEmpty() && e->text()[0].isPrint() && e->key() != Qt::Key_Space) {
    m_input->setFocus();
    m_input->setText(m_input->text() + e->text());
    return;
  }
  if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter || e->key() == Qt::Key_Space) {
    const QString t = m_input->text();
    m_input->clear();
    Submit(t);
    return;
  }
  QMainWindow::keyPressEvent(e);
}

bool MainWindow::eventFilter(QObject* o, QEvent* e) {
  if (o == m_input && e->type() == QEvent::KeyPress) {
    auto* k = static_cast<QKeyEvent*>(e);
    if (k->key() == Qt::Key_Escape) { m_input->clear(); Cancel(); return true; }
    // Invio e (come in Rhino) Spazio confermano la riga; lo spazio resta nel testo solo tra virgolette.
    const bool inQuotes = m_input->text().count('"') % 2 == 1;
    if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter || (k->key() == Qt::Key_Space && !inQuotes)) {
      const QString t = m_input->text();
      m_input->clear();
      Submit(t);
      return true;  // non deve arrivare anche alla finestra (sarebbe un secondo Invio)
    }
  }
  return QMainWindow::eventFilter(o, e);
}

}  // namespace cad::ui

namespace cad::ui {

void MainWindow::RebuildLayoutTabs() {
  m_tabsUpdating = true;
  while (m_layoutTabs->count()) m_layoutTabs->removeTab(0);
  m_layoutTabs->addTab("Modello");
  int current = 0;
  const ONX_Model& m = m_engine.Model();
  for (int li : LayoutIndices(m)) {
    const auto& v = m.m_settings.m_views[li];
    const int t = m_layoutTabs->addTab(QString::fromStdString(ToUtf8(v.m_name)));
    if (v.m_vp.ViewportId() == m_engine.activeLayoutId) current = t;
  }
  m_layoutTabs->setCurrentIndex(current);
  m_tabsUpdating = false;
  if (current > 0) {
    m_layoutView->SetLayout(m_engine.activeLayoutId);
    m_stack->setCurrentIndex(1);
  } else {
    m_stack->setCurrentIndex(0);
  }
}

void MainWindow::NewLayoutDialog() {
  QDialog d(this);
  d.setWindowTitle("Nuova tavola");
  auto* form = new QFormLayout(&d);
  auto* name = new QLineEdit;
  for (int i = 1;; i++) {
    const std::string n = "Pagina " + std::to_string(i);
    if (FindLayout(m_engine.Model(), n) < 0) { name->setText(QString::fromStdString(n)); break; }
  }
  auto* size = new QComboBox;
  size->addItems({"A4", "A3", "A2", "A1", "A0", "Letter", "Tabloid"});
  auto* orient = new QComboBox;
  orient->addItems({"Orizzontale", "Verticale"});
  auto* details = new QComboBox;
  details->addItems({"1", "4", "0"});
  form->addRow("Nome", name);
  form->addRow("Formato", size);
  form->addRow("Orientamento", orient);
  form->addRow("Dettagli iniziali", details);
  auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
  connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
  form->addRow(bb);
  if (d.exec() != QDialog::Accepted || name->text().trimmed().isEmpty()) return;
  Submit(QString("_Layout \"%1\" Size=%2 Orientation=%3 Details=%4")
             .arg(name->text().trimmed(), size->currentText(), orient->currentIndex() ? "Portrait" : "Landscape",
                  details->currentText()));
}

bool MainWindow::PrintPages(const std::vector<PageDrawing>& pages) {
  if (pages.empty()) return false;
  QPrinter printer(QPrinter::HighResolution);
  printer.setPageSize(QPageSize(QSizeF(pages[0].widthMm, pages[0].heightMm), QPageSize::Millimeter));
  printer.setPageOrientation(QPageLayout::Portrait);
  printer.setFullPage(true);
  QPrintDialog dlg(&printer, this);
  dlg.setWindowTitle("Stampa");
  if (dlg.exec() != QDialog::Accepted) return false;
  QPainter p;
  if (!p.begin(&printer)) return false;
  for (size_t i = 0; i < pages.size(); i++) {
    if (i) {
      printer.setPageSize(QPageSize(QSizeF(pages[i].widthMm, pages[i].heightMm), QPageSize::Millimeter));
      printer.newPage();
    }
    const QRectF r = printer.pageLayout().fullRectPixels(printer.resolution());
    const double s = std::min(r.width() / pages[i].widthMm, r.height() / pages[i].heightMm);
    PaintPage(p, pages[i], s, 0, 0);
  }
  p.end();
  return true;
}

}  // namespace cad::ui
