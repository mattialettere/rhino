#pragma once
// Geometria di visualizzazione delle annotazioni (testi, quote, direttrici):
// linee e frecce in coordinate del mondo, piu' le etichette di testo.
// Calcolata da noi a partire dai punti di definizione di openNURBS, perche'
// fuori da Windows openNURBS non misura i caratteri (riquadri di testo vuoti).
#include "opennurbs.h"
#include <string>
#include <vector>

namespace cad {

struct AnnotationLabel {
  ON_3dPoint origin;     // linea di base della prima riga (punto di allineamento)
  ON_3dVector xdir, ydir;  // versori del testo nel mondo
  double height = 1.0;     // altezza del testo nel mondo
  int halign = 0;          // 0 sinistra, 1 centro, 2 destra
  std::string text;        // UTF-8, righe separate da '\n'
  bool dot = false;        // punto di testo: dimensione fissa sullo schermo, su fondo colorato
  std::string font;        // famiglia del carattere (UTF-8); vuota = carattere predefinito
  bool bold = false, italic = false;
};

struct AnnotationDisplay {
  std::vector<std::vector<ON_3dPoint>> wires;
  std::vector<AnnotationLabel> labels;
  std::vector<ON_3dPoint> triangles;  // frecce piene: 3 punti per triangolo
};

// Stile di quota "genitore" dell'annotazione (quello del documento con il suo id,
// altrimenti lo stile predefinito).
const ON_DimStyle& ParentDimStyle(const ONX_Model& model, const ON_Annotation& a);

bool BuildAnnotationDisplay(const ONX_Model& model, const ON_Annotation& a, AnnotationDisplay& out);

}  // namespace cad
