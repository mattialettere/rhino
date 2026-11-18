// kurva3d: finestra principale in stile Rhino 8.
//   kurva3d [file.3dm] [--script comandi.txt] [--screenshot out.png]
#include "python/PythonHost.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include <QApplication>
#include <QFile>
#include <QDockWidget>
#include <QFileOpenEvent>
#include <QIcon>
#include <QScreen>
#include <QSurfaceFormat>
#include <QTimer>
#include <functional>
#include <QTextStream>

// macOS: i file aperti dal Finder (doppio clic, trascinamento sull'icona)
// arrivano come evento QFileOpenEvent invece che come argomento.
class FileOpenFilter : public QObject {
public:
  std::function<void(const QString&)> open;
  bool eventFilter(QObject* o, QEvent* e) override {
    if (e->type() == QEvent::FileOpen && open) { open(static_cast<QFileOpenEvent*>(e)->file()); return true; }
    return QObject::eventFilter(o, e);
  }
};

int main(int argc, char** argv) {
  // Contesto OpenGL di compatibilita' (pipeline fissa, funziona ovunque)
  QSurfaceFormat fmt;
  fmt.setProfile(QSurfaceFormat::CompatibilityProfile);
  fmt.setVersion(2, 1);
  fmt.setDepthBufferSize(24);
  fmt.setSamples(4);
  QSurfaceFormat::setDefaultFormat(fmt);
  QApplication app(argc, argv);
  QApplication::setOrganizationName("Kurva3D");
  QApplication::setApplicationName("Kurva3D");
  QApplication::setApplicationVersion(KURVA3D_VERSION);
  QApplication::setWindowIcon(QIcon(":/kurva3d.png"));
  cad::ui::ApplyTheme(cad::ui::SavedThemeMode());
  cad::InitLibrary();
  cad::SetPythonProgramDir(QCoreApplication::applicationDirPath().toStdString());
  QString open, script, shot, panel;
  int wait = 800;
  const QStringList args = app.arguments();
  for (int i = 1; i < args.size(); i++) {
    if (args[i] == "--script" && i + 1 < args.size()) script = args[++i];
    else if (args[i] == "--screenshot" && i + 1 < args.size()) shot = args[++i];
    else if (args[i] == "--panel" && i + 1 < args.size()) panel = args[++i];  // es. textureDock
    else if (args[i] == "--wait" && i + 1 < args.size()) wait = args[++i].toInt();  // ms prima della cattura
    else if (!args[i].startsWith("-psn")) open = args[i];
  }
  int rc;
  {
    cad::ui::MainWindow w;
    FileOpenFilter fof;
    fof.open = [&w](const QString& f) { w.OpenFile(f); };
    app.installEventFilter(&fof);
    w.show();
    if (!open.isEmpty()) w.OpenFile(open);
    if (!script.isEmpty()) {
      QFile f(script);
      if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&f);
        QStringList lines;
        while (!in.atEnd()) {
          const QString line = in.readLine().trimmed();
          if (!line.isEmpty() && !line.startsWith(';')) lines << line;
        }
        for (int i = 0; i < lines.size(); i++) {
          // "@" = riga che risponde al prompt del comando in corso (come scrivere nella riga di comando)
          w.Submit(lines[i].startsWith('@') ? lines[i].mid(1).trimmed() : lines[i]);
          const bool continues = i + 1 < lines.size() && lines[i + 1].startsWith('@');
          // altrimenti, come in una macro di Rhino, la fine della riga equivale a Invio
          for (int guard = 0; !continues && w.HasPending() && guard < 10; guard++) w.Submit("");
        }
      }
      w.ZoomExtentsAll();
    }
    for (auto* d : w.findChildren<QDockWidget*>())
      if (!panel.isEmpty() && d->objectName() == panel) { d->show(); d->raise(); }
    if (!shot.isEmpty()) {
      QTimer::singleShot(wait, [&] {
        // cattura dello schermo (include i titoli delle viste disegnati sopra OpenGL)
        w.screen()->grabWindow(w.winId()).save(shot);
        app.quit();
      });
    }
    rc = app.exec();
  }
  cad::ShutdownLibrary();
  return rc;
}
