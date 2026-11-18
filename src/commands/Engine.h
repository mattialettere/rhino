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
  std::vector<ON_UUID> AllObjectIds(bool includeHidden = false) const;

  // --- selezione ---
  // Oggetti selezionati ancora esistenti, nell'ordine in cui sono stati
  // selezionati (conta per BooleanDifference, Split, Loft, Sweep1).
  std::vector<ON_UUID> Selection() const;
  void Select(const ON_UUID& id) { if (m_selection.insert(id).second) m_selectionOrder.push_back(id); }
  void Deselect(const ON_UUID& id);
  bool IsSelected(const ON_UUID& id) const { return m_selection.count(id) > 0; }
  void ClearSelection() { m_selection.clear(); m_selectionOrder.clear(); }
  // Oggetti creati dall'ultimo comando che ha creato qualcosa (per SelLast).
  const std::vector<ON_UUID>& LastCreated() const { return m_previousCreated; }
  // Oggetti selezionati. Se non ce ne sono: in modalita' interattiva il comando
  // resta in attesa che l'utente li selezioni (prompt), altrimenti errore.
  std::vector<ON_UUID> RequireSelection(const char* command, const char* prompt = nullptr);

  // --- annulla ---
  bool Undo();
  bool Redo();

  // --- messaggi ---
  std::ostream& Out() { return m_out; }
  bool Fail(const std::string& message);
  const std::string& LastError() const { return m_lastError; }

  std::string currentPath;   // percorso del file aperto/salvato
  // Oggetti con i punti di controllo visibili (PointsOn / PointsOff)
  std::set<ON_UUID, UuidLess> pointsOn;
  ON_3dPoint lastPoint = ON_3dPoint::Origin;

private:
  Document3dm& m_doc;
  std::ostream& m_out;
  std::map<std::string, CommandInfo> m_commands;  // chiave: nome normalizzato
  std::set<ON_UUID, UuidLess> m_selection;
  std::vector<ON_UUID> m_selectionOrder;
  std::vector<ON_UUID> m_lastCreated, m_previousCreated;
  bool m_collectingCreated = false;
  std::vector<std::vector<unsigned char>> m_undo, m_redo;
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
// Mesh a griglia da una funzione (u,v) in [0,1]x[0,1]; closedU/V uniscono i bordi.
ON_Mesh* GridMesh(int nu, int nv, bool closedU, const std::function<ON_3dPoint(double, double)>& f, bool closedV = false);
#ifdef CAD_WITH_OCCT
void RegisterKernelCommands(Engine& e);
void RegisterCurveEditCommands(Engine& e);
void RegisterSurfaceMoreCommands(Engine& e);
#endif

// Utilita' geometriche condivise
ON_NurbsCurve* InterpolateCubic(const std::vector<ON_3dPoint>& points);
ON_NurbsCurve* ControlPointCurve(const std::vector<ON_3dPoint>& points, int degree);
std::string DescribeObject(const ON_Geometry* g);
double CurveLengthOf(const ON_Curve& c);

}  // namespace cad
