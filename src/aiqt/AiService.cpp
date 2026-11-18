#include "aiqt/AiService.h"
#include "commands/Engine.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTimer>
#include <QUrl>

namespace cad::aiqt {

namespace {
// HF_ENDPOINT (come negli strumenti di Hugging Face) permette un mirror o un server di prova
QString Hub() {
  const QString e = QProcessEnvironment::systemEnvironment().value("HF_ENDPOINT");
  return e.isEmpty() ? QString("https://huggingface.co") : e;
}

QNetworkRequest Req(const QString& url) {
  QNetworkRequest r{QUrl(url)};
  r.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
  r.setHeader(QNetworkRequest::UserAgentHeader, "Kurva3D");
  return r;
}

QString HumanSize(qint64 b) { return QString::number(double(b) / (1024.0 * 1024.0 * 1024.0), 'f', 2) + " GB"; }
}  // namespace

QString ModelsDir() {
  // stessa cartella per il programma e per gli strumenti (Windows: %LOCALAPPDATA%\Kurva3D\models)
  QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
  if (base.isEmpty()) base = QDir::homePath();
  return base + "/Kurva3D/models";
}

QString InstalledModelPath(const ai::ModelInfo& m) {
  const QString dir = ModelsDir() + "/" + QString::fromStdString(m.id);
  QFile meta(dir + "/model.json");
  if (!meta.open(QIODevice::ReadOnly)) return {};
  const QJsonObject o = QJsonDocument::fromJson(meta.readAll()).object();
  const QString path = dir + "/" + o.value("file").toString();
  const QFileInfo fi(path);
  if (!fi.exists() || fi.size() != (qint64)o.value("size").toDouble()) return {};
  return path;
}

// --- ModelDownloader -------------------------------------------------------------------------
ModelDownloader::ModelDownloader(QObject* parent) : QObject(parent) {}

void ModelDownloader::Fail(const QString& error) {
  m_busy = false;
  if (m_out) { m_out->close(); m_out->remove(); m_out.reset(); }
  emit Finished(false, error);
}

void ModelDownloader::Start(const ai::ModelInfo& m) {
  if (m_busy) return;
  m_busy = true;
  m_model = m;
  m_file.clear();
  m_sha256.clear();
  m_size = 0;
  GetInfo();
}

void ModelDownloader::Cancel() {
  if (m_reply) m_reply->abort();
}

void ModelDownloader::GetInfo() {
  emit Status("Controllo della licenza del modello…");
  m_reply = m_net.get(Req(QString("%1/api/models/%2").arg(Hub(), QString::fromStdString(m_model.repo))));
  connect(m_reply, &QNetworkReply::finished, this, [this] {
    QNetworkReply* r = m_reply;
    r->deleteLater();
    if (r->error() != QNetworkReply::NoError) return Fail("Hugging Face non raggiungibile: " + r->errorString());
    const QJsonObject o = QJsonDocument::fromJson(r->readAll()).object();
    QString lic = o.value("cardData").toObject().value("license").toString();
    if (lic.isEmpty())
      for (const auto& t : o.value("tags").toArray())
        if (t.toString().startsWith("license:")) lic = t.toString().mid(8);
    m_license = lic;
    if (!ai::LicenseAllowed(lic.toStdString()))
      return Fail("licenza del modello non ammessa (" + (lic.isEmpty() ? QString("sconosciuta") : lic) +
                  "): sono consentite solo Apache 2.0 e MIT");
    GetTree();
  });
}

void ModelDownloader::GetTree() {
  emit Status("Ricerca del file del modello…");
  m_reply = m_net.get(Req(QString("%1/api/models/%2/tree/main").arg(Hub(), QString::fromStdString(m_model.repo))));
  connect(m_reply, &QNetworkReply::finished, this, [this] {
    QNetworkReply* r = m_reply;
    r->deleteLater();
    if (r->error() != QNetworkReply::NoError) return Fail("elenco dei file non disponibile: " + r->errorString());
    const QJsonArray files = QJsonDocument::fromJson(r->readAll()).array();
    for (const auto& q : m_model.quants) {
      for (const auto& v : files) {
        const QJsonObject f = v.toObject();
        const QString path = f.value("path").toString();
        if (!path.endsWith(".gguf", Qt::CaseInsensitive) || path.contains('/')) continue;
        if (!path.endsWith("-" + QString::fromStdString(q) + ".gguf", Qt::CaseInsensitive)) continue;
        const QJsonObject lfs = f.value("lfs").toObject();
        m_file = path;
        m_sha256 = lfs.value("oid").toString().toLower();
        m_size = (qint64)(lfs.contains("size") ? lfs.value("size").toDouble() : f.value("size").toDouble());
        break;
      }
      if (!m_file.isEmpty()) break;
    }
    if (m_file.isEmpty() || m_sha256.size() != 64) return Fail("file GGUF del modello non trovato nel repository");
    Download();
  });
}

void ModelDownloader::Download() {
  const QString dir = ModelsDir() + "/" + QString::fromStdString(m_model.id);
  QDir().mkpath(dir);
  m_out = std::make_unique<QFile>(dir + "/" + m_file + ".part");
  if (!m_out->open(QIODevice::WriteOnly | QIODevice::Truncate)) return Fail("impossibile scrivere in " + dir);
  m_hash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
  emit Status("Download di " + m_file + " (" + HumanSize(m_size) + ")…");
  QNetworkRequest rq = Req(QString("%1/%2/resolve/main/%3").arg(Hub(), QString::fromStdString(m_model.repo), m_file));
  m_reply = m_net.get(rq);
  connect(m_reply, &QNetworkReply::readyRead, this, [this] {
    const QByteArray b = m_reply->readAll();
    m_hash->addData(b);
    if (m_out->write(b) != b.size()) m_reply->abort();
  });
  connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 d, qint64 t) { emit Progress(d, t > 0 ? t : m_size); });
  connect(m_reply, &QNetworkReply::finished, this, [this, dir] {
    QNetworkReply* r = m_reply;
    r->deleteLater();
    const QByteArray rest = r->readAll();
    if (!rest.isEmpty()) { m_hash->addData(rest); m_out->write(rest); }
    if (r->error() != QNetworkReply::NoError)
      return Fail(r->error() == QNetworkReply::OperationCanceledError ? "download annullato" : "download interrotto: " + r->errorString());
    m_out->close();
    emit Status("Verifica dell'integrità…");
    const QString got = QString::fromLatin1(m_hash->result().toHex());
    if (m_out->size() != m_size || got != m_sha256) return Fail("il file scaricato non corrisponde (SHA-256 diverso): riprova");
    const QString final = dir + "/" + m_file;
    QFile::remove(final);
    if (!m_out->rename(final)) return Fail("impossibile salvare il modello");
    m_out.reset();
    QJsonObject meta{{"repo", QString::fromStdString(m_model.repo)}, {"file", m_file}, {"sha256", m_sha256},
                     {"size", double(m_size)}, {"license", m_license}};
    QFile mf(dir + "/model.json");
    if (mf.open(QIODevice::WriteOnly | QIODevice::Truncate)) mf.write(QJsonDocument(meta).toJson());
    m_busy = false;
    emit Finished(true, final);
  });
}

// --- LlamaServer ------------------------------------------------------------------------------
LlamaServer::LlamaServer(QObject* parent) : QObject(parent) {
  m_proc.setProcessChannelMode(QProcess::MergedChannels);
  connect(&m_proc, &QProcess::readyRead, this, [this] {
    m_log += QString::fromUtf8(m_proc.readAll());
    if (m_log.size() > 200000) m_log = m_log.right(100000);
  });
  connect(&m_proc, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
    const bool wasStarting = !m_ready && m_polls >= 0;
    m_ready = false;
    if (wasStarting) {
      m_polls = -1;
      emit Started(false, QString("llama-server si e' chiuso (codice %1)").arg(code));
    }
  });
}

LlamaServer::~LlamaServer() { Stop(); }

QString LlamaServer::Executable() {
  const QString env = QProcessEnvironment::systemEnvironment().value("KURVA3D_LLAMA_SERVER");
  if (!env.isEmpty() && QFileInfo::exists(env)) return env;
  const QString dir = QCoreApplication::applicationDirPath();
#ifdef Q_OS_WIN
  const QString name = "llama-server.exe";
#else
  const QString name = "llama-server";
#endif
  for (const QString& c : {dir + "/llama/" + name, dir + "/" + name, dir + "/../Resources/llama/" + name})
    if (QFileInfo::exists(c)) return c;
  return QStandardPaths::findExecutable(name);
}

void LlamaServer::Start(const QString& modelPath, int threads) {
  Stop();
  const QString exe = Executable();
  if (exe.isEmpty()) { emit Started(false, "llama-server non trovato accanto al programma"); return; }
  QTcpServer probe;  // porta libera
  probe.listen(QHostAddress::LocalHost, 0);
  m_port = probe.serverPort();
  probe.close();
  m_model = modelPath;
  m_log.clear();
  m_polls = 0;
  m_ready = false;
  QStringList args{"-m", modelPath, "--host", "127.0.0.1", "--port", QString::number(m_port), "-c", "8192",
                   "--jinja", "--reasoning", "off", "--no-webui", "-t", QString::number(std::max(1, threads))};
  m_proc.setWorkingDirectory(QFileInfo(exe).absolutePath());
  m_proc.start(exe, args);
  emit Status("Avvio del modello…");
  QTimer::singleShot(500, this, &LlamaServer::PollHealth);
}

void LlamaServer::PollHealth() {
  if (m_polls < 0 || m_proc.state() == QProcess::NotRunning) return;
  if (++m_polls > 600) {  // 5 minuti: modelli grandi su dischi lenti
    Stop();
    emit Started(false, "il modello non si e' avviato in tempo");
    return;
  }
  QNetworkReply* r = m_net.get(Req(QString("http://127.0.0.1:%1/health").arg(m_port)));
  connect(r, &QNetworkReply::finished, this, [this, r] {
    r->deleteLater();
    if (r->error() == QNetworkReply::NoError && r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200) {
      m_ready = true;
      m_polls = -1;
      emit Status("Modello pronto");
      emit Started(true, {});
    } else {
      QTimer::singleShot(500, this, &LlamaServer::PollHealth);
    }
  });
}

void LlamaServer::Stop() {
  m_polls = -1;
  m_ready = false;
  if (m_proc.state() != QProcess::NotRunning) {
    m_proc.terminate();
    if (!m_proc.waitForFinished(3000)) { m_proc.kill(); m_proc.waitForFinished(2000); }
  }
}

// --- Generator --------------------------------------------------------------------------------
Generator::Generator(cad::Engine& engine, QObject* parent) : QObject(parent), m_engine(engine) {}

void Generator::Run(int port, const QString& request, int maxRepairs) {
  Cancel();
  m_port = port;
  m_repairs = 0;
  m_maxRepairs = maxRepairs;
  m_messages = QJsonArray{QJsonObject{{"role", "system"}, {"content", QString::fromStdString(ai::SystemPrompt())}},
                          QJsonObject{{"role", "user"}, {"content", request}}};
  Send();
}

void Generator::Cancel() {
  if (m_reply) { QNetworkReply* r = m_reply; m_reply = nullptr; r->abort(); r->deleteLater(); }
}

void Generator::Send() {
  emit Status(m_repairs == 0 ? "Il modello sta scrivendo i comandi…" : QString("Correzione dei comandi (%1)…").arg(m_repairs));
  QNetworkRequest rq = Req(QString("http://127.0.0.1:%1/v1/chat/completions").arg(m_port));
  rq.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
  rq.setTransferTimeout(10 * 60 * 1000);
  const QJsonObject body{{"messages", m_messages}, {"temperature", 0.2}, {"max_tokens", 2048},
                         {"chat_template_kwargs", QJsonObject{{"enable_thinking", false}}}};
  m_reply = m_net.post(rq, QJsonDocument(body).toJson(QJsonDocument::Compact));
  QNetworkReply* r = m_reply;
  connect(r, &QNetworkReply::finished, this, [this, r] {
    if (m_reply != r) return;  // annullata
    m_reply = nullptr;
    r->deleteLater();
    if (r->error() != QNetworkReply::NoError) {
      emit Finished(false, {}, "errore del modello: " + r->errorString());
      return;
    }
    const QJsonObject o = QJsonDocument::fromJson(r->readAll()).object();
    const QString content = o.value("choices").toArray().at(0).toObject().value("message").toObject().value("content").toString();
    const auto lines = ai::ExtractScript(content.toStdString());
    QStringList script;
    for (const auto& l : lines) script << QString::fromStdString(l);
    const ai::Validation v = ai::Validate(m_engine, lines);
    if (v.ok) { emit Finished(true, script, {}); return; }
    if (m_repairs >= m_maxRepairs) {
      emit Finished(false, script, QString("riga %1: %2").arg(v.line + 1).arg(QString::fromStdString(v.error)));
      return;
    }
    m_repairs++;
    m_messages.append(QJsonObject{{"role", "assistant"}, {"content", content}});
    m_messages.append(QJsonObject{{"role", "user"}, {"content", QString::fromStdString(ai::RepairRequest(lines, v))}});
    Send();
  });
}

}  // namespace cad::aiqt
