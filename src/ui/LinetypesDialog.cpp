// Finestra dei tipi di linea (come la pagina Tipi di linea delle proprieta' del documento di
// Rhino 8): elenco con anteprima del motivo, nuovo, modifica del motivo, rinomina, elimina e
// assegnazione agli oggetti selezionati. Le modifiche passano dai comandi (annullabili).
#include "ui/MainWindow.h"

#include <QDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace cad::ui {
namespace {

QString PatternText(const ON_Linetype& lt) {
  QStringList parts;
  for (int i = 0; i < lt.SegmentCount(); i++) {
    const ON_LinetypeSegment s = lt.Segment(i);
    const double v = s.m_seg_type == ON_LinetypeSegment::eSegType::stSpace ? -s.m_length : s.m_length;
    parts << ((s.m_seg_type == ON_LinetypeSegment::eSegType::stSpace && s.m_length == 0) ? QString("-0") : QString::number(v, 'g', 6));
  }
  return parts.join(",");
}

QPixmap PatternPreview(const ON_Linetype* lt, int w = 160, int h = 14) {
  QPixmap pm(w, h);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(QPen(QColor(30, 30, 30), 2));
  const double y = h / 2.0;
  if (!lt || lt->SegmentCount() < 2) { p.drawLine(QPointF(2, y), QPointF(w - 2, y)); return pm; }
  double total = 0;
  for (int i = 0; i < lt->SegmentCount(); i++) total += lt->Segment(i).m_length;
  const double scale = total > 0 ? std::max(1.0, 40.0 / total) : 1.0;  // almeno un motivo ogni 40 pixel circa
  double x = 2;
  for (int rep = 0; x < w - 2 && rep < 1000; rep++)
    for (int i = 0; i < lt->SegmentCount() && x < w - 2; i++) {
      const ON_LinetypeSegment s = lt->Segment(i);
      const double len = s.m_length * scale;
      if (s.m_seg_type == ON_LinetypeSegment::eSegType::stLine) {
        if (len < 1) p.drawPoint(QPointF(x, y));
        else p.drawLine(QPointF(x, y), QPointF(std::min(x + len, w - 2.0), y));
      }
      x += std::max(len, 1.0);
    }
  return pm;
}

}  // namespace

void MainWindow::ShowLinetypes() {
  auto* dlg = new QDialog(this);
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  dlg->setWindowTitle("Tipi di linea");
  dlg->resize(560, 380);
  auto* table = new QTableWidget(0, 3, dlg);
  table->setHorizontalHeaderLabels({"Nome", "Motivo (mm)", "Anteprima"});
  table->horizontalHeader()->setStretchLastSection(true);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSelectionMode(QAbstractItemView::SingleSelection);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->verticalHeader()->hide();
  auto refresh = [this, table] {
    table->setRowCount(0);
    auto addRow = [&](const QString& name, const QString& pat, const ON_Linetype* lt) {
      const int r = table->rowCount();
      table->insertRow(r);
      table->setItem(r, 0, new QTableWidgetItem(name));
      table->setItem(r, 1, new QTableWidgetItem(pat));
      auto* lab = new QLabel;
      lab->setPixmap(PatternPreview(lt));
      table->setCellWidget(r, 2, lab);
    };
    addRow("Continuous", "continua", nullptr);
    ONX_ModelComponentIterator it(m_engine.Model(), ON_ModelComponent::Type::LinePattern);
    for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent())
      if (const auto* lt = ON_Linetype::Cast(c); lt && !lt->IsDeleted())
        addRow(QString::fromWCharArray(static_cast<const wchar_t*>(lt->Name())), PatternText(*lt), lt);
    table->resizeColumnsToContents();
  };
  refresh();
  auto current = [table]() -> QString {
    const int r = table->currentRow();
    return r >= 0 && table->item(r, 0) ? table->item(r, 0)->text() : QString();
  };
  auto quoted = [](const QString& s) { return "\"" + s + "\""; };
  auto run = [this, refresh](const QString& cmd) {
    PropertyCommand(cmd);
    refresh();
  };
  auto* bNew = new QPushButton("Nuovo...");
  auto* bEdit = new QPushButton("Modifica motivo...");
  auto* bRen = new QPushButton("Rinomina...");
  auto* bDel = new QPushButton("Elimina");
  auto* bSet = new QPushButton("Assegna agli oggetti selezionati");
  auto* bLayer = new QPushButton("Oggetti: da livello");
  connect(bNew, &QPushButton::clicked, dlg, [=] {
    bool ok = false;
    const QString n = QInputDialog::getText(dlg, "Nuovo tipo di linea", "Nome:", QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || n.isEmpty()) return;
    const QString p = QInputDialog::getText(dlg, "Nuovo tipo di linea",
                                            "Motivo in mm (positivo = tratto, negativo = spazio, 0 = punto), es. 5,-2:",
                                            QLineEdit::Normal, "5,-2", &ok).trimmed();
    if (!ok || p.isEmpty()) return;
    run("-_Linetypes New " + quoted(n) + " " + quoted(p));
  });
  connect(bEdit, &QPushButton::clicked, dlg, [=] {
    const QString n = current();
    if (n.isEmpty() || n == "Continuous") return;
    bool ok = false;
    const QString p = QInputDialog::getText(dlg, "Motivo di " + n, "Motivo in mm (es. 5,-2,0,-2):", QLineEdit::Normal,
                                            table->item(table->currentRow(), 1)->text(), &ok).trimmed();
    if (!ok || p.isEmpty()) return;
    run("-_Linetypes Edit " + quoted(n) + " " + quoted(p));
  });
  connect(bRen, &QPushButton::clicked, dlg, [=] {
    const QString n = current();
    if (n.isEmpty() || n == "Continuous") return;
    bool ok = false;
    const QString nn = QInputDialog::getText(dlg, "Rinomina", "Nuovo nome:", QLineEdit::Normal, n, &ok).trimmed();
    if (!ok || nn.isEmpty() || nn == n) return;
    run("-_Linetypes Rename " + quoted(n) + " " + quoted(nn));
  });
  connect(bDel, &QPushButton::clicked, dlg, [=] {
    const QString n = current();
    if (n.isEmpty() || n == "Continuous") return;
    run("-_Linetypes Delete " + quoted(n));
  });
  connect(bSet, &QPushButton::clicked, dlg, [=] {
    const QString n = current();
    if (n.isEmpty()) return;
    run("-_SetObjectLinetype " + quoted(n));
  });
  connect(bLayer, &QPushButton::clicked, dlg, [=] { run("-_SetObjectLinetype ByLayer"); });
  auto* buttons = new QVBoxLayout;
  for (auto* b : {bNew, bEdit, bRen, bDel}) buttons->addWidget(b);
  buttons->addSpacing(12);
  buttons->addWidget(bSet);
  buttons->addWidget(bLayer);
  buttons->addStretch(1);
  auto* close = new QPushButton("Chiudi");
  connect(close, &QPushButton::clicked, dlg, &QDialog::close);
  buttons->addWidget(close);
  auto* lay = new QHBoxLayout(dlg);
  lay->addWidget(table, 1);
  lay->addLayout(buttons);
  dlg->show();
}

}  // namespace cad::ui
