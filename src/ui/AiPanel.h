// Pannello "Assistente AI": descrizione in testo -> comandi di Kurva3D generati da un
// modello locale, mostrati in anteprima nel disegno e confermati o scartati dall'utente.
#pragma once
#include "aiqt/AiService.h"
#include <QWidget>
#include <functional>

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

 private:
  void Generate();
  void ApplyPreview(const QStringList& script);
  void Confirm();
  void Discard();
  void SetBusy(bool busy, const QString& status);
  void UpdateModelLabel();
  QString CurrentModelId() const;
  void EnsureServer(std::function<void()> then);

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
