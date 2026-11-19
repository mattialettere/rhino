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
  // colore del materiale di rendering (modalita' Renderizzato)
  ON_Color material = ON_Color(230, 230, 230);
  bool hasMaterial = false;
  bool selected = false, locked = false;
  bool unclipped = false;              // i piani di sezione non tagliano se stessi
  ON_BoundingBox bbox;
};

// Piano di sezione (ClippingPlane) e viste in cui e' attivo
struct ClipPlane {
  ON_PlaneEquation eq;
  std::vector<ON_UUID> views;
};

class Scene {
public:
  explicit Scene(Engine& e) : m_engine(e) {}
  // Spazio mostrato: se fixedSpace, sempre "space" (nil = modello, altrimenti una tavola);
  // altrimenti lo spazio attivo del motore.
  bool fixedSpace = false;
  ON_UUID space = ON_nil_uuid;
  // Cartella dove estrarre le immagini incorporate delle texture
  static std::string textureCacheDir;
  // Ricostruisce (con cache delle mesh) dopo ogni comando.
  void Rebuild();
  void RefreshSelection();
  const std::vector<DisplayObject>& Objects() const { return m_objects; }
  const std::vector<ClipPlane>& Clips() const { return m_clips; }
  Engine& GetEngine() const { return m_engine; }
  int Version() const { return m_version; }   // cresce a ogni ricostruzione
  ON_BoundingBox Bounds() const;
  // Anteprima dell'editor a nodi: geometria non nel documento, disegnata con un colore proprio
  void SetPreview(const std::vector<std::shared_ptr<const ON_Geometry>>& geometry);
  const std::vector<DisplayObject>& Preview() const { return m_preview; }
  const ON_Geometry* Geometry(const ON_UUID& id) const {
    const auto* o = m_engine.Object(id);
    return o ? o->Geometry(nullptr) : nullptr;
  }

private:
  void AddGeometry(const ON_Geometry* g, DisplayObject& d, const std::string& sig,
                   std::map<std::string, std::shared_ptr<ON_Mesh>>& newCache, int depth);
  static void AddShaded(DisplayObject& d, const std::shared_ptr<ON_Mesh>& m);
  void ApplyTexture(const ON_3dmObjectAttributes& a, DisplayObject& d);
  void AddWorksession(std::map<std::string, std::shared_ptr<ON_Mesh>>& newCache);
  Engine& m_engine;
  // Modello da cui leggere blocchi, stili e retini: il documento o un file della Worksession
  const ONX_Model* m_sourceModel = nullptr;
  const ONX_Model& Source() const { return m_sourceModel ? *m_sourceModel : m_engine.Model(); }
  std::vector<DisplayObject> m_objects;
  std::vector<DisplayObject> m_preview;
  std::map<std::string, std::shared_ptr<ON_Mesh>> m_previewCache;
  std::vector<ClipPlane> m_clips;
  double m_lightScale = 100;
  int m_version = 0;
  std::map<std::string, std::shared_ptr<ON_Mesh>> m_meshCache;  // chiave: id + firma
  using EdgeList = std::vector<std::vector<ON_3dPoint>>;
  std::map<std::string, std::shared_ptr<EdgeList>> m_subdEdges, m_newSubdEdges;  // spigoli limite dei SubD
};

void TessellateCurve(const ON_Curve& c, std::vector<ON_3dPoint>& out);

}  // namespace cad::ui
