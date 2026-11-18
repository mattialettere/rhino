// Assistente AI di Kurva3D (in piu' rispetto a Rhino 8): un modello linguistico
// locale (llama.cpp, file GGUF) trasforma la descrizione di un oggetto in una
// sequenza di comandi di Kurva3D. Qui c'e' la parte indipendente da Qt: catalogo
// dei modelli (solo licenze Apache 2.0 o MIT), hardware, istruzioni per il
// modello, lettura della risposta e verifica dei comandi su una copia del documento.
#pragma once
#include <string>
#include <vector>

namespace cad {
class Engine;
}

namespace cad::ai {

struct ModelInfo {
  std::string id;                   // es. "qwen3-8b"
  std::string label;                // nome mostrato
  std::string repo;                 // repository Hugging Face con i file GGUF
  std::vector<std::string> quants;  // quantizzazioni preferite, in ordine (suffisso del file)
  double sizeGB = 0;                // dimensione indicativa del file
  std::string license;              // "apache-2.0" o "mit" (verificata anche al download)
  bool listed = true;               // false: solo per le prove automatiche
  int layers = 0;                   // strati del modello (per dividere tra GPU e CPU)
};

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

// Istruzioni di sistema per il modello (linguaggio dei comandi ed esempi).
std::string SystemPrompt();
// Righe di comando di esempio usate nelle istruzioni (verificate dai test).
std::vector<std::string> ReferenceExamples();

// Comandi estratti dalla risposta del modello (blocco ```kurva o righe che
// iniziano con "_"); toglie il ragionamento <think>...</think>.
std::vector<std::string> ExtractScript(const std::string& reply);
// Traduce le estensioni per l'AI in comandi normali: "comando ... => Nome" da'
// il nome Nome agli oggetti creati dal comando.
std::vector<std::string> ExpandScript(const std::vector<std::string>& lines, std::vector<int>* origin = nullptr);

struct Validation {
  bool ok = false;
  int line = -1;        // riga (dello script del modello) che non funziona
  std::string error;    // messaggio d'errore di Kurva3D
  int created = 0;      // oggetti presenti alla fine
};
// Esegue lo script su una copia del documento dell'engine (il documento vero non cambia).
Validation Validate(Engine& e, const std::vector<std::string>& lines);
// Messaggio per chiedere al modello di correggere lo script.
std::string RepairRequest(const std::vector<std::string>& lines, const Validation& v);
// Comandi che l'assistente non puo' usare (file, annulla, ecc.).
bool CommandAllowed(const std::string& name);

}  // namespace cad::ai
