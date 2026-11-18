// Superfici e solidi di Rhino 8, seconda parte. Con openNURBS: SrfPt, Plane3Pt,
// PlaneVertical, ExtrudeCrvToPoint, ExtrudeCrvAlongCrv, UntrimAll,
// ShrinkTrimmedSrf. Con OpenCASCADE: EdgeSrf, Patch, Pyramid, Wedge.
#include "commands/Engine.h"
#include "kernel/OcctConvert.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepOffsetAPI_MakeFilling.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRepPrimAPI_MakeWedge.hxx>
#include <BRep_Tool.hxx>
#include <GeomFill_BSplineCurves.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Ax2.hxx>
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

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

ON_Brep* BrepOf(ON_Surface* s) {
  ON_Brep* b = ON_Brep::New();
  ON_Surface* sp = s;
  if (!b->Create(sp)) { delete b; delete s; return nullptr; }
  return b;
}

// Superficie bilineare dai 4 angoli (a,b,c,d in giro).
ON_NurbsSurface* Bilinear(const ON_3dPoint& a, const ON_3dPoint& b, const ON_3dPoint& c, const ON_3dPoint& d) {
  auto* s = new ON_NurbsSurface(3, false, 2, 2, 2, 2);
  s->MakeClampedUniformKnotVector(0, 1.0);
  s->MakeClampedUniformKnotVector(1, 1.0);
  s->SetCV(0, 0, a);
  s->SetCV(1, 0, b);
  s->SetCV(1, 1, c);
  s->SetCV(0, 1, d);
  return s;
}

// SrfPt: 3 o 4 angoli (Invio dopo il terzo per una superficie triangolare).
bool CmdSrfPt(Engine& e, TokenStream& ts) {
  std::vector<ON_3dPoint> p;
  const char* prompts[4] = {"primo angolo", "secondo angolo", "terzo angolo", "quarto angolo (Invio per 3 angoli)"};
  for (int i = 0; i < 4; i++) {
    if (i == 3 && !e.MoreInput(ts)) break;
    auto q = e.ReadPoint(ts, prompts[i]);
    if (!q) return false;
    p.push_back(*q);
  }
  e.SkipEnter(ts);
  if (p.size() == 3) p.push_back(p[2]);  // lato degenere, come Rhino
  ON_Brep* b = BrepOf(Bilinear(p[0], p[1], p[2], p[3]));
  if (!b) return e.Fail("superficie non valida");
  return Created(e, e.AddObject(b), "superficie da punti");
}

// Plane3Pt: inizio e fine di un lato, poi la larghezza.
bool CmdPlane3Pt(Engine& e, TokenStream& ts) {
  auto a = e.ReadPoint(ts, "inizio del lato");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "fine del lato");
  if (!b) return false;
  auto c = e.ReadPoint(ts, "larghezza");
  if (!c) return false;
  ON_3dVector x = *b - *a;
  if (!x.Unitize()) return e.Fail("lato nullo");
  const ON_3dVector w = (*c - *a) - x * ((*c - *a) * x);
  if (w.Length() <= e.Tolerance()) return e.Fail("larghezza nulla");
  ON_Brep* br = BrepOf(Bilinear(*a, *b, *b + w, *a + w));
  if (!br) return e.Fail("piano non valido");
  return Created(e, e.AddObject(br), "piano");
}

// PlaneVertical: due punti alla base, poi l'altezza.
bool CmdPlaneVertical(Engine& e, TokenStream& ts) {
  auto a = e.ReadPoint(ts, "inizio della base");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "fine della base");
  if (!b) return false;
  auto h = e.ReadNumber(ts, "altezza");
  if (!h) return false;
  if (a->DistanceTo(*b) <= e.Tolerance() || std::fabs(*h) <= e.Tolerance()) return e.Fail("piano degenere");
  const ON_3dVector up(0, 0, *h);
  ON_Brep* br = BrepOf(Bilinear(*a, *b, *b + up, *a + up));
  if (!br) return e.Fail("piano non valido");
  return Created(e, e.AddObject(br), "piano verticale");
}

// ExtrudeCrvToPoint: le curve selezionate verso un punto (superfici rigate).
bool CmdExtrudeCrvToPoint(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("ExtrudeCrvToPoint", "seleziona le curve da estrudere, poi Invio");
  if (sel.empty()) return false;
  auto p = e.ReadPoint(ts, "punto di estrusione");
  if (!p) return false;
  int n = 0;
  for (const auto& id : sel) {
    const auto* c = ON_Curve::Cast(Geo(e, id));
    if (!c) continue;
    ON_NurbsCurve nc, apex;
    if (!c->GetNurbForm(nc)) continue;
    apex = nc;
    for (int i = 0; i < apex.CVCount(); i++) {
      if (apex.IsRational()) apex.SetCV(i, ON_4dPoint(p->x * apex.Weight(i), p->y * apex.Weight(i), p->z * apex.Weight(i), apex.Weight(i)));
      else apex.SetCV(i, *p);
    }
    auto* s = new ON_NurbsSurface();
    if (!s->CreateRuledSurface(nc, apex)) { delete s; continue; }
    if (ON_Brep* b = BrepOf(s)) { e.AddObject(b); n++; }
  }
  if (!n) return e.Fail("nessuna curva selezionata");
  e.Out() << "  Create " << n << " superfici\n";
  return true;
}

// Curva piu' vicina al punto cliccato, esclusi gli oggetti indicati.
const ON_Curve* NearestCurveTo(Engine& e, const ON_3dPoint& p, const std::vector<ON_UUID>& exclude, ON_UUID* idOut) {
  double best = 1e300;
  const ON_Curve* out = nullptr;
  for (const auto& id : e.AllObjectIds(false)) {
    bool skip = false;
    for (const auto& x : exclude) if (ON_UuidCompare(x, id) == 0) skip = true;
    if (skip) continue;
    const auto* c = ON_Curve::Cast(Geo(e, id));
    if (!c) continue;
    const ON_Interval dom = c->Domain();
    double d = 1e300;
    for (int i = 0; i <= 400; i++) d = std::min(d, c->PointAt(dom.ParameterAt(i / 400.0)).DistanceTo(p));
    if (d < best) { best = d; out = c; if (idOut) *idOut = id; }
  }
  return out;
}

// ExtrudeCrvAlongCrv: le curve selezionate traslate lungo la curva del percorso
// (clic vicino al percorso); la forma del profilo non ruota, come in Rhino.
bool CmdExtrudeCrvAlongCrv(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("ExtrudeCrvAlongCrv", "seleziona le curve da estrudere, poi Invio");
  if (sel.empty()) return false;
  auto p = e.ReadPick(ts, "clicca la curva del percorso");
  if (!p) return false;
  const ON_Curve* path = NearestCurveTo(e, *p, sel, nullptr);
  if (!path) return e.Fail("curva del percorso non trovata");
  // il percorso parte dall'estremo piu' vicino al clic
  std::unique_ptr<ON_Curve> pc(path->DuplicateCurve());
  if (p->DistanceTo(pc->PointAtEnd()) < p->DistanceTo(pc->PointAtStart())) pc->Reverse();
  int n = 0;
  for (const auto& id : sel) {
    const auto* c = ON_Curve::Cast(Geo(e, id));
    if (!c) continue;
    auto* s = new ON_SumSurface();
    if (!s->Create(*c, *pc)) { delete s; continue; }
    // ON_SumSurface somma i due punti: si toglie l'inizio del percorso
    s->m_basepoint = -ON_3dVector(pc->PointAtStart());
    if (ON_Brep* b = BrepOf(s)) { e.AddObject(b); n++; }
  }
  if (!n) return e.Fail("nessuna curva da estrudere");
  e.Out() << "  Create " << n << " superfici\n";
  return true;
}

// UntrimAll: superfici senza i tagli (una per faccia).
bool CmdUntrimAll(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("UntrimAll", "seleziona le superfici tagliate, poi Invio");
  if (sel.empty()) return false;
  int n = 0;
  for (const auto& id : sel) {
    const auto* b = ON_Brep::Cast(Geo(e, id));
    if (!b) continue;
    ON_3dmObjectAttributes at = *e.Object(id)->Attributes(&ON_3dmObjectAttributes::DefaultAttributes);
    std::vector<ON_Brep*> out;
    for (int fi = 0; fi < b->m_F.Count(); fi++) {
      const ON_Surface* s = b->m_F[fi].SurfaceOf();
      if (!s) continue;
      ON_Surface* d = s->DuplicateSurface();
      if (b->m_F[fi].m_bRev) d->Reverse(0);  // conserva il verso della normale
      if (ON_Brep* nb = BrepOf(d)) out.push_back(nb);
    }
    if (out.empty()) continue;
    e.DeleteObject(id);
    for (auto* nb : out) e.AddObject(nb, &at);
    n++;
  }
  if (!n) return e.Fail("nessuna superficie selezionata");
  e.Out() << "  Tolti i tagli da " << n << " oggetti\n";
  return true;
}

// ShrinkTrimmedSrf: riduce le superfici sottostanti ai bordi di taglio.
bool CmdShrinkTrimmedSrf(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("ShrinkTrimmedSrf", "seleziona le superfici tagliate, poi Invio");
  if (sel.empty()) return false;
  int n = 0;
  for (const auto& id : sel)
    if (auto* b = ON_Brep::Cast(e.EditableGeometry(id))) { b->ShrinkSurfaces(); n++; }
  if (!n) return e.Fail("nessuna superficie selezionata");
  e.Out() << "  Ridotte " << n << " superfici\n";
  return true;
}

#ifdef CAD_WITH_OCCT
// Lo stile di Coons di OCCT richiede almeno 4 poli: grado almeno 3.
Handle(Geom_BSplineCurve) Cubic(Handle(Geom_BSplineCurve) c) {
  if (!c.IsNull() && c->Degree() < 3) c->IncreaseDegree(3);
  return c;
}

// Curve selezionate in forma OCCT, ordinate e orientate testa-coda (per EdgeSrf).
bool OrderedLoop(Engine& e, const std::vector<const ON_Curve*>& in, std::vector<Handle(Geom_BSplineCurve)>& out) {
  std::vector<ON_NurbsCurve> nc(in.size());
  for (size_t i = 0; i < in.size(); i++) if (!in[i]->GetNurbForm(nc[i])) return false;
  const double tol = std::max(10 * e.Tolerance(), 1e-6);
  std::vector<bool> used(nc.size(), false);
  std::vector<ON_NurbsCurve> loop{nc[0]};
  used[0] = true;
  for (size_t k = 1; k < nc.size(); k++) {
    const ON_3dPoint end = loop.back().PointAtEnd();
    bool found = false;
    for (size_t i = 0; i < nc.size() && !found; i++) {
      if (used[i]) continue;
      if (nc[i].PointAtStart().DistanceTo(end) <= tol) { loop.push_back(nc[i]); used[i] = found = true; }
      else if (nc[i].PointAtEnd().DistanceTo(end) <= tol) { ON_NurbsCurve r = nc[i]; r.Reverse(); loop.push_back(r); used[i] = found = true; }
    }
    if (!found) return false;
  }
  for (auto& c : loop) out.push_back(Cubic(occt::ToOcct(c)));
  return true;
}

// EdgeSrf: superficie dai bordi (2, 3 o 4 curve), stile di Coons.
bool CmdEdgeSrf(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("EdgeSrf", "seleziona da 2 a 4 curve di bordo, poi Invio");
  if (sel.empty()) return false;
  std::vector<const ON_Curve*> crvs;
  for (const auto& id : sel) if (const auto* c = ON_Curve::Cast(Geo(e, id))) crvs.push_back(c);
  if (crvs.size() < 2 || crvs.size() > 4) return e.Fail("servono da 2 a 4 curve");
  Handle(Geom_BSplineSurface) srf;
  try {
    std::vector<Handle(Geom_BSplineCurve)> c;
    if (crvs.size() == 2) {
      // due lati opposti: stesso verso
      ON_NurbsCurve a, b;
      crvs[0]->GetNurbForm(a);
      crvs[1]->GetNurbForm(b);
      if (a.PointAtStart().DistanceTo(b.PointAtStart()) > a.PointAtStart().DistanceTo(b.PointAtEnd())) b.Reverse();
      GeomFill_BSplineCurves f(Cubic(occt::ToOcct(a)), Cubic(occt::ToOcct(b)), GeomFill_CoonsStyle);
      srf = f.Surface();
    } else {
      if (!OrderedLoop(e, crvs, c)) return e.Fail("le curve devono toccarsi negli estremi formando un contorno chiuso");
      if (c.size() == 3) srf = GeomFill_BSplineCurves(c[0], c[1], c[2], GeomFill_CoonsStyle).Surface();
      else srf = GeomFill_BSplineCurves(c[0], c[1], c[2], c[3], GeomFill_CoonsStyle).Surface();
    }
  } catch (const Standard_Failure& f) {
    return e.Fail(std::string("superficie dai bordi non riuscita: ") + (f.GetMessageString() ? f.GetMessageString() : f.DynamicType()->Name()));
  } catch (...) {
    return e.Fail("superficie dai bordi non riuscita");
  }
  if (srf.IsNull()) return e.Fail("superficie dai bordi non riuscita");
  ON_NurbsSurface* ns = occt::FromOcct(srf);
  if (!ns) return e.Fail("conversione non riuscita");
  ON_Brep* b = BrepOf(ns);
  if (!b) return e.Fail("superficie non valida");
  return Created(e, e.AddObject(b), "superficie dai bordi");
}

// Patch: superficie che passa per le curve e i punti selezionati; le curve
// chiuse (o il contorno formato da tutte) fanno da bordo.
bool CmdPatch(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("Patch", "seleziona curve e punti da interpolare, poi Invio");
  if (sel.empty()) return false;
  BRepOffsetAPI_MakeFilling mk(3, 15, 2, Standard_False, 1e-5, 1e-4, 1e-2, 0.1, 8, 9);
  int boundary = 0, interior = 0;
  for (const auto& id : sel) {
    const ON_Geometry* g = Geo(e, id);
    if (const auto* p = ON_Point::Cast(g)) {
      mk.Add(gp_Pnt(p->point.x, p->point.y, p->point.z));
      interior++;
    } else if (const auto* c = ON_Curve::Cast(g)) {
      TopoDS_Shape s = occt::CurveToShape(*c);
      for (TopExp_Explorer ex(s, TopAbs_EDGE); ex.More(); ex.Next()) {
        mk.Add(TopoDS::Edge(ex.Current()), GeomAbs_C0, Standard_True);
        boundary++;
      }
    }
  }
  if (!boundary) return e.Fail("serve almeno una curva di contorno");
  try { mk.Build(); } catch (...) {}
  if (!mk.IsDone()) return e.Fail("patch non riuscita (le curve devono formare un contorno chiuso)");
  ON_Brep* b = occt::ShapeToBrep(mk.Shape(), e.Tolerance());
  if (!b) return e.Fail("patch: conversione non riuscita");
  (void)opts;
  e.Out() << "  Patch con " << boundary << " curve e " << interior << " punti\n";
  return Created(e, e.AddObject(b), "patch");
}

// Pyramid: NumSides=, centro della base, vertice della base, altezza.
bool CmdPyramid(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  const int n = opts.count("numsides") ? std::atoi(opts["numsides"].c_str()) : 4;
  if (n < 3) return e.Fail("servono almeno 3 lati");
  auto c = e.ReadPoint(ts, "centro della base");
  if (!c) return false;
  auto v = e.ReadPoint(ts, "vertice della base");
  if (!v) return false;
  auto h = e.ReadNumber(ts, "altezza");
  if (!h) return false;
  ON_3dVector r = *v - *c;
  r.z = 0;
  if (r.Length() <= e.Tolerance() || std::fabs(*h) <= e.Tolerance()) return e.Fail("piramide degenere");
  BRepBuilderAPI_MakePolygon poly;
  for (int i = 0; i < n; i++) {
    ON_3dVector q = r;
    q.Rotate(2 * ON_PI * i / n, ON_3dVector(0, 0, 1));
    const ON_3dPoint p = *c + q;
    poly.Add(gp_Pnt(p.x, p.y, p.z));
  }
  poly.Close();
  BRepOffsetAPI_ThruSections mk(Standard_True, Standard_True, e.Tolerance());
  mk.AddWire(poly.Wire());
  mk.AddVertex(BRepBuilderAPI_MakeVertex(gp_Pnt(c->x, c->y, c->z + *h)).Vertex());
  try { mk.Build(); } catch (...) {}
  if (!mk.IsDone()) return e.Fail("piramide non riuscita");
  ON_Brep* b = occt::ShapeToBrep(mk.Shape(), e.Tolerance());
  if (!b) return e.Fail("piramide: conversione non riuscita");
  return Created(e, e.AddObject(b), "piramide");
}

// Wedge: due angoli della base e altezza; la faccia superiore si riduce a uno
// spigolo sopra il lato iniziale (prisma triangolare).
bool CmdWedge(Engine& e, TokenStream& ts) {
  auto a = e.ReadPoint(ts, "primo angolo della base");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "angolo opposto della base");
  if (!b) return false;
  auto h = e.ReadNumber(ts, "altezza");
  if (!h) return false;
  const double dx = b->x - a->x, dy = b->y - a->y;
  if (std::fabs(dx) <= e.Tolerance() || std::fabs(dy) <= e.Tolerance() || *h <= e.Tolerance()) return e.Fail("cuneo degenere");
  const gp_Pnt o(std::min(a->x, b->x), std::min(a->y, b->y), a->z);
  BRepPrimAPI_MakeWedge mk(gp_Ax2(o, gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)), std::fabs(dx), std::fabs(dy), *h, 0.0);
  TopoDS_Shape s;
  try { s = mk.Shape(); } catch (...) { return e.Fail("cuneo non riuscito"); }
  ON_Brep* br = occt::ShapeToBrep(s, e.Tolerance());
  if (!br) return e.Fail("cuneo: conversione non riuscita");
  return Created(e, e.AddObject(br), "cuneo");
}
#endif

}  // namespace

void RegisterSurfaceMoreCommands(Engine& e) {
  auto reg = [&](const char* n, const char* cat, const char* d, CommandFn f) {
    CommandInfo i{n, cat, d, f};
    i.modifiesModel = true;
    e.Register(i);
  };
  reg("SrfPt", "Superfici", "Superficie da 3 o 4 angoli", CmdSrfPt);
  reg("Plane3Pt", "Superfici", "Piano da un lato e dalla larghezza", CmdPlane3Pt);
  reg("PlaneVertical", "Superfici", "Piano verticale (base e altezza)", CmdPlaneVertical);
  reg("ExtrudeCrvToPoint", "Superfici", "Estrude le curve fino a un punto", CmdExtrudeCrvToPoint);
  reg("ExtrudeCrvAlongCrv", "Superfici", "Estrude le curve lungo una curva", CmdExtrudeCrvAlongCrv);
  reg("UntrimAll", "Superfici", "Toglie tutti i tagli dalle superfici", CmdUntrimAll);
  reg("ShrinkTrimmedSrf", "Superfici", "Riduce le superfici ai bordi di taglio", CmdShrinkTrimmedSrf);
#ifdef CAD_WITH_OCCT
  reg("EdgeSrf", "Superfici", "Superficie da 2, 3 o 4 curve di bordo", CmdEdgeSrf);
  reg("Patch", "Superfici", "Superficie che passa per curve e punti", CmdPatch);
  reg("Pyramid", "Solidi", "Piramide (NumSides=, centro, vertice, altezza)", CmdPyramid);
  reg("Wedge", "Solidi", "Cuneo (due angoli, altezza)", CmdWedge);
#endif
}

}  // namespace cad
