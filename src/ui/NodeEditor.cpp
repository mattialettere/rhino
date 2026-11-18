#include "ui/NodeEditor.h"
#include "commands/Engine.h"
#include "core/Document3dm.h"
#include <QApplication>
#include <QCloseEvent>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace cad::ui {

using namespace cad::nodes;

namespace {

constexpr double kHeader = 22, kRow = 18, kPortR = 5;

QColor CategoryColor(const std::string& cat) {
  if (cat == "Parametri") return QColor(88, 128, 196);
  if (cat == "Matematica") return QColor(112, 156, 84);
  if (cat == "Vettori") return QColor(196, 142, 58);
  if (cat == "Curve") return QColor(52, 148, 158);
  if (cat == "Superfici") return QColor(146, 98, 168);
  if (cat == "Trasforma") return QColor(186, 86, 86);
  return QColor(128, 128, 128);
}

const ComponentDef* Def(const Node& n) { return FindDef(n.type); }

double Param(const Node& n, const char* k, double def) {
  auto it = n.params.find(k);
  if (it == n.params.end()) return def;
  char* end = nullptr;
  const double v = std::strtod(it->second.c_str(), &end);
  return end == it->second.c_str() ? def : v;
}

std::string Fmt(double v, int dec) {
  char b[64];
  std::snprintf(b, sizeof b, "%.*f", std::max(0, dec), v);
  return b;
}

QStringList PanelLines(const Node& n) {
  QStringList lines;
  if (!n.inputs.empty() && !n.inputs[0].empty()) {
    int i = 0;
    for (const auto& v : n.inputs[0]) {
      lines << QString("%1  %2").arg(i++).arg(QString::fromStdString(v.ToString()));
      if (lines.size() > 200) break;
    }
  } else {
    auto it = n.params.find("text");
    if (it != n.params.end()) lines = QString::fromStdString(it->second).split('\n');
  }
  return lines;
}

}  // namespace

// ================================================================ area di lavoro

NodeCanvas::NodeCanvas(Graph& g, QWidget* parent) : QWidget(parent), m_graph(g) {
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);
  setMinimumSize(400, 300);
}

QRectF NodeCanvas::NodeRect(const Node& n) const {
  const ComponentDef* d = Def(n);
  const int rows = d ? (int)std::max(d->in.size(), d->out.size()) : 1;
  double w = 132, h = kHeader + std::max(1, rows) * kRow + 6;
  if (n.type == "Slider") { w = 240; h = kHeader + 28; }
  else if (n.type == "Toggle") { w = 120; h = kHeader + 28; }
  else if (n.type == "Panel") {
    w = 210;
    h = kHeader + std::clamp((double)PanelLines(n).size() * 14 + 10, 60.0, 260.0);
  } else if (n.type == "Expression") { w = 190; h = std::max(h, kHeader + 3 * kRow + 22); }
  else if (n.type == "GeometryRef") { w = 180; h = kHeader + 28; }
  return QRectF(n.x, n.y, w, h);
}

QPointF NodeCanvas::PortPos(const Node& n, int port, bool output) const {
  const QRectF r = NodeRect(n);
  const ComponentDef* d = Def(n);
  const int count = d ? (int)(output ? d->out.size() : d->in.size()) : 0;
  double y;
  if (n.type == "Slider" || n.type == "Toggle" || n.type == "GeometryRef" || (n.type == "Panel" && count == 1))
    y = r.top() + kHeader + (r.height() - kHeader) / 2;
  else
    y = r.top() + kHeader + kRow * (port + 0.5) + 3;
  (void)count;
  return QPointF(output ? r.right() : r.left(), y);
}

QRectF NodeCanvas::SliderTrack(const Node& n) const {
  const QRectF r = NodeRect(n);
  return QRectF(r.left() + 12, r.top() + kHeader + 14, r.width() - 80, 4);
}

NodeCanvas::Hit NodeCanvas::HitTest(QPointF p) const {
  Hit h;
  const double tol = std::max(7.0, 7.0 / m_zoom);
  for (auto it = m_graph.nodes.rbegin(); it != m_graph.nodes.rend(); ++it) {
    const Node& n = *it;
    const ComponentDef* d = Def(n);
    if (!d) continue;
    for (int o = 0; o < (int)d->out.size(); o++)
      if (QLineF(PortPos(n, o, true), p).length() <= tol) { h.node = n.id; h.port = o; h.output = true; return h; }
    for (int i = 0; i < (int)d->in.size(); i++)
      if (QLineF(PortPos(n, i, false), p).length() <= tol) { h.node = n.id; h.port = i; h.output = false; return h; }
    const QRectF r = NodeRect(n);
    if (!r.contains(p)) continue;
    h.node = n.id;
    if (n.type == "Slider" && SliderTrack(n).adjusted(-6, -10, 6, 10).contains(p)) h.slider = true;
    if (n.type == "Toggle" && p.y() > r.top() + kHeader) h.toggle = true;
    return h;
  }
  return h;
}

void NodeCanvas::AddNodeAt(const std::string& type, QPointF screen) {
  const QPointF g = ToGraph(screen);
  m_selected = m_graph.Add(type, std::round(g.x()), std::round(g.y()));
  emit Changed();
  update();
}

void NodeCanvas::AddNodeCentered(const std::string& type) {
  // un po' spostato a ogni aggiunta, per non sovrapporre i nodi
  static int k = 0;
  k = (k + 1) % 8;
  AddNodeAt(type, QPointF(width() / 2.0 - 60 + k * 18, height() / 2.0 - 30 + k * 18));
}

void NodeCanvas::ZoomAll() {
  if (m_graph.nodes.empty()) { m_offset = QPointF(40, 40); m_zoom = 1; update(); return; }
  QRectF b;
  for (const auto& n : m_graph.nodes) b = b.isNull() ? NodeRect(n) : b.united(NodeRect(n));
  b.adjust(-40, -40, 40, 40);
  m_zoom = std::clamp(std::min(width() / b.width(), height() / b.height()), 0.2, 2.0);
  m_offset = QPointF(width() / 2.0, height() / 2.0) - b.center() * m_zoom;
  update();
}

void NodeCanvas::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const bool dark = palette().color(QPalette::Window).lightness() < 128;
  const QColor bg = dark ? QColor(48, 50, 54) : QColor(214, 218, 222);
  p.fillRect(rect(), bg);
  // griglia
  const double step = 24 * m_zoom;
  if (step > 6) {
    p.setPen(QPen(dark ? QColor(58, 60, 65) : QColor(202, 206, 211), 1));
    for (double x = std::fmod(m_offset.x(), step); x < width(); x += step) p.drawLine(QPointF(x, 0), QPointF(x, height()));
    for (double y = std::fmod(m_offset.y(), step); y < height(); y += step) p.drawLine(QPointF(0, y), QPointF(width(), y));
  }
  p.translate(m_offset);
  p.scale(m_zoom, m_zoom);
  // fili
  auto wirePath = [](QPointF a, QPointF b) {
    QPainterPath path(a);
    const double dx = std::max(40.0, std::fabs(b.x() - a.x()) * 0.5);
    path.cubicTo(a + QPointF(dx, 0), b - QPointF(dx, 0), b);
    return path;
  };
  for (const auto& w : m_graph.wires) {
    const Node* a = m_graph.Get(w.from);
    const Node* b = m_graph.Get(w.to);
    if (!a || !b) continue;
    const bool empty = w.fromPort >= (int)a->outputs.size() || a->outputs[w.fromPort].empty();
    p.setPen(QPen(empty ? QColor(200, 120, 60) : (dark ? QColor(170, 175, 180) : QColor(80, 84, 90)), 2));
    p.setBrush(Qt::NoBrush);
    p.drawPath(wirePath(PortPos(*a, w.fromPort, true), PortPos(*b, w.toPort, false)));
  }
  if (m_drag == Drag::Wire) {
    if (const Node* a = m_graph.Get(m_dragNode)) {
      p.setPen(QPen(QColor(40, 120, 220), 2, Qt::DashLine));
      p.drawPath(wirePath(PortPos(*a, m_dragPort, true), ToGraph(m_wireEnd)));
    }
  }
  // nodi
  QFont f = font();
  f.setPointSizeF(8.5);
  QFont bold = f;
  bold.setBold(true);
  for (const auto& n : m_graph.nodes) {
    const ComponentDef* d = Def(n);
    const QRectF r = NodeRect(n);
    QColor body = dark ? QColor(78, 80, 86) : QColor(244, 244, 246);
    QColor border = dark ? QColor(30, 30, 32) : QColor(120, 124, 130);
    double bw = 1.2;
    if (!n.warning.empty()) { border = QColor(232, 150, 30); bw = 2; }
    if (!n.error.empty()) { border = QColor(214, 50, 40); bw = 2.4; }
    if (n.id == m_selected) { border = QColor(40, 120, 220); bw = 2.4; }
    p.setPen(QPen(border, bw));
    p.setBrush(body);
    p.drawRoundedRect(r, 6, 6);
    // intestazione
    QColor head = d ? CategoryColor(d->category) : QColor(200, 60, 60);
    if (!n.preview) head = head.lighter(140);
    QPainterPath hp;
    hp.addRoundedRect(QRectF(r.left(), r.top(), r.width(), kHeader), 6, 6);
    hp.addRect(QRectF(r.left(), r.top() + kHeader - 6, r.width(), 6));
    p.fillPath(hp, head);
    p.setPen(Qt::white);
    p.setFont(bold);
    QString title = d ? QString::fromStdString(d->name) : QString::fromStdString(n.type);
    if (auto it = n.params.find("name"); it != n.params.end() && !it->second.empty()) title = QString::fromStdString(it->second);
    if (!n.preview) title += "  (anteprima spenta)";
    p.drawText(QRectF(r.left() + 6, r.top(), r.width() - 12, kHeader), Qt::AlignVCenter | Qt::AlignLeft,
               p.fontMetrics().elidedText(title, Qt::ElideRight, (int)r.width() - 12));
    p.setFont(f);
    const QColor text = dark ? QColor(225, 225, 228) : QColor(30, 30, 34);
    p.setPen(text);
    if (!d) continue;
    // contenuto dei nodi parametro
    if (n.type == "Slider") {
      const QRectF t = SliderTrack(n);
      const double mn = Param(n, "min", 0), mx = Param(n, "max", 10), v = Param(n, "value", mn);
      const int dec = (int)Param(n, "decimals", 2);
      p.setPen(Qt::NoPen);
      p.setBrush(dark ? QColor(40, 40, 44) : QColor(190, 194, 200));
      p.drawRoundedRect(t, 2, 2);
      const double f01 = mx > mn ? std::clamp((v - mn) / (mx - mn), 0.0, 1.0) : 0.0;
      const QPointF knob(t.left() + f01 * t.width(), t.center().y());
      p.setBrush(QColor(40, 120, 220));
      p.drawEllipse(knob, 6, 6);
      p.setPen(text);
      p.drawText(QRectF(t.right() + 8, r.top() + kHeader, r.right() - t.right() - 16, r.height() - kHeader),
                 Qt::AlignVCenter | Qt::AlignRight, QString::fromStdString(Fmt(v, dec)));
    } else if (n.type == "Toggle") {
      const bool on = n.params.count("value") && n.params.at("value") == "true";
      const QRectF b(r.left() + 10, r.top() + kHeader + 5, r.width() - 34, 18);
      p.setPen(QPen(border.darker(110), 1));
      p.setBrush(on ? QColor(70, 160, 90) : (dark ? QColor(60, 60, 64) : QColor(220, 220, 224)));
      p.drawRoundedRect(b, 4, 4);
      p.setPen(on ? Qt::white : text);
      p.drawText(b, Qt::AlignCenter, on ? "Vero" : "Falso");
    } else if (n.type == "Panel") {
      const QRectF b(r.left() + 8, r.top() + kHeader + 4, r.width() - 22, r.height() - kHeader - 8);
      p.setPen(Qt::NoPen);
      p.setBrush(dark ? QColor(250, 246, 220).darker(330) : QColor(252, 248, 222));
      p.drawRect(b);
      p.setPen(text);
      p.save();
      p.setClipRect(b);
      double y = b.top() + 2;
      for (const QString& line : PanelLines(n)) {
        p.drawText(QRectF(b.left() + 4, y, b.width() - 8, 14), Qt::AlignLeft | Qt::AlignVCenter,
                   p.fontMetrics().elidedText(line, Qt::ElideRight, (int)b.width() - 8));
        y += 14;
        if (y > b.bottom()) break;
      }
      p.restore();
    } else if (n.type == "GeometryRef") {
      const std::string ids = n.params.count("ids") ? n.params.at("ids") : "";
      const int count = ids.empty() ? 0 : (int)std::count(ids.begin(), ids.end(), ',') + 1;
      p.drawText(QRectF(r.left() + 10, r.top() + kHeader, r.width() - 30, r.height() - kHeader), Qt::AlignVCenter,
                 count ? QString("%1 oggetti del modello").arg(count) : QString("doppio clic: dalla selezione"));
    } else if (n.type == "Expression") {
      const QString e = QString::fromStdString(n.params.count("expr") ? n.params.at("expr") : "");
      p.drawText(QRectF(r.left() + 24, r.bottom() - 22, r.width() - 48, 18), Qt::AlignCenter,
                 p.fontMetrics().elidedText(e, Qt::ElideRight, (int)r.width() - 48));
    }
    // porte ed etichette
    for (int i = 0; i < (int)d->in.size(); i++) {
      const QPointF pp = PortPos(n, i, false);
      p.setPen(QPen(border, 1));
      bool wired = false;
      for (const auto& w : m_graph.wires) if (w.to == n.id && w.toPort == i) wired = true;
      p.setBrush(wired ? QColor(40, 120, 220) : (d->in[i].optional || d->in[i].hasDefault ? body : QColor(232, 150, 30)));
      p.drawEllipse(pp, kPortR, kPortR);
      if (n.type != "Panel") {
        p.setPen(text);
        p.drawText(QRectF(pp.x() + 8, pp.y() - kRow / 2, r.width() / 2 - 8, kRow), Qt::AlignVCenter | Qt::AlignLeft,
                   QString::fromStdString(d->in[i].nick));
      }
    }
    for (int o = 0; o < (int)d->out.size(); o++) {
      const QPointF pp = PortPos(n, o, true);
      p.setPen(QPen(border, 1));
      p.setBrush(QColor(40, 120, 220));
      p.drawEllipse(pp, kPortR, kPortR);
      if (n.type == "Slider" || n.type == "Toggle" || n.type == "Panel" || n.type == "GeometryRef") continue;
      p.setPen(text);
      const size_t count = o < (int)n.outputs.size() ? n.outputs[o].size() : 0;
      QString label = QString::fromStdString(d->out[o].nick);
      if (count != 1) label += QString(" (%1)").arg(count);
      p.drawText(QRectF(r.left() + r.width() / 2, pp.y() - kRow / 2, r.width() / 2 - 8, kRow), Qt::AlignVCenter | Qt::AlignRight, label);
    }
  }
}

void NodeCanvas::SetSlider(Node& n, double gx) {
  const QRectF t = SliderTrack(n);
  const double mn = Param(n, "min", 0), mx = Param(n, "max", 10);
  const int dec = (int)Param(n, "decimals", 2);
  const double f = std::clamp((gx - t.left()) / t.width(), 0.0, 1.0);
  const double scale = std::pow(10.0, dec);
  const double v = std::round((mn + f * (mx - mn)) * scale) / scale;
  n.params["value"] = Fmt(v, dec);
}

void NodeCanvas::mousePressEvent(QMouseEvent* e) {
  setFocus();
  m_press = m_last = e->position();
  const QPointF g = ToGraph(e->position());
  if (e->button() == Qt::MiddleButton) { m_drag = Drag::Pan; return; }
  if (e->button() != Qt::LeftButton) return;
  const Hit h = HitTest(g);
  if (h.node && h.port >= 0) {
    if (h.output) {
      m_drag = Drag::Wire;
      m_dragNode = h.node;
      m_dragPort = h.port;
      m_wireEnd = e->position();
    } else {
      // ingresso collegato: si stacca l'ultimo filo e lo si porta altrove
      for (size_t i = m_graph.wires.size(); i-- > 0;) {
        const Wire w = m_graph.wires[i];
        if (w.to == h.node && w.toPort == h.port) {
          m_graph.RemoveWire(i);
          m_drag = Drag::Wire;
          m_dragNode = w.from;
          m_dragPort = w.fromPort;
          m_wireEnd = e->position();
          emit Changed();
          break;
        }
      }
    }
    update();
    return;
  }
  m_selected = h.node;
  if (h.node && h.slider) {
    m_drag = Drag::Slider;
    m_dragNode = h.node;
    SetSlider(*m_graph.Get(h.node), g.x());
    emit Changed();
  } else if (h.node && h.toggle) {
    Node* n = m_graph.Get(h.node);
    n->params["value"] = n->params["value"] == "true" ? "false" : "true";
    emit Changed();
  } else if (h.node) {
    m_drag = Drag::Move;
    m_dragNode = h.node;
    // in primo piano
    auto it = std::find_if(m_graph.nodes.begin(), m_graph.nodes.end(), [&](const Node& n) { return n.id == h.node; });
    if (it != m_graph.nodes.end() && it + 1 != m_graph.nodes.end()) std::rotate(it, it + 1, m_graph.nodes.end());
  } else {
    m_drag = Drag::Pan;
  }
  if (const Node* n = m_graph.Get(m_selected)) {
    const ComponentDef* d = Def(*n);
    QString msg = d ? QString::fromStdString(d->name + ": " + d->description) : QString();
    if (!n->error.empty()) msg = "Errore: " + QString::fromStdString(n->error);
    else if (!n->warning.empty()) msg = "Attenzione: " + QString::fromStdString(n->warning);
    emit Message(msg);
  }
  update();
}

void NodeCanvas::mouseMoveEvent(QMouseEvent* e) {
  const QPointF d = e->position() - m_last;
  m_last = e->position();
  switch (m_drag) {
    case Drag::Pan: m_offset += d; break;
    case Drag::Move:
      if (Node* n = m_graph.Get(m_dragNode)) { n->x += d.x() / m_zoom; n->y += d.y() / m_zoom; }
      break;
    case Drag::Wire: m_wireEnd = e->position(); break;
    case Drag::Slider:
      if (Node* n = m_graph.Get(m_dragNode)) { SetSlider(*n, ToGraph(e->position()).x()); emit Changed(); }
      break;
    case Drag::None: {
      const Hit h = HitTest(ToGraph(e->position()));
      QString tip;
      if (const Node* n = m_graph.Get(h.node); n && h.port >= 0) {
        const ComponentDef* def = Def(*n);
        const Port& pt = h.output ? def->out[h.port] : def->in[h.port];
        tip = QString("%1 (%2)").arg(QString::fromStdString(pt.name), KindName(pt.kind));
        const List* data = nullptr;
        if (h.output && h.port < (int)n->outputs.size()) data = &n->outputs[h.port];
        if (!h.output && h.port < (int)n->inputs.size()) data = &n->inputs[h.port];
        if (data) {
          tip += QString("\n%1 valori").arg(data->size());
          for (size_t i = 0; i < data->size() && i < 8; i++) tip += "\n" + QString::fromStdString((*data)[i].ToString());
          if (data->size() > 8) tip += "\n...";
        }
      }
      setToolTip(tip);
      return;
    }
  }
  update();
}

void NodeCanvas::mouseReleaseEvent(QMouseEvent* e) {
  if (m_drag == Drag::Wire) {
    const Hit h = HitTest(ToGraph(e->position()));
    if (h.node && h.port >= 0 && !h.output) {
      // Maiuscolo: si aggiunge un filo; altrimenti il nuovo filo sostituisce i precedenti
      if (!(e->modifiers() & Qt::ShiftModifier)) m_graph.Disconnect(h.node, h.port);
      if (!m_graph.Connect(m_dragNode, m_dragPort, h.node, h.port)) emit Message("Collegamento non possibile (creerebbe un ciclo)");
      emit Changed();
    }
  } else if (m_drag == Drag::Move) {
    if (Node* n = m_graph.Get(m_dragNode)) { n->x = std::round(n->x); n->y = std::round(n->y); }
  }
  m_drag = Drag::None;
  update();
}

void NodeCanvas::mouseDoubleClickEvent(QMouseEvent* e) {
  const Hit h = HitTest(ToGraph(e->position()));
  if (h.node && h.port < 0) EditNode(h.node);
  else if (!h.node) ShowSearch(e->position());
}

void NodeCanvas::wheelEvent(QWheelEvent* e) {
  const double steps = e->angleDelta().y() / 120.0;
  const double nz = std::clamp(m_zoom * std::pow(1.15, steps), 0.2, 3.0);
  const QPointF at = e->position();
  const QPointF g = ToGraph(at);
  m_zoom = nz;
  m_offset = at - g * m_zoom;
  update();
}

void NodeCanvas::keyPressEvent(QKeyEvent* e) {
  if ((e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) && m_selected) {
    m_graph.Remove(m_selected);
    m_selected = 0;
    emit Changed();
    update();
  } else if (e->key() == Qt::Key_Space || e->key() == Qt::Key_Return) {
    ShowSearch(mapFromGlobal(QCursor::pos()));
  } else {
    QWidget::keyPressEvent(e);
  }
}

void NodeCanvas::contextMenuEvent(QContextMenuEvent* e) {
  const Hit h = HitTest(ToGraph(e->pos()));
  QMenu m(this);
  if (Node* n = m_graph.Get(h.node)) {
    const int id = n->id;
    m_selected = id;
    if (h.port >= 0 && !h.output) {
      m.addAction("Scollega l'ingresso", this, [this, id, port = h.port] { m_graph.Disconnect(id, port); emit Changed(); update(); });
      m.addSeparator();
    }
    QAction* pv = m.addAction("Anteprima");
    pv->setCheckable(true);
    pv->setChecked(n->preview);
    connect(pv, &QAction::toggled, this, [this, id](bool on) { if (Node* x = m_graph.Get(id)) x->preview = on; emit Changed(); update(); });
    m.addAction("Bake (aggiungi al modello)", this, [this, id] { if (bake) bake(id); });
    if (n->type == "GeometryRef") m.addAction("Imposta dalla selezione", this, [this, id] { if (setFromSelection) setFromSelection(id); });
    if (n->type == "Slider" || n->type == "Panel" || n->type == "Expression") m.addAction("Modifica…", this, [this, id] { EditNode(id); });
    m.addSeparator();
    m.addAction("Elimina", this, [this, id] { m_graph.Remove(id); m_selected = 0; emit Changed(); update(); });
  } else {
    const QPointF at = e->pos();
    m.addAction("Aggiungi componente…", this, [this, at] { ShowSearch(at); });
    m.addAction("Adatta alla finestra", this, [this] { ZoomAll(); });
  }
  m.exec(e->globalPos());
  update();
}

void NodeCanvas::EditNode(int id) {
  Node* n = m_graph.Get(id);
  if (!n) return;
  if (n->type == "Slider") {
    QDialog dlg(this);
    dlg.setWindowTitle("Cursore numerico");
    auto* form = new QFormLayout(&dlg);
    auto spin = [&](double v) {
      auto* s = new QDoubleSpinBox(&dlg);
      s->setRange(-1e9, 1e9);
      s->setDecimals(6);
      s->setValue(v);
      return s;
    };
    auto* name = new QLineEdit(QString::fromStdString(n->params.count("name") ? n->params.at("name") : ""), &dlg);
    form->addRow("Nome", name);
    auto* mn = spin(Param(*n, "min", 0));
    auto* mx = spin(Param(*n, "max", 10));
    auto* val = spin(Param(*n, "value", 5));
    auto* dec = new QSpinBox(&dlg);
    dec->setRange(0, 6);
    dec->setValue((int)Param(*n, "decimals", 2));
    form->addRow("Minimo", mn);
    form->addRow("Massimo", mx);
    form->addRow("Valore", val);
    form->addRow("Decimali (0 = intero)", dec);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;
    n = m_graph.Get(id);
    n->params["name"] = name->text().toStdString();
    n->params["min"] = Fmt(std::min(mn->value(), mx->value()), 6);
    n->params["max"] = Fmt(std::max(mn->value(), mx->value()), 6);
    n->params["decimals"] = std::to_string(dec->value());
    n->params["value"] = Fmt(std::clamp(val->value(), std::min(mn->value(), mx->value()), std::max(mn->value(), mx->value())), dec->value());
  } else if (n->type == "Panel") {
    bool ok = false;
    const QString t = QInputDialog::getMultiLineText(this, "Pannello", "Un valore per riga:", QString::fromStdString(n->params["text"]), &ok);
    if (!ok) return;
    m_graph.Get(id)->params["text"] = t.toStdString();
  } else if (n->type == "Expression") {
    bool ok = false;
    const QString t = QInputDialog::getText(this, "Espressione", "Formula (variabili x, y, z):", QLineEdit::Normal,
                                            QString::fromStdString(n->params["expr"]), &ok);
    if (!ok) return;
    m_graph.Get(id)->params["expr"] = t.toStdString();
  } else if (n->type == "Toggle") {
    n->params["value"] = n->params["value"] == "true" ? "false" : "true";
  } else if (n->type == "GeometryRef") {
    if (setFromSelection) setFromSelection(id);
    return;
  } else {
    return;
  }
  emit Changed();
  update();
}

void NodeCanvas::ShowSearch(QPointF screen) {
  auto* pop = new QWidget(this, Qt::Popup);
  pop->setAttribute(Qt::WA_DeleteOnClose);
  auto* lay = new QVBoxLayout(pop);
  lay->setContentsMargins(4, 4, 4, 4);
  auto* edit = new QLineEdit(pop);
  edit->setPlaceholderText("Cerca un componente (es. cerchio, loft, serie)");
  auto* list = new QListWidget(pop);
  lay->addWidget(edit);
  lay->addWidget(list);
  auto fill = [list](const QString& q) {
    list->clear();
    for (const auto* d : SearchDefs(q.toStdString())) {
      auto* it = new QListWidgetItem(QString::fromStdString(d->name + "  —  " + d->category), list);
      it->setData(Qt::UserRole, QString::fromStdString(d->type));
      it->setToolTip(QString::fromStdString(d->description));
    }
    if (list->count()) list->setCurrentRow(0);
  };
  fill("");
  const QPointF at = screen;
  auto choose = [this, list, pop, at] {
    if (auto* it = list->currentItem()) AddNodeAt(it->data(Qt::UserRole).toString().toStdString(), at);
    pop->close();
  };
  connect(edit, &QLineEdit::textChanged, pop, fill);
  connect(edit, &QLineEdit::returnPressed, pop, choose);
  connect(list, &QListWidget::itemActivated, pop, choose);
  pop->resize(320, 300);
  pop->move(mapToGlobal(screen.toPoint()));
  pop->show();
  edit->setFocus();
}

// ================================================================ finestra

NodeEditor::NodeEditor(Engine& e, std::function<void(const std::vector<std::shared_ptr<const ON_Geometry>>&)> preview,
                       std::function<void()> afterBake, QWidget* parent)
    : QWidget(parent, Qt::Window), m_engine(e), m_preview(std::move(preview)), m_afterBake(std::move(afterBake)) {
  setWindowTitle("Editor a nodi - Kurva3D");
  resize(1180, 760);
  auto* lay = new QVBoxLayout(this);
  lay->setContentsMargins(6, 6, 6, 6);
  lay->setSpacing(4);
  // barra: file, anteprima, bake, ricerca
  auto* bar = new QHBoxLayout();
  auto button = [&](const QString& text, const QString& tip) {
    auto* b = new QToolButton(this);
    b->setText(text);
    b->setToolTip(tip);
    bar->addWidget(b);
    return b;
  };
  auto* bNew = button("Nuovo", "Nuovo grafo vuoto");
  auto* bOpen = button("Apri…", "Apri un grafo (.k3nodi)");
  auto* bSave = button("Salva…", "Salva il grafo in un file (.k3nodi); viene salvato anche nel .3dm");
  bar->addSpacing(12);
  auto* bPrev = button("Anteprima", "Mostra nelle viste la geometria dei nodi");
  bPrev->setCheckable(true);
  bPrev->setChecked(true);
  auto* bBake = button("Bake", "Aggiunge al modello la geometria del nodo selezionato");
  auto* bFit = button("Adatta", "Mostra tutti i nodi");
  bar->addStretch();
  auto* search = new QLineEdit(this);
  search->setPlaceholderText("Aggiungi componente: scrivi e premi Invio");
  search->setMaximumWidth(300);
  bar->addWidget(search);
  lay->addLayout(bar);
  // tavolozza per categoria
  auto* tabs = new QTabWidget(this);
  tabs->setMaximumHeight(72);
  std::vector<std::string> cats;
  for (const auto& d : Library())
    if (std::find(cats.begin(), cats.end(), d.category) == cats.end()) cats.push_back(d.category);
  m_canvas = new NodeCanvas(m_graph, this);
  for (const auto& cat : cats) {
    auto* list = new QListWidget(tabs);
    list->setFlow(QListView::LeftToRight);
    list->setWrapping(true);
    list->setResizeMode(QListView::Adjust);
    list->setSpacing(2);
    for (const auto& d : Library()) {
      if (d.category != cat) continue;
      auto* it = new QListWidgetItem(QString::fromStdString(d.name), list);
      it->setData(Qt::UserRole, QString::fromStdString(d.type));
      it->setToolTip(QString::fromStdString(d.description));
    }
    connect(list, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) {
      m_canvas->AddNodeCentered(it->data(Qt::UserRole).toString().toStdString());
    });
    tabs->addTab(list, QString::fromStdString(cat));
  }
  lay->addWidget(tabs);
  lay->addWidget(m_canvas, 1);
  m_status = new QLabel(this);
  lay->addWidget(m_status);
  m_timer = new QTimer(this);
  m_timer->setSingleShot(true);
  m_timer->setInterval(40);
  connect(m_timer, &QTimer::timeout, this, [this] { Evaluate(); });
  connect(m_canvas, &NodeCanvas::Changed, this, [this] { m_timer->start(); });
  connect(m_canvas, &NodeCanvas::Message, m_status, &QLabel::setText);
  connect(bNew, &QToolButton::clicked, this, [this] {
    if (!m_graph.nodes.empty() &&
        QMessageBox::question(this, "Editor a nodi", "Eliminare tutti i nodi?") != QMessageBox::Yes)
      return;
    m_graph.Clear();
    m_path.clear();
    Evaluate();
  });
  connect(bOpen, &QToolButton::clicked, this, [this] { Open(); });
  connect(bSave, &QToolButton::clicked, this, [this] { SaveAs(); });
  connect(bPrev, &QToolButton::toggled, this, [this](bool on) { m_previewAll = on; Evaluate(); });
  connect(bBake, &QToolButton::clicked, this, [this] {
    if (m_canvas->SelectedNode() && m_canvas->bake) m_canvas->bake(m_canvas->SelectedNode());
    else m_status->setText("Seleziona il nodo da aggiungere al modello");
  });
  connect(bFit, &QToolButton::clicked, m_canvas, &NodeCanvas::ZoomAll);
  connect(search, &QLineEdit::returnPressed, this, [this, search] {
    const auto found = SearchDefs(search->text().toStdString());
    if (found.empty()) { m_status->setText("Nessun componente trovato"); return; }
    m_canvas->AddNodeCentered(found.front()->type);
    search->clear();
  });
  m_canvas->bake = [this](int id) {
    int n = 0;
    m_engine.Transaction("Bake", [&] { n = m_graph.Bake(id, m_engine); return n > 0; });
    m_status->setText(n ? QString("Aggiunti al modello %1 oggetti").arg(n) : QString("Il nodo non ha geometria da aggiungere"));
    if (m_afterBake) m_afterBake();
  };
  m_canvas->setFromSelection = [this](int id) {
    std::string ids;
    for (const auto& oid : m_engine.Selection()) {
      char b[64];
      ON_UuidToString(oid, b);
      ids += std::string(ids.empty() ? "" : ",") + b;
    }
    if (ids.empty()) { m_status->setText("Seleziona prima gli oggetti nelle viste"); return; }
    if (Node* n = m_graph.Get(id)) n->params["ids"] = ids;
    Evaluate();
  };
  UpdateStatus();
}

void NodeEditor::Evaluate() {
  m_graph.Evaluate(&m_engine);
  std::vector<std::shared_ptr<const ON_Geometry>> geo;
  if (m_previewAll && isVisible()) m_graph.Preview(geo);
  if (m_preview) m_preview(geo);
  m_canvas->update();
  UpdateStatus();
}

void NodeEditor::UpdateStatus() {
  int errors = 0, warnings = 0;
  for (const auto& n : m_graph.nodes) {
    if (!n.error.empty()) errors++;
    else if (!n.warning.empty()) warnings++;
  }
  QString s = QString("%1 nodi, %2 fili").arg(m_graph.nodes.size()).arg(m_graph.wires.size());
  if (errors) s += QString(" — %1 con errori (bordo rosso)").arg(errors);
  if (warnings) s += QString(" — %1 con avvisi (bordo arancione)").arg(warnings);
  if (m_graph.nodes.empty()) s = "Doppio clic sull'area vuota (o spazio) per cercare un componente; trascina da un'uscita a un ingresso per collegare";
  m_status->setText(s);
}

void NodeEditor::DocumentChanged() {
  for (const auto& n : m_graph.nodes)
    if (n.type == "GeometryRef") { m_timer->start(); return; }
}

void NodeEditor::SaveToDocument() {
  if (m_graph.nodes.empty()) m_engine.Model().SetDocumentUserString(L"Kurva3D.Nodes", nullptr);
  else m_engine.Model().SetDocumentUserString(L"Kurva3D.Nodes", ON_wString(m_graph.Save().c_str()));
}

void NodeEditor::LoadFromDocument() {
  ON_wString w;
  m_graph.Clear();
  if (m_engine.Model().GetDocumentUserString(L"Kurva3D.Nodes", w)) {
    m_graph.Load(ToUtf8(w));
  }
  Evaluate();
  m_canvas->ZoomAll();
}

void NodeEditor::Open() {
  const QString f = QFileDialog::getOpenFileName(this, "Apri grafo", m_path, "Grafo di Kurva3D (*.k3nodi)");
  if (!f.isEmpty()) LoadFile(f);
}

bool NodeEditor::LoadFile(const QString& f) {
  QFile file(f);
  if (!file.open(QIODevice::ReadOnly)) return false;
  std::string err;
  Graph g;
  if (!g.Load(file.readAll().toStdString(), &err)) {
    QMessageBox::warning(this, "Editor a nodi", QString::fromStdString(err));
    return false;
  }
  m_graph = std::move(g);
  m_path = f;
  Evaluate();
  m_canvas->ZoomAll();
  return true;
}

bool NodeEditor::SaveAs() {
  QString f = QFileDialog::getSaveFileName(this, "Salva grafo", m_path.isEmpty() ? "grafo.k3nodi" : m_path, "Grafo di Kurva3D (*.k3nodi)");
  if (f.isEmpty()) return false;
  if (!f.endsWith(".k3nodi", Qt::CaseInsensitive)) f += ".k3nodi";
  QFile file(f);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
  file.write(QByteArray::fromStdString(m_graph.Save()));
  m_path = f;
  m_status->setText("Salvato " + f);
  return true;
}

void NodeEditor::closeEvent(QCloseEvent* e) {
  // chiusa la finestra l'anteprima sparisce; il grafo resta (si riapre con NodeEditor)
  if (m_preview) m_preview({});
  e->accept();
}

}  // namespace cad::ui
