#include "ui/WhatsNew.h"
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace cad::ui {

namespace {
const char* kKey = "novita/nascostaPerVersione";
QPointer<QDialog> g_open;  // una sola finestra alla volta
}

bool WhatsNewWanted() {
  return QSettings().value(kKey).toString() != QString(KURVA3D_VERSION);
}

void ShowWhatsNew(QWidget* parent) {
  if (g_open) { g_open->raise(); g_open->activateWindow(); return; }
  auto* dlg = new QDialog(parent);
  g_open = dlg;
  dlg->setObjectName("whatsNewDialog");
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  dlg->setWindowTitle(QString("Novità di Kurva3D %1").arg(KURVA3D_VERSION));
  dlg->resize(620, 560);

  auto* lay = new QVBoxLayout(dlg);
  auto* head = new QHBoxLayout;
  auto* icon = new QLabel;
  icon->setPixmap(QIcon(":/kurva3d.png").pixmap(48, 48));
  head->addWidget(icon);
  auto* title = new QLabel(QString("<h2 style='margin:0'>Novità della versione %1</h2>").arg(KURVA3D_VERSION));
  head->addWidget(title, 1);
  lay->addLayout(head);

  auto* text = new QTextBrowser;
  text->setOpenExternalLinks(true);
  QFile f(":/novita.html");
  if (f.open(QIODevice::ReadOnly | QIODevice::Text)) text->setHtml(QString::fromUtf8(f.readAll()));
  lay->addWidget(text, 1);

  auto* hide = new QCheckBox("Non mostrare più per questa versione");
  hide->setObjectName("whatsNewHide");
  hide->setChecked(!WhatsNewWanted());
  lay->addWidget(hide);

  auto* bb = new QDialogButtonBox(QDialogButtonBox::Close);
  bb->button(QDialogButtonBox::Close)->setText("Chiudi");
  lay->addWidget(bb);
  QObject::connect(bb, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
  // la scelta vale subito, anche se la finestra si chiude con la X
  QObject::connect(hide, &QCheckBox::toggled, dlg, [](bool on) {
    QSettings s;
    if (on) s.setValue(kKey, QString(KURVA3D_VERSION));
    else s.remove(kKey);
  });
  dlg->show();
  dlg->raise();
}

}  // namespace cad::ui
