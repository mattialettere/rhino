// Aspetto di Kurva3D: tema chiaro o scuro (automatico = come Windows), foglio di
// stile moderno e icone disegnate dal programma (originali, non quelle di Rhino).
#pragma once
#include <QColor>
#include <QFont>
#include <QIcon>
#include <QString>

class QApplication;

namespace cad::ui {

enum class ThemeMode { Auto, Light, Dark };

struct ThemePalette {
  bool dark = false;
  QColor window, panel, base, alt, text, muted, border, hover, pressed, accent, accentText;
};

// Modo salvato nelle impostazioni (predefinito: automatico).
ThemeMode SavedThemeMode();
void SaveThemeMode(ThemeMode m);
// Il sistema usa il tema scuro?
bool SystemPrefersDark();
// Applica il tema (tavolozza, foglio di stile, colori delle viste).
void ApplyTheme(ThemeMode m);
const ThemePalette& CurrentPalette();

// Carattere a spaziatura fissa del sistema (Consolas su Windows, Menlo su macOS).
QFont MonoFont(int pointSize);

// Icona di un comando (es. "_Line") nei colori del tema attuale; vuota se non c'e'.
QIcon CommandIcon(const QString& command);

}  // namespace cad::ui
