#include "ui/CollabPanel.h"
#include "collab/Session.h"
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QSettings>
#include <QSysInfo>
#include <QTime>
#include <QVBoxLayout>

namespace cad::ui {

CollabPanel::CollabPanel(collab::Session& s, std::function<QString()> projectName,
                         std::function<void(const QString&, const QString&, std::function<void(QString)>)> runAi,
                         QWidget* parent)
    : QWidget(parent), m_s(s), m_project(std::move(projectName)), m_runAi(std::move(runAi)) {
  auto* lay = new QVBoxLayout(this);
  m_status = new QLabel(this);
  m_status->setWordWrap(true);
  lay->addWidget(m_status);

  auto* nameRow = new QFormLayout;
  m_name = new QLineEdit(QSettings().value("collab/name", QSysInfo::machineHostName()).toString(), this);
  m_name->setMaxLength(60);
  nameRow->addRow("Il tuo nome:", m_name);
  lay->addLayout(nameRow);
  connect(m_name, &QLineEdit::editingFinished, this, [this] { QSettings().setValue("collab/name", m_name->text()); });

  // --- ospitare
  m_hostBox = new QGroupBox("Condividi questo progetto", this);
  auto* hl = new QVBoxLayout(m_hostBox);
  m_host = new QPushButton("Avvia sessione", m_hostBox);
  m_pin = new QLabel(m_hostBox);
  m_pin->setTextInteractionFlags(Qt::TextSelectableByMouse);
  m_pin->setWordWrap(true);
  hl->addWidget(m_host);
  hl->addWidget(m_pin);
  lay->addWidget(m_hostBox);
  connect(m_host, &QPushButton::clicked, this, &CollabPanel::HostSession);

  // --- entrare
  m_joinBox = new QGroupBox("Entra in una sessione", this);
  auto* jl = new QVBoxLayout(m_joinBox);
  jl->addWidget(new QLabel("Sessioni trovate sulla rete locale (doppio clic per sceglierla):", m_joinBox));
  m_found = new QListWidget(m_joinBox);
  m_found->setMaximumHeight(90);
  jl->addWidget(m_found);
  auto* jf = new QFormLayout;
  m_address = new QLineEdit(m_joinBox);
  m_address->setPlaceholderText("es. 192.168.1.20 oppure 192.168.1.20:45455");
  m_code = new QLineEdit(m_joinBox);
  m_code->setPlaceholderText("6 cifre");
  m_code->setMaxLength(6);
  m_code->setValidator(new QRegularExpressionValidator(QRegularExpression("[0-9]{0,6}"), m_code));
  jf->addRow("Indirizzo:", m_address);
  jf->addRow("Codice:", m_code);
  jl->addLayout(jf);
  m_join = new QPushButton("Entra", m_joinBox);
  jl->addWidget(m_join);
  lay->addWidget(m_joinBox);
  connect(m_found, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) {
    m_address->setText(it->data(Qt::UserRole).toString());
    m_code->setFocus();
  });
  connect(m_found, &QListWidget::itemClicked, this,
          [this](QListWidgetItem* it) { m_address->setText(it->data(Qt::UserRole).toString()); });
  connect(m_join, &QPushButton::clicked, this, &CollabPanel::JoinSession);
  connect(m_code, &QLineEdit::returnPressed, this, &CollabPanel::JoinSession);

  m_leave = new QPushButton("Esci dalla sessione", this);
  lay->addWidget(m_leave);
  connect(m_leave, &QPushButton::clicked, this, [this] { m_s.Leave(); });

  // --- partecipanti, chat, AI
  lay->addWidget(new QLabel("Partecipanti:", this));
  m_peers = new QListWidget(this);
  m_peers->setMaximumHeight(80);
  lay->addWidget(m_peers);
  m_chat = new QPlainTextEdit(this);
  m_chat->setReadOnly(true);
  m_chat->setPlaceholderText("Messaggi della sessione");
  lay->addWidget(m_chat, 1);
  auto* cr = new QHBoxLayout;
  m_chatInput = new QLineEdit(this);
  m_chatInput->setPlaceholderText("Scrivi un messaggio…");
  auto* send = new QPushButton("Invia", this);
  cr->addWidget(m_chatInput, 1);
  cr->addWidget(send);
  lay->addLayout(cr);
  connect(send, &QPushButton::clicked, this, &CollabPanel::SendChat);
  connect(m_chatInput, &QLineEdit::returnPressed, this, &CollabPanel::SendChat);

  auto* aiBox = new QGroupBox("Compito per un'AI", this);
  auto* al = new QVBoxLayout(aiBox);
  m_target = new QComboBox(aiBox);
  m_task = new QLineEdit(aiBox);
  m_task->setPlaceholderText("es. aggiungi quattro colonne di raggio 20 agli angoli");
  m_sendTask = new QPushButton("Invia all'AI", aiBox);
  al->addWidget(m_target);
  al->addWidget(m_task);
  al->addWidget(m_sendTask);
  lay->addWidget(aiBox);
  connect(m_sendTask, &QPushButton::clicked, this, &CollabPanel::SendTask);
  connect(m_task, &QLineEdit::returnPressed, this, &CollabPanel::SendTask);

  // --- segnali della sessione
  connect(&m_s, &collab::Session::PeersChanged, this, &CollabPanel::Refresh);
  connect(&m_s, &collab::Session::Joined, this, &CollabPanel::Refresh);
  connect(&m_s, &collab::Session::DiscoveredChanged, this, &CollabPanel::RefreshFound);
  connect(&m_s, &collab::Session::Message, this, [this](const QString& t) { Append("• " + t); });
  connect(&m_s, &collab::Session::Ended, this, [this](const QString& r) {
    if (!r.isEmpty()) Append("• Sessione terminata: " + r);
    Refresh();
  });
  connect(&m_s, &collab::Session::Chat, this, [this](const QString& f, const QString& t) { Append(f + ": " + t); });
  connect(&m_s, &collab::Session::AiTask, this, [this](const QString& from, const QString& text) {
    Append("• Compito per la tua AI da " + from + ": " + text);
    if (!m_runAi) return;
    const QString me = MyName();
    m_runAi(from, text, [this, from, text, me](const QString& result) {
      const QString line = "AI di " + me + " (compito di " + from + "): " + result;
      if (m_s.Active()) m_s.SendChat(line);
      else Append(line);
    });
  });
  m_s.StartDiscovery();
  Refresh();
}

QString CollabPanel::MyName() const {
  if (m_s.Active() && !m_s.MyName().isEmpty()) return m_s.MyName();
  const QString n = m_name->text().trimmed();
  return n.isEmpty() ? QSysInfo::machineHostName() : n;
}

void CollabPanel::Append(const QString& line) {
  m_chat->appendPlainText(QTime::currentTime().toString("HH:mm") + "  " + line);
}

void CollabPanel::HostSession() {
  QSettings().setValue("collab/name", m_name->text());
  const QString pin = m_s.Host(MyName(), m_project ? m_project() : QString());
  if (pin.isEmpty()) return;
  Refresh();
}

void CollabPanel::JoinSession() {
  QString addr = m_address->text().trimmed();
  quint16 port = collab::kSessionPort;
  const int colon = addr.lastIndexOf(':');
  if (colon > 0 && addr.count(':') == 1) {
    port = static_cast<quint16>(addr.mid(colon + 1).toUInt());
    addr = addr.left(colon);
  }
  if (addr.isEmpty() || m_code->text().size() != 6) {
    m_status->setText("Inserisci l'indirizzo di chi ospita e il codice a 6 cifre che vede nel suo pannello.");
    return;
  }
  if (QMessageBox::question(this, "Entra nella sessione",
                            "Il modello aperto verra' sostituito da quello di chi ospita la sessione "
                            "(salvalo prima se ti serve). Continuare?") != QMessageBox::Yes)
    return;
  QSettings().setValue("collab/name", m_name->text());
  m_s.Join(addr, port, m_code->text(), MyName());
  Refresh();
}

void CollabPanel::SendChat() {
  const QString t = m_chatInput->text().trimmed();
  if (t.isEmpty()) return;
  if (!m_s.Active()) {
    m_status->setText("Prima avvia una sessione o entra in una.");
    return;
  }
  m_s.SendChat(t);
  m_chatInput->clear();
}

void CollabPanel::SendTask() {
  const QString t = m_task->text().trimmed();
  if (t.isEmpty()) return;
  const int target = m_target->currentData().isValid() ? m_target->currentData().toInt() : m_s.MyId();
  if (target != m_s.MyId()) Append("• Compito per l'AI di " + m_target->currentText() + ": " + t);
  m_s.SendAiTask(target, t);
  m_task->clear();
}

void CollabPanel::Refresh() {
  const bool active = m_s.Active(), connecting = m_s.Connecting();
  m_hostBox->setVisible(!active && !connecting);
  m_joinBox->setVisible(!active && !connecting);
  if (m_s.IsHost()) m_hostBox->setVisible(true);  // il codice resta in vista
  m_host->setVisible(!active);
  m_leave->setVisible(active || connecting);
  m_leave->setText(m_s.IsHost() ? "Chiudi la sessione" : "Esci dalla sessione");
  m_name->setEnabled(!active && !connecting);
  if (m_s.IsHost()) {
    const QStringList ips = collab::Session::LocalAddresses();
    m_pin->setText(QString("<span style='font-size:20pt; font-weight:600; letter-spacing:4px'>%1</span><br>"
                           "Comunica il codice a chi deve entrare. Indirizzo di questo computer: %2")
                       .arg(m_s.Pin(), ips.isEmpty() ? QString("(nessuna rete)") : ips.join(", ")));
    m_status->setText("Stai ospitando la sessione: chi entra lavora sul tuo progetto e vede le modifiche di tutti.");
  } else if (active) {
    m_pin->clear();
    m_status->setText("Sei in una sessione: le modifiche si condividono automaticamente.");
  } else if (connecting) {
    m_status->setText("Connessione in corso…");
  } else {
    m_pin->clear();
    m_status->setText("Lavora insieme ad altri sulla rete locale: avvia una sessione per condividere questo progetto, "
                      "oppure entra in quella di un altro computer.");
  }
  m_peers->clear();
  const int cur = m_target->currentData().isValid() ? m_target->currentData().toInt() : -1;
  m_target->clear();
  m_target->addItem("La mia AI", m_s.MyId());
  for (const collab::Peer& p : m_s.Participants()) {
    QString label = p.name;
    if (p.id == 0) label += " (ospite)";
    if (active && p.id == m_s.MyId()) label += " (tu)";
    m_peers->addItem(label);
    if (active && p.id != m_s.MyId()) m_target->addItem("AI di " + p.name, p.id);
  }
  const int idx = m_target->findData(cur);
  if (idx >= 0) m_target->setCurrentIndex(idx);
  RefreshFound();
}

void CollabPanel::RefreshFound() {
  m_found->clear();
  for (const collab::FoundSession& f : m_s.Discovered()) {
    QString label = f.name;
    if (!f.project.isEmpty()) label += " — " + f.project;
    label += "  (" + f.address + ")";
    auto* it = new QListWidgetItem(label, m_found);
    const QString addr = f.port == collab::kSessionPort ? f.address : f.address + ":" + QString::number(f.port);
    it->setData(Qt::UserRole, addr);
  }
  if (m_found->count() == 0) {
    auto* it = new QListWidgetItem("(nessuna sessione trovata: chiedi l'indirizzo IP a chi ospita)", m_found);
    it->setFlags(Qt::NoItemFlags);
  }
}

}  // namespace cad::ui
