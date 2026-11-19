// Worksession (Rhino 8): altri file .3dm collegati come riferimento. I loro oggetti si vedono
// nelle viste, bloccati (si agganciano con gli osnap ma non si selezionano ne' si modificano) e
// non vengono salvati nel documento. L'elenco dei file si ricorda nel documento (testo utente
// "Kurva3D.Worksession"), cosi' riaprendo il file la sessione di lavoro si ricostruisce.
#include "commands/Engine.h"

#include <algorithm>
#include <filesystem>
#include <sstream>

namespace cad {
namespace {

const wchar_t* kKey = L"Kurva3D.Worksession";

std::string ToUtf8(const ON_wString& w) {
  const ON_String s(w);
  return std::string(static_cast<const char*>(s));
}

std::string Unquote(std::string s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
  return s;
}

std::vector<std::string> StoredPaths(Engine& e) {
  ON_wString v;
  std::vector<std::string> out;
  if (!e.Model().GetDocumentUserString(kKey, v)) return out;
  std::stringstream ss(ToUtf8(v));
  std::string line;
  while (std::getline(ss, line)) if (!line.empty()) out.push_back(line);
  return out;
}

void Store(Engine& e, const std::vector<std::string>& paths) {
  std::string all;
  for (const auto& p : paths) all += p + "\n";
  e.Model().SetDocumentUserString(kKey, all.empty() ? nullptr : static_cast<const wchar_t*>(ON_wString(all.c_str())));
}

std::string Normalize(const std::string& p) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path a = fs::absolute(fs::u8path(p), ec);
  const auto s = (ec ? fs::u8path(p) : a.lexically_normal()).u8string();
  return std::string(s.begin(), s.end());
}

bool CmdWorksession(Engine& e, TokenStream& ts) {
  auto action = e.ReadString(ts, "azione (Attach, Detach, DetachAll, List, Refresh)");
  if (!action) return false;
  const std::string a = NormalizeName(*action);
  auto paths = StoredPaths(e);
  if (a == "list") {
    e.SyncWorksession();
    if (e.worksession.empty()) e.Out() << "  Nessun file collegato\n";
    for (const auto& f : e.worksession)
      e.Out() << "  " << f.path << (f.doc ? " (" + std::to_string(f.objectCount) + " oggetti)" : std::string(" (non trovato)")) << "\n";
    return true;
  }
  if (a == "refresh") {
    e.worksession.clear();
    e.SyncWorksession();
    e.Out() << "  Ricaricati " << e.worksession.size() << " file\n";
    return true;
  }
  if (a == "detachall") {
    Store(e, {});
    e.SyncWorksession();
    e.Out() << "  Sessione di lavoro vuota\n";
    return true;
  }
  auto f = e.ReadString(ts, "file .3dm");
  if (!f) return false;
  const std::string path = Normalize(Unquote(*f));
  if (a == "attach") {
    if (!e.currentPath.empty() && Normalize(e.currentPath) == path) return e.Fail("il documento attivo non si collega a se stesso");
    for (const auto& p : paths) if (p == path) return e.Fail("file gia' collegato: " + path);
    auto doc = std::make_shared<Document3dm>();
    std::string log;
    if (!doc->Load(path, &log)) return e.Fail("file non leggibile: " + path);
    paths.push_back(path);
    Store(e, paths);
    e.SyncWorksession();
    for (const auto& w : e.worksession)
      if (w.path == path) e.Out() << "  Collegato " << path << " (" << w.objectCount << " oggetti)\n";
    return true;
  }
  if (a == "detach") {
    const size_t before = paths.size();
    paths.erase(std::remove(paths.begin(), paths.end(), path), paths.end());
    if (paths.size() == before) return e.Fail("file non collegato: " + path);
    Store(e, paths);
    e.SyncWorksession();
    e.Out() << "  Scollegato " << path << "\n";
    return true;
  }
  return e.Fail("azione di Worksession non valida: " + *action);
}

}  // namespace

void Engine::SyncWorksession() {
  const auto paths = StoredPaths(*this);
  std::vector<WorksessionFile> next;
  for (const auto& p : paths) {
    auto it = std::find_if(worksession.begin(), worksession.end(), [&](const WorksessionFile& w) { return w.path == p; });
    if (it != worksession.end()) { next.push_back(*it); continue; }
    WorksessionFile w;
    w.path = p;
    auto doc = std::make_shared<Document3dm>();
    if (doc->Load(p)) {
      w.doc = doc;
      ONX_ModelComponentIterator ci(doc->Model(), ON_ModelComponent::Type::ModelGeometry);
      for (const ON_ModelComponent* c = ci.FirstComponent(); c; c = ci.NextComponent()) w.objectCount++;
    }
    next.push_back(std::move(w));
  }
  worksession = std::move(next);
}

void RegisterWorksessionCommands(Engine& e) {
  CommandInfo i{"Worksession", "File", "Collega altri file .3dm come riferimento (Attach, Detach, DetachAll, List, Refresh)", CmdWorksession};
  e.Register(i);
}

}  // namespace cad
