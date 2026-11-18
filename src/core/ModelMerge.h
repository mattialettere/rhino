// Copia di oggetti da un modello .3dm a un altro con tutte le tabelle che usano:
// livelli (con i livelli padre, per percorso completo), tipi di linea, materiali (con il
// render content di Rhino 8 e le immagini incorporate delle texture), mappature delle
// texture, gruppi, stili di quota, motivi di retino e definizioni di blocco (anche annidate).
// Usata da Import (file -> documento) ed Export (selezione -> file nuovo), come Rhino 8:
// le voci con lo stesso nome gia' presenti nel modello di destinazione vengono riusate
// (per i blocchi: "usa quello esistente"), le altre vengono copiate.
#pragma once
#include "core/Document3dm.h"
#include <functional>
#include <vector>

namespace cad {

struct MergeOptions {
  // Oggetti di primo livello da copiare (nil = tutti quelli dello spazio modello).
  std::function<bool(const ON_UUID& id)> filter;
  // Aggiunge un oggetto al modello di destinazione e restituisce il suo id (l'Engine lo
  // usa per selezione e annullamento). Se vuoto: AddManagedModelGeometryComponent con id nuovo.
  std::function<ON_UUID(ON_Object* geometry, const ON_3dmObjectAttributes& attrs)> addObject;
};

struct MergeResult {
  std::vector<ON_UUID> objects;  // oggetti di primo livello aggiunti
  int layers = 0, linetypes = 0, materials = 0, blocks = 0, blocksReused = 0, groups = 0,
      dimStyles = 0, hatchPatterns = 0, embeddedFiles = 0, renderContents = 0;
  int skipped = 0;  // oggetti dello spazio pagina o non validi
};

MergeResult MergeModel(ONX_Model& dst, const ONX_Model& src, const MergeOptions& options = {});

}  // namespace cad
