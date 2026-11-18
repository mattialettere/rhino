// Servizi Qt dell'assistente AI: modelli scaricati da Hugging Face (licenza e
// integrita' verificate), server llama.cpp locale e richieste al modello.
#pragma once
#include "ai/Assistant.h"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <memory>

namespace cad::aiqt {

// Cartella dei modelli scaricati (dati dell'applicazione dell'utente).
QString ModelsDir();
// Percorso del file del modello se gia' scaricato e verificato, altrimenti vuoto.
QString InstalledModelPath(const ai::ModelInfo& m);

// Download di un modello: controllo della licenza sul repository, scelta del file
// GGUF, scaricamento con avanzamento, verifica SHA-256 e dimensione.
class ModelDownloader : public QObject {
  Q_OBJECT
 public:
  explicit ModelDownloader(QObject* parent = nullptr);
  void Start(const ai::ModelInfo& m);
  void Cancel();
  bool Busy() const { return m_busy; }

 signals:
  void Status(const QString& text);
  void Progress(qint64 done, qint64 total);
  void Finished(bool ok, const QString& pathOrError);

 private:
  void Fail(const QString& error);
  void GetInfo();
  void GetTree();
  void Download();

  QNetworkAccessManager m_net;
  QPointer<QNetworkReply> m_reply;
  ai::ModelInfo m_model;
  QString m_file, m_sha256, m_license;
  qint64 m_size = 0;
  std::unique_ptr<QFile> m_out;
  std::unique_ptr<QCryptographicHash> m_hash;
  bool m_busy = false;
};

// Server llama.cpp (llama-server) avviato in locale su una porta libera.
class LlamaServer : public QObject {
  Q_OBJECT
 public:
  explicit LlamaServer(QObject* parent = nullptr);
  ~LlamaServer() override;
  // Percorso di llama-server (accanto al programma, variabile KURVA3D_LLAMA_SERVER o PATH).
  static QString Executable();
  void Start(const QString& modelPath, int threads);
  void Stop();
  bool Ready() const { return m_ready; }
  QString ModelPath() const { return m_model; }
  int Port() const { return m_port; }
  QString Log() const { return m_log; }

 signals:
  void Status(const QString& text);
  void Started(bool ok, const QString& error);

 private:
  void PollHealth();
  QProcess m_proc;
  QNetworkAccessManager m_net;
  QString m_model, m_log;
  int m_port = 0;
  int m_polls = 0;
  bool m_ready = false;
};

// Conversazione con il modello: genera lo script e lo fa correggere se Kurva3D lo rifiuta.
class Generator : public QObject {
  Q_OBJECT
 public:
  Generator(cad::Engine& engine, QObject* parent = nullptr);
  void Run(int port, const QString& request, int maxRepairs = 2);
  void Cancel();
  bool Busy() const { return m_reply != nullptr; }

 signals:
  void Status(const QString& text);
  // ok: script verificato; altrimenti l'ultimo script e l'errore
  void Finished(bool ok, const QStringList& script, const QString& error);

 private:
  void Send();
  cad::Engine& m_engine;
  QNetworkAccessManager m_net;
  QPointer<QNetworkReply> m_reply;
  QJsonArray m_messages;
  int m_port = 0, m_repairs = 0, m_maxRepairs = 2;
};

}  // namespace cad::aiqt
