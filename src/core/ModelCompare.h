#pragma once
// Confronto strutturale tra due modelli: usato per verificare che
// lettura -> scrittura -> rilettura non perda o alteri dati.
#include "core/Document3dm.h"
#include <map>
#include <string>
#include <vector>

namespace cad {

struct ObjectSnapshot {
  std::string cls, name;
  ON_BoundingBox bbox;
  std::string userData;
  std::string hash;  // SHA-1 della serializzazione Rhino 8 della geometria
  // Riferimenti alle tabelle confrontati per id: gli indici possono essere
  // rinumerati legittimamente in scrittura.
  std::string layer, linetype, material;
  int colorSource = 0, mode = 0, materialSource = 0;
  unsigned int color = 0;
  bool visible = true;
  std::string userText;  // testo utente "chiave=valore;" ordinato per chiave
  std::string groups;    // gruppi dell'oggetto (per nome o id)
  std::string print;     // colore e spessore di stampa con le loro sorgenti
  std::string space;     // spazio modello o pagina (con id della tavola)
};

// Istantanea confrontabile di un modello (oggetti per id, tabelle per nome).
struct ModelSnapshot {
  std::map<std::string, ObjectSnapshot> objects;
  std::map<std::string, std::map<std::string, std::string>> tables;
  int namedViews = 0, unitSystem = 0;
  double absTolerance = 0.0;
};

struct CompareResult {
  std::vector<std::string> differences;  // vuoto = modelli equivalenti
  int objectsCompared = 0;
  bool Equal() const { return differences.empty(); }
};

// Confronta oggetti (per id: classe, bounding box, CRC dei dati geometrici,
// attributi principali, testo utente, gruppi, stampa, spazio) e tabelle (livelli,
// materiali anche PBR, blocchi, tipi di linea, gruppi, stili di quota, motivi di
// retino, mappature) piu' i dati del documento: testo utente, render content di
// Rhino 8 (materiali, ambienti, texture), file incorporati, record di cronologia,
// viste e CPlane con nome, tavole, sole/cielo/piano di terra.
ModelSnapshot TakeSnapshot(const ONX_Model& m);
CompareResult CompareSnapshots(const ModelSnapshot& a, const ModelSnapshot& b,
                               double bboxTolerance = 1e-9);
CompareResult CompareModels(const ONX_Model& a, const ONX_Model& b,
                            double bboxTolerance = 1e-9);

}  // namespace cad
