// Comandi di creazione di punti e curve (menu Curve di Rhino 8).
#include "commands/Engine.h"
#include <cmath>

namespace cad {
namespace {

bool LooksLikePoint(const std::string& t) {
  return t.find(',') != std::string::npos || t.find('<') != std::string::npos;
}

// Raggio da un numero o dalla distanza tra centro e un punto.
std::optional<double> ReadRadius(Engine& e, TokenStream& ts, const ON_3dPoint& center, const char* prompt) {
  if (!ts.AtEnd() && LooksLikePoint(ts.Peek())) {
    auto p = e.ReadPoint(ts, prompt);
    if (!p) return std::nullopt;
    return center.DistanceTo(*p);
  }
  return e.ReadNumber(ts, prompt);
}

bool Report(Engine& e, const ON_UUID& id, const char* what) {
  if (ON_UuidIsNil(id)) return e.Fail(std::string("impossibile creare ") + what);
  e.Out() << "  Creato: " << what << "\n";
  return true;
}

// Point: un punto.
bool CmdPoint(Engine& e, TokenStream& ts) {
  auto p = e.ReadPoint(ts, "posizione del punto");
  if (!p) return false;
  return Report(e, e.AddObject(new ON_Point(*p)), "punto");
}

// Points: piu' punti fino a Invio.
bool CmdPoints(Engine& e, TokenStream& ts) {
  int n = 0;
  while (e.MoreInput(ts)) {
    auto p = e.ReadPoint(ts, "posizione del punto");
    if (!p) return false;
    e.AddObject(new ON_Point(*p));
    n++;
  }
  e.SkipEnter(ts);
  if (!n) return e.Fail("nessun punto");
  e.Out() << "  Creati " << n << " punti\n";
  return true;
}

// Line: due punti -> ON_LineCurve.
bool CmdLine(Engine& e, TokenStream& ts) {
  auto a = e.ReadPoint(ts, "inizio della linea");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "fine della linea");
  if (!b) return false;
  if (a->DistanceTo(*b) <= e.Tolerance()) return e.Fail("linea di lunghezza nulla");
  return Report(e, e.AddObject(new ON_LineCurve(*a, *b)), "linea");
}

// Polyline: punti fino a Invio; opzione Close (o primo = ultimo punto).
bool CmdPolyline(Engine& e, TokenStream& ts) {
  ON_Polyline pl;
  bool close = false;
  while (e.MoreInput(ts)) {
    if (NormalizeName(ts.Peek()) == "close") { ts.Next(); close = true; break; }
    auto p = e.ReadPoint(ts, "punto della polilinea");
    if (!p) return false;
    pl.Append(*p);
  }
  e.SkipEnter(ts);
  if (pl.Count() < 2) return e.Fail("servono almeno 2 punti");
  if (close && pl.Count() >= 3) pl.Append(pl[0]);
  if (pl.Count() == 2) return Report(e, e.AddObject(new ON_LineCurve(pl[0], pl[1])), "linea");
  return Report(e, e.AddObject(new ON_PolylineCurve(pl)), close ? "polilinea chiusa" : "polilinea");
}

// Rectangle: due angoli opposti nel piano XY del primo angolo.
bool CmdRectangle(Engine& e, TokenStream& ts) {
  auto a = e.ReadPoint(ts, "primo angolo del rettangolo");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "altro angolo");
  if (!b) return false;
  if (std::fabs(a->x - b->x) <= e.Tolerance() || std::fabs(a->y - b->y) <= e.Tolerance())
    return e.Fail("rettangolo degenere");
  ON_Polyline pl;
  pl.Append(*a);
  pl.Append(ON_3dPoint(b->x, a->y, a->z));
  pl.Append(ON_3dPoint(b->x, b->y, a->z));
  pl.Append(ON_3dPoint(a->x, b->y, a->z));
  pl.Append(*a);
  return Report(e, e.AddObject(new ON_PolylineCurve(pl)), "rettangolo");
}

// Polygon: centro e vertice; opzione NumSides (predefinito 5).
bool CmdPolygon(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  int sides = opts.count("numsides") ? std::atoi(opts["numsides"].c_str()) : 5;
  if (sides < 3) return e.Fail("NumSides deve essere almeno 3");
  auto c = e.ReadPoint(ts, "centro del poligono");
  if (!c) return false;
  auto v = e.ReadPoint(ts, "vertice del poligono");
  if (!v) return false;
  ON_3dVector r = *v - *c;
  r.z = 0;
  const double radius = r.Length();
  if (radius <= e.Tolerance()) return e.Fail("raggio nullo");
  const double a0 = std::atan2(r.y, r.x);
  ON_Polyline pl;
  for (int i = 0; i <= sides; i++) {
    const double a = a0 + 2.0 * ON_PI * (i % sides) / sides;
    pl.Append(ON_3dPoint(c->x + radius * std::cos(a), c->y + radius * std::sin(a), c->z));
  }
  return Report(e, e.AddObject(new ON_PolylineCurve(pl)), "poligono");
}

// Circle: centro e raggio (numero o punto). Opzione Diameter.
bool CmdCircle(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts, {"diameter", "3point"});
  if (opts.count("3point")) {
    auto a = e.ReadPoint(ts, "primo punto"); if (!a) return false;
    auto b = e.ReadPoint(ts, "secondo punto"); if (!b) return false;
    auto c = e.ReadPoint(ts, "terzo punto"); if (!c) return false;
    ON_Circle circle(*a, *b, *c);
    if (!circle.IsValid()) return e.Fail("punti allineati");
    return Report(e, e.AddObject(new ON_ArcCurve(circle)), "cerchio");
  }
  auto c = e.ReadPoint(ts, "centro del cerchio");
  if (!c) return false;
  auto r = ReadRadius(e, ts, *c, opts.count("diameter") ? "diametro" : "raggio");
  if (!r) return false;
  double radius = opts.count("diameter") ? *r / 2.0 : *r;
  if (radius <= e.Tolerance()) return e.Fail("raggio nullo");
  ON_Plane plane(*c, ON_3dVector::ZAxis);
  return Report(e, e.AddObject(new ON_ArcCurve(ON_Circle(plane, radius))), "cerchio");
}

// Arc: centro, punto iniziale, punto finale (o angolo in gradi), antiorario.
bool CmdArc(Engine& e, TokenStream& ts) {
  auto c = e.ReadPoint(ts, "centro dell'arco");
  if (!c) return false;
  auto s = e.ReadPoint(ts, "inizio dell'arco");
  if (!s) return false;
  ON_3dVector x = *s - *c;
  x.z = 0;
  const double radius = x.Length();
  if (radius <= e.Tolerance()) return e.Fail("raggio nullo");
  double angle;
  if (!ts.AtEnd() && LooksLikePoint(ts.Peek())) {
    auto p = e.ReadPoint(ts, "fine dell'arco");
    if (!p) return false;
    ON_3dVector v = *p - *c;
    angle = std::atan2(v.y, v.x) - std::atan2(x.y, x.x);
    while (angle <= 0) angle += 2.0 * ON_PI;
  } else {
    auto deg = e.ReadNumber(ts, "angolo");
    if (!deg) return false;
    angle = *deg * ON_PI / 180.0;
  }
  x.Unitize();
  ON_Plane plane(*c, x, ON_CrossProduct(ON_3dVector::ZAxis, x));
  ON_Arc arc(plane, radius, angle);
  if (!arc.IsValid()) return e.Fail("arco non valido");
  return Report(e, e.AddObject(new ON_ArcCurve(arc)), "arco");
}

// Ellipse: centro, fine del primo asse, fine del secondo asse.
bool CmdEllipse(Engine& e, TokenStream& ts) {
  auto c = e.ReadPoint(ts, "centro dell'ellisse");
  if (!c) return false;
  auto a = e.ReadPoint(ts, "fine del primo asse");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "fine del secondo asse");
  if (!b) return false;
  ON_3dVector x = *a - *c;
  x.z = 0;
  const double r1 = x.Length();
  x.Unitize();
  ON_3dVector y = ON_CrossProduct(ON_3dVector::ZAxis, x);
  const double r2 = std::fabs((*b - *c) * y);
  if (r1 <= e.Tolerance() || r2 <= e.Tolerance()) return e.Fail("ellisse degenere");
  ON_Ellipse ellipse(ON_Plane(*c, x, y), r1, r2);
  auto* nc = new ON_NurbsCurve();
  if (!ellipse.GetNurbForm(*nc)) { delete nc; return e.Fail("ellisse non valida"); }
  return Report(e, e.AddObject(nc), "ellisse");
}

std::vector<ON_3dPoint> ReadPointList(Engine& e, TokenStream& ts, bool& close, bool& ok) {
  std::vector<ON_3dPoint> pts;
  ok = true;
  while (e.MoreInput(ts)) {
    if (NormalizeName(ts.Peek()) == "close") { ts.Next(); close = true; break; }
    auto p = e.ReadPoint(ts, "punto");
    if (!p) { ok = false; return pts; }
    pts.push_back(*p);
  }
  e.SkipEnter(ts);
  return pts;
}

// Curve: curva per punti di controllo, opzione Degree (predefinito 3).
bool CmdCurve(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  int degree = opts.count("degree") ? std::atoi(opts["degree"].c_str()) : 3;
  bool close = false, ok;
  auto pts = ReadPointList(e, ts, close, ok);
  if (!ok) return false;
  if (pts.size() < 2) return e.Fail("servono almeno 2 punti");
  if (close) pts.push_back(pts.front());
  ON_NurbsCurve* c = ControlPointCurve(pts, degree);
  if (!c) return e.Fail("curva non valida");
  return Report(e, e.AddObject(c), "curva");
}

// InterpCrv: curva cubica che passa per i punti (nodi a lunghezza di corda).
bool CmdInterpCrv(Engine& e, TokenStream& ts) {
  e.ReadOptions(ts);
  bool close = false, ok;
  auto pts = ReadPointList(e, ts, close, ok);
  if (!ok) return false;
  if (pts.size() < 2) return e.Fail("servono almeno 2 punti");
  ON_NurbsCurve* c = InterpolateCubic(pts);
  if (!c) return e.Fail("interpolazione non riuscita");
  return Report(e, e.AddObject(c), "curva interpolata");
}

}  // namespace

ON_NurbsCurve* ControlPointCurve(const std::vector<ON_3dPoint>& points, int degree) {
  const int n = static_cast<int>(points.size());
  if (n < 2 || degree < 1) return nullptr;
  degree = std::min(degree, n - 1);
  auto* c = new ON_NurbsCurve(3, false, degree + 1, n);
  for (int i = 0; i < n; i++) c->SetCV(i, points[i]);
  ON_MakeClampedUniformKnotVector(degree + 1, n, c->m_knot, 1.0);
  if (!c->IsValid()) { delete c; return nullptr; }
  return c;
}

// Spline cubica con estremi "naturali" (derivata seconda nulla): n punti,
// n+2 punti di controllo, nodi nei parametri a lunghezza di corda.
ON_NurbsCurve* InterpolateCubic(const std::vector<ON_3dPoint>& points) {
  std::vector<ON_3dPoint> pts;
  for (const auto& p : points)
    if (pts.empty() || pts.back().DistanceTo(p) > ON_ZERO_TOLERANCE) pts.push_back(p);
  const int n = static_cast<int>(pts.size());
  if (n < 2) return nullptr;
  if (n == 2) return ControlPointCurve(pts, 1);
  std::vector<double> t(n, 0.0);
  for (int i = 1; i < n; i++) t[i] = t[i - 1] + pts[i].DistanceTo(pts[i - 1]);
  const int m = n + 2;  // punti di controllo
  auto* c = new ON_NurbsCurve(3, false, 4, m);
  int k = 0;
  for (int i = 0; i < 3; i++) c->m_knot[k++] = t[0];
  for (int i = 1; i < n - 1; i++) c->m_knot[k++] = t[i];
  for (int i = 0; i < 3; i++) c->m_knot[k++] = t[n - 1];

  // Matrice delle funzioni base: righe = equazioni, colonne = punti di controllo.
  std::vector<std::vector<double>> A(m, std::vector<double>(m, 0.0));
  ON_NurbsCurve basis(1, false, 4, m);
  for (int i = 0; i < basis.KnotCount(); i++) basis.m_knot[i] = c->m_knot[i];
  for (int j = 0; j < m; j++) {
    for (int i = 0; i < m; i++) basis.m_cv[i] = (i == j) ? 1.0 : 0.0;
    double v[3];
    for (int i = 0; i < n; i++) {
      basis.Evaluate(t[i], 0, 1, v, (i == n - 1) ? -1 : 1);
      A[i][j] = v[0];
    }
    basis.Evaluate(t[0], 2, 1, v, 1);
    A[n][j] = v[2];
    basis.Evaluate(t[n - 1], 2, 1, v, -1);
    A[n + 1][j] = v[2];
  }
  std::vector<std::vector<double>> B(m, std::vector<double>(3, 0.0));
  for (int i = 0; i < n; i++) { B[i][0] = pts[i].x; B[i][1] = pts[i].y; B[i][2] = pts[i].z; }
  // Eliminazione di Gauss con pivot parziale.
  for (int col = 0; col < m; col++) {
    int piv = col;
    for (int r = col + 1; r < m; r++) if (std::fabs(A[r][col]) > std::fabs(A[piv][col])) piv = r;
    if (std::fabs(A[piv][col]) < 1e-14) { delete c; return nullptr; }
    std::swap(A[piv], A[col]);
    std::swap(B[piv], B[col]);
    for (int r = 0; r < m; r++) {
      if (r == col) continue;
      const double f = A[r][col] / A[col][col];
      if (f == 0.0) continue;
      for (int q = col; q < m; q++) A[r][q] -= f * A[col][q];
      for (int d = 0; d < 3; d++) B[r][d] -= f * B[col][d];
    }
  }
  for (int i = 0; i < m; i++)
    c->SetCV(i, ON_3dPoint(B[i][0] / A[i][i], B[i][1] / A[i][i], B[i][2] / A[i][i]));
  if (!c->IsValid()) { delete c; return nullptr; }
  return c;
}

void RegisterCreateCurveCommands(Engine& e) {
  e.Register({"Point", "Punti", "Crea un punto", CmdPoint});
  e.Register({"Points", "Punti", "Crea piu' punti", CmdPoints});
  e.Register({"Line", "Curve", "Linea tra due punti", CmdLine});
  e.Register({"Polyline", "Curve", "Polilinea per punti (opzione Close)", CmdPolyline});
  e.Register({"Rectangle", "Curve", "Rettangolo da due angoli", CmdRectangle});
  e.Register({"Polygon", "Curve", "Poligono regolare (centro, vertice; NumSides)", CmdPolygon});
  e.Register({"Circle", "Curve", "Cerchio (centro, raggio; Diameter, 3Point)", CmdCircle});
  e.Register({"Arc", "Curve", "Arco (centro, inizio, fine o angolo)", CmdArc});
  e.Register({"Ellipse", "Curve", "Ellisse (centro, due assi)", CmdEllipse});
  e.Register({"Curve", "Curve", "Curva per punti di controllo (Degree)", CmdCurve});
  e.Register({"InterpCrv", "Curve", "Curva interpolata per punti", CmdInterpCrv});
}

}  // namespace cad
