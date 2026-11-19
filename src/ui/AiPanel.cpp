#include "ui/AiPanel.h"
#include <QTimer>
#include <QTextBlock>
#include <QTextCursor>
#include <algorithm>
#include "ui/Theme.h"
#include "commands/Engine.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QVBoxLayout>
#include <set>

namespace cad::ui {

AiPanel::AiPanel(cad::Engine& engine, std::function<void()> afterChange, QWidget* parent)
    : QWidget(parent), m_engine(engine), m_after(std::move(afterChange)), m_gen(engine) {
  auto* lay = new QVBoxLayout(this);
  lay->setContentsMargins(8, 8, 8, 8);
  auto* top = new QHBoxLayout;
  m_model = new QLabel;
  m_model->setWordWrap(true);
  auto* choose = new QPushButton("Modello…");
  top->addWidget(m_model, 1);
  top->addWidget(choose);
  lay->addLayout(top);
  lay->addWidget(new QLabel("Descrivi l'oggetto da creare:"));
  m_request = new QPlainTextEdit;
  m_request->setPlaceholderText("Per esempio: un tavolo rotondo di 90 cm di diametro con quattro gambe");
  m_request->setMaximumHeight(90);
  lay->addWidget(m_request);
  m_generate = new QPushButton("Genera");
  m_generate->setDefault(true);
  lay->addWidget(m_generate);
  m_busy = new QProgressBar;
  m_busy->setRange(0, 0);
  m_busy->setVisible(false);
  m_busy->setMaximumHeight(6);
  m_busy->setTextVisible(false);
  lay->addWidget(m_busy);
  m_status = new QLabel;
  m_status->setWordWrap(true);
  lay->addWidget(m_status);
  lay->addWidget(new QLabel("Comandi (modificabili):"));
  m_script = new QPlainTextEdit;
  m_script->setFont(MonoFont(9));
  lay->addWidget(m_script, 1);
  auto* row = new QHBoxLayout;
  m_apply = new QPushButton("Anteprima");
  m_confirm = new QPushButton("Conferma");
  m_discard = new QPushButton("Scarta");
  row->addWidget(m_apply);
  row->addWidget(m_confirm);
  row->addWidget(m_discard);
  lay->addLayout(row);
  m_confirm->setEnabled(false);
  m_discard->setEnabled(false);

  connect(choose, &QPushButton::clicked, this, &AiPanel::ShowModelDialog);
  connect(m_generate, &QPushButton::clicked, this, &AiPanel::Generate);
  connect(m_apply, &QPushButton::clicked, this, [this] {
    QStringList s;
    for (const QString& l : m_script->toPlainText().split('\n')) if (!l.trimmed().isEmpty()) s << l.trimmed();
    std::vector<std::string> lines;
    for (const auto& l : s) lines.push_back(l.toStdString());
    if (m_previewing) Discard();
    PreviewValidPart(s);
  });
  connect(m_confirm, &QPushButton::clicked, this, &AiPanel::Confirm);
  connect(m_discard, &QPushButton::clicked, this, &AiPanel::Discard);
  connect(&m_gen, &aiqt::Generator::Status, this, [this](const QString& s) { m_status->setText(s); });
  connect(&m_server, &aiqt::LlamaServer::Status, this, [this](const QString& s) { m_status->setText(s); });
  connect(&m_gen, &aiqt::Generator::Finished, this, [this](bool ok, const QStringList& script, const QString& err) {
    SetBusy(false, {});
    QTimer::singleShot(0, this, [this] { StartNextTask(); });  // compiti in fila dopo questa richiesta
    m_script->setPlainText(script.join('\n'));
    if (!ok) {
      if (script.isEmpty()) {
        m_status->setText("Il modello non ha prodotto comandi (" + err + "). Riformula la richiesta.");
        if (m_task) FinishTask("nessun comando (" + err + ")");
        return;
      }
      // comandi in parte sbagliati: anteprima della parte che funziona, errore indicato nel testo
      PreviewValidPart(script);
      if (m_task) {
        if (m_previewing) Confirm();
        FinishTask("fatto in parte, errore alla " + err);
      }
      return;
    }
    ApplyPreview(script);
    if (m_task) {
      const bool changed = m_previewing;
      Confirm();
      FinishTask(changed ? QString("fatto (%1 comandi)").arg(script.size()) : QString("nessuna modifica al disegno"));
    }
  });
  UpdateModelLabel();
}

AiPanel::~AiPanel() { m_server.Stop(); }

QString AiPanel::CurrentModelId() const {
  QSettings st;
  QString id = st.value("ai/model").toString();
  if (id.isEmpty() || !ai::FindModel(id.toStdString())) {
    // nessuna scelta: il primo modello gia' scaricato, altrimenti quello consigliato
    for (const auto& m : ai::Catalog())
      if (m.listed && !aiqt::InstalledModelPath(m).isEmpty()) return QString::fromStdString(m.id);
    return QString::fromStdString(ai::Catalog()[ai::RecommendModel(ai::DetectHardware())].id);
  }
  return id;
}

void AiPanel::UpdateModelLabel() {
  const ai::ModelInfo* m = ai::FindModel(CurrentModelId().toStdString());
  const bool have = m && !aiqt::InstalledModelPath(*m).isEmpty();
  m_model->setText(m ? QString("<b>%1</b><br>%2").arg(QString::fromStdString(m->label),
                                                     have ? "pronto, funziona senza internet" : "da scaricare")
                     : QString("Nessun modello"));
}

void AiPanel::SetBusy(bool busy, const QString& status) {
  m_busy->setVisible(busy);
  m_generate->setText(busy ? "Annulla" : "Genera");
  m_apply->setEnabled(!busy);
  if (!status.isEmpty()) m_status->setText(status);
}

void AiPanel::EnsureServer(std::function<void()> then, std::function<void(QString)> fail) {
  const ai::ModelInfo* m = ai::FindModel(CurrentModelId().toStdString());
  const QString path = m ? aiqt::InstalledModelPath(*m) : QString();
  if (path.isEmpty()) {
    SetBusy(false, "Scarica prima il modello.");
    if (fail) fail("su questo computer il modello dell'AI non e' ancora scaricato");
    else ShowModelDialog();
    return;
  }
  if (m_server.Ready() && m_server.ModelPath() == path) { then(); return; }
  auto* conn = new QMetaObject::Connection;
  *conn = connect(&m_server, &aiqt::LlamaServer::Started, this, [this, conn, then, fail](bool ok, const QString& err) {
    disconnect(*conn);
    delete conn;
    if (!ok) {
      SetBusy(false, "Il modello non si avvia: " + err);
      if (fail) fail("il modello dell'AI non si avvia: " + err);
      return;
    }
    then();
  });
  m_server.Start(path, ai::DetectHardware().threads);
}

void AiPanel::Generate() {
  if (m_busy->isVisible()) {  // "Annulla"
    m_gen.Cancel();
    SetBusy(false, "Annullato");
    if (m_task) FinishTask("annullato da chi lo stava eseguendo");
    return;
  }
  if (m_task) return;  // un compito di un partecipante e' in corso
  const QString req = m_request->toPlainText().trimmed();
  if (req.isEmpty()) { m_status->setText("Scrivi prima che cosa vuoi creare."); return; }
  if (m_previewing) Discard();
  SetBusy(true, "Preparazione…");
  EnsureServer([this, req] { m_gen.Run(m_server.Port(), req, 3, true, QString::fromStdString(ai::SystemPromptFor(ai::FindModel(CurrentModelId().toStdString())))); });
}

void AiPanel::ApplyPreview(const QStringList& script) {
  std::vector<std::string> lines;
  for (const auto& l : script) lines.push_back(l.toStdString());
  m_undoDepth = m_engine.UndoDepth();
  const auto beforeIds = m_engine.AllObjectIds(true);
  const std::set<ON_UUID, UuidLess> before(beforeIds.begin(), beforeIds.end());
  for (const auto& l : ai::ExpandScript(lines))
    if (!m_engine.Run(l)) break;
  m_previewing = m_engine.UndoDepth() > m_undoDepth;
  // gli oggetti nuovi restano selezionati: si vedono evidenziati come anteprima
  m_engine.ClearSelection();
  for (const auto& id : m_engine.AllObjectIds(true))
    if (!before.count(id)) m_engine.Select(id);
  m_confirm->setEnabled(m_previewing);
  m_discard->setEnabled(m_previewing);
  m_status->setText(m_previewing ? "Anteprima nel disegno: premi Conferma per tenerla o Scarta per toglierla."
                                 : "Nessuna modifica al disegno.");
  if (m_after) m_after();
}

void AiPanel::PreviewValidPart(const QStringList& script) {
  std::vector<std::string> lines;
  for (const auto& l : script) lines.push_back(l.toStdString());
  const auto v = ai::Validate(m_engine, lines);
  if (v.ok) {
    ApplyPreview(script);
    return;
  }
  const int bad = std::clamp(v.line, 0, (int)script.size());
  if (bad > 0) ApplyPreview(script.mid(0, bad));
  const QString msg = QString("Riga %1: %2.").arg(bad + 1).arg(QString::fromStdString(v.error));
  m_status->setText(bad > 0 ? msg + QString(" Nel disegno c'e' l'anteprima delle righe 1-%1: puoi confermarla, "
                                           "oppure correggere la riga evidenziata e premere di nuovo Anteprima.").arg(bad)
                            : msg + " Correggi la riga evidenziata e premi di nuovo Anteprima, o riformula la richiesta.");
  // riga sbagliata evidenziata nel testo dei comandi
  QTextCursor c(m_script->document()->findBlockByNumber(bad));
  c.select(QTextCursor::LineUnderCursor);
  m_script->setTextCursor(c);
  m_script->setFocus();
}

void AiPanel::Confirm() {
  if (!m_previewing) return;
  m_engine.SquashUndo(m_undoDepth);  // un solo passo di Annulla per tutto lo script
  m_previewing = false;
  m_confirm->setEnabled(false);
  m_discard->setEnabled(false);
  m_status->setText("Confermato (Annulla toglie tutto in un passo).");
  if (m_after) m_after();
  if (!m_task) QTimer::singleShot(0, this, [this] { StartNextTask(); });
}

void AiPanel::Discard() {
  if (!m_previewing) return;
  m_engine.RestoreUndo(m_undoDepth);
  m_previewing = false;
  m_confirm->setEnabled(false);
  m_discard->setEnabled(false);
  m_status->setText("Anteprima scartata.");
  if (m_after) m_after();
  if (!m_task) QTimer::singleShot(0, this, [this] { StartNextTask(); });
}

void AiPanel::RunTask(const QString& from, const QString& text, std::function<void(QString)> done) {
  m_tasks.push_back(Task{from, text, std::move(done)});
  StartNextTask();
}

void AiPanel::StartNextTask() {
  // si aspetta che finisca la richiesta dell'utente e che la sua anteprima sia confermata o scartata
  if (m_task || m_tasks.empty() || m_busy->isVisible() || m_previewing) return;
  m_task = std::move(m_tasks.front());
  m_tasks.pop_front();
  m_request->setPlainText(m_task->text);
  SetBusy(true, "Compito da " + m_task->from + "…");
  const QString req = m_task->text;
  EnsureServer([this, req] { m_gen.Run(m_server.Port(), req, 3, true, QString::fromStdString(ai::SystemPromptFor(ai::FindModel(CurrentModelId().toStdString())))); }, [this](const QString& err) { FinishTask(err); });
}

void AiPanel::FinishTask(const QString& result) {
  if (!m_task) return;
  auto done = std::move(m_task->done);
  m_status->setText("Compito da " + m_task->from + ": " + result);
  m_task.reset();
  if (done) done(result);
  QTimer::singleShot(0, this, [this] { StartNextTask(); });
}

void AiPanel::ShowModelDialog() {
  QDialog dlg(this);
  dlg.setWindowTitle("Modello dell'assistente AI");
  dlg.resize(720, 420);
  auto* lay = new QVBoxLayout(&dlg);
  const ai::HardwareInfo hw = ai::DetectHardware();
  const int rec = ai::RecommendModel(hw);
  auto* hwl = new QLabel(QString::fromStdString(ai::DescribeHardware(hw)));
  hwl->setWordWrap(true);
  lay->addWidget(hwl);
  auto* note = new QLabel(
      "Il modello gira sul tuo computer, senza internet e senza inviare dati. Si scarica una volta sola da Hugging Face "
      "(modelli Qwen3, licenza Apache 2.0, uso commerciale consentito).");
  note->setWordWrap(true);
  lay->addWidget(note);
  std::vector<const ai::ModelInfo*> models;
  for (const auto& m : ai::Catalog()) if (m.listed) models.push_back(&m);
  auto* table = new QTableWidget((int)models.size(), 3);
  table->setHorizontalHeaderLabels({"Modello", "Dimensione", "Stato"});
  table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  table->verticalHeader()->setVisible(false);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSelectionMode(QAbstractItemView::SingleSelection);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  auto refresh = [&] {
    for (int r = 0; r < (int)models.size(); r++) {
      const auto& m = *models[r];
      QString name = QString::fromStdString(m.label);
      if (&m == &ai::Catalog()[rec]) name += "  ★ consigliato";
      table->setItem(r, 0, new QTableWidgetItem(name));
      table->setItem(r, 1, new QTableWidgetItem(QString::number(m.sizeGB, 'f', 1) + " GB"));
      QString st = !aiqt::InstalledModelPath(m).isEmpty() ? (m.repo.empty() ? "pronto (file dal disco)" : "scaricato")
                   : m.repo.empty()                      ? "scegli il file con \"Usa un file…\""
                   : ai::ModelFits(m, hw)                ? "da scaricare"
                                                         : "da scaricare (troppo grande per questo computer)";
      if (QString::fromStdString(m.id) == CurrentModelId()) st += ", in uso";
      table->setItem(r, 2, new QTableWidgetItem(st));
    }
  };
  refresh();
  for (int r = 0; r < (int)models.size(); r++)
    if (QString::fromStdString(models[r]->id) == CurrentModelId()) table->selectRow(r);
  lay->addWidget(table, 1);
  auto* status = new QLabel;
  status->setWordWrap(true);
  lay->addWidget(status);
  auto* bar = new QProgressBar;
  bar->setVisible(false);
  lay->addWidget(bar);
  auto* buttons = new QDialogButtonBox;
  auto* useBtn = buttons->addButton("Usa questo modello", QDialogButtonBox::AcceptRole);
  auto* dlBtn = buttons->addButton("Scarica", QDialogButtonBox::ActionRole);
  auto* delBtn = buttons->addButton("Elimina dal disco", QDialogButtonBox::ActionRole);
  auto* fileBtn = buttons->addButton("Usa un file…", QDialogButtonBox::ActionRole);
  fileBtn->setToolTip("Per un modello che non si scarica (es. quello addestrato su Kurva3D): scegli il file .gguf dal disco");
  buttons->addButton("Chiudi", QDialogButtonBox::RejectRole);
  lay->addWidget(buttons);
  aiqt::ModelDownloader dl;
  auto selected = [&]() -> const ai::ModelInfo* {
    const auto rows = table->selectionModel()->selectedRows();
    return rows.isEmpty() ? nullptr : models[rows.first().row()];
  };
  auto updateButtons = [&] {
    const ai::ModelInfo* m = selected();
    const bool have = m && !aiqt::InstalledModelPath(*m).isEmpty();
    useBtn->setEnabled(have && !dl.Busy());
    delBtn->setEnabled(have && !dl.Busy());
    dlBtn->setText(dl.Busy() ? "Annulla download" : "Scarica");
    const bool local = m && m->repo.empty();  // si sceglie il file dal disco, non si scarica
    dlBtn->setEnabled(dl.Busy() || (m && !have && !local));
    fileBtn->setEnabled(m && local && !dl.Busy());
  };
  connect(table->selectionModel(), &QItemSelectionModel::selectionChanged, &dlg, [&] { updateButtons(); });
  connect(&dl, &aiqt::ModelDownloader::Status, &dlg, [&](const QString& s) { status->setText(s); });
  connect(&dl, &aiqt::ModelDownloader::Progress, &dlg, [&](qint64 d, qint64 t) {
    bar->setVisible(true);
    bar->setRange(0, 1000);
    bar->setValue(t > 0 ? int(1000.0 * double(d) / double(t)) : 0);
    bar->setFormat(QString("%1 / %2 GB").arg(double(d) / 1073741824.0, 0, 'f', 2).arg(double(t) / 1073741824.0, 0, 'f', 2));
  });
  connect(&dl, &aiqt::ModelDownloader::Finished, &dlg, [&](bool ok, const QString& r) {
    bar->setVisible(false);
    status->setText(ok ? "Modello scaricato e verificato (licenza e SHA-256)." : "Download non riuscito: " + r);
    if (ok) if (const ai::ModelInfo* m = selected()) QSettings().setValue("ai/model", QString::fromStdString(m->id));
    refresh();
    updateButtons();
    UpdateModelLabel();
  });
  connect(dlBtn, &QPushButton::clicked, &dlg, [&] {
    if (dl.Busy()) { dl.Cancel(); return; }
    const ai::ModelInfo* m = selected();
    if (!m) return;
    if (!ai::ModelFits(*m, hw) &&
        QMessageBox::question(&dlg, "Modello grande", "Questo modello potrebbe essere troppo lento o non entrare nella memoria. Scaricarlo comunque?") !=
            QMessageBox::Yes)
      return;
    dl.Start(*m);
    updateButtons();
  });
  connect(fileBtn, &QPushButton::clicked, &dlg, [&] {
    const ai::ModelInfo* m = selected();
    if (!m) return;
    const QString f = QFileDialog::getOpenFileName(&dlg, "Scegli il modello GGUF", QDir::homePath(), "Modelli GGUF (*.gguf)");
    if (f.isEmpty()) return;
    if (!aiqt::UseLocalModelFile(*m, f)) { status->setText("Il file scelto non e' un modello GGUF."); return; }
    QSettings().setValue("ai/model", QString::fromStdString(m->id));
    status->setText("Modello pronto: " + QDir::toNativeSeparators(f));
    refresh();
    updateButtons();
    UpdateModelLabel();
  });
  connect(delBtn, &QPushButton::clicked, &dlg, [&] {
    const ai::ModelInfo* m = selected();
    if (!m) return;
    if (QMessageBox::question(&dlg, "Elimina modello",
                              m->repo.empty() ? "Togliere il modello dall'elenco? Il file scelto resta dov'e'."
                                              : "Eliminare il modello dal disco?") != QMessageBox::Yes)
      return;
    if (m_server.ModelPath() == aiqt::InstalledModelPath(*m)) m_server.Stop();
    QDir(aiqt::ModelsDir() + "/" + QString::fromStdString(m->id)).removeRecursively();
    refresh();
    updateButtons();
    UpdateModelLabel();
  });
  connect(buttons, &QDialogButtonBox::accepted, &dlg, [&] {
    if (const ai::ModelInfo* m = selected()) QSettings().setValue("ai/model", QString::fromStdString(m->id));
    dlg.accept();
  });
  connect(buttons, &QDialogButtonBox::rejected, &dlg, [&] {
    if (dl.Busy()) dl.Cancel();
    dlg.reject();
  });
  updateButtons();
  dlg.exec();
  if (dl.Busy()) dl.Cancel();
  UpdateModelLabel();
}

}  // namespace cad::ui
