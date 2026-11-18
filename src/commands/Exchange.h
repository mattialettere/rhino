// Scambio di file con altri programmi: STL, OBJ e DXF (scritti qui), STEP e IGES
// (OpenCASCADE). Import ed Export scelgono il formato dall'estensione, come Rhino 8.
#pragma once
#include "opennurbs.h"
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cad {

class Engine;

// Materiale scambiato con i formati mesh (OBJ/MTL, glTF, 3MF, PLY).
struct ExchangeMaterial {
  std::string name;
  ON_Color diffuse = ON_Color(204, 204, 204);
  double transparency = 0.0;           // 0 = opaco
  double shine = 0.0;                  // 0..1
  double ior = 1.0;
  ON_Color specular = ON_Color(255, 255, 255);
  ON_Color emission = ON_Color::Black;
  bool pbr = false;                    // materiale fisico (glTF): usa metallic e roughness
  double metallic = 0.0, roughness = 0.5;
  std::string texture;                 // immagine diffusa: percorso (assoluto)
  std::vector<unsigned char> textureData;  // byte dell'immagine se non e' su disco (es. .glb)
  std::string textureMime;             // "image/png", "image/jpeg"
};

struct ImportedObject {
  std::unique_ptr<ON_Geometry> geom;
  std::string layer;          // vuoto = livello corrente
  std::string name;
  ON_Color color = ON_UNSET_COLOR;  // colore del livello (DXF)
  std::shared_ptr<ExchangeMaterial> material;  // materiale da creare e assegnare (o nullptr)
};

struct ExportItem {
  const ON_Geometry* geom = nullptr;
  std::string name, layer;
  ON_Color color = ON_Color::Black;
  std::shared_ptr<ExchangeMaterial> material;  // materiale di rendering dell'oggetto (o nullptr)
  ON_TextureMapping mapping;                   // mappatura della texture (se hasMapping)
  bool hasMapping = false;
};

// Mesh per l'esportazione con le coordinate di texture della mappatura dell'oggetto.
std::unique_ptr<ON_Mesh> ExportMeshFor(const ExportItem& item, double tolerance);

// Estensione in minuscolo senza il punto ("stl", "obj", ...).
std::string FileExtension(const std::string& path);
bool IsExchangeFormat(const std::string& ext);   // stl, obj, dxf, step/stp, iges/igs
std::string ExchangeFormatsDescription();

// Mesh per l'esportazione (mesh, SubD, superfici e solidi). nullptr se non si puo'.
std::unique_ptr<ON_Mesh> ExportMesh(const ON_Geometry& g, double tolerance);

bool ReadSTL(const std::string& path, std::vector<ImportedObject>& out, std::string& err);
bool WriteSTL(const std::string& path, const std::vector<ExportItem>& items, double tolerance, bool binary,
              std::string& err);
bool ReadOBJ(const std::string& path, std::vector<ImportedObject>& out, std::string& err);
bool WriteOBJ(const std::string& path, const std::vector<ExportItem>& items, double tolerance, std::string& err);
// unitsScale: fattore per portare le unita' del file in quelle del documento
bool ReadDXF(const std::string& path, ON::LengthUnitSystem docUnits, std::vector<ImportedObject>& out, std::string& err);
bool WriteDXF(const std::string& path, const std::vector<ExportItem>& items, ON::LengthUnitSystem docUnits,
              double tolerance, std::string& err);
// STEP e IGES (solo con OpenCASCADE)
bool ReadStepIges(const std::string& path, ON::LengthUnitSystem docUnits, double tolerance,
                  std::vector<ImportedObject>& out, std::string& err);
bool WriteStepIges(const std::string& path, const std::vector<ExportItem>& items, ON::LengthUnitSystem docUnits,
                   double tolerance, std::string& err);

// OBJ con materiali: WriteOBJ scrive anche il .mtl (e copia le immagini delle texture).
// glTF 2.0 (.gltf con dati incorporati, .glb binario): Y in alto e metri, come Rhino 8.
bool ReadGLTF(const std::string& path, ON::LengthUnitSystem docUnits, std::vector<ImportedObject>& out, std::string& err);
bool WriteGLTF(const std::string& path, const std::vector<ExportItem>& items, ON::LengthUnitSystem docUnits,
               double tolerance, bool binary, std::string& err);
// 3MF (stampa 3D): mesh con colori dei materiali, unita' del documento.
bool Read3MF(const std::string& path, ON::LengthUnitSystem docUnits, std::vector<ImportedObject>& out, std::string& err);
bool Write3MF(const std::string& path, const std::vector<ExportItem>& items, ON::LengthUnitSystem docUnits,
              double tolerance, std::string& err);
// PLY: mesh (o nuvola di punti) con colori per vertice, ASCII o binario.
bool ReadPLY(const std::string& path, std::vector<ImportedObject>& out, std::string& err);
bool WritePLY(const std::string& path, const std::vector<ExportItem>& items, double tolerance, bool binary,
              std::string& err);

// Usati dai comandi Import/Export/Open per i formati di scambio.
bool ImportExchange(Engine& e, const std::string& path, const std::map<std::string, std::string>& opts);
bool ExportExchange(Engine& e, const std::vector<ON_UUID>& ids, const std::string& path,
                    const std::map<std::string, std::string>& opts);

}  // namespace cad
