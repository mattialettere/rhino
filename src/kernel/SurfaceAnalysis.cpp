#include "kernel/SurfaceAnalysis.h"
#include "kernel/OcctConvert.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepLProp_SLProps.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>

namespace cad {
namespace {

constexpr double kHugeRadius = 1e6;

bool BrepCurvature(const ON_Geometry& g, double tol, CurvatureData& out) {
  TopoDS_Shape shape = occt::GeometryToShape(g, tol);
  if (shape.IsNull()) return false;
  ON_BoundingBox bb;
  g.GetTightBoundingBox(bb);
  const double lin = std::max(bb.Diagonal().Length() / 250.0, std::max(tol, 1e-4));
  BRepMesh_IncrementalMesh mesher(shape, lin, Standard_False, 0.2, Standard_True);
  ON_Mesh& m = out.mesh;
  m.Destroy();
  out.k1.clear();
  out.k2.clear();
  for (TopExp_Explorer fx(shape, TopAbs_FACE); fx.More(); fx.Next()) {
    const TopoDS_Face& face = TopoDS::Face(fx.Current());
    TopLoc_Location loc;
    Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
    if (tri.IsNull()) continue;
    const gp_Trsf tr = loc.Transformation();
    const int base = m.m_V.Count();
    BRepAdaptor_Surface as(face, Standard_False);
    // la normale della faccia: invertita se la faccia e' rovesciata; le curvature con segno
    // seguono la stessa convenzione (positive dove la superficie si chiude verso la normale opposta)
    const double sgn = face.Orientation() == TopAbs_REVERSED ? -1.0 : 1.0;
    double u0, u1, v0, v1;
    BRepTools::UVBounds(face, u0, u1, v0, v1);
    const double uc = 0.5 * (u0 + u1), vc = 0.5 * (v0 + v1);
    for (int i = 1; i <= tri->NbNodes(); i++) {
      const gp_Pnt p = tri->Node(i).Transformed(tr);
      m.m_V.Append(ON_3fPoint(float(p.X()), float(p.Y()), float(p.Z())));
      double k1 = 0, k2 = 0;
      gp_Vec n(0, 0, 0);
      if (tri->HasUVNodes()) {
        const gp_Pnt2d uv = tri->UVNode(i);
        // nei punti singolari (poli) la curvatura non e' definita: si valuta poco piu' all'interno
        bool done = false;
        for (int attempt = 0; attempt < 4 && !done; attempt++) {
          const double w = attempt == 0 ? 0.0 : std::pow(10.0, attempt - 5);
          const double u = uv.X() + w * (uc - uv.X()), v = uv.Y() + w * (vc - uv.Y());
          try {
            BRepLProp_SLProps pr(as, u, v, 2, 1e-9);
            if (pr.IsNormalDefined() && attempt == 0) n = gp_Vec(pr.Normal()) * sgn;
            if (pr.IsCurvatureDefined()) {
              // Rhino: curvatura positiva per una sfera con le normali verso l'esterno
              k1 = -sgn * pr.MaxCurvature();
              k2 = -sgn * pr.MinCurvature();
              if (k1 < k2) std::swap(k1, k2);
              if (attempt > 0 && n.Magnitude() < 0.5 && pr.IsNormalDefined()) n = gp_Vec(pr.Normal()) * sgn;
              done = true;
            }
          } catch (...) {}
        }
      }
      if (!loc.IsIdentity()) n.Transform(tr);
      if (n.Magnitude() > 1e-15) n.Normalize();
      m.m_N.Append(ON_3fVector(float(n.X()), float(n.Y()), float(n.Z())));
      out.k1.push_back(k1);
      out.k2.push_back(k2);
    }
    const bool rev = face.Orientation() == TopAbs_REVERSED;
    for (int i = 1; i <= tri->NbTriangles(); i++) {
      int a, b, c;
      tri->Triangle(i).Get(a, b, c);
      if (rev) std::swap(b, c);
      m.SetTriangle(m.m_F.Count(), base + a - 1, base + b - 1, base + c - 1);
    }
  }
  bool bad = false;
  for (int i = 0; i < m.m_N.Count(); i++) if (m.m_N[i].LengthSquared() < 0.5f) bad = true;
  if (bad) {
    ON_3fVectorArray keep = m.m_N;
    m.m_N.Destroy();
    m.ComputeVertexNormals();
    if (keep.Count() == m.m_N.Count())
      for (int i = 0; i < keep.Count(); i++) if (keep[i].LengthSquared() >= 0.5f) m.m_N[i] = keep[i];
  }
  out.exact = true;
  return m.m_F.Count() > 0;
}

// Stima per le mesh: curvatura normale lungo ogni spigolo, kn = 2 n.(pj - pi) / |pj - pi|^2;
// massimo e minimo sui vicini come curvature principali.
bool MeshCurvature(const ON_Mesh& src, CurvatureData& out) {
  out.mesh = src;
  ON_Mesh& m = out.mesh;
  if (!m.HasVertexNormals()) m.ComputeVertexNormals();
  const int nv = m.m_V.Count();
  std::vector<double> hi(nv, -ON_DBL_MAX), lo(nv, ON_DBL_MAX);
  // i vertici coincidenti (mesh non saldate) condividono i vicini
  auto edge = [&](int i, int j) {
    const ON_3dPoint pi(m.m_V[i]), pj(m.m_V[j]);
    const ON_3dVector d = pj - pi;
    const double l2 = d.LengthSquared();
    if (l2 < 1e-24) return;
    const double ki = -2.0 * (ON_3dVector(m.m_N[i]) * d) / l2;
    const double kj = 2.0 * (ON_3dVector(m.m_N[j]) * d) / l2;
    hi[i] = std::max(hi[i], ki); lo[i] = std::min(lo[i], ki);
    hi[j] = std::max(hi[j], kj); lo[j] = std::min(lo[j], kj);
  };
  for (int f = 0; f < m.m_F.Count(); f++) {
    const ON_MeshFace& F = m.m_F[f];
    const int n = F.IsQuad() ? 4 : 3;
    for (int k = 0; k < n; k++) edge(F.vi[k], F.vi[(k + 1) % n]);
  }
  out.k1.assign(nv, 0);
  out.k2.assign(nv, 0);
  for (int i = 0; i < nv; i++)
    if (hi[i] >= lo[i]) { out.k1[i] = hi[i]; out.k2[i] = lo[i]; }
  out.exact = false;
  return m.m_F.Count() > 0;
}

}  // namespace

bool ComputeCurvature(const ON_Geometry& g, double tol, CurvatureData& out, const ON_Mesh* displayMesh) {
  if (const auto* m = ON_Mesh::Cast(&g)) return MeshCurvature(*m, out);
  if (ON_Brep::Cast(&g) || ON_Extrusion::Cast(&g) || ON_Surface::Cast(&g)) {
    try {
      if (BrepCurvature(g, tol, out)) return true;
    } catch (...) {}
  }
  if (displayMesh) return MeshCurvature(*displayMesh, out);
  return false;
}

double CurvatureValue(CurvatureStyle s, double k1, double k2) {
  switch (s) {
    case CurvatureStyle::Gaussian: return k1 * k2;
    case CurvatureStyle::Mean: return 0.5 * (k1 + k2);
    case CurvatureStyle::MinRadius: {
      const double k = std::max(std::fabs(k1), std::fabs(k2));
      return k > 1.0 / kHugeRadius ? 1.0 / k : kHugeRadius;
    }
    case CurvatureStyle::MaxRadius: {
      const double k = std::min(std::fabs(k1), std::fabs(k2));
      return k > 1.0 / kHugeRadius ? 1.0 / k : kHugeRadius;
    }
  }
  return 0;
}

CurvatureStyle ParseCurvatureStyle(const std::string& name, CurvatureStyle def) {
  std::string n;
  for (char c : name) if (std::isalnum(static_cast<unsigned char>(c))) n += char(std::tolower(static_cast<unsigned char>(c)));
  if (n == "gaussian" || n == "gaussiana") return CurvatureStyle::Gaussian;
  if (n == "mean" || n == "media") return CurvatureStyle::Mean;
  if (n == "minradius" || n == "raggiominimo") return CurvatureStyle::MinRadius;
  if (n == "maxradius" || n == "raggiomassimo") return CurvatureStyle::MaxRadius;
  return def;
}

const char* CurvatureStyleName(CurvatureStyle s) {
  switch (s) {
    case CurvatureStyle::Gaussian: return "Gaussian";
    case CurvatureStyle::Mean: return "Mean";
    case CurvatureStyle::MinRadius: return "MinRadius";
    case CurvatureStyle::MaxRadius: return "MaxRadius";
  }
  return "Gaussian";
}

const char* CurvatureStyleLabel(CurvatureStyle s) {
  switch (s) {
    case CurvatureStyle::Gaussian: return "Gaussiana";
    case CurvatureStyle::Mean: return "Media";
    case CurvatureStyle::MinRadius: return "Raggio minimo";
    case CurvatureStyle::MaxRadius: return "Raggio massimo";
  }
  return "Gaussiana";
}

bool CurvatureRange(const std::vector<const CurvatureData*>& data, CurvatureStyle s, bool autoRange, double& lo, double& hi) {
  std::vector<double> v;
  for (const auto* d : data)
    for (size_t i = 0; i < d->k1.size(); i++) v.push_back(CurvatureValue(s, d->k1[i], d->k2[i]));
  if (v.empty()) return false;
  std::sort(v.begin(), v.end());
  if (autoRange) {
    lo = v[size_t(0.05 * (v.size() - 1))];
    hi = v[size_t(0.95 * (v.size() - 1))];
  } else {
    lo = v.front();
    hi = v.back();
  }
  if (hi - lo <= 1e-6 * std::max({std::fabs(lo), std::fabs(hi), 1e-6})) {
    // valore costante (piano, sfera): intervallo simmetrico intorno al valore
    const double d = std::max(std::fabs(lo) * 0.1, 1e-6);
    lo -= d;
    hi += d;
  }
  return true;
}

void CurvatureColor(CurvatureStyle s, double v, double lo, double hi, float c[3]) {
  double f = hi > lo ? (v - lo) / (hi - lo) : 0.5;
  if (s == CurvatureStyle::Gaussian || s == CurvatureStyle::Mean) f = 1.0 - f;  // alto = rosso
  f = std::clamp(f, 0.0, 1.0);
  if (f < 0.5) { c[0] = float(1 - 2 * f); c[1] = float(2 * f); c[2] = 0; }
  else { c[0] = 0; c[1] = float(2 - 2 * f); c[2] = float(2 * f - 1); }
}

}  // namespace cad
