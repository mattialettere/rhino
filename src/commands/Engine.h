#pragma once
// Motore dei comandi: interpreta la sintassi delle macro/script della riga
// di comando di Rhino 8 (es. "_Line 0,0,0 10,0,0", "_SelAll _Move 0,0 5,0").
//
// Regole di sintassi (come nelle macro di Rhino):
//  - i nomi di comando e di opzione possono avere i prefissi "_", "-", "!";
//    il confronto ignora maiuscole/minuscole;
//  - spazio e a capo separano gli input; "_Enter" (o la fine riga) conclude
//    i comandi che accettano un numero variabile di punti;
//  - punti: "x,y" o "x,y,z"; relativi all'ultimo punto: "r5,0" o "@5,0";
//    polari: "10<45" (dall'origine) o "r10<45" (dall'ultimo punto);
//  - opzioni: "Nome=Valore" (es. "_Cap=_Yes", "NumSides=6") oppure
//    l'opzione da sola quando e' un interruttore (es. "_Close").
#include "core/Document3dm.h"
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <vector>

namespace cad {

struct UuidLess {
  bool operator()(const ON_UUID& a, const ON_UUID& b) const { return ON_UuidCompare(a, b) < 0; }
};

class TokenStream {
public:
  // appendFinalEnter=false nella modalita' interattiva: la fine del testo
  // significa "aspetto altro input", non "Invio".
  explicit TokenStream(const std::string& script, bool appendFinalEnter = true);
  size_t Pos() const { return m_pos; }
  std::string Join(size_t from, size_t to) const;  // token [from, to) come testo da rieseguire
  // Ricompone i token da "from" in poi (per tenere in sospeso un comando).
  std::string JoinFrom(size_t from) const;
  bool AtEnd() const { return m_pos >= m_tokens.size(); }
  const std::string& Peek() const;
  std::string Next();
  void Back() { if (m_pos > 0) m_pos--; }
private:
  std::vector<std::string> m_tokens;
  size_t m_pos = 0;
};

// Nome normalizzato: senza prefissi "_-!'" e in minuscolo.
std::string NormalizeName(const std::string& token);
bool IsEnter(const std::string& token);

class Engine;
using CommandFn = std::function<bool(Engine&, TokenStream&)>;

struct CommandInfo {
  std::string name;         // nome inglese di Rhino 8 (es. "Line")
  std::string category;     // es. "Curve", "Solidi"
  std::string description;  // descrizione italiana breve
  CommandFn fn;
  bool modifiesModel = true;
  // Opzioni mostrate (e cliccabili) sulla riga di comando: "Nome=A|B" o "Nome=#numero"
  std::vector<std::string> options;
};

class Engine {
public:
  Engine(Document3dm& doc, std::ostream& out);

  // Esegue uno script (una o piu' righe). Si ferma al primo errore.
  bool Run(const std::string& script);
  // Modalita' interattiva (interfaccia grafica): un comando a cui mancano
  // punti o valori non e' un errore ma resta "in attesa" di altro input.
  enum class Result { Done, NeedsInput, Error };
  Result RunInteractive(const std::string& script, std::string* pending);
  bool interactive = false;
  // Se un comando fallisce dopo aver modificato il modello (es. errore in uno script Python)
  // le modifiche gia' fatte restano, come in Rhino.
  bool keepChangesOnFailure = false;
  // Storia di costruzione (History): Record = i comandi che la supportano registrano
  // ingressi e parametri; Update = se un ingresso cambia gli oggetti creati si ricalcolano;
  // Lock = gli oggetti figli non si possono modificare a mano senza interrompere la storia.
  bool recordHistory = false;
  bool historyUpdate = true;
  bool historyLock = false;
  static bool SupportsHistory(const std::string& commandName);
  // Modifica del modello fatta dall'interfaccia (es. Bake dell'editor a nodi) come un comando:
  // un passo di annullamento e gli oggetti creati diventano "ultimi creati".
  bool Transaction(const std::string& name, const std::function<bool()>& fn);
  // Ricalcola gli oggetti con storia i cui ingressi sono cambiati (dopo ogni comando).
  void UpdateHistory();
  bool NeedsInput() const { return m_needsInput; }
  // Che tipo di input aspetta il comando in sospeso:
  //  Point   = un punto (clic sul piano di costruzione, con osnap)
  //  Pick    = un punto SU un oggetto (clic su curva/superficie: Trim, Extend, Fillet, Shell...)
  //  Objects = oggetti da selezionare, poi Invio (come "Seleziona oggetti" di Rhino)
  //  Value   = numero o testo
  enum class InputKind { Point, Pick, Objects, Value };
  InputKind PendingInputKind() const { return m_inputKind; }
  const std::string& Prompt() const { return m_prompt; }
  const std::string& LastCommandName() const { return m_lastCommandName; }
  // Vero finche' ci sono altri elementi di una lista (punti di Polyline...).
  bool MoreInput(TokenStream& ts);
  // Esegue un solo comando gia' letto dallo stream.
  bool RunCommand(const std::string& name, TokenStream& ts);

  const std::map<std::string, CommandInfo>& Commands() const { return m_commands; }
  // Comando per nome inglese o italiano (alias); nullptr se sconosciuto.
  const CommandInfo* FindCommand(const std::string& name) const;
  bool IsCommand(const std::string& name) const { return FindCommand(name) != nullptr; }
  void SetCommandOptions(const std::string& name, std::vector<std::string> options) {
    auto it = m_commands.find(NormalizeName(name));
    if (it != m_commands.end()) it->second.options = std::move(options);
  }
  // Nomi italiani (normalizzati) -> nome normalizzato del comando inglese
  const std::map<std::string, std::string>& Aliases() const { return m_aliases; }
  Document3dm& Document() { return m_doc; }
  void Register(CommandInfo info);

  // --- input per i comandi ---
  std::optional<ON_3dPoint> ReadPoint(TokenStream& ts, const char* prompt);
  // Come ReadPoint, ma il punto indica un oggetto (l'interfaccia lo prende sull'oggetto cliccato).
  std::optional<ON_3dPoint> ReadPick(TokenStream& ts, const char* prompt);
  std::optional<double> ReadNumber(TokenStream& ts, const char* prompt);
  std::optional<int> ReadInt(TokenStream& ts, const char* prompt);
  std::optional<std::string> ReadString(TokenStream& ts, const char* prompt);
  // Legge eventuali opzioni "Nome=Valore" o interruttori conosciuti.
  std::map<std::string, std::string> ReadOptions(TokenStream& ts,
                                                 const std::set<std::string>& switches = {});
  // Consuma un "_Enter" se presente.
  void SkipEnter(TokenStream& ts);

  // --- documento ---
  Document3dm& Doc() { return m_doc; }
  ONX_Model& Model() { return m_doc.Model(); }
  double Tolerance() const;
  ON_UUID AddObject(ON_Object* geometry, const ON_3dmObjectAttributes* attrs = nullptr);
  bool DeleteObject(const ON_UUID& id);
  // Sostituisce la geometria conservando attributi e id dell'oggetto.
  ON_UUID ReplaceObject(const ON_UUID& id, ON_Object* geometry);
  const ON_ModelGeometryComponent* Object(const ON_UUID& id) const;
  ON_Geometry* EditableGeometry(const ON_UUID& id);
  ON_3dmObjectAttributes* EditableAttributes(const ON_UUID& id);
  // Oggetti dello spazio attivo: il modello, o la tavola attiva (activeLayoutId).
  std::vector<ON_UUID> AllObjectIds(bool includeHidden = false) const;
  // Oggetti di uno spazio: nil = modello, altrimenti l'id della tavola.
  std::vector<ON_UUID> ObjectIdsInSpace(const ON_UUID& layoutId, bool includeHidden = false) const;

  // --- selezione ---
  // Oggetti selezionati ancora esistenti, nell'ordine in cui sono stati
  // selezionati (conta per BooleanDifference, Split, Loft, Sweep1).
  std::vector<ON_UUID> Selection() const;
  void Select(const ON_UUID& id) { if (m_selection.insert(id).second) m_selectionOrder.push_back(id); }
  void Deselect(const ON_UUID& id);
  bool IsSelected(const ON_UUID& id) const { return m_selection.count(id) > 0; }
  void ClearSelection() {
    if (!m_selectionOrder.empty()) m_prevSelection = m_selectionOrder;
    m_selection.clear();
    m_selectionOrder.clear();
  }
  // Ultima selezione non vuota prima dell'attuale (SelPrev).
  const std::vector<ON_UUID>& PreviousSelection() const { return m_prevSelection; }
  // Oggetti creati dall'ultimo comando che ha creato qualcosa (per SelLast).
  const std::vector<ON_UUID>& LastCreated() const { return m_previousCreated; }
  // Oggetti selezionati. Se non ce ne sono: in modalita' interattiva il comando
  // resta in attesa che l'utente li selezioni (prompt), altrimenti errore.
  std::vector<ON_UUID> RequireSelection(const char* command, const char* prompt = nullptr);

  // --- annulla ---
  bool Undo();
  bool Redo();
  // Gruppi di comandi annullabili in un passo (assistente AI): profondita' attuale,
  // fusione dei passi successivi in uno solo, ripristino dello stato a quella profondita'.
  size_t UndoDepth() const { return m_undo.size(); }
  void SquashUndo(size_t depth);
  bool RestoreUndo(size_t depth);
  // Collaborazione: modifica ricevuta da un altro computer. Si applica al modello e si
  // ricorda, cosi' Annulla/Ripeti dei propri comandi non cancellano il lavoro degli altri.
  bool ApplyRemote(const std::function<bool(ONX_Model&)>& change);
  // Nuovo documento o fine della sessione: si dimenticano le modifiche ricevute e la cronologia.
  void ClearUndo() { m_undo.clear(); m_redo.clear(); m_remote.clear(); }
  // Dopo annulla/apertura: se la tavola attiva non esiste piu' si torna al modello.
  void CheckActiveLayout();

  // --- messaggi ---
  std::ostream& Out() { return m_out; }
  bool Fail(const std::string& message);
  const std::string& LastError() const { return m_lastError; }

  std::string currentPath;   // percorso del file aperto/salvato
  // Worksession: file collegati come riferimento (solo visualizzazione e osnap)
  struct WorksessionFile {
    std::string path;
    std::shared_ptr<Document3dm> doc;  // nullptr se il file non si legge
    int objectCount = 0;
  };
  std::vector<WorksessionFile> worksession;
  // Allinea i file caricati all'elenco salvato nel documento (dopo Apri, Annulla, Worksession)
  void SyncWorksession();
  // Oggetti con i punti di controllo visibili (PointsOn / PointsOff)
  std::set<ON_UUID, UuidLess> pointsOn;
  // EditPtOn: curve che mostrano i punti di modifica (sulla curva) invece dei punti di controllo
  std::set<ON_UUID, UuidLess> editPointsOn;
  // BlockEdit in corso: definizione modificata, trasformazione dell'istanza, copie in modifica
  ON_UUID blockEditDef = ON_nil_uuid;
  ON_Xform blockEditXform = ON_Xform::IdentityTransformation;
  std::vector<ON_UUID> blockEditObjects;
  // Servizi forniti dall'interfaccia grafica (Qt): immagini in toni di grigio (0..1)
  // e contorni dei caratteri (ogni contorno: tratti di 2 punti = linea, 4 = Bezier cubica).
  std::function<bool(const std::string& path, int& w, int& h, std::vector<double>& gray)> imageLoader;
  std::function<bool(const std::string& text, const std::string& font, double height,
                     std::vector<std::vector<std::vector<ON_3dPoint>>>& contours)> textOutliner;
  // Viste (dall'interfaccia): vista attiva come ON_3dmView (camera, CPlane, nome), ripristino
  // di una vista salvata e CPlane della vista attiva (nullptr = quello predefinito della vista).
  std::function<bool(ON_3dmView&)> getActiveView;
  std::function<bool(const ON_3dmView&)> setActiveView;
  std::function<bool(const ON_Plane*)> setActiveCPlane;
  std::function<void()> beforeSave;         // l'interfaccia scrive le viste correnti nel modello
  ON_UUID activeViewportId = ON_nil_uuid;   // vista attiva (piani di sezione)
  // Tavola attiva (Layout): nil = spazio del modello. I nuovi oggetti vanno nello spazio attivo.
  ON_UUID activeLayoutId = ON_nil_uuid;
  std::function<void()> layoutsChanged;
  // Finestra di rendering progressivo (Render senza File=)
  std::function<bool(const ON_3dmView& view, int width, int height, int samples)> renderWindow;
  std::function<bool(const std::string& mode)> setDisplayMode;  // nome normalizzato (wireframe, shaded, ...)
  // ScriptEditor: apre l'editor di script Python (eventualmente con un file)
  std::function<bool(const std::string& file)> showScriptEditor;
  std::function<bool()> showNodeEditor;  // NodeEditor: editor visuale a nodi
  std::function<bool()> showCollaboration;  // Collaborate: pannello della collaborazione in rete locale
  // Stampa su stampante (dalla finestra): pagine gia' composte
  std::function<bool(const std::vector<struct PageDrawing>&)> printPages;
  std::vector<ON_UUID> viewportIds;         // tutte le viste della finestra
  // ViewCaptureToFile/ToClipboard: immagine della vista attiva (percorso vuoto = appunti);
  // larghezza e altezza 0 = come la vista
  std::function<bool(const std::string& path, int width, int height, bool grid)> viewCapture;
  // CageEdit: geometria originale degli oggetti catturati (per id) e gabbia
  // prima dell'ultima modifica (serve se l'originale non e' in memoria)
  std::map<ON_UUID, std::shared_ptr<ON_Geometry>, UuidLess> cageOriginals;
  std::shared_ptr<ON_MorphControl> cageBefore;
  // SubDDisplayToggle: SubD mostrati come gabbia piatta invece che lisci
  bool subdFlat = false;
  ON_3dPoint lastPoint = ON_3dPoint::Origin;

private:
  Document3dm& m_doc;
  std::ostream& m_out;
  std::map<std::string, CommandInfo> m_commands;  // chiave: nome normalizzato
  std::map<std::string, std::string> m_aliases;   // nomi italiani
  std::map<std::string, std::string> m_leadingOptions;  // opzioni dopo il nome del comando in corso
  std::set<ON_UUID, UuidLess> m_selection;
  std::vector<ON_UUID> m_selectionOrder, m_prevSelection;
  std::vector<ON_UUID> m_lastCreated, m_previousCreated;
  bool m_collectingCreated = false;
  bool m_silent = false;  // aggiornamento della storia: niente "Comando: ..." nella cronologia
  void RecordHistory(const std::string& command, const std::string& args, const std::vector<ON_UUID>& inputs,
                     const std::vector<std::string>& inputHashes);
  std::vector<std::string> HashObjects(const std::vector<ON_UUID>& ids) const;
  int m_commandDepth = 0;
  struct UndoStep {
    std::vector<unsigned char> state;
    size_t remote = 0;  // modifiche ricevute (collaborazione) gia' contenute in "state"
  };
  std::vector<UndoStep> m_undo, m_redo;
  std::vector<std::function<bool(ONX_Model&)>> m_remote;
  UndoStep Step(std::vector<unsigned char>&& state) const { return {std::move(state), m_remote.size()}; }
  bool RestoreStep(const UndoStep& step);
  std::string m_lastError;
  bool m_needsInput = false;
  std::string m_prompt, m_lastCommandName;
  InputKind m_inputKind = InputKind::Value, m_nextKind = InputKind::Value;
  bool AtInputEnd(TokenStream& ts, const char* prompt);
};

// Registrazione dei gruppi di comandi (un file .cpp per gruppo).
void RegisterCreateCurveCommands(Engine& e);
void RegisterCreateSurfaceSolidCommands(Engine& e);
void RegisterTransformCommands(Engine& e);
void RegisterEditCommands(Engine& e);
void RegisterDocumentCommands(Engine& e);
void RegisterMoreCommands(Engine& e);
void RegisterAnnotationCommands(Engine& e);
void RegisterExtraCommands(Engine& e);
void RegisterExtra2Commands(Engine& e);
void RegisterExtra3Commands(Engine& e);
void RegisterExtra4Commands(Engine& e);
void RegisterExtra5Commands(Engine& e);
void RegisterExtra6Commands(Engine& e);
void RegisterMaterialCommands(Engine& e);
void RegisterExchangeCommands(Engine& e);
void RegisterViewCommands(Engine& e);
void RegisterLayoutCommands(Engine& e);
void RegisterRenderCommands(Engine& e);
void RegisterPythonCommands(Engine& e);
void RegisterHistoryCommands(Engine& e);
void RegisterNodeCommands(Engine& e);
void RegisterCollabCommands(Engine& e);
// PDF della tavola attiva o della vista (opzioni come Print)
bool ExportPdf(Engine& e, const std::string& path, const std::map<std::string, std::string>& opts);
// CageEdit: deforma di nuovo gli oggetti catturati dalla gabbia; restituisce quanti
int UpdateCageCaptives(Engine& e, const ON_UUID& cageId);
// Mesh a griglia da una funzione (u,v) in [0,1]x[0,1]; closedU/V uniscono i bordi.
ON_Mesh* GridMesh(int nu, int nv, bool closedU, const std::function<ON_3dPoint(double, double)>& f, bool closedV = false);
#ifdef CAD_WITH_OCCT
void RegisterKernelCommands(Engine& e);
void RegisterCurveEditCommands(Engine& e);
void RegisterSurfaceMoreCommands(Engine& e);
void RegisterSurface3Commands(Engine& e);
void RegisterKernel4Commands(Engine& e);
void RegisterKernel5Commands(Engine& e);
void RegisterKernel6Commands(Engine& e);
void RegisterAnalysisCommands(Engine& e);
void RegisterModelingCommands(Engine& e);
void RegisterWorksessionCommands(Engine& e);
const std::vector<std::pair<const char*, const char*>>& ItalianCommandNames();
void RegisterCommandOptions(Engine& e);
#endif

// Utilita' geometriche condivise
ON_NurbsCurve* InterpolateCubic(const std::vector<ON_3dPoint>& points);
ON_NurbsCurve* ControlPointCurve(const std::vector<ON_3dPoint>& points, int degree);
std::string DescribeObject(const ON_Geometry* g);
double CurveLengthOf(const ON_Curve& c);

}  // namespace cad
