// kurva3d: finestra principale in stile Rhino 8.
//   kurva3d [file.3dm] [--script comandi.txt] [--screenshot out.png]
#include "python/PythonHost.h"
#include "ui/MainWindow.h"
#include "collab/Session.h"
#include "ui/NodeEditor.h"
#include "ui/Theme.h"
#include "ui/WhatsNew.h"
#include <QApplication>
#include <QFile>
#include <QDockWidget>
#include <QFileOpenEvent>
#include <QIcon>
#include <QScreen>
#include <QSurfaceFormat>
#include <QTimer>
#include <QPointingDevice>
#include <QTouchEvent>
#include "ui/Viewport.h"
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
  QString touchDemo;
  QString open, script, shot, panel, nodesFile, nodesShot, collabHost, collabJoin, collabChat, collabScript, collabTask;
  int wait = 800;
  bool keepPending = false;
  bool whatsNew = true, forceWhatsNew = false;  // finestra delle novita' all'avvio
  const QStringList args = app.arguments();
  for (int i = 1; i < args.size(); i++) {
    if (args[i] == "--script" && i + 1 < args.size()) script = args[++i];
    else if (args[i] == "--screenshot" && i + 1 < args.size()) shot = args[++i];
    else if (args[i] == "--panel" && i + 1 < args.size()) panel = args[++i];  // es. textureDock
    else if (args[i] == "--wait" && i + 1 < args.size()) wait = args[++i].toInt();  // ms prima della cattura
    else if (args[i] == "--keep-pending") keepPending = true;  // l'ultimo comando dello script resta in attesa
    // prova del tocco nella vista Top: dito giu' in x,y, spostato in x2,y2 e (con ",up") sollevato
    else if (args[i] == "--touch-demo" && i + 1 < args.size()) touchDemo = args[++i];
    else if (args[i] == "--no-whats-new") whatsNew = false;
    else if (args[i] == "--whats-new") forceWhatsNew = true;  // per le prove: la mostra sempre
    else if (args[i] == "--nodes" && i + 1 < args.size()) nodesFile = args[++i];        // grafo da aprire nell'editor a nodi
    else if (args[i] == "--nodes-screenshot" && i + 1 < args.size()) nodesShot = args[++i];
    // prove della collaborazione: ospitare (il codice va nel file indicato) o entrare (ip:porta:codice),
    // poi un messaggio e comandi da eseguire una volta entrati
    else if (args[i] == "--collab-host" && i + 1 < args.size()) collabHost = args[++i];
    else if (args[i] == "--collab-join" && i + 1 < args.size()) collabJoin = args[++i];
    else if (args[i] == "--collab-chat" && i + 1 < args.size()) collabChat = args[++i];
    else if (args[i] == "--collab-script" && i + 1 < args.size()) collabScript = args[++i];
    else if (args[i] == "--collab-ai-task" && i + 1 < args.size()) collabTask = args[++i];  // per l'AI dell'ospite
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
    // novita' della versione: all'avvio normale (non durante script, screenshot e prove di collaborazione)
    const bool automated = !script.isEmpty() || !shot.isEmpty() || !nodesShot.isEmpty() || !collabHost.isEmpty() || !collabJoin.isEmpty();
    if (forceWhatsNew || (whatsNew && !automated && cad::ui::WhatsNewWanted()))
      QTimer::singleShot(0, &w, [&w] { cad::ui::ShowWhatsNew(&w); });
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
          if (keepPending && i + 1 == lines.size()) break;
          for (int guard = 0; !continues && w.HasPending() && guard < 10; guard++) w.Submit("");
        }
      }
      w.ZoomExtentsAll();
    }
    if (!touchDemo.isEmpty()) {
      QTimer::singleShot(wait / 2, &w, [&w, touchDemo] {
        const QStringList v = touchDemo.split(',');
        auto views = w.findChildren<cad::ui::Viewport*>();
        if (v.size() < 4 || views.isEmpty()) return;
        cad::ui::Viewport* vp = views.first();
        for (auto* x : views)
          if (x->Cam().kind == cad::ui::Camera::Kind::Top) vp = x;
        static QPointingDevice dev("Kurva3D touch demo", 4242, QInputDevice::DeviceType::TouchScreen,
                                   QPointingDevice::PointerType::Finger, QInputDevice::Capability::Position, 10, 0);
        auto send = [vp](QEvent::Type t, QEventPoint::State st, QPointF at) {
          QEventPoint pt(1, st, at, vp->mapToGlobal(at));
          QTouchEvent ev(t, &dev, Qt::NoModifier, {pt});
          QCoreApplication::sendEvent(vp, &ev);
        };
        const QPointF a(v[0].toDouble(), v[1].toDouble()), b(v[2].toDouble(), v[3].toDouble());
        send(QEvent::TouchBegin, QEventPoint::State::Pressed, a);
        send(QEvent::TouchUpdate, QEventPoint::State::Updated, b);
        if (v.size() > 4 && v[4] == "up") send(QEvent::TouchEnd, QEventPoint::State::Released, b);
      });
    }
    for (auto* d : w.findChildren<QDockWidget*>())
      if (!panel.isEmpty() && d->objectName() == panel) { d->show(); d->raise(); }
    if (auto* cs = w.Collab()) {
      if (!collabHost.isEmpty()) {
        QFile pf(collabHost);
        if (pf.open(QIODevice::WriteOnly | QIODevice::Text)) pf.write(cs->Host("Ospite", "prova.3dm").toUtf8());
        if (!collabChat.isEmpty()) QTimer::singleShot(wait / 2, [cs, collabChat] { cs->SendChat(collabChat); });
      }
      if (!collabJoin.isEmpty()) {
        const QStringList p = collabJoin.split(':');
        QObject::connect(cs, &cad::collab::Session::Joined, &w, [&w, cs, collabChat, collabScript, collabTask] {
          if (!collabChat.isEmpty()) cs->SendChat(collabChat);
          if (!collabTask.isEmpty()) cs->SendAiTask(0, collabTask);
          for (const QString& l : collabScript.split(';', Qt::SkipEmptyParts)) w.Submit(l.trimmed());
        });
        if (p.size() == 3) cs->Join(p[0], static_cast<quint16>(p[1].toUInt()), p[2], "Partecipante");
      }
    }
    auto* nodeEd = w.findChild<cad::ui::NodeEditor*>();
    if (nodeEd && !nodesFile.isEmpty()) {
      nodeEd->show();
      nodeEd->LoadFile(nodesFile);
    }
    if (nodeEd && !nodesShot.isEmpty()) {
      QTimer::singleShot(wait, [&, nodeEd] {
        nodeEd->ZoomAll();
        nodeEd->grab().save(nodesShot);
        nodeEd->hide();  // la cattura della finestra principale mostra l'anteprima nelle viste
        if (shot.isEmpty()) app.quit();
      });
    }
    if (!shot.isEmpty()) {
      QTimer::singleShot(wait + 200, [&] {
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
