// Editor visuale a nodi (l'equivalente di Grasshopper in Rhino 8, D-066): grafo di
// componenti con ingressi e uscite collegati da fili. Ogni uscita e' una lista di valori;
// i componenti elaborano elemento per elemento con l'abbinamento "lista piu' lunga".
// Questo file non dipende da Qt: valutazione, salvataggio e anteprima si provano nei test.
#pragma once
#include "opennurbs.h"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cad {
class Engine;
}

namespace cad::nodes {

enum class Kind { Any, Number, Integer, Bool, Text, Point, Vector, Plane, Curve, Surface, Brep, Geometry };
const char* KindName(Kind k);

struct Value {
  Kind kind = Kind::Number;
  double num = 0;
  std::string text;
  ON_3dPoint pt = ON_3dPoint::Origin;  // punti e vettori
  ON_Plane plane = ON_Plane::World_xy;
  std::shared_ptr<const ON_Geometry> geo;

  static Value Num(double v) { Value x; x.kind = Kind::Number; x.num = v; return x; }
  static Value Int(int v) { Value x; x.kind = Kind::Integer; x.num = v; return x; }
  static Value Bool(bool v) { Value x; x.kind = Kind::Bool; x.num = v ? 1 : 0; return x; }
  static Value Text(const std::string& s) { Value x; x.kind = Kind::Text; x.text = s; return x; }
  static Value Pt(const ON_3dPoint& p) { Value x; x.kind = Kind::Point; x.pt = p; return x; }
  static Value Vec(const ON_3dVector& v) { Value x; x.kind = Kind::Vector; x.pt = ON_3dPoint(v); return x; }
  static Value Pln(const ON_Plane& p) { Value x; x.kind = Kind::Plane; x.plane = p; return x; }
  static Value Geo(ON_Geometry* g);  // ne prende possesso; il tipo segue la geometria
  std::string ToString() const;
};
using List = std::vector<Value>;

// Conversione di un valore al tipo di un ingresso (numero da testo, punto da ON_Point, ...)
bool Coerce(const Value& v, Kind k, Value& out);

struct Port {
  std::string name, nick;
  Kind kind = Kind::Any;
  bool list = false;      // riceve tutta la lista (altrimenti un elemento alla volta)
  bool optional = false;
  bool hasDefault = false;
  Value def;
};

struct EvalContext {
  Engine* doc = nullptr;                                   // documento (geometria di riferimento)
  const std::map<std::string, std::string>* params = nullptr;  // parametri del nodo (cursore, pannello, ...)
  std::string error;
};

// in: un elemento (o la lista intera per gli ingressi "list") per ogni ingresso;
// out: si aggiungono i valori di ogni uscita.
using ComponentFn = std::function<bool(EvalContext&, const std::vector<List>& in, std::vector<List>& out)>;

struct ComponentDef {
  std::string type;         // identificatore nel file (inglese)
  std::string name;         // nome mostrato
  std::string category;     // scheda della tavolozza
  std::string description;
  std::vector<Port> in, out;
  ComponentFn fn;
};

const std::vector<ComponentDef>& Library();
const ComponentDef* FindDef(const std::string& type);
// Ricerca per nome o tipo (finestra "cerca componente")
std::vector<const ComponentDef*> SearchDefs(const std::string& text);

struct Node {
  int id = 0;
  std::string type;
  double x = 0, y = 0;
  bool preview = true;
  std::map<std::string, std::string> params;  // valore del cursore, testo del pannello, ...
  std::vector<List> outputs;                   // risultato dell'ultima valutazione
  std::vector<List> inputs;                    // dati arrivati agli ingressi (per il pannello)
  std::string error;                           // errore dell'ultima valutazione
  std::string warning;                         // ingresso mancante o vuoto
};

struct Wire {
  int from = 0, fromPort = 0, to = 0, toPort = 0;
};

class Graph {
public:
  int Add(const std::string& type, double x = 0, double y = 0);
  // Collega un'uscita a un ingresso (piu' fili sullo stesso ingresso si sommano). No cicli.
  bool Connect(int from, int fromPort, int to, int toPort);
  void Disconnect(int to, int toPort);  // tutti i fili dell'ingresso
  void RemoveWire(size_t index);
  void Remove(int id);
  Node* Get(int id);
  const Node* Get(int id) const;
  void Clear() { nodes.clear(); wires.clear(); nextId = 1; }
  // Valuta tutto il grafo in ordine topologico.
  void Evaluate(Engine* doc);
  // Testo del file (.k3nodi) e lettura
  std::string Save() const;
  bool Load(const std::string& text, std::string* err = nullptr);
  // Geometrie da mostrare in anteprima (nodi con anteprima attiva)
  void Preview(std::vector<std::shared_ptr<const ON_Geometry>>& out) const;
  // Bake: aggiunge al documento la geometria delle uscite del nodo; restituisce il numero di oggetti
  int Bake(int id, Engine& e) const;

  std::vector<Node> nodes;
  std::vector<Wire> wires;
  int nextId = 1;
};

// Espressioni del componente Expression: + - * / ^ ( ), funzioni sin cos tan asin acos atan
// sqrt abs exp log ln floor ceil round min max pow, costanti pi e, variabili x y z (o altre).
bool EvalExpression(const std::string& expr, const std::map<std::string, double>& vars, double& out, std::string& err);

}  // namespace cad::nodes
