// Altri comandi di Rhino 8 realizzati con openNURBS:
// analisi (Length, Distance, Angle, Radius, BoundingBox), curve (Divide, Rebuild,
// ChangeDegree, ProjectToCPlane, Helix, Spiral), gruppi e blocchi.
#include "commands/Engine.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <memory>
#include <cstdio>
#include <sstream>

namespace cad {
namespace {

ON_3dmObjectAttributes AttrsOf(Engine& e, const ON_UUID& id) {
  return *e.Object(id)->Attributes(&ON_3dmObjectAttributes::DefaultAttributes);
}

const ON_Geometry* Geo(Engine& e, const ON_UUID& id) {
  const auto* o = e.Object(id);
  return o ? o->Geometry(nullptr) : nullptr;
}

std::string Num(double v) {
  std::ostringstream s;
  s << std::setprecision(10) << (std::fabs(v) < 1e-12 ? 0.0 : v);
  return s.str();
}

// Lunghezza di un tratto di curva: Gauss-Legendre a 5 punti su ogni span, suddiviso.
double LengthBetween(const ON_Curve& c, double t0, double t1) {
  if (t1 <= t0) return 0;
  static const double gx[5] = {-0.9061798459386640, -0.5384693101056831, 0.0, 0.5384693101056831, 0.9061798459386640};
  static const double gw[5] = {0.2369268850561891, 0.4786286704993665, 0.5688888888888889, 0.4786286704993665, 0.2369268850561891};
  std::vector<double> spans(c.SpanCount() + 1);
  c.GetSpanVector(spans.data());
  std::vector<double> cuts{t0};
  for (double s : spans) if (s > t0 && s < t1) cuts.push_back(s);
  cuts.push_back(t1);
  double L = 0;
  for (size_t k = 0; k + 1 < cuts.size(); k++) {
    const int sub = 16;
    for (int j = 0; j < sub; j++) {
      const double a = cuts[k] + (cuts[k + 1] - cuts[k]) * j / sub, b = cuts[k] + (cuts[k + 1] - cuts[k]) * (j + 1) / sub;
      const double m = 0.5 * (a + b), h = 0.5 * (b - a);
      for (int g = 0; g < 5; g++) {
        ON_3dPoint p; ON_3dVector d;
        // lato del limite di span: valuta dall'interno
        c.Ev1Der(m + h * gx[g], p, d, 0);
        L += gw[g] * h * d.Length();
      }
    }
  }
  return L;
}

double CurveLength(const ON_Curve& c) { return LengthBetween(c, c.Domain()[0], c.Domain()[1]); }

// Parametro alla lunghezza s dall'inizio (bisezione).
double ParamAtLength(const ON_Curve& c, double s) {
  double lo = c.Domain()[0], hi = c.Domain()[1];
  for (int i = 0; i < 60; i++) {
    const double mid = 0.5 * (lo + hi);
    if (LengthBetween(c, c.Domain()[0], mid) < s) lo = mid; else hi = mid;
  }
  return 0.5 * (lo + hi);
}

std::vector<std::pair<ON_UUID, const ON_Curve*>> SelectedCurves(Engine& e, const std::vector<ON_UUID>& sel) {
  std::vector<std::pair<ON_UUID, const ON_Curve*>> out;
  for (const auto& id : sel)
    if (const ON_Curve* c = ON_Curve::Cast(Geo(e, id))) out.push_back({id, c});
  return out;
}

// --- Analisi -------------------------------------------------------------------
bool CmdLength(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("Length", "seleziona le curve da misurare, poi Invio");
  if (sel.empty()) return false;
  auto crvs = SelectedCurves(e, sel);
  if (crvs.empty()) return e.Fail("nessuna curva selezionata");
  double tot = 0;
  for (auto& [id, c] : crvs) tot += CurveLength(*c);
  e.Out() << "  Lunghezza = " << Num(tot) << " mm\n";
  return true;
}

bool CmdDistance(Engine& e, TokenStream& ts) {
  auto a = e.ReadPoint(ts, "primo punto per la distanza");
  if (!a) return false;
  e.lastPoint = *a;
  auto b = e.ReadPoint(ts, "secondo punto per la distanza");
  if (!b) return false;
  const ON_3dVector d = *b - *a;
  e.Out() << "  Distanza = " << Num(d.Length()) << " mm   dx = " << Num(d.x) << "  dy = " << Num(d.y)
          << "  dz = " << Num(d.z) << "\n";
  return true;
}

// Angle: vertice, punto sul primo lato, punto sul secondo lato.
bool CmdAngle(Engine& e, TokenStream& ts) {
  auto v = e.ReadPoint(ts, "vertice dell'angolo");
  if (!v) return false;
  e.lastPoint = *v;
  auto a = e.ReadPoint(ts, "punto sul primo lato");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "punto sul secondo lato");
  if (!b) return false;
  ON_3dVector u = *a - *v, w = *b - *v;
  if (!u.Unitize() || !w.Unitize()) return e.Fail("punti coincidenti");
  const double ang = std::acos(std::clamp(u * w, -1.0, 1.0)) * 180.0 / ON_PI;
  e.Out() << "  Angolo = " << Num(ang) << " gradi\n";
  return true;
}

bool CmdRadius(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("Radius", "seleziona archi o cerchi, poi Invio");
  if (sel.empty()) return false;
  int n = 0;
  for (auto& [id, c] : SelectedCurves(e, sel)) {
    ON_Arc arc;
    if (const auto* ac = ON_ArcCurve::Cast(c)) arc = ac->m_arc;
    else if (!c->IsArc(nullptr, &arc, e.Tolerance())) continue;
    e.Out() << "  Raggio = " << Num(arc.Radius()) << " mm   Diametro = " << Num(2 * arc.Radius()) << " mm\n";
    n++;
  }
  if (!n) return e.Fail("nessun arco o cerchio selezionato");
  return true;
}

// BoundingBox: polisuperficie scatola (rettangolo se gli oggetti sono piatti), assi del mondo.
bool CmdBoundingBox(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("BoundingBox", "seleziona gli oggetti, poi Invio");
  if (sel.empty()) return false;
  ON_BoundingBox bb;
  for (const auto& id : sel)
    if (const ON_Geometry* g = Geo(e, id)) { ON_BoundingBox t; g->GetTightBoundingBox(t); bb.Union(t); }
  if (!bb.IsValid()) return e.Fail("bounding box non valida");
  const double tol = e.Tolerance();
  const ON_3dVector size = bb.Diagonal();
  if (size.z <= tol) {
    ON_Polyline pl;
    pl.Append(ON_3dPoint(bb.m_min.x, bb.m_min.y, bb.m_min.z));
    pl.Append(ON_3dPoint(bb.m_max.x, bb.m_min.y, bb.m_min.z));
    pl.Append(ON_3dPoint(bb.m_max.x, bb.m_max.y, bb.m_min.z));
    pl.Append(ON_3dPoint(bb.m_min.x, bb.m_max.y, bb.m_min.z));
    pl.Append(pl[0]);
    e.AddObject(new ON_PolylineCurve(pl));
    e.Out() << "  Creato rettangolo di ingombro " << Num(size.x) << " x " << Num(size.y) << "\n";
    return true;
  }
  ON_3dPoint c[8] = {ON_3dPoint(bb.m_min.x, bb.m_min.y, bb.m_min.z), ON_3dPoint(bb.m_max.x, bb.m_min.y, bb.m_min.z),
                     ON_3dPoint(bb.m_max.x, bb.m_max.y, bb.m_min.z), ON_3dPoint(bb.m_min.x, bb.m_max.y, bb.m_min.z),
                     ON_3dPoint(bb.m_min.x, bb.m_min.y, bb.m_max.z), ON_3dPoint(bb.m_max.x, bb.m_min.y, bb.m_max.z),
                     ON_3dPoint(bb.m_max.x, bb.m_max.y, bb.m_max.z), ON_3dPoint(bb.m_min.x, bb.m_max.y, bb.m_max.z)};
  ON_Brep* b = ON_BrepBox(c);
  if (!b) return e.Fail("impossibile creare la scatola");
  e.AddObject(b);
  e.Out() << "  Creata scatola di ingombro " << Num(size.x) << " x " << Num(size.y) << " x " << Num(size.z) << "\n";
  return true;
}

// --- Curve -------------------------------------------------------------------------
// Divide: punti a lunghezze uguali (numero di segmenti, oppure Length=)
bool CmdDivide(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("Divide", "seleziona le curve da dividere, poi Invio");
  if (sel.empty()) return false;
  auto crvs = SelectedCurves(e, sel);
  if (crvs.empty()) return e.Fail("nessuna curva selezionata");
  double segLen = opts.count("length") ? std::atof(opts["length"].c_str()) : 0;
  int segs = 0;
  if (segLen <= 0) {
    auto n = e.ReadInt(ts, "numero di segmenti");
    if (!n) return false;
    if (*n < 1) return e.Fail("numero di segmenti non valido");
    segs = *n;
  }
  int pts = 0;
  for (auto& [id, c] : crvs) {
    const double L = CurveLength(*c);
    int n = segs;
    double step = segLen;
    if (segs) step = L / segs; else n = int(std::floor(L / segLen + 1e-9));
    const bool closed = c->IsClosed();
    for (int i = 0; i <= n; i++) {
      if (closed && i == n && segs) break;  // su una curva chiusa l'ultimo coincide col primo
      const double s = std::min(L, i * step);
      e.AddObject(new ON_Point(c->PointAt(i == 0 ? c->Domain()[0] : ParamAtLength(*c, s))));
      pts++;
    }
  }
  e.Out() << "  Creati " << pts << " punti\n";
  return true;
}

// Measure: come Divide con Length=, la lunghezza si chiede come valore.
bool CmdMeasure(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("Measure", "seleziona le curve, poi Invio");
  if (sel.empty()) return false;
  auto len = e.ReadNumber(ts, "lunghezza dei tratti");
  if (!len) return false;
  if (*len <= 0) return e.Fail("lunghezza non valida");
  TokenStream sub("Length=" + std::to_string(*len));
  return CmdDivide(e, sub);
}

// Minimi quadrati: n punti di controllo, grado p, nodi uniformi, estremi fissi.
ON_NurbsCurve* FitCurve(const ON_Curve& c, int n, int p) {
  if (n < p + 1) n = p + 1;
  const int m = std::max(10 * n, 200);
  std::vector<ON_3dPoint> Q(m + 1);
  const double L = CurveLength(c);
  for (int i = 0; i <= m; i++) Q[i] = c.PointAt(i == 0 ? c.Domain()[0] : i == m ? c.Domain()[1] : ParamAtLength(c, L * i / m));
  auto* nc = new ON_NurbsCurve(3, false, p + 1, n);
  // nodi uniformi (formato openNURBS: n+p-1 nodi)
  const int kc = nc->KnotCount();
  for (int i = 0; i < kc; i++) {
    int k = i - (p - 1);
    nc->m_knot[i] = std::clamp(double(k) / (n - p), 0.0, 1.0);
  }
  // funzioni base sui parametri uniformi
  auto basis = [&](double u, std::vector<double>& N) {
    N.assign(n, 0.0);
    ON_NurbsCurve tmp(1, false, p + 1, n);
    for (int i = 0; i < kc; i++) tmp.m_knot[i] = nc->m_knot[i];
    for (int j = 0; j < n; j++) {
      for (int k = 0; k < n; k++) tmp.SetCV(k, ON_3dPoint(k == j ? 1.0 : 0.0, 0, 0));
      N[j] = tmp.PointAt(u).x;
    }
  };
  std::vector<std::vector<double>> A(n, std::vector<double>(n, 0.0));
  std::vector<ON_3dVector> B(n, ON_3dVector::ZeroVector);
  std::vector<double> N;
  for (int i = 0; i <= m; i++) {
    basis(double(i) / m, N);
    for (int a = 0; a < n; a++) {
      if (N[a] == 0) continue;
      for (int b = 0; b < n; b++) A[a][b] += N[a] * N[b];
      B[a] = B[a] + N[a] * ON_3dVector(Q[i]);
    }
  }
  // estremi fissi
  for (int idx : {0, n - 1}) {
    for (int b = 0; b < n; b++) A[idx][b] = (b == idx) ? 1.0 : 0.0;
    B[idx] = ON_3dVector(idx == 0 ? Q[0] : Q[m]);
  }
  // eliminazione di Gauss con pivot
  for (int col = 0; col < n; col++) {
    int piv = col;
    for (int r = col + 1; r < n; r++) if (std::fabs(A[r][col]) > std::fabs(A[piv][col])) piv = r;
    std::swap(A[col], A[piv]);
    std::swap(B[col], B[piv]);
    if (std::fabs(A[col][col]) < 1e-14) { delete nc; return nullptr; }
    for (int r = 0; r < n; r++) {
      if (r == col) continue;
      const double f = A[r][col] / A[col][col];
      if (f == 0) continue;
      for (int k = col; k < n; k++) A[r][k] -= f * A[col][k];
      B[r] = B[r] - f * B[col];
    }
  }
  for (int i = 0; i < n; i++) nc->SetCV(i, ON_3dPoint(B[i] / A[i][i]));
  nc->SetDomain(c.Domain()[0], c.Domain()[1]);
  return nc;
}

bool CmdRebuild(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  const int n = opts.count("pointcount") ? std::atoi(opts["pointcount"].c_str()) : 10;
  const int p = opts.count("degree") ? std::atoi(opts["degree"].c_str()) : 3;
  if (p < 1 || p > 11 || n < p + 1) return e.Fail("PointCount deve essere almeno Degree+1");
  auto sel = e.RequireSelection("Rebuild", "seleziona le curve da ricostruire, poi Invio");
  if (sel.empty()) return false;
  int k = 0;
  for (auto& [id, c] : SelectedCurves(e, sel)) {
    if (c->IsClosed()) continue;  // le curve chiuse richiedono il fitting periodico (non ancora)
    ON_NurbsCurve* nc = FitCurve(*c, n, p);
    if (!nc) continue;
    ON_3dmObjectAttributes a = AttrsOf(e, id);
    e.DeleteObject(id);
    ON_UUID nid = e.AddObject(nc, &a);
    e.Select(nid);
    k++;
  }
  if (!k) return e.Fail("nessuna curva aperta ricostruita");
  e.Out() << "  Ricostruite " << k << " curve (" << n << " punti, grado " << p << ")\n";
  return true;
}

bool CmdChangeDegree(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("ChangeDegree", "seleziona le curve, poi Invio");
  if (sel.empty()) return false;
  auto deg = e.ReadInt(ts, "nuovo grado");
  if (!deg) return false;
  if (*deg < 1 || *deg > 11) return e.Fail("grado non valido (1-11)");
  int k = 0;
  for (auto& [id, c] : SelectedCurves(e, sel)) {
    auto* nc = new ON_NurbsCurve();
    if (!c->GetNurbForm(*nc)) { delete nc; continue; }
    if (*deg > nc->Degree()) {
      nc->IncreaseDegree(*deg);
    } else if (*deg < nc->Degree()) {
      // diminuire il grado cambia la forma: approssimazione con lo stesso numero di punti
      const int cv = std::max(nc->CVCount(), *deg + 1);
      delete nc;
      if (c->IsClosed()) continue;
      nc = FitCurve(*c, cv, *deg);
      if (!nc) continue;
    }
    ON_3dmObjectAttributes a = AttrsOf(e, id);
    e.DeleteObject(id);
    e.Select(e.AddObject(nc, &a));
    k++;
  }
  if (!k) return e.Fail("nessuna curva modificata");
  e.Out() << "  Grado cambiato a " << *deg << " su " << k << " curve\n";
  return true;
}

bool CmdProjectToCPlane(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("ProjectToCPlane", "seleziona gli oggetti da proiettare, poi Invio");
  if (sel.empty()) return false;
  ON_Xform xf = ON_Xform::IdentityTransformation;
  xf.m_xform[2][2] = 0.0;  // z = 0 (piano di costruzione Top)
  int k = 0;
  for (const auto& id : sel)
    if (ON_Geometry* g = e.EditableGeometry(id)) k += g->Transform(xf) ? 1 : 0;
  e.Out() << "  Proiettati " << k << " oggetti sul piano di costruzione\n";
  return k > 0;
}

// Elica e spirale: punti campionati e curva interpolata cubica (Rhino crea una NURBS simile).
ON_NurbsCurve* SpiralCurve(const ON_3dPoint& a, const ON_3dPoint& b, const ON_3dPoint& startDir, double r0, double r1,
                           double turns) {
  ON_3dVector axis = b - a;
  const double h = axis.Length();
  ON_Plane pl;
  if (h > 0) {
    axis.Unitize();
    ON_3dVector x = startDir - a;
    x = x - (x * axis) * axis;
    if (!x.Unitize()) { x.PerpendicularTo(axis); x.Unitize(); }
    pl = ON_Plane(a, x, ON_CrossProduct(axis, x));
  } else {
    ON_3dVector x = startDir - a;
    x.z = 0;
    if (!x.Unitize()) x = ON_3dVector::XAxis;
    pl = ON_Plane(a, x, ON_CrossProduct(ON_3dVector::ZAxis, x));
  }
  const int n = std::max(8, int(std::ceil(turns * 16)));
  std::vector<ON_3dPoint> pts;
  for (int i = 0; i <= n; i++) {
    const double t = double(i) / n, ang = 2 * ON_PI * turns * t, r = r0 + (r1 - r0) * t;
    pts.push_back(pl.PointAt(r * std::cos(ang), r * std::sin(ang)) + pl.zaxis * (h * t));
  }
  return InterpolateCubic(pts);
}

bool CmdHelix(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  const double turns = opts.count("turns") ? std::atof(opts["turns"].c_str()) : 10.0;
  auto a = e.ReadPoint(ts, "inizio dell'asse dell'elica");
  if (!a) return false;
  e.lastPoint = *a;
  auto b = e.ReadPoint(ts, "fine dell'asse");
  if (!b) return false;
  auto r = e.ReadPoint(ts, "raggio e punto iniziale");
  if (!r) return false;
  if (a->DistanceTo(*b) <= e.Tolerance()) return e.Fail("asse nullo");
  ON_3dVector ax = *b - *a; ax.Unitize();
  ON_3dVector rv = *r - *a; rv = rv - (rv * ax) * ax;
  const double rad = rv.Length();
  if (rad <= e.Tolerance() || turns <= 0) return e.Fail("raggio o giri non validi");
  ON_NurbsCurve* c = SpiralCurve(*a, *b, *r, rad, rad, turns);
  if (!c) return e.Fail("elica non riuscita");
  e.AddObject(c);
  e.Out() << "  Creata elica, " << Num(turns) << " giri, raggio " << Num(rad) << "\n";
  return true;
}

bool CmdSpiral(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts, {"flat"});
  const double turns = opts.count("turns") ? std::atof(opts["turns"].c_str()) : 10.0;
  const bool flat = opts.count("flat") && opts["flat"] != "no";
  auto a = e.ReadPoint(ts, flat ? "centro della spirale" : "inizio dell'asse della spirale");
  if (!a) return false;
  e.lastPoint = *a;
  ON_3dPoint b = *a;
  if (!flat) {
    auto bp = e.ReadPoint(ts, "fine dell'asse");
    if (!bp) return false;
    b = *bp;
  }
  auto r0 = e.ReadPoint(ts, "primo raggio e punto iniziale");
  if (!r0) return false;
  auto r1 = e.ReadNumber(ts, "secondo raggio");
  if (!r1) return false;
  ON_3dVector ax = b - *a;
  ON_3dVector rv = *r0 - *a;
  if (ax.Unitize()) rv = rv - (rv * ax) * ax; else rv.z = 0;
  ON_NurbsCurve* c = SpiralCurve(*a, b, *r0, rv.Length(), *r1, turns);
  if (!c) return e.Fail("spirale non riuscita");
  e.AddObject(c);
  e.Out() << "  Creata spirale, " << Num(turns) << " giri\n";
  return true;
}

// Nome libero "Base 01", "Base 02"... per una tabella
ON_wString FreeName(Engine& e, ON_ModelComponent::Type type, const char* base) {
  for (int i = 1; i < 100000; i++) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s %02d", base, i);
    ON_wString w(buf);
    bool used = false;
    ONX_ModelComponentIterator it(e.Model(), type);
    for (const ON_ModelComponent* c = it.FirstComponent(); c && !used; c = it.NextComponent())
      used = ON_wString::EqualOrdinal(c->Name(), w, true);
    if (!used) return w;
  }
  return ON_wString(base);
}

// --- Gruppi ------------------------------------------------------------------------
bool CmdGroup(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("Group", "seleziona gli oggetti da raggruppare, poi Invio");
  if (sel.empty()) return false;
  ONX_Model& m = e.Model();
  ON_Group g;
  g.SetName(FreeName(e, ON_ModelComponent::Type::Group, "Gruppo"));
  ON_ModelComponentReference ref = m.AddModelComponent(g, true);
  const ON_ModelComponent* gc = ref.ModelComponent();
  if (!gc) return e.Fail("impossibile creare il gruppo");
  const int gi = gc->Index();
  for (const auto& id : sel)
    if (auto* a = e.EditableAttributes(id)) a->AddToGroup(gi);
  e.Out() << "  Raggruppati " << sel.size() << " oggetti\n";
  return true;
}

bool CmdUngroup(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("Ungroup", "seleziona i gruppi, poi Invio");
  if (sel.empty()) return false;
  int k = 0;
  for (const auto& id : sel)
    if (auto* a = e.EditableAttributes(id); a && a->GroupCount() > 0) { a->RemoveFromAllGroups(); k++; }
  e.Out() << "  Separati " << k << " oggetti dai gruppi\n";
  return true;
}

// --- Blocchi -----------------------------------------------------------------------
const ON_InstanceDefinition* FindBlock(Engine& e, const std::string& name) {
  ON_wString w(name.c_str());
  ONX_ModelComponentIterator it(e.Model(), ON_ModelComponent::Type::InstanceDefinition);
  for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent())
    if (ON_wString::EqualOrdinal(c->Name(), w, true)) return ON_InstanceDefinition::Cast(c);
  return nullptr;
}

ON_BoundingBox BlockBox(Engine& e, const ON_InstanceDefinition& idef) {
  ON_BoundingBox bb;
  const auto& ids = idef.InstanceGeometryIdList();
  for (int i = 0; i < ids.Count(); i++) {
    const auto* o = ON_ModelGeometryComponent::Cast(e.Model().ComponentFromId(ON_ModelComponent::Type::ModelGeometry, ids[i]).ModelComponent());
    if (const ON_Geometry* g = o ? o->Geometry(nullptr) : nullptr) { ON_BoundingBox t; g->GetTightBoundingBox(t); bb.Union(t); }
  }
  return bb;
}

ON_UUID InsertBlock(Engine& e, const ON_InstanceDefinition& idef, const ON_Xform& xf) {
  auto* ir = new ON_InstanceRef();
  ir->m_instance_definition_uuid = idef.Id();
  ir->m_xform = xf;
  ON_BoundingBox bb = BlockBox(e, idef);
  if (bb.IsValid()) { bb.Transform(xf); ir->m_bbox = bb; }
  return e.AddObject(ir);
}

// Block: oggetti selezionati -> definizione di blocco (punto base = origine della definizione)
// e un'istanza al loro posto. Opzione Name=.
bool CmdBlock(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("Block", "seleziona gli oggetti del blocco, poi Invio");
  if (sel.empty()) return false;
  auto base = e.ReadPoint(ts, "punto base del blocco");
  if (!base) return false;
  ONX_Model& m = e.Model();
  std::string name;
  if (auto it = opts.find("name"); it != opts.end()) name = it->second;
  ON_wString wname = name.empty()
      ? FreeName(e, ON_ModelComponent::Type::InstanceDefinition, "Blocco")
      : ON_wString(name.c_str());
  if (FindBlock(e, ToUtf8(wname))) return e.Fail("esiste gia' un blocco con questo nome");
  const ON_Xform toBase = ON_Xform::TranslationTransformation(-ON_3dVector(*base));
  ON_SimpleArray<ON_UUID> members;
  for (const auto& id : sel) {
    const ON_Geometry* g = Geo(e, id);
    if (!g) continue;
    ON_Geometry* copy = static_cast<ON_Geometry*>(g->Duplicate());
    copy->Transform(toBase);
    ON_3dmObjectAttributes a = AttrsOf(e, id);
    a.SetMode(ON::object_mode::idef_object);
    ON_UUID nid = e.AddObject(copy, &a);
    members.Append(nid);
    e.DeleteObject(id);
  }
  auto* idef = new ON_InstanceDefinition();
  idef->SetName(wname);
  idef->SetInstanceGeometryIdList(members);
  ON_BoundingBox bb = BlockBox(e, *idef);
  idef->SetBoundingBox(bb);
  ON_ModelComponentReference ref = m.AddManagedModelComponent(idef, true);
  if (!ref.ModelComponent()) return e.Fail("impossibile creare il blocco");
  const auto* def = ON_InstanceDefinition::Cast(ref.ModelComponent());
  ON_UUID inst = InsertBlock(e, *def, ON_Xform::TranslationTransformation(ON_3dVector(*base)));
  e.ClearSelection();
  e.Select(inst);
  e.Out() << "  Creato blocco \"" << ToUtf8(wname) << "\" con " << members.Count() << " oggetti\n";
  return true;
}

// Insert: nome, punto di inserimento, scala, rotazione (gradi).
bool CmdInsert(Engine& e, TokenStream& ts) {
  auto name = e.ReadString(ts, "nome del blocco");
  if (!name) return false;
  std::string n = *name;
  if (n.size() >= 2 && n.front() == '"' && n.back() == '"') n = n.substr(1, n.size() - 2);
  const ON_InstanceDefinition* idef = FindBlock(e, n);
  if (!idef) return e.Fail("blocco \"" + n + "\" non trovato");
  auto p = e.ReadPoint(ts, "punto di inserimento");
  if (!p) return false;
  double scale = 1, rot = 0;
  if (e.MoreInput(ts) && !ts.AtEnd()) { auto s = e.ReadNumber(ts, "scala"); if (!s) return false; scale = *s; }
  if (e.MoreInput(ts) && !ts.AtEnd()) { auto r = e.ReadNumber(ts, "rotazione"); if (!r) return false; rot = *r; }
  e.SkipEnter(ts);
  if (scale == 0) return e.Fail("scala nulla");
  ON_Xform xf = ON_Xform::TranslationTransformation(ON_3dVector(*p)) *
                [&] { ON_Xform r; r.Rotation(rot * ON_PI / 180.0, ON_3dVector::ZAxis, ON_3dPoint::Origin); return r; }() *
                ON_Xform::DiagonalTransformation(scale);
  ON_UUID id = InsertBlock(e, *idef, xf);
  if (ON_UuidIsNil(id)) return e.Fail("inserimento non riuscito");
  e.Out() << "  Inserito blocco \"" << n << "\"\n";
  return true;
}


std::string Unquote(std::string n) {
  if (n.size() >= 2 && n.front() == '"' && n.back() == '"') n = n.substr(1, n.size() - 2);
  return n;
}

// ReplaceBlock: le istanze selezionate usano la definizione del blocco indicato.
bool CmdReplaceBlock(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("ReplaceBlock", "seleziona le istanze da sostituire, poi Invio");
  if (sel.empty()) return false;
  auto name = e.ReadString(ts, "nome del blocco sostitutivo");
  if (!name) return false;
  const ON_InstanceDefinition* idef = FindBlock(e, Unquote(*name));
  if (!idef) return e.Fail("blocco \"" + Unquote(*name) + "\" non trovato");
  int n = 0;
  for (const auto& id : sel) {
    const auto* ir = ON_InstanceRef::Cast(Geo(e, id));
    if (!ir) continue;
    auto* nr = new ON_InstanceRef(*ir);
    nr->m_instance_definition_uuid = idef->Id();
    ON_BoundingBox bb = BlockBox(e, *idef);
    if (bb.IsValid()) { bb.Transform(nr->m_xform); nr->m_bbox = bb; }
    e.ReplaceObject(id, nr);
    n++;
  }
  if (!n) return e.Fail("nessuna istanza di blocco selezionata");
  e.Out() << "  Sostituite " << n << " istanze con \"" << Unquote(*name) << "\"\n";
  return true;
}

// RenameBlock: nome attuale, nuovo nome.
bool CmdRenameBlock(Engine& e, TokenStream& ts) {
  auto oldName = e.ReadString(ts, "nome del blocco");
  if (!oldName) return false;
  auto newName = e.ReadString(ts, "nuovo nome");
  if (!newName) return false;
  const std::string from = Unquote(*oldName), to = Unquote(*newName);
  const ON_InstanceDefinition* idef = FindBlock(e, from);
  if (!idef) return e.Fail("blocco \"" + from + "\" non trovato");
  if (to.empty()) return e.Fail("nome vuoto");
  if (FindBlock(e, to)) return e.Fail("esiste gia' un blocco \"" + to + "\"");
  // la definizione si sostituisce con una copia rinominata (stesso id: le istanze restano collegate)
  auto* copy = new ON_InstanceDefinition(*idef);
  const ON_UUID id = idef->Id();
  e.Model().RemoveModelComponent(ON_ModelComponent::Type::InstanceDefinition, id);
  copy->SetName(ON_wString(to.c_str()));
  copy->SetId(id);
  if (!e.Model().AddManagedModelComponent(copy, false).ModelComponent()) return e.Fail("rinomina non riuscita");
  e.Out() << "  Blocco \"" << from << "\" rinominato in \"" << to << "\"\n";
  return true;
}

// BlockEdit: modifica di un blocco. La prima volta si sceglie un'istanza: gli oggetti
// della definizione compaiono come copie modificabili (selezionate) al posto
// dell'istanza. Si modificano con i comandi normali, poi BlockEdit di nuovo
// conclude: la definizione prende le copie e tutte le istanze si aggiornano.
bool CmdBlockEdit(Engine& e, TokenStream& ts) {
  ONX_Model& m = e.Model();
  if (!ON_UuidIsNil(e.blockEditDef)) {
    const auto* idef = ON_InstanceDefinition::Cast(m.ComponentFromId(ON_ModelComponent::Type::InstanceDefinition, e.blockEditDef).ModelComponent());
    if (!idef) { e.blockEditDef = ON_nil_uuid; e.blockEditObjects.clear(); return e.Fail("blocco non piu' presente"); }
    ON_Xform inv = e.blockEditXform;
    if (!inv.Invert()) return e.Fail("trasformazione dell'istanza non invertibile");
    // vecchi oggetti della definizione via, nuovi dalle copie
    const ON_SimpleArray<ON_UUID> old = idef->InstanceGeometryIdList();
    ON_SimpleArray<ON_UUID> members;
    for (const auto& id : e.blockEditObjects) {
      const ON_Geometry* g = Geo(e, id);
      if (!g) continue;
      ON_Geometry* copy = static_cast<ON_Geometry*>(g->Duplicate());
      copy->Transform(inv);
      ON_3dmObjectAttributes a = AttrsOf(e, id);
      a.SetMode(ON::object_mode::idef_object);
      members.Append(e.AddObject(copy, &a));
      e.DeleteObject(id);
    }
    if (members.Count() == 0) return e.Fail("il blocco resterebbe vuoto: annulla o aggiungi oggetti");
    for (int i = 0; i < old.Count(); i++) e.DeleteObject(old[i]);
    auto* nd = new ON_InstanceDefinition(*idef);
    const ON_UUID did = idef->Id();
    m.RemoveModelComponent(ON_ModelComponent::Type::InstanceDefinition, did);
    nd->SetId(did);
    nd->SetInstanceGeometryIdList(members);
    nd->SetBoundingBox(BlockBox(e, *nd));
    const auto* def = ON_InstanceDefinition::Cast(m.AddManagedModelComponent(nd, false).ModelComponent());
    if (!def) return e.Fail("blocco non aggiornato");
    // riquadri delle istanze
    for (const auto& id : e.AllObjectIds(true)) {
      const auto* ir = ON_InstanceRef::Cast(Geo(e, id));
      if (!ir || ON_UuidCompare(ir->m_instance_definition_uuid, did) != 0) continue;
      auto* nr = new ON_InstanceRef(*ir);
      ON_BoundingBox bb = BlockBox(e, *def);
      if (bb.IsValid()) { bb.Transform(nr->m_xform); nr->m_bbox = bb; }
      e.ReplaceObject(id, nr);
    }
    e.blockEditDef = ON_nil_uuid;
    e.blockEditObjects.clear();
    e.ClearSelection();
    e.Out() << "  Blocco \"" << ToUtf8(def->Name()) << "\" aggiornato con " << members.Count() << " oggetti\n";
    return true;
  }
  auto sel = e.RequireSelection("BlockEdit", "seleziona l'istanza del blocco da modificare");
  if (sel.empty()) return false;
  (void)ts;
  const ON_InstanceRef* ir = nullptr;
  ON_UUID iid;
  for (const auto& id : sel) if ((ir = ON_InstanceRef::Cast(Geo(e, id)))) { iid = id; break; }
  if (!ir) return e.Fail("seleziona un'istanza di blocco");
  const auto* idef = ON_InstanceDefinition::Cast(m.ComponentFromId(ON_ModelComponent::Type::InstanceDefinition, ir->m_instance_definition_uuid).ModelComponent());
  if (!idef) return e.Fail("definizione del blocco mancante");
  e.blockEditDef = idef->Id();
  e.blockEditXform = ir->m_xform;
  e.blockEditObjects.clear();
  e.ClearSelection();
  const auto& ids = idef->InstanceGeometryIdList();
  for (int i = 0; i < ids.Count(); i++) {
    const auto* o = ON_ModelGeometryComponent::Cast(m.ComponentFromId(ON_ModelComponent::Type::ModelGeometry, ids[i]).ModelComponent());
    const ON_Geometry* g = o ? o->Geometry(nullptr) : nullptr;
    if (!g) continue;
    ON_Geometry* copy = static_cast<ON_Geometry*>(g->Duplicate());
    copy->Transform(ir->m_xform);
    ON_3dmObjectAttributes a = *o->Attributes(&ON_3dmObjectAttributes::DefaultAttributes);
    a.SetMode(ON::object_mode::normal_object);
    const ON_UUID nid = e.AddObject(copy, &a);
    e.blockEditObjects.push_back(nid);
    e.Select(nid);
  }
  e.Out() << "  Modifica del blocco \"" << ToUtf8(idef->Name()) << "\": " << e.blockEditObjects.size()
          << " oggetti. Al termine scrivi di nuovo BlockEdit\n";
  return true;
}

bool CmdExplodeBlock(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("ExplodeBlock", "seleziona le istanze di blocco, poi Invio");
  if (sel.empty()) return false;
  int k = 0;
  for (const auto& id : sel) {
    const auto* ir = ON_InstanceRef::Cast(Geo(e, id));
    if (!ir) continue;
    const auto* idef = ON_InstanceDefinition::Cast(
        e.Model().ComponentFromId(ON_ModelComponent::Type::InstanceDefinition, ir->m_instance_definition_uuid).ModelComponent());
    if (!idef) continue;
    const auto& ids = idef->InstanceGeometryIdList();
    ON_3dmObjectAttributes base = AttrsOf(e, id);
    for (int i = 0; i < ids.Count(); i++) {
      const auto* o = ON_ModelGeometryComponent::Cast(e.Model().ComponentFromId(ON_ModelComponent::Type::ModelGeometry, ids[i]).ModelComponent());
      const ON_Geometry* g = o ? o->Geometry(nullptr) : nullptr;
      if (!g) continue;
      ON_Geometry* copy = static_cast<ON_Geometry*>(g->Duplicate());
      copy->Transform(ir->m_xform);
      ON_3dmObjectAttributes a = *o->Attributes(&ON_3dmObjectAttributes::DefaultAttributes);
      a.SetMode(ON::object_mode::normal_object);
      a.m_layer_index = base.m_layer_index;
      e.AddObject(copy, &a);
    }
    e.DeleteObject(id);
    k++;
  }
  if (!k) return e.Fail("nessuna istanza di blocco selezionata");
  e.Out() << "  Esplose " << k << " istanze\n";
  return true;
}

}  // namespace

double CurveLengthOf(const ON_Curve& c) { return CurveLength(c); }
double ParamAtLengthOf(const ON_Curve& c, double s) { return ParamAtLength(c, s); }

void RegisterMoreCommands(Engine& e) {
  auto reg = [&](const char* n, const char* cat, const char* d, CommandFn f, bool mod = true) {
    CommandInfo i{n, cat, d, f};
    i.modifiesModel = mod;
    e.Register(i);
  };
  reg("Length", "Analisi", "Lunghezza delle curve selezionate", CmdLength, false);
  reg("Distance", "Analisi", "Distanza tra due punti", CmdDistance, false);
  reg("Angle", "Analisi", "Angolo tra due direzioni (vertice e due punti)", CmdAngle, false);
  reg("Radius", "Analisi", "Raggio di archi e cerchi", CmdRadius, false);
  reg("BoundingBox", "Analisi", "Scatola di ingombro degli oggetti", CmdBoundingBox);
  reg("Divide", "Punti", "Punti a distanze uguali lungo le curve (numero o Length=)", CmdDivide);
  reg("DivideCrv", "Punti", "Come Divide", CmdDivide);
  reg("Measure", "Punti", "Punti lungo le curve a una distanza data (lunghezza dei tratti)", CmdMeasure);
  reg("Rebuild", "Curve", "Ricostruisce le curve (PointCount=, Degree=)", CmdRebuild);
  reg("ChangeDegree", "Curve", "Cambia il grado delle curve", CmdChangeDegree);
  reg("ProjectToCPlane", "Trasforma", "Proietta gli oggetti sul piano di costruzione", CmdProjectToCPlane);
  reg("Helix", "Curve", "Elica (asse, raggio; Turns=)", CmdHelix);
  reg("Spiral", "Curve", "Spirale (asse o Flat, due raggi; Turns=)", CmdSpiral);
  reg("Group", "Modifica", "Raggruppa gli oggetti selezionati", CmdGroup);
  reg("Ungroup", "Modifica", "Separa gli oggetti dai gruppi", CmdUngroup);
  reg("Block", "Blocchi", "Crea una definizione di blocco dagli oggetti selezionati (Name=)", CmdBlock);
  reg("Insert", "Blocchi", "Inserisce un blocco (nome, punto, scala, rotazione)", CmdInsert);
  reg("BlockEdit", "Blocchi", "Modifica un blocco sul posto (di nuovo BlockEdit per concludere)", CmdBlockEdit);
  reg("ExplodeBlock", "Blocchi", "Trasforma le istanze di blocco in oggetti normali", CmdExplodeBlock);
  reg("ReplaceBlock", "Blocchi", "Le istanze selezionate usano un altro blocco", CmdReplaceBlock);
  reg("RenameBlock", "Blocchi", "Rinomina un blocco", CmdRenameBlock);
}

}  // namespace cad
