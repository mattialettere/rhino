// Pannello "Texture": libreria di immagini da una cartella (anteprime), da applicare
// agli oggetti selezionati o trascinandole su un oggetto nelle viste.
#pragma once
#include <QString>
#include <QWidget>
#include <functional>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;

namespace cad::ui {

class TexturePanel : public QWidget {
  Q_OBJECT
 public:
  // apply(file, true) applica alla selezione; restituisce false se non c'e' selezione
  using ApplyFn = std::function<bool(const QString& file)>;
  TexturePanel(ApplyFn apply, std::function<void()> remove, QWidget* parent = nullptr);
  // Opzioni del comando ApplyTexture scelte nel pannello (mappatura e ripetizione).
  QString CommandOptions() const;
  QString Folder() const { return m_folder; }
  void Reload();

  static const char* MimeType() { return "application/x-kurva3d-texture"; }
  static bool IsImageFile(const QString& path);

 private:
  void ChooseFolder();
  void AddImages();
  QString SelectedFile() const;

  ApplyFn m_apply;
  std::function<void()> m_remove;
  QString m_folder;
  QLabel* m_folderLabel = nullptr;
  QLabel* m_hint = nullptr;
  QListWidget* m_list = nullptr;
  QComboBox* m_mapping = nullptr;
  QDoubleSpinBox* m_repeat = nullptr;
};

}  // namespace cad::ui
