#include "collab/Session.h"
#include "commands/Engine.h"
#include <QDataStream>
#include <QDateTime>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>

namespace cad::collab {
namespace {

constexpr quint32 kMaxFrame = 1024u * 1024u * 1024u;  // 1 GB
constexpr int kMaxBadPin = 5;
constexpr qint64 kBadPinWindowMs = 60 * 1000;
constexpr qint64 kFoundTimeoutMs = 7000;

QByteArray Payload(quint8 type, const std::function<void(QDataStream&)>& fill = {}) {
  QByteArray b;
  QDataStream out(&b, QIODevice::WriteOnly);
  out.setVersion(QDataStream::Qt_6_0);
  out << type;
  if (fill) fill(out);
  return b;
}

QByteArray ToBytes(const std::string& s) { return QByteArray(s.data(), static_cast<qsizetype>(s.size())); }
std::string FromBytes(const QByteArray& b) { return std::string(b.constData(), static_cast<size_t>(b.size())); }

QString Clean(QString s, int max) {
  s = s.simplified();
  if (s.size() > max) s.truncate(max);
  return s;
}

}  // namespace

Session::Session(Engine& e, QString version, QObject* parent)
    : QObject(parent), m_engine(e), m_version(std::move(version)) {}

Session::~Session() {
  Stop(QString(), false);
  StopDiscovery();
}

QStringList Session::LocalAddresses() {
  QStringList out;
  for (const QNetworkInterface& itf : QNetworkInterface::allInterfaces()) {
    const auto flags = itf.flags();
    if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning) ||
        (flags & QNetworkInterface::IsLoopBack))
      continue;
    for (const QNetworkAddressEntry& a : itf.addressEntries())
      if (a.ip().protocol() == QAbstractSocket::IPv4Protocol) out << a.ip().toString();
  }
  return out;
}

// ---------------------------------------------------------------- ospite

QString Session::Host(const QString& myName, const QString& project, quint16 port) {
  Stop(QString(), true);
  m_server = new QTcpServer(this);
  if (!m_server->listen(QHostAddress::AnyIPv4, port)) {
    emit Message(tr("Porta %1 non disponibile: %2").arg(port).arg(m_server->errorString()));
    delete m_server;
    m_server = nullptr;
    return QString();
  }
  connect(m_server, &QTcpServer::newConnection, this, &Session::OnNewConnection);
  m_hosting = true;
  m_myId = 0;
  m_nextId = 1;
  m_myName = Clean(myName, 60);
  m_project = Clean(project, 120);
  m_pin = QString::number(QRandomGenerator::system()->bounded(1000000)).rightJustified(6, '0');
  m_peers = {Peer{0, m_myName}};
  m_base = TakeSnapshot(m_engine.Model());
  m_pending.clear();
  m_badPin.clear();
  // annunci sulla rete locale ogni 2 secondi
  m_sessionTag = QString::number(QRandomGenerator::system()->generate64(), 16);
  m_udpOut = new QUdpSocket(this);
  m_announce = new QTimer(this);
  connect(m_announce, &QTimer::timeout, this, &Session::Announce);
  m_announce->start(2000);
  Announce();
  emit PeersChanged();
  emit Message(tr("Sessione aperta sulla porta %1").arg(m_server->serverPort()));
  return m_pin;
}

void Session::Announce() {
  if (!m_hosting || !m_udpOut || !m_server) return;
  const QByteArray msg = QStringLiteral("K3COLLAB|%1|%2|%3|%4|%5|%6")
                             .arg(kProtocol)
                             .arg(QString(m_myName).replace('|', '/'))
                             .arg(m_server->serverPort())
                             .arg(m_version)
                             .arg(m_sessionTag)
                             .arg(QString(m_project).replace('|', '/'))
                             .toUtf8();
  // a tutte le reti del computer (indirizzo di broadcast di ciascuna) e al computer stesso
  QList<QHostAddress> targets{QHostAddress(QHostAddress::Broadcast), QHostAddress(QHostAddress::LocalHost)};
  for (const QNetworkInterface& itf : QNetworkInterface::allInterfaces()) {
    if (!(itf.flags() & QNetworkInterface::IsUp) || !(itf.flags() & QNetworkInterface::CanBroadcast)) continue;
    for (const QNetworkAddressEntry& a : itf.addressEntries())
      if (a.ip().protocol() == QAbstractSocket::IPv4Protocol && !a.broadcast().isNull() && !targets.contains(a.broadcast()))
        targets << a.broadcast();
  }
  for (const QHostAddress& t : targets) m_udpOut->writeDatagram(msg, t, discoveryPort);
}

void Session::OnNewConnection() {
  while (m_server && m_server->hasPendingConnections()) {
    QTcpSocket* s = m_server->nextPendingConnection();
    s->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    Conn c;
    c.socket = s;
    m_conns[s] = c;
    connect(s, &QTcpSocket::readyRead, this, [this, s] { OnReadyRead(s); });
    connect(s, &QTcpSocket::disconnected, this, [this, s] { OnDisconnected(s); });
    // chi non si presenta entro 15 secondi viene chiuso
    QTimer::singleShot(15000, s, [this, s] {
      auto it = m_conns.find(s);
      if (it != m_conns.end() && it->second.id < 0) s->disconnectFromHost();
    });
  }
}

void Session::HandleAsHost(Conn& c, quint8 type, QDataStream& in) {
  if (c.id < 0) {
    if (type != Hello) return;
    QString name, pin, version;
    qint32 protocol = 0;
    in >> protocol >> name >> pin >> version;
    const QString addr = c.socket->peerAddress().toString();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    auto bad = m_badPin.value(addr, {0, 0});
    if (now - bad.second > kBadPinWindowMs) bad = {0, 0};
    QString reason;
    if (bad.first >= kMaxBadPin) reason = tr("troppi codici sbagliati, riprova tra un minuto");
    else if (protocol != kProtocol || version != m_version)
      reason = tr("versione diversa (ospite %1, tu %2): serve la stessa versione di Kurva3D").arg(m_version, version);
    else if (pin != m_pin) {
      m_badPin[addr] = {bad.first + 1, now};
      reason = tr("codice sbagliato");
    }
    if (!reason.isEmpty()) {
      Send(c.socket, Payload(Reject, [&](QDataStream& o) { o << reason; }));
      c.socket->disconnectFromHost();
      return;
    }
    LocalChanged();  // le proprie modifiche non ancora inviate entrano nel modello di partenza
    c.id = m_nextId++;
    c.name = Clean(name, 60);
    if (c.name.isEmpty()) c.name = tr("Partecipante %1").arg(c.id);
    m_peers << Peer{c.id, c.name};
    const auto buf = m_engine.Doc().ToBuffer();
    const QByteArray doc(reinterpret_cast<const char*>(buf.data()), static_cast<qsizetype>(buf.size()));
    Send(c.socket, Payload(Welcome, [&](QDataStream& o) { o << qint32(c.id) << m_project << doc; }));
    Broadcast(PeersPayload());
    emit PeersChanged();
    emit Message(tr("%1 e' entrato nella sessione").arg(c.name));
    return;
  }
  switch (type) {
    case PatchMsg: {
      QByteArray patch;
      in >> patch;
      LocalChanged();  // prima le proprie modifiche, nell'ordine in cui sono state fatte
      ApplyRemotePatch(patch);
      // agli altri la modifica, a chi l'ha fatta la conferma (stesso ordine per tutti)
      const QByteArray fwd = Payload(PatchMsg, [&](QDataStream& o) { o << qint32(c.id) << patch; });
      Broadcast(fwd, c.id);
      Send(c.socket, Payload(Ack));
      break;
    }
    case ChatMsg: {
      QString text;
      in >> text;
      text = text.left(4000);
      Broadcast(Payload(ChatMsg, [&](QDataStream& o) { o << c.name << text; }), c.id);
      emit Chat(c.name, text);
      break;
    }
    case AiTaskMsg: {
      qint32 target = 0;
      QString text;
      in >> target >> text;
      text = text.left(4000);
      if (target == 0) emit AiTask(c.name, text);
      else
        for (auto& [s, other] : m_conns)
          if (other.id == target) Send(s, Payload(AiTaskMsg, [&](QDataStream& o) { o << c.name << text; }));
      break;
    }
    default:
      break;
  }
}

QByteArray Session::PeersPayload() const {
  return Payload(Peers, [&](QDataStream& o) {
    o << qint32(m_peers.size());
    for (const Peer& p : m_peers) o << qint32(p.id) << p.name;
  });
}

void Session::Broadcast(const QByteArray& payload, int exceptId) {
  for (auto& [s, c] : m_conns)
    if (c.id >= 0 && c.id != exceptId) Send(s, payload);
}

// ---------------------------------------------------------------- partecipante

void Session::Join(const QString& address, quint16 port, const QString& pin, const QString& myName) {
  Stop(QString(), true);
  m_myName = Clean(myName, 60);
  m_host = new QTcpSocket(this);
  m_host->setSocketOption(QAbstractSocket::LowDelayOption, 1);
  m_connecting = true;
  Conn c;
  c.socket = m_host;
  c.id = 0;
  m_conns[m_host] = c;
  QTcpSocket* s = m_host;
  connect(s, &QTcpSocket::connected, this, [this, s, pin] {
    Send(s, Payload(Hello, [&](QDataStream& o) { o << qint32(kProtocol) << m_myName << pin << m_version; }));
  });
  connect(s, &QTcpSocket::readyRead, this, [this, s] { OnReadyRead(s); });
  connect(s, &QTcpSocket::disconnected, this, [this, s] { OnDisconnected(s); });
  connect(s, &QTcpSocket::errorOccurred, this, [this, s](QAbstractSocket::SocketError) {
    if (m_connecting && s == m_host) Stop(tr("connessione non riuscita: %1").arg(s->errorString()), true);
  });
  emit Message(tr("Connessione a %1:%2...").arg(address).arg(port));
  s->connectToHost(address, port);
}

void Session::HandleAsClient(quint8 type, QDataStream& in) {
  switch (type) {
    case Welcome: {
      qint32 id = 0;
      QByteArray doc;
      in >> id >> m_project >> doc;
      std::vector<unsigned char> buf(doc.begin(), doc.end());
      if (!m_engine.Doc().FromBuffer(buf)) {
        Stop(tr("modello dell'ospite illeggibile"), true);
        return;
      }
      m_engine.ClearUndo();
      m_engine.ClearSelection();
      m_myId = id;
      m_joined = true;
      m_connecting = false;
      m_base = TakeSnapshot(m_engine.Model());
      m_pending.clear();
      emit DocumentReplaced();
      emit Message(tr("Sei entrato nella sessione"));
      emit Joined();
      break;
    }
    case Reject: {
      QString reason;
      in >> reason;
      Stop(tr("accesso rifiutato: %1").arg(reason), true);
      break;
    }
    case PatchMsg: {
      qint32 from = 0;
      QByteArray patch;
      in >> from >> patch;
      LocalChanged();
      ApplyRemotePatch(patch);
      break;
    }
    case Ack:
      if (!m_pending.empty()) m_pending.pop_front();
      break;
    case ChatMsg: {
      QString from, text;
      in >> from >> text;
      emit Chat(from, text);
      break;
    }
    case AiTaskMsg: {
      QString from, text;
      in >> from >> text;
      emit AiTask(from, text);
      break;
    }
    case Peers: {
      qint32 n = 0;
      in >> n;
      m_peers.clear();
      for (int i = 0; i < n && i < 1000; i++) {
        Peer p;
        qint32 id = 0;
        in >> id >> p.name;
        p.id = id;
        m_peers << p;
      }
      emit PeersChanged();
      break;
    }
    case Bye: {
      QString reason;
      in >> reason;
      Stop(reason, true);
      break;
    }
    default:
      break;
  }
}

// ---------------------------------------------------------------- comune

void Session::ApplyRemotePatch(const QByteArray& patch) {
  const std::string p = FromBytes(patch);
  std::string err;
  const bool ok = m_engine.ApplyRemote([p](ONX_Model& m) { return ApplyPatch(m, p); });
  if (!ok) {
    emit Message(tr("modifica ricevuta non applicabile"));
    return;
  }
  // le proprie modifiche non ancora confermate dall'ospite arrivano dopo questa: si riapplicano
  for (const std::string& mine : m_pending) ApplyPatch(m_engine.Model(), mine);
  m_base = TakeSnapshot(m_engine.Model());
  patchesApplied++;
  emit ModelChanged();
}

void Session::LocalChanged() {
  if (!Active() || (!m_hosting && !m_joined)) return;
  PatchInfo info;
  Snapshot now;
  const std::string p = MakePatch(m_engine.Model(), m_base, &info, &now);
  if (info.Empty()) return;
  m_base = std::move(now);
  patchesSent++;
  if (m_hosting) {
    Broadcast(Payload(PatchMsg, [&](QDataStream& o) { o << qint32(0) << ToBytes(p); }));
  } else if (m_host) {
    m_pending.push_back(p);
    Send(m_host, Payload(PatchMsg, [&](QDataStream& o) { o << ToBytes(p); }));
  }
}

void Session::SendChat(const QString& text) {
  if (!Active() || text.trimmed().isEmpty()) return;
  if (m_hosting) Broadcast(Payload(ChatMsg, [&](QDataStream& o) { o << m_myName << text; }));
  else if (m_host) Send(m_host, Payload(ChatMsg, [&](QDataStream& o) { o << text; }));
  emit Chat(m_myName, text);
}

void Session::SendAiTask(int target, const QString& text) {
  if (text.trimmed().isEmpty()) return;
  if (!Active() || target == m_myId) {
    emit AiTask(m_myName, text);
    return;
  }
  if (m_hosting) {
    for (auto& [s, c] : m_conns)
      if (c.id == target) Send(s, Payload(AiTaskMsg, [&](QDataStream& o) { o << m_myName << text; }));
  } else if (m_host) {
    Send(m_host, Payload(AiTaskMsg, [&](QDataStream& o) { o << qint32(target) << text; }));
  }
  emit Message(tr("Compito inviato all'AI di %1").arg(PeerName(target)));
}

QString Session::PeerName(int id) const {
  for (const Peer& p : m_peers)
    if (p.id == id) return p.name;
  return id == 0 ? tr("ospite") : QString::number(id);
}

void Session::Send(QTcpSocket* s, const QByteArray& payload) {
  if (!s || s->state() != QAbstractSocket::ConnectedState) return;
  QByteArray frame;
  QDataStream out(&frame, QIODevice::WriteOnly);
  out << quint32(payload.size());
  frame.append(payload);
  s->write(frame);
}

void Session::OnReadyRead(QTcpSocket* s) {
  auto it = m_conns.find(s);
  if (it == m_conns.end()) return;
  it->second.buffer.append(s->readAll());
  for (;;) {
    it = m_conns.find(s);  // Handle puo' chiudere la connessione
    if (it == m_conns.end()) return;
    QByteArray& buf = it->second.buffer;
    if (buf.size() < 4) return;
    QDataStream hdr(buf);
    quint32 len = 0;
    hdr >> len;
    if (len > kMaxFrame || (it->second.id < 0 && len > 64 * 1024)) {  // prima del codice: messaggi piccoli
      s->abort();
      return;
    }
    if (static_cast<quint32>(buf.size()) < 4 + len) return;
    const QByteArray frame = buf.mid(4, len);
    buf.remove(0, 4 + static_cast<qsizetype>(len));
    Handle(it->second, frame);
  }
}

void Session::Handle(Conn& c, const QByteArray& frame) {
  QDataStream in(frame);
  in.setVersion(QDataStream::Qt_6_0);
  quint8 type = 0;
  in >> type;
  if (m_hosting) HandleAsHost(c, type, in);
  else HandleAsClient(type, in);
}

void Session::OnDisconnected(QTcpSocket* s) {
  auto it = m_conns.find(s);
  if (it == m_conns.end()) return;
  const int id = it->second.id;
  const QString name = it->second.name;
  m_conns.erase(it);
  s->deleteLater();
  if (m_hosting) {
    if (id < 0) return;
    for (int i = 0; i < m_peers.size(); i++)
      if (m_peers[i].id == id) m_peers.removeAt(i--);
    Broadcast(PeersPayload());
    emit PeersChanged();
    emit Message(tr("%1 ha lasciato la sessione").arg(name));
  } else if (s == m_host) {
    m_host = nullptr;
    if (m_joined || m_connecting) Stop(tr("connessione con l'ospite chiusa"), true);
  }
}

void Session::Leave() { Stop(tr("hai lasciato la sessione"), true); }

void Session::Stop(const QString& reason, bool notify) {
  const bool was = m_hosting || m_joined || m_connecting;
  if (m_hosting) {
    Broadcast(Payload(Bye, [&](QDataStream& o) { o << tr("l'ospite ha chiuso la sessione"); }));
    for (auto& [s, c] : m_conns) s->flush();
  }
  auto conns = std::move(m_conns);
  m_conns.clear();
  for (auto& [s, c] : conns) {
    s->disconnect(this);
    s->disconnectFromHost();
    s->deleteLater();
  }
  m_host = nullptr;
  if (m_server) {
    m_server->close();
    m_server->deleteLater();
    m_server = nullptr;
  }
  if (m_announce) {
    m_announce->stop();
    m_announce->deleteLater();
    m_announce = nullptr;
  }
  if (m_udpOut) {
    m_udpOut->deleteLater();
    m_udpOut = nullptr;
  }
  m_hosting = m_joined = m_connecting = false;
  m_pin.clear();
  m_peers.clear();
  m_pending.clear();
  m_myId = 0;
  if (was && notify) {
    emit PeersChanged();
    emit Ended(reason);
  }
}

// ---------------------------------------------------------------- ricerca

bool Session::StartDiscovery(quint16 port) {
  if (m_udp) return true;
  m_udp = new QUdpSocket(this);
  if (!m_udp->bind(QHostAddress::AnyIPv4, port, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
    emit Message(tr("ricerca automatica non disponibile (porta %1 occupata): inserisci l'indirizzo IP").arg(port));
    delete m_udp;
    m_udp = nullptr;
    return false;
  }
  connect(m_udp, &QUdpSocket::readyRead, this, &Session::ReadDiscovery);
  return true;
}

void Session::StopDiscovery() {
  delete m_udp;
  m_udp = nullptr;
  m_found.clear();
}

void Session::ReadDiscovery() {
  bool changed = false;
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  while (m_udp && m_udp->hasPendingDatagrams()) {
    QByteArray data(static_cast<qsizetype>(m_udp->pendingDatagramSize()), 0);
    QHostAddress from;
    m_udp->readDatagram(data.data(), data.size(), &from);
    const QStringList parts = QString::fromUtf8(data).split('|');
    if (parts.size() < 6 || parts[0] != QLatin1String("K3COLLAB") || parts[1].toInt() != kProtocol) continue;
    if (m_hosting && parts[5] == m_sessionTag) continue;  // il proprio annuncio
    FoundSession f;
    f.name = parts[2].left(60);
    f.port = static_cast<quint16>(parts[3].toUInt());
    f.version = parts[4].left(30);
    f.project = parts.size() > 6 ? parts[6].left(120) : QString();
    QHostAddress v4(from.toIPv4Address());
    f.address = from.protocol() == QAbstractSocket::IPv6Protocol && !v4.isNull() ? v4.toString() : from.toString();
    f.seenMs = now;
    const QString key = parts[5];  // stessa sessione da piu' reti/indirizzi: una voce sola
    auto it = m_found.find(key);
    if (it == m_found.end() || it->second.name != f.name || it->second.project != f.project) changed = true;
    // l'indirizzo locale (stesso computer) vale solo se non c'e' quello di rete
    if (it != m_found.end() && QHostAddress(f.address).isLoopback() && !QHostAddress(it->second.address).isLoopback())
      f.address = it->second.address;
    m_found[key] = f;
  }
  for (auto it = m_found.begin(); it != m_found.end();) {
    if (now - it->second.seenMs > kFoundTimeoutMs) {
      it = m_found.erase(it);
      changed = true;
    } else
      ++it;
  }
  if (changed) emit DiscoveredChanged();
}

QList<FoundSession> Session::Discovered() const {
  QList<FoundSession> out;
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  for (const auto& [k, f] : m_found)
    if (now - f.seenMs <= kFoundTimeoutMs) out << f;
  return out;
}

}  // namespace cad::collab

namespace cad::collab {
quint16 Session::Port() const { return m_server ? m_server->serverPort() : 0; }
}  // namespace cad::collab
