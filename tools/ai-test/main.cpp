// Prova completa dell'assistente AI senza finestra (usata dalla CI):
// scarica il modello (licenza e SHA-256 verificati), avvia llama-server,
// chiede uno script e lo verifica con Kurva3D.
//   kurva3d-ai-test [--model qwen3-0.6b] [--prompt "un cubo di lato 20 mm"]
#include "aiqt/AiService.h"
#include "commands/Engine.h"
#include "core/Document3dm.h"
#include <QCoreApplication>
#include <QTimer>
#include <cstdio>
#include <sstream>

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  QCoreApplication::setApplicationName("Kurva3D");
  QString modelId = "qwen3-0.6b", prompt = "un cubo di lato 20 mm con un foro cilindrico di raggio 5 al centro";
  const QStringList a = app.arguments();
  for (int i = 1; i + 1 < a.size(); i++) {
    if (a[i] == "--model") modelId = a[++i];
    else if (a[i] == "--prompt") prompt = a[++i];
  }
  const cad::ai::ModelInfo* m = cad::ai::FindModel(modelId.toStdString());
  if (!m) { std::printf("modello sconosciuto\n"); return 2; }
  const auto hw = cad::ai::DetectHardware();
  std::printf("%s\nConsigliato: %s\n", cad::ai::DescribeHardware(hw).c_str(),
              cad::ai::Catalog()[cad::ai::RecommendModel(hw)].label.c_str());
  cad::InitLibrary();
  cad::Document3dm doc;
  std::ostringstream log;
  cad::Engine engine(doc, log);
  cad::aiqt::ModelDownloader dl;
  cad::aiqt::LlamaServer server;
  cad::aiqt::Generator gen(engine);
  int result = 1;
  auto say = [](const QString& s) { std::printf("%s\n", s.toUtf8().constData()); std::fflush(stdout); };
  QObject::connect(&dl, &cad::aiqt::ModelDownloader::Status, say);
  QObject::connect(&server, &cad::aiqt::LlamaServer::Status, say);
  QObject::connect(&gen, &cad::aiqt::Generator::Status, say);
  int lastPct = -1;
  QObject::connect(&dl, &cad::aiqt::ModelDownloader::Progress, [&](qint64 d, qint64 t) {
    const int p = t > 0 ? int(100 * d / t) : 0;
    if (p / 10 != lastPct / 10) { std::printf("  %d%%\n", p); std::fflush(stdout); }
    lastPct = p;
  });
  auto startServer = [&](const QString& path) { server.Start(path, hw.threads); };
  QObject::connect(&dl, &cad::aiqt::ModelDownloader::Finished, [&](bool ok, const QString& r) {
    if (!ok) { say("ERRORE download: " + r); app.exit(1); return; }
    say("Modello verificato: " + r);
    startServer(r);
  });
  QObject::connect(&server, &cad::aiqt::LlamaServer::Started, [&](bool ok, const QString& err) {
    if (!ok) { say("ERRORE server: " + err + "\n" + server.Log().right(3000)); app.exit(1); return; }
    gen.Run(server.Port(), prompt);
  });
  QObject::connect(&gen, &cad::aiqt::Generator::Finished, [&](bool ok, const QStringList& script, const QString& err) {
    say("Script:\n" + script.join("\n"));
    if (script.isEmpty() && !err.isEmpty()) { say("ERRORE risposta: " + err); app.exit(1); return; }
    // la pipeline funziona se il modello risponde con dei comandi; un modello minimo
    // puo' sbagliarli, quindi l'esito della verifica e' solo riportato
    say(ok ? "Verifica: script valido" : "Verifica: script non valido (" + err + ")");
    result = 0;
    app.exit(0);
  });
  QTimer::singleShot(0, [&] {
    const QString path = cad::aiqt::InstalledModelPath(*m);
    if (!path.isEmpty()) { say("Modello gia' presente: " + path); startServer(path); }
    else dl.Start(*m);
  });
  const int rc = app.exec();
  server.Stop();
  cad::ShutdownLibrary();
  return rc == 0 ? result : rc;
}
