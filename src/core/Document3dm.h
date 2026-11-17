#pragma once
// Documento .3dm: caricamento, salvataggio e riepilogo per categoria.
// Tutto l'accesso al formato passa da openNURBS (nessun codice proprietario).
#include "opennurbs.h"
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cad {

// Versione di archivio scritta di default: Rhino 8.
constexpr int kRhino8ArchiveVersion = 8;

// Categorie della matrice di compatibilita' (docs/COMPATIBILITA.md).
enum class Category {
  Point, PointCloud, NurbsCurve, OtherCurve, NurbsSurface, OtherSurface,
  Brep, Extrusion, SubD, Mesh, BlockInstance, Annotation, Hatch, Light,
  Other,
  // Tabelle
  BlockDefinition, Layer, Material, Linetype, Group, DimStyle, HatchPattern,
  TextureMapping, Image, NamedView
};
const char* CategoryName(Category c);
Category ClassifyGeometry(const ON_Geometry* g);

struct ModelSummary {
  int archiveVersion = 0;          // valore grezzo openNURBS (es. 80 per Rhino 8)
  int rhinoVersion = 0;            // versione Rhino del file (es. 8)
  std::string createdBy;           // applicazione che ha creato il file
  std::string lastEditedBy;
  std::map<Category, int> counts;
  std::map<std::string, int> geometryClasses;  // nome classe openNURBS -> numero
};

class Document3dm {
public:
  Document3dm();
  bool Load(const std::string& path, std::string* log = nullptr);
  bool Save(const std::string& path, int version = kRhino8ArchiveVersion,
            std::string* log = nullptr) const;
  ModelSummary Summarize() const;
  const ONX_Model& Model() const { return *m_model; }
  ONX_Model& Model() { return *m_model; }
  // Sostituisce il modello (usato da Nuovo, Apri e Annulla).
  void Replace(std::unique_ptr<ONX_Model> model) { m_model = std::move(model); }
  // Nuovo modello vuoto: millimetri, tolleranza 0.001, livello "Default".
  void NewEmpty();
  // Serializza / ripristina tutto il modello in memoria (per Annulla/Ripeti).
  std::vector<unsigned char> ToBuffer() const;
  bool FromBuffer(const std::vector<unsigned char>& buffer);

private:
  std::unique_ptr<ONX_Model> m_model;
};

// Da chiamare una volta all'avvio/chiusura del programma.
void InitLibrary();
void ShutdownLibrary();

std::string ToUtf8(const ON_wString& s);

}  // namespace cad
