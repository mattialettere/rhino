// Layout (tavole), dettagli e stampa: composizione di una pagina in millimetri
// (origine in basso a sinistra, come le tavole di Rhino) e scrittura in PDF.
// Usato dai comandi Layout, Detail, Print e dall'esportazione in PDF; la finestra
// disegna la stessa composizione nelle schede delle tavole e sulla stampante.
#pragma once
#include "opennurbs.h"
#include <string>
#include <vector>

namespace cad {

class Engine;
namespace ui { class Scene; }

struct PagePolyline {
  std::vector<ON_2dPoint> pts;
  ON_Color color = ON_Color::Black;
  double widthMm = 0.25;
};
struct PageFill {  // poligono pieno (frecce delle quote)
  std::vector<ON_2dPoint> pts;
  ON_Color color = ON_Color::Black;
};
struct PageText {
  ON_2dPoint pos;            // linea di base
  double heightMm = 3.5;     // altezza delle maiuscole (come Rhino)
  double angle = 0;          // radianti, antiorario
  int halign = 0;            // 0 sinistra, 1 centro, 2 destra
  std::string text;          // UTF-8, una riga
  ON_Color color = ON_Color::Black;
};
struct PageImage {  // immagine RGB (prima riga = in alto)
  double x = 0, y = 0, w = 0, h = 0;  // rettangolo sulla pagina (mm)
  int pw = 0, ph = 0;
  std::vector<unsigned char> rgb;
};

struct PageDrawing {
  std::string name;
  double widthMm = 297, heightMm = 210;
  std::vector<PageImage> images;   // disegnate per prime
  std::vector<PageFill> fills;
  std::vector<PagePolyline> lines;
  std::vector<PageText> texts;
};

struct PrintOptions {
  double dpi = 150;         // risoluzione delle viste ombreggiate
  bool blackWhite = false;  // tutto in nero (come "Bianco e nero" di Rhino)
  double defaultWidthMm = 0.25;
};

// --- tavole nel modello: ON_3dmView di tipo pagina in m_settings.m_views ---
std::vector<int> LayoutIndices(const ONX_Model& m);
int FindLayout(const ONX_Model& m, const std::string& name);   // indice in m_views o -1
int FindLayout(const ONX_Model& m, const ON_UUID& id);
// Dimensioni standard in mm (A0-A4, Letter, Tabloid); false se il nome non e' noto.
bool PaperSize(const std::string& name, double& w, double& h);

// Display mode di una vista: true se ombreggiata (Shaded, Rendered, Ghosted, Raggi X...).
bool IsShadedView(const ON_3dmView& v);
// Adatta la vista a un riquadro (rapporto di forma) e a una scala (rapporto pagina/modello,
// 0 = nessuna scala); se zoomBox e' valido inquadra quel volume.
void FitView(ON_3dmView& v, double wMm, double hMm, double pagePerModel, ON::LengthUnitSystem modelUnits,
             const ON_BoundingBox& zoomBox);
// Vista standard (Top, Bottom, Front, Back, Right, Left, Perspective) che inquadra il volume.
ON_3dmView StandardView(const std::string& projection, const ON_BoundingBox& box, double aspect);

// Disegna il modello visto da "view" nel rettangolo (x0,y0)-(x1,y1) della pagina.
void DrawModelInRect(Engine& e, const ui::Scene& scene, const ON_3dmView& view, double x0, double y0, double x1,
                     double y1, double pagePerModel, const PrintOptions& o, PageDrawing& out);
// Tavola completa: oggetti di pagina e dettagli.
// modelScene: scena del modello gia' pronta (la finestra la riusa), altrimenti se ne crea una.
bool ComposeLayout(Engine& e, int layoutIndex, const PrintOptions& o, PageDrawing& out,
                   const ui::Scene* modelScene = nullptr);
// Vista del modello stampata su un foglio (margini 10 mm), adattata alla vista.
bool ComposeView(Engine& e, const ON_3dmView& view, double wMm, double hMm, const PrintOptions& o, PageDrawing& out);

// PDF vettoriale (linee e testo) con le viste ombreggiate come immagini.
bool WritePdf(const std::string& path, const std::vector<PageDrawing>& pages, std::string& err);

}  // namespace cad
