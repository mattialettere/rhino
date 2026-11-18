#pragma once
// Scena di visualizzazione: per ogni oggetto del modello le polilinee del
// wireframe, i punti e (se disponibile) la mesh ombreggiata.
#include "commands/Annotation.h"
#include "commands/Engine.h"
#include "commands/Grips.h"
#include <map>
#include <string>
#include <memory>
#include <vector>

namespace cad::ui {

// Tipi di snap ad oggetto (come la barra Osnap di Rhino 8)
enum class SnapKind { End, Mid, Center, Quad, Knot, Point, Vertex };
struct SnapPoint {
  ON_3dPoint p;
  SnapKind kind;
};
const char* SnapName(SnapKind k);

struct DisplayObject {
  ON_UUID id = ON_nil_uuid;
  std::vector<std::vector<ON_3dPoint>> wires;
  std::vector<std::vector<ON_3dPoint>> isocurves;  // isocurve delle superfici (solo disegno e selezione)
  std::vector<ON_3dPoint> points;
  std::vector<AnnotationLabel> labels;   // testi di testi e quote
  std::vector<ON_3dPoint> triangles;     // frecce piene delle quote
  // Spigoli della mesh di visualizzazione con le normali delle due facce:
  // nelle viste diventano il contorno apparente (silhouette) delle superfici curve.
  struct SilEdge { ON_3dPoint a, b; ON_3dVector n1, n2; };
  std::vector<SilEdge> silhouette;
  std::vector<SnapPoint> snapPoints;   // estremi, punti medi, centri, quadranti, nodi, vertici
  std::shared_ptr<ON_Mesh> shaded;     // nullptr se non ombreggiabile
  // Punti di controllo (PointsOn) e poligono/rete di controllo
  std::vector<Grip> grips;
  std::vector<std::vector<ON_3dPoint>> cage;
  // Texture del materiale: immagine leggibile e coordinate per ogni vertice di "shaded"
  std::string texture;
  std::vector<ON_2fPoint> uv;
  ON_Color diffuse = ON_Color(255, 255, 255);
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
  const ON_Geometry* Geometry(const ON_UUID& id) const {
    const auto* o = m_engine.Object(id);
    return o ? o->Geometry(nullptr) : nullptr;
  }

private:
  void AddGeometry(const ON_Geometry* g, DisplayObject& d, const std::string& sig,
                   std::map<std::string, std::shared_ptr<ON_Mesh>>& newCache, int depth);
  static void AddShaded(DisplayObject& d, const std::shared_ptr<ON_Mesh>& m);
  void ApplyTexture(const ON_3dmObjectAttributes& a, DisplayObject& d);
  Engine& m_engine;
  std::vector<DisplayObject> m_objects;
  std::map<std::string, std::shared_ptr<ON_Mesh>> m_meshCache;  // chiave: id + firma
  using EdgeList = std::vector<std::vector<ON_3dPoint>>;
  std::map<std::string, std::shared_ptr<EdgeList>> m_subdEdges, m_newSubdEdges;  // spigoli limite dei SubD
};

void TessellateCurve(const ON_Curve& c, std::vector<ON_3dPoint>& out);

}  // namespace cad::ui
