// Prova completa dell'assistente AI senza finestra (usata dalla CI):
// scarica il modello (licenza e SHA-256 verificati), avvia llama-server,
// chiede uno script e lo verifica con Kurva3D.
//   kurva3d-ai-test [--model qwen3-0.6b] [--prompt "un cubo di lato 20 mm"] [--no-grammar]
// Confronto tra modelli (banco di prova): per ogni modello e ogni richiesta del banco
// riporta se lo script e' valido, quanti tentativi sono serviti, il tempo e se la
// forma ottenuta e' plausibile (altezza totale, numero di oggetti).
//   kurva3d-ai-test --bench --models qwen3-1.7b,qwen2.5-coder-1.5b [--no-grammar] [--both]
// Modello addestrato (file GGUF locale, istruzioni brevi senza esempi):
//   kurva3d-ai-test --bench --gguf kurva3d-qwen3-4b.gguf [--compact]
// Verifica degli esempi di addestramento (tools/ai-train/generate_dataset.py):
//   kurva3d-ai-test --validate-jsonl train.jsonl [--every 5]   (uno ogni 5 esempi)
#include "aiqt/AiService.h"
#include "commands/Engine.h"
#include "core/Document3dm.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#ifdef CAD_WITH_OCCT
#include "kernel/OcctConvert.h"
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#endif
#include <algorithm>
#include <cstdio>
#include <functional>
#include <sstream>

namespace {
struct BenchCase {
  const char* prompt;
  double minHeight, maxHeight;  // altezza totale plausibile (mm)
  double minWidth, maxWidth;    // ingombro in pianta plausibile (lato maggiore, mm)
};
// Le richieste del banco: il tavolo con la gamba centrale e' quello dei file di esempio.
// Le ultime sono diverse dagli esempi delle istruzioni, per vedere se il modello ragiona
// o copia soltanto.
const BenchCase kBench[] = {
    {"un tavolo con gamba centrale", 600, 900, 500, 1600},
    {"un tavolo rotondo di 90 cm di diametro con una sola gamba centrale e la base a disco", 600, 900, 880, 920},
    {"una sedia con lo schienale", 750, 1100, 350, 700},
    {"una lampada da tavolo ad arco", 250, 800, 150, 700},
    {"un cubo di lato 20 mm con un foro cilindrico di raggio 5 al centro", 19, 21, 19, 21},
    {"uno sgabello rotondo a tre gambe alto 65 cm", 600, 700, 250, 600},
    {"un tavolino basso quadrato 60x60 cm con quattro gambe cilindriche", 300, 550, 590, 620},
    {"una mensola lunga 80 cm con due staffe sotto", 100, 400, 790, 810},
    {"un vaso cilindrico cavo alto 30 cm", 290, 310, 80, 300},
};
struct Outcome {
  bool valid = false, plausible = false;
  int attempts = 0, objects = 0;
  double seconds = 0, height = 0, width = 0;
};

// Esegue lo script verificato su un documento nuovo e misura il risultato.
void Measure(const std::vector<std::string>& lines, const BenchCase& bc, Outcome& out) {
  cad::Document3dm doc;
  std::ostringstream log;
  cad::Engine e(doc, log);
  for (const auto& l : cad::ai::ExpandScript(lines))
    if (!e.Run(l)) return;
  ON_BoundingBox all;
  const auto ids = e.AllObjectIds(true);
  for (const auto& id : ids)
    if (const auto* o = e.Object(id))
      if (const ON_Geometry* g = o->Geometry(nullptr)) {
        ON_BoundingBox b;  // box stretto: quello normale di superfici e polisuperfici e' piu' largo
#ifdef CAD_WITH_OCCT
        // il box di openNURBS per i solidi usa le superfici non tagliate e i punti di
        // controllo dei cerchi: OpenCASCADE misura la forma vera
        if (g->ObjectType() == ON::brep_object || g->ObjectType() == ON::extrusion_object) {
          bool done = false;
          try {
            const TopoDS_Shape sh = cad::occt::GeometryToShape(*g, 0.001);
            if (!sh.IsNull()) {
              Bnd_Box bb;
              BRepBndLib::AddOptimal(sh, bb, Standard_False, Standard_False);
              if (!bb.IsVoid()) {
                double x0, y0, z0, x1, y1, z1;
                bb.Get(x0, y0, z0, x1, y1, z1);
                all.Union(ON_BoundingBox(ON_3dPoint(x0, y0, z0), ON_3dPoint(x1, y1, z1)));
                done = true;
              }
            }
          } catch (...) {
          }
          if (done) continue;
        }
#endif
        if (g->GetTightBoundingBox(b)) all.Union(b);
      }
  out.objects = (int)ids.size();
  out.height = all.IsValid() ? all.m_max.z - all.m_min.z : 0;
  out.width = all.IsValid() ? std::max(all.m_max.x - all.m_min.x, all.m_max.y - all.m_min.y) : 0;
  out.plausible = all.IsValid() && out.height >= bc.minHeight && out.height <= bc.maxHeight &&
                  out.width >= bc.minWidth && out.width <= bc.maxWidth;
}
void Measure(const QStringList& script, const BenchCase& bc, Outcome& out) {
  std::vector<std::string> lines;
  for (const auto& l : script) lines.push_back(l.toStdString());
  Measure(lines, bc, out);
}

// Verifica ogni esempio di addestramento: lo script dev'essere valido per Kurva3D e,
// se l'esempio indica le misure attese, l'oggetto deve averle (tolleranza 1%).
int ValidateJsonl(const QString& path, int every) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) { std::printf("non trovo %s\n", path.toUtf8().constData()); return 2; }
  cad::Document3dm doc;
  std::ostringstream log;
  cad::Engine engine(doc, log);
  int n = 0, bad = 0;
  while (!f.atEnd()) {
    const QByteArray line = f.readLine().trimmed();
    if (line.isEmpty()) continue;
    static int lineNo = 0;
    if (lineNo++ % every) continue;
    n++;
    if (n % 50 == 0) { std::printf("  %d esempi verificati, %d con errori\n", n, bad); std::fflush(stdout); }
    const QJsonObject row = QJsonDocument::fromJson(line).object();
    const QJsonArray msgs = row["messages"].toArray();
    if (msgs.size() < 3) { bad++; std::printf("riga %d: messaggi mancanti\n", n); continue; }
    const std::string user = msgs[msgs.size() - 2].toObject()["content"].toString().toStdString();
    const auto script = cad::ai::ExtractScript(msgs[msgs.size() - 1].toObject()["content"].toString().toStdString());
    std::string why;
    for (const auto& l : script) {
      if (l.rfind("#", 0) == 0) continue;
      if (!cad::ai::FindSpec(l.substr(0, l.find(' ')))) { why = "comando non nella tabella: " + l; break; }
      if (!cad::ai::ArgumentsMatch(l)) { why = "argomenti non validi: " + l; break; }
    }
    if (why.empty()) {
      const auto v = cad::ai::Validate(engine, script);
      if (!v.ok) why = "riga " + std::to_string(v.line + 1) + ": " + v.error;
    }
    if (why.empty() && row.contains("check")) {
      const QJsonObject c = row["check"].toObject();
      BenchCase bc{"", 0, 1e9, 0, 1e9};
      if (c.contains("h")) { const double h = c["h"].toDouble(); bc.minHeight = h * 0.99 - 0.5; bc.maxHeight = h * 1.01 + 0.5; }
      if (c.contains("w")) { const double w = c["w"].toDouble(); bc.minWidth = w * 0.99 - 0.5; bc.maxWidth = w * 1.01 + 0.5; }
      Outcome o;
      Measure(script, bc, o);
      if (!o.plausible)
        why = "misure diverse: altezza " + std::to_string(int(o.height)) + " (attesa " + std::to_string(int(c["h"].toDouble())) +
              "), pianta " + std::to_string(int(o.width)) + " (attesa " + std::to_string(int(c["w"].toDouble())) + ")";
    }
    if (!why.empty()) {
      bad++;
      if (bad <= 50) std::printf("ERRORE esempio %d \"%s\": %s\n", n, user.c_str(), why.c_str());
    }
  }
  std::printf("%d esempi, %d validi, %d con errori\n", n, n - bad, bad);
  return bad == 0 && n > 0 ? 0 : 1;
}
}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);  // in CI l'output resta anche se il programma si ferma
  QCoreApplication app(argc, argv);
  QCoreApplication::setApplicationName("Kurva3D");
  QString modelId = "qwen3-0.6b", prompt = "un cubo di lato 20 mm con un foro cilindrico di raggio 5 al centro";
  QStringList models;
  QString gguf, jsonl;
  int every = 1;
  bool bench = false, grammar = true, both = false, compact = false;
  const QStringList a = app.arguments();
  for (int i = 1; i < a.size(); i++) {
    if (a[i] == "--bench") bench = true;
    else if (a[i] == "--no-grammar") grammar = false;
    else if (a[i] == "--both") both = true;
    else if (a[i] == "--compact") compact = true;
    else if (i + 1 < a.size() && a[i] == "--gguf") gguf = a[++i];
    else if (i + 1 < a.size() && a[i] == "--validate-jsonl") jsonl = a[++i];
    else if (i + 1 < a.size() && a[i] == "--every") every = std::max(1, a[++i].toInt());
    else if (i + 1 < a.size() && a[i] == "--model") modelId = a[++i];
    else if (i + 1 < a.size() && a[i] == "--models") models = a[++i].split(',', Qt::SkipEmptyParts);
    else if (i + 1 < a.size() && a[i] == "--prompt") prompt = a[++i];
  }
  if (!jsonl.isEmpty()) {
    cad::InitLibrary();
    const int r = ValidateJsonl(jsonl, every);
    cad::ShutdownLibrary();
    return r;
  }
  if (!gguf.isEmpty()) {
    if (!QFileInfo::exists(gguf)) { std::printf("non trovo %s\n", gguf.toUtf8().constData()); return 2; }
    models = QStringList{"gguf:" + QFileInfo(gguf).fileName()};
  }
  if (models.isEmpty()) models << modelId;
  for (const auto& id : models)
    if (gguf.isEmpty() && !cad::ai::FindModel(id.toStdString())) { std::printf("modello sconosciuto: %s\n", id.toUtf8().constData()); return 2; }
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
    gen.Run(server.Port(), j.bench >= 0 ? QString::fromUtf8(kBench[j.bench].prompt) : prompt, 3, j.grammar,
            compact ? QString::fromStdString(cad::ai::CompactSystemPrompt()) : QString());
  };
  auto startServer = [&](const QString& path) { server.Start(path, hw.threads); };
  next = [&] {
    if (cur >= jobs.size()) { result = 0; app.exit(0); return; }
    const QString m = jobs[cur].model;
    if (m == loaded && server.Ready()) { runJob(); return; }
    loaded = m;
    server.Stop();
    if (!gguf.isEmpty()) { say("Modello locale: " + gguf); startServer(gguf); return; }
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
      say(QString("Risultato: %1 oggetti, altezza %2 mm, lato in pianta %3 mm (%4)")
              .arg(o.objects).arg(o.height, 0, 'f', 1).arg(o.width, 0, 'f', 1).arg(o.plausible ? "plausibile" : "NON plausibile"));
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
                  !o.valid ? "-" : o.plausible ? "plausibile"
                  : QString("altezza %1, pianta %2").arg(o.height, 0, 'f', 0).arg(o.width, 0, 'f', 0).toUtf8().constData());
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
