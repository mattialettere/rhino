// Assistente AI di Kurva3D (in piu' rispetto a Rhino 8): un modello linguistico
// locale (llama.cpp, file GGUF) trasforma la descrizione di un oggetto in una
// sequenza di comandi di Kurva3D. Qui c'e' la parte indipendente da Qt: catalogo
// dei modelli (solo licenze Apache 2.0 o MIT), hardware, istruzioni per il
// modello, lettura della risposta e verifica dei comandi su una copia del documento.
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace cad {
class Engine;
}

namespace cad::ai {

struct ModelInfo {
  std::string id;                   // es. "qwen3-8b"
  std::string label;                // nome mostrato
  std::string repo;                 // repository Hugging Face con i file GGUF, oppure link diretto "https://..." al file
  std::vector<std::string> quants;  // quantizzazioni preferite, in ordine (suffisso del file)
  double sizeGB = 0;                // dimensione indicativa del file
  std::string license;              // "apache-2.0" o "mit" (verificata anche al download)
  bool listed = true;               // false: solo per le prove automatiche
  int layers = 0;                   // strati del modello (per dividere tra GPU e CPU)
  bool preferred = true;            // famiglia da cui si sceglie il modello consigliato
  bool finetuned = false;           // addestrato su Kurva3D: istruzioni brevi, senza esempi
  long long bytes = 0;              // link diretto: dimensione esatta del file (verifica)
  std::string sha256;               // link diretto: impronta del file (vuota: solo dimensione e formato)
};
// Il modello si scarica da un link diretto (non da Hugging Face)?
inline bool IsDirectUrl(const ModelInfo& m) { return m.repo.rfind("https://", 0) == 0; }

// Modelli consentiti (licenza Apache 2.0 o MIT), dal piu' piccolo al piu' grande.
const std::vector<ModelInfo>& Catalog();
const ModelInfo* FindModel(const std::string& id);
// Licenze accettate per i modelli (il programma e' venduto).
bool LicenseAllowed(const std::string& license);

struct GpuInfo {
  std::string name;
  double vramGB = 0;  // memoria dedicata
  bool discrete = false;
};
struct HardwareInfo {
  double ramGB = 0;
  int threads = 1;
  std::vector<GpuInfo> gpus;
  bool unifiedMemory = false;  // Apple Silicon: la GPU usa la RAM
};

HardwareInfo DetectHardware();
std::string DescribeHardware(const HardwareInfo& hw);
// Memoria utilizzabile dal modello (GB) e se conviene la GPU.
double ModelBudgetGB(const HardwareInfo& hw, bool* useGpu = nullptr);
// Indice nel catalogo del modello consigliato per l'hardware.
int RecommendModel(const HardwareInfo& hw);
// Il modello entra nella memoria disponibile?
bool ModelFits(const ModelInfo& m, const HardwareInfo& hw);

// Comandi che l'assistente conosce, con la forma dei loro argomenti. Da questa
// tabella vengono l'elenco nelle istruzioni, la grammatica che vincola il modello
// e il controllo degli argomenti. Codici degli argomenti (separati da spazi):
//   P punto x,y[,z]   P+ uno o piu' punti   N numero   I intero   S testo tra virgolette
//   W+ uno o piu' nomi   C colore r,g,b   Chiave=N|I|S opzione con valore
//   a|b una delle parole indicate   altro: parola fissa (es. _Solid=_Yes)
struct CommandSpec {
  const char* name;
  const char* args;
  bool named;           // ammette " => Nome"
  bool creates;         // elencato tra i comandi che creano oggetti
  const char* example;  // riga d'esempio (verificata dai test)
  const char* note;     // spiegazione per il modello
};
const std::vector<CommandSpec>& CommandSpecs();
const CommandSpec* FindSpec(const std::string& commandName);
// Il comando agisce sugli oggetti selezionati e accetta le destinazioni "@Nome" alla fine?
bool AcceptsTargets(const CommandSpec& c);
// La riga rispetta la forma degli argomenti del suo comando? (comandi fuori tabella: true)
bool ArgumentsMatch(const std::string& line);
// Grammatica GBNF di llama.cpp: la risposta puo' essere solo un blocco ```kurva
// con righe di commento "# ..." e comandi della tabella scritti nella forma giusta.
std::string Grammar();

// Comando composto per l'AI (forme per oggetti complessi: _SymCurve, _SectionLoft, ...)?
bool IsShapeHelper(const CommandSpec& c);
// Regole e comandi del linguaggio (helpers: anche forme composte, @Nome e consigli per oggetti complessi).
std::string CommandReference(bool helpers);
// Istruzioni di sistema per il modello (linguaggio dei comandi ed esempi).
std::string SystemPrompt();
// Istruzioni senza gli script di esempio: per i modelli addestrati su Kurva3D
// (uguali a tools/ai-train/system_prompt.txt, usato per l'addestramento).
std::string CompactSystemPrompt();
// Istruzioni adatte al modello (brevi per i modelli addestrati).
std::string SystemPromptFor(const ModelInfo* m);
// Righe di comando di esempio usate nelle istruzioni (verificate dai test).
std::vector<std::string> ReferenceExamples();

// Comandi estratti dalla risposta del modello (blocco ```kurva o righe che
// iniziano con "_"); toglie il ragionamento <think>...</think>.
std::vector<std::string> ExtractScript(const std::string& reply);
// Traduce le estensioni per l'AI in comandi normali di Kurva3D:
//  - "comando ... => Nome" da' il nome Nome agli oggetti creati dal comando;
//  - "comando ... @A @B" seleziona prima gli oggetti chiamati A e B;
//  - forme composte: _SymCurve (contorno simmetrico chiuso), _SectionLoft (solido per
//    sezioni ellittiche), _Lathe (solido di rivoluzione da coppie raggio,altezza),
//    _PipeThrough (tubo lungo una curva per punti), _Wheel (ruota), _Blades (pale).
std::vector<std::string> ExpandScript(const std::vector<std::string>& lines, std::vector<int>* origin = nullptr);

struct Validation {
  bool ok = false;
  int line = -1;        // riga (dello script del modello) che non funziona
  std::string error;    // messaggio d'errore di Kurva3D
  int created = 0;      // oggetti presenti alla fine
};
// Esegue lo script su una copia del documento dell'engine (il documento vero non cambia).
// after: chiamata con l'engine della copia se lo script e' valido (per misurare il risultato).
Validation Validate(Engine& e, const std::vector<std::string>& lines, const std::function<void(Engine&)>& after = {});
// Spiegazione in inglese dei messaggi d'errore piu' comuni di Kurva3D (vuota se non c'e').
std::string ErrorHint(const std::string& error);
// Messaggio per chiedere al modello di correggere lo script.
std::string RepairRequest(const std::vector<std::string>& lines, const Validation& v);
// Comandi che l'assistente non puo' usare (file, annulla, ecc.).
bool CommandAllowed(const std::string& name);

// --- Oggetti complessi: piano in parti ---------------------------------------------------
// Il modello scrive prima un piano (blocco ```plan con una riga "PART Nome | cosa e come | x0,y0,z0 x1,y1,z1"
// per parte) e poi i comandi di una parte alla volta; ogni parte viene verificata subito,
// anche nella posizione (deve stare nel suo ingombro del piano).
struct PlanPart {
  std::string name, what;
  double box[6] = {0, 0, 0, 0, 0, 0};  // minimo x,y,z e massimo x,y,z
};
struct Plan {
  std::string summary;   // riga "# ..." iniziale
  std::string features;  // riga "# Distinctive: ..." (cio' che distingue l'oggetto da quelli simili)
  std::vector<PlanPart> parts;
};
// Legge il piano dalla risposta; error spiega cosa non va (in inglese, per il modello).
bool ParsePlan(const std::string& reply, Plan& plan, std::string* error = nullptr);
// Grammatica GBNF del piano.
std::string PlanGrammar();
// Istruzioni per lavorare per parti, con gli esempi piu' vicini alla richiesta.
// exclude: id di un esempio da non usare (per le prove di generalizzazione).
std::string PlanSystemPrompt(const std::string& request, const std::string& exclude = {});
// Primo messaggio (richiesta + piani d'esempio + "scrivi il piano") e richiesta dei comandi
// della parte i (con le tecniche delle parti simili degli esempi).
std::string PlanRequest(const std::string& request, const std::string& exclude = {});
std::string PartRequest(const Plan& plan, int i, const std::string& request = {}, const std::string& exclude = {});
// Controlli del piano oltre alla forma: non deve copiare il piano di un altro oggetto della
// libreria e ogni parte deve toccarne un'altra. Vuoto se va bene, altrimenti il motivo (per il modello).
std::string CheckPlan(const Plan& plan, const std::string& request, const std::string& exclude = {});
// L'esempio descrive lo stesso oggetto della richiesta ("un violino" e l'esempio violino:
// si puo' seguire; "un violoncello" o "una chitarra elettrica" no).
bool SameObject(const std::string& request, const char* exampleId);
// Righe che creano geometria copiate da un esempio di un altro oggetto: id dell'esempio
// copiato (almeno meta' delle righe uguali), altrimenti vuoto.
std::string CopiedExample(const std::vector<std::string>& lines, const std::string& request,
                          const std::string& exclude = {}, int* copied = nullptr);
// Verifica la parte i: lo script delle parti precedenti (gia' valido) seguito da quello
// della parte. In caso di errore v.line e' la riga dello script della parte.
// request/exclude: riservati (le copie degli esempi si riconoscono nel piano, con CheckPlan).
Validation CheckPart(Engine& e, const std::vector<std::string>& before, const std::vector<std::string>& part,
                     const Plan& plan, int i, const std::string& request = {}, const std::string& exclude = {});
std::string PartRepairRequest(const Plan& plan, int i, const std::vector<std::string>& part, const Validation& v);

// Esempi completi (piano + parti), verificati dai test; scelti in base alla richiesta.
struct ExamplePart {
  const char* name;
  const char* script;
};
struct Example {
  const char* id;        // es. "violino"
  const char* keywords;  // parole (o inizi di parole) che lo richiamano, separate da spazi
  const char* request;
  const char* plan;      // contenuto del blocco ```plan
  std::vector<ExamplePart> parts;
};
const std::vector<Example>& Examples();
std::vector<const Example*> SelectExamples(const std::string& request, int count, const std::string& exclude = {});
// Tutte le righe di comando dell'esempio, parte dopo parte.
std::vector<std::string> ExampleScript(const Example& ex);
// Testo dell'esempio come appare nelle istruzioni (piano e parti).
std::string FormatExample(const Example& ex);

}  // namespace cad::ai
