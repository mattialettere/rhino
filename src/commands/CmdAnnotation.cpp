// Annotazioni di Rhino 8: Text, Dim, DimAligned, DimRotated, DimRadius,
// DimDiameter, DimAngle, Leader, Centermark. Gli oggetti sono quelli di
// openNURBS (ON_Text, ON_DimLinear, ...), quindi si salvano nei .3dm come in Rhino.
#include "commands/Annotation.h"
#include "commands/Engine.h"
#include <cmath>
#include <sstream>

namespace cad {
double CurveLengthOf(const ON_Curve& c);
namespace {

const ON_DimStyle& CurrentStyle(Engine& e) {
  // stile corrente (DimStyle), altrimenti quello predefinito del modello
  // stile corrente scelto con DimStyle (segnato da una stringa utente); lo stile
  // creato con il documento non viene usato, per non cambiare le quote esistenti
  const ON_UUID cur = e.Model().CurrentDimensionStyleId();
  const auto* ds = ON_UuidIsNil(cur) ? nullptr : ON_DimStyle::Cast(e.Model().DimensionStyleFromId(cur).ModelComponent());
  ON_wString mark;
  if (ds && !ds->GetUserString(L"Kurva3D.DimStyle", mark)) ds = nullptr;
  if (!ds) ds = ON_DimStyle::Cast(e.Model().DefaultDimensionStyle().ModelComponent());
  return ds ? *ds : ON_DimStyle::Default;
}

// Applica Height= come sostituzione dello stile (come la casella Altezza di Rhino).
void ApplyHeight(Engine& e, ON_Annotation& a, const std::map<std::string, std::string>& opts) {
  auto it = opts.find("height");
  if (it == opts.end()) return;
  const double h = std::atof(it->second.c_str());
  if (h > 0) a.SetTextHeight(&CurrentStyle(e), h);
}

// Piano dell'annotazione: parallelo al piano XY del mondo (piano di costruzione Top).
ON_Plane PlaneAt(const ON_3dPoint& p) {
  ON_Plane pl = ON_Plane::World_xy;
  pl.origin = p;
  pl.UpdateEquation();
  return pl;
}

std::string Num(double v) {
  std::ostringstream s;
  s.precision(10);
  s << (std::fabs(v) < 1e-12 ? 0.0 : v);
  return s.str();
}

std::string Unquote(std::string s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
  return s;
}

ON_UUID AddAnnotation(Engine& e, ON_Annotation* a) {
  // Le quote nascono senza testo: lo costruiamo subito (valore formattato),
  // cosi' l'oggetto in memoria e' identico a quello riletto dal .3dm.
  if (auto* d = ON_Dimension::Cast(a)) {
    const ON_DimStyle& parent = CurrentStyle(e);
    if (auto* an = ON_DimAngular::Cast(d)) an->UpdateDimensionText(&d->DimensionStyle(parent));
    else d->UpdateDimensionText(e.Model().m_settings.m_ModelUnitsAndTolerances.m_unit_system.UnitSystem(),
                                &d->DimensionStyle(parent));
  }
  ON_UUID id = e.AddObject(a);
  e.ClearSelection();
  return id;
}

// Text: punto, testo, Height=
bool CmdText(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto p = e.ReadPoint(ts, "punto iniziale del testo");
  if (!p) return false;
  auto s = e.ReadString(ts, "testo (tra virgolette se contiene spazi; \\n per andare a capo)");
  if (!s) return false;
  std::string txt = Unquote(*s);
  for (size_t i; (i = txt.find("\\n")) != std::string::npos;) txt.replace(i, 2, "\n");
  for (auto& [k, v] : e.ReadOptions(ts)) opts[k] = v;
  e.SkipEnter(ts);
  auto* t = new ON_Text();
  const ON_DimStyle& st = CurrentStyle(e);
  if (!t->Create(ON_wString(txt.c_str()), &st, PlaneAt(*p))) { delete t; return e.Fail("testo non valido"); }
  t->SetDimensionStyleId(st.Id());
  ApplyHeight(e, *t, opts);
  AddAnnotation(e, t);
  e.Out() << "  Creato testo \"" << txt << "\"\n";
  return true;
}

void ReportDim(Engine& e, const ON_Dimension& d, double value, const char* what) {
  e.Out() << "  " << what << " = " << Num(value) << "\n";
  (void)d;
}

// Dim: quota lineare orizzontale o verticale (decisa da dove si pone la linea di quota).
bool CmdDim(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto a = e.ReadPoint(ts, "origine della prima linea di estensione");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "origine della seconda linea di estensione");
  if (!b) return false;
  auto c = e.ReadPoint(ts, "posizione della linea di quota");
  if (!c) return false;
  e.SkipEnter(ts);
  const double xlo = std::min(a->x, b->x), xhi = std::max(a->x, b->x);
  const double ylo = std::min(a->y, b->y), yhi = std::max(a->y, b->y);
  // come Rhino: linea di quota sopra/sotto i punti -> orizzontale, a lato -> verticale
  bool vertical = false;
  if (c->x < xlo || c->x > xhi) vertical = !(c->y < ylo || c->y > yhi) || std::fabs(xhi - xlo) < std::fabs(yhi - ylo);
  if (auto it = opts.find("direction"); it != opts.end()) vertical = it->second == "vertical" || it->second == "v";
  const ON_DimStyle& st = CurrentStyle(e);
  ON_Line dl = vertical ? ON_Line(*c, *c + ON_3dVector::YAxis) : ON_Line(*c, *c + ON_3dVector::XAxis);
  ON_DimLinear* d = ON_DimLinear::CreateRotated(*a, *b, dl, ON_3dVector::ZAxis, st.Id(), nullptr);
  if (!d) return e.Fail("quota non valida");
  ApplyHeight(e, *d, opts);
  AddAnnotation(e, d);
  ReportDim(e, *d, d->Measurement(), vertical ? "Quota verticale" : "Quota orizzontale");
  return true;
}

bool CmdDimAligned(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto a = e.ReadPoint(ts, "origine della prima linea di estensione");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "origine della seconda linea di estensione");
  if (!b) return false;
  auto c = e.ReadPoint(ts, "posizione della linea di quota");
  if (!c) return false;
  e.SkipEnter(ts);
  if (a->DistanceTo(*b) < e.Tolerance()) return e.Fail("i due punti coincidono");
  ON_DimLinear* d = ON_DimLinear::CreateAligned(*a, *b, *c, ON_3dVector::ZAxis, CurrentStyle(e).Id(), nullptr);
  if (!d) return e.Fail("quota non valida");
  ApplyHeight(e, *d, opts);
  AddAnnotation(e, d);
  ReportDim(e, *d, d->Measurement(), "Quota allineata");
  return true;
}

// DimRotated: angolo della linea di quota (gradi), poi i tre punti.
bool CmdDimRotated(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto ang = e.ReadNumber(ts, "angolo della linea di quota (gradi)");
  if (!ang) return false;
  auto a = e.ReadPoint(ts, "origine della prima linea di estensione");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "origine della seconda linea di estensione");
  if (!b) return false;
  auto c = e.ReadPoint(ts, "posizione della linea di quota");
  if (!c) return false;
  e.SkipEnter(ts);
  const double r = *ang * ON_PI / 180.0;
  ON_Line dl(*c, *c + ON_3dVector(std::cos(r), std::sin(r), 0));
  ON_DimLinear* d = ON_DimLinear::CreateRotated(*a, *b, dl, ON_3dVector::ZAxis, CurrentStyle(e).Id(), nullptr);
  if (!d) return e.Fail("quota non valida");
  ApplyHeight(e, *d, opts);
  AddAnnotation(e, d);
  ReportDim(e, *d, d->Measurement(), "Quota ruotata");
  return true;
}

// Arco o cerchio selezionato (per DimRadius/DimDiameter/Centermark).
bool SelectedCircle(Engine& e, const char* cmd, ON_Arc& arc) {
  auto sel = e.RequireSelection(cmd, "seleziona un arco o un cerchio");
  if (sel.empty()) return false;
  for (const auto& id : sel) {
    const auto* o = e.Object(id);
    const ON_Curve* c = o ? ON_Curve::Cast(o->Geometry(nullptr)) : nullptr;
    if (c && c->IsArc(nullptr, &arc, e.Tolerance())) return true;
  }
  return e.Fail("la selezione non contiene archi o cerchi");
}

bool RadialDim(Engine& e, TokenStream& ts, bool diameter) {
  auto opts = e.ReadOptions(ts);
  ON_Arc arc;
  if (!SelectedCircle(e, diameter ? "DimDiameter" : "DimRadius", arc)) return false;
  auto p = e.ReadPoint(ts, "posizione della quota");
  if (!p) return false;
  e.SkipEnter(ts);
  ON_Plane pl = arc.plane;
  pl.origin = arc.Center();
  pl.UpdateEquation();
  // punto sul cerchio nella direzione del punto scelto
  ON_3dVector v = pl.ClosestPointTo(*p) - arc.Center();
  if (!v.Unitize()) v = pl.xaxis;
  const ON_3dPoint rp = arc.Center() + v * arc.Radius();
  auto* d = new ON_DimRadial();
  if (!d->Create(diameter ? ON::AnnotationType::Diameter : ON::AnnotationType::Radius, CurrentStyle(e).Id(), pl,
                 arc.Center(), rp, pl.ClosestPointTo(*p))) {
    delete d;
    return e.Fail("quota non valida");
  }
  ApplyHeight(e, *d, opts);
  AddAnnotation(e, d);
  ReportDim(e, *d, d->Measurement(), diameter ? "Diametro" : "Raggio");
  return true;
}
bool CmdDimRadius(Engine& e, TokenStream& ts) { return RadialDim(e, ts, false); }
bool CmdDimDiameter(Engine& e, TokenStream& ts) { return RadialDim(e, ts, true); }

// DimAngle: vertice, punto sul primo lato, punto sul secondo lato, posizione dell'arco.
bool CmdDimAngle(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto c = e.ReadPoint(ts, "vertice dell'angolo");
  if (!c) return false;
  auto a = e.ReadPoint(ts, "punto sul primo lato");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "punto sul secondo lato");
  if (!b) return false;
  auto p = e.ReadPoint(ts, "posizione dell'arco di quota");
  if (!p) return false;
  e.SkipEnter(ts);
  auto* d = new ON_DimAngular();
  if (!d->Create(CurrentStyle(e).Id(), PlaneAt(*c), ON_3dVector::XAxis, *c, *a, *b, *p)) {
    delete d;
    return e.Fail("quota angolare non valida");
  }
  ApplyHeight(e, *d, opts);
  AddAnnotation(e, d);
  ReportDim(e, *d, d->Measurement() * 180.0 / ON_PI, "Angolo (gradi)");
  return true;
}

// Testo della lunghezza con il simbolo d'arco davanti (stile di Rhino: simbolo prima del testo).
ON_wString ArcLengthText(Engine& e, double length) {
  ON_wString t;
  ON_TextContent::FormatDistanceMeasurement(length, e.Model().m_settings.m_ModelUnitsAndTolerances.m_unit_system.UnitSystem(),
                                            &CurrentStyle(e), L"<>", t);
  return ON_wString(L"\x2312") + t;
}

// DimArc: quota della lunghezza di un arco (quota angolare sull'arco con la lunghezza come testo).
bool CmdDimArc(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("DimArc", "seleziona un arco");
  if (sel.empty()) return false;
  ON_Arc arc;
  bool found = false;
  for (const auto& id : sel) {
    const auto* o = e.Object(id);
    const ON_Curve* c = o ? ON_Curve::Cast(o->Geometry(nullptr)) : nullptr;
    if (c && c->IsArc(nullptr, &arc, e.Tolerance()) && !arc.IsCircle()) { found = true; break; }
  }
  if (!found) return e.Fail("DimArc: la selezione non contiene archi (i cerchi non hanno estremi)");
  auto p = e.ReadPoint(ts, "posizione dell'arco di quota");
  if (!p) return false;
  e.SkipEnter(ts);
  ON_Plane pl = arc.plane;
  pl.origin = arc.Center();
  pl.UpdateEquation();
  // punto della linea di quota: in mezzo all'arco, alla distanza dal centro del punto scelto
  const double r = std::max(pl.ClosestPointTo(*p).DistanceTo(arc.Center()), 0.1 * arc.Radius());
  ON_3dVector mid = arc.PointAt(arc.Domain().Mid()) - arc.Center();
  mid.Unitize();
  auto* d = new ON_DimAngular();
  if (!d->Create(CurrentStyle(e).Id(), pl, pl.xaxis, arc.Center(), arc.StartPoint(), arc.EndPoint(), arc.Center() + mid * r)) {
    delete d;
    return e.Fail("quota d'arco non valida");
  }
  const double len = arc.Length();
  d->SetUserText(ArcLengthText(e, len));
  ApplyHeight(e, *d, opts);
  AddAnnotation(e, d);
  ReportDim(e, *d, len, "Lunghezza d'arco");
  return true;
}

// DimCurveLength: quota allineata tra gli estremi di una curva aperta con la lunghezza della curva.
bool CmdDimCurveLength(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("DimCurveLength", "seleziona una curva");
  if (sel.empty()) return false;
  const ON_Curve* c = nullptr;
  for (const auto& id : sel) {
    const auto* o = e.Object(id);
    if (const ON_Curve* cc = o ? ON_Curve::Cast(o->Geometry(nullptr)) : nullptr) { c = cc; break; }
  }
  if (!c) return e.Fail("DimCurveLength: seleziona una curva");
  if (c->IsClosed()) return e.Fail("DimCurveLength: la curva e' chiusa (servono due estremi distinti)");
  auto p = e.ReadPoint(ts, "posizione della linea di quota");
  if (!p) return false;
  e.SkipEnter(ts);
  const double len = CurveLengthOf(*c);
  ON_3dVector n = ON_3dVector::ZAxis;
  ON_Plane cp;
  if (c->IsPlanar(&cp, e.Tolerance())) n = cp.zaxis * (cp.zaxis.z < 0 ? -1.0 : 1.0);
  ON_DimLinear* d = ON_DimLinear::CreateAligned(c->PointAtStart(), c->PointAtEnd(), *p, n, CurrentStyle(e).Id(), nullptr);
  if (!d) return e.Fail("quota non valida");
  d->SetUserText(ArcLengthText(e, len));
  ApplyHeight(e, *d, opts);
  AddAnnotation(e, d);
  ReportDim(e, *d, len, "Lunghezza della curva");
  return true;
}

// Leader: punti (il primo e' la punta della freccia) fino a Invio, poi il testo.
bool CmdLeader(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  ON_3dPointArray pts;
  auto p0 = e.ReadPoint(ts, "punta della freccia");
  if (!p0) return false;
  pts.Append(*p0);
  while (e.MoreInput(ts)) {
    auto p = e.ReadPoint(ts, "punto successivo (Invio per terminare)");
    if (!p) return false;
    pts.Append(*p);
  }
  e.SkipEnter(ts);
  if (pts.Count() < 2) return e.Fail("servono almeno due punti");
  auto s = e.ReadString(ts, "testo della direttrice (\"\" per nessuno)");
  if (!s) return false;
  std::string txt = Unquote(*s);
  if (txt == "\"\"") txt.clear();
  for (size_t i; (i = txt.find("\\n")) != std::string::npos;) txt.replace(i, 2, "\n");
  e.SkipEnter(ts);
  auto* l = new ON_Leader();
  const ON_DimStyle& st = CurrentStyle(e);
  if (!l->Create(ON_wString(txt.c_str()), &st, pts.Count(), pts.Array(), PlaneAt(pts[0]), false, 0.0)) {
    delete l;
    return e.Fail("direttrice non valida");
  }
  l->SetDimensionStyleId(st.Id());
  ApplyHeight(e, *l, opts);
  AddAnnotation(e, l);
  e.Out() << "  Creata direttrice con " << pts.Count() << " punti\n";
  return true;
}

bool CmdCentermark(Engine& e, TokenStream&) {
  ON_Arc arc;
  if (!SelectedCircle(e, "Centermark", arc)) return false;
  ON_Plane pl = arc.plane;
  pl.origin = arc.Center();
  pl.UpdateEquation();
  auto* d = new ON_Centermark();
  if (!d->Create(CurrentStyle(e).Id(), pl, arc.Center(), arc.Radius())) { delete d; return e.Fail("segno del centro non valido"); }
  AddAnnotation(e, d);
  e.Out() << "  Creato segno del centro\n";
  return true;
}

// DimOrdinate: punto da quotare e fine della direttrice; l'origine e' quella
// del piano di costruzione. La direzione misurata (X o Y) segue la direttrice,
// come in Rhino: direttrice verticale -> coordinata X.
bool CmdDimOrdinate(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto p = e.ReadPoint(ts, "punto da quotare");
  if (!p) return false;
  auto l = e.ReadPoint(ts, "fine della direttrice");
  if (!l) return false;
  e.SkipEnter(ts);
  const ON_3dVector d = *l - *p;
  if (d.Length() < e.Tolerance()) return e.Fail("la direttrice e' nulla");
  const auto dir = std::fabs(d.y) >= std::fabs(d.x) ? ON_DimOrdinate::MeasuredDirection::Xaxis
                                                    : ON_DimOrdinate::MeasuredDirection::Yaxis;
  ON_Plane pl = ON_xy_plane;
  pl.origin.z = p->z;
  pl.UpdateEquation();
  const ON_DimStyle& st = CurrentStyle(e);
  const double kink = 2.0 * st.TextHeight();
  auto* dim = new ON_DimOrdinate();
  if (!dim->Create(st.Id(), pl, dir, pl.origin, *p, *l, kink, kink)) { delete dim; return e.Fail("quota non valida"); }
  ApplyHeight(e, *dim, opts);
  AddAnnotation(e, dim);
  ReportDim(e, *dim, dim->Measurement(), dir == ON_DimOrdinate::MeasuredDirection::Xaxis ? "Quota coordinata X" : "Quota coordinata Y");
  return true;
}

}  // namespace

void RegisterAnnotationCommands(Engine& e) {
  auto reg = [&](const char* n, const char* d, CommandFn f) {
    CommandInfo i{n, "Annotazioni", d, f};
    i.modifiesModel = true;
    e.Register(i);
  };
  reg("Text", "Testo (punto, \"testo\"; Height=)", CmdText);
  reg("Dim", "Quota lineare orizzontale o verticale (due punti, posizione)", CmdDim);
  reg("DimAligned", "Quota allineata (due punti, posizione)", CmdDimAligned);
  reg("DimRotated", "Quota ruotata (angolo, due punti, posizione)", CmdDimRotated);
  reg("DimRadius", "Quota del raggio di un arco o cerchio", CmdDimRadius);
  reg("DimDiameter", "Quota del diametro di un arco o cerchio", CmdDimDiameter);
  reg("DimAngle", "Quota angolare (vertice, due punti, posizione)", CmdDimAngle);
  reg("Leader", "Direttrice con freccia e testo", CmdLeader);
  reg("Centermark", "Segno del centro di archi e cerchi", CmdCentermark);
  reg("DimOrdinate", "Quota coordinata (punto, fine della direttrice)", CmdDimOrdinate);
  reg("DimArc", "Quota della lunghezza di un arco (arco, posizione)", CmdDimArc);
  reg("DimCurveLength", "Quota della lunghezza di una curva (curva, posizione)", CmdDimCurveLength);
}

}  // namespace cad
