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
#include <QRegularExpression>
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
  if (o.contains("path")) {  // file GGUF scelto dal disco (modello addestrato, non scaricato)
    const QFileInfo fi(o.value("path").toString());
    return fi.exists() && fi.size() == (qint64)o.value("size").toDouble() ? fi.absoluteFilePath() : QString();
  }
  QJsonArray parts = o.value("parts").toArray();
  if (parts.isEmpty()) parts.append(QJsonObject{{"file", o.value("file")}, {"size", o.value("size")}});
  for (const auto& v : parts) {
    const QJsonObject p = v.toObject();
    const QFileInfo fi(dir + "/" + p.value("file").toString());
    if (p.value("file").toString().isEmpty() || !fi.exists() || fi.size() != (qint64)p.value("size").toDouble()) return {};
  }
  // per un GGUF diviso llama.cpp apre la prima parte e trova le altre accanto
  return dir + "/" + parts.at(0).toObject().value("file").toString();
}

bool UseLocalModelFile(const ai::ModelInfo& m, const QString& ggufPath) {
  const QFileInfo fi(ggufPath);
  if (!fi.exists() || fi.size() < 1024) return false;
  QFile head(ggufPath);  // un GGUF comincia con la parola "GGUF"
  if (!head.open(QIODevice::ReadOnly) || head.read(4) != "GGUF") return false;
  const QString dir = ModelsDir() + "/" + QString::fromStdString(m.id);
  QDir().mkpath(dir);
  QFile meta(dir + "/model.json");
  if (!meta.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
  meta.write(QJsonDocument(QJsonObject{{"path", fi.absoluteFilePath()}, {"size", (double)fi.size()}}).toJson());
  return true;
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
  m_parts.clear();
  m_part = 0;
  m_doneBefore = m_total = 0;
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
    auto part = [](const QJsonObject& f) {
      const QJsonObject lfs = f.value("lfs").toObject();
      return Part{f.value("path").toString(), lfs.value("oid").toString().toLower(),
                  (qint64)(lfs.contains("size") ? lfs.value("size").toDouble() : f.value("size").toDouble())};
    };
    for (const auto& q : m_model.quants) {
      const QString quant = QString::fromStdString(q);
      // un file solo ...-Q4_K_M.gguf, oppure le parti ...-q4_k_m-00001-of-00002.gguf
      const QRegularExpression single("-" + QRegularExpression::escape(quant) + "\\.gguf$",
                                      QRegularExpression::CaseInsensitiveOption);
      const QRegularExpression split("-" + QRegularExpression::escape(quant) + "-(\\d{5})-of-(\\d{5})\\.gguf$",
                                     QRegularExpression::CaseInsensitiveOption);
      std::vector<Part> parts;
      int count = 0;
      for (const auto& v : files) {
        const QJsonObject f = v.toObject();
        const QString path = f.value("path").toString();
        if (path.contains('/')) continue;
        if (single.match(path).hasMatch()) { parts = {part(f)}; count = 1; break; }
        const auto sm = split.match(path);
        if (sm.hasMatch()) {
          const int k = sm.captured(1).toInt(), n = sm.captured(2).toInt();
          if (k < 1 || n < 1 || n > 20 || (count && n != count)) continue;
          count = n;
          if ((int)parts.size() < n) parts.resize(n);
          parts[k - 1] = part(f);
        }
      }
      bool complete = count > 0 && (int)parts.size() == count;
      for (const auto& p : parts) complete = complete && !p.file.isEmpty() && p.sha256.size() == 64 && p.size > 0;
      if (complete) { m_parts = parts; break; }
    }
    if (m_parts.empty()) return Fail("file GGUF del modello non trovato nel repository");
    for (const auto& p : m_parts) m_total += p.size;
    m_dir = ModelsDir() + "/" + QString::fromStdString(m_model.id);
    QDir().mkpath(m_dir);
    m_part = 0;
    DownloadPart();
  });
}

void ModelDownloader::DownloadPart() {
  const Part& p = m_parts[m_part];
  m_out = std::make_unique<QFile>(m_dir + "/" + p.file + ".part");
  if (!m_out->open(QIODevice::WriteOnly | QIODevice::Truncate)) return Fail("impossibile scrivere in " + m_dir);
  m_hash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
  emit Status(m_parts.size() == 1 ? "Download di " + p.file + " (" + HumanSize(p.size) + ")…"
                                  : QString("Download di %1, parte %2 di %3 (%4 in tutto)…")
                                        .arg(p.file).arg(m_part + 1).arg(m_parts.size()).arg(HumanSize(m_total)));
  QNetworkRequest rq = Req(QString("%1/%2/resolve/main/%3").arg(Hub(), QString::fromStdString(m_model.repo), p.file));
  m_reply = m_net.get(rq);
  connect(m_reply, &QNetworkReply::readyRead, this, [this] {
    const QByteArray b = m_reply->readAll();
    m_hash->addData(b);
    if (m_out->write(b) != b.size()) m_reply->abort();
  });
  connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 d, qint64) { emit Progress(m_doneBefore + d, m_total); });
  connect(m_reply, &QNetworkReply::finished, this, [this] {
    QNetworkReply* r = m_reply;
    r->deleteLater();
    const QByteArray rest = r->readAll();
    if (!rest.isEmpty()) { m_hash->addData(rest); m_out->write(rest); }
    if (r->error() != QNetworkReply::NoError)
      return Fail(r->error() == QNetworkReply::OperationCanceledError ? "download annullato" : "download interrotto: " + r->errorString());
    m_out->close();
    emit Status("Verifica dell'integrità…");
    const Part& p = m_parts[m_part];
    const QString got = QString::fromLatin1(m_hash->result().toHex());
    if (m_out->size() != p.size || got != p.sha256) return Fail("il file scaricato non corrisponde (SHA-256 diverso): riprova");
    const QString final = m_dir + "/" + p.file;
    QFile::remove(final);
    if (!m_out->rename(final)) return Fail("impossibile salvare il modello");
    m_out.reset();
    m_doneBefore += p.size;
    if (++m_part < m_parts.size()) { DownloadPart(); return; }
    QJsonArray parts;
    for (const auto& q : m_parts) parts.append(QJsonObject{{"file", q.file}, {"sha256", q.sha256}, {"size", double(q.size)}});
    QJsonObject meta{{"repo", QString::fromStdString(m_model.repo)}, {"file", m_parts[0].file}, {"sha256", m_parts[0].sha256},
                     {"size", double(m_parts[0].size)}, {"parts", parts}, {"license", m_license}};
    QFile mf(m_dir + "/model.json");
    if (mf.open(QIODevice::WriteOnly | QIODevice::Truncate)) mf.write(QJsonDocument(meta).toJson());
    m_busy = false;
    emit Finished(true, m_dir + "/" + m_parts[0].file);
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
  for (const QString& c : {dir + "/llama/" + name, dir + "/" + name, dir + "/../Helpers/llama/" + name,
                         dir + "/../Resources/llama/" + name})
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

void Generator::Run(int port, const QString& request, int maxRepairs, bool useGrammar, const QString& systemPrompt) {
  Cancel();
  m_port = port;
  m_repairs = 0;
  m_maxRepairs = maxRepairs;
  m_grammar = useGrammar;
  const QString sys = systemPrompt.isEmpty() ? QString::fromStdString(ai::SystemPrompt()) : systemPrompt;
  m_messages = QJsonArray{QJsonObject{{"role", "system"}, {"content", sys}},
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
  // nei nuovi tentativi un po' piu' di varieta', per non ripetere lo stesso errore
  QJsonObject body{{"messages", m_messages}, {"temperature", m_repairs == 0 ? 0.2 : 0.4}, {"max_tokens", 2048},
                   {"chat_template_kwargs", QJsonObject{{"enable_thinking", false}}}};
  if (m_grammar) body.insert("grammar", QString::fromStdString(ai::Grammar()));
  m_reply = m_net.post(rq, QJsonDocument(body).toJson(QJsonDocument::Compact));
  QNetworkReply* r = m_reply;
  connect(r, &QNetworkReply::finished, this, [this, r] {
    if (m_reply != r) return;  // annullata
    m_reply = nullptr;
    r->deleteLater();
    if (r->error() != QNetworkReply::NoError) {
      // un llama-server che non accetta la grammatica: si riprova senza vincolo
      if (m_grammar && r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 400) {
        m_grammar = false;
        Send();
        return;
      }
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
