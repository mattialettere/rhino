// Componenti dell'editor a nodi: parametri, matematica, vettori, curve, superfici,
// trasformazioni e liste. Le operazioni complesse (loft, booleane, offset, tubi) usano i
// comandi di Kurva3D su un documento temporaneo: stesso risultato della riga di comando.
#include "commands/Engine.h"
#include "core/Document3dm.h"
#include "nodes/Graph.h"
#include "python/PythonHost.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace cad {
double ParamAtLengthOf(const ON_Curve& c, double s);
}

namespace cad::nodes {

namespace {

Port P(const char* name, const char* nick, Kind k) { Port p; p.name = name; p.nick = nick; p.kind = k; return p; }
Port PD(const char* name, const char* nick, Kind k, const Value& d) { Port p = P(name, nick, k); p.hasDefault = true; p.def = d; return p; }
Port PL(const char* name, const char* nick, Kind k) { Port p = P(name, nick, k); p.list = true; return p; }
Port PO(const char* name, const char* nick, Kind k) { Port p = P(name, nick, k); p.optional = true; return p; }
Port PLO(const char* name, const char* nick, Kind k) { Port p = PL(name, nick, k); p.optional = true; return p; }

const Value& A(const std::vector<List>& in, size_t i) { return in[i].front(); }
double N(const std::vector<List>& in, size_t i) { return in[i].front().num; }
ON_3dPoint Pt(const std::vector<List>& in, size_t i) { return in[i].front().pt; }
ON_3dVector Vc(const std::vector<List>& in, size_t i) { return ON_3dVector(in[i].front().pt); }
const ON_Plane& Pl(const std::vector<List>& in, size_t i) { return in[i].front().plane; }
const ON_Curve* Crv(const std::vector<List>& in, size_t i) { return ON_Curve::Cast(in[i].front().geo.get()); }

bool Fail(EvalContext& c, const std::string& msg) { c.error = msg; return false; }

std::string Param(EvalContext& c, const char* key, const char* def = "") {
  if (!c.params) return def;
  auto it = c.params->find(key);
  return it == c.params->end() ? def : it->second;
}

double ParamNum(EvalContext& c, const char* key, double def) {
  const std::string s = Param(c, key);
  char* end = nullptr;
  const double v = std::strtod(s.c_str(), &end);
  return s.empty() || end == s.c_str() ? def : v;
}

// Comando di Kurva3D su un documento temporaneo: gli oggetti "sel" sono selezionati
// nell'ordine dato; il risultato sono gli oggetti creati dal comando.
bool RunScratch(EvalContext& c, const std::string& cmd, const std::vector<const ON_Geometry*>& sel, List& out) {
  Document3dm doc;
  std::ostringstream log;
  Engine e(doc, log);
  e.interactive = false;
  for (const ON_Geometry* g : sel) {
    if (!g) continue;
    const ON_UUID id = e.AddObject(g->Duplicate());
    e.Select(id);
  }
  if (!e.Run(cmd)) return Fail(c, e.LastError());
  for (const auto& id : e.LastCreated())
    if (const auto* o = e.Object(id))
      if (const ON_Geometry* g = o->Geometry(nullptr)) out.push_back(Value::Geo(g->Duplicate()));
  if (out.empty()) return Fail(c, "nessun risultato");
  return true;
}

std::string P3(const ON_3dPoint& p) {
  char b[128];
  std::snprintf(b, sizeof b, "%.17g,%.17g,%.17g", p.x, p.y, p.z);
  return b;
}

Value TransformValue(const Value& v, const ON_Xform& x);

std::string G17(double v) {
  char b[64];
  std::snprintf(b, sizeof b, "%.17g", v);
  return b;
}

// Solido creato con un comando nell'origine (asse Z) e portato sul piano
bool OnPlane(EvalContext& c, const std::string& cmd, const ON_Plane& pl, List& out) {
  List tmp;
  if (!RunScratch(c, cmd, {}, tmp)) return false;
  ON_Xform x;
  x.Rotation(ON_Plane::World_xy, pl);
  for (auto& v : tmp) out.push_back(TransformValue(v, x));
  return true;
}

// Parametri a lunghezza d'arco uguale (n segmenti, n+1 punti; curve chiuse: n punti)
std::vector<double> DivideParams(const ON_Curve& c, int n) {
  std::vector<double> t;
  const double L = CurveLengthOf(c);
  const int count = c.IsClosed() ? n : n + 1;
  for (int i = 0; i < count; i++) t.push_back(i == 0 ? c.Domain()[0] : (i == n ? c.Domain()[1] : ParamAtLengthOf(c, L * i / n)));
  return t;
}

Value TransformValue(const Value& v, const ON_Xform& x) {
  Value r = v;
  if (v.kind == Kind::Point) r.pt = x * v.pt;
  else if (v.kind == Kind::Vector) { ON_3dVector d(v.pt); d.Transform(x); r.pt = ON_3dPoint(d); }
  else if (v.kind == Kind::Plane) { r.plane.Transform(x); }
  else if (v.geo) {
    ON_Geometry* g = ON_Geometry::Cast(v.geo->Duplicate());
    if (g && g->Transform(x)) r.geo.reset(g);
    else delete g;
  }
  return r;
}

ON_Brep* ExtrudeGeometry(const ON_Geometry* g, const ON_3dVector& d) {
  ON_LineCurve path(ON_3dPoint::Origin, ON_3dPoint(d));
  if (const auto* c = ON_Curve::Cast(g)) {
    ON_Plane pl;
    if (c->IsClosed() && c->IsPlanar(&pl) && std::fabs(pl.zaxis * d) > 1e-9) {
      // curva chiusa piana: solido chiuso (come Extrude di Grasshopper)
      ON_Brep* b = ON_BrepTrimmedPlane(pl, *c);
      if (b && ON_BrepExtrude(*b, path, true)) return b;
      delete b;
    }
    auto* s = new ON_SumSurface();
    if (!s->Create(*c, d)) { delete s; return nullptr; }
    auto* b = new ON_Brep();
    ON_Surface* srf = s;
    if (!b->Create(srf)) { delete b; delete srf; return nullptr; }
    return b;
  }
  ON_Brep* b = nullptr;
  if (const auto* br = ON_Brep::Cast(g)) b = new ON_Brep(*br);
  else if (const auto* x = ON_Extrusion::Cast(g)) b = x->BrepForm();
  else if (const auto* s = ON_Surface::Cast(g)) b = s->BrepForm();
  if (!b) return nullptr;
  if (!ON_BrepExtrude(*b, path, true)) { delete b; return nullptr; }
  return b;
}

std::vector<ComponentDef> Build() {
  std::vector<ComponentDef> L;
  auto add = [&](const char* type, const char* name, const char* cat, const char* desc, std::vector<Port> in,
                 std::vector<Port> out, ComponentFn fn) { L.push_back({type, name, cat, desc, std::move(in), std::move(out), std::move(fn)}); };
  const Value zero = Value::Num(0), one = Value::Num(1);
  const Value origin = Value::Pt(ON_3dPoint::Origin);
  const Value xy = Value::Pln(ON_Plane::World_xy);

  // ---------------------------------------------------------------- parametri
  add("Slider", "Cursore numerico", "Parametri", "Numero scelto con un cursore (minimo, massimo, decimali)", {},
      {P("Numero", "N", Kind::Number)}, [](EvalContext& c, const std::vector<List>&, std::vector<List>& out) {
        const double mn = ParamNum(c, "min", 0), mx = ParamNum(c, "max", 10);
        const int dec = (int)ParamNum(c, "decimals", 2);
        double v = std::clamp(ParamNum(c, "value", mn), std::min(mn, mx), std::max(mn, mx));
        const double f = std::pow(10.0, dec);
        v = std::round(v * f) / f;
        out[0].push_back(dec == 0 ? Value::Int((int)v) : Value::Num(v));
        return true;
      });
  add("Toggle", "Interruttore", "Parametri", "Valore vero/falso", {}, {P("Valore", "B", Kind::Bool)},
      [](EvalContext& c, const std::vector<List>&, std::vector<List>& out) {
        out[0].push_back(Value::Bool(Param(c, "value") == "true"));
        return true;
      });
  add("Panel", "Pannello", "Parametri", "Mostra i dati che riceve; senza ingresso, ogni riga del testo e' un valore",
      {PLO("Ingresso", "In", Kind::Any)}, {P("Uscita", "Out", Kind::Any)},
      [](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
        if (!in[0].empty()) { out[0] = in[0]; return true; }
        std::istringstream s(Param(c, "text"));
        for (std::string line; std::getline(s, line);) {
          if (!line.empty() && line.back() == '\r') line.pop_back();
          if (line.empty()) continue;
          Value v = Value::Text(line), n;
          if (Coerce(v, Kind::Number, n)) out[0].push_back(n);
          else if (Coerce(v, Kind::Point, n) && line.find(',') != std::string::npos) out[0].push_back(n);
          else out[0].push_back(v);
        }
        return true;
      });
  add("GeometryRef", "Geometria dal modello", "Parametri", "Oggetti del modello (scelti con \"Imposta dalla selezione\")",
      {}, {P("Geometria", "G", Kind::Geometry)}, [](EvalContext& c, const std::vector<List>&, std::vector<List>& out) {
        if (!c.doc) return true;
        std::istringstream s(Param(c, "ids"));
        for (std::string id; std::getline(s, id, ',');) {
          if (id.empty()) continue;
          const auto* o = c.doc->Object(ON_UuidFromString(id.c_str()));
          const ON_Geometry* g = o ? o->Geometry(nullptr) : nullptr;
          if (g) out[0].push_back(Value::Geo(g->Duplicate()));
          else c.error = "un oggetto di riferimento non esiste piu'";
        }
        return true;
      });

  // ---------------------------------------------------------------- matematica
  auto binary = [&](const char* type, const char* name, const char* desc, const Value& defB, double (*op)(double, double)) {
    add(type, name, "Matematica", desc, {PD("A", "A", Kind::Number, zero), PD("B", "B", Kind::Number, defB)}, {P("Risultato", "R", Kind::Number)},
        [op](EvalContext&, const std::vector<List>& in, std::vector<List>& out) {
          out[0].push_back(Value::Num(op(N(in, 0), N(in, 1))));
          return true;
        });
  };
  binary("Addition", "Somma", "A + B", zero, [](double a, double b) { return a + b; });
  binary("Subtraction", "Sottrazione", "A - B", zero, [](double a, double b) { return a - b; });
  binary("Multiplication", "Moltiplicazione", "A x B", one, [](double a, double b) { return a * b; });
  add("Division", "Divisione", "Matematica", "A / B", {PD("A", "A", Kind::Number, zero), PD("B", "B", Kind::Number, one)},
      {P("Risultato", "R", Kind::Number)}, [](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
        if (N(in, 1) == 0) return Fail(c, "divisione per zero");
        out[0].push_back(Value::Num(N(in, 0) / N(in, 1)));
        return true;
      });
  binary("Power", "Potenza", "A elevato B", one, [](double a, double b) { return std::pow(a, b); });
  binary("Modulus", "Resto", "Resto di A / B", one, [](double a, double b) { return b == 0 ? 0.0 : std::fmod(a, b); });
  add("Negative", "Opposto", "Matematica", "-x", {P("Valore", "x", Kind::Number)}, {P("Risultato", "y", Kind::Number)},
      [](EvalContext&, const std::vector<List>& in, std::vector<List>& out) { out[0].push_back(Value::Num(-N(in, 0))); return true; });
  add("Expression", "Espressione", "Matematica", "Formula con le variabili x, y, z (es. sin(x)*10)",
      {PD("x", "x", Kind::Number, zero), PD("y", "y", Kind::Number, zero), PD("z", "z", Kind::Number, zero)},
      {P("Risultato", "R", Kind::Number)}, [](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
        double r;
        std::string err;
        if (!EvalExpression(Param(c, "expr", "x"), {{"x", N(in, 0)}, {"y", N(in, 1)}, {"z", N(in, 2)}}, r, err)) return Fail(c, err);
        out[0].push_back(Value::Num(r));
        return true;
      });
  add("Pi", "Pi greco", "Matematica", "N x pi", {PD("Fattore", "N", Kind::Number, one)}, {P("Risultato", "y", Kind::Number)},
      [](EvalContext&, const std::vector<List>& in, std::vector<List>& out) { out[0].push_back(Value::Num(ON_PI * N(in, 0))); return true; });
  add("Series", "Serie", "Matematica", "Numeri da S con passo N, C valori",
      {PD("Inizio", "S", Kind::Number, zero), PD("Passo", "N", Kind::Number, one), PD("Quanti", "C", Kind::Integer, Value::Int(10))},
      {P("Serie", "S", Kind::Number)}, [](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
        const int n = (int)N(in, 2);
        if (n < 0 || n > 100000) return Fail(c, "numero di valori non valido");
        for (int i = 0; i < n; i++) out[0].push_back(Value::Num(N(in, 0) + i * N(in, 1)));
        return true;
      });
  add("Range", "Intervallo", "Matematica", "N+1 valori da D0 a D1",
      {PD("Da", "D0", Kind::Number, zero), PD("A", "D1", Kind::Number, one), PD("Passi", "N", Kind::Integer, Value::Int(10))},
      {P("Valori", "R", Kind::Number)}, [](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
        const int n = (int)N(in, 2);
        if (n < 1 || n > 100000) return Fail(c, "numero di passi non valido");
        for (int i = 0; i <= n; i++) out[0].push_back(Value::Num(N(in, 0) + (N(in, 1) - N(in, 0)) * i / n));
        return true;
      });
  add("Random", "Casuali", "Matematica", "N numeri casuali tra Min e Max (seme S)",
      {PD("Min", "R0", Kind::Number, zero), PD("Max", "R1", Kind::Number, one), PD("Quanti", "N", Kind::Integer, Value::Int(10)),
       PD("Seme", "S", Kind::Integer, Value::Int(1))},
      {P("Numeri", "R", Kind::Number)}, [](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
        const int n = (int)N(in, 2);
        if (n < 0 || n > 100000) return Fail(c, "numero di valori non valido");
        unsigned s = (unsigned)N(in, 3) * 2654435761u + 12345u;
        for (int i = 0; i < n; i++) {
          s = s * 1664525u + 1013904223u;
          out[0].push_back(Value::Num(N(in, 0) + (N(in, 1) - N(in, 0)) * ((s >> 8) / 16777216.0)));
        }
        return true;
      });
  add("MassAddition", "Somma della lista", "Matematica", "Somma di tutti i numeri", {PL("Numeri", "I", Kind::Number)},
      {P("Somma", "R", Kind::Number)}, [](EvalContext&, const std::vector<List>& in, std::vector<List>& out) {
        double s = 0;
        for (const auto& v : in[0]) s += v.num;
        out[0].push_back(Value::Num(s));
        return true;
      });
  add("Average", "Media", "Matematica", "Media dei numeri", {PL("Numeri", "I", Kind::Number)}, {P("Media", "AM", Kind::Number)},
      [](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
        if (in[0].empty()) return Fail(c, "lista vuota");
        double s = 0;
        for (const auto& v : in[0]) s += v.num;
        out[0].push_back(Value::Num(s / in[0].size()));
        return true;
      });

  // ---------------------------------------------------------------- vettori e piani
  add("ConstructPoint", "Punto XYZ", "Vettori", "Punto dalle coordinate",
      {PD("X", "X", Kind::Number, zero), PD("Y", "Y", Kind::Number, zero), PD("Z", "Z", Kind::Number, zero)}, {P("Punto", "Pt", Kind::Point)},
      [](EvalContext&, const std::vector<List>& in, std::vector<List>& out) {
        out[0].push_back(Value::Pt(ON_3dPoint(N(in, 0), N(in, 1), N(in, 2))));
        return true;
      });
  add("DeconstructPoint", "Scomponi punto", "Vettori", "Coordinate del punto", {P("Punto", "P", Kind::Point)},
      {P("X", "X", Kind::Number), P("Y", "Y", Kind::Number), P("Z", "Z", Kind::Number)},
      [](EvalContext&, const std::vector<List>& in, std::vector<List>& out) {
        const ON_3dPoint p = Pt(in, 0);
        out[0].push_back(Value::Num(p.x));
        out[1].push_back(Value::Num(p.y));
        out[2].push_back(Value::Num(p.z));
        return true;
      });
  add("VectorXYZ", "Vettore XYZ", "Vettori", "Vettore dalle componenti",
      {PD("X", "X", Kind::Number, zero), PD("Y", "Y", Kind::Number, zero), PD("Z", "Z", Kind::Number, zero)},
      {P("Vettore", "V", Kind::Vector), P("Lunghezza", "L", Kind::Number)}, [](EvalContext&, const std::vector<List>& in, std::vector<List>& out) {
        const ON_3dVector v(N(in, 0), N(in, 1), N(in, 2));
        out[0].push_back(Value::Vec(v));
        out[1].push_back(Value::Num(v.Length()));
        return true;
      });
  add("Vector2Pt", "Vettore da 2 punti", "Vettori", "Da A a B", {P("A", "A", Kind::Point), P("B", "B", Kind::Point)},
      {P("Vettore", "V", Kind::Vector), P("Lunghezza", "L", Kind::Number)}, [](EvalContext&, const std::vector<List>& in, std::vector<List>& out) {
        const ON_3dVector v = Pt(in, 1) - Pt(in, 0);
        out[0].push_back(Value::Vec(v));
        out[1].push_back(Value::Num(v.Length()));
        return true;
      });
  for (int axis = 0; axis < 3; axis++) {
    static const char* types[] = {"UnitX", "UnitY", "UnitZ"};
    static const char* names[] = {"Unitario X", "Unitario Y", "Unitario Z"};
    add(types[axis], names[axis], "Vettori", "Vettore lungo l'asse moltiplicato per F", {PD("Fattore", "F", Kind::Number, one)},
        {P("Vettore", "V", Kind::Vector)}, [axis](EvalContext&, const std::vector<List>& in, std::vector<List>& out) {
          ON_3dVector v(0, 0, 0);
          v[axis] = N(in, 0);
          out[0].push_back(Value::Vec(v));
          return true;
        });
  }
  add("Amplitude", "Lunghezza del vettore", "Vettori", "Vettore con la stessa direzione e lunghezza A",
      {P("Vettore", "V", Kind::Vector), PD("Lunghezza", "A", Kind::Number, one)}, {P("Vettore", "V", Kind::Vector)},
      [](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
        ON_3dVector v = Vc(in, 0);
        if (!v.Unitize()) return Fail(c, "vettore nullo");
        out[0].push_back(Value::Vec(v * N(in, 1)));
        return true;
      });
  add("Distance", "Distanza", "Vettori", "Distanza tra due punti", {P("A", "A", Kind::Point), P("B", "B", Kind::Point)},
      {P("Distanza", "D", Kind::Number)}, [](EvalContext&, const std::vector<List>& in, std::vector<List>& out) {
        out[0].push_back(Value::Num(Pt(in, 0).DistanceTo(Pt(in, 1))));
        return true;
      });
  auto worldPlane = [&](const char* type, const char* name, ON_Plane base) {
    add(type, name, "Vettori", "Piano del mondo con origine O", {PD("Origine", "O", Kind::Point, origin)}, {P("Piano", "P", Kind::Plane)},
        [base](EvalContext&, const std::vector<List>& in, std::vector<List>& out) {
          ON_Plane p = base;
          p.SetOrigin(Pt(in, 0));
          out[0].push_back(Value::Pln(p));
          return true;
        });
  };
  worldPlane("XYPlane", "Piano XY", ON_Plane::World_xy);
  worldPlane("XZPlane", "Piano XZ", ON_Plane(ON_3dPoint::Origin, ON_3dVector::XAxis, ON_3dVector::ZAxis));
  worldPlane("YZPlane", "Piano YZ", ON_Plane::World_yz);
  add("PlaneNormal", "Piano da normale", "Vettori", "Piano con origine O e normale Z",
      {PD("Origine", "O", Kind::Point, origin), PD("Normale", "Z", Kind::Vector, Value::Vec(ON_3dVector::ZAxis))}, {P("Piano", "P", Kind::Plane)},
      [](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
        ON_Plane p(Pt(in, 0), Vc(in, 1));
        if (!p.IsValid()) return Fail(c, "normale nulla");
        out[0].push_back(Value::Pln(p));
        return true;
      });
  add("DeconstructPlane", "Scomponi piano", "Vettori", "Origine e assi del piano", {P("Piano", "P", Kind::Plane)},
      {P("Origine", "O", Kind::Point), P("Asse X", "X", Kind::Vector), P("Asse Y", "Y", Kind::Vector), P("Asse Z", "Z", Kind::Vector)},
      [](EvalContext&, const std::vector<List>& in, std::vector<List>& out) {
        const ON_Plane& p = Pl(in, 0);
        out[0].push_back(Value::Pt(p.origin));
        out[1].push_back(Value::Vec(p.xaxis));
        out[2].push_back(Value::Vec(p.yaxis));
        out[3].push_back(Value::Vec(p.zaxis));
        return true;
      });

  // ---------------------------------------------------------------- curve
  add("Line", "Linea", "Curve", "Linea da A a B", {P("Inizio", "A", Kind::Point), P("Fine", "B", Kind::Point)}, {P("Linea", "L", Kind::Curve)},
      [](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
        if (Pt(in, 0).DistanceTo(Pt(in, 1)) <= ON_ZERO_TOLERANCE) return Fail(c, "punti coincidenti");
        out[0].push_back(Value::Geo(new ON_LineCurve(Pt(in, 0), Pt(in, 1))));
        return true;
      });
  add("LineSDL", "Linea punto-direzione", "Curve", "Linea da S lungo D, lunga L",
      {P("Inizio", "S", Kind::Point), PD("Direzione", "D", Kind::Vector, Value::Vec(ON_3dVector::ZAxis)), PD("Lunghezza", "L", Kind::Number, one)},
      {P("Linea", "L", Kind::Curve)}, [](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
        ON_3dVector d = Vc(in, 1);
        if (!d.Unitize() || N(in, 2) == 0) return Fail(c, "direzione o lunghezza nulla");
        out[0].push_back(Value::Geo(new ON_LineCurve(Pt(in, 0), Pt(in, 0) + d * N(in, 2))));
        return true;
      });
  add("Circle", "Cerchio", "Curve", "Cerchio sul piano P (centro nell'origine del piano)",
      {PD("Piano", "P", Kind::Plane, xy), PD("Raggio", "R", Kind::Number, one)}, {P("Cerchio", "C", Kind::Curve)},
      [](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
        if (N(in, 1) <= 0) return Fail(c, "raggio non positivo");
        out[0].push_back(Value::Geo(new ON_ArcCurve(ON_Circle(Pl(in, 0), N(in, 1)))));
        return true;
      });
  add("Arc3Pt", "Arco per 3 punti", "Curve", "Arco da A a C passando per B",
      {P("Inizio", "A", Kind::Point), P("Passaggio", "B", Kind::Point), P("Fine", "C", Kind::Point)}, {P("Arco", "A", Kind::Curve)},
      [](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
        ON_Arc a;
        if (!a.Create(Pt(in, 0), Pt(in, 1), Pt(in, 2))) return Fail(c, "punti allineati");
        out[0].push_back(Value::Geo(new ON_ArcCurve(a)));
        return true;
      });
  add("Rectangle", "Rettangolo", "Curve", "Rettangolo X per Y con un angolo nell'origine del piano",
      {PD("Piano", "P", Kind::Plane, xy), PD("Lato X", "X", Kind::Number, one), PD("Lato Y", "Y", Kind::Number, one)},
      {P("Rettangolo", "R", Kind::Curve)}, [](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
        const ON_Plane& p = Pl(in, 0);
        const double x = N(in, 1), y = N(in, 2);
        if (x == 0 || y == 0) return Fail(c, "lato nullo");
        ON_Polyline pl;
        pl.Append(p.PointAt(0, 0));
        pl.Append(p.PointAt(x, 0));
        pl.Append(p.PointAt(x, y));
        pl.Append(p.PointAt(0, y));
        pl.Append(p.PointAt(0, 0));
        out[0].push_back(Value::Geo(new ON_PolylineCurve(pl)));
        return true;
      });
  add("Polygon", "Poligono", "Curve", "Poligono regolare di S lati inscritto nel cerchio di raggio R",
      {PD("Piano", "P", Kind::Plane, xy), PD("Raggio", "R", Kind::Number, one), PD("Lati", "S", Kind::Integer, Value::Int(6))},
      {P("Poligono", "P", Kind::Curve)}, [](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
        const int s = (int)N(in, 2);
        if (s < 3 || s > 10000 || N(in, 1) <= 0) return Fail(c, "lati o raggio non validi");
        ON_Polyline pl;
        for (int i = 0; i <= s; i++) {
          const double a = 2 * ON_PI * (i % s) / s;
          pl.Append(Pl(in, 0).PointAt(N(in, 1) * std::cos(a), N(in, 1) * std::sin(a)));
        }
        out[0].push_back(Value::Geo(new ON_PolylineCurve(pl)));
        return true;
      });
  add("Polyline", "Polilinea", "Curve", "Polilinea dai vertici (chiusa se C e' vero)",
      {PL("Vertici", "V", Kind::Point), PD("Chiusa", "C", Kind::Bool, Value::Bool(false))}, {P("Polilinea", "Pl", Kind::Curve)},
      [](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
        ON_Polyline pl;
        for (const auto& v : in[0]) if (pl.Count() == 0 || pl[pl.Count() - 1].DistanceTo(v.pt) > ON_ZERO_TOLERANCE) pl.Append(v.pt);
        if (N(in, 1) != 0 && pl.Count() > 2 && pl[0].DistanceTo(pl[pl.Count() - 1]) > ON_ZERO_TOLERANCE) pl.Append(pl[0]);
        if (pl.Count() < 2) return Fail(c, "servono almeno 2 punti");
        out[0].push_back(Value::Geo(new ON_PolylineCurve(pl)));
        return true;
      });
  add("Interpolate", "Curva interpolata", "Curve", "Curva che passa per i punti (grado 3; 1 = polilinea)",
      {PL("Vertici", "V", Kind::Point), PD("Grado", "D", Kind::Integer, Value::Int(3))}, {P("Curva", "C", Kind::Curve)},
      [](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
        std::vector<ON_3dPoint> pts;
        for (const auto& v : in[0]) pts.push_back(v.pt);
        if (pts.size() < 2) return Fail(c, "servono almeno 2 punti");
        if (N(in, 1) <= 1) {
          ON_Polyline pl;
          for (const auto& p : pts) pl.Append(p);
          out[0].push_back(Value::Geo(new ON_PolylineCurve(pl)));
          return true;
        }
        ON_NurbsCurve* nc = InterpolateCubic(pts);
        if (!nc) return Fail(c, "interpolazione non riuscita");
        out[0].push_back(Value::Geo(nc));
        return true;
      });
  add("NurbsCurve", "Curva per punti di controllo", "Curve", "Curva NURBS con i punti di controllo dati",
      {PL("Vertici", "V", Kind::Point), PD("Grado", "D", Kind::Integer, Value::Int(3))}, {P("Curva", "C", Kind::Curve)},
      [](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
        std::vector<ON_3dPoint> pts;
        for (const auto& v : in[0]) pts.push_back(v.pt);
        ON_NurbsCurve* nc = ControlPointCurve(pts, (int)N(in, 1));
        if (!nc) return Fail(c, "servono almeno 2 punti");
        out[0].push_back(Value::Geo(nc));
        return true;
      });
  add("DivideCurve", "Dividi curva", "Curve", "N segmenti di uguale lunghezza: punti, tangenti e parametri",
      {P("Curva", "C", Kind::Curve), PD("Segmenti", "N", Kind::Integer, Value::Int(10))},
      {P("Punti", "P", Kind::Point), P("Tangenti", "T", Kind::Vector), P("Parametri", "t", Kind::Number)},
      [](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
        const int n = (int)N(in, 1);
        if (n < 1 || n > 100000) return Fail(c, "numero di segmenti non valido");
        const ON_Curve* cv = Crv(in, 0);
        for (double t : DivideParams(*cv, n)) {
          out[0].push_back(Value::Pt(cv->PointAt(t)));
          out[1].push_back(Value::Vec(cv->TangentAt(t)));
          out[2].push_back(Value::Num(t));
        }
        return true;
      });
  add("CurveLength", "Lunghezza", "Curve", "Lunghezza della curva", {P("Curva", "C", Kind::Curve)}, {P("Lunghezza", "L", Kind::Number)},
      [](EvalContext&, const std::vector<List>& in, std::vector<List>& out) {
        out[0].push_back(Value::Num(CurveLengthOf(*Crv(in, 0))));
        return true;
      });
  add("PointOnCurve", "Punto sulla curva", "Curve", "Punto a una frazione t (0-1) della lunghezza",
      {P("Curva", "C", Kind::Curve), PD("Frazione", "t", Kind::Number, Value::Num(0.5))}, {P("Punto", "P", Kind::Point), P("Tangente", "T", Kind::Vector)},
      [](EvalContext&, const std::vector<List>& in, std::vector<List>& out) {
        const ON_Curve* cv = Crv(in, 0);
        const double f = std::clamp(N(in, 1), 0.0, 1.0);
        const double t = f <= 0 ? cv->Domain()[0] : f >= 1 ? cv->Domain()[1] : ParamAtLengthOf(*cv, CurveLengthOf(*cv) * f);
        out[0].push_back(Value::Pt(cv->PointAt(t)));
        out[1].push_back(Value::Vec(cv->TangentAt(t)));
        return true;
      });
  add("EndPoints", "Estremi", "Curve", "Punto iniziale e finale", {P("Curva", "C", Kind::Curve)}, {P("Inizio", "S", Kind::Point), P("Fine", "E", Kind::Point)},
      [](EvalContext&, const std::vector<List>& in, std::vector<List>& out) {
        out[0].push_back(Value::Pt(Crv(in, 0)->PointAtStart()));
        out[1].push_back(Value::Pt(Crv(in, 0)->PointAtEnd()));
        return true;
      });
  add("OffsetCurve", "Offset curva", "Curve", "Curva parallela a distanza D (positiva a sinistra, sul piano P)",
      {P("Curva", "C", Kind::Curve), PD("Distanza", "D", Kind::Number, one), PD("Piano", "P", Kind::Plane, xy)}, {P("Curva", "C", Kind::Curve)},
      [](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
        const ON_Curve* cv = Crv(in, 0);
        const double d = N(in, 1);
        if (d == 0) { out[0].push_back(in[0].front()); return true; }
        const double t = cv->Domain().Mid();
        ON_3dVector side = ON_CrossProduct(Pl(in, 2).zaxis, cv->TangentAt(t));
        if (!side.Unitize()) return Fail(c, "curva non sul piano");
        const ON_3dPoint sidePt = cv->PointAt(t) + side * (d > 0 ? 1 : -1) * std::fabs(d) * 0.5;
        return RunScratch(c, "_Offset " + std::to_string(std::fabs(d)) + " " + P3(sidePt), {cv}, out[0]);
      });
  add("JoinCurves", "Unisci curve", "Curve", "Unisce le curve che si toccano", {PL("Curve", "C", Kind::Curve)}, {P("Curve", "C", Kind::Curve)},
      [](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
        if (in[0].size() < 2) { out[0] = in[0]; return true; }
        std::vector<const ON_Geometry*> g;
        for (const auto& v : in[0]) g.push_back(v.geo.get());
        return RunScratch(c, "_Join", g, out[0]);
      });

  // ---------------------------------------------------------------- superfici e solidi
  add("Extrude", "Estrudi", "Superfici", "Estrusione lungo un vettore (curve chiuse piane: solido chiuso)",
      {P("Base", "B", Kind::Geometry), PD("Direzione", "D", Kind::Vector, Value::Vec(ON_3dVector(0, 0, 10)))}, {P("Estrusione", "E", Kind::Brep)},
      [](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
        if (Vc(in, 1).Length() <= ON_ZERO_TOLERANCE) return Fail(c, "direzione nulla");
        ON_Brep* b = ExtrudeGeometry(A(in, 0).geo.get(), Vc(in, 1));
        if (!b) return Fail(c, "estrusione non riuscita");
        out[0].push_back(Value::Geo(b));
        return true;
      });
  add("Loft", "Loft", "Superfici", "Superficie attraverso le curve, nell'ordine", {PL("Curve", "C", Kind::Curve)}, {P("Loft", "L", Kind::Brep)},
      [](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
        if (in[0].size() < 2) return Fail(c, "servono almeno 2 curve");
        std::vector<const ON_Geometry*> g;
        for (const auto& v : in[0]) g.push_back(v.geo.get());
        return RunScratch(c, "_Loft", g, out[0]);
      });
  add("PlanarSrf", "Superficie piana", "Superfici", "Superfici piane dalle curve chiuse", {PL("Bordi", "E", Kind::Curve)}, {P("Superfici", "S", Kind::Brep)},
      [](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
        std::vector<const ON_Geometry*> g;
        for (const auto& v : in[0]) g.push_back(v.geo.get());
        return RunScratch(c, "_PlanarSrf", g, out[0]);
      });
  add("CenterBox", "Parallelepipedo", "Superfici", "Parallelepipedo centrato nell'origine del piano, lati X, Y, Z",
      {PD("Piano", "P", Kind::Plane, xy), PD("X", "X", Kind::Number, one), PD("Y", "Y", Kind::Number, one), PD("Z", "Z", Kind::Number, one)},
      {P("Box", "B", Kind::Brep)}, [](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
        const ON_Plane& p = Pl(in, 0);
        const double x = std::fabs(N(in, 1)) / 2, y = std::fabs(N(in, 2)) / 2, z = std::fabs(N(in, 3)) / 2;
        if (x == 0 || y == 0 || z == 0) return Fail(c, "lato nullo");
        ON_3dPoint pts[8] = {p.PointAt(-x, -y, -z), p.PointAt(x, -y, -z), p.PointAt(x, y, -z), p.PointAt(-x, y, -z),
                             p.PointAt(-x, -y, z),  p.PointAt(x, -y, z),  p.PointAt(x, y, z),  p.PointAt(-x, y, z)};
        ON_Brep* b = ON_BrepBox(pts);
        if (!b) return Fail(c, "box non valido");
        out[0].push_back(Value::Geo(b));
        return true;
      });
  add("Box2Pt", "Box da 2 punti", "Superfici", "Box allineato agli assi tra due angoli opposti", {P("A", "A", Kind::Point), P("B", "B", Kind::Point)},
      {P("Box", "B", Kind::Brep)}, [](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
        ON_BoundingBox bb;
        bb.Set(Pt(in, 0), false);
        bb.Set(Pt(in, 1), true);
        ON_3dPoint pts[8];
        if (!bb.GetCorners(pts)) return Fail(c, "box non valido");
        // GetCorners: ordine diverso da ON_BrepBox
        ON_3dPoint q[8] = {pts[0], pts[1], pts[3], pts[2], pts[4], pts[5], pts[7], pts[6]};
        ON_Brep* b = ON_BrepBox(q);
        if (!b || bb.Diagonal().x * bb.Diagonal().y * bb.Diagonal().z == 0) { delete b; return Fail(c, "box piatto"); }
        out[0].push_back(Value::Geo(b));
        return true;
      });
  add("Sphere", "Sfera", "Superfici", "Sfera di centro B e raggio R", {PD("Centro", "B", Kind::Point, origin), PD("Raggio", "R", Kind::Number, one)},
      {P("Sfera", "S", Kind::Brep)}, [](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
        if (N(in, 1) <= 0) return Fail(c, "raggio non positivo");
        return RunScratch(c, "_Sphere " + P3(Pt(in, 0)) + " " + G17(N(in, 1)), {}, out[0]);
      });
  add("Cylinder", "Cilindro", "Superfici", "Cilindro sul piano B, raggio R, altezza L",
      {PD("Base", "B", Kind::Plane, xy), PD("Raggio", "R", Kind::Number, one), PD("Altezza", "L", Kind::Number, Value::Num(10))},
      {P("Cilindro", "C", Kind::Brep)}, [](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
        if (N(in, 1) <= 0 || N(in, 2) == 0) return Fail(c, "raggio o altezza non validi");
        return OnPlane(c, "_Cylinder 0,0,0 " + G17(N(in, 1)) + " " + G17(N(in, 2)), Pl(in, 0), out[0]);
      });
  add("Cone", "Cono", "Superfici", "Cono con base sul piano B, raggio R, altezza L",
      {PD("Base", "B", Kind::Plane, xy), PD("Raggio", "R", Kind::Number, one), PD("Altezza", "L", Kind::Number, Value::Num(10))},
      {P("Cono", "C", Kind::Brep)}, [](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
        if (N(in, 1) <= 0 || N(in, 2) <= 0) return Fail(c, "raggio o altezza non validi");
        return OnPlane(c, "_Cone 0,0,0 " + G17(N(in, 1)) + " " + G17(N(in, 2)), Pl(in, 0), out[0]);
      });
  add("Pipe", "Tubo", "Superfici", "Tubo di raggio R lungo la curva", {P("Curva", "C", Kind::Curve), PD("Raggio", "R", Kind::Number, one)},
      {P("Tubo", "P", Kind::Brep)}, [](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
        if (N(in, 1) <= 0) return Fail(c, "raggio non positivo");
        return RunScratch(c, "_Pipe " + std::to_string(N(in, 1)), {Crv(in, 0)}, out[0]);
      });
  add("Revolve", "Rivoluzione", "Superfici", "Rivoluzione del profilo attorno all'asse (linea), angolo in gradi",
      {P("Profilo", "C", Kind::Curve), P("Asse", "A", Kind::Curve), PD("Angolo", "D", Kind::Number, Value::Num(360))}, {P("Superficie", "S", Kind::Brep)},
      [](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
        const ON_Curve* ax = Crv(in, 1);
        ON_Line axis(ax->PointAtStart(), ax->PointAtEnd());
        if (axis.Length() <= ON_ZERO_TOLERANCE) return Fail(c, "asse nullo");
        auto* rs = new ON_RevSurface();
        rs->m_curve = Crv(in, 0)->DuplicateCurve();
        rs->m_axis = axis;
        const double a = std::clamp(N(in, 2), -360.0, 360.0) * ON_PI / 180.0;
        if (a == 0) { delete rs; return Fail(c, "angolo nullo"); }
        rs->m_angle = a > 0 ? ON_Interval(0, a) : ON_Interval(0, -a);
        rs->m_t = rs->m_angle;
        if (a < 0) { rs->m_axis.Reverse(); }
        ON_Brep* b = ON_BrepRevSurface(rs, false, false);
        if (!b) { delete rs; return Fail(c, "rivoluzione non riuscita"); }
        out[0].push_back(Value::Geo(b));
        return true;
      });
  auto massComp = [&](const char* type, const char* name, const char* desc, bool volume) {
    add(type, name, "Superfici", desc, {P("Geometria", "G", Kind::Geometry)},
        {P(volume ? "Volume" : "Area", volume ? "V" : "A", Kind::Number), P("Baricentro", "C", Kind::Point)},
        [volume](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
          if (!MassPropertiesFn) return Fail(c, "serve il motore geometrico (OpenCASCADE)");
          const ON_Geometry* g = A(in, 0).geo.get();
          std::unique_ptr<ON_Geometry> filled;
          if (const auto* cv = ON_Curve::Cast(g); cv && !volume) {
            // area racchiusa da una curva chiusa piana
            ON_Plane pl;
            if (!cv->IsClosed() || !cv->IsPlanar(&pl)) return Fail(c, "la curva deve essere chiusa e piana");
            filled.reset(ON_BrepTrimmedPlane(pl, *cv));
            g = filled.get();
          }
          if (!g) return Fail(c, "geometria non valida");
          Document3dm doc;
          std::ostringstream log;
          Engine e(doc, log);
          const ON_UUID id = e.AddObject(g->Duplicate());
          double v = 0;
          ON_3dPoint centroid;
          if (!MassPropertiesFn(e, id, volume, v, centroid)) return Fail(c, "calcolo non riuscito");
          out[0].push_back(Value::Num(v));
          out[1].push_back(Value::Pt(centroid));
          return true;
        });
  };
  massComp("Area", "Area", "Area e baricentro (superfici o curve chiuse piane)", false);
  massComp("Volume", "Volume", "Volume e baricentro di un solido chiuso", true);
  auto boolComp = [&](const char* type, const char* name, const char* desc, const char* cmd) {
    add(type, name, "Superfici", desc, {P("A", "A", Kind::Brep), PL("B", "B", Kind::Brep)}, {P("Risultato", "R", Kind::Brep)},
        [cmd](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
          std::vector<const ON_Geometry*> g{A(in, 0).geo.get()};
          for (const auto& v : in[1]) g.push_back(v.geo.get());
          return RunScratch(c, cmd, g, out[0]);
        });
  };
  add("BooleanUnion", "Unione booleana", "Superfici", "Unione dei solidi", {PL("Solidi", "B", Kind::Brep)}, {P("Risultato", "R", Kind::Brep)},
      [](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
        if (in[0].size() < 2) { out[0] = in[0]; return true; }
        std::vector<const ON_Geometry*> g;
        for (const auto& v : in[0]) g.push_back(v.geo.get());
        return RunScratch(c, "_BooleanUnion", g, out[0]);
      });
  boolComp("BooleanDifference", "Differenza booleana", "A meno i solidi B", "_BooleanDifference");
  boolComp("BooleanIntersection", "Intersezione booleana", "Parte comune di A e dei solidi B", "_BooleanIntersection");
  add("BoundingBox", "Riquadro di ingombro", "Superfici", "Box allineato agli assi che contiene la geometria", {PL("Geometria", "G", Kind::Geometry)},
      {P("Box", "B", Kind::Brep)}, [](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
        ON_BoundingBox bb;
        for (const auto& v : in[0]) {
          if (v.kind == Kind::Point) bb.Set(v.pt, bb.IsValid());
          else if (v.geo) { ON_BoundingBox t; if (v.geo->GetTightBoundingBox(t)) bb.Union(t); }
        }
        ON_3dPoint pts[8];
        if (!bb.IsValid() || !bb.GetCorners(pts)) return Fail(c, "nessuna geometria");
        ON_3dPoint q[8] = {pts[0], pts[1], pts[3], pts[2], pts[4], pts[5], pts[7], pts[6]};
        ON_Brep* b = ON_BrepBox(q);
        if (!b) return Fail(c, "ingombro piatto");
        out[0].push_back(Value::Geo(b));
        return true;
      });

  // ---------------------------------------------------------------- trasformazioni
  add("Move", "Sposta", "Trasforma", "Sposta la geometria del vettore T",
      {P("Geometria", "G", Kind::Any), PD("Vettore", "T", Kind::Vector, Value::Vec(ON_3dVector(0, 0, 10)))}, {P("Geometria", "G", Kind::Any)},
      [](EvalContext&, const std::vector<List>& in, std::vector<List>& out) {
        out[0].push_back(TransformValue(A(in, 0), ON_Xform::TranslationTransformation(Vc(in, 1))));
        return true;
      });
  add("Rotate", "Ruota", "Trasforma", "Ruota di A gradi attorno alla normale del piano P",
      {P("Geometria", "G", Kind::Any), PD("Angolo", "A", Kind::Number, Value::Num(90)), PD("Piano", "P", Kind::Plane, xy)},
      {P("Geometria", "G", Kind::Any)}, [](EvalContext&, const std::vector<List>& in, std::vector<List>& out) {
        ON_Xform x;
        x.Rotation(N(in, 1) * ON_PI / 180.0, Pl(in, 2).zaxis, Pl(in, 2).origin);
        out[0].push_back(TransformValue(A(in, 0), x));
        return true;
      });
  add("Scale", "Scala", "Trasforma", "Scala uniforme dal centro C",
      {P("Geometria", "G", Kind::Any), PD("Centro", "C", Kind::Point, origin), PD("Fattore", "F", Kind::Number, Value::Num(0.5))},
      {P("Geometria", "G", Kind::Any)}, [](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
        if (N(in, 2) == 0) return Fail(c, "fattore nullo");
        out[0].push_back(TransformValue(A(in, 0), ON_Xform::ScaleTransformation(Pt(in, 1), N(in, 2))));
        return true;
      });
  add("Mirror", "Specchia", "Trasforma", "Simmetria rispetto al piano P",
      {P("Geometria", "G", Kind::Any), PD("Piano", "P", Kind::Plane, Value::Pln(ON_Plane::World_yz))}, {P("Geometria", "G", Kind::Any)},
      [](EvalContext&, const std::vector<List>& in, std::vector<List>& out) {
        ON_Xform x;
        x.Mirror(Pl(in, 1).origin, Pl(in, 1).zaxis);
        out[0].push_back(TransformValue(A(in, 0), x));
        return true;
      });
  add("LinearArray", "Serie lineare", "Trasforma", "N copie spostate ogni volta del vettore D",
      {P("Geometria", "G", Kind::Any), PD("Direzione", "D", Kind::Vector, Value::Vec(ON_3dVector(10, 0, 0))), PD("Copie", "N", Kind::Integer, Value::Int(5))},
      {P("Geometria", "G", Kind::Any)}, [](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
        const int n = (int)N(in, 2);
        if (n < 1 || n > 10000) return Fail(c, "numero di copie non valido");
        for (int i = 0; i < n; i++) out[0].push_back(TransformValue(A(in, 0), ON_Xform::TranslationTransformation(Vc(in, 1) * i)));
        return true;
      });
  add("PolarArray", "Serie polare", "Trasforma", "N copie ruotate attorno alla normale del piano, su un angolo totale",
      {P("Geometria", "G", Kind::Any), PD("Piano", "P", Kind::Plane, xy), PD("Copie", "N", Kind::Integer, Value::Int(6)),
       PD("Angolo", "A", Kind::Number, Value::Num(360))},
      {P("Geometria", "G", Kind::Any)}, [](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
        const int n = (int)N(in, 2);
        if (n < 1 || n > 10000) return Fail(c, "numero di copie non valido");
        const double total = N(in, 3);
        const double step = (std::fabs(std::fabs(total) - 360) < 1e-9 ? total / n : (n > 1 ? total / (n - 1) : 0)) * ON_PI / 180.0;
        for (int i = 0; i < n; i++) {
          ON_Xform x;
          x.Rotation(step * i, Pl(in, 1).zaxis, Pl(in, 1).origin);
          out[0].push_back(TransformValue(A(in, 0), x));
        }
        return true;
      });

  // ---------------------------------------------------------------- liste
  add("ListItem", "Elemento della lista", "Liste", "Elemento all'indice i (W: ricomincia dall'inizio oltre la fine)",
      {PL("Lista", "L", Kind::Any), PD("Indice", "i", Kind::Integer, Value::Int(0)), PD("Ripeti", "W", Kind::Bool, Value::Bool(true))},
      {P("Elemento", "E", Kind::Any)}, [](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
        const long n = (long)in[0].size();
        if (n == 0) return Fail(c, "lista vuota");
        long i = (long)N(in, 1);
        if (N(in, 2) != 0) i = ((i % n) + n) % n;
        else if (i < 0 || i >= n) return Fail(c, "indice fuori dalla lista");
        out[0].push_back(in[0][(size_t)i]);
        return true;
      });
  add("ListLength", "Lunghezza della lista", "Liste", "Numero di elementi", {PL("Lista", "L", Kind::Any)}, {P("Lunghezza", "L", Kind::Integer)},
      [](EvalContext&, const std::vector<List>& in, std::vector<List>& out) {
        out[0].push_back(Value::Int((int)in[0].size()));
        return true;
      });
  add("Reverse", "Inverti lista", "Liste", "Elementi in ordine inverso", {PL("Lista", "L", Kind::Any)}, {P("Lista", "L", Kind::Any)},
      [](EvalContext&, const std::vector<List>& in, std::vector<List>& out) {
        out[0].assign(in[0].rbegin(), in[0].rend());
        return true;
      });
  add("Merge", "Unisci liste", "Liste", "Elementi di D1 seguiti da quelli di D2", {PLO("Dati 1", "D1", Kind::Any), PLO("Dati 2", "D2", Kind::Any)},
      {P("Risultato", "R", Kind::Any)}, [](EvalContext&, const std::vector<List>& in, std::vector<List>& out) {
        out[0] = in[0];
        out[0].insert(out[0].end(), in[1].begin(), in[1].end());
        return true;
      });
  add("ShiftList", "Scorri lista", "Liste", "Sposta gli elementi di S posizioni",
      {PL("Lista", "L", Kind::Any), PD("Scorrimento", "S", Kind::Integer, Value::Int(1)), PD("Ripeti", "W", Kind::Bool, Value::Bool(true))},
      {P("Lista", "L", Kind::Any)}, [](EvalContext&, const std::vector<List>& in, std::vector<List>& out) {
        const long n = (long)in[0].size();
        const long s = (long)N(in, 1);
        for (long i = 0; i < n; i++) {
          long j = i + s;
          if (N(in, 2) != 0) j = ((j % n) + n) % n;
          else if (j < 0 || j >= n) continue;
          out[0].push_back(in[0][(size_t)j]);
        }
        return true;
      });
  add("CullIndex", "Togli per indice", "Liste", "Toglie gli elementi agli indici dati",
      {PL("Lista", "L", Kind::Any), PL("Indici", "I", Kind::Integer)}, {P("Lista", "L", Kind::Any)},
      [](EvalContext&, const std::vector<List>& in, std::vector<List>& out) {
        const long n = (long)in[0].size();
        std::vector<bool> drop((size_t)n, false);
        for (const auto& v : in[1]) {
          long i = (long)v.num;
          if (n && i < 0) i += n;
          if (i >= 0 && i < n) drop[(size_t)i] = true;
        }
        for (long i = 0; i < n; i++) if (!drop[(size_t)i]) out[0].push_back(in[0][(size_t)i]);
        return true;
      });
  add("Dispatch", "Smista", "Liste", "Elementi con schema vero in A, gli altri in B (lo schema si ripete)",
      {PL("Lista", "L", Kind::Any), PL("Schema", "P", Kind::Bool)}, {P("Veri", "A", Kind::Any), P("Falsi", "B", Kind::Any)},
      [](EvalContext& c, const std::vector<List>& in, std::vector<List>& out) {
        if (in[1].empty()) return Fail(c, "schema vuoto");
        for (size_t i = 0; i < in[0].size(); i++) out[in[1][i % in[1].size()].num != 0 ? 0 : 1].push_back(in[0][i]);
        return true;
      });
  add("Concatenate", "Concatena testo", "Liste", "Testo A seguito da B", {PD("A", "A", Kind::Text, Value::Text("")), PD("B", "B", Kind::Text, Value::Text(""))},
      {P("Testo", "R", Kind::Text)}, [](EvalContext&, const std::vector<List>& in, std::vector<List>& out) {
        out[0].push_back(Value::Text(A(in, 0).text + A(in, 1).text));
        return true;
      });
  return L;
}

}  // namespace

const std::vector<ComponentDef>& Library() {
  static const std::vector<ComponentDef> lib = Build();
  return lib;
}

const ComponentDef* FindDef(const std::string& type) {
  for (const auto& d : Library()) if (d.type == type) return &d;
  return nullptr;
}

std::vector<const ComponentDef*> SearchDefs(const std::string& text) {
  auto lower = [](std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; };
  const std::string q = lower(text);
  std::vector<const ComponentDef*> starts, contains;
  for (const auto& d : Library()) {
    const std::string n = lower(d.name), t = lower(d.type);
    if (n.rfind(q, 0) == 0 || t.rfind(q, 0) == 0) starts.push_back(&d);
    else if (n.find(q) != std::string::npos || t.find(q) != std::string::npos || lower(d.description).find(q) != std::string::npos) contains.push_back(&d);
  }
  starts.insert(starts.end(), contains.begin(), contains.end());
  return starts;
}

}  // namespace cad::nodes
