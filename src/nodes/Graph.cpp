#include "nodes/Graph.h"
#include "commands/Engine.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <sstream>

namespace cad::nodes {

const char* KindName(Kind k) {
  switch (k) {
    case Kind::Any: return "Dato";
    case Kind::Number: return "Numero";
    case Kind::Integer: return "Intero";
    case Kind::Bool: return "Booleano";
    case Kind::Text: return "Testo";
    case Kind::Point: return "Punto";
    case Kind::Vector: return "Vettore";
    case Kind::Plane: return "Piano";
    case Kind::Curve: return "Curva";
    case Kind::Surface: return "Superficie";
    case Kind::Brep: return "Brep";
    case Kind::Geometry: return "Geometria";
  }
  return "";
}

Value Value::Geo(ON_Geometry* g) {
  Value x;
  x.kind = Kind::Geometry;
  if (!g) return x;
  if (const auto* p = ON_Point::Cast(g)) {
    x.kind = Kind::Point;
    x.pt = p->point;
    delete g;
    return x;
  }
  if (ON_Curve::Cast(g)) x.kind = Kind::Curve;
  else if (ON_Surface::Cast(g) && !ON_Extrusion::Cast(g)) x.kind = Kind::Surface;
  else if (ON_Brep::Cast(g) || ON_Extrusion::Cast(g)) x.kind = Kind::Brep;
  x.geo.reset(g);
  return x;
}

namespace {
std::string Fmt(double v) {
  char b[64];
  std::snprintf(b, sizeof b, "%.10g", v);
  std::string s = b;
  return s == "-0" ? "0" : s;
}

std::string Describe(const ON_Geometry* g) {
  if (!g) return "<nulla>";
  if (const auto* c = ON_Curve::Cast(g)) {
    if (ON_LineCurve::Cast(c) || c->IsLinear()) return "Curva (linea)";
    ON_Arc a;
    if (c->IsArc(nullptr, &a)) return a.IsCircle() ? "Curva (cerchio)" : "Curva (arco)";
    if (ON_PolylineCurve::Cast(c) || c->IsPolyline()) return "Curva (polilinea)";
    return "Curva (NURBS)";
  }
  if (const auto* b = ON_Brep::Cast(g)) return b->m_F.Count() == 1 ? "Brep (superficie)" : (b->IsSolid() ? "Brep (solido chiuso)" : "Brep (polisuperficie)");
  if (ON_Extrusion::Cast(g)) return "Estrusione";
  if (ON_Surface::Cast(g)) return "Superficie";
  if (ON_Mesh::Cast(g)) return "Mesh";
  return g->ClassId()->ClassName();
}
}  // namespace

std::string Value::ToString() const {
  switch (kind) {
    case Kind::Number: return Fmt(num);
    case Kind::Integer: return Fmt(std::round(num));
    case Kind::Bool: return num != 0 ? "True" : "False";
    case Kind::Text: return text;
    case Kind::Point:
    case Kind::Vector: return "{" + Fmt(pt.x) + ", " + Fmt(pt.y) + ", " + Fmt(pt.z) + "}";
    case Kind::Plane:
      return "O(" + Fmt(plane.origin.x) + "," + Fmt(plane.origin.y) + "," + Fmt(plane.origin.z) + ") Z(" + Fmt(plane.zaxis.x) +
             "," + Fmt(plane.zaxis.y) + "," + Fmt(plane.zaxis.z) + ")";
    default: return Describe(geo.get());
  }
}

static bool ParseNumber(const std::string& s, double& v) {
  if (s.empty()) return false;
  char* end = nullptr;
  v = std::strtod(s.c_str(), &end);
  while (end && (*end == ' ' || *end == '\t')) end++;
  return end && *end == 0;
}

static ON_Brep* ToBrep(const ON_Geometry* g) {
  if (!g) return nullptr;
  if (const auto* b = ON_Brep::Cast(g)) return new ON_Brep(*b);
  if (const auto* x = ON_Extrusion::Cast(g)) return x->BrepForm();
  if (const auto* s = ON_Surface::Cast(g)) return s->BrepForm();
  return nullptr;
}

bool Coerce(const Value& v, Kind k, Value& out) {
  if (k == Kind::Any) { out = v; return true; }
  switch (k) {
    case Kind::Number:
    case Kind::Integer: {
      double d = 0;
      if (v.kind == Kind::Number || v.kind == Kind::Integer || v.kind == Kind::Bool) d = v.num;
      else if (v.kind == Kind::Text) { if (!ParseNumber(v.text, d)) return false; }
      else if (v.kind == Kind::Vector) d = ON_3dVector(v.pt).Length();
      else if (v.kind == Kind::Curve && v.geo) d = CurveLengthOf(*ON_Curve::Cast(v.geo.get()));
      else return false;
      out = k == Kind::Integer ? Value::Int((int)std::lround(d)) : Value::Num(d);
      return true;
    }
    case Kind::Bool:
      if (v.kind == Kind::Number || v.kind == Kind::Integer || v.kind == Kind::Bool) { out = Value::Bool(v.num != 0); return true; }
      if (v.kind == Kind::Text) {
        std::string t;
        for (char c : v.text) t += (char)std::tolower((unsigned char)c);
        if (t == "true" || t == "si" || t == "vero" || t == "1") { out = Value::Bool(true); return true; }
        if (t == "false" || t == "no" || t == "falso" || t == "0") { out = Value::Bool(false); return true; }
      }
      return false;
    case Kind::Text: out = Value::Text(v.ToString()); return true;
    case Kind::Point:
    case Kind::Vector: {
      ON_3dPoint p;
      if (v.kind == Kind::Point || v.kind == Kind::Vector) p = v.pt;
      else if (v.kind == Kind::Plane) p = k == Kind::Point ? v.plane.origin : ON_3dPoint(v.plane.zaxis);
      else if (v.kind == Kind::Text) {
        double c[3] = {0, 0, 0};
        std::string t = v.text;
        for (char& ch : t) if (ch == '{' || ch == '}' || ch == '(' || ch == ')') ch = ' ';
        std::istringstream in(t);
        std::string part;
        int n = 0;
        while (n < 3 && std::getline(in, part, ',')) if (!ParseNumber(part, c[n++])) return false;
        if (n < 2) return false;
        p = ON_3dPoint(c[0], c[1], c[2]);
      } else if (v.kind == Kind::Number && k == Kind::Vector) {
        p = ON_3dPoint(0, 0, v.num);  // un numero come vettore: lungo Z (come Grasshopper)
      } else return false;
      out = k == Kind::Point ? Value::Pt(p) : Value::Vec(ON_3dVector(p));
      return true;
    }
    case Kind::Plane:
      if (v.kind == Kind::Plane) { out = v; return true; }
      if (v.kind == Kind::Point) { ON_Plane pl = ON_Plane::World_xy; pl.SetOrigin(v.pt); out = Value::Pln(pl); return true; }
      if (v.kind == Kind::Curve && v.geo) {
        ON_Plane pl;
        if (ON_Curve::Cast(v.geo.get())->IsPlanar(&pl)) { out = Value::Pln(pl); return true; }
      }
      return false;
    case Kind::Curve:
      if (v.geo && ON_Curve::Cast(v.geo.get())) { out = v; out.kind = Kind::Curve; return true; }
      return false;
    case Kind::Surface:
      if (v.geo && ON_Surface::Cast(v.geo.get())) { out = v; out.kind = Kind::Surface; return true; }
      if (const auto* b = ON_Brep::Cast(v.geo.get()); b && b->m_F.Count() == 1) { out = v; out.kind = Kind::Surface; return true; }
      return false;
    case Kind::Brep: {
      if (v.geo && ON_Brep::Cast(v.geo.get())) { out = v; out.kind = Kind::Brep; return true; }
      ON_Brep* b = ToBrep(v.geo.get());
      if (!b) return false;
      out = Value();
      out.kind = Kind::Brep;
      out.geo.reset(b);
      return true;
    }
    case Kind::Geometry:
      if (v.geo) { out = v; return true; }
      if (v.kind == Kind::Point) { out = v; return true; }  // si converte in ON_Point dove serve
      return false;
    default: return false;
  }
}

// ---------------------------------------------------------------- grafo

int Graph::Add(const std::string& type, double x, double y) {
  Node n;
  n.id = nextId++;
  n.type = type;
  n.x = x;
  n.y = y;
  // valori iniziali dei nodi parametro
  if (type == "Slider") n.params = {{"min", "0"}, {"max", "10"}, {"value", "5"}, {"decimals", "2"}};
  else if (type == "Toggle") n.params = {{"value", "false"}};
  else if (type == "Panel") n.params = {{"text", ""}};
  else if (type == "Expression") n.params = {{"expr", "x"}};
  nodes.push_back(n);
  return n.id;
}

Node* Graph::Get(int id) {
  for (auto& n : nodes) if (n.id == id) return &n;
  return nullptr;
}
const Node* Graph::Get(int id) const {
  for (const auto& n : nodes) if (n.id == id) return &n;
  return nullptr;
}

bool Graph::Connect(int from, int fromPort, int to, int toPort) {
  const Node* a = Get(from);
  const Node* b = Get(to);
  if (!a || !b || from == to) return false;
  const auto* da = FindDef(a->type);
  const auto* db = FindDef(b->type);
  if (!da || !db || fromPort < 0 || fromPort >= (int)da->out.size() || toPort < 0 || toPort >= (int)db->in.size()) return false;
  for (const auto& w : wires)
    if (w.from == from && w.fromPort == fromPort && w.to == to && w.toPort == toPort) return true;
  // niente cicli: "to" non deve arrivare a "from"
  std::set<int> seen;
  std::vector<int> stack{to};
  while (!stack.empty()) {
    const int n = stack.back();
    stack.pop_back();
    if (n == from) return false;
    if (!seen.insert(n).second) continue;
    for (const auto& w : wires) if (w.from == n) stack.push_back(w.to);
  }
  wires.push_back({from, fromPort, to, toPort});
  return true;
}

void Graph::Disconnect(int to, int toPort) {
  wires.erase(std::remove_if(wires.begin(), wires.end(), [&](const Wire& w) { return w.to == to && w.toPort == toPort; }), wires.end());
}

void Graph::RemoveWire(size_t i) {
  if (i < wires.size()) wires.erase(wires.begin() + (long)i);
}

void Graph::Remove(int id) {
  wires.erase(std::remove_if(wires.begin(), wires.end(), [&](const Wire& w) { return w.from == id || w.to == id; }), wires.end());
  nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [&](const Node& n) { return n.id == id; }), nodes.end());
}

void Graph::Evaluate(Engine* doc) {
  // ordine topologico (Kahn); i nodi in un ciclo (non dovrebbero esserci) restano senza risultato
  std::map<int, int> indeg;
  for (const auto& n : nodes) indeg[n.id] = 0;
  for (const auto& w : wires) if (indeg.count(w.to) && indeg.count(w.from)) indeg[w.to]++;
  std::vector<int> order, ready;
  for (const auto& n : nodes) if (indeg[n.id] == 0) ready.push_back(n.id);
  while (!ready.empty()) {
    const int id = ready.front();
    ready.erase(ready.begin());
    order.push_back(id);
    for (const auto& w : wires)
      if (w.from == id && indeg.count(w.to) && --indeg[w.to] == 0) ready.push_back(w.to);
  }
  for (auto& n : nodes) { n.outputs.clear(); n.inputs.clear(); n.error.clear(); n.warning.clear(); }
  for (const int id : order) {
    Node* n = Get(id);
    const ComponentDef* def = FindDef(n->type);
    if (!def) { n->error = "componente sconosciuto: " + n->type; continue; }
    n->outputs.assign(def->out.size(), {});
    // dati agli ingressi (convertiti al tipo dell'ingresso)
    std::vector<List> ins(def->in.size());
    bool missing = false;
    for (size_t p = 0; p < def->in.size(); p++) {
      bool wired = false;
      for (const auto& w : wires) {
        if (w.to != id || w.toPort != (int)p) continue;
        wired = true;
        const Node* src = Get(w.from);
        if (!src || w.fromPort >= (int)src->outputs.size()) continue;
        for (const auto& v : src->outputs[w.fromPort]) {
          Value c;
          if (Coerce(v, def->in[p].kind, c)) ins[p].push_back(c);
          else n->warning = "dato non convertibile in " + std::string(KindName(def->in[p].kind)) + " (" + def->in[p].name + ")";
        }
      }
      if (!wired && def->in[p].hasDefault) ins[p].push_back(def->in[p].def);
      if (ins[p].empty() && !def->in[p].optional) {
        missing = true;
        if (n->warning.empty()) n->warning = "manca l'ingresso " + def->in[p].name;
      }
    }
    n->inputs = ins;
    if (missing) continue;
    // abbinamento "lista piu' lunga" sugli ingressi a elemento singolo
    size_t count = 1;
    bool anyItem = false;
    for (size_t p = 0; p < def->in.size(); p++)
      if (!def->in[p].list && !ins[p].empty()) { count = anyItem ? std::max(count, ins[p].size()) : ins[p].size(); anyItem = true; }
    EvalContext ctx;
    ctx.doc = doc;
    ctx.params = &n->params;
    for (size_t i = 0; i < count; i++) {
      std::vector<List> args(def->in.size());
      for (size_t p = 0; p < def->in.size(); p++) {
        if (def->in[p].list) args[p] = ins[p];
        else if (!ins[p].empty()) args[p] = {ins[p][std::min(i, ins[p].size() - 1)]};
      }
      std::vector<List> out(def->out.size());
      bool ok = false;
      try {
        ok = def->fn(ctx, args, out);
      } catch (const std::exception& e) {
        ctx.error = std::string("errore interno: ") + e.what();
      } catch (...) {
        ctx.error = "errore interno del motore geometrico";
      }
      if (!ok) {
        n->error = ctx.error.empty() ? "errore" : ctx.error;
        continue;
      }
      for (size_t o = 0; o < out.size() && o < n->outputs.size(); o++)
        n->outputs[o].insert(n->outputs[o].end(), out[o].begin(), out[o].end());
    }
  }
}

// ---------------------------------------------------------------- file

namespace {
std::string Esc(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '\\') o += "\\\\";
    else if (c == '\n') o += "\\n";
    else if (c == '\r') continue;
    else o += c;
  }
  return o;
}
std::string Unesc(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      o += s[i + 1] == 'n' ? '\n' : s[i + 1];
      i++;
    } else {
      o += s[i];
    }
  }
  return o;
}
}  // namespace

std::string Graph::Save() const {
  std::ostringstream o;
  o.precision(17);
  o << "KURVA3D-NODI 1\n";
  for (const auto& n : nodes) {
    o << "node " << n.id << " " << n.type << " " << n.x << " " << n.y << " " << (n.preview ? 1 : 0) << "\n";
    for (const auto& [k, v] : n.params) o << "param " << n.id << " " << k << " " << Esc(v) << "\n";
  }
  for (const auto& w : wires) o << "wire " << w.from << " " << w.fromPort << " " << w.to << " " << w.toPort << "\n";
  return o.str();
}

bool Graph::Load(const std::string& text, std::string* err) {
  std::istringstream in(text);
  std::string line;
  if (!std::getline(in, line) || line.rfind("KURVA3D-NODI", 0) != 0) {
    if (err) *err = "non e' un file di nodi di Kurva3D";
    return false;
  }
  Graph g;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream ls(line);
    std::string kw;
    ls >> kw;
    if (kw == "node") {
      Node n;
      int pv = 1;
      ls >> n.id >> n.type >> n.x >> n.y >> pv;
      n.preview = pv != 0;
      g.nodes.push_back(n);
      g.nextId = std::max(g.nextId, n.id + 1);
    } else if (kw == "param") {
      int id;
      std::string key;
      ls >> id >> key;
      std::string rest;
      std::getline(ls, rest);
      if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
      if (Node* n = g.Get(id)) n->params[key] = Unesc(rest);
    } else if (kw == "wire") {
      Wire w;
      ls >> w.from >> w.fromPort >> w.to >> w.toPort;
      if (g.Get(w.from) && g.Get(w.to)) g.wires.push_back(w);
    }
  }
  *this = std::move(g);
  return true;
}

// ---------------------------------------------------------------- anteprima e bake

static ON_Geometry* ToGeometry(const Value& v) {
  if (v.kind == Kind::Point) return new ON_Point(v.pt);
  if (v.geo) return v.geo->Duplicate();
  return nullptr;
}

void Graph::Preview(std::vector<std::shared_ptr<const ON_Geometry>>& out) const {
  for (const auto& n : nodes) {
    if (!n.preview) continue;
    const auto* def = FindDef(n.type);
    if (!def) continue;
    for (size_t o = 0; o < n.outputs.size(); o++)
      for (const auto& v : n.outputs[o]) {
        if (v.geo) out.push_back(v.geo);
        else if (v.kind == Kind::Point && def->out[o].kind != Kind::Vector) out.push_back(std::make_shared<ON_Point>(v.pt));
      }
  }
}

int Graph::Bake(int id, Engine& e) const {
  const Node* n = Get(id);
  if (!n) return 0;
  int count = 0;
  for (const auto& list : n->outputs)
    for (const auto& v : list)
      if (v.kind != Kind::Vector)
        if (ON_Geometry* g = ToGeometry(v)) {
          e.AddObject(g);
          count++;
        }
  return count;
}

// ---------------------------------------------------------------- espressioni

namespace {
struct ExprParser {
  const std::string& s;
  size_t i = 0;
  const std::map<std::string, double>& vars;
  std::string err;
  void Skip() { while (i < s.size() && std::isspace((unsigned char)s[i])) i++; }
  bool Eat(char c) { Skip(); if (i < s.size() && s[i] == c) { i++; return true; } return false; }
  double Expr() {
    double v = Term();
    for (;;) {
      if (Eat('+')) v += Term();
      else if (Eat('-')) v -= Term();
      else return v;
    }
  }
  double Term() {
    double v = Unary();
    for (;;) {
      if (Eat('*')) v *= Unary();
      else if (Eat('/')) v /= Unary();
      else if (Eat('%')) v = std::fmod(v, Unary());
      else return v;
    }
  }
  double Unary() {
    if (Eat('-')) return -Unary();
    if (Eat('+')) return Unary();
    return Power();
  }
  double Power() {
    double b = Primary();
    if (Eat('^')) return std::pow(b, Unary());
    return b;
  }
  double Primary() {
    Skip();
    if (Eat('(')) {
      double v = Expr();
      if (!Eat(')')) err = "manca )";
      return v;
    }
    if (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == '.')) {
      char* end = nullptr;
      double v = std::strtod(s.c_str() + i, &end);
      i = end - s.c_str();
      return v;
    }
    if (i < s.size() && (std::isalpha((unsigned char)s[i]) || s[i] == '_')) {
      std::string name;
      while (i < s.size() && (std::isalnum((unsigned char)s[i]) || s[i] == '_')) name += s[i++];
      std::string lower;
      for (char c : name) lower += (char)std::tolower((unsigned char)c);
      if (Eat('(')) {
        std::vector<double> a;
        if (!Eat(')')) {
          do a.push_back(Expr()); while (Eat(','));
          if (!Eat(')')) err = "manca ) dopo " + name;
        }
        auto need = [&](size_t n) { if (a.size() != n) { err = name + ": numero di argomenti errato"; return false; } return true; };
        const double kDeg = 1.0;
        (void)kDeg;
        if (lower == "sin" && need(1)) return std::sin(a[0]);
        if (lower == "cos" && need(1)) return std::cos(a[0]);
        if (lower == "tan" && need(1)) return std::tan(a[0]);
        if (lower == "asin" && need(1)) return std::asin(a[0]);
        if (lower == "acos" && need(1)) return std::acos(a[0]);
        if (lower == "atan" && need(1)) return std::atan(a[0]);
        if (lower == "atan2" && need(2)) return std::atan2(a[0], a[1]);
        if (lower == "sqrt" && need(1)) return std::sqrt(a[0]);
        if (lower == "abs" && need(1)) return std::fabs(a[0]);
        if (lower == "exp" && need(1)) return std::exp(a[0]);
        if ((lower == "ln" || lower == "log") && need(1)) return std::log(a[0]);
        if (lower == "log10" && need(1)) return std::log10(a[0]);
        if (lower == "floor" && need(1)) return std::floor(a[0]);
        if (lower == "ceil" && need(1)) return std::ceil(a[0]);
        if (lower == "round" && need(1)) return std::round(a[0]);
        if (lower == "pow" && need(2)) return std::pow(a[0], a[1]);
        if (lower == "min" && !a.empty()) return *std::min_element(a.begin(), a.end());
        if (lower == "max" && !a.empty()) return *std::max_element(a.begin(), a.end());
        if (lower == "rad" && need(1)) return a[0] * ON_PI / 180.0;
        if (lower == "deg" && need(1)) return a[0] * 180.0 / ON_PI;
        if (err.empty()) err = "funzione sconosciuta: " + name;
        return 0;
      }
      if (lower == "pi") return ON_PI;
      if (lower == "e") return std::exp(1.0);
      auto it = vars.find(name);
      if (it == vars.end()) it = vars.find(lower);
      if (it != vars.end()) return it->second;
      err = "variabile sconosciuta: " + name;
      return 0;
    }
    err = "espressione non valida";
    return 0;
  }
};
}  // namespace

bool EvalExpression(const std::string& expr, const std::map<std::string, double>& vars, double& out, std::string& err) {
  ExprParser p{expr, 0, vars, {}};
  out = p.Expr();
  p.Skip();
  if (p.err.empty() && p.i != expr.size()) p.err = "testo in piu' dopo l'espressione";
  err = p.err;
  return err.empty();
}

}  // namespace cad::nodes

namespace cad {

// NodeEditor: apre l'editor visuale a nodi (solo nella finestra)
void RegisterNodeCommands(Engine& e) {
  e.Register({"NodeEditor", "Script", "Editor visuale a nodi (algoritmi con componenti collegati)",
              [](Engine& en, TokenStream&) {
                if (!en.showNodeEditor) return en.Fail("l'editor a nodi e' disponibile solo nella finestra di Kurva3D");
                return en.showNodeEditor();
              },
              false});
}

}  // namespace cad
