// Altri comandi di Rhino 8 realizzati con openNURBS: forme di curve (Circle3Pt,
// Arc3Pt, Rectangle3Pt, PolygonEdge, PolygonStar, EllipseDiameter), punti
// (PointGrid, ExtractPt, ExtractControlPoints), solidi (TruncatedCone, Ellipsoid,
// Tube), primitive mesh, modifica (Reverse, CloseCrv, Isolate, Unisolate),
// selezione, livelli, trasformazioni (Rotate3D, Scale1D, ScaleNU, Orient,
// Orient3Pt), Dot, Hatch, DupFaceBorder, EvaluatePt, Check.
#include "commands/Engine.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>

namespace cad {
namespace {

const ON_Geometry* Geo(Engine& e, const ON_UUID& id) {
  const auto* o = e.Object(id);
  return o ? o->Geometry(nullptr) : nullptr;
}

bool Created(Engine& e, const ON_UUID& id, const char* what) {
  if (ON_UuidIsNil(id)) return e.Fail(std::string("impossibile creare ") + what);
  e.Out() << "  Creato: " << what << "\n";
  return true;
}

int OptInt(const std::map<std::string, std::string>& o, const char* k, int def) {
  auto it = o.find(k);
  return it == o.end() ? def : std::atoi(it->second.c_str());
}

std::string Unquote(std::string s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
  return s;
}

std::string Num(double v) {
  std::ostringstream s;
  s.precision(10);
  s << (std::fabs(v) < 1e-12 ? 0.0 : v);
  return s.str();
}

// --- Curve --------------------------------------------------------------------
bool CmdCircle3Pt(Engine& e, TokenStream& ts) {
  auto a = e.ReadPoint(ts, "primo punto sul cerchio");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "secondo punto sul cerchio");
  if (!b) return false;
  auto c = e.ReadPoint(ts, "terzo punto sul cerchio");
  if (!c) return false;
  ON_Circle circle;
  if (!circle.Create(*a, *b, *c)) return e.Fail("i tre punti sono allineati");
  return Created(e, e.AddObject(new ON_ArcCurve(circle)), "cerchio");
}

// Arc3Pt (come Arc, opzione 3Point di Rhino): inizio, fine, punto sull'arco.
bool CmdArc3Pt(Engine& e, TokenStream& ts) {
  auto a = e.ReadPoint(ts, "inizio dell'arco");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "fine dell'arco");
  if (!b) return false;
  auto m = e.ReadPoint(ts, "punto sull'arco");
  if (!m) return false;
  ON_Arc arc;
  if (!arc.Create(*a, *m, *b)) return e.Fail("arco non valido (punti allineati)");
  return Created(e, e.AddObject(new ON_ArcCurve(arc)), "arco");
}

// Rectangle3Pt: due estremi di un lato, poi la larghezza (punto).
bool CmdRectangle3Pt(Engine& e, TokenStream& ts) {
  auto a = e.ReadPoint(ts, "inizio del lato");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "fine del lato");
  if (!b) return false;
  auto c = e.ReadPoint(ts, "larghezza");
  if (!c) return false;
  ON_3dVector x = *b - *a;
  if (x.Length() <= e.Tolerance()) return e.Fail("lato nullo");
  ON_3dVector u = x; u.Unitize();
  ON_3dVector w = (*c - *a) - u * ((*c - *a) * u);
  if (w.Length() <= e.Tolerance()) return e.Fail("larghezza nulla");
  ON_Polyline pl;
  pl.Append(*a); pl.Append(*b); pl.Append(*b + w); pl.Append(*a + w); pl.Append(*a);
  return Created(e, e.AddObject(new ON_PolylineCurve(pl)), "rettangolo");
}

// PolygonEdge: NumSides=, due estremi di un lato (poligono a sinistra del lato, nel piano XY).
bool CmdPolygonEdge(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  const int n = OptInt(opts, "numsides", 5);
  if (n < 3) return e.Fail("servono almeno 3 lati");
  auto a = e.ReadPoint(ts, "inizio del lato");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "fine del lato");
  if (!b) return false;
  const double side = a->DistanceTo(*b);
  if (side <= e.Tolerance()) return e.Fail("lato nullo");
  ON_Polyline pl;
  ON_3dPoint p = *a;
  ON_3dVector d = *b - *a;
  d.Unitize();
  for (int i = 0; i < n; i++) {
    pl.Append(p);
    p = p + d * side;
    d.Rotate(2 * ON_PI / n, ON_3dVector::ZAxis);
  }
  pl.Append(*a);
  return Created(e, e.AddObject(new ON_PolylineCurve(pl)), "poligono");
}

// PolygonStar: NumSides=, centro, primo raggio (punto), secondo raggio (numero).
bool CmdPolygonStar(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  const int n = OptInt(opts, "numsides", 5);
  if (n < 3) return e.Fail("servono almeno 3 punte");
  auto c = e.ReadPoint(ts, "centro della stella");
  if (!c) return false;
  auto p = e.ReadPoint(ts, "punta della stella");
  if (!p) return false;
  auto r2 = e.ReadNumber(ts, "secondo raggio");
  if (!r2) return false;
  ON_3dVector v = *p - *c;
  v.z = 0;
  const double r1 = v.Length();
  if (r1 <= e.Tolerance() || *r2 <= e.Tolerance()) return e.Fail("raggio nullo");
  const double a0 = std::atan2(v.y, v.x);
  ON_Polyline pl;
  for (int i = 0; i < 2 * n; i++) {
    const double r = (i % 2 == 0) ? r1 : *r2, a = a0 + i * ON_PI / n;
    pl.Append(*c + ON_3dVector(r * std::cos(a), r * std::sin(a), 0));
  }
  pl.Append(pl[0]);
  return Created(e, e.AddObject(new ON_PolylineCurve(pl)), "stella");
}

// EllipseDiameter: estremi del primo asse, poi estremo del secondo asse.
bool CmdEllipseDiameter(Engine& e, TokenStream& ts) {
  auto a = e.ReadPoint(ts, "inizio del primo asse");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "fine del primo asse");
  if (!b) return false;
  auto c = e.ReadPoint(ts, "fine del secondo asse");
  if (!c) return false;
  const ON_3dPoint ctr = 0.5 * (*a + *b);
  ON_3dVector x = *b - ctr;
  const double r1 = x.Length();
  if (r1 <= e.Tolerance()) return e.Fail("asse nullo");
  x.Unitize();
  ON_3dVector y = (*c - ctr) - x * ((*c - ctr) * x);
  const double r2 = y.Length();
  if (r2 <= e.Tolerance()) return e.Fail("secondo asse nullo");
  y.Unitize();
  ON_Ellipse el(ON_Plane(ctr, x, y), r1, r2);
  ON_NurbsCurve* nc = new ON_NurbsCurve();
  if (!el.GetNurbForm(*nc)) { delete nc; return e.Fail("ellisse non valida"); }
  return Created(e, e.AddObject(nc), "ellisse");
}

// --- Punti ----------------------------------------------------------------------
// PointGrid: numero in X, numero in Y, passo X, passo Y, punto base.
bool CmdPointGrid(Engine& e, TokenStream& ts) {
  auto nx = e.ReadInt(ts, "numero di punti in X");
  if (!nx) return false;
  auto ny = e.ReadInt(ts, "numero di punti in Y");
  if (!ny) return false;
  auto sx = e.ReadNumber(ts, "passo in X");
  if (!sx) return false;
  auto sy = e.ReadNumber(ts, "passo in Y");
  if (!sy) return false;
  auto p = e.ReadPoint(ts, "punto base");
  if (!p) return false;
  if (*nx < 1 || *ny < 1 || *nx * *ny > 100000) return e.Fail("numero di punti non valido");
  for (int j = 0; j < *ny; j++)
    for (int i = 0; i < *nx; i++) e.AddObject(new ON_Point(*p + ON_3dVector(i * *sx, j * *sy, 0)));
  e.Out() << "  Creati " << (*nx * *ny) << " punti\n";
  return true;
}

// ExtractPt: i punti delle nuvole di punti diventano punti singoli.
bool CmdExtractPt(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("ExtractPt", "seleziona le nuvole di punti, poi Invio");
  if (sel.empty()) return false;
  int n = 0;
  for (const auto& id : sel)
    if (const auto* pc = ON_PointCloud::Cast(Geo(e, id)))
      for (int i = 0; i < pc->m_P.Count(); i++, n++) e.AddObject(new ON_Point(pc->m_P[i]));
  if (!n) return e.Fail("nessuna nuvola di punti selezionata");
  e.Out() << "  Estratti " << n << " punti\n";
  return true;
}

// ExtractControlPoints: punti di controllo di curve e superfici come punti.
bool CmdExtractControlPoints(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("ExtractControlPoints", "seleziona curve o superfici, poi Invio");
  if (sel.empty()) return false;
  int n = 0;
  for (const auto& id : sel) {
    const ON_Geometry* g = Geo(e, id);
    if (const auto* c = ON_Curve::Cast(g)) {
      ON_NurbsCurve nc;
      if (!c->GetNurbForm(nc)) continue;
      for (int i = 0; i < nc.CVCount(); i++, n++) { ON_3dPoint p; nc.GetCV(i, p); e.AddObject(new ON_Point(p)); }
    } else {
      const ON_Surface* s = ON_Surface::Cast(g);
      const ON_Brep* b = ON_Brep::Cast(g);
      if (b && b->m_F.Count() == 1) s = b->m_F[0].SurfaceOf();
      if (!s) continue;
      ON_NurbsSurface ns;
      if (!s->GetNurbForm(ns)) continue;
      for (int i = 0; i < ns.CVCount(0); i++)
        for (int j = 0; j < ns.CVCount(1); j++, n++) { ON_3dPoint p; ns.GetCV(i, j, p); e.AddObject(new ON_Point(p)); }
    }
  }
  if (!n) return e.Fail("nessuna curva o superficie selezionata");
  e.Out() << "  Estratti " << n << " punti di controllo\n";
  return true;
}

// EvaluatePt: coordinate di un punto (assolute, come nella barra di stato).
bool CmdEvaluatePt(Engine& e, TokenStream& ts) {
  auto p = e.ReadPoint(ts, "punto da valutare");
  if (!p) return false;
  e.Out() << "  Punto: " << Num(p->x) << "," << Num(p->y) << "," << Num(p->z) << "\n";
  return true;
}

// --- Solidi ---------------------------------------------------------------------
// TruncatedCone: centro della base, raggio della base, altezza, raggio superiore.
bool CmdTruncatedCone(Engine& e, TokenStream& ts) {
  auto c = e.ReadPoint(ts, "centro della base");
  if (!c) return false;
  auto r1 = e.ReadNumber(ts, "raggio della base");
  if (!r1) return false;
  auto h = e.ReadNumber(ts, "altezza");
  if (!h) return false;
  auto r2 = e.ReadNumber(ts, "raggio superiore");
  if (!r2) return false;
  if (*r1 <= e.Tolerance() || *r2 <= e.Tolerance() || std::fabs(*h) <= e.Tolerance()) return e.Fail("tronco di cono degenere");
  ON_LineCurve* profile = new ON_LineCurve(*c + ON_3dVector(*r1, 0, 0), *c + ON_3dVector(*r2, 0, *h));
  auto* rs = new ON_RevSurface();
  rs->m_curve = profile;
  rs->m_axis = ON_Line(*c, *c + ON_3dVector(0, 0, 1));
  rs->m_angle.Set(0, 2 * ON_PI);
  rs->m_t = rs->m_angle;
  ON_Brep* b = ON_BrepRevSurface(rs, true, true);
  if (!b) { delete rs; return e.Fail("tronco di cono non valido"); }
  return Created(e, e.AddObject(b), "tronco di cono");
}

// Ellipsoid: centro, tre semiassi (X, Y, Z).
bool CmdEllipsoid(Engine& e, TokenStream& ts) {
  auto c = e.ReadPoint(ts, "centro");
  if (!c) return false;
  auto a = e.ReadNumber(ts, "semiasse X");
  if (!a) return false;
  auto b = e.ReadNumber(ts, "semiasse Y");
  if (!b) return false;
  auto z = e.ReadNumber(ts, "semiasse Z");
  if (!z) return false;
  if (*a <= e.Tolerance() || *b <= e.Tolerance() || *z <= e.Tolerance()) return e.Fail("ellissoide degenere");
  // sfera unitaria in forma NURBS, poi scala non uniforme
  ON_NurbsSurface ns;
  ON_Sphere(ON_3dPoint::Origin, 1.0).GetNurbForm(ns);
  ns.Transform(ON_Xform::TranslationTransformation(ON_3dVector(*c)) * ON_Xform::DiagonalTransformation(*a, *b, *z));
  ON_Brep* br = ON_Brep::New();
  ON_Surface* sp = new ON_NurbsSurface(ns);
  if (!br->Create(sp)) { delete br; delete sp; return e.Fail("ellissoide non valido"); }
  return Created(e, e.AddObject(br), "ellissoide");
}

// Tube: centro della base, raggio esterno, raggio interno, altezza (estrusione con foro).
bool CmdTube(Engine& e, TokenStream& ts) {
  auto c = e.ReadPoint(ts, "centro della base");
  if (!c) return false;
  auto r1 = e.ReadNumber(ts, "raggio esterno");
  if (!r1) return false;
  auto r2 = e.ReadNumber(ts, "raggio interno");
  if (!r2) return false;
  auto h = e.ReadNumber(ts, "altezza");
  if (!h) return false;
  const double ro = std::max(*r1, *r2), ri = std::min(*r1, *r2);
  if (ri <= e.Tolerance() || ro - ri <= e.Tolerance() || std::fabs(*h) <= e.Tolerance()) return e.Fail("tubo degenere");
  auto* ex = new ON_Extrusion();
  ON_Line path(*c, *c + ON_3dVector(0, 0, *h));
  if (*h < 0) path = ON_Line(*c + ON_3dVector(0, 0, *h), *c);
  ex->SetPathAndUp(path.from, path.to, ON_3dVector::YAxis);
  // profili nel piano dell'estrusione (coordinate 2D relative al percorso)
  ON_Circle outer(ON_Plane::World_xy, ro), inner(ON_Plane::World_xy, ri);
  inner.Reverse();
  ex->SetOuterProfile(new ON_ArcCurve(outer), true);
  ex->AddInnerProfile(new ON_ArcCurve(inner));
  if (!ex->IsValid()) { delete ex; return e.Fail("tubo non valido"); }
  return Created(e, e.AddObject(ex), "tubo (estrusione chiusa)");
}

// --- Mesh -----------------------------------------------------------------------
}  // namespace

// Mesh a griglia da una funzione (u,v) in [0,1]x[0,1]; closedU/V uniscono i bordi.
ON_Mesh* GridMesh(int nu, int nv, bool closedU, const std::function<ON_3dPoint(double, double)>& f, bool closedV) {
  auto* m = new ON_Mesh();
  const int cu = closedU ? nu : nu + 1, cv = closedV ? nv : nv + 1;
  for (int j = 0; j < cv; j++)
    for (int i = 0; i < cu; i++) {
      ON_3dPoint p = f(double(i) / nu, double(j) / nv);
      for (int k = 0; k < 3; k++) if (std::fabs(p[k]) < 1e-12) p[k] = 0;  // poli identici (niente -0 o 1e-16)
      m->m_V.Append(ON_3fPoint(p));
    }
  for (int j = 0; j < nv; j++)
    for (int i = 0; i < nu; i++) {
      const int i1 = closedU ? (i + 1) % nu : i + 1, j1 = closedV ? (j + 1) % nv : j + 1;
      m->SetQuad(m->m_F.Count(), j * cu + i, j * cu + i1, j1 * cu + i1, j1 * cu + i);
    }
  m->CombineIdenticalVertices();  // poli della sfera e del cono
  m->CullDegenerateFaces();
  m->CullUnusedVertices();
  m->ComputeVertexNormals();
  m->Compact();
  return m;
}

namespace {

bool AddMesh(Engine& e, ON_Mesh* m, const char* what) {
  if (!m || !m->IsValid()) { delete m; return e.Fail(std::string(what) + " non valida"); }
  return Created(e, e.AddObject(m), what);
}

// MeshPlane: due angoli (una faccia, come la densita' minima di Rhino).
bool CmdMeshPlane(Engine& e, TokenStream& ts) {
  auto a = e.ReadPoint(ts, "primo angolo");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "secondo angolo");
  if (!b) return false;
  const ON_3dPoint p = *a, q = *b;
  if (std::fabs(q.x - p.x) <= e.Tolerance() || std::fabs(q.y - p.y) <= e.Tolerance()) return e.Fail("piano degenere");
  return AddMesh(e, GridMesh(1, 1, false, [&](double u, double v) { return ON_3dPoint(p.x + u * (q.x - p.x), p.y + v * (q.y - p.y), p.z); }), "mesh piana");
}

// MeshBox: due angoli della base e altezza; 1 faccia per lato.
bool CmdMeshBox(Engine& e, TokenStream& ts) {
  auto a = e.ReadPoint(ts, "primo angolo della base");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "angolo opposto della base");
  if (!b) return false;
  auto h = e.ReadNumber(ts, "altezza");
  if (!h) return false;
  ON_BoundingBox bb(*a, *b);
  bb.m_max.z = a->z + *h;
  bb.m_min.z = std::min(a->z, a->z + *h);
  bb.m_max.z = std::max(a->z, a->z + *h);
  if (bb.m_max.x - bb.m_min.x <= e.Tolerance() || bb.m_max.y - bb.m_min.y <= e.Tolerance() || bb.m_max.z - bb.m_min.z <= e.Tolerance())
    return e.Fail("parallelepipedo degenere");
  auto* m = new ON_Mesh();
  // 0..3 in basso (x,y) = (min,min),(max,min),(max,max),(min,max); 4..7 in alto
  // (ON_BoundingBox::GetCorners usa un altro ordine: x, poi y, poi z)
  for (int k = 0; k < 2; k++) {
    const double z = k ? bb.m_max.z : bb.m_min.z;
    for (const auto& xy : {ON_2dPoint(bb.m_min.x, bb.m_min.y), ON_2dPoint(bb.m_max.x, bb.m_min.y),
                           ON_2dPoint(bb.m_max.x, bb.m_max.y), ON_2dPoint(bb.m_min.x, bb.m_max.y)})
      m->m_V.Append(ON_3fPoint(float(xy.x), float(xy.y), float(z)));
  }
  const int F[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
  for (auto& f : F) m->SetQuad(m->m_F.Count(), f[0], f[1], f[2], f[3]);
  m->ComputeVertexNormals();
  return AddMesh(e, m, "mesh parallelepipedo");
}

bool CmdMeshSphere(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto c = e.ReadPoint(ts, "centro della sfera");
  if (!c) return false;
  auto r = e.ReadNumber(ts, "raggio");
  if (!r) return false;
  if (*r <= e.Tolerance()) return e.Fail("raggio nullo");
  const int nu = OptInt(opts, "uvcount", 16), nv = std::max(2, OptInt(opts, "vertical", nu / 2));
  const ON_3dPoint C = *c;
  const double R = *r;
  return AddMesh(e, GridMesh(nu, nv, true, [&](double u, double v) {
    if (v <= 0 || v >= 1) return C + ON_3dVector(0, 0, v <= 0 ? -R : R);  // poli esatti
    const double a = 2 * ON_PI * u, b = -ON_PI / 2 + ON_PI * v;
    return C + ON_3dVector(R * std::cos(b) * std::cos(a), R * std::cos(b) * std::sin(a), R * std::sin(b));
  }), "mesh sferica");
}

bool CmdMeshCylinder(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto c = e.ReadPoint(ts, "centro della base");
  if (!c) return false;
  auto r = e.ReadNumber(ts, "raggio");
  if (!r) return false;
  auto h = e.ReadNumber(ts, "altezza");
  if (!h) return false;
  if (*r <= e.Tolerance() || std::fabs(*h) <= e.Tolerance()) return e.Fail("cilindro degenere");
  const int n = OptInt(opts, "aroundcount", 16);
  const ON_3dPoint C = *c;
  const double R = *r, H = *h;
  // parete + due tappi (anelli che si chiudono al centro)
  ON_Mesh* m = GridMesh(n, 3, true, [&](double u, double v) {
    const double a = 2 * ON_PI * u;
    const double rr = (v == 0 || v == 1) ? 0 : R;
    const double z = v <= 1.0 / 3 ? 0 : H;
    return C + ON_3dVector(rr * std::cos(a), rr * std::sin(a), z);
  });
  return AddMesh(e, m, "mesh cilindrica");
}

bool CmdMeshCone(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto c = e.ReadPoint(ts, "centro della base");
  if (!c) return false;
  auto r = e.ReadNumber(ts, "raggio");
  if (!r) return false;
  auto h = e.ReadNumber(ts, "altezza");
  if (!h) return false;
  if (*r <= e.Tolerance() || std::fabs(*h) <= e.Tolerance()) return e.Fail("cono degenere");
  const int n = OptInt(opts, "aroundcount", 16);
  const ON_3dPoint C = *c;
  const double R = *r, H = *h;
  return AddMesh(e, GridMesh(n, 2, true, [&](double u, double v) {
    const double a = 2 * ON_PI * u;
    const double rr = v == 0.5 ? R : 0;
    const double z = v < 0.75 ? 0 : H;
    return C + ON_3dVector(rr * std::cos(a), rr * std::sin(a), z);
  }), "mesh conica");
}

bool CmdMeshTorus(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto c = e.ReadPoint(ts, "centro del toro");
  if (!c) return false;
  auto r1 = e.ReadNumber(ts, "raggio maggiore");
  if (!r1) return false;
  auto r2 = e.ReadNumber(ts, "raggio minore");
  if (!r2) return false;
  if (*r2 <= e.Tolerance() || *r1 <= *r2) return e.Fail("toro degenere");
  const int nu = OptInt(opts, "aroundcount", 24), nv = OptInt(opts, "verticalcount", 12);
  const ON_3dPoint C = *c;
  const double A = *r1, B = *r2;
  // anello chiuso in entrambe le direzioni: l'ultima riga coincide con la prima
  return AddMesh(e, GridMesh(nu, nv, true, [&](double u, double v) {
    const double a = 2 * ON_PI * u, b = 2 * ON_PI * v;
    return C + ON_3dVector((A + B * std::cos(b)) * std::cos(a), (A + B * std::cos(b)) * std::sin(a), B * std::sin(b));
  }, true), "mesh toroidale");
}

// --- Modifica -------------------------------------------------------------------
bool CmdReverse(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("Reverse", "seleziona le curve da invertire, poi Invio");
  if (sel.empty()) return false;
  int n = 0;
  for (const auto& id : sel)
    if (auto* c = ON_Curve::Cast(e.EditableGeometry(id)); c && c->Reverse()) n++;
  if (!n) return e.Fail("nessuna curva selezionata");
  e.Out() << "  Invertita la direzione di " << n << " curve\n";
  return true;
}

// CloseCrv: chiude le curve aperte; se gli estremi sono vicini sposta la fine
// sull'inizio, altrimenti aggiunge un segmento (come le opzioni di Rhino).
bool CmdCloseCrv(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("CloseCrv", "seleziona le curve aperte, poi Invio");
  if (sel.empty()) return false;
  int n = 0;
  for (const auto& id : sel) {
    const auto* c = ON_Curve::Cast(Geo(e, id));
    if (!c || c->IsClosed()) continue;
    const ON_3dPoint a = c->PointAtStart(), b = c->PointAtEnd();
    std::unique_ptr<ON_Curve> nc(c->DuplicateCurve());
    if (a.DistanceTo(b) <= 10 * e.Tolerance()) {
      nc->SetEndPoint(a);
    } else {
      auto* pc = new ON_PolyCurve();
      pc->Append(nc.release());
      pc->Append(new ON_LineCurve(b, a));
      nc.reset(pc);
    }
    if (!nc->IsClosed()) continue;
    ON_3dmObjectAttributes at = *e.Object(id)->Attributes(&ON_3dmObjectAttributes::DefaultAttributes);
    e.DeleteObject(id);
    e.AddObject(nc.release(), &at);
    n++;
  }
  if (!n) return e.Fail("nessuna curva aperta selezionata");
  e.Out() << "  Chiuse " << n << " curve\n";
  return true;
}

// Isolate / Unisolate: nasconde tutto il resto; Unisolate mostra solo quello
// nascosto da Isolate (segnato con una chiave nel testo utente dell'oggetto).
const wchar_t* kIsolated = L".cad.isolated";
bool CmdIsolate(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("Isolate", "seleziona gli oggetti da isolare, poi Invio");
  if (sel.empty()) return false;
  std::set<ON_UUID, UuidLess> keep(sel.begin(), sel.end());
  int n = 0;
  for (const auto& id : e.AllObjectIds(false)) {
    if (keep.count(id)) continue;
    if (auto* a = e.EditableAttributes(id)) {
      a->SetMode(ON::object_mode::hidden_object);
      a->SetUserString(kIsolated, L"1");
      n++;
    }
  }
  e.Out() << "  Nascosti " << n << " oggetti\n";
  return true;
}

bool CmdUnisolate(Engine& e, TokenStream&) {
  int n = 0;
  for (const auto& id : e.AllObjectIds(true)) {
    const auto* o = e.Object(id);
    ON_wString v;
    if (!o || !o->Attributes(nullptr)->GetUserString(kIsolated, v)) continue;
    if (auto* a = e.EditableAttributes(id)) {
      a->SetMode(ON::object_mode::normal_object);
      a->SetUserString(kIsolated, nullptr);  // nullptr = elimina la chiave
      n++;
    }
  }
  e.Out() << "  Mostrati " << n << " oggetti\n";
  return true;
}

// DupFaceBorder: bordi di ogni faccia delle polisuperfici selezionate.
bool CmdDupFaceBorder(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("DupFaceBorder", "seleziona superfici o polisuperfici, poi Invio");
  if (sel.empty()) return false;
  int n = 0;
  for (const auto& id : sel) {
    const ON_Geometry* g = Geo(e, id);
    std::unique_ptr<ON_Brep> owned;
    const ON_Brep* b = ON_Brep::Cast(g);
    if (!b) {
      if (const auto* x = ON_Extrusion::Cast(g)) owned.reset(x->BrepForm());
      else if (const auto* s = ON_Surface::Cast(g)) owned.reset(s->BrepForm());
      b = owned.get();
    }
    if (!b) continue;
    for (int fi = 0; fi < b->m_F.Count(); fi++) {
      const ON_BrepFace& f = b->m_F[fi];
      for (int li = 0; li < f.m_li.Count(); li++) {
        const ON_BrepLoop& L = b->m_L[f.m_li[li]];
        ON_SimpleArray<const ON_Curve*> segs;
        std::vector<std::unique_ptr<ON_Curve>> keep;
        for (int k = 0; k < L.m_ti.Count(); k++) {
          const ON_BrepTrim& t = b->m_T[L.m_ti[k]];
          if (t.m_ei < 0) continue;  // trim singolare (polo)
          const ON_BrepEdge& ed = b->m_E[t.m_ei];
          std::unique_ptr<ON_Curve> c(ed.DuplicateCurve());
          if (!c) continue;
          if (t.m_bRev3d) c->Reverse();
          segs.Append(c.get());
          keep.push_back(std::move(c));
        }
        ON_SimpleArray<ON_Curve*> joined;
        ON_JoinCurves(segs, joined, e.Tolerance(), false, nullptr);
        for (int i = 0; i < joined.Count(); i++, n++) e.AddObject(joined[i]);
      }
    }
  }
  if (!n) return e.Fail("nessuna superficie selezionata");
  e.Out() << "  Create " << n << " curve di bordo\n";
  return true;
}

// --- Selezione ------------------------------------------------------------------
bool SelectWhere(Engine& e, const std::function<bool(const ON_Geometry*)>& pred, const char* what) {
  int n = 0;
  for (const auto& id : e.AllObjectIds(false))
    if (const ON_Geometry* g = Geo(e, id); g && pred(g)) { e.Select(id); n++; }
  e.Out() << "  Selezionati " << n << " " << what << "\n";
  return true;
}
bool CmdSelClosedCrv(Engine& e, TokenStream&) {
  return SelectWhere(e, [](const ON_Geometry* g) { auto* c = ON_Curve::Cast(g); return c && c->IsClosed(); }, "curve chiuse");
}
bool CmdSelOpenCrv(Engine& e, TokenStream&) {
  return SelectWhere(e, [](const ON_Geometry* g) { auto* c = ON_Curve::Cast(g); return c && !c->IsClosed(); }, "curve aperte");
}
bool CmdSelVisible(Engine& e, TokenStream&) {
  return SelectWhere(e, [](const ON_Geometry*) { return true; }, "oggetti visibili");
}

// SelDup: oggetti con geometria identica a un oggetto precedente (stessa serializzazione).
bool CmdSelDup(Engine& e, TokenStream&) {
  std::map<std::string, ON_UUID> seen;
  int n = 0;
  for (const auto& id : e.AllObjectIds(false)) {
    const ON_Geometry* g = Geo(e, id);
    if (!g) continue;
    std::unique_ptr<ON_Object> copy(g->Duplicate());
    copy->PurgeUserData();
    ON_Write3dmBufferArchive ar(0, 0, 80, ON::Version());
    if (!ar.WriteObject(*copy)) continue;
    ON_SHA1 sha;
    sha.AccumulateBytes(ar.Buffer(), ar.SizeOfArchive());
    const std::string key = static_cast<const char*>(sha.Hash().ToUTF8String(false));
    if (seen.count(key)) { e.Select(id); n++; }
    else seen[key] = id;
  }
  e.Out() << "  Selezionati " << n << " duplicati\n";
  return true;
}

// --- Livelli --------------------------------------------------------------------
ON_Layer* LayerByName(Engine& e, const std::string& name) {
  ONX_ModelComponentIterator it(e.Model(), ON_ModelComponent::Type::Layer);
  for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent())
    if (ON_wString(name.c_str()).CompareNoCase(c->Name()) == 0)
      return const_cast<ON_Layer*>(ON_Layer::Cast(c));
  return nullptr;
}
ON_Layer* LayerByIndex(Engine& e, int i) {
  return const_cast<ON_Layer*>(ON_Layer::Cast(e.Model().ComponentFromIndex(ON_ModelComponent::Type::Layer, i).ModelComponent()));
}

// Livelli degli oggetti selezionati (LayerOff, LayerLock) o per nome/All (LayerOn, LayerUnlock).
bool LayerSet(Engine& e, TokenStream& ts, const char* cmd, bool fromSelection, const std::function<void(ON_Layer&)>& f) {
  std::set<int> idx;
  if (fromSelection) {
    auto sel = e.RequireSelection(cmd, "seleziona gli oggetti dei livelli, poi Invio");
    if (sel.empty()) return false;
    for (const auto& id : sel) idx.insert(e.Object(id)->Attributes(nullptr)->m_layer_index);
  } else {
    auto name = e.ReadString(ts, "nome del livello (All per tutti)");
    if (!name) return false;
    const std::string n = Unquote(*name);
    if (NormalizeName(n) == "all") {
      ONX_ModelComponentIterator it(e.Model(), ON_ModelComponent::Type::Layer);
      for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent()) idx.insert(c->Index());
    } else if (ON_Layer* l = LayerByName(e, n)) {
      idx.insert(l->Index());
    } else {
      return e.Fail("livello inesistente: " + n);
    }
  }
  const int current = e.Model().m_settings.CurrentLayerIndex();
  int n = 0;
  for (int i : idx)
    if (ON_Layer* l = LayerByIndex(e, i)) {
      if (fromSelection && i == current) continue;  // come Rhino: il livello corrente non si spegne ne' si blocca
      f(*l);
      n++;
    }
  e.ClearSelection();
  e.Out() << "  Modificati " << n << " livelli\n";
  return true;
}
bool CmdLayerOff(Engine& e, TokenStream& ts) { return LayerSet(e, ts, "LayerOff", true, [](ON_Layer& l) { l.SetVisible(false); }); }
bool CmdLayerLock(Engine& e, TokenStream& ts) { return LayerSet(e, ts, "LayerLock", true, [](ON_Layer& l) { l.SetLocked(true); }); }
bool CmdLayerOn(Engine& e, TokenStream& ts) { return LayerSet(e, ts, "LayerOn", false, [](ON_Layer& l) { l.SetVisible(true); }); }
bool CmdLayerUnlock(Engine& e, TokenStream& ts) { return LayerSet(e, ts, "LayerUnlock", false, [](ON_Layer& l) { l.SetLocked(false); }); }

// CurrentLayer: nome del livello da rendere corrente.
bool CmdCurrentLayer(Engine& e, TokenStream& ts) {
  auto name = e.ReadString(ts, "nome del livello");
  if (!name) return false;
  ON_Layer* l = LayerByName(e, Unquote(*name));
  if (!l) return e.Fail("livello inesistente: " + *name);
  l->SetVisible(true);
  e.Model().m_settings.SetCurrentLayerId(l->Id());
  e.Out() << "  Livello corrente: " << Unquote(*name) << "\n";
  return true;
}

// --- Trasformazioni -------------------------------------------------------------
bool Apply(Engine& e, const std::vector<ON_UUID>& sel, const ON_Xform& x) {
  int n = 0;
  for (const auto& id : sel)
    if (ON_Geometry* g = e.EditableGeometry(id); g && g->Transform(x)) n++;
  e.Out() << "  Trasformati " << n << " oggetti\n";
  return n > 0 || e.Fail("nessun oggetto trasformato");
}

// Rotate3D: inizio e fine dell'asse, angolo in gradi.
bool CmdRotate3D(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("Rotate3D");
  if (sel.empty()) return false;
  auto a = e.ReadPoint(ts, "inizio dell'asse di rotazione");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "fine dell'asse di rotazione");
  if (!b) return false;
  auto ang = e.ReadNumber(ts, "angolo (gradi)");
  if (!ang) return false;
  ON_3dVector ax = *b - *a;
  if (!ax.Unitize()) return e.Fail("asse nullo");
  ON_Xform r;
  r.Rotation(*ang * ON_PI / 180.0, ax, *a);
  return Apply(e, sel, r);
}

// Scale1D: punto base, fattore, punto che indica la direzione di scala.
bool CmdScale1D(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("Scale1D");
  if (sel.empty()) return false;
  auto c = e.ReadPoint(ts, "punto base");
  if (!c) return false;
  auto f = e.ReadNumber(ts, "fattore di scala");
  if (!f) return false;
  auto d = e.ReadPoint(ts, "punto nella direzione di scala");
  if (!d) return false;
  ON_3dVector u = *d - *c;
  if (!u.Unitize()) return e.Fail("direzione nulla");
  if (std::fabs(*f) <= ON_ZERO_TOLERANCE) return e.Fail("fattore di scala nullo");
  // I + (f-1) u u^T attorno a c
  ON_Xform s(1.0);
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++) s[i][j] += (*f - 1) * u[i] * u[j];
  const ON_Xform x = ON_Xform::TranslationTransformation(ON_3dVector(*c)) * s * ON_Xform::TranslationTransformation(-ON_3dVector(*c));
  return Apply(e, sel, x);
}

// ScaleNU: punto base, fattori X, Y, Z.
bool CmdScaleNU(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("ScaleNU");
  if (sel.empty()) return false;
  auto c = e.ReadPoint(ts, "punto base");
  if (!c) return false;
  auto x = e.ReadNumber(ts, "fattore X");
  if (!x) return false;
  auto y = e.ReadNumber(ts, "fattore Y");
  if (!y) return false;
  auto z = e.ReadNumber(ts, "fattore Z");
  if (!z) return false;
  if (std::fabs(*x * *y * *z) <= ON_ZERO_TOLERANCE) return e.Fail("fattore di scala nullo");
  return Apply(e, sel, ON_Xform::ScaleTransformation(*c, *x, *y, *z));
}

// Orient: due punti di riferimento e due di destinazione (Scale=Yes scala con la distanza).
bool CmdOrient(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("Orient");
  if (sel.empty()) return false;
  auto r1 = e.ReadPoint(ts, "primo punto di riferimento");
  if (!r1) return false;
  auto r2 = e.ReadPoint(ts, "secondo punto di riferimento");
  if (!r2) return false;
  auto t1 = e.ReadPoint(ts, "primo punto di destinazione");
  if (!t1) return false;
  auto t2 = e.ReadPoint(ts, "secondo punto di destinazione");
  if (!t2) return false;
  ON_3dVector a = *r2 - *r1, b = *t2 - *t1;
  const double la = a.Length(), lb = b.Length();
  if (la <= e.Tolerance() || lb <= e.Tolerance()) return e.Fail("punti coincidenti");
  ON_Xform rot;
  rot.Rotation(a, b, ON_3dPoint::Origin);  // porta la direzione a sulla direzione b
  const bool scale = opts.count("scale") && (opts["scale"] == "yes" || opts["scale"] == "si");
  ON_Xform x = ON_Xform::TranslationTransformation(ON_3dVector(*t1)) *
               (scale ? ON_Xform::DiagonalTransformation(lb / la) : ON_Xform(1.0)) * rot *
               ON_Xform::TranslationTransformation(-ON_3dVector(*r1));
  return Apply(e, sel, x);
}

// Orient3Pt: tre punti di riferimento e tre di destinazione (movimento rigido).
bool CmdOrient3Pt(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("Orient3Pt");
  if (sel.empty()) return false;
  ON_3dPoint r[3], t[3];
  const char* pr[3] = {"primo punto di riferimento", "secondo punto di riferimento", "terzo punto di riferimento"};
  const char* pt[3] = {"primo punto di destinazione", "secondo punto di destinazione", "terzo punto di destinazione"};
  for (int i = 0; i < 3; i++) { auto p = e.ReadPoint(ts, pr[i]); if (!p) return false; r[i] = *p; }
  for (int i = 0; i < 3; i++) { auto p = e.ReadPoint(ts, pt[i]); if (!p) return false; t[i] = *p; }
  ON_Plane from, to;
  if (!from.CreateFromFrame(r[0], r[1] - r[0], r[2] - r[0]) || !to.CreateFromFrame(t[0], t[1] - t[0], t[2] - t[0]))
    return e.Fail("punti allineati");
  ON_Xform x;
  x.Rotation(from, to);
  return Apply(e, sel, x);
}

// --- Annotazioni ----------------------------------------------------------------
// Dot: punto e testo (ON_TextDot, sempre rivolto verso lo schermo in Rhino).
bool CmdDot(Engine& e, TokenStream& ts) {
  auto p = e.ReadPoint(ts, "posizione del punto di testo");
  if (!p) return false;
  auto s = e.ReadString(ts, "testo");
  if (!s) return false;
  const std::string txt = Unquote(*s);
  if (txt.empty()) return e.Fail("testo vuoto");
  auto* d = new ON_TextDot();
  d->SetCenterPoint(*p);
  d->SetPrimaryText(ON_wString(txt.c_str()));
  return Created(e, e.AddObject(d), "punto di testo");
}

// Hatch: tratteggio pieno dentro le curve chiuse e planari selezionate.
bool CmdHatch(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("Hatch", "seleziona le curve chiuse di contorno, poi Invio");
  if (sel.empty()) return false;
  ON_SimpleArray<const ON_Curve*> crvs;
  for (const auto& id : sel)
    if (const auto* c = ON_Curve::Cast(Geo(e, id)); c && c->IsClosed() && c->IsPlanar(nullptr, e.Tolerance())) crvs.Append(c);
  if (crvs.Count() == 0) return e.Fail("servono curve chiuse e planari");
  ON_Plane plane;
  // il contorno esterno e' la curva con l'ingombro maggiore; le altre sono fori
  std::vector<const ON_Curve*> order(crvs.Array(), crvs.Array() + crvs.Count());
  auto area = [](const ON_Curve* c) { ON_BoundingBox b = c->BoundingBox(); return b.Diagonal().Length(); };
  std::sort(order.begin(), order.end(), [&](const ON_Curve* a, const ON_Curve* b) { return area(a) > area(b); });
  if (!order[0]->IsPlanar(&plane, e.Tolerance())) return e.Fail("curva non planare");
  // motivo: Pattern=nome dalla tabella del modello; predefinito "Solid" (aggiunto se manca)
  const std::string want = opts.count("pattern") ? opts["pattern"] : "solid";
  int pattern = -1;
  ONX_ModelComponentIterator hit(e.Model(), ON_ModelComponent::Type::HatchPattern);
  for (const ON_ModelComponent* c = hit.FirstComponent(); c; c = hit.NextComponent())
    if (ON_wString(want.c_str()).CompareNoCase(c->Name()) == 0) pattern = c->Index();
  if (pattern < 0) {
    if (want != "solid") return e.Fail("motivo di tratteggio inesistente: " + want);
    ON_HatchPattern hp(ON_HatchPattern::Solid);
    hp.ClearId();
    hp.ClearIndex();
    hp.SetName(L"Solid");
    ON_ModelComponentReference ref = e.Model().AddModelComponent(hp);
    if (!ref.ModelComponent()) return e.Fail("impossibile aggiungere il motivo Solid");
    pattern = ref.ModelComponent()->Index();
  }
  // le curve dei cicli vanno nelle coordinate del piano del tratteggio (2D)
  ON_Xform toPlane;
  toPlane.Rotation(plane, ON_xy_plane);
  std::vector<std::unique_ptr<ON_Curve>> local;
  ON_SimpleArray<const ON_Curve*> loops;
  for (const ON_Curve* c : order) {
    local.emplace_back(c->DuplicateCurve());
    local.back()->Transform(toPlane);
    loops.Append(local.back().get());
  }
  auto* hatch = new ON_Hatch();
  if (!hatch->Create(plane, loops, pattern, 0.0, 1.0)) { delete hatch; return e.Fail("tratteggio non valido"); }
  return Created(e, e.AddObject(hatch), "tratteggio");
}

// --- Analisi --------------------------------------------------------------------
bool CmdCheck(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("Check", "seleziona gli oggetti da controllare, poi Invio");
  if (sel.empty()) return false;
  int bad = 0;
  for (const auto& id : sel) {
    const ON_Geometry* g = Geo(e, id);
    ON_wString w;
    ON_TextLog log(w);
    if (g && !g->IsValid(&log)) {
      bad++;
      e.Out() << "  Oggetto non valido (" << g->ClassId()->ClassName() << "):\n" << ToUtf8(w).substr(0, 600) << "\n";
    }
  }
  if (!bad) e.Out() << "  " << sel.size() << " oggetti controllati: tutti validi\n";
  return true;
}

}  // namespace

void RegisterExtraCommands(Engine& e) {
  auto reg = [&](const char* n, const char* cat, const char* d, CommandFn f, bool mod = true) {
    CommandInfo i{n, cat, d, f};
    i.modifiesModel = mod;
    e.Register(i);
  };
  reg("Circle3Pt", "Curve", "Cerchio per tre punti", CmdCircle3Pt);
  reg("Arc3Pt", "Curve", "Arco per tre punti (inizio, fine, punto sull'arco)", CmdArc3Pt);
  reg("Rectangle3Pt", "Curve", "Rettangolo da un lato e dalla larghezza", CmdRectangle3Pt);
  reg("PolygonEdge", "Curve", "Poligono da un lato (NumSides=)", CmdPolygonEdge);
  reg("PolygonStar", "Curve", "Stella (NumSides=, centro, punta, secondo raggio)", CmdPolygonStar);
  reg("EllipseDiameter", "Curve", "Ellisse dagli estremi degli assi", CmdEllipseDiameter);
  reg("PointGrid", "Punti", "Griglia di punti", CmdPointGrid);
  reg("ExtractPt", "Punti", "Punti singoli dalle nuvole di punti", CmdExtractPt);
  reg("ExtractControlPoints", "Punti", "Punti di controllo come oggetti punto", CmdExtractControlPoints);
  reg("EvaluatePt", "Analisi", "Coordinate di un punto", CmdEvaluatePt, false);
  reg("TruncatedCone", "Solidi", "Tronco di cono (centro, raggio, altezza, raggio superiore)", CmdTruncatedCone);
  reg("Ellipsoid", "Solidi", "Ellissoide (centro, tre semiassi)", CmdEllipsoid);
  reg("Tube", "Solidi", "Tubo (centro, raggio esterno, raggio interno, altezza)", CmdTube);
  reg("MeshPlane", "Mesh", "Piano mesh", CmdMeshPlane);
  reg("MeshBox", "Mesh", "Parallelepipedo mesh", CmdMeshBox);
  reg("MeshSphere", "Mesh", "Sfera mesh (UVCount=)", CmdMeshSphere);
  reg("MeshCylinder", "Mesh", "Cilindro mesh (AroundCount=)", CmdMeshCylinder);
  reg("MeshCone", "Mesh", "Cono mesh (AroundCount=)", CmdMeshCone);
  reg("MeshTorus", "Mesh", "Toro mesh (AroundCount=, VerticalCount=)", CmdMeshTorus);
  reg("Reverse", "Modifica", "Inverte la direzione delle curve", CmdReverse);
  reg("CloseCrv", "Modifica", "Chiude le curve aperte", CmdCloseCrv);
  reg("Isolate", "Visibilità", "Nasconde tutti gli oggetti non selezionati", CmdIsolate);
  reg("Unisolate", "Visibilità", "Mostra gli oggetti nascosti da Isolate", CmdUnisolate);
  reg("DupFaceBorder", "Curve", "Curve di bordo di ogni faccia", CmdDupFaceBorder);
  reg("SelClosedCrv", "Seleziona", "Seleziona le curve chiuse", CmdSelClosedCrv, false);
  reg("SelOpenCrv", "Seleziona", "Seleziona le curve aperte", CmdSelOpenCrv, false);
  reg("SelVisible", "Seleziona", "Seleziona gli oggetti visibili", CmdSelVisible, false);
  reg("SelDup", "Seleziona", "Seleziona gli oggetti duplicati", CmdSelDup, false);
  reg("LayerOff", "Livelli", "Spegne i livelli degli oggetti selezionati", CmdLayerOff);
  reg("LayerOn", "Livelli", "Accende un livello (nome o All)", CmdLayerOn);
  reg("LayerLock", "Livelli", "Blocca i livelli degli oggetti selezionati", CmdLayerLock);
  reg("LayerUnlock", "Livelli", "Sblocca un livello (nome o All)", CmdLayerUnlock);
  reg("CurrentLayer", "Livelli", "Rende corrente un livello", CmdCurrentLayer);
  reg("Rotate3D", "Trasforma", "Rotazione attorno a un asse qualsiasi", CmdRotate3D);
  reg("Scale1D", "Trasforma", "Scala in una direzione", CmdScale1D);
  reg("ScaleNU", "Trasforma", "Scala non uniforme (X, Y, Z)", CmdScaleNU);
  reg("Orient", "Trasforma", "Orienta con due punti (Scale=)", CmdOrient);
  reg("Orient3Pt", "Trasforma", "Orienta con tre punti", CmdOrient3Pt);
  reg("Dot", "Annotazioni", "Punto di testo", CmdDot);
  reg("Hatch", "Annotazioni", "Tratteggio dentro curve chiuse (Pattern=)", CmdHatch);
  reg("Check", "Analisi", "Controlla la validita' degli oggetti", CmdCheck, false);
}

}  // namespace cad
