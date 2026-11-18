#include "ui/LayoutView.h"
#include "ui/Viewport.h"
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QWheelEvent>
#include <cmath>

namespace cad::ui {

void PaintPage(QPainter& p, const PageDrawing& pg, double s, double ox, double oy) {
  auto X = [&](double x) { return ox + x * s; };
  auto Y = [&](double y) { return oy + (pg.heightMm - y) * s; };
  p.save();
  p.setRenderHint(QPainter::Antialiasing, true);
  p.setRenderHint(QPainter::SmoothPixmapTransform, true);
  for (const auto& im : pg.images) {
    if (im.pw <= 0 || im.ph <= 0 || (int)im.rgb.size() != im.pw * im.ph * 3) continue;
    const QImage img(im.rgb.data(), im.pw, im.ph, im.pw * 3, QImage::Format_RGB888);
    p.drawImage(QRectF(X(im.x), Y(im.y + im.h), im.w * s, im.h * s), img);
  }
  p.setPen(Qt::NoPen);
  for (const auto& f : pg.fills) {
    QPolygonF poly;
    for (const auto& q : f.pts) poly << QPointF(X(q.x), Y(q.y));
    p.setBrush(QColor(f.color.Red(), f.color.Green(), f.color.Blue()));
    p.drawPolygon(poly);
  }
  p.setBrush(Qt::NoBrush);
  for (const auto& l : pg.lines) {
    QPen pen(QColor(l.color.Red(), l.color.Green(), l.color.Blue()));
    pen.setWidthF(std::max(l.widthMm * s, 0.0));
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    pen.setCosmetic(l.widthMm * s < 1.0);  // a schermo almeno un pixel
    p.setPen(pen);
    QPolygonF poly;
    for (const auto& q : l.pts) poly << QPointF(X(q.x), Y(q.y));
    p.drawPolyline(poly);
  }
  for (const auto& t : pg.texts) {
    // unita' di 1/100 mm per avere caratteri interi anche quando sono piccoli
    QFont f("Arial");
    const int px = std::max(1, (int)std::lround(t.heightMm / 0.718 * 100));
    f.setPixelSize(px);
    p.save();
    p.translate(X(t.pos.x), Y(t.pos.y));
    p.rotate(-t.angle * 180.0 / ON_PI);
    p.scale(s / 100, s / 100);
    p.setFont(f);
    p.setPen(QColor(t.color.Red(), t.color.Green(), t.color.Blue()));
    const QString str = QString::fromStdString(t.text);
    const double w = QFontMetricsF(f).horizontalAdvance(str);
    p.drawText(QPointF(-w * (t.halign == 1 ? 0.5 : t.halign == 2 ? 1.0 : 0.0), 0), str);
    p.restore();
  }
  p.restore();
}

LayoutView::LayoutView(Engine& e, const Scene& modelScene, QWidget* parent)
    : QWidget(parent), m_engine(e), m_model(modelScene), m_page(e) {
  m_page.fixedSpace = true;
  setMouseTracking(true);
  setFocusPolicy(Qt::StrongFocus);
  setMinimumSize(200, 150);
  m_refresh = new QTimer(this);
  m_refresh->setSingleShot(true);
  m_refresh->setInterval(300);
  connect(m_refresh, &QTimer::timeout, this, [this] { Refresh(); });
}

void LayoutView::SetLayout(const ON_UUID& id) {
  if (m_layout != id) m_fitted = false;
  m_layout = id;
  m_page.space = id;
  Refresh();
}

void LayoutView::Refresh() {
  m_page.Rebuild();
  const int li = FindLayout(m_engine.Model(), m_layout);
  m_drawing = PageDrawing();
  if (li >= 0) {
    PrintOptions o;
    o.dpi = std::clamp(m_scale * 25.4 * devicePixelRatioF(), 36.0, 150.0);
    ComposeLayout(m_engine, li, o, m_drawing, &m_model);
  }
  if (!m_fitted && width() > 10) FitPage();
  update();
}

void LayoutView::FitPage() {
  const double w = m_drawing.widthMm, h = m_drawing.heightMm;
  if (w <= 0 || h <= 0 || width() < 10) return;
  m_scale = std::min((width() - 40) / w, (height() - 40) / h);
  if (m_scale <= 0) m_scale = 1;
  m_ox = (width() - w * m_scale) / 2;
  m_oy = (height() - h * m_scale) / 2;
  m_fitted = true;
}

void LayoutView::resizeEvent(QResizeEvent*) {
  if (!m_fitted) Refresh();
  else FitPage();
}

ON_3dPoint LayoutView::ToPage(const QPointF& p) const {
  return ON_3dPoint((p.x() - m_ox) / m_scale, m_drawing.heightMm - (p.y() - m_oy) / m_scale, 0);
}

QPointF LayoutView::ToScreen(const ON_3dPoint& p) const {
  return QPointF(m_ox + p.x * m_scale, m_oy + (m_drawing.heightMm - p.y) * m_scale);
}

ON_3dPoint LayoutView::Snapped(const QPointF& sp, bool* snapped) const {
  if (snapped) *snapped = false;
  ON_3dPoint best = ToPage(sp);
  double bd = 10;  // pixel
  for (const auto& o : m_page.Objects())
    for (const auto& s : o.snapPoints) {
      const QPointF q = ToScreen(s.p);
      const double d = std::hypot(q.x() - sp.x(), q.y() - sp.y());
      if (d < bd) { bd = d; best = s.p; if (snapped) *snapped = true; }
    }
  if ((!snapped || !*snapped) && Viewport::Shared().gridSnap) {
    const double g = Viewport::Shared().gridSpacing > 0 ? Viewport::Shared().gridSpacing : 1;
    best.x = std::round(best.x / g) * g;
    best.y = std::round(best.y / g) * g;
  }
  return best;
}

std::vector<ON_UUID> LayoutView::PickAt(const QPointF& sp) const {
  double bd = 6;
  ON_UUID best = ON_nil_uuid;
  auto segDist = [](const QPointF& p, const QPointF& a, const QPointF& b) {
    const QPointF ab = b - a, ap = p - a;
    const double l2 = ab.x() * ab.x() + ab.y() * ab.y();
    double t = l2 > 0 ? (ap.x() * ab.x() + ap.y() * ab.y()) / l2 : 0;
    t = std::clamp(t, 0.0, 1.0);
    const QPointF c = a + ab * t;
    return std::hypot(p.x() - c.x(), p.y() - c.y());
  };
  for (const auto& o : m_page.Objects()) {
    auto test = [&](const std::vector<ON_3dPoint>& w) {
      for (size_t i = 0; i + 1 < w.size(); i++) {
        const double d = segDist(sp, ToScreen(w[i]), ToScreen(w[i + 1]));
        if (d < bd) { bd = d; best = o.id; }
      }
    };
    for (const auto& w : o.wires) test(w);
    for (const auto& t : o.triangles) test({t, t});
    for (const auto& l : o.labels) {  // testo: clic vicino al punto di inserimento o sopra la riga
      const QPointF a = ToScreen(l.origin), b = ToScreen(l.origin + l.xdir * l.height * 0.6 * (double)std::max<size_t>(1, l.text.size()));
      const QPointF up = ToScreen(l.origin + l.ydir * l.height) - a;
      QPolygonF box;
      box << a << b << b + up << a + up;
      if (box.containsPoint(sp, Qt::OddEvenFill)) { bd = 0; best = o.id; }
    }
    for (const auto& q : o.points) test({q, q});
  }
  if (ON_UuidIsNil(best)) return {};
  return {best};
}

std::vector<ON_UUID> LayoutView::PickWindow(const QPointF& a, const QPointF& b) const {
  const QRectF r = QRectF(a, b).normalized();
  const bool crossing = b.x() < a.x();  // da destra a sinistra: anche gli oggetti toccati
  std::vector<ON_UUID> out;
  for (const auto& o : m_page.Objects()) {
    bool all = true, any = false;
    auto test = [&](const ON_3dPoint& p) {
      const bool in = r.contains(ToScreen(p));
      all = all && in;
      any = any || in;
    };
    for (const auto& w : o.wires) for (const auto& p : w) test(p);
    for (const auto& l : o.labels) test(l.origin);
    for (const auto& p : o.points) test(p);
    if (crossing ? any : (all && any)) out.push_back(o.id);
  }
  return out;
}

void LayoutView::paintEvent(QPaintEvent*) {
  QPainter p(this);
  const auto& C = Viewport::Shared().colors;
  p.fillRect(rect(), C.dark ? QColor(58, 60, 64) : QColor(160, 163, 168));
  const QRectF sheet(m_ox, m_oy, m_drawing.widthMm * m_scale, m_drawing.heightMm * m_scale);
  p.fillRect(sheet.translated(4, 4), QColor(0, 0, 0, 60));
  p.fillRect(sheet, Qt::white);
  PaintPage(p, m_drawing, m_scale, m_ox, m_oy);
  // oggetti di pagina selezionati
  p.setRenderHint(QPainter::Antialiasing, true);
  QPen sel(C.selected);
  sel.setWidthF(2);
  p.setPen(sel);
  for (const auto& o : m_page.Objects()) {
    if (!o.selected) continue;
    for (const auto& w : o.wires) {
      QPolygonF poly;
      for (const auto& q : w) poly << ToScreen(q);
      p.drawPolyline(poly);
    }
    for (const auto& l : o.labels) {
      const QPointF a = ToScreen(l.origin);
      p.drawLine(a, ToScreen(l.origin + l.xdir * l.height * 0.6 * (double)std::max<size_t>(1, l.text.size())));
    }
  }
  // finestra di selezione ed elastico
  if (m_window) {
    QPen pen(C.overlay);
    pen.setStyle(m_hover.x() < m_press.x() ? Qt::DashLine : Qt::SolidLine);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(m_press, m_hover).normalized());
  }
  if (Viewport::Shared().waitingForPoint) {
    bool snapped = false;
    const QPointF h = ToScreen(Snapped(m_hover, &snapped));
    p.setPen(QPen(snapped ? QColor(230, 120, 0) : C.overlay, 1));
    p.drawLine(h + QPointF(-8, 0), h + QPointF(8, 0));
    p.drawLine(h + QPointF(0, -8), h + QPointF(0, 8));
    if (Viewport::Shared().hasLastPoint) {
      QPen d(C.overlay);
      d.setStyle(Qt::DashLine);
      p.setPen(d);
      p.drawLine(ToScreen(Viewport::Shared().lastPoint), h);
    }
  }
  p.setPen(C.overlay);
  p.drawText(QPointF(8, 18), QString::fromStdString(m_drawing.name) +
                                 QString("  (%1 x %2 mm)").arg(m_drawing.widthMm).arg(m_drawing.heightMm));
}

void LayoutView::mousePressEvent(QMouseEvent* e) {
  m_press = m_last = m_hover = e->position();
  m_button = e->button();
  setFocus();
}

void LayoutView::mouseMoveEvent(QMouseEvent* e) {
  const QPointF pos = e->position();
  if (m_button == Qt::RightButton || m_button == Qt::MiddleButton) {
    m_ox += pos.x() - m_last.x();
    m_oy += pos.y() - m_last.y();
  } else if (m_button == Qt::LeftButton && !Viewport::Shared().waitingForPoint &&
             (pos - m_press).manhattanLength() > 4) {
    m_window = true;
  }
  m_last = m_hover = pos;
  emit MouseWorld(Snapped(pos));
  update();
}

void LayoutView::mouseReleaseEvent(QMouseEvent* e) {
  const bool drag = (e->position() - m_press).manhattanLength() > 4;
  if (m_button == Qt::LeftButton) {
    const bool add = e->modifiers() & Qt::ShiftModifier, remove = e->modifiers() & Qt::ControlModifier;
    if (m_window) emit ObjectsClicked(PickWindow(m_press, e->position()), add, remove);
    else if (Viewport::Shared().waitingForPoint) emit PointPicked(Snapped(e->position()));
    else if (!drag) emit ObjectsClicked(PickAt(e->position()), add, remove);
  }
  m_window = false;
  m_button = Qt::NoButton;
  update();
}

void LayoutView::wheelEvent(QWheelEvent* e) {
  const double f = std::pow(1.0015, e->angleDelta().y());
  const QPointF c = e->position();
  m_ox = c.x() - (c.x() - m_ox) * f;
  m_oy = c.y() - (c.y() - m_oy) * f;
  m_scale *= f;
  update();
  m_refresh->start();  // le viste ombreggiate si ricalcolano alla nuova risoluzione
}

}  // namespace cad::ui
