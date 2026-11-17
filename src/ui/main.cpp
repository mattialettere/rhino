// rhino-ui: finestra principale in stile Rhino 8.
//   rhino-ui [file.3dm] [--script comandi.txt] [--screenshot out.png]
#include "ui/MainWindow.h"
#include <QApplication>
#include <QFile>
#include <QScreen>
#include <QSurfaceFormat>
#include <QTimer>
#include <QTextStream>

int main(int argc, char** argv) {
  // Contesto OpenGL di compatibilita' (pipeline fissa, funziona ovunque)
  QSurfaceFormat fmt;
  fmt.setProfile(QSurfaceFormat::CompatibilityProfile);
  fmt.setVersion(2, 1);
  fmt.setDepthBufferSize(24);
  fmt.setSamples(4);
  QSurfaceFormat::setDefaultFormat(fmt);
  QApplication app(argc, argv);
  cad::InitLibrary();
  QString open, script, shot;
  const QStringList args = app.arguments();
  for (int i = 1; i < args.size(); i++) {
    if (args[i] == "--script" && i + 1 < args.size()) script = args[++i];
    else if (args[i] == "--screenshot" && i + 1 < args.size()) shot = args[++i];
    else open = args[i];
  }
  int rc;
  {
    cad::ui::MainWindow w;
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
    if (!shot.isEmpty()) {
      QTimer::singleShot(800, [&] {
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
