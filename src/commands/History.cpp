// Storia di costruzione (History di Rhino 8): un oggetto creato con la storia attiva
// ricorda il comando, i parametri e gli oggetti di ingresso; quando un ingresso cambia
// il comando viene rieseguito e gli oggetti figli si aggiornano mantenendo id e attributi.
// I record si salvano nel .3dm come testo utente del documento ("Kurva3D.History"): cosi'
// seguono anche Annulla/Ripeti e Rhino li conserva senza interpretarli (D-065).
#include "commands/Engine.h"
#include "core/Document3dm.h"
#include <algorithm>
#include <memory>
#include <set>
#include <sstream>

namespace cad {

namespace {

const wchar_t* kKey = L"Kurva3D.History";

struct Record {
  std::string command, args;
  std::vector<ON_UUID> inputs, outputs;
  std::vector<std::string> inputHash, outputHash;
};

std::string IdStr(const ON_UUID& id) {
  char s[64];
  ON_UuidToString(id, s);
  return s;
}

// testo di un campo: niente '|', ';' di separazione o a capo
std::string Esc(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '%' || c == '|' || c == '\n' || c == '\r' || c == ',') {
      char b[4];
      std::snprintf(b, sizeof b, "%%%02X", (unsigned char)c);
      o += b;
    } else {
      o += c;
    }
  }
  return o;
}

std::string Unesc(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size()) {
      o += (char)std::stoi(s.substr(i + 1, 2), nullptr, 16);
      i += 2;
    } else {
      o += s[i];
    }
  }
  return o;
}

std::vector<std::string> Split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == sep) { out.push_back(cur); cur.clear(); }
    else cur += c;
  }
  out.push_back(cur);
  return out;
}

std::string JoinIds(const std::vector<ON_UUID>& ids) {
  std::string o;
  for (const auto& id : ids) o += (o.empty() ? "" : ",") + IdStr(id);
  return o;
}

std::string JoinStr(const std::vector<std::string>& v) {
  std::string o;
  for (size_t i = 0; i < v.size(); i++) o += (i ? "," : "") + v[i];
  return o;
}

std::vector<Record> Load(const ONX_Model& m) {
  std::vector<Record> out;
  ON_wString w;
  if (!m.GetDocumentUserString(kKey, w)) return out;
  std::istringstream in(ToUtf8(w));
  for (std::string line; std::getline(in, line);) {
    auto f = Split(line, '|');
    if (f.size() != 6) continue;
    Record r;
    r.command = Unesc(f[0]);
    r.args = Unesc(f[1]);
    if (!f[2].empty()) for (auto& s : Split(f[2], ',')) r.inputs.push_back(ON_UuidFromString(s.c_str()));
    if (!f[3].empty()) for (auto& s : Split(f[3], ',')) r.outputs.push_back(ON_UuidFromString(s.c_str()));
    if (!f[4].empty()) r.inputHash = Split(f[4], ',');
    if (!f[5].empty()) r.outputHash = Split(f[5], ',');
    if (r.inputHash.size() != r.inputs.size() || r.outputHash.size() != r.outputs.size()) continue;
    out.push_back(std::move(r));
  }
  return out;
}

void Save(ONX_Model& m, const std::vector<Record>& recs) {
  if (recs.empty()) { m.SetDocumentUserString(kKey, nullptr); return; }
  std::string s;
  for (const auto& r : recs)
    s += Esc(r.command) + "|" + Esc(r.args) + "|" + JoinIds(r.inputs) + "|" + JoinIds(r.outputs) + "|" +
         JoinStr(r.inputHash) + "|" + JoinStr(r.outputHash) + "\n";
  m.SetDocumentUserString(kKey, ON_wString(s.c_str()));
}

// Impronta della geometria (SHA-1 della serializzazione, senza dati utente).
std::string Hash(const ON_Geometry& g) {
  if (const ON_SubD* subd = ON_SubD::Cast(&g))
    return std::string("s") + static_cast<const char*>(subd->GeometryHash().ToUTF8String(false));
  std::unique_ptr<ON_Object> copy(g.Duplicate());
  if (!copy) return "-";
  copy->PurgeUserData();
  if (const ON_Brep* b = ON_Brep::Cast(copy.get())) (void)b->IsSolid();
  ON_Write3dmBufferArchive ar(0, 0, 80, ON::Version());
  if (!ar.WriteObject(*copy)) return "-";
  ON_SHA1 sha;
  sha.AccumulateBytes(ar.Buffer(), ar.SizeOfArchive());
  return static_cast<const char*>(sha.Hash().ToUTF8String(false));
}

bool Has(const std::vector<ON_UUID>& v, const ON_UUID& id) {
  return std::any_of(v.begin(), v.end(), [&](const ON_UUID& x) { return ON_UuidCompare(x, id) == 0; });
}

}  // namespace

bool Engine::SupportsHistory(const std::string& name) {
  // comandi che in Rhino 8 registrano la storia e qui creano oggetti nuovi dagli ingressi
  static const std::set<std::string> k = {
      "loft", "extrudecrv", "extrudesrf", "extrudecrvtopoint", "extrudecrvalongcrv", "extrudecrvtapered",
      "revolve", "railrevolve", "sweep1", "sweep2", "pipe", "offset", "offsetsrf", "planarsrf", "edgesrf",
      "networksrf", "patch", "arraypolar", "arraylinear", "array", "arraycrv", "mirror", "project", "pull",
      "dupedge", "dupborder", "contour", "intersect", "silhouette", "blendcrv", "blendsrf", "filletsrf",
      "extractisocurve", "smash", "twist", "bend", "taper", "flow"};
  std::string n;
  for (char c : name) n += (char)std::tolower((unsigned char)c);
  return k.count(n) > 0;
}

std::vector<std::string> Engine::HashObjects(const std::vector<ON_UUID>& ids) const {
  std::vector<std::string> out;
  for (const auto& id : ids) {
    const auto* o = Object(id);
    const ON_Geometry* g = o ? o->Geometry(nullptr) : nullptr;
    out.push_back(g ? Hash(*g) : "");
  }
  return out;
}

void Engine::RecordHistory(const std::string& command, const std::string& args, const std::vector<ON_UUID>& inputs,
                           const std::vector<std::string>& inputHashes) {
  // la storia vale solo se gli ingressi sono rimasti com'erano (es. Twist senza copia li modifica)
  const auto now = HashObjects(inputs);
  for (size_t i = 0; i < inputs.size(); i++)
    if (now[i].empty() || now[i] != inputHashes[i]) return;
  std::vector<ON_UUID> outs;
  for (const auto& id : m_lastCreated)
    if (Object(id) && !Has(inputs, id) && !Has(outs, id)) outs.push_back(id);
  if (outs.empty()) return;
  auto recs = Load(Model());
  Record r;
  r.command = command;
  r.args = args;
  r.inputs = inputs;
  r.outputs = outs;
  r.inputHash = now;
  r.outputHash = HashObjects(outs);
  recs.push_back(std::move(r));
  Save(Model(), recs);
  m_out << "  Storia registrata (" << command << ")\n";
}

void Engine::UpdateHistory() {
  ON_wString probe;
  if (!Model().GetDocumentUserString(kKey, probe)) return;
  const auto savedCreated = m_lastCreated, savedPrevious = m_previousCreated, savedPrevSel = m_prevSelection;
  const auto savedSel = Selection();
  const bool savedInteractive = interactive, savedSilent = m_silent;
  interactive = false;
  m_silent = true;
  std::ostringstream sink;  // i messaggi dei comandi rieseguiti non vanno nella cronologia
  std::streambuf* const outBuf = m_out.rdbuf(sink.rdbuf());
  int updated = 0, broken = 0;
  // a cascata: un figlio aggiornato puo' essere l'ingresso di un altro record
  for (int pass = 0; pass < 16; pass++) {
    auto recs = Load(Model());
    bool changed = false;
    for (size_t k = 0; k < recs.size();) {
      Record& r = recs[k];
      bool erase = false;
      for (const auto& id : r.inputs) if (!Object(id)) erase = true;
      bool anyOut = false;
      for (const auto& id : r.outputs) if (Object(id)) anyOut = true;
      if (!anyOut) erase = true;  // figli cancellati: la storia non serve piu'
      std::vector<std::string> outNow, inNow;
      if (!erase) {
        // un figlio modificato a mano interrompe la storia (con Lock la modifica si annulla al prossimo aggiornamento)
        outNow = HashObjects(r.outputs);
        for (size_t i = 0; i < r.outputs.size(); i++)
          if (outNow[i] != r.outputHash[i] && !historyLock) erase = true;
      }
      if (erase) {
        recs.erase(recs.begin() + (long)k);
        broken++;
        changed = true;
        continue;
      }
      inNow = HashObjects(r.inputs);
      bool same = inNow == r.inputHash;
      if (historyLock && outNow != r.outputHash) same = false;
      if (same) { k++; continue; }
      // riesecuzione con gli ingressi nell'ordine originale
      ClearSelection();
      for (const auto& id : r.inputs) Select(id);
      m_lastCreated.clear();
      TokenStream ts(r.args);
      const bool ok = RunCommand(r.command, ts);
      std::vector<ON_UUID> fresh;
      for (const auto& id : m_lastCreated)
        if (Object(id) && !Has(r.inputs, id) && !Has(r.outputs, id) && !Has(fresh, id)) fresh.push_back(id);
      ClearSelection();
      if (!ok || fresh.empty()) {
        for (const auto& id : fresh) DeleteObject(id);
        recs.erase(recs.begin() + (long)k);
        broken++;
        changed = true;
        continue;
      }
      if (fresh.size() == r.outputs.size()) {
        // stessi id e attributi dei figli: cambia solo la geometria
        for (size_t i = 0; i < fresh.size(); i++) {
          const auto* o = Object(fresh[i]);
          ON_Object* g = o && o->Geometry(nullptr) ? o->Geometry(nullptr)->Duplicate() : nullptr;
          if (g && Object(r.outputs[i])) ReplaceObject(r.outputs[i], g);
          else delete g;
          DeleteObject(fresh[i]);
        }
      } else {
        // numero di oggetti diverso: i nuovi prendono il posto dei vecchi (attributi del primo)
        ON_3dmObjectAttributes attrs;
        bool haveAttrs = false;
        for (const auto& id : r.outputs)
          if (const auto* o = Object(id)) {
            if (!haveAttrs) { attrs = *o->Attributes(nullptr); haveAttrs = true; }
            DeleteObject(id);
          }
        if (haveAttrs)
          for (const auto& id : fresh)
            if (auto* a = EditableAttributes(id)) {
              const ON_UUID keep = a->m_uuid;
              *a = attrs;
              a->m_uuid = keep;
            }
        r.outputs = fresh;
      }
      r.inputHash = inNow;
      r.outputHash = HashObjects(r.outputs);
      updated++;
      changed = true;
      k++;
    }
    Save(Model(), recs);
    if (!changed) break;
  }
  ClearSelection();
  for (const auto& id : savedSel) if (Object(id)) Select(id);
  m_lastCreated = savedCreated;
  m_previousCreated = savedPrevious;
  m_prevSelection = savedPrevSel;
  interactive = savedInteractive;
  m_silent = savedSilent;
  m_out.rdbuf(outBuf);
  if (updated) m_out << "  Storia: aggiornati " << updated << " oggetti\n";
  if (broken) m_out << "  Storia interrotta per " << broken << " oggetti\n";
}

// ---------------------------------------------------------------- comandi

namespace {

// History Record=Yes|No Update=Yes|No Lock=Yes|No
bool CmdHistory(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto yn = [&](const char* k, bool& v) {
    auto it = opts.find(k);
    if (it == opts.end()) return;
    v = it->second == "yes" || it->second == "si" || it->second == "on" || it->second == "true";
  };
  yn("record", e.recordHistory);
  yn("update", e.historyUpdate);
  yn("lock", e.historyLock);
  e.Out() << "  Registra=" << (e.recordHistory ? "Si" : "No") << " Aggiorna=" << (e.historyUpdate ? "Si" : "No")
          << " Blocca=" << (e.historyLock ? "Si" : "No") << "\n";
  if (e.historyUpdate) e.UpdateHistory();
  return true;
}

std::vector<Record> Records(Engine& e) { return Load(e.Model()); }

// HistoryPurge: toglie la storia dagli oggetti selezionati (o da tutti con All)
bool CmdHistoryPurge(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts, {"all"});
  auto recs = Records(e);
  const auto sel = e.Selection();
  const bool all = opts.count("all") || sel.empty();
  size_t before = recs.size();
  recs.erase(std::remove_if(recs.begin(), recs.end(), [&](const Record& r) {
               if (all) return true;
               for (const auto& id : sel) if (Has(r.outputs, id) || Has(r.inputs, id)) return true;
               return false;
             }), recs.end());
  Save(e.Model(), recs);
  e.Out() << "  Storia tolta da " << (before - recs.size()) << " record\n";
  return true;
}

bool SelectRelated(Engine& e, bool children) {
  const auto sel = e.Selection();
  if (sel.empty()) return e.Fail("seleziona gli oggetti");
  int n = 0;
  for (const auto& r : Records(e))
    for (const auto& id : sel)
      if (Has(children ? r.inputs : r.outputs, id))
        for (const auto& x : children ? r.outputs : r.inputs)
          if (e.Object(x) && !e.IsSelected(x)) { e.Select(x); n++; }
  e.Out() << "  Selezionati " << n << " oggetti\n";
  return true;
}

bool CmdSelChildren(Engine& e, TokenStream&) { return SelectRelated(e, true); }
bool CmdSelParents(Engine& e, TokenStream&) { return SelectRelated(e, false); }

bool CmdSelObjectsWithHistory(Engine& e, TokenStream&) {
  int n = 0;
  for (const auto& r : Records(e))
    for (const auto& id : r.outputs)
      if (e.Object(id) && !e.IsSelected(id)) { e.Select(id); n++; }
  e.Out() << "  Selezionati " << n << " oggetti con storia\n";
  return true;
}

}  // namespace

void RegisterHistoryCommands(Engine& e) {
  e.Register({"History", "Storia", "Registra/aggiorna la storia di costruzione (Record, Update, Lock)", CmdHistory});
  e.Register({"HistoryPurge", "Storia", "Toglie la storia dagli oggetti selezionati", CmdHistoryPurge});
  e.Register({"SelChildren", "Storia", "Seleziona gli oggetti creati dai selezionati", CmdSelChildren, false});
  e.Register({"SelParents", "Storia", "Seleziona gli ingressi degli oggetti selezionati", CmdSelParents, false});
  e.Register({"SelObjectsWithHistory", "Storia", "Seleziona gli oggetti con storia", CmdSelObjectsWithHistory, false});
}

}  // namespace cad
