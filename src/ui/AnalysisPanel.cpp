// Analisi visive delle superfici e delle curve, come le finestre di analisi di Rhino 8:
// Zebra, DraftAngleAnalysis, CurvatureAnalysis, EMap, CurvatureGraph, ShowEdges.
// Ogni analisi si accende sugli oggetti selezionati e si spegne con <Nome>Off; le opzioni
// si danno sulla riga di comando (Nome=Valore) o nella finestra "Opzioni analisi".
#include "ui/MainWindow.h"
#include "kernel/SurfaceAnalysis.h"

#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QPainter>
#include <QLinearGradient>
#include <QSignalBlocker>
#include <algorithm>
#include <map>

namespace cad::ui {
namespace {

// Opzioni "Nome=Valore" dopo il nome del comando (chiavi in minuscolo, valori come scritti).
std::map<std::string, std::string> ParseOptions(const std::string& text, std::string& command) {
  std::map<std::string, std::string> out;
  std::vector<std::string> tok;
  std::string cur;
  bool quoted = false;
  for (char c : text) {
    if (c == '"') { quoted = !quoted; continue; }
    if (!quoted && (c == ' ' || c == '\t')) { if (!cur.empty()) tok.push_back(cur); cur.clear(); }
    else cur += c;
  }
  if (!cur.empty()) tok.push_back(cur);
  command = tok.empty() ? std::string() : cad::NormalizeName(tok[0]);
  for (size_t i = 1; i < tok.size(); i++) {
    const auto eq = tok[i].find('=');
    if (eq == std::string::npos) out[cad::NormalizeName(tok[i])] = "yes";
    else out[cad::NormalizeName(tok[i].substr(0, eq))] = tok[i].substr(eq + 1);
  }
  return out;
}

double Num(const std::map<std::string, std::string>& o, const char* k, double def) {
  auto it = o.find(k);
  if (it == o.end()) return def;
  try { return std::stod(it->second); } catch (...) { return def; }
}

bool InList(const std::vector<ON_UUID>& v, const ON_UUID& id) {
  for (const auto& x : v) if (ON_UuidCompare(x, id) == 0) return true;
  return false;
}

}  // namespace

bool MainWindow::HandleAnalysisCommand(const std::string& text) {
  if (!m_pending.empty()) return false;
  std::string cmd;
  const auto opts = ParseOptions(text, cmd);
  auto& S = Viewport::Shared();
  auto& A = S.analysis;
  struct Def { const char* on; const char* on2; const char* off; std::vector<ON_UUID>* list; const char* what; const char* name; int page; };
  const Def defs[] = {{"zebra", "zebra", "zebraoff", &S.zebra, "Zebra", "Zebra", 0},
                      {"draftangleanalysis", "draft", "draftangleanalysisoff", &S.draft, "Angolo di sformo", "DraftAngleAnalysis", 1},
                      {"curvatureanalysis", "curvatureanalysis", "curvatureanalysisoff", &S.curvatureAnalysis, "Analisi di curvatura", "CurvatureAnalysis", 2},
                      {"emap", "emap", "emapoff", &S.emap, "Mappa d'ambiente", "EMap", 3},
                      {"curvaturegraph", "curvaturegraphon", "curvaturegraphoff", &S.curvature, "Grafico di curvatura", "CurvatureGraph", -1},
                      {"showedges", "showedges", "showedgesoff", &S.edges, "Spigoli nudi", "ShowEdges", -1}};
  if (cmd == "analysisoptions") { ShowAnalysisOptions(0); return true; }
  for (const auto& d : defs) {
    if (cmd == d.off || (cmd == "draftoff" && d.list == &S.draft)) {
      d.list->clear();
      Log(QString("%1: spento").arg(d.what));
      for (auto* v : m_views) v->update();
      return true;
    }
    if (cmd != d.on && cmd != d.on2) continue;
    // opzioni della riga di comando
    if (d.page == 0) {
      if (opts.count("direction")) A.zebraHorizontal = cad::NormalizeName(opts.at("direction")).rfind("h", 0) == 0;
      if (opts.count("size")) {
        const std::string sz = cad::NormalizeName(opts.at("size"));
        A.zebraSize = sz == "thin" || sz == "sottili" ? 0 : sz == "thick" || sz == "spesse" ? 2 : 1;
      }
    } else if (d.page == 1) {
      A.draftMin = Num(opts, "min", A.draftMin);
      A.draftMax = Num(opts, "max", A.draftMax);
    } else if (d.page == 3) {
      if (opts.count("file")) { A.emapFile = opts.at("file"); A.emapRevision++; }
    }
    std::vector<ON_UUID> ids;
    for (const auto& obj : m_scene.Objects())
      if (obj.selected) ids.push_back(obj.id);
    if (ids.empty()) { Log(QString("%1: seleziona prima gli oggetti").arg(d.what)); return true; }
    for (const auto& id : ids) if (!InList(*d.list, id)) d.list->push_back(id);
    if (d.page == 2) {
      if (opts.count("style")) A.curvStyle = int(cad::ParseCurvatureStyle(opts.at("style")));
      const bool setMin = opts.count("min"), setMax = opts.count("max");
      if (setMin) A.curvMin = Num(opts, "min", A.curvMin);
      if (setMax) A.curvMax = Num(opts, "max", A.curvMax);
      if (setMin || setMax) A.curvRangeSet = true;
      if (opts.count("autorange") || opts.count("maxrange") || opts.count("style") || !A.curvRangeSet) {
        std::vector<std::shared_ptr<cad::CurvatureData>> keep;
        std::vector<const cad::CurvatureData*> data;
        for (const auto& obj : m_scene.Objects())
          if (InList(S.curvatureAnalysis, obj.id))
            if (auto cd = Viewport::CurvatureOf(m_scene, obj)) { keep.push_back(cd); data.push_back(cd.get()); }
        double lo, hi;
        if (cad::CurvatureRange(data, static_cast<cad::CurvatureStyle>(A.curvStyle), !opts.count("maxrange"), lo, hi)) {
          A.curvMin = lo;
          A.curvMax = hi;
          A.curvRangeSet = true;
        }
      }
      Log(QString("Analisi di curvatura (%1): da %2 (rosso) a %3 (blu)")
              .arg(cad::CurvatureStyleLabel(static_cast<cad::CurvatureStyle>(A.curvStyle)))
              .arg(A.curvStyle < 2 ? A.curvMax : A.curvMin, 0, 'g', 6)
              .arg(A.curvStyle < 2 ? A.curvMin : A.curvMax, 0, 'g', 6));
    }
    Log(QString("%1: attivo su %2 oggetti (%3Off per spegnerlo)").arg(d.what).arg(d.list->size()).arg(d.name));
    if (d.page >= 0) ShowAnalysisOptions(d.page);
    if (m_analysisRefresh) m_analysisRefresh();
    for (auto* v : m_views) v->update();
    return true;
  }
  return false;
}

// Finestra non modale con una scheda per analisi; i cambiamenti si vedono subito nelle viste.
void MainWindow::ShowAnalysisOptions(int page) {
  auto& S = Viewport::Shared();
  auto& A = S.analysis;
  if (!m_analysisDlg) {
    auto* dlg = new QDialog(this);
    dlg->setWindowTitle("Opzioni analisi");
    dlg->setModal(false);
    auto* tabs = new QTabWidget(dlg);
    tabs->setObjectName("tabs");
    auto redraw = [this] { for (auto* v : m_views) v->update(); };

    // Zebra
    auto* zw = new QWidget;
    auto* zf = new QFormLayout(zw);
    auto* zDir = new QComboBox;
    zDir->addItems({"Verticali", "Orizzontali"});
    auto* zSize = new QComboBox;
    zSize->addItems({"Sottili", "Medie", "Spesse"});
    zf->addRow("Direzione delle strisce", zDir);
    zf->addRow("Larghezza delle strisce", zSize);
    auto* zOff = new QPushButton("Spegni Zebra");
    zf->addRow(zOff);
    connect(zDir, &QComboBox::currentIndexChanged, this, [&A, redraw](int i) { A.zebraHorizontal = i == 1; redraw(); });
    connect(zSize, &QComboBox::currentIndexChanged, this, [&A, redraw](int i) { A.zebraSize = i; redraw(); });
    connect(zOff, &QPushButton::clicked, this, [this] { HandleAnalysisCommand("ZebraOff"); });
    tabs->addTab(zw, "Zebra");

    // Angolo di sformo
    auto* dw = new QWidget;
    auto* df = new QFormLayout(dw);
    auto* dMin = new QDoubleSpinBox;
    auto* dMax = new QDoubleSpinBox;
    for (auto* sb : {dMin, dMax}) { sb->setRange(-90, 90); sb->setDecimals(2); sb->setSuffix(" °"); }
    df->addRow("Angolo minimo (rosso)", dMin);
    df->addRow("Angolo massimo (blu)", dMax);
    df->addRow(new QLabel("Angolo rispetto al piano XY del mondo (direzione di estrazione: Z)."));
    auto* dOff = new QPushButton("Spegni sformo");
    df->addRow(dOff);
    connect(dMin, &QDoubleSpinBox::valueChanged, this, [&A, redraw](double v) { A.draftMin = v; redraw(); });
    connect(dMax, &QDoubleSpinBox::valueChanged, this, [&A, redraw](double v) { A.draftMax = v; redraw(); });
    connect(dOff, &QPushButton::clicked, this, [this] { HandleAnalysisCommand("DraftAngleAnalysisOff"); });
    tabs->addTab(dw, "Sformo");

    // Curvatura
    auto* cw = new QWidget;
    auto* cf = new QFormLayout(cw);
    auto* cStyle = new QComboBox;
    for (int i = 0; i < 4; i++) cStyle->addItem(cad::CurvatureStyleLabel(static_cast<cad::CurvatureStyle>(i)));
    auto* cMin = new QLineEdit;
    auto* cMax = new QLineEdit;
    auto* cLegend = new QLabel;
    cLegend->setFixedHeight(18);
    {
      QLinearGradient g(0, 0, 1, 0);
      g.setCoordinateMode(QGradient::ObjectBoundingMode);
      g.setColorAt(0, Qt::red); g.setColorAt(0.5, Qt::green); g.setColorAt(1, Qt::blue);
      QPixmap pm(240, 18);
      QPainter p(&pm);
      p.fillRect(pm.rect(), g);
      p.end();
      cLegend->setPixmap(pm);
    }
    auto* cLegendText = new QLabel;
    auto* cAuto = new QPushButton("Intervallo automatico");
    auto* cMaxR = new QPushButton("Intervallo massimo");
    auto* cOff = new QPushButton("Spegni curvatura");
    auto* cBtns = new QHBoxLayout;
    cBtns->addWidget(cAuto);
    cBtns->addWidget(cMaxR);
    cf->addRow("Stile", cStyle);
    cf->addRow("Valore minimo", cMin);
    cf->addRow("Valore massimo", cMax);
    cf->addRow(cLegend);
    cf->addRow(cLegendText);
    cf->addRow(cBtns);
    cf->addRow(cOff);
    auto applyRange = [this, &A, cMin, cMax, redraw] {
      bool ok1, ok2;
      const double lo = cMin->text().toDouble(&ok1), hi = cMax->text().toDouble(&ok2);
      if (ok1 && ok2) { A.curvMin = lo; A.curvMax = hi; A.curvRangeSet = true; }
      if (m_analysisRefresh) m_analysisRefresh();
      redraw();
    };
    connect(cMin, &QLineEdit::editingFinished, this, applyRange);
    connect(cMax, &QLineEdit::editingFinished, this, applyRange);
    connect(cStyle, &QComboBox::activated, this, [this](int i) {
      HandleAnalysisCommand(std::string("CurvatureAnalysis Style=") + cad::CurvatureStyleName(static_cast<cad::CurvatureStyle>(i)));
    });
    connect(cAuto, &QPushButton::clicked, this, [this] { HandleAnalysisCommand("CurvatureAnalysis AutoRange"); });
    connect(cMaxR, &QPushButton::clicked, this, [this] { HandleAnalysisCommand("CurvatureAnalysis MaxRange"); });
    connect(cOff, &QPushButton::clicked, this, [this] { HandleAnalysisCommand("CurvatureAnalysisOff"); });
    tabs->addTab(cw, "Curvatura");

    // EMap
    auto* ew = new QWidget;
    auto* ef = new QFormLayout(ew);
    auto* eFile = new QLabel;
    auto* eBrowse = new QPushButton("Scegli immagine...");
    auto* eDefault = new QPushButton("Ambiente da studio");
    auto* eOff = new QPushButton("Spegni EMap");
    ef->addRow("Immagine", eFile);
    ef->addRow(eBrowse);
    ef->addRow(eDefault);
    ef->addRow(eOff);
    connect(eBrowse, &QPushButton::clicked, this, [this, &A, redraw] {
      const QString f = QFileDialog::getOpenFileName(this, "Immagine per EMap", QString(),
                                                     "Immagini (*.png *.jpg *.jpeg *.bmp *.tif *.tiff);;Tutti i file (*)");
      if (f.isEmpty()) return;
      A.emapFile = f.toStdString();
      A.emapRevision++;
      if (m_analysisRefresh) m_analysisRefresh();
      redraw();
    });
    connect(eDefault, &QPushButton::clicked, this, [this, &A, redraw] {
      A.emapFile.clear();
      A.emapRevision++;
      if (m_analysisRefresh) m_analysisRefresh();
      redraw();
    });
    connect(eOff, &QPushButton::clicked, this, [this] { HandleAnalysisCommand("EMapOff"); });
    tabs->addTab(ew, "EMap");

    auto* close = new QPushButton("Chiudi");
    connect(close, &QPushButton::clicked, dlg, &QDialog::hide);
    auto* lay = new QVBoxLayout(dlg);
    lay->addWidget(tabs);
    lay->addWidget(close, 0, Qt::AlignRight);

    m_analysisRefresh = [&A, zDir, zSize, dMin, dMax, cStyle, cMin, cMax, cLegendText, eFile] {
      const QSignalBlocker b1(zDir), b2(zSize), b3(dMin), b4(dMax), b5(cStyle);
      zDir->setCurrentIndex(A.zebraHorizontal ? 1 : 0);
      zSize->setCurrentIndex(A.zebraSize);
      dMin->setValue(A.draftMin);
      dMax->setValue(A.draftMax);
      cStyle->setCurrentIndex(A.curvStyle);
      cMin->setText(QString::number(A.curvMin, 'g', 6));
      cMax->setText(QString::number(A.curvMax, 'g', 6));
      const bool radius = A.curvStyle >= 2;
      cLegendText->setText(radius ? QString("rosso: raggio <= %1    blu: raggio >= %2").arg(A.curvMin, 0, 'g', 6).arg(A.curvMax, 0, 'g', 6)
                                  : QString("rosso: >= %1    blu: <= %2").arg(A.curvMax, 0, 'g', 6).arg(A.curvMin, 0, 'g', 6));
      eFile->setText(A.emapFile.empty() ? QString("ambiente da studio (predefinito)") : QString::fromStdString(A.emapFile));
    };
    m_analysisDlg = dlg;
  }
  m_analysisRefresh();
  if (auto* tabs = m_analysisDlg->findChild<QTabWidget*>("tabs")) tabs->setCurrentIndex(std::clamp(page, 0, 3));
  m_analysisDlg->show();
  m_analysisDlg->raise();
}

}  // namespace cad::ui
