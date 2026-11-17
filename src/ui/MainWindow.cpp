#include "ui/MainWindow.h"
#include <QAction>
#include <QCompleter>
#include <QStringListModel>
#include <QTabWidget>
#include <QApplication>
#include <QCheckBox>
#include <QDockWidget>
#include <QFileDialog>
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
#include <QVBoxLayout>
#include <set>

namespace cad::ui {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), m_engine(m_doc, m_out), m_scene(m_engine) {
  m_engine.interactive = true;
  setWindowTitle("Senza titolo - Rhinoceros CAD (personale)");
  resize(1400, 900);

  // Area comandi in alto (come Rhino 8): cronologia + riga "Comando:"
  auto* central = new QWidget;
  auto* vbox = new QVBoxLayout(central);
  vbox->setContentsMargins(2, 2, 2, 2);
  vbox->setSpacing(2);
  m_history = new QPlainTextEdit;
  m_history->setReadOnly(true);
  m_history->setMaximumHeight(70);
  m_history->setFont(QFont("Consolas", 9));
  vbox->addWidget(m_history);
  auto* line = new QHBoxLayout;
  m_prompt = new QLabel("Comando:");
  m_input = new QLineEdit;
  m_input->setFont(QFont("Consolas", 10));
  m_input->installEventFilter(this);
  {  // completamento automatico dei nomi dei comandi (come la riga di comando di Rhino 8)
    QStringList names;
    for (auto& [k, c] : m_engine.Commands()) names << QString::fromStdString(c.name);
    names.sort(Qt::CaseInsensitive);
    auto* comp = new QCompleter(names, this);
    comp->setCaseSensitivity(Qt::CaseInsensitive);
    comp->setCompletionMode(QCompleter::PopupCompletion);
    m_input->setCompleter(comp);
    m_completer = comp;
  }
  line->addWidget(m_prompt);
  line->addWidget(m_input, 1);
  vbox->addLayout(line);
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
    connect(m_views[i], &Viewport::ToggleMaximize, this, &MainWindow::ToggleMaximize);
    connect(m_views[i], &Viewport::Activated, this, [this](Viewport* v) { m_active = v; });
    connect(m_views[i], &Viewport::MouseWorld, this, [this](ON_3dPoint p) {
      m_coords->setText(QString("x %1   y %2   z %3").arg(p.x, 0, 'f', 3).arg(p.y, 0, 'f', 3).arg(p.z, 0, 'f', 3));
    });
  }
  vbox->addWidget(views, 1);
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
  auto* propsDock = new QDockWidget("Proprietà", this);
  m_props = new QPlainTextEdit;
  m_props->setReadOnly(true);
  propsDock->setWidget(m_props);
  addDockWidget(Qt::RightDockWidgetArea, propsDock);

  BuildMenus();
  BuildSidebar();
  BuildStatusBar();
  Log("Rhinoceros CAD (personale) - modello nuovo in millimetri. Scrivi un comando, ? per l'elenco.");
  AfterCommand();
}

void MainWindow::Log(const QString& s) {
  m_history->appendPlainText(s);
  m_history->verticalScrollBar()->setValue(m_history->verticalScrollBar()->maximum());
}

void MainWindow::BuildMenus() {
  auto* file = menuBar()->addMenu("&File");
  file->addAction("Nuovo", QKeySequence::New, this, [this] { Submit("_New"); });
  file->addAction("Apri...", QKeySequence::Open, this, [this] {
    QString p = QFileDialog::getOpenFileName(this, "Apri", QString(), "Modelli Rhino (*.3dm)");
    if (!p.isEmpty()) OpenFile(p);
  });
  file->addAction("Salva", QKeySequence::Save, this, [this] {
    if (m_engine.currentPath.empty()) {
      QString p = QFileDialog::getSaveFileName(this, "Salva", QString(), "Modelli Rhino 8 (*.3dm)");
      if (p.isEmpty()) return;
      Submit("_SaveAs \"" + p + "\"");
    } else {
      Submit("_Save");
    }
  });
  file->addAction("Salva con nome...", QKeySequence::SaveAs, this, [this] {
    QString p = QFileDialog::getSaveFileName(this, "Salva con nome", QString(), "Modelli Rhino 8 (*.3dm)");
    if (!p.isEmpty()) Submit("_SaveAs \"" + p + "\"");
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

  auto* view = menuBar()->addMenu("&Vista");
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

  auto addCmds = [&](const char* title, std::initializer_list<std::pair<const char*, const char*>> items) {
    auto* m = menuBar()->addMenu(title);
    for (auto& [label, cmd] : items) {
      QString c = cmd;
      m->addAction(label, this, [this, c] { Submit(c); });
    }
  };
  addCmds("&Curve", {{"Punto", "_Point"}, {"Linea", "_Line"}, {"Polilinea", "_Polyline"}, {"Rettangolo", "_Rectangle"},
                     {"Poligono", "_Polygon"}, {"Cerchio", "_Circle"}, {"Arco", "_Arc"}, {"Ellisse", "_Ellipse"},
                     {"Curva per punti di controllo", "_Curve"}, {"Curva interpolata", "_InterpCrv"},
                     {"Offset curva", "_Offset"}, {"Raccordo curve", "_Fillet"}, {"Smusso curve", "_Chamfer"},
                     {"Intersezione", "_Intersect"}, {"Copia bordi", "_DupBorder"},
                     {"Dividi in punti", "_Divide"}, {"Ricostruisci", "_Rebuild"}, {"Cambia grado", "_ChangeDegree"},
                     {"Elica", "_Helix"}, {"Spirale", "_Spiral"}});
  addCmds("&Superfici", {{"Piano", "_Plane"}, {"Da curve planari", "_PlanarSrf"}, {"Estrudi curva", "_ExtrudeCrv"},
                         {"Rivoluzione", "_Revolve"}, {"Loft", "_Loft"}, {"Sweep 1 binario", "_Sweep1"},
                         {"Offset superficie", "_OffsetSrf"}});
  addCmds("S&olidi", {{"Parallelepipedo", "_Box"}, {"Sfera", "_Sphere"}, {"Cilindro", "_Cylinder"}, {"Cono", "_Cone"},
                      {"Toro", "_Torus"}, {"Tubo", "_Pipe"}, {"Unione booleana", "_BooleanUnion"},
                      {"Differenza booleana", "_BooleanDifference"}, {"Intersezione booleana", "_BooleanIntersection"},
                      {"Divisione booleana", "_BooleanSplit"}, {"Raccorda spigoli", "_FilletEdge"},
                      {"Smussa spigoli", "_ChamferEdge"}, {"Chiudi fori piani", "_Cap"}, {"Guscio", "_Shell"}});
  addCmds("Su&bD", {{"Parallelepipedo SubD", "_SubDBox"}});
  addCmds("&Mesh", {{"Mesh da NURBS", "_Mesh"}});
  addCmds("&Quota", {{"Quota lineare", "_Dim"}, {"Quota allineata", "_DimAligned"}, {"Quota ruotata", "_DimRotated"},
                      {"Quota del raggio", "_DimRadius"}, {"Quota del diametro", "_DimDiameter"},
                      {"Quota angolare", "_DimAngle"}, {"Direttrice", "_Leader"}, {"Testo", "_Text"},
                      {"Segno del centro", "_Centermark"}});
  addCmds("&Trasforma", {{"Sposta", "_Move"}, {"Copia", "_Copy"}, {"Ruota", "_Rotate"}, {"Scala", "_Scale"},
                         {"Scala 2D", "_Scale2D"}, {"Specchia", "_Mirror"}, {"Serie", "_Array"},
                         {"Serie lineare", "_ArrayLinear"}, {"Serie polare", "_ArrayPolar"}});
  addCmds("&Analizza", {{"Cos'è (What)", "_What"}, {"Lunghezza", "_Length"}, {"Distanza", "_Distance"},
                         {"Angolo", "_Angle"}, {"Raggio", "_Radius"}, {"Area", "_Area"}, {"Volume", "_Volume"},
                         {"Scatola di ingombro", "_BoundingBox"}});
  addCmds("&Blocchi", {{"Crea blocco", "_Block"}, {"Inserisci blocco", "_Insert"}, {"Esplodi blocco", "_ExplodeBlock"},
                       {"Raggruppa", "_Group"}, {"Separa gruppo", "_Ungroup"}});
}

// Barre strumenti a schede sopra le viste, come Rhino 8 (pulsanti con testo:
// le icone di Rhino non si copiano).
QWidget* MainWindow::BuildToolTabs() {
  auto* tabs = new QTabWidget;
  tabs->setDocumentMode(true);
  tabs->setMaximumHeight(62);
  using Btn = std::pair<const char*, const char*>;
  auto addTab = [&](const char* title, std::initializer_list<Btn> buttons) {
    auto* tb = new QToolBar;
    tb->setIconSize(QSize(16, 16));
    tb->setToolButtonStyle(Qt::ToolButtonTextOnly);
    for (auto& [label, cmd] : buttons) {
      QString c = cmd;
      tb->addAction(label, this, [this, c] { Submit(c); });
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
      {"Fantasma", Viewport::DisplayMode::Ghosted}, {"Raggi X", Viewport::DisplayMode::XRay}};
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
                                 {"Rivoluzione", "_Revolve"}, {"Loft", "_Loft"}, {"Sweep 1", "_Sweep1"},
                                 {"Offset superficie", "_OffsetSrf"}, {"Dividi", "_Split"}, {"Estrai superficie", "_ExtractSrf"}});
  addTab("Strumenti solidi", {{"Box", "_Box"}, {"Sfera", "_Sphere"}, {"Cilindro", "_Cylinder"}, {"Cono", "_Cone"},
                              {"Toro", "_Torus"}, {"Tubo", "_Pipe"}, {"Unione", "_BooleanUnion"},
                              {"Differenza", "_BooleanDifference"}, {"Intersezione", "_BooleanIntersection"},
                              {"Divisione", "_BooleanSplit"}, {"Raccorda spigoli", "_FilletEdge"},
                              {"Smussa spigoli", "_ChamferEdge"}, {"Chiudi fori", "_Cap"}, {"Guscio", "_Shell"},
                              {"Estrudi superficie", "_ExtrudeSrf"}});
  addTab("Strumenti SubD", {{"Box SubD", "_SubDBox"}});
  addTab("Strumenti mesh", {{"Mesh da NURBS", "_Mesh"}});
  addTab("Analizza", {{"Cos'è", "_What"}, {"Lunghezza", "_Length"}, {"Distanza", "_Distance"}, {"Angolo", "_Angle"},
                      {"Raggio", "_Radius"}, {"Area", "_Area"}, {"Volume", "_Volume"}, {"Ingombro", "_BoundingBox"}});
  addTab("Disegno", {{"Lineare", "_Dim"}, {"Allineata", "_DimAligned"}, {"Ruotata", "_DimRotated"},
                     {"Raggio", "_DimRadius"}, {"Diametro", "_DimDiameter"}, {"Angolare", "_DimAngle"},
                     {"Direttrice", "_Leader"}, {"Testo", "_Text"}, {"Segno del centro", "_Centermark"}});
  addTab("Blocchi", {{"Crea blocco", "_Block"}, {"Inserisci", "_Insert"}, {"Esplodi blocco", "_ExplodeBlock"},
                     {"Raggruppa", "_Group"}, {"Separa gruppo", "_Ungroup"}});
  return tabs;
}

void MainWindow::BuildSidebar() {
  // Barra laterale con i comandi piu' usati (testo: le icone di Rhino non si copiano).
  auto* tb = new QToolBar("Comandi");
  tb->setOrientation(Qt::Vertical);
  addToolBar(Qt::LeftToolBarArea, tb);
  const std::pair<const char*, const char*> items[] = {
    {"Linea", "_Line"}, {"Polilinea", "_Polyline"}, {"Cerchio", "_Circle"}, {"Arco", "_Arc"},
    {"Rettangolo", "_Rectangle"}, {"Curva", "_Curve"}, {"Interp", "_InterpCrv"}, {"Box", "_Box"},
    {"Sfera", "_Sphere"}, {"Cilindro", "_Cylinder"}, {"Estrudi", "_ExtrudeCrv"}, {"Unione", "_BooleanUnion"},
    {"Differenza", "_BooleanDifference"}, {"Raccordo", "_FilletEdge"}, {"Offset", "_Offset"},
    {"Taglia", "_Trim"}, {"Estendi", "_Extend"}, {"Racc. curve", "_Fillet"},
    {"Sposta", "_Move"}, {"Copia", "_Copy"}, {"Ruota", "_Rotate"}, {"Scala", "_Scale"},
    {"Specchia", "_Mirror"}, {"Unisci", "_Join"}, {"Esplodi", "_Explode"}, {"Dividi", "_Split"},
    {"Elimina", "_Delete"}};
  for (auto& [label, cmd] : items) {
    QString c = cmd;
    tb->addAction(label, this, [this, c] { Submit(c); });
  }
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
  toggle("Osnap", S.osnap);
}

bool MainWindow::OpenFile(const QString& path) {
  Submit("_Open \"" + path + "\"");
  if (m_engine.currentPath.empty()) return false;
  ZoomExtentsAll();
  return true;
}

void MainWindow::ZoomExtentsAll() {
  for (auto* v : m_views) v->ZoomExtents();
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
  // Come Rhino: un nome di comando incompleto viene completato con il primo comando che inizia cosi'
  if (m_pending.empty() && !t.empty() && t.find(' ') == std::string::npos && t.find(',') == std::string::npos) {
    const std::string n = NormalizeName(t);
    if (!n.empty() && !m_engine.Commands().count(n)) {
      for (auto& [k, c] : m_engine.Commands())
        if (k.rfind(n, 0) == 0) { t = (t[0] == '-' ? "-_" : "_") + c.name; break; }
    }
  }
  const std::string norm = NormalizeName(t);
  if (norm == "zoom" || norm == "zea" || norm == "zoomextents") { ZoomExtentsAll(); return; }
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
    auto it = m_engine.Commands().find(name);
    if (it != m_engine.Commands().end()) name = it->second.name;
    m_prompt->setText(QString::fromStdString(name + " - " + m_engine.Prompt() +
                                             " (Invio = fine, Esc = annulla):"));
  }
}

void MainWindow::AfterCommand() {
  m_scene.Rebuild();
  UpdatePrompt();
  RefreshPanels();
  QString title = m_engine.currentPath.empty() ? "Senza titolo" : QString::fromStdString(m_engine.currentPath);
  setWindowTitle(title + " - Rhinoceros CAD (personale)");
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
    item->setForeground(0, QColor(col.Red(), col.Green(), col.Blue()));
  }
  m_layers->resizeColumnToContents(0);
  // Proprieta' della selezione
  QString props;
  auto sel = m_engine.Selection();
  if (sel.empty()) props = "Nessun oggetto selezionato";
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
    else { m_engine.ClearSelection(); m_scene.RefreshSelection(); RefreshPanels(); for (auto* v : m_views) v->update(); }
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
