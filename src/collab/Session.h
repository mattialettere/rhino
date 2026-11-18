// Sessione di collaborazione in rete locale (Qt Network, senza interfaccia grafica).
//
// Un computer "ospita" il progetto: annuncia la sessione sulla rete locale (UDP, porta 45454)
// e accetta connessioni TCP (porta 45455) da chi conosce il codice a 6 cifre. Chi entra riceve
// il modello completo; poi ogni modifica (patch per oggetto, vedi Patch.h) passa dall'ospite,
// che la applica e la inoltra agli altri nello stesso ordine per tutti. Chi ha mandato una
// modifica non ancora confermata la riapplica sopra quelle arrivate prima: tutti convergono
// allo stesso modello (sullo stesso oggetto vince l'ultima modifica arrivata all'ospite).
// Messaggi di chat e "compiti" per l'assistente AI di un partecipante viaggiano sulla stessa
// connessione.
#pragma once
#include "collab/Patch.h"
#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QString>
#include <deque>
#include <map>

class QTcpServer;
class QTcpSocket;
class QTimer;
class QUdpSocket;

namespace cad {
class Engine;
}

namespace cad::collab {

constexpr quint16 kDiscoveryPort = 45454;
constexpr quint16 kSessionPort = 45455;
constexpr int kProtocol = 1;

struct Peer {
  int id = 0;  // 0 = ospite
  QString name;
};

struct FoundSession {
  QString name;     // nome di chi ospita
  QString project;  // nome del file
  QString address;
  quint16 port = 0;
  QString version;
  qint64 seenMs = 0;
};

class Session : public QObject {
  Q_OBJECT
 public:
  Session(Engine& e, QString version, QObject* parent = nullptr);
  ~Session() override;

  // --- ospitare: restituisce il codice a 6 cifre (vuoto se la porta non e' disponibile)
  QString Host(const QString& myName, const QString& project, quint16 port = kSessionPort);
  // --- entrare in una sessione (indirizzo IP o nome del computer)
  void Join(const QString& address, quint16 port, const QString& pin, const QString& myName);
  void Leave();

  bool Active() const { return m_hosting || m_joined; }
  bool IsHost() const { return m_hosting; }
  bool Connecting() const { return m_connecting; }
  QString Pin() const { return m_pin; }
  quint16 Port() const;
  int MyId() const { return m_myId; }
  QString MyName() const { return m_myName; }
  QList<Peer> Participants() const { return m_peers; }
  // indirizzi IPv4 di questo computer (da comunicare a chi entra a mano)
  static QStringList LocalAddresses();

  // --- ricerca delle sessioni sulla rete locale
  bool StartDiscovery(quint16 port = kDiscoveryPort);
  void StopDiscovery();
  QList<FoundSession> Discovered() const;
  quint16 discoveryPort = kDiscoveryPort;  // porta degli annunci dell'ospite

  // --- da chiamare dopo ogni modifica locale (comando, annulla, interfaccia): invia la patch
  void LocalChanged();
  void SendChat(const QString& text);
  // Compito per l'assistente AI del partecipante "target" (anche se stessi)
  void SendAiTask(int target, const QString& text);

  // statistiche (test)
  int patchesSent = 0, patchesApplied = 0;

 signals:
  void ModelChanged();       // modifiche ricevute applicate: ridisegnare
  void DocumentReplaced();   // entrando si riceve il modello dell'ospite
  void PeersChanged();
  void DiscoveredChanged();
  void Chat(QString from, QString text);
  void AiTask(QString from, QString text);  // compito per la propria AI
  void Message(QString text);               // stato per l'utente
  void Joined();
  void Ended(QString reason);

 private:
  enum Type : quint8 { Hello = 1, Welcome, Reject, PatchMsg, Ack, ChatMsg, AiTaskMsg, Peers, Bye };
  struct Conn {
    QTcpSocket* socket = nullptr;
    QByteArray buffer;
    int id = -1;  // -1 = non ancora autenticato
    QString name;
  };
  void OnNewConnection();
  void OnReadyRead(QTcpSocket* s);
  void OnDisconnected(QTcpSocket* s);
  void Handle(Conn& c, const QByteArray& frame);
  void HandleAsHost(Conn& c, quint8 type, QDataStream& in);
  void HandleAsClient(quint8 type, QDataStream& in);
  void Send(QTcpSocket* s, const QByteArray& payload);
  void Broadcast(const QByteArray& payload, int exceptId = -1);
  QByteArray PeersPayload() const;
  void ApplyRemotePatch(const QByteArray& patch);
  void Announce();
  void ReadDiscovery();
  void Stop(const QString& reason, bool notify);
  QString PeerName(int id) const;

  Engine& m_engine;
  QString m_version, m_myName, m_project, m_pin;
  QTcpServer* m_server = nullptr;
  std::map<QTcpSocket*, Conn> m_conns;
  QTcpSocket* m_host = nullptr;  // connessione all'ospite (lato partecipante)
  bool m_hosting = false, m_joined = false, m_connecting = false;
  int m_myId = 0, m_nextId = 1;
  QList<Peer> m_peers;
  Snapshot m_base;                   // stato gia' condiviso
  std::deque<std::string> m_pending;  // patch inviate e non ancora confermate (partecipante)
  // ricerca
  QUdpSocket* m_udp = nullptr;      // annunci ricevuti
  QUdpSocket* m_udpOut = nullptr;   // annunci inviati (ospite)
  QTimer* m_announce = nullptr;
  QString m_sessionTag;
  std::map<QString, FoundSession> m_found;  // chiave: indirizzo:porta
  QHash<QString, QPair<int, qint64>> m_badPin;  // tentativi sbagliati per indirizzo (numero, ultimo)
};

}  // namespace cad::collab
