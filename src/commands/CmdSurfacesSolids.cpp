// Comandi di creazione di superfici, solidi e SubD (menu Superfici/Solidi/SubD).
#include "commands/Engine.h"
#include <cmath>
#include <cstdlib>

namespace cad {
namespace {

bool LooksLikePoint(const std::string& t) {
  return t.find(',') != std::string::npos || t.find('<') != std::string::npos;
}

bool YesNo(const std::map<std::string, std::string>& o, const char* key, bool def) {
  auto it = o.find(key);
  if (it == o.end()) return def;
  return it->second == "yes" || it->second == "si" || it->second == "true" || it->second == "1";
}

// Altezza: numero, oppure punto (si usa la differenza in Z dal riferimento).
std::optional<double> ReadHeight(Engine& e, TokenStream& ts, const ON_3dPoint& base) {
  if (!ts.AtEnd() && LooksLikePoint(ts.Peek())) {
    auto p = e.ReadPoint(ts, "altezza");
    if (!p) return std::nullopt;
    return p->z - base.z;
  }
  return e.ReadNumber(ts, "altezza");
}

std::optional<double> ReadRadius(Engine& e, TokenStream& ts, const ON_3dPoint& center, const char* prompt) {
  if (!ts.AtEnd() && LooksLikePoint(ts.Peek())) {
    auto p = e.ReadPoint(ts, prompt);
    if (!p) return std::nullopt;
    ON_3dVector v = *p - center;
    v.z = 0;
    return v.Length();
  }
  return e.ReadNumber(ts, prompt);
}

bool Report(Engine& e, const ON_UUID& id, const char* what) {
  if (ON_UuidIsNil(id)) return e.Fail(std::string("impossibile creare ") + what);
  e.Out() << "  Creato: " << what << "\n";
  return true;
}

ON_Brep* BrepFromSurface(ON_Surface* s) {
  ON_Brep* b = ON_Brep::New();
  if (!b->Create(s)) { delete b; delete s; return nullptr; }
  return b;
}

// Box: due angoli della base e altezza. Rhino 8 crea un oggetto estrusione.
bool CmdBox(Engine& e, TokenStream& ts) {
  auto a = e.ReadPoint(ts, "primo angolo della base");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "altro angolo della base");
  if (!b) return false;
  auto h = ReadHeight(e, ts, *a);
  if (!h) return false;
  if (std::fabs(a->x - b->x) <= e.Tolerance() || std::fabs(a->y - b->y) <= e.Tolerance() ||
      std::fabs(*h) <= e.Tolerance())
    return e.Fail("parallelepipedo degenere");
  ON_Polyline pl;
  pl.Append(*a);
  pl.Append(ON_3dPoint(b->x, a->y, a->z));
  pl.Append(ON_3dPoint(b->x, b->y, a->z));
  pl.Append(ON_3dPoint(a->x, b->y, a->z));
  pl.Append(*a);
  ON_PolylineCurve profile(pl);
  ON_Plane plane(*a, ON_3dVector::ZAxis);
  ON_Extrusion* ex = ON_Extrusion::CreateFrom3dCurve(profile, &plane, *h, true);
  if (!ex) return e.Fail("parallelepipedo non valido");
  return Report(e, e.AddObject(ex), "parallelepipedo (estrusione chiusa)");
}

bool CmdSphere(Engine& e, TokenStream& ts) {
  auto c = e.ReadPoint(ts, "centro della sfera");
  if (!c) return false;
  auto r = ReadRadius(e, ts, *c, "raggio");
  if (!r || *r <= e.Tolerance()) return r ? e.Fail("raggio nullo") : false;
  ON_Brep* b = ON_BrepSphere(ON_Sphere(*c, *r));
  if (!b) return e.Fail("sfera non valida");
  return Report(e, e.AddObject(b), "sfera");
}

// Cylinder: centro della base, raggio, altezza. Rhino 8 crea un'estrusione.
bool CmdCylinder(Engine& e, TokenStream& ts) {
  auto c = e.ReadPoint(ts, "centro della base");
  if (!c) return false;
  auto r = ReadRadius(e, ts, *c, "raggio");
  if (!r) return false;
  auto h = ReadHeight(e, ts, *c);
  if (!h) return false;
  if (*r <= e.Tolerance() || std::fabs(*h) <= e.Tolerance()) return e.Fail("cilindro degenere");
  ON_Cylinder cyl(ON_Circle(ON_Plane(*c, ON_3dVector::ZAxis), *r), *h);
  ON_Extrusion* ex = ON_Extrusion::Cylinder(cyl, true, true);
  if (!ex) return e.Fail("cilindro non valido");
  return Report(e, e.AddObject(ex), "cilindro (estrusione chiusa)");
}

bool CmdCone(Engine& e, TokenStream& ts) {
  auto c = e.ReadPoint(ts, "centro della base");
  if (!c) return false;
  auto r = ReadRadius(e, ts, *c, "raggio");
  if (!r) return false;
  auto h = ReadHeight(e, ts, *c);
  if (!h) return false;
  if (*r <= e.Tolerance() || std::fabs(*h) <= e.Tolerance()) return e.Fail("cono degenere");
  // ON_Cone: origine del piano = vertice; la base sta a "height" lungo Z.
  ON_Plane plane(*c + ON_3dVector(0, 0, *h), ON_3dVector::ZAxis);
  ON_Cone cone(plane, -*h, *r);
  ON_Brep* b = ON_BrepCone(cone, true);
  if (!b) return e.Fail("cono non valido");
  return Report(e, e.AddObject(b), "cono");
}

bool CmdTorus(Engine& e, TokenStream& ts) {
  auto c = e.ReadPoint(ts, "centro del toro");
  if (!c) return false;
  auto r1 = ReadRadius(e, ts, *c, "raggio maggiore");
  if (!r1) return false;
  auto r2 = e.ReadNumber(ts, "raggio minore");
  if (!r2) return false;
  if (*r2 <= e.Tolerance() || *r1 <= e.Tolerance()) return e.Fail("toro degenere");
  ON_Brep* b = ON_BrepTorus(ON_Torus(ON_Plane(*c, ON_3dVector::ZAxis), *r1, *r2));
  if (!b) return e.Fail("toro non valido");
  return Report(e, e.AddObject(b), "toro");
}

// Plane: superficie piana rettangolare da due angoli.
bool CmdPlane(Engine& e, TokenStream& ts) {
  auto a = e.ReadPoint(ts, "primo angolo del piano");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "altro angolo");
  if (!b) return false;
  const double dx = b->x - a->x, dy = b->y - a->y;
  if (std::fabs(dx) <= e.Tolerance() || std::fabs(dy) <= e.Tolerance()) return e.Fail("piano degenere");
  ON_Plane plane(ON_3dPoint(std::min(a->x, b->x), std::min(a->y, b->y), a->z), ON_3dVector::ZAxis);
  auto* ps = new ON_PlaneSurface(plane);
  ps->SetExtents(0, ON_Interval(0, std::fabs(dx)), true);
  ps->SetExtents(1, ON_Interval(0, std::fabs(dy)), true);
  ON_Brep* br = BrepFromSurface(ps);
  if (!br) return e.Fail("piano non valido");
  return Report(e, e.AddObject(br), "superficie piana");
}

// PlanarSrf: una superficie piana tagliata per ogni curva chiusa planare selezionata.
bool CmdPlanarSrf(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("PlanarSrf");
  if (sel.empty()) return false;
  int n = 0;
  for (const auto& id : sel) {
    const auto* o = e.Object(id);
    const ON_Curve* c = ON_Curve::Cast(o ? o->Geometry(nullptr) : nullptr);
    if (!c || !c->IsClosed()) continue;
    ON_Plane plane;
    if (!c->IsPlanar(&plane, e.Tolerance())) continue;
    ON_Brep* b = ON_BrepTrimmedPlane(plane, *c);
    if (b && !ON_UuidIsNil(e.AddObject(b))) n++;
  }
  if (!n) return e.Fail("nessuna curva chiusa planare selezionata");
  e.Out() << "  Create " << n << " superfici piane\n";
  return true;
}

// ExtrudeCrv: estrude le curve selezionate lungo Z del piano di costruzione.
// Opzioni: Solid=Yes/No (chiude le curve chiuse). Distanza: numero o punto.
bool CmdExtrudeCrv(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  const bool solid = YesNo(opts, "solid", YesNo(opts, "cap", false));
  auto sel = e.RequireSelection("ExtrudeCrv");
  if (sel.empty()) return false;
  auto h = ReadHeight(e, ts, ON_3dPoint::Origin);
  if (!h) return false;
  if (std::fabs(*h) <= e.Tolerance()) return e.Fail("distanza nulla");
  int n = 0;
  for (const auto& id : sel) {
    const auto* o = e.Object(id);
    const ON_Curve* c = ON_Curve::Cast(o ? o->Geometry(nullptr) : nullptr);
    if (!c) continue;
    ON_Plane cp;
    const bool planarXY = c->IsPlanar(&cp, e.Tolerance()) &&
                          std::fabs(std::fabs(cp.zaxis * ON_3dVector::ZAxis) - 1.0) < 1e-10;
    if (planarXY) {
      ON_Plane plane(c->PointAtStart(), ON_3dVector::ZAxis);
      ON_Extrusion* ex = ON_Extrusion::CreateFrom3dCurve(*c, &plane, *h, solid);
      if (ex && !ON_UuidIsNil(e.AddObject(ex))) { n++; continue; }
      delete ex;
    }
    // Caso generale: superficie somma curva + segmento verticale.
    ON_LineCurve path(ON_3dPoint::Origin, ON_3dPoint(0, 0, *h));
    ON_SumSurface* ss = ON_SumSurface::New();
    if (!ss->Create(*c, path)) { delete ss; continue; }
    ON_Brep* b = BrepFromSurface(ss);
    if (b && !ON_UuidIsNil(e.AddObject(b))) n++;
  }
  if (!n) return e.Fail("nessuna curva selezionata");
  e.Out() << "  Create " << n << " estrusioni\n";
  return true;
}

// Revolve: rivoluzione delle curve selezionate attorno a un asse (2 punti),
// angolo facoltativo in gradi (predefinito 360).
bool CmdRevolve(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("Revolve");
  if (sel.empty()) return false;
  auto a = e.ReadPoint(ts, "inizio dell'asse");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "fine dell'asse");
  if (!b) return false;
  double angle = 360.0;
  if (!ts.AtEnd() && !IsEnter(ts.Peek()) && !LooksLikePoint(ts.Peek()) &&
      !e.IsCommand(ts.Peek())) {
    auto v = e.ReadNumber(ts, "angolo di rivoluzione");
    if (!v) return false;
    angle = *v;
  }
  int n = 0;
  for (const auto& id : sel) {
    const auto* o = e.Object(id);
    const ON_Curve* c = ON_Curve::Cast(o ? o->Geometry(nullptr) : nullptr);
    if (!c) continue;
    auto* rev = new ON_RevSurface();
    rev->m_curve = c->DuplicateCurve();
    rev->m_axis = ON_Line(*a, *b);
    rev->m_angle = ON_Interval(0.0, angle * ON_PI / 180.0);
    rev->m_t = rev->m_angle;
    if (!rev->IsValid()) { delete rev; continue; }
    ON_Brep* br = ON_BrepRevSurface(rev, false, false);
    if (br && !ON_UuidIsNil(e.AddObject(br))) n++;
  }
  if (!n) return e.Fail("nessuna curva selezionata");
  e.Out() << "  Create " << n << " superfici di rivoluzione\n";
  return true;
}

// SubDBox: parallelepipedo SubD; opzioni XFaces, YFaces, ZFaces (predefinito 1).
bool CmdSubDBox(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto count = [&](const char* k) {
    return opts.count(k) ? std::max(1, std::atoi(opts[k].c_str())) : 1;
  };
  auto a = e.ReadPoint(ts, "primo angolo della base");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "altro angolo della base");
  if (!b) return false;
  auto h = ReadHeight(e, ts, *a);
  if (!h) return false;
  const double z0 = a->z, z1 = a->z + *h;
  ON_3dPoint corners[8] = {
    ON_3dPoint(a->x, a->y, z0), ON_3dPoint(b->x, a->y, z0), ON_3dPoint(b->x, b->y, z0), ON_3dPoint(a->x, b->y, z0),
    ON_3dPoint(a->x, a->y, z1), ON_3dPoint(b->x, a->y, z1), ON_3dPoint(b->x, b->y, z1), ON_3dPoint(a->x, b->y, z1)};
  ON_SubD* s = ON_SubD::CreateSubDBox(corners, ON_SubDEdgeTag::Smooth,
                                      count("xfaces"), count("yfaces"), count("zfaces"), nullptr);
  if (!s) return e.Fail("SubD non valido");
  return Report(e, e.AddObject(s), "parallelepipedo SubD");
}

}  // namespace

void RegisterCreateSurfaceSolidCommands(Engine& e) {
  e.Register({"Box", "Solidi", "Parallelepipedo (due angoli, altezza)", CmdBox});
  e.Register({"Sphere", "Solidi", "Sfera (centro, raggio)", CmdSphere});
  e.Register({"Cylinder", "Solidi", "Cilindro (centro, raggio, altezza)", CmdCylinder});
  e.Register({"Cone", "Solidi", "Cono (centro, raggio, altezza)", CmdCone});
  e.Register({"Torus", "Solidi", "Toro (centro, raggio maggiore, raggio minore)", CmdTorus});
  e.Register({"Plane", "Superfici", "Superficie piana da due angoli", CmdPlane});
  e.Register({"PlanarSrf", "Superfici", "Superficie da curve chiuse planari", CmdPlanarSrf});
  e.Register({"ExtrudeCrv", "Superfici", "Estrusione rettilinea di curve (Solid)", CmdExtrudeCrv});
  e.Register({"Revolve", "Superfici", "Rivoluzione di curve attorno a un asse", CmdRevolve});
  e.Register({"SubDBox", "SubD", "Parallelepipedo SubD", CmdSubDBox});
}

}  // namespace cad
