// Editor di script Python (comando ScriptEditor): testo con evidenziazione della sintassi,
// Apri/Salva ed Esegui (F5). Lo script gira con RunPythonScript; l'output va nella cronologia.
#pragma once
#include <QString>
#include <QWidget>
#include <functional>

class QPlainTextEdit;
class QLabel;

namespace cad::ui {

class ScriptEditor : public QWidget {
  Q_OBJECT
 public:
  // run(percorso del file .py da eseguire)
  ScriptEditor(std::function<void(const QString& file)> run, QWidget* parent = nullptr);
  bool OpenFile(const QString& file);
  void Run();

 private:
  bool Save(bool askName);
  void UpdateTitle();
  std::function<void(const QString&)> m_run;
  QPlainTextEdit* m_text = nullptr;
  QLabel* m_file = nullptr;
  QString m_path;
};

}  // namespace cad::ui
