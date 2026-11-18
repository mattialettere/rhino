#include "ui/ScriptEditor.h"
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QShortcut>
#include <QStandardPaths>
#include <QSyntaxHighlighter>
#include <QTextStream>
#include <QToolButton>
#include <QVBoxLayout>

namespace cad::ui {

namespace {

class PyHighlighter : public QSyntaxHighlighter {
 public:
  explicit PyHighlighter(QTextDocument* d) : QSyntaxHighlighter(d) {
    const bool dark = d->parent() && qobject_cast<QWidget*>(d->parent()) &&
                      qobject_cast<QWidget*>(d->parent())->palette().color(QPalette::Base).lightness() < 128;
    m_kw.setForeground(dark ? QColor(120, 170, 255) : QColor(0, 0, 200));
    m_kw.setFontWeight(QFont::Bold);
    m_str.setForeground(dark ? QColor(230, 160, 110) : QColor(160, 60, 0));
    m_com.setForeground(dark ? QColor(110, 180, 110) : QColor(0, 128, 0));
    m_num.setForeground(dark ? QColor(180, 200, 120) : QColor(9, 134, 88));
  }

 protected:
  void highlightBlock(const QString& text) override {
    static const QRegularExpression kw(
        "\\b(and|as|assert|break|class|continue|def|del|elif|else|except|False|finally|for|from|global|if|import|in|is|"
        "lambda|None|nonlocal|not|or|pass|raise|return|True|try|while|with|yield)\\b");
    static const QRegularExpression num("\\b[0-9]+(\\.[0-9]*)?([eE][-+]?[0-9]+)?\\b");
    static const QRegularExpression str("\"[^\"\\\\]*(\\\\.[^\"\\\\]*)*\"|'[^'\\\\]*(\\\\.[^'\\\\]*)*'");
    for (auto it = kw.globalMatch(text); it.hasNext();) { auto m = it.next(); setFormat(m.capturedStart(), m.capturedLength(), m_kw); }
    for (auto it = num.globalMatch(text); it.hasNext();) { auto m = it.next(); setFormat(m.capturedStart(), m.capturedLength(), m_num); }
    int commentAt = -1;
    for (auto it = str.globalMatch(text); it.hasNext();) { auto m = it.next(); setFormat(m.capturedStart(), m.capturedLength(), m_str); }
    // commento: il primo # fuori dalle stringhe
    char q = 0;
    for (int i = 0; i < text.size(); i++) {
      const QChar c = text[i];
      if (q) { if (c == '\\') i++; else if (c == q) q = 0; }
      else if (c == '"' || c == '\'') q = c.toLatin1();
      else if (c == '#') { commentAt = i; break; }
    }
    if (commentAt >= 0) setFormat(commentAt, text.size() - commentAt, m_com);
  }

 private:
  QTextCharFormat m_kw, m_str, m_com, m_num;
};

QToolButton* Button(const QString& text, const QString& tip, QWidget* parent) {
  auto* b = new QToolButton(parent);
  b->setText(text);
  b->setToolTip(tip);
  b->setToolButtonStyle(Qt::ToolButtonTextOnly);
  return b;
}

const char* kTemplate =
    "# Script Python per Kurva3D (rhinoscriptsyntax)\n"
    "import rhinoscriptsyntax as rs\n"
    "\n"
    "for i in range(5):\n"
    "    rs.AddCircle(rs.WorldXYPlane(), 2 + i)\n"
    "print('creati', len(rs.AllObjects()), 'oggetti')\n";

}  // namespace

ScriptEditor::ScriptEditor(std::function<void(const QString&)> run, QWidget* parent) : QWidget(parent), m_run(std::move(run)) {
  auto* lay = new QVBoxLayout(this);
  lay->setContentsMargins(4, 4, 4, 4);
  auto* bar = new QHBoxLayout();
  auto* bNew = Button("Nuovo", "Nuovo script", this);
  auto* bOpen = Button("Apri…", "Apri uno script .py", this);
  auto* bSave = Button("Salva", "Salva lo script (Ctrl+S)", this);
  auto* bSaveAs = Button("Salva come…", "Salva lo script con un altro nome", this);
  auto* bRun = Button("▶ Esegui", "Esegui lo script (F5)", this);
  for (auto* b : {bNew, bOpen, bSave, bSaveAs}) bar->addWidget(b);
  bar->addStretch();
  bar->addWidget(bRun);
  lay->addLayout(bar);
  m_file = new QLabel(this);
  lay->addWidget(m_file);
  m_text = new QPlainTextEdit(this);
  QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
  m_text->setFont(mono);
  m_text->setLineWrapMode(QPlainTextEdit::NoWrap);
  m_text->setTabStopDistance(QFontMetricsF(mono).horizontalAdvance(' ') * 4);
  m_text->setPlainText(kTemplate);
  new PyHighlighter(m_text->document());
  lay->addWidget(m_text, 1);
  connect(bNew, &QToolButton::clicked, this, [this] { m_path.clear(); m_text->setPlainText(kTemplate); UpdateTitle(); });
  connect(bOpen, &QToolButton::clicked, this, [this] {
    const QString f = QFileDialog::getOpenFileName(this, "Apri script Python", QFileInfo(m_path).absolutePath(), "Script Python (*.py)");
    if (!f.isEmpty()) OpenFile(f);
  });
  connect(bSave, &QToolButton::clicked, this, [this] { Save(m_path.isEmpty()); });
  connect(bSaveAs, &QToolButton::clicked, this, [this] { Save(true); });
  connect(bRun, &QToolButton::clicked, this, [this] { Run(); });
  auto* f5 = new QShortcut(QKeySequence(Qt::Key_F5), this);
  f5->setContext(Qt::WidgetWithChildrenShortcut);
  connect(f5, &QShortcut::activated, this, [this] { Run(); });
  auto* save = new QShortcut(QKeySequence::Save, this);
  save->setContext(Qt::WidgetWithChildrenShortcut);
  connect(save, &QShortcut::activated, this, [this] { Save(m_path.isEmpty()); });
  UpdateTitle();
}

bool ScriptEditor::OpenFile(const QString& file) {
  QFile f(file);
  if (!f.open(QIODevice::ReadOnly)) {
    QMessageBox::warning(this, "Kurva3D", "Impossibile aprire " + file);
    return false;
  }
  m_text->setPlainText(QString::fromUtf8(f.readAll()));
  m_path = file;
  UpdateTitle();
  return true;
}

bool ScriptEditor::Save(bool askName) {
  QString path = m_path;
  if (askName || path.isEmpty()) {
    path = QFileDialog::getSaveFileName(this, "Salva script Python", path.isEmpty() ? QDir::homePath() + "/script.py" : path, "Script Python (*.py)");
    if (path.isEmpty()) return false;
    if (!path.endsWith(".py", Qt::CaseInsensitive)) path += ".py";
  }
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    QMessageBox::warning(this, "Kurva3D", "Impossibile salvare " + path);
    return false;
  }
  f.write(m_text->toPlainText().toUtf8());
  m_path = path;
  UpdateTitle();
  return true;
}

void ScriptEditor::Run() {
  // con un file aperto si salva e si esegue quello; altrimenti un file temporaneo
  QString file = m_path;
  if (!file.isEmpty()) {
    if (!Save(false)) return;
  } else {
    file = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/kurva3d_script.py";
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write(m_text->toPlainText().toUtf8());
  }
  if (m_run) m_run(file);
}

void ScriptEditor::UpdateTitle() { m_file->setText(m_path.isEmpty() ? "(script senza nome)" : QDir::toNativeSeparators(m_path)); }

}  // namespace cad::ui
