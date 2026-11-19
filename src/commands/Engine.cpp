#include "commands/Engine.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace cad {

// ---------------------------------------------------------------- token
static bool IsPythonCommand(std::string t) {
  while (!t.empty() && (t[0] == '-' || t[0] == '_' || t[0] == '!' || t[0] == '\'')) t.erase(0, 1);
  for (auto& ch : t) ch = (char)std::tolower((unsigned char)ch);
  return t == "runpythonscript";
}

TokenStream::TokenStream(const std::string& script, bool appendFinalEnter) {
  std::string cur;
  bool inQuote = false;
  auto flush = [&] { if (!cur.empty()) { m_tokens.push_back(cur); cur.clear(); } };
  for (size_t i = 0; i < script.size(); i++) {
    char c = script[i];
    if (inQuote) {
      if (c == '"') { inQuote = false; m_tokens.push_back(cur); cur.clear(); }
      else cur += c;
    } else if (c == '(' && cur.empty() && !m_tokens.empty() && IsPythonCommand(m_tokens.back())) {
      // -RunPythonScript (codice): il codice resta un solo token, con virgolette e a capo
      size_t j = i;
      int depth = 0;
      char str = 0;
      for (; j < script.size(); j++) {
        const char d = script[j];
        if (str) {
          if (d == '\\') j++;
          else if (d == str) str = 0;
        } else if (d == '"' || d == '\'') str = d;
        else if (d == '(') depth++;
        else if (d == ')' && --depth == 0) break;
      }
      m_tokens.push_back(script.substr(i, std::min(j, script.size() - 1) - i + 1));
      i = j;
    } else if (c == '"') {
      flush(); inQuote = true;
    } else if (c == '\n' || c == '\r') {
      flush();
      // Fine riga = Invio (concludono i comandi a punti multipli).
      if (c == '\n' && (m_tokens.empty() || m_tokens.back() != "_Enter")) m_tokens.push_back("_Enter");
    } else if (c == ' ' || c == '\t') {
      flush();
    } else if (c == ';' && cur.empty()) {
      // commento fino a fine riga
      while (i + 1 < script.size() && script[i + 1] != '\n') i++;
    } else {
      cur += c;
    }
  }
  flush();
  // In modalita' interattiva solo un Invio esplicito conclude le liste.
  if (!appendFinalEnter) return;
  if (!m_tokens.empty() && m_tokens.back() != "_Enter") m_tokens.push_back("_Enter");
}

std::string TokenStream::Join(size_t from, size_t to) const {
  std::string out;
  for (size_t i = from; i < to && i < m_tokens.size(); i++) {
    if (!out.empty()) out += ' ';
    const std::string& t = m_tokens[i];
    if (t.empty() || t.find_first_of(" \t;") != std::string::npos) out += '"' + t + '"';
    else out += t;
  }
  return out;
}

std::string TokenStream::JoinFrom(size_t from) const {
  std::string out;
  for (size_t i = from; i < m_tokens.size(); i++) {
    if (!out.empty()) out += ' ';
    if (m_tokens[i].find(' ') != std::string::npos) out += '"' + m_tokens[i] + '"';
    else out += m_tokens[i];
  }
  return out;
}

const std::string& TokenStream::Peek() const {
  static const std::string empty;
  return AtEnd() ? empty : m_tokens[m_pos];
}

std::string TokenStream::Next() { return AtEnd() ? std::string() : m_tokens[m_pos++]; }

std::string NormalizeName(const std::string& token) {
  size_t i = 0;
  while (i < token.size() && (token[i] == '_' || token[i] == '-' || token[i] == '!' || token[i] == '\'')) i++;
  std::string s = token.substr(i);
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool IsEnter(const std::string& token) { return NormalizeName(token) == "enter"; }

// ---------------------------------------------------------------- engine
Engine::Engine(Document3dm& doc, std::ostream& out) : m_doc(doc), m_out(out) {
  if (doc.Model().ActiveComponentCount(ON_ModelComponent::Type::Layer) == 0) doc.NewEmpty();
  RegisterCreateCurveCommands(*this);
  RegisterCreateSurfaceSolidCommands(*this);
  RegisterTransformCommands(*this);
  RegisterEditCommands(*this);
  RegisterDocumentCommands(*this);
  RegisterMoreCommands(*this);
  RegisterAnnotationCommands(*this);
  RegisterExtraCommands(*this);
  RegisterExtra2Commands(*this);
  RegisterExtra3Commands(*this);
  RegisterExtra4Commands(*this);
  RegisterExtra5Commands(*this);
  RegisterExtra6Commands(*this);
  RegisterMaterialCommands(*this);
  RegisterExchangeCommands(*this);
  RegisterViewCommands(*this);
  RegisterLayoutCommands(*this);
  RegisterRenderCommands(*this);
  RegisterPythonCommands(*this);
  RegisterHistoryCommands(*this);
  RegisterNodeCommands(*this);
  RegisterCollabCommands(*this);
#ifdef CAD_WITH_OCCT
  RegisterKernelCommands(*this);
  RegisterCurveEditCommands(*this);
  RegisterSurfaceMoreCommands(*this);
  RegisterSurface3Commands(*this);
  RegisterKernel4Commands(*this);
  RegisterKernel5Commands(*this);
  RegisterKernel6Commands(*this);
  RegisterAnalysisCommands(*this);
  RegisterModelingCommands(*this);
#endif
  RegisterCommandOptions(*this);
  // nomi italiani: solo per i comandi presenti e senza coprire un nome inglese
  for (const auto& [it, en] : ItalianCommandNames()) {
    const std::string k = NormalizeName(it), v = NormalizeName(en);
    if (m_commands.count(v) && !m_commands.count(k)) m_aliases[k] = v;
  }
}

const CommandInfo* Engine::FindCommand(const std::string& name) const {
  const std::string n = NormalizeName(name);
  auto it = m_commands.find(n);
  if (it != m_commands.end()) return &it->second;
  // il prefisso "_" chiede il nome inglese: niente alias
  if (!name.empty() && (name[0] == '_' || (name.size() > 1 && name[0] == '-' && name[1] == '_'))) return nullptr;
  auto a = m_aliases.find(n);
  if (a == m_aliases.end()) return nullptr;
  it = m_commands.find(a->second);
  return it != m_commands.end() ? &it->second : nullptr;
}

void Engine::Register(CommandInfo info) {
  m_commands[NormalizeName(info.name)] = std::move(info);
}

bool Engine::Fail(const std::string& message) {
  m_lastError = message;
  m_out << "  Errore: " << message << "\n";
  return false;
}

bool Engine::Run(const std::string& script) {
  TokenStream ts(script);
  while (!ts.AtEnd()) {
    std::string t = ts.Next();
    if (IsEnter(t)) continue;
    if (!RunCommand(t, ts)) return false;
  }
  return true;
}

bool Engine::Transaction(const std::string& name, const std::function<bool()>& fn) {
  std::vector<unsigned char> before = m_doc.ToBuffer();
  m_lastCreated.clear();
  m_collectingCreated = true;
  m_commandDepth++;
  bool ok = false;
  try {
    ok = fn();
  } catch (...) {
    ok = false;
  }
  m_commandDepth--;
  m_collectingCreated = false;
  if (!m_lastCreated.empty()) m_previousCreated = m_lastCreated;
  if (!ok) {
    m_doc.FromBuffer(before);
    return false;
  }
  m_undo.push_back(Step(std::move(before)));
  m_redo.clear();
  m_lastCommandName = name;
  return true;
}

Engine::Result Engine::RunInteractive(const std::string& script, std::string* pending) {
  TokenStream ts(script, false);
  if (pending) pending->clear();
  while (!ts.AtEnd()) {
    const size_t start = ts.Pos();
    std::string t = ts.Next();
    if (IsEnter(t)) continue;
    m_needsInput = false;
    if (!RunCommand(t, ts)) {
      if (m_needsInput) {
        if (pending) *pending = ts.JoinFrom(start);
        return Result::NeedsInput;
      }
      return Result::Error;
    }
  }
  return Result::Done;
}

bool Engine::MoreInput(TokenStream& ts) {
  if (ts.AtEnd()) return interactive;  // la lettura successiva chiedera' altro input
  return !IsEnter(ts.Peek());
}

bool Engine::AtInputEnd(TokenStream& ts, const char* prompt) {
  const InputKind kind = m_nextKind;
  m_nextKind = InputKind::Value;
  if (!ts.AtEnd() && !IsEnter(ts.Peek())) return false;
  if (ts.AtEnd() && interactive) { m_needsInput = true; m_prompt = prompt; m_inputKind = kind; }
  Fail(std::string("manca ") + prompt);
  return true;
}

bool Engine::RunCommand(const std::string& name, TokenStream& ts) {
  const CommandInfo* found = FindCommand(name);
  if (!found) return Fail("Comando sconosciuto: \"" + name + "\"");
  const CommandInfo& cmd = *found;
  if (!interactive && !m_silent) m_out << "Comando: " << cmd.name << "\n";
  // Comandi annidati (es. rs.Command da uno script): un solo passo di annullamento,
  // quello del comando esterno; gli oggetti creati si sommano a quelli dell'esterno.
  const bool nested = m_commandDepth > 0;
  std::vector<unsigned char> before;
  if (cmd.modifiesModel) before = m_doc.ToBuffer();
  std::vector<ON_UUID> outerCreated;
  if (nested) outerCreated = m_lastCreated;
  const bool outerCollecting = m_collectingCreated;
  if (!nested) keepChangesOnFailure = false;
  // storia: ingressi (selezione) e parametri del comando
  const bool wantHistory = !nested && recordHistory && cmd.modifiesModel && SupportsHistory(cmd.name);
  const size_t argStart = ts.Pos();
  std::vector<ON_UUID> histInputs;
  std::vector<std::string> histHashes;
  if (wantHistory) { histInputs = Selection(); histHashes = HashObjects(histInputs); }
  m_lastCreated.clear();
  m_collectingCreated = true;
  // opzioni dichiarate scritte subito dopo il nome (anche se il comando le legge piu' avanti)
  auto outerOptions = std::move(m_leadingOptions);
  m_leadingOptions.clear();
  if (!cmd.options.empty()) {
    std::set<std::string> known;
    for (const auto& o : cmd.options) known.insert(NormalizeName(o.substr(0, o.find('='))));
    while (!ts.AtEnd()) {
      const std::string& t = ts.Peek();
      const auto eq = t.find('=');
      if (eq == std::string::npos || !known.count(NormalizeName(t.substr(0, eq)))) break;
      m_leadingOptions[NormalizeName(t.substr(0, eq))] = NormalizeName(t.substr(eq + 1));
      ts.Next();
    }
  }
  m_commandDepth++;
  bool ok = false;
  try {
    ok = cmd.fn(*this, ts);
  } catch (const std::exception& ex) {
    ok = Fail(std::string("errore interno: ") + ex.what());
  } catch (...) {
    ok = Fail("errore interno del motore geometrico");
  }
  m_commandDepth--;
  m_leadingOptions = std::move(outerOptions);
  m_collectingCreated = outerCollecting;
  if (!m_lastCreated.empty()) m_previousCreated = m_lastCreated;
  if (nested) {
    if (!ok && cmd.modifiesModel && !before.empty()) { m_doc.FromBuffer(before); m_lastCreated.clear(); }
    outerCreated.insert(outerCreated.end(), m_lastCreated.begin(), m_lastCreated.end());
    m_lastCreated = std::move(outerCreated);
    return ok;
  }
  if (ok && cmd.modifiesModel) {
    if (wantHistory && !histInputs.empty() && !m_lastCreated.empty())
      RecordHistory(cmd.name, ts.Join(argStart, ts.Pos()), histInputs, histHashes);
    if (historyUpdate) {
      m_commandDepth++;
      UpdateHistory();
      m_commandDepth--;
    }
  }
  if ((ok || keepChangesOnFailure) && cmd.modifiesModel) {
    m_undo.push_back(Step(std::move(before)));
    m_redo.clear();
  }
  if (!ok && !keepChangesOnFailure && cmd.modifiesModel && !before.empty()) {
    // Un comando non concluso non lascia modifiche a meta'.
    m_doc.FromBuffer(before);
    m_lastCreated.clear();
  }
  if (ok) {
    m_lastCommandName = cmd.name;
    if (interactive && !m_silent) m_out << "Comando: " << cmd.name << "\n";
  }
  return ok;
}

// ---------------------------------------------------------------- input
static bool ParseDouble(const std::string& s, double& v) {
  if (s.empty()) return false;
  char* end = nullptr;
  v = std::strtod(s.c_str(), &end);
  return end && *end == 0;
}

std::optional<double> Engine::ReadNumber(TokenStream& ts, const char* prompt) {
  if (AtInputEnd(ts, prompt)) return std::nullopt;
  double v;
  std::string t = ts.Next();
  if (!ParseDouble(t, v)) { Fail(std::string(prompt) + ": numero non valido \"" + t + "\""); return std::nullopt; }
  return v;
}

std::optional<int> Engine::ReadInt(TokenStream& ts, const char* prompt) {
  auto v = ReadNumber(ts, prompt);
  if (!v) return std::nullopt;
  return static_cast<int>(std::lround(*v));
}

std::optional<std::string> Engine::ReadString(TokenStream& ts, const char* prompt) {
  if (AtInputEnd(ts, prompt)) return std::nullopt;
  return ts.Next();
}

std::optional<ON_3dPoint> Engine::ReadPick(TokenStream& ts, const char* prompt) {
  m_nextKind = InputKind::Pick;
  return ReadPoint(ts, prompt);
}

std::optional<ON_3dPoint> Engine::ReadPoint(TokenStream& ts, const char* prompt) {
  if (m_nextKind != InputKind::Pick) m_nextKind = InputKind::Point;
  if (AtInputEnd(ts, prompt)) return std::nullopt;
  std::string t = ts.Next();
  std::string s = t;
  bool relative = false;
  if (!s.empty() && (s[0] == 'r' || s[0] == 'R' || s[0] == '@')) { relative = true; s = s.substr(1); }
  else if (!s.empty() && (s[0] == 'w' || s[0] == 'W')) s = s.substr(1);
  ON_3dPoint p;
  if (auto lt = s.find('<'); lt != std::string::npos) {
    double d, a;
    if (!ParseDouble(s.substr(0, lt), d) || !ParseDouble(s.substr(lt + 1), a)) {
      Fail(std::string(prompt) + ": punto polare non valido \"" + t + "\""); return std::nullopt;
    }
    const double r = a * ON_PI / 180.0;
    p = ON_3dPoint(d * std::cos(r), d * std::sin(r), 0.0);
  } else {
    std::vector<double> c;
    std::stringstream ss(s);
    std::string part;
    while (std::getline(ss, part, ',')) {
      double v;
      if (!ParseDouble(part, v)) { c.clear(); break; }
      c.push_back(v);
    }
    if (c.size() < 2 || c.size() > 3) {
      // Ammesso un numero singolo? No: in Rhino un numero e' una distanza.
      Fail(std::string(prompt) + ": punto non valido \"" + t + "\""); return std::nullopt;
    }
    p = ON_3dPoint(c[0], c[1], c.size() == 3 ? c[2] : 0.0);
  }
  if (relative) p = lastPoint + ON_3dVector(p);
  lastPoint = p;
  return p;
}

std::map<std::string, std::string> Engine::ReadOptions(TokenStream& ts, const std::set<std::string>& switches) {
  std::map<std::string, std::string> opts;
  while (!ts.AtEnd()) {
    const std::string& t = ts.Peek();
    auto eq = t.find('=');
    if (eq != std::string::npos) {
      const std::string key = NormalizeName(t.substr(0, eq));
      std::string val = t.substr(eq + 1);
      ts.Next();
      // Name="Con spazi": il valore tra virgolette e' il token successivo
      if (val.empty() && !ts.AtEnd() && !IsEnter(ts.Peek())) { val = ts.Peek(); ts.Next(); }
      // nomi e caratteri mantengono maiuscole e minuscole
      opts[key] = key == "name" || key == "font" || key == "file" ? val : NormalizeName(val);
    } else if (switches.count(NormalizeName(t))) {
      opts[NormalizeName(t)] = "yes";
      ts.Next();
    } else {
      break;
    }
  }
  // le opzioni scritte dopo il nome del comando valgono se non ripetute qui
  for (const auto& [k, v] : m_leadingOptions) opts.emplace(k, v);
  return opts;
}

void Engine::SkipEnter(TokenStream& ts) {
  if (!ts.AtEnd() && IsEnter(ts.Peek())) ts.Next();
}

// ---------------------------------------------------------------- doc
double Engine::Tolerance() const {
  double t = m_doc.Model().m_settings.m_ModelUnitsAndTolerances.m_absolute_tolerance;
  return t > 0.0 ? t : 0.001;
}

ON_UUID Engine::AddObject(ON_Object* geometry, const ON_3dmObjectAttributes* attrs) {
  ONX_Model& m = Model();
  auto* a = new ON_3dmObjectAttributes();
  if (attrs) *a = *attrs;
  else {
    const ON_UUID layerId = m.m_settings.CurrentLayerId();
    const ON_ModelComponent* layer = m.ComponentFromId(ON_ModelComponent::Type::Layer, layerId).ModelComponent();
    a->m_layer_index = layer ? layer->Index() : 0;
  }
  if (!attrs && !ON_UuidIsNil(activeLayoutId)) {
    a->m_space = ON::page_space;
    a->m_viewport_id = activeLayoutId;
  }
  a->m_uuid = ON_CreateId();
  ON_ModelComponentReference ref = m.AddManagedModelGeometryComponent(geometry, a, true);
  const ON_ModelComponent* c = ref.ModelComponent();
  if (!c) return ON_nil_uuid;
  const auto* mgc = ON_ModelGeometryComponent::Cast(c);
  ON_UUID id = (mgc && mgc->Attributes(nullptr)) ? mgc->Attributes(nullptr)->m_uuid : c->Id();
  if (m_collectingCreated) m_lastCreated.push_back(id);
  return id;
}

const ON_ModelGeometryComponent* Engine::Object(const ON_UUID& id) const {
  const ON_ModelGeometryComponent* g = ON_ModelGeometryComponent::Cast(
      m_doc.Model().ComponentFromId(ON_ModelComponent::Type::ModelGeometry, id).ModelComponent());
  if (g) return g;
  // le luci sono componenti a parte nel modello di openNURBS
  return ON_ModelGeometryComponent::Cast(m_doc.Model().ComponentFromId(ON_ModelComponent::Type::RenderLight, id).ModelComponent());
}

ON_Geometry* Engine::EditableGeometry(const ON_UUID& id) {
  const auto* o = Object(id);
  return o ? o->ExclusiveGeometry() : nullptr;
}

ON_3dmObjectAttributes* Engine::EditableAttributes(const ON_UUID& id) {
  const auto* o = Object(id);
  return o ? o->ExclusiveAttributes() : nullptr;
}

ON_UUID Engine::ReplaceObject(const ON_UUID& id, ON_Object* geometry) {
  const auto* o = Object(id);
  if (!o || !geometry) { delete geometry; return ON_nil_uuid; }
  auto* a = new ON_3dmObjectAttributes(*o->Attributes(&ON_3dmObjectAttributes::DefaultAttributes));
  const bool wasSelected = IsSelected(id);
  ONX_Model& m = Model();
  m.RemoveModelComponent(o->ComponentType(), id);
  a->m_uuid = id;  // stesso id: selezione, gruppi e riferimenti restano validi
  ON_ModelComponentReference ref = m.AddManagedModelGeometryComponent(geometry, a, true);
  const auto* mgc = ON_ModelGeometryComponent::Cast(ref.ModelComponent());
  if (!mgc || !mgc->Attributes(nullptr)) return ON_nil_uuid;
  const ON_UUID nid = mgc->Attributes(nullptr)->m_uuid;
  if (wasSelected && ON_UuidCompare(nid, id) != 0) Deselect(id);
  return nid;
}

bool Engine::DeleteObject(const ON_UUID& id) {
  Deselect(id);
  if (!Model().RemoveModelComponent(ON_ModelComponent::Type::ModelGeometry, id).IsEmpty()) return true;
  return !Model().RemoveModelComponent(ON_ModelComponent::Type::RenderLight, id).IsEmpty();
}

std::vector<ON_UUID> Engine::AllObjectIds(bool includeHidden) const {
  return ObjectIdsInSpace(activeLayoutId, includeHidden);
}

std::vector<ON_UUID> Engine::ObjectIdsInSpace(const ON_UUID& layoutId, bool includeHidden) const {
  std::vector<ON_UUID> ids;
  const bool page = !ON_UuidIsNil(layoutId);
  // geometria e luci (le luci sono un tipo di componente a parte)
  for (auto type : {ON_ModelComponent::Type::ModelGeometry, ON_ModelComponent::Type::RenderLight}) {
    ONX_ModelComponentIterator it(m_doc.Model(), type);
    for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent()) {
      const auto* mgc = ON_ModelGeometryComponent::Cast(c);
      if (!mgc) continue;
      const ON_3dmObjectAttributes* a = mgc->Attributes(nullptr);
      if (!a) continue;
      if (a->IsInstanceDefinitionObject()) continue;  // oggetti dentro i blocchi
      if (!includeHidden && a->Mode() == ON::object_mode::hidden_object) continue;
      if (page != (a->m_space == ON::page_space)) continue;
      if (page && a->m_viewport_id != layoutId) continue;
      ids.push_back(a->m_uuid);
    }
  }
  return ids;
}

std::vector<ON_UUID> Engine::Selection() const {
  std::vector<ON_UUID> out;
  for (const auto& id : m_selectionOrder)
    if (m_selection.count(id) && Object(id)) out.push_back(id);
  return out;
}

void Engine::Deselect(const ON_UUID& id) {
  m_selection.erase(id);
  m_selectionOrder.erase(std::remove_if(m_selectionOrder.begin(), m_selectionOrder.end(),
                                        [&](const ON_UUID& x) { return ON_UuidCompare(x, id) == 0; }),
                         m_selectionOrder.end());
}

std::vector<ON_UUID> Engine::RequireSelection(const char* command, const char* prompt) {
  auto sel = Selection();
  if (sel.empty() && interactive) {
    // Come Rhino: il comando chiede gli oggetti e riparte quando l'utente preme Invio.
    m_needsInput = true;
    m_prompt = prompt ? prompt : "seleziona gli oggetti, poi Invio";
    m_inputKind = InputKind::Objects;
    m_lastError = "nessun oggetto selezionato";
    return sel;
  }
  if (sel.empty()) Fail(std::string(command) + ": nessun oggetto selezionato (usa SelAll, SelLast, SelCrv...)");
  return sel;
}

bool Engine::RestoreStep(const UndoStep& step) {
  if (!m_doc.FromBuffer(step.state)) return false;
  // le modifiche ricevute dopo questo stato restano: si riapplicano nell'ordine di arrivo
  for (size_t i = step.remote; i < m_remote.size(); i++) m_remote[i](Model());
  return true;
}

bool Engine::ApplyRemote(const std::function<bool(ONX_Model&)>& change) {
  if (!change(Model())) return false;
  m_remote.push_back(change);
  return true;
}

bool Engine::Undo() {
  if (m_undo.empty()) return Fail("niente da annullare");
  m_redo.push_back(Step(m_doc.ToBuffer()));
  if (!RestoreStep(m_undo.back())) return Fail("ripristino non riuscito");
  m_undo.pop_back();
  CheckActiveLayout();
  return true;
}

void Engine::SquashUndo(size_t depth) {
  if (m_undo.size() > depth + 1) m_undo.erase(m_undo.begin() + (long)depth + 1, m_undo.end());
  m_redo.clear();
}

bool Engine::RestoreUndo(size_t depth) {
  if (m_undo.size() <= depth) return true;  // nessun passo da togliere
  if (!RestoreStep(m_undo[depth])) return false;
  m_undo.resize(depth);
  m_redo.clear();
  ClearSelection();
  CheckActiveLayout();
  return true;
}

bool Engine::Redo() {
  if (m_redo.empty()) return Fail("niente da ripetere");
  m_undo.push_back(Step(m_doc.ToBuffer()));
  if (!RestoreStep(m_redo.back())) return Fail("ripristino non riuscito");
  m_redo.pop_back();
  CheckActiveLayout();
  return true;
}

void Engine::CheckActiveLayout() {
  if (ON_UuidIsNil(activeLayoutId)) return;
  const auto& views = Model().m_settings.m_views;
  for (int i = 0; i < views.Count(); i++)
    if (views[i].m_view_type == ON::view_type::page_view_type && views[i].m_vp.ViewportId() == activeLayoutId) return;
  activeLayoutId = ON_nil_uuid;
  if (layoutsChanged) layoutsChanged();
}

}  // namespace cad
