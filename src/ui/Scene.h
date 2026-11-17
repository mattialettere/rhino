#pragma once
// Scena di visualizzazione: per ogni oggetto del modello le polilinee del
// wireframe, i punti e (se disponibile) la mesh ombreggiata.
#include "commands/Engine.h"
#include <map>
#include <memory>
#include <vector>

namespace cad::ui {

struct DisplayObject {
  ON_UUID id = ON_nil_uuid;
  std::vector<std::vector<ON_3dPoint>> wires;
  std::vector<ON_3dPoint> points;
  std::vector<ON_3dPoint> snapPoints;  // estremi, punti medi, centri, vertici
  std::shared_ptr<ON_Mesh> shaded;     // nullptr se non ombreggiabile
  ON_Color color = ON_Color::Black;
  bool selected = false, locked = false;
  ON_BoundingBox bbox;
};

class Scene {
public:
  explicit Scene(Engine& e) : m_engine(e) {}
  // Ricostruisce (con cache delle mesh) dopo ogni comando.
  void Rebuild();
  void RefreshSelection();
  const std::vector<DisplayObject>& Objects() const { return m_objects; }
  ON_BoundingBox Bounds() const;

private:
  Engine& m_engine;
  std::vector<DisplayObject> m_objects;
  std::map<std::string, std::shared_ptr<ON_Mesh>> m_meshCache;  // chiave: id + firma
};

void TessellateCurve(const ON_Curve& c, std::vector<ON_3dPoint>& out);

}  // namespace cad::ui
