// Finestre di servizio come in Rhino 8:
//  - Opzioni (Options / DocumentProperties): unita' e tolleranze, griglia, osnap,
//    SmartTrack, alias dei comandi, aspetto (tema), PrintDisplay;
//  - Cronologia dei comandi (CommandHistory, F2);
//  - pannelli Viste con nome, CPlane con nome, Istantanee, Selezioni con nome, Stati dei livelli.
// Le modifiche al documento passano dai comandi (annullabili e registrate nella cronologia).
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/Viewport.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace cad::ui {

namespace {
QString Quote(const QString& s) { return "\"" + QString(s).remove('"') + "\""; }
}  // namespace

// --- alias salvati tra le sessioni -------------------------------------------------------
void MainWindow::LoadAliases() {
  QSettings st;
  st.beginGroup("alias");
  for (const QString& k : st.childKeys()) m_engine.userAliases[NormalizeName(k.toStdString())] = st.value(k).toString().toStdString();
  st.endGroup();
  m_engine.aliasesChanged = [this] { SaveAliases(); };
}

void MainWindow::SaveAliases() {
  QSettings st;
  st.remove("alias");
  st.beginGroup("alias");
  for (const auto& [k, v] : m_engine.userAliases) st.setValue(QString::fromStdString(k), QString::fromStdString(v));
  st.endGroup();
}

// --- CommandHistory ----------------------------------------------------------------------
void MainWindow::ShowCommandHistory() {
  auto* dlg = new QDialog(this);
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  dlg->setWindowTitle("Cronologia dei comandi");
  dlg->resize(640, 420);
  auto* lay = new QVBoxLayout(dlg);
  auto* text = new QPlainTextEdit(dlg);
  text->setReadOnly(true);
  text->setFont(MonoFont(9));
  text->setPlainText(m_history->toPlainText());
  text->moveCursor(QTextCursor::End);
  lay->addWidget(text);
  auto* buttons = new QDialogButtonBox(dlg);
  auto* copy = buttons->addButton("Copia", QDialogButtonBox::ActionRole);
  auto* clear = buttons->addButton("Cancella", QDialogButtonBox::ActionRole);
  buttons->addButton(QDialogButtonBox::Close);
  connect(copy, &QPushButton::clicked, text, [text] { text->selectAll(); text->copy(); });
  connect(clear, &QPushButton::clicked, this, [this, text] { m_history->clear(); text->clear(); });
  connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::close);
  lay->addWidget(buttons);
  dlg->show();
}

// --- Opzioni ------------------------------------------------------------------------------
void MainWindow::ShowOptions(int page) {
  auto& S = Viewport::Shared();
  ONX_Model& model = m_engine.Model();
  const auto& ut = model.m_settings.m_ModelUnitsAndTolerances;

  QDialog dlg(this);
  dlg.setWindowTitle("Opzioni");
  dlg.resize(620, 460);
  auto* lay = new QVBoxLayout(&dlg);
  auto* tabs = new QTabWidget(&dlg);
  lay->addWidget(tabs);

  // Unita' e tolleranze (proprieta' del documento)
  auto* unitsPage = new QWidget;
  auto* uf = new QFormLayout(unitsPage);
  auto* units = new QComboBox;
  struct U { const char* label; const char* cmd; ON::LengthUnitSystem u; };
  const U unitList[] = {{"Micron", "Microns", ON::LengthUnitSystem::Microns},
                        {"Millimetri", "Millimeters", ON::LengthUnitSystem::Millimeters},
                        {"Centimetri", "Centimeters", ON::LengthUnitSystem::Centimeters},
                        {"Metri", "Meters", ON::LengthUnitSystem::Meters},
                        {"Chilometri", "Kilometers", ON::LengthUnitSystem::Kilometers},
                        {"Pollici", "Inches", ON::LengthUnitSystem::Inches},
                        {"Piedi", "Feet", ON::LengthUnitSystem::Feet}};
  for (const U& u : unitList) {
    units->addItem(u.label, u.cmd);
    if (u.u == ut.m_unit_system.UnitSystem()) units->setCurrentIndex(units->count() - 1);
  }
  auto* scaleObjs = new QCheckBox("Scala gli oggetti alle nuove unità");
  auto* absTol = new QDoubleSpinBox;
  absTol->setDecimals(6); absTol->setRange(1e-6, 100); absTol->setValue(ut.m_absolute_tolerance);
  auto* relTol = new QDoubleSpinBox;
  relTol->setDecimals(4); relTol->setRange(1e-4, 100); relTol->setSuffix(" %"); relTol->setValue(ut.m_relative_tolerance * 100.0);
  auto* angTol = new QDoubleSpinBox;
  angTol->setDecimals(3); angTol->setRange(0.001, 90); angTol->setSuffix(" °"); angTol->setValue(ut.m_angle_tolerance * 180.0 / ON_PI);
  uf->addRow("Unità del modello:", units);
  uf->addRow("", scaleObjs);
  uf->addRow("Tolleranza assoluta:", absTol);
  uf->addRow("Tolleranza relativa:", relTol);
  uf->addRow("Tolleranza angolare:", angTol);
  tabs->addTab(unitsPage, "Unità");

  // Griglia e modellazione
  auto* gridPage = new QWidget;
  auto* gf = new QFormLayout(gridPage);
  auto* spacing = new QDoubleSpinBox;
  spacing->setDecimals(4); spacing->setRange(1e-4, 1e6); spacing->setValue(S.gridSpacing);
  auto* gridSnap = new QCheckBox("Snap alla griglia");
  gridSnap->setChecked(S.gridSnap);
  auto* ortho = new QCheckBox("Orto");
  ortho->setChecked(S.ortho);
  gf->addRow("Passo dello snap:", spacing);
  gf->addRow("", gridSnap);
  gf->addRow("", ortho);
  tabs->addTab(gridPage, "Griglia");

  // Osnap, Proietta, SmartTrack
  auto* snapPage = new QWidget;
  auto* sl = new QVBoxLayout(snapPage);
  struct B { const char* label; unsigned bit; };
  const B bits[] = {{"Fine", Viewport::End}, {"Vicino", Viewport::Near}, {"Punto", Viewport::Point},
                    {"Medio", Viewport::Mid}, {"Centro", Viewport::Cen}, {"Intersezione", Viewport::Int},
                    {"Perpendicolare", Viewport::Perp}, {"Tangente", Viewport::Tan}, {"Quadrante", Viewport::Quad},
                    {"Nodo", Viewport::Knot}, {"Vertice", Viewport::Vertex}};
  std::vector<std::pair<QCheckBox*, unsigned>> snapBoxes;
  for (const B& b : bits) {
    auto* cb = new QCheckBox(b.label);
    cb->setChecked(S.osnapMask & b.bit);
    sl->addWidget(cb);
    snapBoxes.push_back({cb, b.bit});
  }
  auto* project = new QCheckBox("Proietta sul CPlane");
  project->setChecked(S.osnapProject);
  auto* smart = new QCheckBox("SmartTrack (linee guida dai punti osnap)");
  smart->setChecked(S.smartTrack);
  sl->addSpacing(8);
  sl->addWidget(project);
  sl->addWidget(smart);
  sl->addStretch();
  tabs->addTab(snapPage, "Osnap");

  // Alias
  auto* aliasPage = new QWidget;
  auto* al = new QVBoxLayout(aliasPage);
  auto* table = new QTableWidget(0, 2);
  table->setHorizontalHeaderLabels({"Alias", "Macro"});
  table->horizontalHeader()->setStretchLastSection(true);
  table->verticalHeader()->hide();
  for (const auto& [k, v] : m_engine.userAliases) {
    const int r = table->rowCount();
    table->insertRow(r);
    table->setItem(r, 0, new QTableWidgetItem(QString::fromStdString(k)));
    table->setItem(r, 1, new QTableWidgetItem(QString::fromStdString(v)));
  }
  auto* arow = new QHBoxLayout;
  auto* addA = new QPushButton("Nuovo");
  auto* delA = new QPushButton("Elimina");
  arow->addWidget(addA);
  arow->addWidget(delA);
  arow->addStretch();
  connect(addA, &QPushButton::clicked, table, [table] {
    const int r = table->rowCount();
    table->insertRow(r);
    table->setItem(r, 0, new QTableWidgetItem("nuovo"));
    table->setItem(r, 1, new QTableWidgetItem("_Line"));
    table->editItem(table->item(r, 0));
  });
  connect(delA, &QPushButton::clicked, table, [table] { if (table->currentRow() >= 0) table->removeRow(table->currentRow()); });
  al->addWidget(new QLabel("Un alias scritto nella riga di comando esegue la sua macro (es. ll → _Line 0,0,0)."));
  al->addWidget(table);
  al->addLayout(arow);
  tabs->addTab(aliasPage, "Alias");

  // Aspetto
  auto* lookPage = new QWidget;
  auto* lf = new QFormLayout(lookPage);
  auto* theme = new QComboBox;
  theme->addItems({"Automatico (come il sistema)", "Chiaro", "Scuro"});
  theme->setCurrentIndex((int)SavedThemeMode());
  auto* printDisp = new QCheckBox("PrintDisplay: colori e spessori di stampa nelle viste");
  printDisp->setChecked(S.printDisplay);
  lf->addRow("Tema:", theme);
  lf->addRow("", printDisp);
  tabs->addTab(lookPage, "Aspetto");

  tabs->setCurrentIndex(std::clamp(page, 0, tabs->count() - 1));
  auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  lay->addWidget(bb);
  if (dlg.exec() != QDialog::Accepted) return;

  // documento: unita' e tolleranze come comandi (annullabili)
  const QString unitCmd = units->currentData().toString();
  if (units->currentIndex() >= 0 && unitList[units->currentIndex()].u != ut.m_unit_system.UnitSystem())
    Submit("_Units " + unitCmd + (scaleObjs->isChecked() ? " Scale=Yes" : ""));
  const auto& ut2 = m_engine.Model().m_settings.m_ModelUnitsAndTolerances;
  if (std::fabs(absTol->value() - ut2.m_absolute_tolerance) > 1e-12 ||
      std::fabs(relTol->value() / 100.0 - ut2.m_relative_tolerance) > 1e-12 ||
      std::fabs(angTol->value() - ut2.m_angle_tolerance * 180.0 / ON_PI) > 1e-9)
    Submit(QString("_DocumentProperties AbsoluteTolerance=%1 RelativeTolerance=%2 AngleTolerance=%3")
               .arg(absTol->value(), 0, 'g', 10).arg(relTol->value() / 100.0, 0, 'g', 10).arg(angTol->value(), 0, 'g', 10));
  // impostazioni dell'applicazione
  S.gridSpacing = spacing->value();
  S.gridSnap = gridSnap->isChecked();
  S.ortho = ortho->isChecked();
  for (auto& [cb, bit] : snapBoxes) {
    if (cb->isChecked()) S.osnapMask |= bit; else S.osnapMask &= ~bit;
  }
  S.osnapProject = project->isChecked();
  S.smartTrack = smart->isChecked();
  S.printDisplay = printDisp->isChecked();
  m_engine.userAliases.clear();
  for (int r = 0; r < table->rowCount(); r++) {
    const auto* k = table->item(r, 0);
    const auto* v = table->item(r, 1);
    if (!k || !v || k->text().trimmed().isEmpty() || v->text().trimmed().isEmpty()) continue;
    if (m_engine.FindCommand(k->text().trimmed().toStdString())) {
      Log("Alias \"" + k->text() + "\" ignorato: è già il nome di un comando");
      continue;
    }
    m_engine.userAliases[NormalizeName(k->text().trimmed().toStdString())] = v->text().trimmed().toStdString();
  }
  SaveAliases();
  if (theme->currentIndex() != (int)SavedThemeMode()) SetTheme((ThemeMode)theme->currentIndex());
  SyncStatusToggles();
  for (auto* v : m_views) if (v) v->update();
}

// --- pannelli con nome -------------------------------------------------------------------
void MainWindow::BuildNamedPanels(QDockWidget* tabWith) {
  struct P { const char* title; const char* object; const char* cmd; int kind; bool rename; };
  const P panels[] = {{"Viste con nome", "namedViewsDock", "NamedView", 0, true},
                      {"CPlane con nome", "namedCPlanesDock", "NamedCPlane", 1, true},
                      {"Istantanee", "snapshotsDock", "Snapshots", 2, false},
                      {"Selezioni con nome", "namedSelDock", "NamedSelections", 3, false},
                      {"Stati dei livelli", "layerStatesDock", "LayerStateManager", 4, false}};
  for (const P& p : panels) {
    auto* dock = new QDockWidget(p.title, this);
    dock->setObjectName(p.object);
    auto* w = new QWidget(dock);
    auto* lay = new QVBoxLayout(w);
    auto* list = new QListWidget(w);
    lay->addWidget(list);
    auto* row = new QHBoxLayout;
    auto* save = new QPushButton("Salva");
    auto* restore = new QPushButton("Ripristina");
    auto* del = new QPushButton("Elimina");
    row->addWidget(save);
    row->addWidget(restore);
    if (p.rename) {
      auto* ren = new QPushButton("Rinomina");
      row->addWidget(ren);
      const QString cmd = p.cmd;
      connect(ren, &QPushButton::clicked, this, [this, list, cmd] {
        if (!list->currentItem()) return;
        bool ok = false;
        const QString n = QInputDialog::getText(this, "Rinomina", "Nuovo nome:", QLineEdit::Normal, list->currentItem()->text(), &ok);
        if (ok && !n.trimmed().isEmpty()) PropertyCommand("_" + cmd + " Rename " + Quote(list->currentItem()->text()) + " " + Quote(n.trimmed()));
      });
    }
    row->addWidget(del);
    lay->addLayout(row);
    dock->setWidget(w);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    if (tabWith) tabifyDockWidget(tabWith, dock);
    dock->hide();
    const QString cmd = p.cmd;
    const QString title = p.title;
    connect(save, &QPushButton::clicked, this, [this, cmd, title] {
      bool ok = false;
      const QString n = QInputDialog::getText(this, title, "Nome:", QLineEdit::Normal, QString(), &ok);
      if (ok && !n.trimmed().isEmpty()) PropertyCommand("_" + cmd + " Save " + Quote(n.trimmed()));
    });
    auto doRestore = [this, list, cmd] {
      if (list->currentItem()) PropertyCommand("_" + cmd + " Restore " + Quote(list->currentItem()->text()));
    };
    connect(restore, &QPushButton::clicked, this, doRestore);
    connect(list, &QListWidget::itemDoubleClicked, this, doRestore);
    connect(del, &QPushButton::clicked, this, [this, list, cmd] {
      if (list->currentItem()) PropertyCommand("_" + cmd + " Delete " + Quote(list->currentItem()->text()));
    });
    m_namedPanels.push_back({dock, list, p.kind});
  }
}

void MainWindow::RefreshNamedPanels() {
  for (auto& np : m_namedPanels) {
    if (!np.dock->isVisible()) continue;
    const QString cur = np.list->currentItem() ? np.list->currentItem()->text() : QString();
    np.list->clear();
    for (const auto& n : NamedItems(m_engine, np.kind)) np.list->addItem(QString::fromStdString(n));
    const auto found = np.list->findItems(cur, Qt::MatchExactly);
    if (!found.isEmpty()) np.list->setCurrentItem(found.first());
  }
}

bool MainWindow::ShowNamedPanel(const std::string& norm) {
  static const std::pair<const char*, int> names[] = {
      {"namedviews", 0}, {"namedview", 0}, {"named views", 0}, {"namedcplanes", 1}, {"namedcplane", 1},
      {"snapshots", 2}, {"namedselections", 3}, {"layerstatemanager", 4}};
  for (const auto& [n, kind] : names)
    if (norm == n)
      for (auto& np : m_namedPanels)
        if (np.kind == kind) {
          np.dock->show();
          np.dock->raise();
          RefreshNamedPanels();
          return true;
        }
  return false;
}

}  // namespace cad::ui
