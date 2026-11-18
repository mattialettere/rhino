// Pannello "Assistente AI": descrizione in testo -> comandi di Kurva3D generati da un
// modello locale, mostrati in anteprima nel disegno e confermati o scartati dall'utente.
#pragma once
#include "aiqt/AiService.h"
#include <QWidget>
#include <deque>
#include <functional>
#include <optional>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QProgressBar;

namespace cad {
class Engine;
}

namespace cad::ui {

class AiPanel : public QWidget {
  Q_OBJECT
 public:
  AiPanel(cad::Engine& engine, std::function<void()> afterChange, QWidget* parent = nullptr);
  ~AiPanel() override;
  // Finestra per scegliere e scaricare il modello (hardware e consiglio).
  void ShowModelDialog();
  // Collaborazione: compito arrivato da un partecipante. Si genera e si applica subito (senza
  // anteprima da confermare); done riceve l'esito in una riga. I compiti si eseguono in fila.
  void RunTask(const QString& from, const QString& text, std::function<void(QString)> done);

 private:
  struct Task {
    QString from, text;
    std::function<void(QString)> done;
  };
  void StartNextTask();
  void FinishTask(const QString& result);
  std::deque<Task> m_tasks;
  std::optional<Task> m_task;
  void Generate();
  void ApplyPreview(const QStringList& script);
  void Confirm();
  void Discard();
  void SetBusy(bool busy, const QString& status);
  void UpdateModelLabel();
  QString CurrentModelId() const;
  void EnsureServer(std::function<void()> then, std::function<void(QString)> fail = {});

  cad::Engine& m_engine;
  std::function<void()> m_after;
  aiqt::LlamaServer m_server;
  aiqt::Generator m_gen;
  QLabel* m_model = nullptr;
  QLabel* m_status = nullptr;
  QPlainTextEdit* m_request = nullptr;
  QPlainTextEdit* m_script = nullptr;
  QPushButton* m_generate = nullptr;
  QPushButton* m_apply = nullptr;
  QPushButton* m_confirm = nullptr;
  QPushButton* m_discard = nullptr;
  QProgressBar* m_busy = nullptr;
  bool m_previewing = false;
  size_t m_undoDepth = 0;
};

}  // namespace cad::ui
