// Collaborazione: differenze tra due stati del modello per oggetto (e per voce delle tabelle:
// livelli, materiali, tipi di linea, stili di quota, gruppi, definizioni di blocco) e testo
// utente del documento. Una "patch" contiene solo cio' che e' cambiato rispetto a un'istantanea
// e si applica su un altro modello: gli id restano quelli originali, gli indici di livello,
// materiale, tipo di linea e gruppo si rimappano tramite id.
#pragma once
#include "opennurbs.h"
#include <map>
#include <string>
#include <vector>

namespace cad::collab {

struct UuidLess {
  bool operator()(const ON_UUID& a, const ON_UUID& b) const { return ON_UuidCompare(a, b) < 0; }
};

struct TableKeyLess {
  bool operator()(const std::pair<int, ON_UUID>& a, const std::pair<int, ON_UUID>& b) const {
    return a.first != b.first ? a.first < b.first : ON_UuidCompare(a.second, b.second) < 0;
  }
};

struct Snapshot {
  std::map<ON_UUID, std::string, UuidLess> objects;                 // oggetti (geometria+attributi)
  std::map<std::pair<int, ON_UUID>, std::string, TableKeyLess> tables;            // (tipo, id) -> impronta
  std::map<std::wstring, std::wstring> userStrings;                  // testo utente del documento
};

Snapshot TakeSnapshot(const ONX_Model& m);

struct PatchInfo {
  int upserted = 0, deleted = 0, tables = 0, strings = 0;
  bool Empty() const { return !upserted && !deleted && !tables && !strings; }
};

// Patch (byte) con le modifiche di "m" rispetto a "base"; info: quante modifiche contiene.
// now (se non nullo): istantanea attuale, da usare come base per la patch successiva.
std::string MakePatch(const ONX_Model& m, const Snapshot& base, PatchInfo* info = nullptr, Snapshot* now = nullptr);
// Applica una patch. Restituisce false se i dati non sono validi.
bool ApplyPatch(ONX_Model& m, const std::string& patch, PatchInfo* info = nullptr, std::string* err = nullptr);

}  // namespace cad::collab
