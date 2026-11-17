// Comandi di modifica che chiedono di cliccare sugli oggetti (come in Rhino 8):
// Trim, Extend, Fillet, Chamfer (curve) e Cap, Shell, BooleanSplit (solidi).
// Le intersezioni sono calcolate con OpenCASCADE; il risultato resta del tipo
// originale quando possibile (una linea tagliata resta una linea, un arco un arco).
#include "commands/Engine.h"
#include "kernel/OcctConvert.h"

#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Section.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepProj_Projection.hxx>
#include <gp_Pln.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRep_Tool.hxx>
#include <GeomAPI_ExtremaCurveCurve.hxx>
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <ShapeAnalysis_FreeBounds.hxx>
#include <ShapeFix_Solid.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopoDS_Shell.hxx>
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>

namespace cad {
namespace {

const ON_Geometry* Geo(Engine& e, const ON_UUID& id) {
  const auto* o = e.Object(id);
  return o ? o->Geometry(nullptr) : nullptr;
}

ON_3dmObjectAttributes Attrs(Engine& e, const ON_UUID& id) {
  return *e.Object(id)->Attributes(&ON_3dmObjectAttributes::DefaultAttributes);
}

// Parametro del punto della curva piu' vicino a p.
bool ClosestParam(const ON_Curve& c, const ON_3dPoint& p, double* t, double* dist) {
  ON_NurbsCurve nc;
  if (!c.GetNurbForm(nc)) return false;
  Handle(Geom_BSplineCurve) h = occt::ToOcct(nc);
  double tn = nc.Domain()[0];
  double best = p.DistanceTo(nc.PointAtStart());
  if (p.DistanceTo(nc.PointAtEnd()) < best) { best = p.DistanceTo(nc.PointAtEnd()); tn = nc.Domain()[1]; }
  try {
    GeomAPI_ProjectPointOnCurve pr(gp_Pnt(p.x, p.y, p.z), h);
    if (pr.NbPoints() > 0 && pr.LowerDistance() < best) { best = pr.LowerDistance(); tn = pr.LowerDistanceParameter(); }
  } catch (const Standard_Failure&) {
  }
  double tc = tn;
  if (!c.GetCurveParameterFromNurbFormParameter(tn, &tc)) tc = tn;
  if (t) *t = tc;
  if (dist) *dist = best;
  return true;
}

struct CurveHit {
  ON_UUID id = ON_nil_uuid;
  double t = 0, dist = 1e300;
};

// Curva (visibile, non bloccata) piu' vicina al punto cliccato.
CurveHit NearestCurve(Engine& e, const ON_3dPoint& p, const ON_UUID* exclude = nullptr) {
  CurveHit best;
  for (const auto& id : e.AllObjectIds()) {
    if (exclude && ON_UuidCompare(id, *exclude) == 0) continue;
    const auto* o = e.Object(id);
    const auto* a = o->Attributes(nullptr);
    if (a && a->Mode() == ON::object_mode::locked_object) continue;
    const ON_Curve* c = ON_Curve::Cast(o->Geometry(nullptr));
    if (!c) continue;
    double t, d;
    if (ClosestParam(*c, p, &t, &d) && d < best.dist) { best.id = id; best.t = t; best.dist = d; }
  }
  return best;
}

// Parametri (della curva c) dei punti in cui c interseca gli oggetti di taglio.
std::vector<double> IntersectionParams(const ON_Curve& c, const std::vector<const ON_Geometry*>& cutters, double tol) {
  std::vector<double> out;
  ON_NurbsCurve nc;
  if (!c.GetNurbForm(nc)) return out;
  Handle(Geom_BSplineCurve) h = occt::ToOcct(nc);
  std::vector<double> tn;
  for (const ON_Geometry* g : cutters) {
    try {
      if (const ON_Curve* cc = ON_Curve::Cast(g)) {
        ON_NurbsCurve n2;
        if (!cc->GetNurbForm(n2)) continue;
        GeomAPI_ExtremaCurveCurve ex(h, occt::ToOcct(n2));
        if (ex.Extrema().IsParallel()) continue;
        for (int i = 1; i <= ex.NbExtrema(); i++)
          if (ex.Distance(i) <= tol) {
            double u1, u2;
            ex.Parameters(i, u1, u2);
            tn.push_back(u1);
          }
      } else {
        TopoDS_Shape s = occt::GeometryToShape(*g, tol);
        if (s.IsNull()) continue;
        TopoDS_Edge edge = BRepBuilderAPI_MakeEdge(h);
        BRepExtrema_DistShapeShape d(edge, s);
        if (!d.IsDone() || d.Value() > tol) continue;
        for (int i = 1; i <= d.NbSolution(); i++) {
          if (d.SupportTypeShape1(i) == BRepExtrema_IsOnEdge) {
            double u;
            d.ParOnEdgeS1(i, u);
            tn.push_back(u);
          } else {
            gp_Pnt q = d.PointOnShape1(i);
            GeomAPI_ProjectPointOnCurve pr(q, h);
            if (pr.NbPoints() > 0) tn.push_back(pr.LowerDistanceParameter());
          }
        }
      }
    } catch (const Standard_Failure&) {
    }
  }
  for (double u : tn) {
    double t = u;
    if (!c.GetCurveParameterFromNurbFormParameter(u, &t)) t = u;
    out.push_back(t);
  }
  std::sort(out.begin(), out.end());
  const double eps = c.Domain().Length() * 1e-9;
  out.erase(std::unique(out.begin(), out.end(), [&](double a, double b) { return std::fabs(a - b) <= eps; }), out.end());
  return out;
}

ON_Curve* SubCurve(const ON_Curve& c, double a, double b) {
  if (b - a <= c.Domain().Length() * 1e-9) return nullptr;
  ON_Curve* d = c.DuplicateCurve();
  if (!d->Trim(ON_Interval(a, b))) { delete d; return nullptr; }
  return d;
}

std::vector<const ON_Geometry*> CutterGeometry(Engine& e, const std::vector<ON_UUID>& ids, const ON_UUID& exclude) {
  std::vector<const ON_Geometry*> g;
  for (const auto& id : ids)
    if (ON_UuidCompare(id, exclude) != 0)
      if (const ON_Geometry* x = Geo(e, id)) g.push_back(x);
  return g;
}

// --- Trim -------------------------------------------------------------------
// Selezione = oggetti di taglio; ogni punto indica la parte di curva da eliminare.
bool TrimAt(Engine& e, const std::vector<ON_UUID>& cutters, const ON_3dPoint& p) {
  CurveHit hit = NearestCurve(e, p);
  if (ON_UuidIsNil(hit.id)) return e.Fail("nessuna curva da tagliare vicino al punto");
  const ON_Curve* c = ON_Curve::Cast(Geo(e, hit.id));
  auto params = IntersectionParams(*c, CutterGeometry(e, cutters, hit.id), e.Tolerance());
  const ON_Interval dom = c->Domain();
  const double eps = dom.Length() * 1e-7;
  std::vector<double> inner;
  for (double t : params) if (t > dom[0] + eps && t < dom[1] - eps) inner.push_back(t);
  const bool closed = c->IsClosed();
  if (closed) {
    // le intersezioni sul punto di chiusura contano anche per le curve chiuse
    for (double t : params) if (t <= dom[0] + eps || t >= dom[1] - eps) { inner.push_back(dom[0]); break; }
    std::sort(inner.begin(), inner.end());
  }
  if (inner.empty() || (closed && inner.size() < 2))
    return e.Fail("la curva non interseca gli oggetti di taglio");
  std::vector<ON_Curve*> keep;
  const double t = hit.t;
  if (!closed) {
    double lo = dom[0], hi = dom[1];
    for (double s : inner) { if (s <= t) lo = s; else { hi = s; break; } }
    if (lo > dom[0]) if (auto* a = SubCurve(*c, dom[0], lo)) keep.push_back(a);
    if (hi < dom[1]) if (auto* b = SubCurve(*c, hi, dom[1])) keep.push_back(b);
  } else {
    // si tiene l'arco "dall'altra parte" e si ricompone attraverso il punto di chiusura
    double lo = inner.back(), hi = inner.front();
    bool wraps = true;
    for (size_t i = 0; i + 1 < inner.size(); i++)
      if (inner[i] <= t && t < inner[i + 1]) { lo = inner[i]; hi = inner[i + 1]; wraps = false; }
    if (wraps) {
      if (auto* a = SubCurve(*c, hi, lo)) keep.push_back(a);
    } else {
      ON_SimpleArray<const ON_Curve*> parts;
      std::unique_ptr<ON_Curve> a(SubCurve(*c, hi, dom[1])), b(SubCurve(*c, dom[0], lo));
      if (a) parts.Append(a.get());
      if (b) parts.Append(b.get());
      ON_SimpleArray<ON_Curve*> joined;
      ON_JoinCurves(parts, joined, e.Tolerance(), false, nullptr);
      for (int i = 0; i < joined.Count(); i++) keep.push_back(joined[i]);
    }
  }
  ON_3dmObjectAttributes a = Attrs(e, hit.id);
  const bool wasSelected = e.IsSelected(hit.id);
  e.DeleteObject(hit.id);
  for (ON_Curve* k : keep) {
    ON_UUID nid = e.AddObject(k, &a);
    if (wasSelected) e.Select(nid);  // un oggetto di taglio tagliato resta oggetto di taglio
  }
  e.Out() << "  Curva tagliata\n";
  return true;
}

bool CmdTrim(Engine& e, TokenStream& ts) {
  auto cutters = e.RequireSelection("Trim", "seleziona gli oggetti di taglio, poi Invio");
  if (cutters.empty()) return false;
  int n = 0;
  do {
    auto p = e.ReadPick(ts, "clicca la parte di curva da eliminare");
    if (!p) return false;
    if (!TrimAt(e, e.Selection(), *p)) return false;
    n++;
    // Nella finestra ogni clic conclude il comando (Invio lo ripete); negli script si continua.
    if (e.interactive && ts.AtEnd()) break;
  } while (e.MoreInput(ts));
  e.SkipEnter(ts);
  return n > 0;
}

// --- Extend -----------------------------------------------------------------
bool ExtendAt(Engine& e, const std::vector<ON_UUID>& bounds, const ON_3dPoint& p) {
  CurveHit hit = NearestCurve(e, p);
  if (ON_UuidIsNil(hit.id)) return e.Fail("nessuna curva da estendere vicino al punto");
  const ON_Curve* c = ON_Curve::Cast(Geo(e, hit.id));
  if (c->IsClosed()) return e.Fail("una curva chiusa non si puo' estendere");
  const ON_Interval dom = c->Domain();
  const bool atEnd = hit.t > dom.Mid();
  const auto cutters = CutterGeometry(e, bounds, hit.id);
  for (double factor : {1.0, 10.0, 100.0}) {
    std::unique_ptr<ON_Curve> ext(c->DuplicateCurve());
    const double L = dom.Length() * factor;
    ON_Interval nd = atEnd ? ON_Interval(dom[0], dom[1] + L) : ON_Interval(dom[0] - L, dom[1]);
    if (const auto* arc = ON_ArcCurve::Cast(c)) {
      // un arco si estende al massimo fino al cerchio quasi completo
      const double room = (2.0 * ON_PI - arc->m_arc.AngleRadians()) * 0.999;
      const double k = dom.Length() / arc->m_arc.AngleRadians();
      nd = atEnd ? ON_Interval(dom[0], dom[1] + room * k) : ON_Interval(dom[0] - room * k, dom[1]);
    }
    if (!ext->Extend(nd)) return e.Fail("questa curva non si puo' estendere");
    // parametri della curva estesa in cui finisce l'originale
    double t0, t1;
    ClosestParam(*ext, c->PointAtStart(), &t0, nullptr);
    ClosestParam(*ext, c->PointAtEnd(), &t1, nullptr);
    const double eps = ext->Domain().Length() * 1e-7;
    auto params = IntersectionParams(*ext, cutters, e.Tolerance());
    double target = ON_UNSET_VALUE;
    if (atEnd) {
      for (double s : params) if (s > t1 + eps) { target = s; break; }
    } else {
      for (auto it = params.rbegin(); it != params.rend(); ++it) if (*it < t0 - eps) { target = *it; break; }
    }
    if (target == ON_UNSET_VALUE) continue;
    ON_Curve* r = atEnd ? SubCurve(*ext, ext->Domain()[0] < t0 ? t0 : ext->Domain()[0], target)
                        : SubCurve(*ext, target, t1);
    if (!r) continue;
    ON_3dmObjectAttributes a = Attrs(e, hit.id);
    e.DeleteObject(hit.id);
    e.AddObject(r, &a);
    e.Out() << "  Curva estesa\n";
    return true;
  }
  return e.Fail("la curva estesa non incontra gli oggetti limite");
}

bool CmdExtend(Engine& e, TokenStream& ts) {
  auto bounds = e.RequireSelection("Extend", "seleziona gli oggetti limite, poi Invio");
  if (bounds.empty()) return false;
  int n = 0;
  do {
    auto p = e.ReadPick(ts, "clicca la curva vicino all'estremo da estendere");
    if (!p) return false;
    if (!ExtendAt(e, e.Selection(), *p)) return false;
    n++;
    if (e.interactive && ts.AtEnd()) break;
  } while (e.MoreInput(ts));
  e.SkipEnter(ts);
  return n > 0;
}

// --- Fillet e Chamfer di curve (per ora: segmenti di linea) -----------------
struct Corner {
  ON_UUID id[2];
  ON_Line line[2];
  ON_3dPoint P;        // intersezione delle rette
  ON_3dVector d[2];    // dal vertice verso la parte da tenere
  ON_3dPoint farEnd[2];
};

bool LineOf(const ON_Curve* c, ON_Line& l) {
  if (!c) return false;
  if (const auto* lc = ON_LineCurve::Cast(c)) { l = lc->m_line; return true; }
  if (c->IsLinear()) { l = ON_Line(c->PointAtStart(), c->PointAtEnd()); return true; }
  return false;
}

bool ReadCorner(Engine& e, TokenStream& ts, Corner& k) {
  auto p1 = e.ReadPick(ts, "clicca la prima curva");
  if (!p1) return false;
  CurveHit h1 = NearestCurve(e, *p1);
  if (ON_UuidIsNil(h1.id)) return e.Fail("nessuna curva vicino al primo punto");
  auto p2 = e.ReadPick(ts, "clicca la seconda curva");
  if (!p2) return false;
  CurveHit h2 = NearestCurve(e, *p2, &h1.id);
  if (ON_UuidIsNil(h2.id)) return e.Fail("nessuna seconda curva vicino al punto");
  const ON_3dPoint picks[2] = {*p1, *p2};
  k.id[0] = h1.id; k.id[1] = h2.id;
  for (int i = 0; i < 2; i++)
    if (!LineOf(ON_Curve::Cast(Geo(e, k.id[i])), k.line[i]))
      return e.Fail("per ora Fillet e Chamfer funzionano solo tra segmenti di linea");
  double a, b;
  if (!ON_Intersect(k.line[0], k.line[1], &a, &b)) return e.Fail("le linee sono parallele");
  const ON_3dPoint A = k.line[0].PointAt(a), B = k.line[1].PointAt(b);
  if (A.DistanceTo(B) > e.Tolerance() * 10) return e.Fail("le linee non sono complanari");
  k.P = 0.5 * (A + B);
  for (int i = 0; i < 2; i++) {
    const ON_Line& l = k.line[i];
    ON_3dVector d = l.ClosestPointTo(picks[i]) - k.P;
    if (d.Length() < e.Tolerance()) d = (l.from.DistanceTo(k.P) > l.to.DistanceTo(k.P) ? l.from : l.to) - k.P;
    d.Unitize();
    k.d[i] = d;
    k.farEnd[i] = ((l.from - k.P) * d > (l.to - k.P) * d) ? l.from : l.to;
  }
  return true;
}

// Sostituisce le due linee accorciandole/allungandole fino a T1 e T2.
bool ApplyCorner(Engine& e, const Corner& k, const ON_3dPoint T[2], ON_Curve* middle) {
  for (int i = 0; i < 2; i++)
    if ((k.farEnd[i] - k.P) * k.d[i] < (T[i] - k.P) * k.d[i] - e.Tolerance()) {
      delete middle;
      return e.Fail("le curve sono troppo corte per questo raggio o distanza");
    }
  for (int i = 0; i < 2; i++) {
    ON_3dmObjectAttributes a = Attrs(e, k.id[i]);
    const bool fromIsFar = k.line[i].from.DistanceTo(k.farEnd[i]) < k.line[i].to.DistanceTo(k.farEnd[i]);
    ON_Line nl = fromIsFar ? ON_Line(k.farEnd[i], T[i]) : ON_Line(T[i], k.farEnd[i]);
    e.DeleteObject(k.id[i]);
    if (nl.Length() > e.Tolerance()) e.AddObject(new ON_LineCurve(nl), &a);
    if (i == 0 && middle) e.AddObject(middle, &a);
  }
  return true;
}

double g_filletRadius = 1.0;
double g_chamferDist[2] = {1.0, 1.0};

bool CmdFillet(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  if (opts.count("radius")) g_filletRadius = std::atof(opts["radius"].c_str());
  const double r = g_filletRadius;
  if (r < 0) return e.Fail("raggio non valido");
  Corner k;
  if (!ReadCorner(e, ts, k)) return false;
  const double cosT = std::clamp(k.d[0] * k.d[1], -1.0, 1.0);
  const double theta = std::acos(cosT);
  if (theta < 1e-6 || theta > ON_PI - 1e-6) return e.Fail("le linee sono allineate");
  ON_3dPoint T[2] = {k.P, k.P};
  ON_Curve* arc = nullptr;
  if (r > 0) {
    const double t = r / std::tan(theta / 2);
    T[0] = k.P + k.d[0] * t;
    T[1] = k.P + k.d[1] * t;
    ON_3dVector bis = k.d[0] + k.d[1];
    bis.Unitize();
    const ON_3dPoint C = k.P + bis * (r / std::sin(theta / 2));
    const ON_3dPoint M = C - bis * r;
    arc = new ON_ArcCurve(ON_Arc(T[0], M, T[1]));
  }
  if (!ApplyCorner(e, k, T, arc)) return false;
  e.Out() << "  Raccordo raggio " << r << "\n";
  return true;
}

bool CmdChamfer(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  if (opts.count("distances")) {
    const std::string v = opts["distances"];
    const auto comma = v.find(',');
    g_chamferDist[0] = std::atof(v.substr(0, comma).c_str());
    g_chamferDist[1] = comma == std::string::npos ? g_chamferDist[0] : std::atof(v.substr(comma + 1).c_str());
  }
  Corner k;
  if (!ReadCorner(e, ts, k)) return false;
  ON_3dPoint T[2] = {k.P + k.d[0] * g_chamferDist[0], k.P + k.d[1] * g_chamferDist[1]};
  ON_Curve* mid = T[0].DistanceTo(T[1]) > e.Tolerance() ? new ON_LineCurve(T[0], T[1]) : nullptr;
  if (!ApplyCorner(e, k, T, mid)) return false;
  e.Out() << "  Smusso " << g_chamferDist[0] << ", " << g_chamferDist[1] << "\n";
  return true;
}

// --- Cap ----------------------------------------------------------------------
bool CmdCap(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("Cap", "seleziona le superfici o polisuperfici da chiudere, poi Invio");
  if (sel.empty()) return false;
  const double tol = e.Tolerance();
  int capped = 0, holes = 0;
  for (const auto& id : sel) {
    const ON_Geometry* g = Geo(e, id);
    if (!g || ON_Curve::Cast(g) || ON_Mesh::Cast(g) || ON_Point::Cast(g)) continue;
    TopoDS_Shape s = occt::GeometryToShape(*g, tol);
    if (s.IsNull()) continue;
    ShapeAnalysis_FreeBounds fb(s, tol * 10, Standard_False, Standard_False);
    TopoDS_Compound wires = fb.GetClosedWires();
    BRepBuilderAPI_Sewing sew(tol * 10);
    sew.Add(s);
    int n = 0;
    for (TopExp_Explorer ex(wires, TopAbs_WIRE); ex.More(); ex.Next()) {
      BRepBuilderAPI_MakeFace mf(TopoDS::Wire(ex.Current()), Standard_True);  // solo fori piani, come Rhino
      if (mf.IsDone()) { sew.Add(mf.Face()); n++; }
    }
    if (!n) continue;
    sew.Perform();
    TopoDS_Shape r = sew.SewedShape();
    for (TopExp_Explorer ex(r, TopAbs_SHELL); ex.More(); ex.Next()) {
      BRepBuilderAPI_MakeSolid ms(TopoDS::Shell(ex.Current()));
      if (ms.IsDone()) {
        ShapeFix_Solid fix(ms.Solid());
        fix.Perform();
        r = fix.Solid();
      }
      break;
    }
    ON_Brep* b = occt::ShapeToBrep(r, tol);
    if (!b) continue;
    ON_3dmObjectAttributes a = Attrs(e, id);
    e.DeleteObject(id);
    e.AddObject(b, &a);
    capped++;
    holes += n;
  }
  if (!capped) return e.Fail("nessun foro piano da chiudere");
  e.Out() << "  Chiusi " << holes << " fori su " << capped << " oggetti\n";
  return true;
}

// --- Shell --------------------------------------------------------------------
double g_shellThickness = 1.0;

bool CmdShell(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  if (opts.count("thickness")) g_shellThickness = std::atof(opts["thickness"].c_str());
  auto sel = e.RequireSelection("Shell", "seleziona il solido, poi Invio");
  if (sel.empty()) return false;
  if (sel.size() != 1) return e.Fail("seleziona un solo solido");
  const ON_Geometry* g = Geo(e, sel[0]);
  TopoDS_Shape s = g ? occt::GeometryToShape(*g, e.Tolerance()) : TopoDS_Shape();
  bool solid = false;
  for (TopExp_Explorer ex(s, TopAbs_SOLID); ex.More(); ex.Next()) { s = ex.Current(); solid = true; break; }
  if (!solid) return e.Fail("Shell richiede un solido chiuso");
  TopTools_ListOfShape remove;
  std::vector<TopoDS_Shape> chosen;
  while (e.MoreInput(ts)) {
    auto p = e.ReadPick(ts, "clicca le facce da rimuovere (Invio per finire)");
    if (!p) return false;
    TopoDS_Shape v = BRepBuilderAPI_MakeVertex(gp_Pnt(p->x, p->y, p->z));
    double best = 1e300;
    TopoDS_Shape face;
    for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next()) {
      BRepExtrema_DistShapeShape d(v, ex.Current());
      if (d.IsDone() && d.Value() < best) { best = d.Value(); face = ex.Current(); }
    }
    if (face.IsNull()) continue;
    bool dup = false;
    for (auto& f : chosen) dup = dup || f.IsSame(face);
    if (!dup) { chosen.push_back(face); remove.Append(face); }
  }
  e.SkipEnter(ts);
  if (remove.IsEmpty()) return e.Fail("nessuna faccia da rimuovere");
  try {
    BRepOffsetAPI_MakeThickSolid mk;
    mk.MakeThickSolidByJoin(s, remove, -g_shellThickness, e.Tolerance());
    mk.Build();
    if (!mk.IsDone()) return e.Fail("Shell non riuscito");
    ON_Brep* b = occt::ShapeToBrep(mk.Shape(), e.Tolerance());
    if (!b) return e.Fail("Shell: risultato vuoto");
    ON_3dmObjectAttributes a = Attrs(e, sel[0]);
    e.DeleteObject(sel[0]);
    e.AddObject(b, &a);
  } catch (const Standard_Failure&) {
    return e.Fail("Shell non riuscito");
  }
  e.Out() << "  Guscio di spessore " << g_shellThickness << "\n";
  return true;
}

// --- BooleanSplit ---------------------------------------------------------------
// Primo oggetto selezionato diviso dagli altri: parte fuori + parte dentro.
bool CmdBooleanSplit(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("BooleanSplit", "seleziona il solido da dividere e poi i solidi di taglio, poi Invio");
  if (sel.empty()) return false;
  if (sel.size() < 2) return e.Fail("seleziona prima il solido da dividere, poi quelli di taglio");
  const double tol = e.Tolerance();
  TopoDS_Shape a = occt::GeometryToShape(*Geo(e, sel[0]), tol);
  TopTools_ListOfShape args, tools;
  args.Append(a);
  for (size_t i = 1; i < sel.size(); i++)
    if (const ON_Geometry* g = Geo(e, sel[i])) tools.Append(occt::GeometryToShape(*g, tol));
  BRepAlgoAPI_Cut cut;
  cut.SetArguments(args);
  cut.SetTools(tools);
  cut.Build();
  BRepAlgoAPI_Common common;
  common.SetArguments(args);
  common.SetTools(tools);
  common.Build();
  if (!cut.IsDone() || !common.IsDone()) return e.Fail("BooleanSplit non riuscito");
  ON_3dmObjectAttributes at = Attrs(e, sel[0]);
  int n = 0;
  for (const TopoDS_Shape* r : {&cut.Shape(), &common.Shape()})
    for (TopExp_Explorer ex(*r, TopAbs_SOLID); ex.More(); ex.Next())
      if (ON_Brep* b = occt::ShapeToBrep(ex.Current(), tol)) { e.AddObject(b, &at); n++; }
  if (n < 2) return e.Fail("gli oggetti di taglio non dividono il solido");
  e.DeleteObject(sel[0]);
  e.Out() << "  Diviso in " << n << " solidi\n";
  return true;
}

// --- ExtractSrf / DupEdge ------------------------------------------------------
std::unique_ptr<ON_Brep> BrepOf(const ON_Geometry* g) {
  if (const auto* b = ON_Brep::Cast(g)) return std::unique_ptr<ON_Brep>(b->Duplicate());
  if (const auto* x = ON_Extrusion::Cast(g)) return std::unique_ptr<ON_Brep>(x->BrepForm());
  if (const auto* sf = ON_Surface::Cast(g)) return std::unique_ptr<ON_Brep>(sf->BrepForm());
  return nullptr;
}

// Faccia del B-rep piu' vicina al punto (distanza calcolata con OpenCASCADE faccia per faccia).
int NearestFace(const ON_Brep& b, const ON_3dPoint& p, double tol, double* dist = nullptr) {
  TopoDS_Shape v = BRepBuilderAPI_MakeVertex(gp_Pnt(p.x, p.y, p.z));
  double best = 1e300;
  int bf = -1;
  for (int fi = 0; fi < b.m_F.Count(); fi++) {
    std::unique_ptr<ON_Brep> one(b.DuplicateFace(fi, false));
    if (!one) continue;
    TopoDS_Shape s = occt::BrepToShape(*one, tol);
    if (s.IsNull()) continue;
    BRepExtrema_DistShapeShape d(v, s);
    if (d.IsDone() && d.Value() < best) { best = d.Value(); bf = fi; }
  }
  if (dist) *dist = best;
  return bf;
}

bool CmdExtractSrf(Engine& e, TokenStream& ts) {
  std::vector<ON_3dPoint> picks;
  while (e.MoreInput(ts)) {
    auto p = e.ReadPick(ts, "clicca le facce da estrarre (Invio = fine)");
    if (!p) return false;
    picks.push_back(*p);
  }
  e.SkipEnter(ts);
  if (picks.empty()) return e.Fail("nessuna faccia indicata");
  // per ogni clic: polisuperficie e faccia piu' vicine
  std::map<std::string, std::pair<ON_UUID, std::set<int>>> byObj;
  for (const ON_3dPoint& p : picks) {
    double best = 1e300;
    ON_UUID bid = ON_nil_uuid;
    int bf = -1;
    for (const auto& id : e.AllObjectIds()) {
      std::unique_ptr<ON_Brep> b = BrepOf(Geo(e, id));
      if (!b || b->m_F.Count() < 2) continue;
      ON_BoundingBox bb; b->GetTightBoundingBox(bb);
      if (bb.MinimumDistanceTo(p) > best) continue;
      double d;
      int fi = NearestFace(*b, p, e.Tolerance(), &d);
      if (fi >= 0 && d < best) { best = d; bid = id; bf = fi; }
    }
    if (bf < 0) continue;
    char k[64]; ON_UuidToString(bid, k);
    byObj[k].first = bid;
    byObj[k].second.insert(bf);
  }
  if (byObj.empty()) return e.Fail("nessuna polisuperficie vicino ai punti cliccati");
  int n = 0;
  for (auto& [k, v] : byObj) {
    std::unique_ptr<ON_Brep> b = BrepOf(Geo(e, v.first));
    ON_3dmObjectAttributes a = Attrs(e, v.first);
    ON_SimpleArray<int> rest;
    for (int fi = 0; fi < b->m_F.Count(); fi++) {
      if (v.second.count(fi)) {
        if (ON_Brep* f = b->DuplicateFace(fi, false)) { e.AddObject(f, &a); n++; }
      } else {
        rest.Append(fi);
      }
    }
    if (rest.Count()) {
      if (ON_Brep* r = b->SubBrep(rest.Count(), rest.Array())) e.AddObject(r, &a);
    }
    e.DeleteObject(v.first);
  }
  e.Out() << "  Estratte " << n << " superfici\n";
  return true;
}

bool CmdDupEdge(Engine& e, TokenStream& ts) {
  std::vector<ON_3dPoint> picks;
  while (e.MoreInput(ts)) {
    auto p = e.ReadPick(ts, "clicca gli spigoli da copiare (Invio = fine)");
    if (!p) return false;
    picks.push_back(*p);
  }
  e.SkipEnter(ts);
  int n = 0;
  for (const ON_3dPoint& p : picks) {
    double best = 1e300;
    std::unique_ptr<ON_Curve> bestC;
    ON_UUID owner = ON_nil_uuid;
    for (const auto& id : e.AllObjectIds()) {
      std::unique_ptr<ON_Brep> b = BrepOf(Geo(e, id));
      if (!b) continue;
      for (int ei = 0; ei < b->m_E.Count(); ei++) {
        double t, d;
        if (ClosestParam(b->m_E[ei], p, &t, &d) && d < best) {
          best = d;
          bestC.reset(b->m_E[ei].DuplicateCurve());
          owner = id;
        }
      }
    }
    if (!bestC) continue;
    ON_3dmObjectAttributes a = Attrs(e, owner);
    e.AddObject(bestC.release(), &a);
    n++;
  }
  if (!n) return e.Fail("nessuno spigolo vicino ai punti cliccati");
  e.Out() << "  Copiati " << n << " spigoli\n";
  return true;
}

// --- ExtrudeSrf ------------------------------------------------------------------
bool CmdExtrudeSrf(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  const bool solid = !(opts.count("solid") && opts["solid"] == "no");
  auto sel = e.RequireSelection("ExtrudeSrf", "seleziona le superfici da estrudere, poi Invio");
  if (sel.empty()) return false;
  auto h = e.ReadNumber(ts, "distanza di estrusione");
  if (!h) return false;
  if (std::fabs(*h) <= e.Tolerance()) return e.Fail("distanza nulla");
  int n = 0;
  for (const auto& id : sel) {
    const ON_Geometry* g = Geo(e, id);
    if (!g || ON_Curve::Cast(g)) continue;
    TopoDS_Shape s = occt::GeometryToShape(*g, e.Tolerance());
    if (s.IsNull()) continue;
    TopoDS_Shape r;
    try {
      if (solid) {
        BRepPrimAPI_MakePrism mk(s, gp_Vec(0, 0, *h));  // direzione: normale del piano di costruzione
        if (mk.IsDone()) r = mk.Shape();
      } else {
        // solo le pareti laterali: estrusione dei bordi
        ShapeAnalysis_FreeBounds fb(s, e.Tolerance() * 10, Standard_False, Standard_False);
        BRepPrimAPI_MakePrism mk(fb.GetClosedWires(), gp_Vec(0, 0, *h));
        if (mk.IsDone()) r = mk.Shape();
      }
    } catch (const Standard_Failure&) {
    }
    if (r.IsNull()) continue;
    ON_3dmObjectAttributes a = Attrs(e, id);
    for (TopExp_Explorer ex(r, solid ? TopAbs_SOLID : TopAbs_SHELL); ex.More(); ex.Next())
      if (ON_Brep* b = occt::ShapeToBrep(ex.Current(), e.Tolerance())) { e.AddObject(b, &a); n++; }
  }
  if (!n) return e.Fail("nessuna superficie estrusa");
  e.Out() << "  Estruse " << n << " superfici\n";
  return true;
}

// --- Contour e Section -------------------------------------------------------------
int SectionWithPlane(Engine& e, const std::vector<ON_UUID>& ids, const gp_Pln& pl) {
  int n = 0;
  for (const auto& id : ids) {
    const ON_Geometry* g = Geo(e, id);
    if (!g || ON_Curve::Cast(g)) continue;
    TopoDS_Shape s = occt::GeometryToShape(*g, e.Tolerance());
    if (s.IsNull()) continue;
    BRepAlgoAPI_Section sec(s, pl, Standard_False);
    sec.ComputePCurveOn1(Standard_False);
    sec.Approximation(Standard_True);
    sec.Build();
    if (!sec.IsDone()) continue;
    std::vector<ON_Curve*> crvs = occt::ShapeToCurves(sec.Shape(), e.Tolerance());
    if (crvs.empty()) continue;
    ON_SimpleArray<const ON_Curve*> in;
    for (auto* c : crvs) in.Append(c);
    ON_SimpleArray<ON_Curve*> joined;
    ON_JoinCurves(in, joined, e.Tolerance() * 10, false, nullptr);
    for (auto* c : crvs) delete c;
    ON_3dmObjectAttributes a = Attrs(e, id);
    for (int i = 0; i < joined.Count(); i++) { e.AddObject(joined[i], &a); n++; }
  }
  return n;
}

// Contour: piani perpendicolari alla direzione data, a distanza costante.
bool CmdContour(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("Contour", "seleziona gli oggetti da sezionare, poi Invio");
  if (sel.empty()) return false;
  auto a = e.ReadPoint(ts, "punto base delle curve di livello");
  if (!a) return false;
  e.lastPoint = *a;
  auto b = e.ReadPoint(ts, "direzione perpendicolare ai piani");
  if (!b) return false;
  auto step = e.ReadNumber(ts, "distanza tra le curve");
  if (!step) return false;
  ON_3dVector dir = *b - *a;
  if (!dir.Unitize() || *step <= e.Tolerance()) return e.Fail("direzione o distanza non valida");
  ON_BoundingBox bb;
  for (const auto& id : sel) if (const ON_Geometry* g = Geo(e, id)) { ON_BoundingBox t; g->GetTightBoundingBox(t); bb.Union(t); }
  double lo = 1e300, hi = -1e300;
  for (int i = 0; i < 8; i++) { const double d = (bb.Corner(i & 1, (i >> 1) & 1, (i >> 2) & 1) - *a) * dir; lo = std::min(lo, d); hi = std::max(hi, d); }
  int n = 0;
  const int k0 = int(std::ceil(lo / *step - 1e-9)), k1 = int(std::floor(hi / *step + 1e-9));
  if (k1 - k0 > 2000) return e.Fail("troppe curve: aumenta la distanza");
  for (int k = k0; k <= k1; k++) {
    const ON_3dPoint o = *a + dir * (k * *step);
    n += SectionWithPlane(e, sel, gp_Pln(gp_Pnt(o.x, o.y, o.z), gp_Dir(dir.x, dir.y, dir.z)));
  }
  if (!n) return e.Fail("nessuna curva di livello");
  e.Out() << "  Create " << n << " curve di livello\n";
  return true;
}

// Section: piano verticale (contiene l'asse Z del piano di costruzione) per i due punti.
bool CmdSection(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("Section", "seleziona gli oggetti da sezionare, poi Invio");
  if (sel.empty()) return false;
  auto a = e.ReadPoint(ts, "inizio della linea di sezione");
  if (!a) return false;
  e.lastPoint = *a;
  auto b = e.ReadPoint(ts, "fine della linea di sezione");
  if (!b) return false;
  ON_3dVector t = *b - *a;
  t.z = 0;
  if (!t.Unitize()) return e.Fail("linea di sezione nulla");
  const ON_3dVector nrm = ON_CrossProduct(t, ON_3dVector::ZAxis);
  const int n = SectionWithPlane(e, sel, gp_Pln(gp_Pnt(a->x, a->y, a->z), gp_Dir(nrm.x, nrm.y, nrm.z)));
  if (!n) return e.Fail("il piano di sezione non taglia gli oggetti");
  e.Out() << "  Create " << n << " curve di sezione\n";
  return true;
}

// --- Project --------------------------------------------------------------------------
// Curve selezionate proiettate (direzione Z del piano di costruzione) sulle superfici selezionate.
bool CmdProject(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("Project", "seleziona le curve e le superfici su cui proiettarle, poi Invio");
  if (sel.empty()) return false;
  std::vector<ON_UUID> crvs, srfs;
  for (const auto& id : sel) (ON_Curve::Cast(Geo(e, id)) ? crvs : srfs).push_back(id);
  if (crvs.empty() || srfs.empty()) return e.Fail("servono sia curve sia superfici");
  int n = 0;
  for (const auto& sid : srfs) {
    TopoDS_Shape target = occt::GeometryToShape(*Geo(e, sid), e.Tolerance());
    if (target.IsNull()) continue;
    for (const auto& cid : crvs) {
      TopoDS_Shape w = occt::CurveToShape(*ON_Curve::Cast(Geo(e, cid)));
      if (w.IsNull()) continue;
      try {
        BRepProj_Projection pr(w, target, gp_Dir(0, 0, 1));
        if (!pr.IsDone()) continue;
        ON_3dmObjectAttributes a = Attrs(e, cid);
        for (ON_Curve* c : occt::ShapeToCurves(pr.Shape(), e.Tolerance())) { e.AddObject(c, &a); n++; }
      } catch (const Standard_Failure&) {
      }
    }
  }
  if (!n) return e.Fail("nessuna proiezione: le curve non cadono sulle superfici");
  e.Out() << "  Create " << n << " curve proiettate\n";
  return true;
}

}  // namespace

void RegisterCurveEditCommands(Engine& e) {
  auto reg = [&](const char* n, const char* cat, const char* d, CommandFn f) { e.Register(CommandInfo{n, cat, d, f}); };
  reg("Trim", "Modifica", "Taglia le curve con gli oggetti selezionati: clic sulla parte da eliminare", CmdTrim);
  reg("Extend", "Modifica", "Estende le curve fino agli oggetti limite selezionati", CmdExtend);
  reg("Fillet", "Curve", "Raccordo ad arco tra due linee (Radius=)", CmdFillet);
  reg("Chamfer", "Curve", "Smusso tra due linee (Distances=a,b)", CmdChamfer);
  reg("Cap", "Solidi", "Chiude i fori piani di superfici e polisuperfici", CmdCap);
  reg("Shell", "Solidi", "Svuota un solido togliendo le facce cliccate (Thickness=)", CmdShell);
  reg("ExtractSrf", "Superfici", "Separa le facce cliccate da una polisuperficie", CmdExtractSrf);
  reg("DupEdge", "Curve", "Copia come curve gli spigoli cliccati", CmdDupEdge);
  reg("ExtrudeSrf", "Solidi", "Estrude superfici lungo Z del piano di costruzione (Solid=)", CmdExtrudeSrf);
  reg("Contour", "Curve", "Curve di livello a distanza costante", CmdContour);
  reg("Section", "Curve", "Curve di sezione con un piano verticale per due punti", CmdSection);
  reg("Project", "Curve", "Proietta le curve sulle superfici (direzione Z)", CmdProject);
  reg("BooleanSplit", "Solidi", "Divide il primo solido con gli altri (parte dentro e fuori)", CmdBooleanSplit);
}

}  // namespace cad
