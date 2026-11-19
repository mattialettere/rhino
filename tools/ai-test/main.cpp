// Prova completa dell'assistente AI senza finestra (usata dalla CI):
// scarica il modello (licenza e SHA-256 verificati), avvia llama-server,
// chiede uno script e lo verifica con Kurva3D.
//   kurva3d-ai-test [--model qwen3-0.6b] [--prompt "un cubo di lato 20 mm"] [--no-grammar]
// Confronto tra modelli (banco di prova): per ogni modello e ogni richiesta del banco
// riporta se lo script e' valido, quanti tentativi sono serviti, il tempo e se la
// forma ottenuta e' plausibile (altezza totale, numero di oggetti).
//   kurva3d-ai-test --bench --models qwen3-1.7b,qwen2.5-coder-1.5b [--no-grammar] [--both]
#include "aiqt/AiService.h"
#include "commands/Engine.h"
#include "core/Document3dm.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTimer>
#include <cstdio>
#include <functional>
#include <sstream>

namespace {
struct BenchCase {
  const char* prompt;
  double minHeight, maxHeight;  // altezza totale plausibile (mm)
};
// Le richieste del banco: il tavolo con la gamba centrale e' quello dei file di esempio.
const BenchCase kBench[] = {
    {"un tavolo con gamba centrale", 600, 900},
    {"un tavolo rotondo di 90 cm di diametro con una sola gamba centrale e la base a disco", 600, 900},
    {"una sedia con lo schienale", 750, 1100},
    {"una lampada da tavolo ad arco", 250, 800},
    {"un cubo di lato 20 mm con un foro cilindrico di raggio 5 al centro", 19, 21},
};

struct Outcome {
  bool valid = false, plausible = false;
  int attempts = 0, objects = 0;
  double seconds = 0, height = 0;
};

// Esegue lo script verificato su un documento nuovo e misura il risultato.
void Measure(const QStringList& script, const BenchCase& bc, Outcome& out) {
  std::vector<std::string> lines;
  for (const auto& l : script) lines.push_back(l.toStdString());
  cad::Document3dm doc;
  std::ostringstream log;
  cad::Engine e(doc, log);
  for (const auto& l : cad::ai::ExpandScript(lines))
    if (!e.Run(l)) return;
  ON_BoundingBox all;
  const auto ids = e.AllObjectIds(true);
  for (const auto& id : ids)
    if (const auto* o = e.Object(id))
      if (const ON_Geometry* g = o->Geometry(nullptr)) all.Union(g->BoundingBox());
  out.objects = (int)ids.size();
  out.height = all.IsValid() ? all.m_max.z - all.m_min.z : 0;
  out.plausible = all.IsValid() && out.height >= bc.minHeight && out.height <= bc.maxHeight;
}
}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  QCoreApplication::setApplicationName("Kurva3D");
  QString modelId = "qwen3-0.6b", prompt = "un cubo di lato 20 mm con un foro cilindrico di raggio 5 al centro";
  QStringList models;
  bool bench = false, grammar = true, both = false;
  const QStringList a = app.arguments();
  for (int i = 1; i < a.size(); i++) {
    if (a[i] == "--bench") bench = true;
    else if (a[i] == "--no-grammar") grammar = false;
    else if (a[i] == "--both") both = true;
    else if (i + 1 < a.size() && a[i] == "--model") modelId = a[++i];
    else if (i + 1 < a.size() && a[i] == "--models") models = a[++i].split(',', Qt::SkipEmptyParts);
    else if (i + 1 < a.size() && a[i] == "--prompt") prompt = a[++i];
  }
  if (models.isEmpty()) models << modelId;
  for (const auto& id : models)
    if (!cad::ai::FindModel(id.toStdString())) { std::printf("modello sconosciuto: %s\n", id.toUtf8().constData()); return 2; }
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

  // piano delle prove: (modello, richiesta, grammatica)
  struct Job {
    QString model;
    int bench;  // indice in kBench, -1: --prompt
    bool grammar;
  };
  std::vector<Job> jobs;
  for (const auto& m : models) {
    const int n = bench ? int(std::size(kBench)) : 1;
    for (int k = 0; k < n; k++) {
      if (both) { jobs.push_back({m, bench ? k : -1, true}); jobs.push_back({m, bench ? k : -1, false}); }
      else jobs.push_back({m, bench ? k : -1, grammar});
    }
  }
  std::vector<Outcome> results(jobs.size());
  size_t cur = 0;
  int result = 1;
  QElapsedTimer clock;
  QString loaded;

  std::function<void()> next;
  auto runJob = [&] {
    const Job& j = jobs[cur];
    say(QString("\n=== %1 | %2 | grammatica %3 ===")
            .arg(j.model, j.bench >= 0 ? QString::fromUtf8(kBench[j.bench].prompt) : prompt, j.grammar ? "si" : "no"));
    clock.start();
    gen.Run(server.Port(), j.bench >= 0 ? QString::fromUtf8(kBench[j.bench].prompt) : prompt, 3, j.grammar);
  };
  auto startServer = [&](const QString& path) { server.Start(path, hw.threads); };
  next = [&] {
    if (cur >= jobs.size()) { result = 0; app.exit(0); return; }
    const QString m = jobs[cur].model;
    if (m == loaded && server.Ready()) { runJob(); return; }
    loaded = m;
    server.Stop();
    const cad::ai::ModelInfo* mi = cad::ai::FindModel(m.toStdString());
    const QString path = cad::aiqt::InstalledModelPath(*mi);
    if (!path.isEmpty()) { say("Modello gia' presente: " + path); startServer(path); }
    else dl.Start(*mi);
  };
  QObject::connect(&dl, &cad::aiqt::ModelDownloader::Finished, [&](bool ok, const QString& r) {
    if (!ok) { say("ERRORE download: " + r); app.exit(1); return; }
    say("Modello verificato: " + r);
    startServer(r);
  });
  QObject::connect(&server, &cad::aiqt::LlamaServer::Started, [&](bool ok, const QString& err) {
    if (!ok) { say("ERRORE server: " + err + "\n" + server.Log().right(3000)); app.exit(1); return; }
    runJob();
  });
  QObject::connect(&gen, &cad::aiqt::Generator::Finished, [&](bool ok, const QStringList& script, const QString& err) {
    const Job& j = jobs[cur];
    Outcome& o = results[cur];
    o.seconds = clock.elapsed() / 1000.0;
    o.attempts = gen.Attempts();
    o.valid = ok;
    say("Script:\n" + script.join("\n"));
    if (!bench && script.isEmpty() && !err.isEmpty()) { say("ERRORE risposta: " + err); app.exit(1); return; }
    // la pipeline funziona se il modello risponde con dei comandi; un modello minimo
    // puo' sbagliarli, quindi l'esito della verifica e' solo riportato
    say(ok ? "Verifica: script valido" : "Verifica: script non valido (" + err + ")");
    if (ok && j.bench >= 0) {
      Measure(script, kBench[j.bench], o);
      say(QString("Risultato: %1 oggetti, altezza %2 mm (%3)")
              .arg(o.objects).arg(o.height, 0, 'f', 1).arg(o.plausible ? "plausibile" : "NON plausibile"));
    }
    cur++;
    QTimer::singleShot(0, next);
  });
  QTimer::singleShot(0, next);
  const int rc = app.exec();
  server.Stop();

  if (bench) {
    std::printf("\n%-20s %-5s %-60s %-6s %-5s %-6s %s\n", "modello", "gramm", "richiesta", "valido", "tent.", "secondi", "forma");
    for (size_t i = 0; i < jobs.size() && i < cur; i++) {
      const auto& j = jobs[i];
      const auto& o = results[i];
      std::printf("%-20s %-5s %-60.60s %-6s %-5d %-7.0f %s\n", j.model.toUtf8().constData(), j.grammar ? "si" : "no",
                  kBench[j.bench].prompt, o.valid ? "si" : "no", o.attempts, o.seconds,
                  !o.valid ? "-" : o.plausible ? "plausibile" : QString("altezza %1").arg(o.height, 0, 'f', 0).toUtf8().constData());
    }
    // riassunto per modello e grammatica
    std::printf("\nRiassunto (valido / plausibile / tentativi medi / secondi medi):\n");
    for (const auto& m : models)
      for (int g = 1; g >= 0; g--) {
        int n = 0, valid = 0, plaus = 0, att = 0;
        double sec = 0;
        for (size_t i = 0; i < jobs.size() && i < cur; i++)
          if (jobs[i].model == m && jobs[i].grammar == (g == 1)) {
            n++; valid += results[i].valid; plaus += results[i].plausible; att += results[i].attempts; sec += results[i].seconds;
          }
        if (n) std::printf("  %-20s grammatica %-2s  %d/%d  %d/%d  %.1f  %.0f\n", m.toUtf8().constData(), g ? "si" : "no",
                           valid, n, plaus, n, double(att) / n, sec / n);
      }
  }
  cad::ShutdownLibrary();
  return rc == 0 ? result : rc;
}
