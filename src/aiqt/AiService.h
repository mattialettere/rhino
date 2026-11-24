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
#include <vector>

namespace cad::aiqt {

// Cartella dei modelli scaricati (dati dell'applicazione dell'utente).
QString ModelsDir();
// Percorso del file del modello se gia' scaricato e verificato, altrimenti vuoto.
QString InstalledModelPath(const ai::ModelInfo& m);
// Usa come modello m un file GGUF gia' sul disco (non lo copia): false se non e' un GGUF.
bool UseLocalModelFile(const ai::ModelInfo& m, const QString& ggufPath);

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

  QNetworkAccessManager m_net;
  QPointer<QNetworkReply> m_reply;
  void DownloadPart();
  struct Part {
    QString file, sha256;
    qint64 size = 0;
  };
  ai::ModelInfo m_model;
  QString m_license, m_dir;
  std::vector<Part> m_parts;  // un file o le parti -0000k-of-0000n di un GGUF diviso
  size_t m_part = 0;
  qint64 m_doneBefore = 0, m_total = 0;
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
  // maxRepairs: nuovi tentativi se Kurva3D rifiuta lo script; con useGrammar il modello
  // puo' scrivere solo comandi conosciuti nella forma giusta (grammatica GBNF di llama.cpp)
  // systemPrompt vuoto: istruzioni complete con gli esempi (ai::SystemPrompt)
  void Run(int port, const QString& request, int maxRepairs = 3, bool useGrammar = true,
           const QString& systemPrompt = {});
  // Oggetti complessi: prima il piano in parti, poi i comandi di una parte alla volta, ognuna
  // verificata subito (anche nella posizione) e fatta correggere fino a maxRepairs volte.
  // Una parte che non riesce viene lasciata fuori (Skipped). exclude: esempio da non usare (prove).
  void RunParts(int port, const QString& request, int maxRepairs = 2, bool useGrammar = true,
                const QString& exclude = {});
  // Scelta automatica: i modelli addestrati su Kurva3D scrivono lo script in una volta
  // (istruzioni brevi), gli altri lavorano per parti.
  void Start(int port, const QString& request, const ai::ModelInfo* model);
  void Cancel();
  bool Busy() const { return m_reply != nullptr; }
  int Attempts() const { return m_requests; }  // richieste fatte al modello nell'ultima Run/RunParts
  bool UsedGrammar() const { return m_grammar; }
  const ai::Plan& LastPlan() const { return m_plan; }
  QStringList Skipped() const { return m_skipped; }  // parti lasciate fuori

 signals:
  void Status(const QString& text);
  // ok: script verificato; altrimenti l'ultimo script e l'errore
  void Finished(bool ok, const QStringList& script, const QString& error);

 private:
  enum class Phase { Single, Plan, Part };
  void Send();
  void OnReply(const QString& content);
  void StartPart();
  void FinishParts();
  cad::Engine& m_engine;
  QNetworkAccessManager m_net;
  QPointer<QNetworkReply> m_reply;
  QJsonArray m_messages;
  int m_port = 0, m_repairs = 0, m_maxRepairs = 3, m_requests = 0;
  bool m_grammar = true;
  Phase m_phase = Phase::Single;
  QString m_request, m_exclude;
  ai::Plan m_plan;
  int m_part = 0;
  int m_partStart = 0;  // indice in m_messages della richiesta della parte corrente
  std::vector<std::string> m_done;  // comandi delle parti gia' verificate
  QStringList m_skipped;
};

}  // namespace cad::aiqt
