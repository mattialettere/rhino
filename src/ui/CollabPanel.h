// Pannello "Collabora": ospitare una sessione (codice a 6 cifre), trovare quelle sulla rete
// locale o entrare con l'indirizzo IP, partecipanti, chat e compiti per l'assistente AI di
// un partecipante (anche il proprio).
#pragma once
#include <QWidget>
#include <functional>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QGroupBox;

namespace cad::collab {
class Session;
}

namespace cad::ui {

class CollabPanel : public QWidget {
  Q_OBJECT
 public:
  // runAi: esegue un compito con l'assistente AI di questo computer, poi chiama done(esito)
  CollabPanel(collab::Session& s, std::function<QString()> projectName,
              std::function<void(const QString& from, const QString& text, std::function<void(QString)> done)> runAi,
              QWidget* parent = nullptr);

 private:
  void Refresh();
  void RefreshFound();
  void HostSession();
  void JoinSession();
  void SendChat();
  void SendTask();
  void Append(const QString& line);
  QString MyName() const;

  collab::Session& m_s;
  std::function<QString()> m_project;
  std::function<void(const QString&, const QString&, std::function<void(QString)>)> m_runAi;
  QLabel* m_status = nullptr;
  QLineEdit* m_name = nullptr;
  QGroupBox* m_hostBox = nullptr;
  QGroupBox* m_joinBox = nullptr;
  QPushButton* m_host = nullptr;
  QLabel* m_pin = nullptr;
  QListWidget* m_found = nullptr;
  QLineEdit* m_address = nullptr;
  QLineEdit* m_code = nullptr;
  QPushButton* m_join = nullptr;
  QPushButton* m_leave = nullptr;
  QListWidget* m_peers = nullptr;
  QPlainTextEdit* m_chat = nullptr;
  QLineEdit* m_chatInput = nullptr;
  QComboBox* m_target = nullptr;
  QLineEdit* m_task = nullptr;
  QPushButton* m_sendTask = nullptr;
};

}  // namespace cad::ui
