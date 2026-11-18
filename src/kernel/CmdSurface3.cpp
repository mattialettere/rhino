// Superfici di Rhino 8, terza parte (OpenCASCADE): ExtrudeCrvTapered, Slab,
// ExtendSrf, Pull, Sweep2; trasformazioni Flow e ArraySrf.
#include "commands/Deform.h"
#include "commands/Engine.h"
#include "kernel/OcctConvert.h"

#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepAlgo_NormalProjection.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <BRepOffsetAPI_MakeFilling.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepOffsetAPI_MakeOffset.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRep_Tool.hxx>
#include <GeomLib.hxx>
#include <GeomAPI_PointsToBSpline.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <TColStd_HArray1OfReal.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <Geom2dAPI_Interpolate.hxx>
#include <Geom2d_BSplineCurve.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TColgp_HArray1OfPnt2d.hxx>
#include <Geom_BSplineSurface.hxx>
#include <Geom_BoundedSurface.hxx>
#include <ShapeAnalysis_FreeBounds.hxx>
#include <ShapeAnalysis_Surface.hxx>
#include <ShapeFix_Solid.hxx>
#include <ShapeFix_Shell.hxx>
#include <BRepLib.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Iterator.hxx>
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

ON_3dmObjectAttributes Attrs(Engine& e, const ON_UUID& id) {
  return *e.Object(id)->Attributes(&ON_3dmObjectAttributes::DefaultAttributes);
}

bool Contains(const std::vector<ON_UUID>& v, const ON_UUID& id) {
  return std::find(v.begin(), v.end(), id) != v.end();
}

bool YesOpt(const std::map<std::string, std::string>& o, const char* k, bool def) {
  auto it = o.find(k);
  if (it == o.end()) return def;
  return it->second == "yes" || it->second == "true" || it->second == "1" || it->second == "si";
}

double NumOpt(const std::map<std::string, std::string>& o, const char* k, double def) {
  auto it = o.find(k);
  if (it == o.end()) return def;
  try { return std::stod(it->second); } catch (...) { return def; }
}

// Wire con un edge per ogni tratto G1 della curva.
TopoDS_Wire WireOf(const ON_Curve& c, bool splitClosed = false) {
  std::vector<double> breaks{c.Domain()[0]};
  double t = c.Domain()[0];
  int hint = 0;
  while (c.GetNextDiscontinuity(ON::continuity::G1_locus_continuous, t, c.Domain()[1], &t, &hint))
    if (t > breaks.back()) breaks.push_back(t);
  if (breaks.back() < c.Domain()[1]) breaks.push_back(c.Domain()[1]);
  // curva chiusa in un solo tratto (cerchio): due edge, l'offset di OpenCASCADE e' piu' robusto
  if (splitClosed && breaks.size() == 2 && c.IsClosed()) breaks.insert(breaks.begin() + 1, c.Domain().ParameterAt(0.5));
  BRepBuilderAPI_MakeWire mw;
  for (size_t i = 0; i + 1 < breaks.size(); i++) {
    std::unique_ptr<ON_Curve> span(c.DuplicateCurve());
    if (!span->Trim(ON_Interval(breaks[i], breaks[i + 1]))) continue;
    TopoDS_Shape s = occt::CurveToShape(*span);
    if (!s.IsNull()) mw.Add(TopoDS::Edge(s));
  }
  return mw.IsDone() ? mw.Wire() : TopoDS_Wire();
}

// Chiude con facce piane i bordi liberi piani e prova a farne un solido.
TopoDS_Shape CapShape(const TopoDS_Shape& s, double tol) {
  ShapeAnalysis_FreeBounds fb(s, tol * 10, Standard_False, Standard_False);
  TopoDS_Compound wires = fb.GetClosedWires();
  BRepBuilderAPI_Sewing sew(tol * 10);
  sew.Add(s);
  int n = 0;
  for (TopExp_Explorer ex(wires, TopAbs_WIRE); ex.More(); ex.Next()) {
    BRepBuilderAPI_MakeFace mf(TopoDS::Wire(ex.Current()), Standard_True);
    if (mf.IsDone()) { sew.Add(mf.Face()); n++; }
  }
  if (!n) return s;
  sew.Perform();
  TopoDS_Shape r = sew.SewedShape();
  for (TopExp_Explorer ex(r, TopAbs_SHELL); ex.More(); ex.Next()) {
    BRepBuilderAPI_MakeSolid ms(TopoDS::Shell(ex.Current()));
    if (ms.IsDone()) {
      ShapeFix_Solid fix(ms.Solid());
      fix.Perform();
      return fix.Solid();
    }
    break;
  }
  return r;
}

bool AddShape(Engine& e, const TopoDS_Shape& s, const ON_3dmObjectAttributes& a) {
  if (s.IsNull()) return false;
  ON_Brep* b = occt::ShapeToBrep(s, e.Tolerance());
  if (!b) return false;
  e.AddObject(b, &a);
  return true;
}

// Superficie (o polisuperficie) piu' vicina al punto cliccato.
const ON_Geometry* NearestSurface(Engine& e, const ON_3dPoint& p, const std::vector<ON_UUID>& exclude, ON_UUID* idOut) {
  double best = 1e300;
  const ON_Geometry* out = nullptr;
  const TopoDS_Shape v = BRepBuilderAPI_MakeVertex(gp_Pnt(p.x, p.y, p.z)).Vertex();
  for (const auto& id : e.AllObjectIds(false)) {
    if (Contains(exclude, id)) continue;
    const ON_Geometry* g = Geo(e, id);
    if (!g || !(ON_Brep::Cast(g) || ON_Extrusion::Cast(g) || ON_Surface::Cast(g))) continue;
    try {
      TopoDS_Shape s = occt::GeometryToShape(*g, e.Tolerance());
      if (s.IsNull()) continue;
      BRepExtrema_DistShapeShape d(v, s);
      if (d.IsDone() && d.Value() < best) { best = d.Value(); out = g; if (idOut) *idOut = id; }
    } catch (...) {
    }
  }
  return out;
}

const ON_Curve* NearestCurve(Engine& e, const ON_3dPoint& p, const std::vector<ON_UUID>& exclude, ON_UUID* idOut = nullptr) {
  double best = 1e300;
  const ON_Curve* out = nullptr;
  for (const auto& id : e.AllObjectIds(false)) {
    if (Contains(exclude, id)) continue;
    const auto* c = ON_Curve::Cast(Geo(e, id));
    if (!c) continue;
    const ON_Interval d = c->Domain();
    for (int i = 0; i <= 400; i++) {
      const double dd = c->PointAt(d.ParameterAt(i / 400.0)).DistanceTo(p);
      if (dd < best) { best = dd; out = c; if (idOut) *idOut = id; }
    }
  }
  return out;
}

// Tabella della lunghezza d'arco di una curva (campionamento fitto).
struct ArcTable {
  const ON_Curve* c = nullptr;
  std::vector<double> t, s;
  explicit ArcTable(const ON_Curve& curve, int n = 2000) : c(&curve) {
    const ON_Interval d = curve.Domain();
    t.resize(n + 1);
    s.assign(n + 1, 0.0);
    ON_3dPoint prev = curve.PointAt(d[0]);
    t[0] = d[0];
    for (int i = 1; i <= n; i++) {
      t[i] = d.ParameterAt(double(i) / n);
      const ON_3dPoint q = curve.PointAt(t[i]);
      s[i] = s[i - 1] + q.DistanceTo(prev);
      prev = q;
    }
  }
  double Length() const { return s.back(); }
  double ParamAt(double len) const {
    len = std::clamp(len, 0.0, Length());
    const auto it = std::lower_bound(s.begin(), s.end(), len);
    const int i = std::max(1, int(it - s.begin()));
    const double seg = s[i] - s[i - 1];
    const double u = seg > 0 ? (len - s[i - 1]) / seg : 0.0;
    return t[i - 1] + u * (t[i] - t[i - 1]);
  }
  double LengthAt(double par) const {
    const auto it = std::lower_bound(t.begin(), t.end(), par);
    const int i = std::clamp(int(it - t.begin()), 1, int(t.size()) - 1);
    const double u = (par - t[i - 1]) / (t[i] - t[i - 1]);
    return s[i - 1] + std::clamp(u, 0.0, 1.0) * (s[i] - s[i - 1]);
  }
  // Parametro del punto della curva piu' vicino a p (campioni + raffinamento).
  double Closest(const ON_3dPoint& p) const {
    int bi = 0;
    double bd = 1e300;
    for (size_t i = 0; i < t.size(); i += 4) {
      const double d = c->PointAt(t[i]).DistanceTo(p);
      if (d < bd) { bd = d; bi = int(i); }
    }
    double a = t[std::max(0, bi - 4)], b = t[std::min(int(t.size()) - 1, bi + 4)];
    for (int k = 0; k < 60; k++) {  // sezione aurea
      const double m1 = a + (b - a) * 0.381966, m2 = a + (b - a) * 0.618034;
      if (c->PointAt(m1).DistanceTo(p) < c->PointAt(m2).DistanceTo(p)) b = m2; else a = m1;
    }
    return 0.5 * (a + b);
  }
};


// Superficie che passa esattamente per sezioni con la stessa struttura NURBS
// (stesso grado, nodi e pesi): ogni fila di punti di controllo si interpola
// in v con una cubica (periodica se le sezioni fanno il giro completo).
ON_Brep* SurfaceFromSections(const std::vector<ON_NurbsCurve>& secs, const std::vector<double>& v, bool periodic, double tol) {
  const size_t K = secs.size();
  if (K < 2 || v.size() != K + (periodic ? 1 : 0)) return nullptr;
  const ON_NurbsCurve& s0 = secs[0];
  const int nc = s0.CVCount();
  std::vector<Handle(Geom_BSplineCurve)> rows(nc);
  int templ = -1;
  for (int c = 0; c < nc; c++) {
    Handle(TColgp_HArray1OfPnt) pts = new TColgp_HArray1OfPnt(1, (int)K);
    double spread = 0;
    for (size_t k = 0; k < K; k++) {
      ON_3dPoint q;
      secs[k].GetCV(c, q);
      pts->SetValue(int(k) + 1, gp_Pnt(q.x, q.y, q.z));
      if (k) spread = std::max(spread, pts->Value(int(k) + 1).Distance(pts->Value(1)));
    }
    if (spread <= tol) continue;  // fila degenere (punto sull'asse): poli tutti uguali
    Handle(TColStd_HArray1OfReal) par = new TColStd_HArray1OfReal(1, (int)v.size());
    for (size_t k = 0; k < v.size(); k++) par->SetValue(int(k) + 1, v[k]);
    GeomAPI_Interpolate in(pts, par, periodic ? Standard_True : Standard_False, 1e-12);
    in.Perform();
    if (!in.IsDone()) return nullptr;
    Handle(Geom_BSplineCurve) r = in.Curve();
    if (r->IsPeriodic()) r->SetNotPeriodic();
    rows[c] = r;
    if (templ < 0) templ = c;
  }
  if (templ < 0) return nullptr;
  const Handle(Geom_BSplineCurve)& T = rows[templ];
  const int nv = T->NbPoles(), dv = T->Degree();
  TColStd_Array1OfReal flat(1, T->NbPoles() + dv + 1);
  T->KnotSequence(flat);
  auto* ns = new ON_NurbsSurface(3, s0.IsRational(), s0.Order(), dv + 1, nc, nv);
  for (int i = 0; i < s0.KnotCount(); i++) ns->m_knot[0][i] = s0.m_knot[i];
  for (int i = 0; i < ns->KnotCount(1); i++) ns->m_knot[1][i] = flat(i + 2);
  for (int c = 0; c < nc; c++) {
    const double w = s0.IsRational() ? s0.Weight(c) : 1.0;
    ON_3dPoint fixed;
    secs[0].GetCV(c, fixed);
    for (int j = 0; j < nv; j++) {
      ON_3dPoint q = fixed;
      if (!rows[c].IsNull()) { const gp_Pnt g = rows[c]->Pole(j + 1); q = ON_3dPoint(g.X(), g.Y(), g.Z()); }
      if (ns->IsRational()) ns->SetCV(c, j, ON_4dPoint(q.x * w, q.y * w, q.z * w, w));
      else ns->SetCV(c, j, q);
    }
  }
  ON_Brep* b = ON_Brep::New();
  ON_Surface* sp = ns;
  if (!b->Create(sp)) { delete b; delete ns; return nullptr; }
  return b;
}

// --- ExtrudeCrvTapered -----------------------------------------------------------
// Estrusione con angolo di sformo lungo z. DraftAngle= in gradi (positivo
// stringe verso l'interno), Solid=Yes chiude le curve chiuse.
bool CmdExtrudeCrvTapered(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("ExtrudeCrvTapered", "seleziona le curve da estrudere, poi Invio");
  if (sel.empty()) return false;
  opts.merge(e.ReadOptions(ts));
  auto h = e.ReadNumber(ts, "distanza di estrusione");
  if (!h) return false;
  if (std::fabs(*h) <= e.Tolerance()) return e.Fail("distanza nulla");
  const double angle = NumOpt(opts, "draftangle", 5.0) * ON_PI / 180.0;
  const bool solid = YesOpt(opts, "solid", true);
  int n = 0;
  for (const auto& id : sel) {
    const auto* c = ON_Curve::Cast(Geo(e, id));
    if (!c) continue;
    try {
      TopoDS_Shape result;
      const double dz = *h;
      if (c->IsClosed() && c->IsPlanar(nullptr, e.Tolerance())) {
        // curva chiusa piana: offset nel suo piano, traslato in alto, e loft rigato
        TopoDS_Wire w = WireOf(*c, true);
        BRepBuilderAPI_MakeFace mf(w, Standard_True);
        if (!mf.IsDone()) continue;
        BRepOffsetAPI_MakeOffset mo(mf.Face(), GeomAbs_Intersection);
        mo.Perform(-std::fabs(dz) * std::tan(angle));
        if (!mo.IsDone()) continue;
        TopoDS_Wire top;
        for (TopExp_Explorer ex(mo.Shape(), TopAbs_WIRE); ex.More(); ex.Next()) { top = TopoDS::Wire(ex.Current()); break; }
        if (top.IsNull()) continue;
        gp_Trsf tr;
        tr.SetTranslation(gp_Vec(0, 0, dz));
        top = TopoDS::Wire(top.Moved(TopLoc_Location(tr)));
        BRepOffsetAPI_ThruSections mk(solid ? Standard_True : Standard_False, Standard_True, e.Tolerance());
        mk.AddWire(w);
        mk.AddWire(top);
        mk.CheckCompatibility(Standard_True);
        mk.Build();
        if (!mk.IsDone()) continue;
        result = mk.Shape();
      } else {
        // curva aperta: copia in alto spostata verso sinistra (rispetto a z) e superficie rigata
        ON_NurbsCurve bot;
        if (!c->GetNurbForm(bot)) continue;
        if (bot.Degree() > 1) {
          for (int k = 1; k < 32; k++) {
            const double tk = bot.Domain().ParameterAt(k / 32.0);
            bool exists = false;
            for (int i = 0; i < bot.KnotCount(); i++) if (std::fabs(bot.m_knot[i] - tk) < 1e-12) exists = true;
            if (!exists) bot.InsertKnot(tk, 1);
          }
        }
        const double off = std::fabs(dz) * std::tan(angle);
        ON_NurbsCurve top = bot;
        const int ncv = bot.CVCount();
        for (int i = 0; i < ncv; i++) {
          ON_3dPoint q;
          bot.GetCV(i, q);
          ON_3dVector nrm;
          if (bot.Degree() == 1) {
            // polilinea: direzione di mitra tra i due lati
            ON_3dPoint pp, pn;
            bot.GetCV(std::max(0, i - 1), pp);
            bot.GetCV(std::min(ncv - 1, i + 1), pn);
            ON_3dVector t0 = q - pp, t1 = pn - q;
            t0.z = t1.z = 0;
            if (!t0.Unitize()) t0 = t1;
            if (!t1.Unitize()) t1 = t0;
            ON_3dVector n0(-t0.y, t0.x, 0), n1(-t1.y, t1.x, 0);
            nrm = n0 + n1;
            if (!nrm.Unitize()) nrm = n0;
            const double cosh = nrm * n0;
            if (cosh > 0.1) nrm = nrm / cosh;
          } else {
            ON_3dVector t = bot.TangentAt(bot.GrevilleAbcissa(i));
            nrm = ON_3dVector(-t.y, t.x, 0);
            if (!nrm.Unitize()) nrm = ON_3dVector(0, 0, 0);
          }
          const ON_3dPoint r = q + nrm * off + ON_3dVector(0, 0, dz);
          if (top.IsRational()) { const double wt = top.Weight(i); top.SetCV(i, ON_4dPoint(r.x * wt, r.y * wt, r.z * wt, wt)); }
          else top.SetCV(i, r);
        }
        auto* srf = new ON_NurbsSurface();
        if (!srf->CreateRuledSurface(bot, top)) { delete srf; continue; }
        ON_Brep* br = ON_Brep::New();
        ON_Surface* sp = srf;
        if (!br->Create(sp)) { delete br; delete srf; continue; }
        const ON_3dmObjectAttributes at = Attrs(e, id);
        e.AddObject(br, &at);
        n++;
        continue;
      }
      if (AddShape(e, result, Attrs(e, id))) n++;
    } catch (const Standard_Failure& f) {
      e.Out() << "  [" << f.GetMessageString() << "]\n";
    } catch (...) {
    }
  }
  if (!n) return e.Fail("estrusione rastremata non riuscita");
  e.Out() << "  Create " << n << " estrusioni rastremate\n";
  return true;
}

// Offset di una polilinea aperta nel piano xy (sinistra per d > 0), spigoli vivi.
ON_PolylineCurve* MiterOffset(const ON_SimpleArray<ON_3dPoint>& p, double d) {
  const int n = p.Count();
  if (n < 2) return nullptr;
  ON_3dPointArray out;
  for (int i = 0; i < n; i++) {
    ON_3dVector t0 = i > 0 ? p[i] - p[i - 1] : p[1] - p[0];
    ON_3dVector t1 = i + 1 < n ? p[i + 1] - p[i] : p[n - 1] - p[n - 2];
    t0.z = t1.z = 0;
    if (!t0.Unitize() || !t1.Unitize()) return nullptr;
    const ON_3dVector n0(-t0.y, t0.x, 0), n1(-t1.y, t1.x, 0);
    ON_3dVector m = n0 + n1;
    if (!m.Unitize()) return nullptr;
    const double c = m * n0;
    if (c < 0.05) return nullptr;
    out.Append(p[i] + m * (d / c));
  }
  return new ON_PolylineCurve(out);
}

// --- Slab ----------------------------------------------------------------------
// Offset della curva aperta da un lato (punto), chiusura alle estremita' ed
// estrusione in un solido dell'altezza data.
bool CmdSlab(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("Slab", "seleziona la curva, poi Invio");
  if (sel.empty()) return false;
  const auto* c = ON_Curve::Cast(Geo(e, sel.front()));
  if (!c || c->IsClosed()) return e.Fail("serve una curva aperta");
  auto d = e.ReadNumber(ts, "distanza di offset");
  if (!d) return false;
  auto side = e.ReadPoint(ts, "lato dell'offset");
  if (!side) return false;
  auto h = e.ReadNumber(ts, "altezza");
  if (!h) return false;
  if (*d <= e.Tolerance() || std::fabs(*h) <= e.Tolerance()) return e.Fail("valori non validi");
  try {
    const TopoDS_Wire w = WireOf(*c);
    ON_SimpleArray<ON_3dPoint> poly;
    const bool isPoly = c->IsPolyline(&poly) >= 2;
    auto make = [&](double dist) -> std::unique_ptr<ON_Curve> {
      if (isPoly) return std::unique_ptr<ON_Curve>(MiterOffset(poly, dist));
      BRepOffsetAPI_MakeOffset mo;
      mo.Init(GeomAbs_Arc, Standard_True);
      mo.AddWire(w);
      mo.Perform(dist);
      if (!mo.IsDone()) return nullptr;
      std::vector<ON_Curve*> cs = occt::ShapeToCurves(mo.Shape(), e.Tolerance());
      ON_SimpleArray<const ON_Curve*> in;
      for (auto* x : cs) in.Append(x);
      ON_SimpleArray<ON_Curve*> joined;
      ON_JoinCurves(in, joined, e.Tolerance() * 10, false);
      for (auto* x : cs) delete x;
      for (int i = 1; i < joined.Count(); i++) delete joined[i];
      return std::unique_ptr<ON_Curve>(joined.Count() ? joined[0] : nullptr);
    };
    auto dist = [&](const ON_Curve& cv) {
      double best = 1e300;
      for (int i = 0; i <= 200; i++) best = std::min(best, cv.PointAt(cv.Domain().ParameterAt(i / 200.0)).DistanceTo(*side));
      return best;
    };
    std::unique_ptr<ON_Curve> a = make(*d), b = make(-*d), off;
    if (a && b) off = dist(*a) <= dist(*b) ? std::move(a) : std::move(b);
    else off = a ? std::move(a) : std::move(b);
    if (!off) return e.Fail("offset non riuscito");
    if (off->PointAtStart().DistanceTo(c->PointAtStart()) > off->PointAtEnd().DistanceTo(c->PointAtStart())) off->Reverse();
    ON_PolyCurve pc;
    pc.Append(c->DuplicateCurve());
    pc.Append(new ON_LineCurve(c->PointAtEnd(), off->PointAtEnd()));
    off->Reverse();
    pc.Append(off.release());
    pc.Append(new ON_LineCurve(pc.SegmentCurve(2)->PointAtEnd(), c->PointAtStart()));
    BRepBuilderAPI_MakeWire mw;
    for (int i = 0; i < pc.Count(); i++) {
      TopoDS_Shape s = occt::CurveToShape(*pc.SegmentCurve(i));
      if (!s.IsNull()) mw.Add(TopoDS::Edge(s));
    }
    if (!mw.IsDone()) return e.Fail("contorno non chiuso");
    BRepBuilderAPI_MakeFace mf(mw.Wire(), Standard_True);
    if (!mf.IsDone()) return e.Fail("la curva deve essere piana");
    BRepPrimAPI_MakePrism pr(mf.Face(), gp_Vec(0, 0, *h));
    if (!AddShape(e, pr.Shape(), Attrs(e, sel.front()))) return e.Fail("solido non riuscito");
  } catch (...) {
    return e.Fail("Slab non riuscito");
  }
  e.Out() << "  Creata lastra\n";
  return true;
}

// --- ExtendSrf ---------------------------------------------------------------------
// Estende la superficie non tagliata dal bordo piu' vicino al clic.
// Type=Smooth (predefinito) o Line.
bool CmdExtendSrf(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto p = e.ReadPick(ts, "clicca il bordo della superficie da estendere");
  if (!p) return false;
  opts.merge(e.ReadOptions(ts));
  ON_UUID id = ON_nil_uuid;
  const ON_Geometry* g = NearestSurface(e, *p, {}, &id);
  if (!g) return e.Fail("superficie non trovata");
  auto len = e.ReadNumber(ts, "lunghezza dell'estensione");
  if (!len) return false;
  if (*len <= e.Tolerance()) return e.Fail("lunghezza non valida");
  std::unique_ptr<ON_Brep> owned;
  const ON_Brep* b = ON_Brep::Cast(g);
  if (!b) { owned.reset(g->BrepForm()); b = owned.get(); }
  if (!b || b->m_F.Count() != 1 || !b->FaceIsSurface(0)) return e.Fail("serve una superficie singola non tagliata");
  ON_NurbsSurface ns;
  if (!b->m_F[0].GetNurbForm(ns)) return e.Fail("superficie non convertibile");
  // bordo piu' vicino: u0, u1, v0, v1
  int bestSide = 0;
  double best = 1e300;
  const ON_Interval du = ns.Domain(0), dv = ns.Domain(1);
  for (int sideIdx = 0; sideIdx < 4; sideIdx++)
    for (int i = 0; i <= 100; i++) {
      const double f = i / 100.0;
      ON_3dPoint q;
      if (sideIdx < 2) q = ns.PointAt(du[sideIdx], dv.ParameterAt(f));
      else q = ns.PointAt(du.ParameterAt(f), dv[sideIdx - 2]);
      if (q.DistanceTo(*p) < best) { best = q.DistanceTo(*p); bestSide = sideIdx; }
    }
  const int cont = opts.count("type") && opts["type"] == "line" ? 1 : 2;
  try {
    Handle(Geom_BoundedSurface) s = occt::ToOcct(ns);
    GeomLib::ExtendSurfByLength(s, *len, cont, bestSide < 2, bestSide % 2 == 1);
    Handle(Geom_BSplineSurface) bs = Handle(Geom_BSplineSurface)::DownCast(s);
    if (bs.IsNull()) return e.Fail("estensione non riuscita");
    ON_NurbsSurface* out = occt::FromOcct(bs);
    if (!out) return e.Fail("estensione non riuscita");
    ON_Brep* nb = ON_Brep::New();
    ON_Surface* sp = out;
    if (!nb->Create(sp)) { delete nb; delete out; return e.Fail("estensione non valida"); }
    e.ReplaceObject(id, nb);
  } catch (...) {
    return e.Fail("estensione non riuscita");
  }
  e.Out() << "  Superficie estesa di " << *len << "\n";
  return true;
}

// --- Pull ------------------------------------------------------------------------
// Proietta curve e punti sulla superficie lungo la normale (punto piu' vicino).
bool CmdPull(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("Pull", "seleziona le curve e i punti da tirare, poi Invio");
  if (sel.empty()) return false;
  auto p = e.ReadPick(ts, "clicca la superficie");
  if (!p) return false;
  const ON_Geometry* g = NearestSurface(e, *p, sel, nullptr);
  if (!g) return e.Fail("superficie non trovata");
  int n = 0;
  try {
    const TopoDS_Shape target = occt::GeometryToShape(*g, e.Tolerance());
    for (const auto& id : sel) {
      const ON_Geometry* x = Geo(e, id);
      const ON_3dmObjectAttributes a = Attrs(e, id);
      if (const auto* pt = ON_Point::Cast(x)) {
        const TopoDS_Shape v = BRepBuilderAPI_MakeVertex(gp_Pnt(pt->point.x, pt->point.y, pt->point.z)).Vertex();
        BRepExtrema_DistShapeShape d(v, target);
        if (!d.IsDone() || d.NbSolution() < 1) continue;
        const gp_Pnt q = d.PointOnShape2(1);
        e.AddObject(new ON_Point(ON_3dPoint(q.X(), q.Y(), q.Z())), &a);
        n++;
        continue;
      }
      const auto* c = ON_Curve::Cast(x);
      if (!c) continue;
      BRepAlgo_NormalProjection proj(target);
      proj.Add(WireOf(*c));
      proj.SetParams(e.Tolerance(), e.Tolerance() * 10, GeomAbs_C2, 14, 16);
      proj.Build();
      if (!proj.IsDone()) continue;
      std::vector<ON_Curve*> cs = occt::ShapeToCurves(proj.Projection(), e.Tolerance());
      ON_SimpleArray<const ON_Curve*> in;
      for (auto* k : cs) in.Append(k);
      ON_SimpleArray<ON_Curve*> joined;
      ON_JoinCurves(in, joined, e.Tolerance() * 10, false);
      for (auto* k : cs) delete k;
      for (int i = 0; i < joined.Count(); i++) { e.AddObject(joined[i], &a); n++; }
    }
  } catch (...) {
  }
  if (!n) return e.Fail("nessuna proiezione riuscita");
  e.Out() << "  Creati " << n << " oggetti sulla superficie\n";
  return true;
}

// --- Sweep2 --------------------------------------------------------------------
// Primi due oggetti selezionati = binari, poi i profili. Ogni sezione e' il
// profilo riportato sul segmento tra i due binari (rotazione e scala uniforme,
// MaintainHeight=Yes scala solo la larghezza); le sezioni si uniscono con un loft.
struct Frame {
  ON_3dPoint o;
  ON_3dVector x, y, z;
  double w;
};

bool StationFrame(const ON_Curve& r1, const ON_Curve& r2, double t1, double t2, Frame& f) {
  f.o = r1.PointAt(t1);
  ON_3dVector x = r2.PointAt(t2) - f.o;
  f.w = x.Length();
  if (!x.Unitize()) return false;
  ON_3dVector z = r1.TangentAt(t1) + r2.TangentAt(t2);
  z = z - x * (z * x);
  if (!z.Unitize()) return false;
  f.x = x;
  f.z = z;
  f.y = ON_CrossProduct(z, x);
  return true;
}

// Stesso grado, razionalita' e nodi (dominio 0..1), senza cambiare la forma.
bool MakeCompatible(ON_NurbsCurve& a, ON_NurbsCurve& b) {
  a.SetDomain(0, 1);
  b.SetDomain(0, 1);
  const int deg = std::max(a.Degree(), b.Degree());
  if (a.Degree() < deg) a.IncreaseDegree(deg);
  if (b.Degree() < deg) b.IncreaseDegree(deg);
  if (a.IsRational() != b.IsRational()) { a.MakeRational(); b.MakeRational(); }
  auto mult = [](const ON_NurbsCurve& c) {
    std::vector<std::pair<double, int>> k;
    for (int i = c.Order() - 1; i <= c.CVCount() - 1; ) {
      const double t = c.m_knot[i];
      int m = c.KnotMultiplicity(i);
      if (t > 1e-12 && t < 1 - 1e-12) k.push_back({t, m});
      i += std::max(1, m);
    }
    return k;
  };
  auto fill = [&](ON_NurbsCurve& dst, const ON_NurbsCurve& src) {
    for (const auto& [t, m] : mult(src)) {
      int have = 0;
      for (int i = 0; i < dst.KnotCount(); i++) if (std::fabs(dst.m_knot[i] - t) < 1e-10) have++;
      if (have < m) {
        for (int i = 0; i < dst.KnotCount(); i++) if (std::fabs(dst.m_knot[i] - t) < 1e-10) { dst.m_knot[i] = t; }
        if (!dst.InsertKnot(t, m)) return false;
      }
    }
    return true;
  };
  if (!fill(a, b) || !fill(b, a)) return false;
  return a.CVCount() == b.CVCount();
}

bool CmdSweep2(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("Sweep2", "seleziona i due binari, poi i profili, poi Invio");
  opts.merge(e.ReadOptions(ts));
  if (sel.size() < 3) return e.Fail("seleziona 2 binari e almeno un profilo");
  const auto* c1 = ON_Curve::Cast(Geo(e, sel[0]));
  const auto* c2 = ON_Curve::Cast(Geo(e, sel[1]));
  if (!c1 || !c2) return e.Fail("i binari devono essere curve");
  std::unique_ptr<ON_Curve> r1(c1->DuplicateCurve()), r2(c2->DuplicateCurve());
  // binari nello stesso verso
  if (r1->PointAtStart().DistanceTo(r2->PointAtStart()) + r1->PointAtEnd().DistanceTo(r2->PointAtEnd()) >
      r1->PointAtStart().DistanceTo(r2->PointAtEnd()) + r1->PointAtEnd().DistanceTo(r2->PointAtStart()))
    r2->Reverse();
  const ArcTable A1(*r1), A2(*r2);
  const bool keepHeight = YesOpt(opts, "maintainheight", false);
  struct Prof {
    double f;  // posizione lungo i binari (frazione della lunghezza)
    ON_NurbsCurve local;  // punti di controllo nel riferimento della stazione
  };
  std::vector<Prof> profs;
  for (size_t i = 2; i < sel.size(); i++) {
    const auto* pc = ON_Curve::Cast(Geo(e, sel[i]));
    if (!pc) continue;
    ON_NurbsCurve nc;
    if (!pc->GetNurbForm(nc)) continue;
    // estremo del profilo sul binario 1
    ON_3dPoint s = nc.PointAtStart(), en = nc.PointAtEnd();
    double ts1 = A1.Closest(s), te1 = A1.Closest(en);
    if (r1->PointAt(te1).DistanceTo(en) < r1->PointAt(ts1).DistanceTo(s)) { nc.Reverse(); std::swap(ts1, te1); s = nc.PointAtStart(); }
    const double f = A1.LengthAt(ts1) / A1.Length();
    const double t2 = A2.Closest(nc.PointAtEnd());
    Frame fr;
    if (!StationFrame(*r1, *r2, ts1, t2, fr)) continue;
    // origine sul binario e fine sul secondo binario: si usano gli estremi veri del profilo
    fr.o = nc.PointAtStart();
    ON_3dVector x = nc.PointAtEnd() - fr.o;
    fr.w = x.Length();
    if (!x.Unitize()) continue;
    fr.z = fr.z - x * (fr.z * x);
    fr.z.Unitize();
    fr.x = x;
    fr.y = ON_CrossProduct(fr.z, x);
    ON_NurbsCurve loc = nc;
    for (int k = 0; k < loc.CVCount(); k++) {
      ON_3dPoint q;
      nc.GetCV(k, q);
      const ON_3dVector v = q - fr.o;
      const double sh = keepHeight ? 1.0 : fr.w;
      const ON_3dPoint l(v * fr.x / fr.w, v * fr.y / sh, v * fr.z / sh);
      if (loc.IsRational()) { const double w = nc.Weight(k); loc.SetCV(k, ON_4dPoint(l.x * w, l.y * w, l.z * w, w)); }
      else loc.SetCV(k, l);
    }
    profs.push_back({f, loc});
  }
  if (profs.empty()) return e.Fail("nessun profilo valido");
  std::sort(profs.begin(), profs.end(), [](const Prof& a, const Prof& b) { return a.f < b.f; });
  for (int pass = 0; pass < 2; pass++)  // il secondo giro porta i nodi del primo a tutti
    for (size_t i = 1; i < profs.size(); i++)
      if (!MakeCompatible(profs[0].local, profs[i].local)) return e.Fail("profili non compatibili");
  const int stations = std::max(12, 6 * int(profs.size()) + 6);
  std::vector<ON_NurbsCurve> secs;
  std::vector<double> vpar;
  for (int k = 0; k <= stations; k++) {
    const double f = double(k) / stations;
    const double t1 = A1.ParamAt(f * A1.Length()), t2 = A2.ParamAt(f * A2.Length());
    Frame fr;
    if (!StationFrame(*r1, *r2, t1, t2, fr)) continue;
    // forma locale: interpolazione lineare tra i profili vicini
    ON_NurbsCurve loc = profs[0].local;
    if (profs.size() > 1) {
      size_t j = 0;
      while (j + 2 < profs.size() && f > profs[j + 1].f) j++;
      const double span = profs[j + 1].f - profs[j].f;
      const double u = span > 1e-12 ? std::clamp((f - profs[j].f) / span, 0.0, 1.0) : 0.0;
      for (int i = 0; i < loc.CVCount(); i++) {
        ON_4dPoint a, b;
        profs[j].local.GetCV(i, a);
        profs[j + 1].local.GetCV(i, b);
        loc.SetCV(i, ON_4dPoint((1 - u) * a.x + u * b.x, (1 - u) * a.y + u * b.y, (1 - u) * a.z + u * b.z, (1 - u) * a.w + u * b.w));
      }
    }
    ON_NurbsCurve sec = loc;
    const double sh = keepHeight ? 1.0 : fr.w;
    for (int i = 0; i < sec.CVCount(); i++) {
      ON_3dPoint l;
      loc.GetCV(i, l);
      const ON_3dPoint q = fr.o + fr.x * (l.x * fr.w) + fr.y * (l.y * sh) + fr.z * (l.z * sh);
      if (sec.IsRational()) { const double w = loc.Weight(i); sec.SetCV(i, ON_4dPoint(q.x * w, q.y * w, q.z * w, w)); }
      else sec.SetCV(i, q);
    }
    secs.push_back(sec);
    vpar.push_back(f);
  }
  if (secs.size() < 2) return e.Fail("sezioni non valide");
  ON_Brep* br = nullptr;
  try { br = SurfaceFromSections(secs, vpar, false, e.Tolerance()); } catch (...) {}
  if (!br) return e.Fail("sweep non riuscito");
  const ON_3dmObjectAttributes at = Attrs(e, sel[2]);
  e.AddObject(br, &at);
  e.Out() << "  Creata superficie di sweep su 2 binari\n";
  return true;
}

// --- Flow --------------------------------------------------------------------------
// Riferimento lungo una curva: tangente, e assi y,z da un "alto" fisso (z del mondo).
void CurveFrame(const ON_Curve& c, double t, ON_3dPoint& o, ON_3dVector& x, ON_3dVector& y, ON_3dVector& z) {
  o = c.PointAt(t);
  x = c.TangentAt(t);
  ON_3dVector up(0, 0, 1);
  if (std::fabs(x * up) > 0.999) up = ON_3dVector(1, 0, 0);
  y = ON_CrossProduct(up, x);
  y.Unitize();
  z = ON_CrossProduct(x, y);
}

// Flow: gli oggetti seguono la curva di destinazione come stavano sulla curva
// di base. Stretch=Yes adatta la lunghezza alla nuova curva.
bool CmdFlow(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("Flow", "seleziona gli oggetti da far scorrere, poi Invio");
  if (sel.empty()) return false;
  opts.merge(e.ReadOptions(ts));
  auto p0 = e.ReadPick(ts, "curva di base (vicino all'inizio)");
  if (!p0) return false;
  const ON_Curve* base = NearestCurve(e, *p0, sel);
  if (!base) return e.Fail("curva di base non trovata");
  auto p1 = e.ReadPick(ts, "curva di destinazione (vicino all'inizio)");
  if (!p1) return false;
  std::vector<ON_UUID> ex = sel;
  ON_UUID baseId = ON_nil_uuid;
  NearestCurve(e, *p0, sel, &baseId);
  ex.push_back(baseId);
  const ON_Curve* target = NearestCurve(e, *p1, ex);
  if (!target) return e.Fail("curva di destinazione non trovata");
  std::unique_ptr<ON_Curve> b(base->DuplicateCurve()), t(target->DuplicateCurve());
  if (p0->DistanceTo(b->PointAtEnd()) < p0->DistanceTo(b->PointAtStart())) b->Reverse();
  if (p1->DistanceTo(t->PointAtEnd()) < p1->DistanceTo(t->PointAtStart())) t->Reverse();
  const ArcTable AB(*b), AT(*t);
  const bool stretch = YesOpt(opts, "stretch", false);
  const double ratio = stretch ? AT.Length() / AB.Length() : 1.0;
  PointMap f = [&](const ON_3dPoint& p) {
    const double tb = AB.Closest(p);
    ON_3dPoint o; ON_3dVector x, y, z;
    CurveFrame(*b, tb, o, x, y, z);
    double s = AB.LengthAt(tb);
    const ON_3dVector v = p - o;
    // oltre le estremita' la curva si prolunga diritta
    s += v * x;
    const double lx = 0, ly = v * y, lz = v * z;
    (void)lx;
    const double st = s * ratio;
    double extra = 0;
    double tt;
    if (st < 0) { tt = t->Domain()[0]; extra = st; }
    else if (st > AT.Length()) { tt = t->Domain()[1]; extra = st - AT.Length(); }
    else tt = AT.ParamAt(st);
    ON_3dPoint O; ON_3dVector X, Y, Z;
    CurveFrame(*t, tt, O, X, Y, Z);
    return O + X * extra + Y * ly + Z * lz;
  };
  int n = 0;
  for (const auto& id : sel) {
    ON_Geometry* g = DeformGeometry(Geo(e, id), f);
    if (!g) continue;
    e.ReplaceObject(id, g);
    n++;
  }
  if (!n) return e.Fail("nessun oggetto deformato");
  e.Out() << "  " << n << " oggetti fatti scorrere lungo la curva\n";
  return true;
}

// --- ArraySrf --------------------------------------------------------------------------
// Copie sulla griglia u x v della superficie, orientate con la sua normale.
bool CmdArraySrf(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("ArraySrf", "seleziona gli oggetti da disporre, poi Invio");
  if (sel.empty()) return false;
  auto base = e.ReadPoint(ts, "punto base di riferimento");
  if (!base) return false;
  auto p = e.ReadPick(ts, "clicca la superficie");
  if (!p) return false;
  const ON_Geometry* g = NearestSurface(e, *p, sel, nullptr);
  if (!g) return e.Fail("superficie non trovata");
  auto nu = e.ReadInt(ts, "numero nella direzione u");
  if (!nu) return false;
  auto nv = e.ReadInt(ts, "numero nella direzione v");
  if (!nv) return false;
  if (*nu < 1 || *nv < 1) return e.Fail("numeri non validi");
  std::unique_ptr<ON_Brep> owned;
  const ON_Brep* b = ON_Brep::Cast(g);
  if (!b) { owned.reset(g->BrepForm()); b = owned.get(); }
  if (!b || b->m_F.Count() < 1) return e.Fail("superficie non valida");
  const ON_BrepFace& face = b->m_F[0];
  const ON_Interval du = face.Domain(0), dv = face.Domain(1);
  const ON_Plane from(*base, ON_3dVector(1, 0, 0), ON_3dVector(0, 1, 0));
  int n = 0;
  for (int i = 0; i < *nu; i++)
    for (int j = 0; j < *nv; j++) {
      const double u = du.ParameterAt(*nu == 1 ? 0.0 : double(i) / (*nu - 1));
      const double v = dv.ParameterAt(*nv == 1 ? 0.0 : double(j) / (*nv - 1));
      ON_3dPoint P; ON_3dVector Su, Sv;
      if (!face.Ev1Der(u, v, P, Su, Sv)) continue;
      ON_3dVector N = face.NormalAt(u, v);
      if (!Su.Unitize()) continue;
      ON_Plane to(P, Su, ON_CrossProduct(N, Su));
      ON_Xform x;
      x.Rotation(from, to);
      for (const auto& id : sel) {
        const ON_Geometry* src = Geo(e, id);
        if (!src) continue;
        ON_Geometry* c = src->Duplicate();
        c->Transform(x);
        const ON_3dmObjectAttributes a = Attrs(e, id);
        e.AddObject(c, &a);
        n++;
      }
    }
  if (!n) return e.Fail("nessuna copia creata");
  e.Out() << "  Create " << n << " copie sulla superficie\n";
  return true;
}

// --- LineSrfNormal ------------------------------------------------------------------
// Linea normale alla superficie nel punto cliccato, della lunghezza data
// (negativa = verso l'interno).
bool CmdLineSrfNormal(Engine& e, TokenStream& ts) {
  auto p = e.ReadPick(ts, "punto sulla superficie");
  if (!p) return false;
  const ON_Geometry* g = NearestSurface(e, *p, {}, nullptr);
  if (!g) return e.Fail("superficie non trovata");
  auto len = e.ReadNumber(ts, "lunghezza");
  if (!len) return false;
  if (std::fabs(*len) <= e.Tolerance()) return e.Fail("lunghezza nulla");
  try {
    const TopoDS_Shape s = occt::GeometryToShape(*g, e.Tolerance());
    const gp_Pnt P(p->x, p->y, p->z);
    const TopoDS_Shape v = BRepBuilderAPI_MakeVertex(P).Vertex();
    // faccia piu' vicina, poi il punto (u,v) su di essa
    double best = 1e300;
    TopoDS_Face face;
    for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next()) {
      BRepExtrema_DistShapeShape d(v, ex.Current());
      if (d.IsDone() && d.Value() < best) { best = d.Value(); face = TopoDS::Face(ex.Current()); }
    }
    if (face.IsNull()) return e.Fail("faccia non trovata");
    BRepExtrema_DistShapeShape d(v, face);
    if (!d.IsDone() || d.NbSolution() < 1) return e.Fail("punto non trovato");
    const gp_Pnt foot = d.PointOnShape2(1);
    ShapeAnalysis_Surface sas(BRep_Tool::Surface(face));
    const gp_Pnt2d uv = sas.ValueOfUV(foot, e.Tolerance());
    BRepGProp_Face gf(face);
    gp_Pnt q;
    gp_Vec n;
    gf.Normal(uv.X(), uv.Y(), q, n);
    if (n.Magnitude() < 1e-15) return e.Fail("normale non definita");
    n.Normalize();
    const ON_3dPoint a(q.X(), q.Y(), q.Z());
    e.AddObject(new ON_LineCurve(a, a + ON_3dVector(n.X(), n.Y(), n.Z()) * *len));
  } catch (...) {
    return e.Fail("normale non calcolata");
  }
  e.Out() << "  Creata linea normale\n";
  return true;
}

// --- NetworkSrf -------------------------------------------------------------------------
// Superficie da una rete di curve: le curve con entrambi gli estremi sugli
// estremi di altre curve formano il bordo, le altre sono vincoli interni.
bool CmdNetworkSrf(Engine& e, TokenStream& ts) {
  (void)ts;
  auto sel = e.RequireSelection("NetworkSrf", "seleziona le curve della rete, poi Invio");
  std::vector<std::unique_ptr<ON_Curve>> cs;
  for (const auto& id : sel)
    if (const auto* c = ON_Curve::Cast(Geo(e, id))) cs.emplace_back(c->DuplicateCurve());
  if (cs.size() < 3) return e.Fail("servono almeno 3 curve");
  const double tol = std::max(e.Tolerance() * 10, 1e-6);
  auto touchesEnd = [&](const ON_3dPoint& p, size_t self) {
    for (size_t j = 0; j < cs.size(); j++)
      if (j != self && (cs[j]->PointAtStart().DistanceTo(p) < tol || cs[j]->PointAtEnd().DistanceTo(p) < tol)) return true;
    return false;
  };
  std::vector<size_t> border, inner;
  for (size_t i = 0; i < cs.size(); i++) {
    if (touchesEnd(cs[i]->PointAtStart(), i) && touchesEnd(cs[i]->PointAtEnd(), i)) border.push_back(i);
    else inner.push_back(i);
  }
  if (border.size() < 2) return e.Fail("il bordo della rete non e' chiuso");
  // bordo ordinato a catena
  std::vector<size_t> chain{border[0]};
  std::vector<bool> used(cs.size(), false);
  used[border[0]] = true;
  ON_3dPoint end = cs[border[0]]->PointAtEnd();
  for (size_t step = 1; step < border.size(); step++) {
    bool found = false;
    for (size_t k : border) {
      if (used[k]) continue;
      if (cs[k]->PointAtStart().DistanceTo(end) < tol) { found = true; }
      else if (cs[k]->PointAtEnd().DistanceTo(end) < tol) { cs[k]->Reverse(); found = true; }
      if (found) { chain.push_back(k); used[k] = true; end = cs[k]->PointAtEnd(); break; }
    }
    if (!found) break;
  }
  if (end.DistanceTo(cs[chain[0]]->PointAtStart()) > tol) return e.Fail("il bordo della rete non e' chiuso");
  for (size_t k : border) if (!used[k]) inner.push_back(k);
  try {
    BRepOffsetAPI_MakeFilling mf(3, 15, 2, Standard_False, 1e-5, 1e-4, 1e-2, 0.1, 8, 9);
    for (size_t k : chain) {
      TopoDS_Shape s = occt::CurveToShape(*cs[k]);
      if (s.IsNull()) return e.Fail("curva di bordo non valida");
      mf.Add(TopoDS::Edge(s), GeomAbs_C0, Standard_True);
    }
    for (size_t k : inner) {
      TopoDS_Shape s = occt::CurveToShape(*cs[k]);
      if (!s.IsNull()) mf.Add(TopoDS::Edge(s), GeomAbs_C0, Standard_False);
    }
    mf.Build();
    if (!mf.IsDone() || !AddShape(e, mf.Shape(), Attrs(e, sel.front()))) return e.Fail("superficie dalla rete non riuscita");
  } catch (...) {
    return e.Fail("superficie dalla rete non riuscita");
  }
  e.Out() << "  Creata superficie da rete di " << cs.size() << " curve (" << inner.size() << " interne)\n";
  return true;
}

// --- MeshBooleanUnion / Difference / Intersection -----------------------------------------
// Le mesh chiuse diventano solidi a facce piane, si combinano con OpenCASCADE e
// il risultato torna mesh.
TopoDS_Shape MeshToSolid(const ON_Mesh& m, double tol) {
  BRepBuilderAPI_Sewing sew(tol);
  for (int i = 0; i < m.FaceCount(); i++) {
    const ON_MeshFace& f = m.m_F[i];
    const int n = f.IsQuad() ? 4 : 3;
    BRepBuilderAPI_MakePolygon poly;
    for (int k = 0; k < n; k++) {
      const ON_3dPoint p = m.Vertex(f.vi[k]);
      poly.Add(gp_Pnt(p.x, p.y, p.z));
    }
    poly.Close();
    if (!poly.IsDone()) continue;
    BRepBuilderAPI_MakeFace mf(poly.Wire(), Standard_True);
    if (mf.IsDone()) sew.Add(mf.Face());
  }
  sew.Perform();
  const TopoDS_Shape r = sew.SewedShape();
  for (TopExp_Explorer ex(r, TopAbs_SHELL); ex.More(); ex.Next()) {
    ShapeFix_Shell fs(TopoDS::Shell(ex.Current()));
    fs.Perform();
    BRepBuilderAPI_MakeSolid ms(fs.Shell());
    if (!ms.IsDone()) break;
    TopoDS_Solid so = ms.Solid();
    BRepLib::OrientClosedSolid(so);
    return so;
  }
  return {};
}

bool MeshBoolean(Engine& e, int op, const char* name) {
  auto sel = e.RequireSelection(name, op == 1 ? "seleziona prima la mesh da cui sottrarre, poi le altre, poi Invio"
                                              : "seleziona le mesh, poi Invio");
  std::vector<ON_UUID> ids;
  std::vector<TopoDS_Shape> solids;
  for (const auto& id : sel) {
    const auto* m = ON_Mesh::Cast(Geo(e, id));
    if (!m) continue;
    if (m->FaceCount() > 20000) return e.Fail("mesh troppo grande (oltre 20000 facce): riducila prima con Reduce");
    TopoDS_Shape s;
    try { s = MeshToSolid(*m, std::max(e.Tolerance(), 1e-6)); } catch (...) {}
    if (s.IsNull()) return e.Fail("una delle mesh non e' chiusa");
    solids.push_back(s);
    ids.push_back(id);
  }
  if (solids.size() < 2) return e.Fail("seleziona almeno 2 mesh chiuse");
  TopoDS_Shape r = solids[0];
  try {
    for (size_t i = 1; i < solids.size(); i++) {
      if (op == 0) r = BRepAlgoAPI_Fuse(r, solids[i]).Shape();
      else if (op == 1) r = BRepAlgoAPI_Cut(r, solids[i]).Shape();
      else r = BRepAlgoAPI_Common(r, solids[i]).Shape();
    }
  } catch (...) {
    return e.Fail("operazione booleana non riuscita");
  }
  ON_Mesh* out = occt::MeshShape(r, 1e-3, 0.5);
  if (!out || out->FaceCount() == 0) { delete out; return e.Fail("risultato vuoto"); }
  out->CombineIdenticalVertices(true, true);
  out->ComputeVertexNormals();
  const ON_3dmObjectAttributes a = Attrs(e, ids.front());
  for (const auto& id : ids) e.DeleteObject(id);
  e.AddObject(out, &a);
  e.Out() << "  " << name << ": mesh di " << out->FaceCount() << " facce\n";
  return true;
}

bool CmdMeshBooleanUnion(Engine& e, TokenStream&) { return MeshBoolean(e, 0, "MeshBooleanUnion"); }
bool CmdMeshBooleanDifference(Engine& e, TokenStream&) { return MeshBoolean(e, 1, "MeshBooleanDifference"); }
bool CmdMeshBooleanIntersection(Engine& e, TokenStream&) { return MeshBoolean(e, 2, "MeshBooleanIntersection"); }

// --- RailRevolve ----------------------------------------------------------------------------
// Rivoluzione del profilo attorno all'asse con l'estremo che segue il binario:
// il profilo si scala in direzione radiale (ScaleHeight=Yes scala anche in altezza).
bool CmdRailRevolve(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto pp = e.ReadPick(ts, "curva del profilo");
  if (!pp) return false;
  ON_UUID pid = ON_nil_uuid;
  const ON_Curve* prof = NearestCurve(e, *pp, {}, &pid);
  if (!prof) return e.Fail("profilo non trovato");
  auto pr = e.ReadPick(ts, "curva del binario");
  if (!pr) return false;
  const ON_Curve* rail = NearestCurve(e, *pr, {pid});
  if (!rail) return e.Fail("binario non trovato");
  auto a0 = e.ReadPoint(ts, "inizio dell'asse");
  if (!a0) return false;
  auto a1 = e.ReadPoint(ts, "fine dell'asse");
  if (!a1) return false;
  opts.merge(e.ReadOptions(ts));
  ON_3dVector Z = *a1 - *a0;
  if (!Z.Unitize()) return e.Fail("asse nullo");
  const bool scaleHeight = YesOpt(opts, "scaleheight", false);
  ON_Plane frame(*a0, Z);
  auto local = [&](const ON_3dPoint& q, double& r, double& ang, double& h) {
    const ON_3dVector v = q - *a0;
    const double x = v * frame.xaxis, y = v * frame.yaxis;
    h = v * Z;
    r = std::hypot(x, y);
    ang = std::atan2(y, x);
  };
  ON_NurbsCurve pc;
  if (!prof->GetNurbForm(pc)) return e.Fail("profilo non valido");
  // estremo del profilo sul binario
  auto railDist = [&](const ON_3dPoint& q) {
    double b = 1e300;
    for (int i = 0; i <= 400; i++) b = std::min(b, rail->PointAt(rail->Domain().ParameterAt(i / 400.0)).DistanceTo(q));
    return b;
  };
  const ON_3dPoint E = railDist(pc.PointAtStart()) < railDist(pc.PointAtEnd()) ? pc.PointAtStart() : pc.PointAtEnd();
  double rE, angE, hE;
  local(E, rE, angE, hE);
  if (rE <= e.Tolerance()) return e.Fail("l'estremo del profilo e' sull'asse");
  // binario campionato in angolo (svolto) attorno all'asse
  const int ns = 2000;
  std::vector<double> rr(ns + 1), aa(ns + 1), hh(ns + 1);
  for (int i = 0; i <= ns; i++) {
    local(rail->PointAt(rail->Domain().ParameterAt(double(i) / ns)), rr[i], aa[i], hh[i]);
    if (i) while (aa[i] - aa[i - 1] > ON_PI) aa[i] -= 2 * ON_PI;
    if (i) while (aa[i] - aa[i - 1] < -ON_PI) aa[i] += 2 * ON_PI;
  }
  const bool closed = rail->IsClosed() && std::fabs(std::fabs(aa[ns] - aa[0]) - 2 * ON_PI) < 0.1;
  const int stations = 48;
  std::vector<ON_NurbsCurve> secs;
  std::vector<double> vpar;
  const int count = closed ? stations : stations + 1;
  for (int k = 0; k < count; k++) {
    const double f = double(k) / stations;
    const int i = std::min(ns - 1, int(f * ns));
    const double u = f * ns - i;
    const double ang = aa[i] + u * (aa[i + 1] - aa[i]);
    const double r = rr[i] + u * (rr[i + 1] - rr[i]);
    const double h = hh[i] + u * (hh[i + 1] - hh[i]);
    const double s = r / rE;
    ON_NurbsCurve sec = pc;
    for (int c = 0; c < sec.CVCount(); c++) {
      ON_3dPoint q;
      pc.GetCV(c, q);
      double qr, qa, qh;
      local(q, qr, qa, qh);
      const double nr = qr * s;
      const double na = qa + (ang - angE);
      const double nh = scaleHeight ? h + (qh - hE) * s : qh + (h - hE);
      const ON_3dPoint w = *a0 + frame.xaxis * (nr * std::cos(na)) + frame.yaxis * (nr * std::sin(na)) + Z * nh;
      if (sec.IsRational()) { const double wt = sec.Weight(c); sec.SetCV(c, ON_4dPoint(w.x * wt, w.y * wt, w.z * wt, wt)); }
      else sec.SetCV(c, w);
    }
    secs.push_back(sec);
    vpar.push_back(f);
  }
  if (closed) vpar.push_back(1.0);
  ON_Brep* br = nullptr;
  try { br = SurfaceFromSections(secs, vpar, closed, e.Tolerance()); } catch (...) {}
  if (!br) return e.Fail("rivoluzione su binario non riuscita");
  const ON_3dmObjectAttributes at = Attrs(e, pid);
  e.AddObject(br, &at);
  e.Out() << "  Creata superficie di rivoluzione su binario\n";
  return true;
}

// --- InterpCrvOnSrf ----------------------------------------------------------------------
// Curva interpolata sulla superficie per i punti dati (Invio per finire).
bool CmdInterpCrvOnSrf(Engine& e, TokenStream& ts) {
  auto ps = e.ReadPick(ts, "clicca la superficie");
  if (!ps) return false;
  const ON_Geometry* g = NearestSurface(e, *ps, {}, nullptr);
  if (!g) return e.Fail("superficie non trovata");
  std::vector<ON_3dPoint> pts;
  while (true) {
    if (pts.size() >= 2 && !e.MoreInput(ts)) break;
    auto q = e.ReadPoint(ts, pts.size() < 2 ? "punto sulla superficie" : "punto successivo (Invio per finire)");
    if (!q) { if (pts.size() >= 2) break; return false; }
    pts.push_back(*q);
  }
  e.SkipEnter(ts);
  try {
    const TopoDS_Shape s = occt::GeometryToShape(*g, e.Tolerance());
    TopExp_Explorer ex(s, TopAbs_FACE);
    if (!ex.More()) return e.Fail("superficie non valida");
    const TopoDS_Face face = TopoDS::Face(ex.Current());
    Handle(Geom_Surface) srf = BRep_Tool::Surface(face);
    ShapeAnalysis_Surface sas(srf);
    Handle(TColgp_HArray1OfPnt2d) uv = new TColgp_HArray1OfPnt2d(1, (int)pts.size());
    for (size_t i = 0; i < pts.size(); i++) uv->SetValue(int(i) + 1, sas.ValueOfUV(gp_Pnt(pts[i].x, pts[i].y, pts[i].z), e.Tolerance()));
    Geom2dAPI_Interpolate interp(uv, Standard_False, 1e-9);
    interp.Perform();
    if (!interp.IsDone()) return e.Fail("interpolazione non riuscita");
    BRepBuilderAPI_MakeEdge me(interp.Curve(), srf);
    if (!me.IsDone()) return e.Fail("curva sulla superficie non riuscita");
    TopoDS_Edge edge = me.Edge();
    BRepLib::BuildCurves3d(edge, 1e-5, GeomAbs_C2, 14, 200);
    std::vector<ON_Curve*> cs = occt::ShapeToCurves(edge, e.Tolerance());
    if (cs.empty()) return e.Fail("curva non convertibile");
    for (size_t i = 1; i < cs.size(); i++) delete cs[i];
    e.AddObject(cs[0]);
  } catch (...) {
    return e.Fail("curva sulla superficie non riuscita");
  }
  e.Out() << "  Creata curva interpolata sulla superficie\n";
  return true;
}

// --- FitCrv -----------------------------------------------------------------------------------
// Ricostruisce le curve con una NURBS cubica entro la tolleranza (Tolerance=).
bool CmdFitCrv(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("FitCrv", "seleziona le curve da riadattare, poi Invio");
  if (sel.empty()) return false;
  opts.merge(e.ReadOptions(ts));
  const double tol = NumOpt(opts, "tolerance", e.Tolerance());
  int n = 0;
  for (const auto& id : sel) {
    const auto* c = ON_Curve::Cast(Geo(e, id));
    if (!c) continue;
    const ArcTable at(*c);
    const int np = 200;
    TColgp_Array1OfPnt arr(1, np + 1);
    for (int i = 0; i <= np; i++) {
      const ON_3dPoint q = c->PointAt(at.ParamAt(at.Length() * i / np));
      arr.SetValue(i + 1, gp_Pnt(q.x, q.y, q.z));
    }
    try {
      GeomAPI_PointsToBSpline fit(arr, 3, 3, GeomAbs_C2, tol);
      if (!fit.IsDone()) continue;
      ON_NurbsCurve* r = occt::FromOcct(fit.Curve());
      if (!r) continue;
      e.ReplaceObject(id, r);
      n++;
    } catch (...) {
    }
  }
  if (!n) return e.Fail("nessuna curva riadattata");
  e.Out() << "  Riadattate " << n << " curve (tolleranza " << tol << ")\n";
  return true;
}

}  // namespace

// Usata anche da CmdKernel5 (FilletSrf, ChamferSrf, BlendSrf).
ON_Brep* SectionsSurface(const std::vector<ON_NurbsCurve>& secs, const std::vector<double>& v, bool periodic, double tol) {
  return SurfaceFromSections(secs, v, periodic, tol);
}

void RegisterSurface3Commands(Engine& e) {
  auto reg = [&](const char* n, const char* cat, const char* d, CommandFn f) {
    CommandInfo i{n, cat, d, f};
    i.modifiesModel = true;
    e.Register(i);
  };
  reg("ExtrudeCrvTapered", "Superfici", "Estrude le curve con angolo di sformo (DraftAngle=, Solid=)", CmdExtrudeCrvTapered);
  reg("Slab", "Solidi", "Offset di una curva aperta, chiuso ed estruso in un solido", CmdSlab);
  reg("ExtendSrf", "Superfici", "Estende una superficie non tagliata dal bordo cliccato (Type=Smooth/Line)", CmdExtendSrf);
  reg("Pull", "Curve", "Proietta curve e punti su una superficie lungo la normale", CmdPull);
  reg("Sweep2", "Superfici", "Sweep su due binari (primi due selezionati) con uno o piu' profili", CmdSweep2);
  reg("Flow", "Trasforma", "Fa scorrere gli oggetti da una curva di base a una di destinazione (Stretch=)", CmdFlow);
  reg("LineSrfNormal", "Curve", "Linea normale a una superficie nel punto cliccato", CmdLineSrfNormal);
  reg("NetworkSrf", "Superfici", "Superficie da una rete di curve (bordo chiuso e curve interne)", CmdNetworkSrf);
  reg("MeshBooleanUnion", "Mesh", "Unione di mesh chiuse", CmdMeshBooleanUnion);
  reg("MeshBooleanDifference", "Mesh", "Differenza di mesh chiuse (la prima meno le altre)", CmdMeshBooleanDifference);
  reg("MeshBooleanIntersection", "Mesh", "Intersezione di mesh chiuse", CmdMeshBooleanIntersection);
  reg("RailRevolve", "Superfici", "Rivoluzione con l'estremo del profilo lungo un binario (ScaleHeight=)", CmdRailRevolve);
  reg("InterpCrvOnSrf", "Curve", "Curva interpolata per punti su una superficie", CmdInterpCrvOnSrf);
  reg("FitCrv", "Curve", "Riadatta le curve con una NURBS cubica entro la tolleranza", CmdFitCrv);
  reg("Fit", "Curve", "Come FitCrv", CmdFitCrv);
  reg("ArraySrf", "Trasforma", "Copie degli oggetti sulla griglia u x v di una superficie", CmdArraySrf);
}

}  // namespace cad
