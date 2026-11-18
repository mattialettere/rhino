#include "ui/Theme.h"
#include "ui/Viewport.h"
#include <QApplication>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QSettings>
#include <QStyle>
#include <array>
#include <QStyleFactory>
#include <QStyleHints>
#include <cmath>
#include <functional>
#include <map>

namespace cad::ui {

namespace {
ThemePalette g_pal;

ThemePalette LightPalette() {
  ThemePalette p;
  p.dark = false;
  p.window = QColor("#eef0f3");
  p.panel = QColor("#f8f9fb");
  p.base = QColor("#ffffff");
  p.alt = QColor("#f3f5f8");
  p.text = QColor("#1d2025");
  p.muted = QColor("#6a717c");
  p.border = QColor("#d5d9df");
  p.hover = QColor("#e3e7ee");
  p.pressed = QColor("#d3dae6");
  p.accent = QColor("#2f6fde");
  p.accentText = QColor("#ffffff");
  return p;
}

ThemePalette DarkPalette() {
  ThemePalette p;
  p.dark = true;
  p.window = QColor("#1c1d20");
  p.panel = QColor("#232529");
  p.base = QColor("#2a2c31");
  p.alt = QColor("#2f3237");
  p.text = QColor("#e4e6ea");
  p.muted = QColor("#9aa1ab");
  p.border = QColor("#383b42");
  p.hover = QColor("#33363c");
  p.pressed = QColor("#3d4149");
  p.accent = QColor("#4b8dff");
  p.accentText = QColor("#ffffff");
  return p;
}

QString Css(const ThemePalette& p) {
  QString s = R"(
QMainWindow, QDialog { background: @window; }
QWidget { color: @text; }
QToolTip { color: @text; background: @panel; border: 1px solid @border; padding: 4px 6px; border-radius: 6px; }
QMenuBar { background: @window; padding: 2px 4px; }
QMenuBar::item { padding: 5px 10px; border-radius: 6px; background: transparent; }
QMenuBar::item:selected { background: @hover; }
QMenu { background: @panel; border: 1px solid @border; border-radius: 8px; padding: 5px; }
QMenu::item { padding: 6px 26px 6px 14px; border-radius: 6px; }
QMenu::item:selected { background: @accent; color: @accentText; }
QMenu::separator { height: 1px; background: @border; margin: 4px 8px; }
QToolBar { background: @window; border: none; spacing: 2px; padding: 3px; }
QToolBar::separator { background: @border; width: 1px; height: 1px; margin: 4px; }
QToolButton { background: transparent; border: 1px solid transparent; border-radius: 7px; padding: 4px 8px; }
QToolButton:hover { background: @hover; }
QToolButton:pressed, QToolButton:checked { background: @pressed; border-color: @border; }
QPushButton { background: @base; border: 1px solid @border; border-radius: 7px; padding: 5px 14px; }
QPushButton:hover { background: @hover; }
QPushButton:pressed { background: @pressed; }
QPushButton:default { background: @accent; color: @accentText; border-color: @accent; }
QPushButton:default:hover { background: @accentHover; }
QPushButton:disabled { color: @muted; background: @alt; }
QLineEdit, QPlainTextEdit, QTextEdit, QComboBox, QSpinBox, QDoubleSpinBox {
  background: @base; border: 1px solid @border; border-radius: 7px; padding: 4px 6px;
  selection-background-color: @accent; selection-color: @accentText; }
QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus, QComboBox:focus { border: 1px solid @accent; }
QComboBox::drop-down { border: none; width: 18px; }
QComboBox::down-arrow { image: url(:/theme/down.png); }
QAbstractSpinBox::up-button, QAbstractSpinBox::down-button { border: none; background: transparent; width: 16px; }
QAbstractSpinBox::up-arrow { image: url(:/theme/up.png); }
QAbstractSpinBox::down-arrow { image: url(:/theme/down.png); }
QComboBox QAbstractItemView { background: @panel; border: 1px solid @border; selection-background-color: @accent; }
QTabWidget::pane { border: none; background: @window; }
QTabBar::tab { background: transparent; color: @muted; padding: 6px 12px; margin-right: 2px; border: none;
  border-bottom: 2px solid transparent; }
QTabBar::tab:selected { color: @text; border-bottom: 2px solid @accent; }
QTabBar::tab:hover:!selected { color: @text; background: @hover; border-radius: 6px; }
QDockWidget { titlebar-close-icon: none; }
QDockWidget::title { background: @window; padding: 6px 8px; text-align: left; }
QTreeWidget, QTableWidget, QListWidget { background: @base; alternate-background-color: @alt; border: 1px solid @border;
  border-radius: 7px; selection-background-color: @accent; selection-color: @accentText; }
QHeaderView::section { background: @panel; color: @muted; border: none; border-bottom: 1px solid @border; padding: 4px 6px; }
QStatusBar { background: @window; border-top: 1px solid @border; }
QStatusBar QLabel { color: @muted; }
QCheckBox { spacing: 6px; }
QCheckBox::indicator { width: 14px; height: 14px; border: 1px solid @muted; border-radius: 4px; background: @base; }
QCheckBox::indicator:hover { border-color: @accent; }
QCheckBox::indicator:checked { background: @accent; border-color: @accent; image: url(:/theme/check.png); }
QProgressBar { background: @alt; border: none; border-radius: 4px; text-align: center; }
QProgressBar::chunk { background: @accent; border-radius: 4px; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }
QScrollBar::handle:vertical { background: @border; border-radius: 5px; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: @muted; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 0; }
QScrollBar::handle:horizontal { background: @border; border-radius: 5px; min-width: 30px; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QSplitter::handle { background: @window; }
QLabel#panelTitle { font-weight: 600; color: @text; }
QLabel#hint { color: @muted; }
QWidget#sidebar QToolButton { padding: 3px; border-radius: 8px; }
QWidget#commandBar { background: @panel; border: 1px solid @border; border-radius: 9px; }
QWidget#commandBar QLineEdit { border: none; background: transparent; padding: 3px 4px; }
QWidget#commandBar QPlainTextEdit { border: none; background: transparent; }
)";
  const QColor accentHover = p.dark ? p.accent.lighter(112) : p.accent.darker(110);
  const std::pair<const char*, QColor> vars[] = {
      {"@accentText", p.accentText}, {"@accentHover", accentHover}, {"@accent", p.accent}, {"@window", p.window},
      {"@panel", p.panel}, {"@base", p.base}, {"@alt", p.alt}, {"@text", p.text}, {"@muted", p.muted},
      {"@border", p.border}, {"@hover", p.hover}, {"@pressed", p.pressed}};
  for (const auto& [k, c] : vars) s.replace(k, c.name());
  return s;
}

void SetViewportColors(bool dark) {
  auto& C = Viewport::Shared().colors;
  C.dark = dark;
  if (dark) {
    C.bgTop = QColor("#363940");
    C.bgBottom = QColor("#202226");
    C.gridMinor = QColor("#33363c");
    C.gridMajor = QColor("#464a52");
    C.axisX = QColor("#b05a5a");
    C.axisY = QColor("#58a066");
    C.ink = QColor("#dde0e5");
    C.overlay = QColor("#e4e6ea");
    C.cage = QColor("#9aa1ab");
  } else {
    C.bgTop = QColor("#d6d9de");
    C.bgBottom = QColor("#b7bbc2");
    C.gridMinor = QColor("#abafb6");
    C.gridMajor = QColor("#91969e");
    C.axisX = QColor("#b04848");
    C.axisY = QColor("#3f9a50");
    C.ink = QColor("#000000");
    C.overlay = QColor("#1f2226");
    C.cage = QColor("#4a4e55");
  }
  C.selected = QColor("#ffd200");
}
}  // namespace

ThemeMode SavedThemeMode() {
  const QString v = QSettings().value("ui/theme", "auto").toString();
  return v == "light" ? ThemeMode::Light : v == "dark" ? ThemeMode::Dark : ThemeMode::Auto;
}

void SaveThemeMode(ThemeMode m) {
  QSettings().setValue("ui/theme", m == ThemeMode::Light ? "light" : m == ThemeMode::Dark ? "dark" : "auto");
}

bool SystemPrefersDark() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  const auto cs = QGuiApplication::styleHints()->colorScheme();
  if (cs == Qt::ColorScheme::Dark) return true;
  if (cs == Qt::ColorScheme::Light) return false;
#endif
#ifdef Q_OS_WIN
  QSettings reg("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", QSettings::NativeFormat);
  return reg.value("AppsUseLightTheme", 1).toInt() == 0;
#else
  return false;
#endif
}

const ThemePalette& CurrentPalette() { return g_pal; }

void ApplyTheme(ThemeMode m) {
  const bool dark = m == ThemeMode::Dark || (m == ThemeMode::Auto && SystemPrefersDark());
  g_pal = dark ? DarkPalette() : LightPalette();
  auto* app = qobject_cast<QApplication*>(QCoreApplication::instance());
  if (!app) return;
  if (QStyle* fusion = QStyleFactory::create("Fusion")) app->setStyle(fusion);
  QPalette pal;
  pal.setColor(QPalette::Window, g_pal.window);
  pal.setColor(QPalette::WindowText, g_pal.text);
  pal.setColor(QPalette::Base, g_pal.base);
  pal.setColor(QPalette::AlternateBase, g_pal.alt);
  pal.setColor(QPalette::Text, g_pal.text);
  pal.setColor(QPalette::Button, g_pal.panel);
  pal.setColor(QPalette::ButtonText, g_pal.text);
  pal.setColor(QPalette::Highlight, g_pal.accent);
  pal.setColor(QPalette::HighlightedText, g_pal.accentText);
  pal.setColor(QPalette::ToolTipBase, g_pal.panel);
  pal.setColor(QPalette::ToolTipText, g_pal.text);
  pal.setColor(QPalette::PlaceholderText, g_pal.muted);
  pal.setColor(QPalette::Link, g_pal.accent);
  pal.setColor(QPalette::Mid, g_pal.border);
  pal.setColor(QPalette::Dark, g_pal.border.darker(130));
  pal.setColor(QPalette::Light, g_pal.base);
  pal.setColor(QPalette::Disabled, QPalette::Text, g_pal.muted);
  pal.setColor(QPalette::Disabled, QPalette::ButtonText, g_pal.muted);
  pal.setColor(QPalette::Disabled, QPalette::WindowText, g_pal.muted);
  app->setPalette(pal);
  app->setStyleSheet(Css(g_pal));
  SetViewportColors(dark);
}

// --- Icone ------------------------------------------------------------------------------------
// Disegnate su una griglia 24x24 con tratti arrotondati: inchiostro del tema e accento.
namespace {
using Draw = std::function<void(QPainter&, const QPen& ink, const QPen& acc, const QColor& accFill)>;

void Dot(QPainter& p, const QColor& c, double x, double y, double r = 1.6) {
  p.save();
  p.setPen(Qt::NoPen);
  p.setBrush(c);
  p.drawEllipse(QPointF(x, y), r, r);
  p.restore();
}

const std::map<QString, Draw>& Icons() {
  static const std::map<QString, Draw> m = {
      {"new", [](QPainter& p, const QPen& k, const QPen& a, const QColor&) {
         p.setPen(k); p.drawPolygon(QPolygonF{{6, 3}, {14, 3}, {18, 7}, {18, 21}, {6, 21}});
         p.drawPolyline(QPolygonF{{14, 3}, {14, 7}, {18, 7}});
         p.setPen(a); p.drawLine(QPointF(12, 11), QPointF(12, 17)); p.drawLine(QPointF(9, 14), QPointF(15, 14)); }},
      {"undo", [](QPainter& p, const QPen& k, const QPen& a, const QColor&) {
         QPainterPath c; c.moveTo(6, 10); c.lineTo(14, 10); c.cubicTo(22, 10, 22, 20, 14, 20); c.lineTo(9, 20);
         p.setPen(k); p.drawPath(c); p.setPen(a); p.drawPolyline(QPolygonF{{10, 6}, {6, 10}, {10, 14}}); }},
      {"redo", [](QPainter& p, const QPen& k, const QPen& a, const QColor&) {
         QPainterPath c; c.moveTo(18, 10); c.lineTo(10, 10); c.cubicTo(2, 10, 2, 20, 10, 20); c.lineTo(15, 20);
         p.setPen(k); p.drawPath(c); p.setPen(a); p.drawPolyline(QPolygonF{{14, 6}, {18, 10}, {14, 14}}); }},
      {"selall", [](QPainter& p, const QPen& k, const QPen& a, const QColor&) {
         QPen d = k; d.setStyle(Qt::DashLine); d.setWidthF(1.2); p.setPen(d); p.drawRect(QRectF(3, 3, 18, 18));
         p.setPen(a); p.drawRect(QRectF(7, 7, 5, 5)); p.drawEllipse(QPointF(15, 15), 3, 3); }},
      {"zoom", [](QPainter& p, const QPen& k, const QPen& a, const QColor&) {
         p.setPen(k); p.drawEllipse(QPointF(10, 10), 6, 6); p.drawLine(QPointF(14.5, 14.5), QPointF(20, 20));
         p.setPen(a); p.drawLine(QPointF(10, 7), QPointF(10, 13)); p.drawLine(QPointF(7, 10), QPointF(13, 10)); }},
      {"what", [](QPainter& p, const QPen& k, const QPen& a, const QColor& af) {
         p.setPen(k); p.drawEllipse(QPointF(12, 12), 9, 9);
         p.setPen(a); p.drawLine(QPointF(12, 11), QPointF(12, 17)); Dot(p, af, 12, 7.5, 1.4); }},
      {"layer", [](QPainter& p, const QPen& k, const QPen& a, const QColor& af) {
         QColor f = af; f.setAlpha(70);
         p.setPen(k); p.drawPolygon(QPolygonF{{12, 12}, {21, 16}, {12, 20}, {3, 16}});
         p.setPen(a); p.setBrush(f); p.drawPolygon(QPolygonF{{12, 4}, {21, 8}, {12, 12}, {3, 8}}); }},
      {"hide", [](QPainter& p, const QPen& k, const QPen& a, const QColor&) {
         QPainterPath e; e.moveTo(3, 12); e.quadTo(12, 3, 21, 12); e.quadTo(12, 21, 3, 12);
         p.setPen(k); p.drawPath(e); p.drawEllipse(QPointF(12, 12), 2.8, 2.8);
         p.setPen(a); p.drawLine(QPointF(4, 20), QPointF(20, 4)); }},
      {"show", [](QPainter& p, const QPen& k, const QPen&, const QColor& af) {
         QPainterPath e; e.moveTo(3, 12); e.quadTo(12, 3, 21, 12); e.quadTo(12, 21, 3, 12);
         p.setPen(k); p.drawPath(e); Dot(p, af, 12, 12, 3); }},
      {"lock", [](QPainter& p, const QPen& k, const QPen& a, const QColor&) {
         p.setPen(k); p.drawRoundedRect(QRectF(5, 11, 14, 10), 2, 2);
         p.setPen(a); p.drawArc(QRectF(8, 4, 8, 12), 0, 180 * 16); p.drawLine(QPointF(8, 10), QPointF(8, 11));
         p.drawLine(QPointF(16, 10), QPointF(16, 11)); }},
      {"unlock", [](QPainter& p, const QPen& k, const QPen& a, const QColor&) {
         p.setPen(k); p.drawRoundedRect(QRectF(5, 11, 14, 10), 2, 2);
         p.setPen(a); p.drawArc(QRectF(13, 3, 8, 12), 0, 180 * 16); p.drawLine(QPointF(21, 9), QPointF(21, 11)); }},
      {"line", [](QPainter& p, const QPen& k, const QPen& a, const QColor& af) {
         p.setPen(k); p.drawLine(QPointF(4, 20), QPointF(20, 4));
         Dot(p, af, 4, 20, 2.2); Dot(p, af, 20, 4, 2.2); (void)a; }},
      {"polyline", [](QPainter& p, const QPen& k, const QPen&, const QColor& af) {
         p.setPen(k); QPolygonF q{{3, 19}, {9, 7}, {15, 16}, {21, 5}}; p.drawPolyline(q);
         for (auto& v : q) Dot(p, af, v.x(), v.y()); }},
      {"circle", [](QPainter& p, const QPen& k, const QPen&, const QColor& af) {
         p.setPen(k); p.drawEllipse(QPointF(12, 12), 8.5, 8.5); Dot(p, af, 12, 12); }},
      {"arc", [](QPainter& p, const QPen& k, const QPen&, const QColor& af) {
         p.setPen(k); p.drawArc(QRectF(3.5, 6.5, 17, 17), 0, 180 * 16); Dot(p, af, 3.5, 15); Dot(p, af, 20.5, 15); }},
      {"rectangle", [](QPainter& p, const QPen& k, const QPen&, const QColor& af) {
         p.setPen(k); p.drawRoundedRect(QRectF(3.5, 6, 17, 12), 1, 1); Dot(p, af, 3.5, 18); Dot(p, af, 20.5, 6); }},
      {"curve", [](QPainter& p, const QPen& k, const QPen& a, const QColor&) {
         QPen d = a; d.setStyle(Qt::DashLine); d.setWidthF(1.0); p.setPen(d);
         p.drawPolyline(QPolygonF{{3, 19}, {7, 4}, {17, 21}, {21, 5}});
         QPainterPath c; c.moveTo(3, 19); c.cubicTo(7, 4, 17, 21, 21, 5); p.setPen(k); p.drawPath(c); }},
      {"interpcrv", [](QPainter& p, const QPen& k, const QPen&, const QColor& af) {
         QPainterPath c; c.moveTo(3, 17); c.cubicTo(6, 6, 10, 6, 12, 12); c.cubicTo(14, 18, 18, 18, 21, 7);
         p.setPen(k); p.drawPath(c); Dot(p, af, 3, 17); Dot(p, af, 12, 12); Dot(p, af, 21, 7); }},
      {"box", [](QPainter& p, const QPen& k, const QPen&, const QColor& af) {
         QColor f = af; f.setAlpha(60); p.setPen(k); p.setBrush(f);
         p.drawPolygon(QPolygonF{{4, 9}, {13, 9}, {13, 20}, {4, 20}});
         p.setBrush(Qt::NoBrush);
         p.drawPolygon(QPolygonF{{4, 9}, {10, 4}, {20, 4}, {13, 9}});
         p.drawPolygon(QPolygonF{{13, 9}, {20, 4}, {20, 15}, {13, 20}}); }},
      {"sphere", [](QPainter& p, const QPen& k, const QPen& a, const QColor&) {
         p.setPen(k); p.drawEllipse(QPointF(12, 12), 8.5, 8.5);
         QPen t = a; t.setWidthF(1.2); p.setPen(t); p.drawEllipse(QPointF(12, 12), 8.5, 3); p.drawEllipse(QPointF(12, 12), 3, 8.5); }},
      {"cylinder", [](QPainter& p, const QPen& k, const QPen&, const QColor& af) {
         QColor f = af; f.setAlpha(60); p.setPen(k);
         p.setBrush(f); p.drawEllipse(QPointF(12, 6), 7, 2.6); p.setBrush(Qt::NoBrush);
         p.drawLine(QPointF(5, 6), QPointF(5, 18)); p.drawLine(QPointF(19, 6), QPointF(19, 18));
         p.drawArc(QRectF(5, 15.4, 14, 5.2), 180 * 16, 180 * 16); }},
      {"extrudecrv", [](QPainter& p, const QPen& k, const QPen& a, const QColor&) {
         p.setPen(a); p.drawEllipse(QPointF(12, 18), 7, 2.5);
         QPen d = k; d.setStyle(Qt::DashLine); d.setWidthF(1.1); p.setPen(d); p.drawEllipse(QPointF(12, 6), 7, 2.5);
         p.setPen(k); p.drawLine(QPointF(12, 15), QPointF(12, 8)); p.drawLine(QPointF(9.5, 10.5), QPointF(12, 8)); p.drawLine(QPointF(14.5, 10.5), QPointF(12, 8)); }},
      {"booleanunion", [](QPainter& p, const QPen& k, const QPen&, const QColor& af) {
         QPainterPath u; u.addEllipse(QPointF(9, 12), 6.5, 6.5); QPainterPath v; v.addEllipse(QPointF(15, 12), 6.5, 6.5);
         QColor f = af; f.setAlpha(110); p.setPen(k); p.setBrush(f); p.drawPath(u.united(v)); }},
      {"booleandifference", [](QPainter& p, const QPen& k, const QPen& a, const QColor& af) {
         QPainterPath u; u.addEllipse(QPointF(9, 12), 7, 7); QPainterPath v; v.addEllipse(QPointF(16, 12), 6, 6);
         QColor f = af; f.setAlpha(110); p.setPen(k); p.setBrush(f); p.drawPath(u.subtracted(v));
         QPen d = a; d.setStyle(Qt::DashLine); d.setWidthF(1.1); p.setPen(d); p.setBrush(Qt::NoBrush); p.drawEllipse(QPointF(16, 12), 6, 6); }},
      {"filletedge", [](QPainter& p, const QPen& k, const QPen& a, const QColor&) {
         p.setPen(k); p.drawLine(QPointF(4, 4), QPointF(4, 12)); p.drawLine(QPointF(12, 20), QPointF(20, 20));
         QPainterPath c; c.moveTo(4, 12); c.quadTo(4, 20, 12, 20); p.setPen(a); p.drawPath(c); }},
      {"fillet", [](QPainter& p, const QPen& k, const QPen& a, const QColor&) {
         p.setPen(k); p.drawLine(QPointF(4, 20), QPointF(4, 11)); p.drawLine(QPointF(11, 4), QPointF(20, 4));
         QPainterPath c; c.moveTo(4, 11); c.quadTo(4, 4, 11, 4); p.setPen(a); p.drawPath(c); }},
      {"offset", [](QPainter& p, const QPen& k, const QPen& a, const QColor&) {
         QPainterPath c; c.moveTo(3, 15); c.cubicTo(8, 5, 14, 5, 21, 13); p.setPen(k); p.drawPath(c);
         QPainterPath d; d.moveTo(3, 21); d.cubicTo(8, 11, 14, 11, 21, 19); p.setPen(a); p.drawPath(d); }},
      {"trim", [](QPainter& p, const QPen& k, const QPen& a, const QColor&) {
         p.setPen(k); p.drawLine(QPointF(12, 3), QPointF(12, 21)); p.drawLine(QPointF(3, 12), QPointF(12, 12));
         QPen d = a; d.setStyle(Qt::DotLine); p.setPen(d); p.drawLine(QPointF(12, 12), QPointF(21, 12)); }},
      {"extend", [](QPainter& p, const QPen& k, const QPen& a, const QColor&) {
         p.setPen(k); p.drawLine(QPointF(20, 3), QPointF(20, 21)); p.drawLine(QPointF(3, 12), QPointF(11, 12));
         QPen d = a; d.setStyle(Qt::DashLine); p.setPen(d); p.drawLine(QPointF(11, 12), QPointF(20, 12)); }},
      {"move", [](QPainter& p, const QPen& k, const QPen&, const QColor&) {
         p.setPen(k); p.drawLine(QPointF(12, 3), QPointF(12, 21)); p.drawLine(QPointF(3, 12), QPointF(21, 12));
         for (auto [x, y, dx, dy] : std::initializer_list<std::array<double, 4>>{{12, 3, 1, 0}, {12, 21, 1, 0}, {3, 12, 0, 1}, {21, 12, 0, 1}}) {
           const double sx = x == 12 ? 0 : (x < 12 ? 1 : -1), sy = y == 12 ? 0 : (y < 12 ? 1 : -1);
           p.drawLine(QPointF(x, y), QPointF(x + sx * 3 + dx * 3, y + sy * 3 + dy * 3));
           p.drawLine(QPointF(x, y), QPointF(x + sx * 3 - dx * 3, y + sy * 3 - dy * 3));
         } }},
      {"copy", [](QPainter& p, const QPen& k, const QPen& a, const QColor&) {
         p.setPen(a); p.drawRoundedRect(QRectF(4, 4, 11, 11), 2, 2); p.setPen(k); p.drawRoundedRect(QRectF(9, 9, 11, 11), 2, 2); }},
      {"rotate", [](QPainter& p, const QPen& k, const QPen&, const QColor& af) {
         p.setPen(k); p.drawArc(QRectF(4, 4, 16, 16), 90 * 16, 270 * 16);
         p.drawLine(QPointF(12, 4), QPointF(15.5, 1.5)); p.drawLine(QPointF(12, 4), QPointF(15.5, 6.5)); Dot(p, af, 12, 12); }},
      {"scale", [](QPainter& p, const QPen& k, const QPen& a, const QColor&) {
         p.setPen(a); p.drawRect(QRectF(3.5, 13.5, 7, 7)); p.setPen(k); p.drawRect(QRectF(3.5, 3.5, 17, 17));
         p.drawLine(QPointF(10.5, 13.5), QPointF(17, 7)); p.drawLine(QPointF(17, 7), QPointF(13, 7)); p.drawLine(QPointF(17, 7), QPointF(17, 11)); }},
      {"mirror", [](QPainter& p, const QPen& k, const QPen& a, const QColor& af) {
         QPen d = a; d.setStyle(Qt::DashLine); d.setWidthF(1.2); p.setPen(d); p.drawLine(QPointF(12, 2), QPointF(12, 22));
         QColor f = af; f.setAlpha(110); p.setPen(k); p.setBrush(f); p.drawPolygon(QPolygonF{{9, 6}, {9, 18}, {3, 18}});
         p.setBrush(Qt::NoBrush); p.drawPolygon(QPolygonF{{15, 6}, {15, 18}, {21, 18}}); }},
      {"join", [](QPainter& p, const QPen& k, const QPen&, const QColor& af) {
         p.setPen(k); p.drawLine(QPointF(3, 17), QPointF(11, 9)); p.drawLine(QPointF(13, 9), QPointF(21, 17)); Dot(p, af, 12, 9, 2.4); }},
      {"explode", [](QPainter& p, const QPen& k, const QPen& a, const QColor&) {
         p.setPen(k); p.drawLine(QPointF(3, 7), QPointF(9, 3)); p.drawLine(QPointF(15, 3), QPointF(21, 7));
         p.drawLine(QPointF(3, 17), QPointF(9, 21)); p.drawLine(QPointF(15, 21), QPointF(21, 17));
         p.setPen(a); p.drawLine(QPointF(12, 9), QPointF(12, 15)); p.drawLine(QPointF(9, 12), QPointF(15, 12)); }},
      {"split", [](QPainter& p, const QPen& k, const QPen& a, const QColor&) {
         p.setPen(k); p.drawLine(QPointF(3, 14), QPointF(10.5, 14)); p.drawLine(QPointF(13.5, 14), QPointF(21, 14));
         p.setPen(a); p.drawLine(QPointF(12, 4), QPointF(12, 20)); }},
      {"delete", [](QPainter& p, const QPen& k, const QPen&, const QColor&) {
         p.setPen(k); p.drawLine(QPointF(4, 7), QPointF(20, 7)); p.drawLine(QPointF(9, 7), QPointF(10, 4)); p.drawLine(QPointF(10, 4), QPointF(14, 4));
         p.drawLine(QPointF(14, 4), QPointF(15, 7)); p.drawPolyline(QPolygonF{{6, 7}, {7.5, 20}, {16.5, 20}, {18, 7}});
         p.drawLine(QPointF(10, 10.5), QPointF(10, 17)); p.drawLine(QPointF(14, 10.5), QPointF(14, 17)); }},
      {"ai", [](QPainter& p, const QPen&, const QPen&, const QColor& af) {
         auto star = [&](double cx, double cy, double r) {
           QPainterPath s; s.moveTo(cx, cy - r); s.quadTo(cx, cy, cx + r, cy); s.quadTo(cx, cy, cx, cy + r);
           s.quadTo(cx, cy, cx - r, cy); s.quadTo(cx, cy, cx, cy - r);
           p.setPen(Qt::NoPen); p.setBrush(af); p.drawPath(s); };
         star(10, 13, 8); star(18.5, 5.5, 3.5); }},
  };
  return m;
}
}  // namespace

QIcon CommandIcon(const QString& command) {
  QString key = command.toLower();
  while (key.startsWith('_') || key.startsWith('-') || key.startsWith('!')) key.remove(0, 1);
  const auto& m = Icons();
  auto it = m.find(key);
  if (it == m.end()) return {};
  const ThemePalette& pal = g_pal.window.isValid() ? g_pal : LightPalette();
  QIcon icon;
  for (int sz : {24, 48}) {
    QPixmap px(sz, sz);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(sz / 24.0, sz / 24.0);
    QPen ink(pal.text, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    QPen acc(pal.accent, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    it->second(p, ink, acc, pal.accent);
    p.end();
    icon.addPixmap(px);
  }
  return icon;
}

}  // namespace cad::ui
