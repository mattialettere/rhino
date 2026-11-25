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
// Esecuzione di uno script scritto nel linguaggio dell'assistente (senza modello):
//   kurva3d-ai-test --run-script violino.kurva [-o violino.3dm]
// Verifica degli esempi per parti (ogni parte nel suo ingombro) e salvataggio in una cartella:
//   kurva3d-ai-test --examples cartella
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
#include <QRegularExpression>
#include <QTimer>
#ifdef CAD_WITH_OCCT
#include "kernel/OcctConvert.h"
#include <memory>
#endif
#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
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
// Oggetti complessi (lavoro per parti). Misura: lato maggiore dell'ingombro in qualunque
// direzione (il modello puo' metterli in piedi o sdraiati) e numero minimo di oggetti.
struct ComplexCase {
  const char* prompt;
  const char* exclude;      // esempio da togliere dalle istruzioni (prova di generalizzazione)
  double minSize, maxSize;  // lato maggiore plausibile (mm)
  int minObjects;
  // parti che rendono riconoscibile l'oggetto: gruppi separati da ";", alternative da "|"
  // (inizi di parola, cercati nei nomi e nelle descrizioni delle parti costruite)
  const char* needs;
};
const ComplexCase kComplex[] = {
    {"un violino", "", 500, 700, 5, "string|cord; neck|manic; bridge|ponticel; scroll|riccio|head|pegbox|peg"},
    {"un violino", "violino", 500, 700, 5, "string|cord; neck|manic; bridge|ponticel; scroll|riccio|head|pegbox|peg"},
    {"un violoncello", "", 1000, 1500, 5, "string|cord; neck|manic; bridge|ponticel; scroll|riccio|head|pegbox|peg"},
    {"una chitarra elettrica", "", 800, 1200, 4, "pickup|magnet; string|cord; neck|manic; head|peg|tuner|meccanic"},
    {"una bicicletta", "", 1400, 2100, 5, "wheel|ruot; frame|telai|tube; handle|manubr; pedal|crank|pedivel; saddle|seat|sell"},
    {"un'automobile sportiva", "", 3500, 5200, 4, "wheel|ruot; body|carrozz; cabin|window|glass|roof|cockpit|abitac"},
    {"un elicottero", "", 6000, 20000, 4, "rotor|blade|pale|elic; tail|coda; skid|landing|patt|gear|carrell; cabin|cockpit|fuselag|body"},
    {"una teiera", "", 150, 400, 3, "spout|beccucc; handle|manic; lid|coperch; body|corpo"},
    // oggetti senza un esempio nella libreria
    {"un drone quadricottero", "", 200, 1500, 5, "rotor|blade|propel|elic|pale; arm|bracc; body|corpo|frame|telai|hub"},
    {"un trattore agricolo", "", 2500, 6500, 5, "wheel|ruot; cabin|cab|abitac; engine|hood|motor|cofan|bonnet"},
};
struct Outcome {
  bool valid = false, plausible = false;
  std::string why;  // perche' non e' plausibile
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
            // misura sulla mesh fine: i vertici stanno sulla superficie vera (il box
            // analitico di OpenCASCADE sui cerchi razionali e' piu' largo)
            const TopoDS_Shape sh = cad::occt::GeometryToShape(*g, 0.001);
            // precisione relativa alla grandezza (0.05 mm su un aereo di 38 m darebbe una mesh enorme)
            ON_BoundingBox loose;
            g->GetBoundingBox(loose);
            const double defl = std::max(0.05, 0.0005 * (loose.IsValid() ? loose.Diagonal().Length() : 0.0));
            std::unique_ptr<ON_Mesh> m(sh.IsNull() ? nullptr : cad::occt::MeshShape(sh, defl, 0.1, false));
            ON_BoundingBox mb;
            if (m && m->VertexCount() > 0 && m->GetTightBoundingBox(mb)) {
              all.Union(mb);
              done = true;
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
// Oggetto complesso: lato maggiore dell'ingombro, numero di oggetti, parti costruite, parti che
// lo rendono riconoscibile e copie di esempi di altri oggetti; con saveAs salva il modello.
void MeasureComplex(const QStringList& script, const ComplexCase& cc, const cad::ai::Plan& plan,
                    const QStringList& skippedParts, Outcome& out, const QString& saveAs) {
  auto isSkipped = [&](const std::string& name) {
    for (const auto& s : skippedParts)
      if (s.startsWith(QString::fromStdString(name) + " (")) return true;
    return false;
  };
  const int skipped = (int)skippedParts.size();
  std::vector<std::string> lines;
  for (const auto& l : script) lines.push_back(l.toStdString());
  BenchCase any{"", 0, 1e9, 0, 1e9};
  Measure(lines, any, out);
  cad::Document3dm doc;
  std::ostringstream log;
  cad::Engine e(doc, log);
  for (const auto& l : cad::ai::ExpandScript(lines)) e.Run(l);
  const bool nonEmpty = !e.AllObjectIds(true).empty();
  const double size = nonEmpty ? std::max(out.height, out.width) : 0;
  out.width = size;
  std::vector<std::string> why;
  if (size < cc.minSize || size > cc.maxSize) why.push_back("lato " + std::to_string((int)size));
  if (out.objects < cc.minObjects) why.push_back(std::to_string(out.objects) + " oggetti");
  const int parts = (int)plan.parts.size(), built = parts - skipped;
  if (parts > 0 && built * 5 < parts * 4) why.push_back(std::to_string(built) + "/" + std::to_string(parts) + " parti");
  // parti riconoscibili: nei nomi e nelle descrizioni delle parti costruite
  std::string text;
  for (const auto& p : plan.parts)
    if (!isSkipped(p.name)) text += " " + p.name + " " + p.what;
  for (auto& c : text) c = std::isalnum((unsigned char)c) ? (char)std::tolower((unsigned char)c) : ' ';
  text = " " + text;
  std::istringstream groups(cc.needs);
  for (std::string g; std::getline(groups, g, ';');) {
    bool found = false;
    std::istringstream alts(g);
    for (std::string a; std::getline(alts, a, '|');) {
      a.erase(std::remove(a.begin(), a.end(), ' '), a.end());
      if (!a.empty() && text.find(" " + a) != std::string::npos) found = true;
    }
    if (!found) {
      g.erase(0, g.find_first_not_of(' '));
      why.push_back("manca " + g.substr(0, g.find('|')));
    }
  }
  int copied = 0;
  const std::string from = cad::ai::CopiedExample(lines, cc.prompt, cc.exclude, &copied);
  if (!from.empty()) why.push_back("copia di " + from + " (" + std::to_string(copied) + " righe)");
  out.plausible = nonEmpty && why.empty();
  out.why.clear();
  for (const auto& w : why) out.why += (out.why.empty() ? "" : ", ") + w;
  if (!saveAs.isEmpty()) doc.Save(saveAs.toStdString());
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
  int n = 0, bad = 0, measures = 0;
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
      // misure diverse: avviso (la misura dei solidi dopo le booleane non e' sempre
      // esatta); la verifica fallisce solo se capita spesso
      if (!o.plausible) {
        measures++;
        if (measures <= 30)
          std::printf("AVVISO esempio %d \"%s\": %d oggetti, altezza %d (attesa %d), pianta %d (attesa %d)\n", n, user.c_str(),
                      o.objects, int(o.height), int(c["h"].toDouble()), int(o.width), int(c["w"].toDouble()));
      }
    }
    if (!why.empty()) {
      bad++;
      if (bad <= 50) std::printf("ERRORE esempio %d \"%s\": %s\n", n, user.c_str(), why.c_str());
    }
  }
  std::printf("%d esempi, %d con errori negli script, %d con misure diverse da quelle attese\n", n, bad, measures);
  return bad == 0 && n > 0 && measures * 20 <= n ? 0 : 1;
}
// Esegue uno script dell'assistente (verifica come per le risposte del modello), riporta
// le misure e, con -o, salva il modello.
int RunScriptFile(const QString& path, const QString& outPath) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) { std::printf("non trovo %s\n", path.toUtf8().constData()); return 2; }
  const auto lines = cad::ai::ExtractScript("```kurva\n" + f.readAll().toStdString() + "\n```");
  cad::Document3dm doc;
  std::ostringstream log;
  cad::Engine engine(doc, log);
  const auto v = cad::ai::Validate(engine, lines);
  if (!v.ok) {
    std::printf("riga %d (%s): %s\n", v.line + 1, v.line >= 0 && v.line < (int)lines.size() ? lines[v.line].c_str() : "",
                v.error.c_str());
    return 1;
  }
  Outcome o;
  BenchCase any{"", 0, 1e9, 0, 1e9};
  Measure(lines, any, o);
  std::printf("script valido: %d righe, %d oggetti, altezza %.1f mm, lato maggiore in pianta %.1f mm\n", (int)lines.size(),
              o.objects, o.height, o.width);
  {  // ingombro di ogni oggetto, per nome
    std::ostringstream log2;
    cad::Document3dm d2;
    cad::Engine e2(d2, log2);
    for (const auto& l : cad::ai::ExpandScript(lines)) e2.Run(l);
    std::map<std::string, ON_BoundingBox> boxes;
    for (const auto& id : e2.AllObjectIds(true))
      if (const auto* ob = e2.Object(id))
        if (const ON_Geometry* g = ob->Geometry(nullptr)) {
          const ON_3dmObjectAttributes* at = ob->Attributes(nullptr);
          const std::string n = at && !at->m_name.IsEmpty() ? cad::ToUtf8(at->m_name) : std::string("(senza nome)");
          ON_BoundingBox b;
          if (g->GetTightBoundingBox(b)) boxes[n].Union(b);
        }
    for (const auto& [n, b] : boxes)
      std::printf("  %-14s %8.1f,%8.1f,%8.1f   %8.1f,%8.1f,%8.1f\n", n.c_str(), b.m_min.x, b.m_min.y, b.m_min.z, b.m_max.x,
                  b.m_max.y, b.m_max.z);
  }
  if (!outPath.isEmpty()) {
    std::ostringstream log2;
    cad::Engine e2(doc, log2);
    for (const auto& l : cad::ai::ExpandScript(lines))
      if (!e2.Run(l)) { std::printf("errore: %s\n%s\n", l.c_str(), log2.str().c_str()); return 1; }
    if (!doc.Save(outPath.toStdString())) { std::printf("salvataggio non riuscito\n"); return 1; }
    std::printf("salvato %s\n", outPath.toUtf8().constData());
  }
  return 0;
}
// Ogni esempio della libreria, parte per parte, come se fosse la risposta del modello.
int CheckExamples(const QString& dir) {
  int bad = 0;
  for (const auto& ex : cad::ai::Examples()) {
    cad::Document3dm doc;
    std::ostringstream log;
    cad::Engine engine(doc, log);
    cad::ai::Plan plan;
    std::string err;
    if (!cad::ai::ParsePlan(std::string("```plan\n") + ex.plan + "```", plan, &err)) {
      std::printf("%s: piano non valido: %s\n", ex.id, err.c_str());
      bad++;
      continue;
    }
    std::vector<std::string> done;
    bool ok = plan.parts.size() == ex.parts.size();
    if (!ok) std::printf("%s: %d parti nel piano, %d negli script\n", ex.id, (int)plan.parts.size(), (int)ex.parts.size());
    if (plan.features.empty()) { std::printf("%s: manca la riga # Distinctive\n", ex.id); ok = false; }
    if (const std::string pe = cad::ai::CheckPlan(plan, ex.request); !pe.empty()) {
      std::printf("%s: piano rifiutato: %s\n", ex.id, pe.c_str());
      ok = false;
    }
    for (size_t i = 0; ok && i < ex.parts.size(); i++) {
      const auto part = cad::ai::ExtractScript(std::string("```kurva\n") + ex.parts[i].script + "\n```");
      const auto v = cad::ai::CheckPart(engine, done, part, plan, (int)i, ex.request);
      if (!v.ok) {
        std::printf("%s, parte %s: %s\n", ex.id, ex.parts[i].name, cad::ai::PartRepairRequest(plan, (int)i, part, v).c_str());
        ok = false;
      }
      done.insert(done.end(), part.begin(), part.end());
    }
    if (ok && !dir.isEmpty()) {
      cad::Document3dm d2;
      std::ostringstream l2;
      cad::Engine e2(d2, l2);
      for (const auto& l : cad::ai::ExpandScript(done)) e2.Run(l);
      d2.Save((dir + "/" + ex.id + ".3dm").toStdString());
    }
    std::printf("%-12s %s (%d parti, %d righe)\n", ex.id, ok ? "ok" : "ERRORE", (int)plan.parts.size(), (int)done.size());
    bad += !ok;
  }
  return bad ? 1 : 0;
}
}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);  // in CI l'output resta anche se il programma si ferma
  QCoreApplication app(argc, argv);
  QCoreApplication::setApplicationName("Kurva3D");
  QString modelId = "qwen3-0.6b", prompt = "un cubo di lato 20 mm con un foro cilindrico di raggio 5 al centro";
  QStringList models;
  QString gguf, jsonl, runScript, outPath;
  int every = 1;
  int onlyCase = -1;  // --case k: solo la richiesta k del banco (un processo per richiesta)
  bool bench = false, grammar = true, both = false, compact = false, examples = false, complex = false;
  int extPort = 0;      // --port: llama-server (o server di prova) gia' avviato
  QString saveDir;      // --save-dir: risultati del banco complesso come .3dm
  const QStringList a = app.arguments();
  for (int i = 1; i < a.size(); i++) {
    if (a[i] == "--bench") bench = true;
    else if (a[i] == "--bench-complex") { bench = true; complex = true; }
    else if (i + 1 < a.size() && a[i] == "--port") extPort = a[++i].toInt();
    else if (i + 1 < a.size() && a[i] == "--save-dir") saveDir = a[++i];
    else if (a[i] == "--no-grammar") grammar = false;
    else if (a[i] == "--both") both = true;
    else if (a[i] == "--compact") compact = true;
    else if (i + 1 < a.size() && a[i] == "--case") onlyCase = a[++i].toInt();
    else if (i + 1 < a.size() && a[i] == "--gguf") gguf = a[++i];
    else if (i + 1 < a.size() && a[i] == "--validate-jsonl") jsonl = a[++i];
    else if (i + 1 < a.size() && a[i] == "--run-script") runScript = a[++i];
    else if (i + 1 < a.size() && a[i] == "--examples") { examples = true; outPath = a[++i]; }
    else if (i + 1 < a.size() && a[i] == "-o") outPath = a[++i];
    else if (i + 1 < a.size() && a[i] == "--every") every = std::max(1, a[++i].toInt());
    else if (i + 1 < a.size() && a[i] == "--model") modelId = a[++i];
    else if (i + 1 < a.size() && a[i] == "--models") models = a[++i].split(',', Qt::SkipEmptyParts);
    else if (i + 1 < a.size() && a[i] == "--prompt") prompt = a[++i];
  }
  if (examples) {
    cad::InitLibrary();
    const int r = CheckExamples(outPath);
    cad::ShutdownLibrary();
    return r;
  }
  if (!runScript.isEmpty()) {
    cad::InitLibrary();
    const int r = RunScriptFile(runScript, outPath);
    cad::ShutdownLibrary();
    return r;
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
  if (models.isEmpty()) models << (extPort ? QString("server:%1").arg(extPort) : modelId);
  for (const auto& id : models)
    if (gguf.isEmpty() && !extPort && !cad::ai::FindModel(id.toStdString())) { std::printf("modello sconosciuto: %s\n", id.toUtf8().constData()); return 2; }
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
    const int n = bench ? int(std::size(kBench)) : 1;  // senza --bench: una richiesta (--prompt)
    const int nb = complex ? int(std::size(kComplex)) : n;
    for (int k = 0; k < nb; k++) {
      if (onlyCase >= 0 && k != onlyCase) continue;
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
  auto promptOf = [&](const Job& j) {
    if (j.bench < 0) return prompt;
    return QString::fromUtf8(complex ? kComplex[j.bench].prompt : kBench[j.bench].prompt);
  };
  auto runJob = [&] {
    const Job& j = jobs[cur];
    const QString ex = complex && j.bench >= 0 && *kComplex[j.bench].exclude
                           ? QString(" (senza l'esempio %1)").arg(kComplex[j.bench].exclude) : QString();
    say(QString("\n=== %1 | %2%3 | grammatica %4 ===").arg(j.model, promptOf(j), ex, j.grammar ? "si" : "no"));
    clock.start();
    if (complex) {
      gen.SetTimeoutMinutes(60);  // senza GPU i modelli grandi impiegano molti minuti per il piano
      gen.RunParts(extPort ? extPort : server.Port(), promptOf(j), 2, j.grammar,
                   j.bench >= 0 ? QString::fromUtf8(kComplex[j.bench].exclude) : QString());
      return;
    }
    gen.Run(server.Port(), j.bench >= 0 ? QString::fromUtf8(kBench[j.bench].prompt) : prompt, 3, j.grammar,
            compact ? QString::fromStdString(cad::ai::CompactSystemPrompt())
                    : QString::fromStdString(cad::ai::SystemPromptFor(cad::ai::FindModel(j.model.toStdString()))));
  };
  auto startServer = [&](const QString& path) { server.Start(path, hw.threads); };
  next = [&] {
    if (cur >= jobs.size()) { result = 0; app.exit(0); return; }
    if (extPort) { runJob(); return; }
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
    if (complex) {
      const auto& plan = gen.LastPlan();
      QString pl = QString::fromStdString(plan.summary) + "\n  Distinctive: " + QString::fromStdString(plan.features);
      for (const auto& p : plan.parts)
        pl += QString("\n  PART %1 | %2").arg(QString::fromStdString(p.name), QString::fromStdString(p.what));
      say("Piano: " + pl);
      if (!gen.Skipped().isEmpty()) say("Parti lasciate fuori: " + gen.Skipped().join("; "));
      for (const auto& r : gen.RepairLog()) say("Correzione chiesta: " + r);
    }
    say("Script:\n" + script.join("\n"));
    if (!bench && script.isEmpty() && !err.isEmpty()) { say("ERRORE risposta: " + err); app.exit(1); return; }
    // la pipeline funziona se il modello risponde con dei comandi; un modello minimo
    // puo' sbagliarli, quindi l'esito della verifica e' solo riportato
    say(ok ? "Verifica: script valido" : "Verifica: script non valido (" + err + ")");
    if (ok && j.bench >= 0 && complex) {
      QString save;
      if (!saveDir.isEmpty()) {
        QString base = QString::fromUtf8(kComplex[j.bench].prompt).replace(QRegularExpression("[^A-Za-z0-9]+"), "_");
        if (*kComplex[j.bench].exclude) base += "_senza_esempio";
        QString mod = j.model;
        save = saveDir + "/" + base + "-" + mod.replace(QRegularExpression("[^A-Za-z0-9.]+"), "_") + ".3dm";
      }
      MeasureComplex(script, kComplex[j.bench], gen.LastPlan(), gen.Skipped(), o, save);
      say(QString("Risultato: %1 oggetti, lato maggiore %2 mm, %3 parti su %4 (%5)%6")
              .arg(o.objects).arg(o.width, 0, 'f', 1).arg(gen.LastPlan().parts.size() - gen.Skipped().size())
              .arg(gen.LastPlan().parts.size()).arg(o.plausible ? "plausibile" : "NON plausibile: " + QString::fromStdString(o.why))
              .arg(save.isEmpty() ? QString() : " -> " + save));
    } else if (ok && j.bench >= 0) {
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
    std::printf("\n%-20s %-5s %-60s %-6s %-5s %-6s %s\n", "modello", "gramm", "richiesta", "valido", "rich.", "secondi", "forma");
    for (size_t i = 0; i < jobs.size() && i < cur; i++) {
      const auto& j = jobs[i];
      const auto& o = results[i];
      std::printf("%-20s %-5s %-60.60s %-6s %-5d %-7.0f %s\n", j.model.toUtf8().constData(), j.grammar ? "si" : "no",
                  (promptOf(j) + (complex && *kComplex[j.bench].exclude ? " (senza esempio)" : "")).toUtf8().constData(),
                  o.valid ? "si" : "no", o.attempts, o.seconds,
                  !o.valid ? "-" : o.plausible ? "plausibile"
                  : complex ? QString::fromStdString(o.why).toUtf8().constData()
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
