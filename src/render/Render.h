// Rendering: scena di triangoli con materiali e luci presa dal modello, path tracer
// progressivo su CPU (modalita' Raytraced e comando Render) e immagini PNG.
// Luci, sole, cielo e piano di terra sono quelli del .3dm (ON_Light, impostazioni di
// rendering di Rhino 8); i materiali sono gli ON_Material (semplici o PBR) degli oggetti.
#pragma once
#include "opennurbs.h"
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cad {

class Engine;
namespace ui { class Scene; }

struct RenderTexture {
  int w = 0, h = 0;
  std::vector<float> rgb;  // lineare
  ON_3fVector Sample(float u, float v) const;
};

struct RenderMaterial {
  ON_3fVector albedo{0.8f, 0.8f, 0.8f};  // lineare
  float roughness = 1.0f;                 // 0 specchio, 1 opaco
  float metallic = 0.0f;
  float specular = 0.04f;                 // riflessione speculare a incidenza normale (dielettrici)
  float transmission = 0.0f;              // trasparenza (vetro)
  float ior = 1.5f;
  ON_3fVector emission{0, 0, 0};
  int texture = -1;
};

struct RenderLight {
  enum class Kind { Point, Directional, Spot, Rect } kind = Kind::Point;
  ON_3dPoint pos;
  ON_3dVector dir{0, 0, -1};  // direzione della luce (verso cui va)
  ON_3dVector u, v;           // lati della luce rettangolare
  ON_3fVector color{1, 1, 1};  // gia' moltiplicato per l'intensita'
  double cosOuter = 0, cosInner = 0;
  double radius = 0;          // morbidezza delle ombre (luci puntiformi)
};

struct RenderOptions {
  int width = 800, height = 600;
  int samples = 64;           // passate per pixel
  int maxBounces = 6;
  int threads = 0;            // 0 = tutti i core
  bool ground = false;        // piano di terra
  double groundZ = 0;
  bool groundShadowOnly = false;
  bool skylight = true;
  float skyIntensity = 0.8f;
  ON_3fVector skyTop{0.65f, 0.75f, 0.9f}, skyBottom{0.35f, 0.33f, 0.3f};
  ON_3fVector background{1, 1, 1};  // colore dello sfondo visto direttamente
  bool backgroundIsSky = false;
  float exposure = 1.0f;
};

struct Ray { ON_3dPoint o; ON_3dVector d; };

class RenderScene {
public:
  // Triangoli e materiali dalla scena di visualizzazione (mesh gia' pronte), luci dal modello.
  void Build(Engine& e, const ui::Scene& scene, RenderOptions& opt);
  size_t TriangleCount() const { return m_tris.size(); }
  const std::vector<RenderLight>& Lights() const { return m_lights; }
  // Intersezione: distanza, triangolo, coordinate baricentriche
  bool Intersect(const Ray& r, double tmax, double& t, int& tri, double& bu, double& bv) const;
  bool Occluded(const Ray& r, double tmax) const;

  struct Tri {
    ON_3fPoint p[3];
    ON_3fVector n[3];
    ON_2fPoint uv[3];
    int mat = 0;
    bool hasUv = false;
  };
  std::vector<Tri> m_tris;
  std::vector<RenderMaterial> m_mats;
  std::vector<RenderTexture> m_textures;
  std::vector<RenderLight> m_lights;
  ON_BoundingBox m_box;

  // funzione per leggere le immagini delle texture (dalla finestra: Qt)
  static std::function<bool(const std::string& path, int& w, int& h, std::vector<unsigned char>& rgb)> imageLoader;

private:
  struct Node { float bmin[3], bmax[3]; int left = -1, right = -1, first = 0, count = 0; };
  std::vector<Node> m_nodes;
  std::vector<int> m_index;
  void BuildBvh();
  int BuildNode(int first, int count, std::vector<ON_BoundingBox>& boxes, std::vector<ON_3dPoint>& centers, int depth);
};

// Camera di rendering dalla vista (prospettica o parallela)
struct RenderCamera {
  ON_3dPoint loc;
  ON_3dVector X, Y, Z;  // Z verso l'osservatore
  double l = -1, r = 1, b = -1, t = 1, n = 1;
  bool persp = true;
  bool Init(const ON_3dmView& v, double aspect);
  Ray Generate(double u, double v) const;  // u,v in [0,1], v=0 in basso
};

class PathTracer {
public:
  PathTracer(const RenderScene& s, const RenderCamera& c, const RenderOptions& o);
  // Una passata (un campione per pixel). Ritorna false se interrotta.
  bool Pass(const std::atomic<bool>* cancel = nullptr);
  int Passes() const { return m_passes; }
  // Immagine sRGB a 8 bit (prima riga = in alto)
  void Image(std::vector<unsigned char>& rgb) const;
  int Width() const { return m_opt.width; }
  int Height() const { return m_opt.height; }

private:
  ON_3fVector Trace(Ray r, unsigned& seed) const;
  ON_3fVector Sky(const ON_3dVector& d) const;
  const RenderScene& m_scene;
  RenderCamera m_cam;
  RenderOptions m_opt;
  std::vector<float> m_accum;
  int m_passes = 0;
};

bool WritePng(const std::string& path, int w, int h, const std::vector<unsigned char>& rgb, std::string& err);

// Comando Render e luci (registrati nel motore)
void RegisterRenderCommands(Engine& e);

}  // namespace cad
