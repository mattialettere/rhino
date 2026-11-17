// Comandi di modifica che chiedono di cliccare sugli oggetti (come in Rhino 8):
// Trim, Extend, Fillet, Chamfer (curve) e Cap, Shell, BooleanSplit (solidi).
// Le intersezioni sono calcolate con OpenCASCADE; il risultato resta del tipo
// originale quando possibile (una linea tagliata resta una linea, un arco un arco).
#include "commands/Engine.h"
#include "kernel/OcctConvert.h"

#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
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
#include <memory>

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

}  // namespace

void RegisterCurveEditCommands(Engine& e) {
  auto reg = [&](const char* n, const char* cat, const char* d, CommandFn f) { e.Register(CommandInfo{n, cat, d, f}); };
  reg("Trim", "Modifica", "Taglia le curve con gli oggetti selezionati: clic sulla parte da eliminare", CmdTrim);
  reg("Extend", "Modifica", "Estende le curve fino agli oggetti limite selezionati", CmdExtend);
  reg("Fillet", "Curve", "Raccordo ad arco tra due linee (Radius=)", CmdFillet);
  reg("Chamfer", "Curve", "Smusso tra due linee (Distances=a,b)", CmdChamfer);
  reg("Cap", "Solidi", "Chiude i fori piani di superfici e polisuperfici", CmdCap);
  reg("Shell", "Solidi", "Svuota un solido togliendo le facce cliccate (Thickness=)", CmdShell);
  reg("BooleanSplit", "Solidi", "Divide il primo solido con gli altri (parte dentro e fuori)", CmdBooleanSplit);
}

}  // namespace cad
