// Test della collaborazione in rete locale: un ospite e due partecipanti nello stesso processo,
// collegati via TCP su questo computer. Ricerca automatica, codice sbagliato, modello iniziale,
// modifiche in tutte le direzioni, modifiche contemporanee sullo stesso oggetto, livelli,
// chat, compiti per l'AI, Annulla e chiusura della sessione.
#include "collab/Session.h"
#include "commands/Engine.h"
#include "core/Document3dm.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <cstdio>
#include <functional>
#include <memory>
#include <sstream>

using namespace cad;
using namespace cad::collab;

static int g_checks = 0, g_failures = 0;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failures++; \
  std::printf("  FALLITO %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

struct Node {
  Document3dm doc;
  std::ostringstream log;
  Engine e{doc, log};
  std::unique_ptr<Session> s;
  QStringList chats, tasks, messages;
  QString ended;
  bool joined = false;
  explicit Node(const QString& name) {
    s = std::make_unique<Session>(e, "1.0-test");
    QObject::connect(s.get(), &Session::Chat, [this](QString f, QString t) { chats << f + ": " + t; });
    QObject::connect(s.get(), &Session::AiTask, [this](QString f, QString t) { tasks << f + ": " + t; });
    QObject::connect(s.get(), &Session::Message, [this](QString t) { messages << t; });
    QObject::connect(s.get(), &Session::Ended, [this](QString r) { ended = r; });
    QObject::connect(s.get(), &Session::Joined, [this] { joined = true; });
    (void)name;
  }
  bool Run(const std::string& script) {
    const bool ok = e.Run(script);
    if (!ok) std::printf("  comando non riuscito: %s (%s)\n", script.c_str(), e.LastError().c_str());
    s->LocalChanged();
    return ok;
  }
  size_t Count() { return e.AllObjectIds().size(); }
};

static bool WaitFor(const std::function<bool()>& pred, int ms = 5000) {
  QElapsedTimer t;
  t.start();
  while (!pred()) {
    if (t.elapsed() > ms) return false;
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  }
  return true;
}

static void Settle(int ms = 300) {
  QElapsedTimer t;
  t.start();
  while (t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

static bool SameModel(Node& a, Node& b) {
  const Snapshot x = TakeSnapshot(a.e.Model()), y = TakeSnapshot(b.e.Model());
  return x.objects == y.objects && x.userStrings == y.userStrings && x.tables.size() == y.tables.size();
}

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  QCoreApplication app(argc, argv);
  InitLibrary();
  {
    Node host("Ospite"), a("Anna"), b("Bruno");
    CHECK(host.Run("_Line 0,0 10,0"));

    std::printf("Ospitare\n");
    host.s->discoveryPort = 45999;
    const QString pin = host.s->Host("Ospite", "progetto.3dm", 0);
    CHECK(pin.size() == 6 && host.s->IsHost());
    const quint16 port = host.s->Port();
    CHECK(port != 0);

    std::printf("Ricerca automatica\n");
    if (a.s->StartDiscovery(45999)) {
      CHECK(WaitFor([&] { return !a.s->Discovered().isEmpty(); }, 6000));
      if (!a.s->Discovered().isEmpty()) {
        const FoundSession f = a.s->Discovered().first();
        CHECK(f.name == "Ospite" && f.port == port && f.project == "progetto.3dm" && f.version == "1.0-test");
      }
    } else {
      std::printf("  (porta UDP non disponibile: ricerca non provata)\n");
    }

    std::printf("Codice sbagliato\n");
    QString wrong = pin == "000000" ? "111111" : "000000";
    a.s->Join("127.0.0.1", port, wrong, "Anna");
    CHECK(WaitFor([&] { return !a.ended.isEmpty(); }));
    CHECK(a.ended.contains("codice sbagliato") && !a.s->Active());

    std::printf("Versione diversa\n");
    {
      Document3dm d;
      std::ostringstream l;
      Engine e(d, l);
      Session old(e, "0.9");
      QString why;
      QObject::connect(&old, &Session::Ended, [&](QString r) { why = r; });
      old.Join("127.0.0.1", port, pin, "Vecchio");
      CHECK(WaitFor([&] { return !why.isEmpty(); }));
      CHECK(why.contains("versione"));
    }

    std::printf("Entrare\n");
    a.ended.clear();
    a.s->Join("127.0.0.1", port, pin, "Anna");
    CHECK(WaitFor([&] { return a.joined; }));
    CHECK(a.Count() == 1 && SameModel(host, a));
    b.s->Join("127.0.0.1", port, pin, "Bruno");
    CHECK(WaitFor([&] { return b.joined; }));
    CHECK(WaitFor([&] { return host.s->Participants().size() == 3 && a.s->Participants().size() == 3; }));
    CHECK(b.s->MyId() == 2 && a.s->MyId() == 1);

    std::printf("Modifiche in tutte le direzioni\n");
    CHECK(a.Run("_Circle 0,0 5"));
    CHECK(WaitFor([&] { return host.Count() == 2 && b.Count() == 2; }));
    CHECK(host.Run("_SelAll _SelNone\n_Rectangle 20,0 30,10"));
    CHECK(WaitFor([&] { return a.Count() == 3 && b.Count() == 3; }));
    // eliminazione dal partecipante B
    {
      auto ids = b.e.AllObjectIds();
      b.e.Select(ids[0]);
      CHECK(b.Run("_Delete"));
    }
    CHECK(WaitFor([&] { return host.Count() == 2 && a.Count() == 2; }));
    Settle();
    CHECK(SameModel(host, a) && SameModel(host, b));

    std::printf("Livelli\n");
    CHECK(b.Run("-_Layer _New Muri"));
    Settle();
    {
      const ON_ModelComponent* l = host.e.Model().ComponentFromName(ON_ModelComponent::Type::Layer, ON_nil_uuid, L"Muri").ModelComponent();
      CHECK(l != nullptr);
    }
    CHECK(SameModel(host, b) && SameModel(a, b));

    std::printf("Modifiche contemporanee sullo stesso oggetto\n");
    {
      ON_UUID circle = ON_nil_uuid;
      for (const auto& id : host.e.AllObjectIds())
        if (ON_ArcCurve::Cast(host.e.Object(id)->Geometry(nullptr))) circle = id;
      CHECK(!ON_UuidIsNil(circle));
      host.e.ClearSelection();
      a.e.ClearSelection();
      b.e.ClearSelection();
      host.e.Select(circle);
      a.e.Select(circle);
      b.e.Select(circle);
      // nessun evento elaborato tra le tre modifiche: arrivano all'ospite in un ordine qualsiasi
      CHECK(host.Run("_Move 0,0 0,100"));
      CHECK(a.Run("_Move 0,0 0,-100"));
      CHECK(b.Run("_Move 0,0 50,0"));
      Settle(800);
      CHECK(SameModel(host, a) && SameModel(host, b));
      ON_BoundingBox bb = host.e.Object(circle)->Geometry(nullptr)->BoundingBox();
      std::printf("  centro finale %g,%g\n", bb.Center().x, bb.Center().y);
    }

    std::printf("Raffica di modifiche\n");
    for (int i = 0; i < 10; i++) {
      CHECK(a.Run("_Point " + std::to_string(i) + ",50"));
      CHECK(b.Run("_Point " + std::to_string(i) + ",60"));
    }
    Settle(800);
    CHECK(host.Count() == a.Count() && a.Count() == b.Count() && host.Count() == 22);
    CHECK(SameModel(host, a) && SameModel(host, b));

    std::printf("Annulla\n");
    {
      const size_t n = host.Count();
      CHECK(host.Run("_Line 0,0 0,-50"));
      Settle();
      CHECK(a.Count() == n + 1);
      CHECK(a.Run("_Point 99,99"));
      Settle();
      CHECK(host.Count() == n + 2);
      // l'ospite annulla la sua linea: il punto di Anna resta
      CHECK(host.Run("_Undo"));
      Settle();
      CHECK(host.Count() == n + 1 && a.Count() == n + 1 && b.Count() == n + 1);
      CHECK(SameModel(host, a) && SameModel(host, b));
    }

    std::printf("Chat e compiti per l'AI\n");
    b.s->SendChat("ciao a tutti");
    CHECK(WaitFor([&] { return !host.chats.isEmpty() && !a.chats.isEmpty(); }));
    CHECK(host.chats.value(0) == "Bruno: ciao a tutti" && a.chats.value(0) == "Bruno: ciao a tutti");
    CHECK(b.chats.value(0) == "Bruno: ciao a tutti");
    a.s->SendAiTask(2, "aggiungi una sfera di raggio 3");
    CHECK(WaitFor([&] { return !b.tasks.isEmpty(); }));
    CHECK(b.tasks.value(0) == "Anna: aggiungi una sfera di raggio 3" && a.tasks.isEmpty() && host.tasks.isEmpty());
    host.s->SendAiTask(1, "colora di rosso");
    CHECK(WaitFor([&] { return !a.tasks.isEmpty(); }));
    b.s->SendAiTask(0, "quota tutto");
    CHECK(WaitFor([&] { return !host.tasks.isEmpty(); }));
    CHECK(host.tasks.value(0) == "Bruno: quota tutto");

    std::printf("Uscita di un partecipante e chiusura\n");
    b.s->Leave();
    CHECK(WaitFor([&] { return host.s->Participants().size() == 2 && a.s->Participants().size() == 2; }));
    host.s->Leave();
    CHECK(WaitFor([&] { return !a.ended.isEmpty(); }));
    CHECK(!a.s->Active() && a.Count() == host.Count());
  }
  ShutdownLibrary();
  std::printf("\n%d controlli, %d falliti\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
