#include "ui/TexturePanel.h"
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QListWidget>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

namespace cad::ui {

namespace {
const QStringList kPatterns = {"*.png", "*.jpg", "*.jpeg", "*.bmp", "*.gif", "*.tif", "*.tiff", "*.webp"};

QString DefaultFolder() {
  return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/Kurva3D/Texture";
}

// Lista che trascina il percorso dell'immagine (sulle viste) invece degli elementi.
class TextureList : public QListWidget {
 public:
  using QListWidget::QListWidget;

 protected:
  QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override {
    auto* md = new QMimeData;
    if (!items.isEmpty()) {
      const QString f = items.first()->data(Qt::UserRole).toString();
      md->setData(TexturePanel::MimeType(), f.toUtf8());
      md->setUrls({QUrl::fromLocalFile(f)});
    }
    return md;
  }
  QStringList mimeTypes() const override { return {TexturePanel::MimeType(), "text/uri-list"}; }
};

QIcon Thumbnail(const QString& file, int size) {
  QImageReader r(file);
  r.setAutoTransform(true);
  const QSize s = r.size();
  if (s.isValid()) r.setScaledSize(s.scaled(size, size, Qt::KeepAspectRatioByExpanding));
  QImage img = r.read();
  if (img.isNull()) return {};
  // quadrata, ritagliata al centro, angoli arrotondati
  img = img.copy((img.width() - std::min(img.width(), size)) / 2, (img.height() - std::min(img.height(), size)) / 2,
                 std::min(img.width(), size), std::min(img.height(), size));
  QPixmap px(size, size);
  px.fill(Qt::transparent);
  QPainter p(&px);
  p.setRenderHint(QPainter::Antialiasing);
  QPainterPath clip;
  clip.addRoundedRect(QRectF(0, 0, size, size), 8, 8);
  p.setClipPath(clip);
  p.drawImage(QRect(0, 0, size, size), img);
  return QIcon(px);
}
}  // namespace

bool TexturePanel::IsImageFile(const QString& path) {
  const QString s = QFileInfo(path).suffix().toLower();
  for (const QString& pat : kPatterns)
    if (pat.mid(2) == s) return true;
  return false;
}

TexturePanel::TexturePanel(ApplyFn apply, std::function<void()> remove, QWidget* parent)
    : QWidget(parent), m_apply(std::move(apply)), m_remove(std::move(remove)) {
  auto* lay = new QVBoxLayout(this);
  lay->setContentsMargins(6, 6, 6, 6);
  lay->setSpacing(6);

  auto* top = new QHBoxLayout;
  m_folderLabel = new QLabel;
  m_folderLabel->setObjectName("hint");
  m_folderLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  m_folderLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);  // percorsi lunghi: non allargano il pannello
  top->addWidget(m_folderLabel, 1);
  auto* choose = new QPushButton("Cartella…");
  choose->setToolTip("Scegli la cartella delle texture");
  connect(choose, &QPushButton::clicked, this, &TexturePanel::ChooseFolder);
  top->addWidget(choose);
  lay->addLayout(top);

  m_list = new TextureList;
  m_list->setViewMode(QListView::IconMode);
  m_list->setIconSize(QSize(80, 80));
  m_list->setGridSize(QSize(96, 112));
  m_list->setResizeMode(QListView::Adjust);
  m_list->setMovement(QListView::Static);
  m_list->setWordWrap(true);
  m_list->setDragEnabled(true);
  m_list->setDragDropMode(QAbstractItemView::DragOnly);
  m_list->setSelectionMode(QAbstractItemView::SingleSelection);
  m_list->setToolTip("Doppio clic: applica agli oggetti selezionati. Oppure trascina la texture su un oggetto.");
  connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) {
    if (!m_apply(it->data(Qt::UserRole).toString())) m_hint->setText("Seleziona prima gli oggetti, o trascina la texture su un oggetto.");
    else m_hint->setText("");
  });
  lay->addWidget(m_list, 1);

  auto* form = new QFormLayout;
  m_mapping = new QComboBox;
  m_mapping->addItem("Automatica", "");
  m_mapping->addItem("Superficie", "Surface");
  m_mapping->addItem("Box", "Box");
  m_mapping->addItem("Piana (dall'alto)", "Planar");
  m_mapping->addItem("Cilindrica", "Cylindrical");
  m_mapping->addItem("Sferica", "Spherical");
  m_mapping->setToolTip("Come l'immagine si avvolge sull'oggetto. Automatica: superficie per NURBS, box per mesh e SubD.");
  form->addRow("Mappatura", m_mapping);
  m_repeat = new QDoubleSpinBox;
  m_repeat->setRange(0.1, 100);
  m_repeat->setDecimals(1);
  m_repeat->setSingleStep(0.5);
  m_repeat->setValue(1);
  m_repeat->setToolTip("Quante volte l'immagine si ripete sull'oggetto");
  form->addRow("Ripetizioni", m_repeat);
  lay->addLayout(form);

  auto* buttons = new QHBoxLayout;
  auto* applyBtn = new QPushButton("Applica");
  applyBtn->setDefault(true);
  applyBtn->setToolTip("Applica la texture scelta agli oggetti selezionati");
  connect(applyBtn, &QPushButton::clicked, this, [this] {
    const QString f = SelectedFile();
    if (f.isEmpty()) { m_hint->setText("Scegli una texture dall'elenco."); return; }
    m_hint->setText(m_apply(f) ? "" : "Seleziona prima gli oggetti, o trascina la texture su un oggetto.");
  });
  auto* removeBtn = new QPushButton("Togli");
  removeBtn->setToolTip("Toglie la texture dagli oggetti selezionati");
  connect(removeBtn, &QPushButton::clicked, this, [this] { m_remove(); });
  auto* addBtn = new QPushButton("Aggiungi…");
  addBtn->setToolTip("Copia immagini nella cartella delle texture");
  connect(addBtn, &QPushButton::clicked, this, &TexturePanel::AddImages);
  buttons->addWidget(applyBtn);
  buttons->addWidget(removeBtn);
  buttons->addStretch(1);
  buttons->addWidget(addBtn);
  lay->addLayout(buttons);
  m_hint = new QLabel;
  m_hint->setObjectName("hint");
  m_hint->setWordWrap(true);
  lay->addWidget(m_hint);

  m_folder = QSettings().value("textures/folder", DefaultFolder()).toString();
  m_mapping->setCurrentIndex(std::max(0, m_mapping->findData(QSettings().value("textures/mapping", "").toString())));
  m_repeat->setValue(QSettings().value("textures/repeat", 1.0).toDouble());
  connect(m_mapping, &QComboBox::currentIndexChanged, this,
          [this] { QSettings().setValue("textures/mapping", m_mapping->currentData()); });
  connect(m_repeat, &QDoubleSpinBox::valueChanged, this, [](double v) { QSettings().setValue("textures/repeat", v); });
  Reload();
}

QString TexturePanel::CommandOptions() const {
  QString o = QString(" Repeat=%1").arg(m_repeat->value());
  const QString m = m_mapping->currentData().toString();
  if (!m.isEmpty()) o += " Mapping=" + m;
  return o;
}

QString TexturePanel::SelectedFile() const {
  const auto items = m_list->selectedItems();
  return items.isEmpty() ? QString() : items.first()->data(Qt::UserRole).toString();
}

void TexturePanel::Reload() {
  QDir().mkpath(m_folder);
  m_folderLabel->setText(QDir(m_folder).dirName());
  m_folderLabel->setToolTip(QDir::toNativeSeparators(m_folder));
  m_list->clear();
  const QFileInfoList files = QDir(m_folder).entryInfoList(kPatterns, QDir::Files, QDir::Name | QDir::IgnoreCase);
  for (const QFileInfo& fi : files) {
    QIcon icon = Thumbnail(fi.absoluteFilePath(), 80);
    if (icon.isNull()) continue;
    auto* it = new QListWidgetItem(icon, fi.completeBaseName());
    it->setData(Qt::UserRole, fi.absoluteFilePath());
    it->setToolTip(fi.fileName());
    m_list->addItem(it);
  }
  if (m_hint)
    m_hint->setText(m_list->count() ? "" : "La cartella è vuota: usa Aggiungi… per copiarci delle immagini, oppure scegli un'altra cartella.");
}

void TexturePanel::ChooseFolder() {
  const QString d = QFileDialog::getExistingDirectory(this, "Cartella delle texture", m_folder);
  if (d.isEmpty()) return;
  m_folder = d;
  QSettings().setValue("textures/folder", d);
  Reload();
}

void TexturePanel::AddImages() {
  const QStringList files = QFileDialog::getOpenFileNames(this, "Aggiungi immagini", QString(),
                                                          "Immagini (" + kPatterns.join(' ') + ")");
  for (const QString& f : files) {
    const QString dst = m_folder + "/" + QFileInfo(f).fileName();
    if (QFileInfo(dst).exists()) continue;
    QFile::copy(f, dst);
  }
  if (!files.isEmpty()) Reload();
}

}  // namespace cad::ui
