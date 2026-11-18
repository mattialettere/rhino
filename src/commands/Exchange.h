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

struct ImportedObject {
  std::unique_ptr<ON_Geometry> geom;
  std::string layer;          // vuoto = livello corrente
  std::string name;
  ON_Color color = ON_UNSET_COLOR;  // colore del livello (DXF)
};

struct ExportItem {
  const ON_Geometry* geom = nullptr;
  std::string name, layer;
  ON_Color color = ON_Color::Black;
};

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

// Usati dai comandi Import/Export/Open per i formati di scambio.
bool ImportExchange(Engine& e, const std::string& path, const std::map<std::string, std::string>& opts);
bool ExportExchange(Engine& e, const std::vector<ON_UUID>& ids, const std::string& path,
                    const std::map<std::string, std::string>& opts);

}  // namespace cad
