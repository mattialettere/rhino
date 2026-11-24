// Assistente AI: oggetti complessi costruiti per parti. Il modello scrive prima un piano
// (parti con il loro ingombro), poi i comandi di una parte alla volta; ogni parte viene
// eseguita e misurata subito, cosi' gli errori si correggono parte per parte.
#include "ai/Assistant.h"
#include "commands/Engine.h"
#include "core/Document3dm.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <sstream>

namespace cad::ai {

namespace {
std::string Trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace((unsigned char)s[a])) a++;
  while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
  return s.substr(a, b - a);
}
std::string Lower(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}
std::string F(double v) {
  if (std::fabs(v) < 5e-7) v = 0;
  char b[48];
  std::snprintf(b, sizeof b, "%.6g", v);
  return b;
}
std::string BoxText(const double* b) {
  return F(b[0]) + "," + F(b[1]) + "," + F(b[2]) + " " + F(b[3]) + "," + F(b[4]) + "," + F(b[5]);
}
bool ParsePoint3(const std::string& s, double* v) {
  int n = 0;
  std::istringstream in(s);
  for (std::string t; std::getline(in, t, ',');) {
    if (n >= 3) return false;
    char* e = nullptr;
    t = Trim(t);
    v[n] = std::strtod(t.c_str(), &e);
    if (t.empty() || *e) return false;
    n++;
  }
  if (n == 2) v[2] = 0;
  return n >= 2;
}
// Toglie gli accenti piu' comuni (UTF-8) e porta in minuscolo: "perché" -> "perche"
std::string Normalize(const std::string& s) {
  static const std::pair<const char*, const char*> acc[] = {
      {"\xc3\xa0", "a"}, {"\xc3\xa1", "a"}, {"\xc3\xa8", "e"}, {"\xc3\xa9", "e"}, {"\xc3\xac", "i"}, {"\xc3\xad", "i"},
      {"\xc3\xb2", "o"}, {"\xc3\xb3", "o"}, {"\xc3\xb9", "u"}, {"\xc3\xba", "u"}, {"\xc3\x80", "a"}, {"\xc3\x88", "e"},
      {"\xc3\x89", "e"}, {"\xc3\x8c", "i"}, {"\xc3\x92", "o"}, {"\xc3\x99", "u"}};
  std::string o = s;
  for (const auto& [from, to] : acc)
    for (size_t p; (p = o.find(from)) != std::string::npos;) o.replace(p, std::string(from).size(), to);
  o = Lower(o);
  for (auto& c : o)
    if (!std::isalnum((unsigned char)c)) c = ' ';
  return o;
}
// Ingombro stretto: punti campionati sulle superfici e sulle curve (il box di openNURBS
// usa i punti di controllo, molto piu' larghi per loft e curve interpolate).
ON_BoundingBox SampledBox(const ON_Geometry& g) {
  ON_BoundingBox box;
  auto sampleSurface = [&](const ON_Surface& srf) {
    const ON_Interval u = srf.Domain(0), v = srf.Domain(1);
    const int n = 24;
    for (int i = 0; i <= n; i++)
      for (int j = 0; j <= n; j++) box.Set(srf.PointAt(u.ParameterAt(double(i) / n), v.ParameterAt(double(j) / n)), true);
  };
  if (const auto* c = ON_Curve::Cast(&g)) {
    const ON_Interval d = c->Domain();
    for (int i = 0; i <= 128; i++) box.Set(c->PointAt(d.ParameterAt(i / 128.0)), true);
    return box;
  }
  std::unique_ptr<ON_Brep> owned;
  const ON_Brep* b = ON_Brep::Cast(&g);
  if (!b) {
    if (const auto* x = ON_Extrusion::Cast(&g)) owned.reset(x->BrepForm());
    else if (const auto* sf = ON_Surface::Cast(&g)) owned.reset(sf->BrepForm());
    b = owned.get();
  }
  if (b) {
    for (int i = 0; i < b->m_F.Count(); i++)
      if (const ON_Surface* sf = b->m_F[i].SurfaceOf()) sampleSurface(*sf);
    if (box.IsValid()) return box;
  }
  g.GetTightBoundingBox(box);
  return box;
}
}  // namespace

bool ParsePlan(const std::string& replyIn, Plan& plan, std::string* error) {
  plan = Plan{};
  std::string reply = replyIn;
  for (size_t a; (a = reply.find("<think>")) != std::string::npos;) {
    const size_t b = reply.find("</think>", a);
    reply.erase(a, b == std::string::npos ? std::string::npos : b + 8 - a);
  }
  auto fail = [&](const std::string& e) {
    if (error) *error = e;
    return false;
  };
  std::set<std::string> used;
  std::istringstream in(reply);
  for (std::string l; std::getline(in, l);) {
    l = Trim(l);
    if (l.rfind("#", 0) == 0 && plan.parts.empty()) {
      const std::string t = Trim(l.substr(1));
      if (Lower(t).rfind("distinctive", 0) == 0) {
        const size_t c = t.find(':');
        plan.features = Trim(c == std::string::npos ? t.substr(11) : t.substr(c + 1));
      } else if (plan.summary.empty()) {
        plan.summary = t;
      }
      continue;
    }
    if (Lower(l).rfind("part ", 0) != 0) continue;
    std::vector<std::string> f;
    std::istringstream fl(l.substr(5));
    for (std::string t; std::getline(fl, t, '|');) f.push_back(Trim(t));
    if (f.size() < 3) return fail("each PART line needs: PART Name | what and how | x0,y0,z0 x1,y1,z1 (" + l + ")");
    PlanPart p;
    for (char c : f[0])
      if (std::isalnum((unsigned char)c) || c == '_') p.name += c;
    if (p.name.empty() || !std::isalpha((unsigned char)p.name[0])) return fail("the part name must be one word: " + l);
    if (p.name.size() > 24) p.name.resize(24);
    for (int k = 2; used.count(Lower(p.name)); k++) p.name = p.name.substr(0, 20) + std::to_string(k);
    used.insert(Lower(p.name));
    p.what = f[1];
    std::istringstream bx(f[2]);
    std::string a, b;
    bx >> a >> b;
    double lo[3], hi[3];
    if (!ParsePoint3(a, lo) || !ParsePoint3(b, hi))
      return fail("the box of part " + p.name + " must be two points x0,y0,z0 x1,y1,z1 (" + f[2] + ")");
    for (int k = 0; k < 3; k++) {
      p.box[k] = std::min(lo[k], hi[k]);
      p.box[k + 3] = std::max(lo[k], hi[k]);
    }
    plan.parts.push_back(p);
  }
  if (plan.parts.empty()) return fail("the plan has no PART lines");
  if (plan.parts.size() > 24) return fail("too many parts: use at most 16");
  return true;
}

std::string PlanGrammar() {
  return "root ::= \"```plan\\n# \" [^\\n]{1,200} \"\\n# Distinctive: \" [^\\n]{1,200} \"\\n\" part{1,16} \"```\"\n"
         "part ::= \"PART \" name \" | \" what \" | \" p3 \" \" p3 \"\\n\"\n"
         "name ::= [A-Za-z] [A-Za-z0-9_]{0,23}\n"
         "what ::= [^|\\n]{2,140}\n"
         "num ::= \"-\"? [0-9]{1,6} (\".\" [0-9]{1,3})?\n"
         "p3 ::= num \",\" num \",\" num\n";
}

std::string FormatExample(const Example& ex) {
  std::ostringstream o;
  o << "Example request: \"" << ex.request << "\"\n```plan\n" << ex.plan;
  if (!std::string(ex.plan).empty() && std::string(ex.plan).back() != '\n') o << "\n";
  o << "```\n";
  for (const auto& p : ex.parts) {
    o << "Part " << p.name << ":\n```kurva\n" << p.script;
    if (!std::string(p.script).empty() && std::string(p.script).back() != '\n') o << "\n";
    o << "```\n";
  }
  return o.str();
}

std::vector<std::string> ExampleScript(const Example& ex) {
  std::vector<std::string> out;
  for (const auto& p : ex.parts) {
    const auto l = ExtractScript(std::string("```kurva\n") + p.script + "\n```");
    out.insert(out.end(), l.begin(), l.end());
  }
  return out;
}

std::vector<const Example*> SelectExamples(const std::string& request, int count, const std::string& exclude) {
  const std::string r = " " + Normalize(request) + " ";
  std::vector<std::pair<int, const Example*>> scored;
  for (const auto& ex : Examples()) {
    if (exclude == ex.id) continue;
    int score = 0;
    std::istringstream kw(ex.keywords);
    for (std::string k; kw >> k;) {
      // inizio di parola: "violin" trova "violino" e "violini"
      if (r.find(" " + Normalize(k)) != std::string::npos) score++;
    }
    scored.push_back({score, &ex});
  }
  std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  std::vector<const Example*> out;
  for (const auto& [sc, ex] : scored)
    if (sc > 0 && (int)out.size() < count) out.push_back(ex);
  // poi esempi che mostrano tecniche diverse: uno semplice (scatole) e uno organico
  for (const char* id : {"chitarra", "tavolo", "lampada", "bottiglia"}) {
    if ((int)out.size() >= count) break;
    for (const auto& [sc, ex] : scored)
      if (std::string(ex->id) == id && std::find(out.begin(), out.end(), ex) == out.end()) out.push_back(ex);
  }
  if ((int)out.size() > count) out.resize(count);
  return out;
}

namespace {
// Parole di una richiesta o di una descrizione (minuscole, senza accenti, almeno 3 lettere)
std::vector<std::string> Words(const std::string& s) {
  std::vector<std::string> w;
  std::istringstream in(Normalize(s));
  for (std::string t; in >> t;)
    if (t.size() >= 3) w.push_back(t);
  return w;
}
// Radice per confrontare le parole ("wheels" e "wheel", "corde" e "corda")
std::string Stem(const std::string& w) { return w.size() > 5 ? w.substr(0, 5) : w; }
// La parola w e' una forma della radice r? (r corta: solo uguale o plurale inglese)
bool WordIs(const std::string& w, const std::string& r) {
  if (w == r || w == r + "s") return true;
  return r.size() >= 5 && w.rfind(r, 0) == 0;
}
struct ObjectWords {
  const char* id;
  const char* same;    // parole che nominano proprio questo oggetto
  const char* differ;  // parole che ne fanno un oggetto diverso ("chitarra elettrica")
};
const ObjectWords kObjects[] = {
    {"tavolo", "tavolo tavoli table tables", "tavolino coffee"},
    {"sedia", "sedia sedie chair chairs", "rotelle girevole office sdraio"},
    {"lampada", "lampada lampade lamp lamps", "lampadario chandelier lampione"},
    {"tazza", "tazza tazze mug mugs", ""},
    {"bottiglia", "bottiglia bottiglie bottle bottles", ""},
    {"chitarra", "chitarra chitarre guitar guitars", "elettric electric basso bass"},
    {"violino", "violino violini violin violins", ""},
    {"aereo", "aereo aerei aeroplano airplane airliner", "caccia fighter elica propeller biplan idrovolant"},
    {"automobile", "auto automobile automobili macchina car cars", "sportiv sports corsa racing formula camion truck furgon"},
    {"teiera", "teiera teiere teapot teapots", ""},
    {"barca", "barca barche sailboat", "motore motor gommon canoa kayak nave ship"},
    {"carriola", "carriola carriole wheelbarrow", ""},
    {"ventilatore", "ventilatore ventilatori fan fans", "soffitto ceiling tavolo desk"},
};
// Righe che creano geometria, senza nomi (=> Nome, @Nome): per riconoscere le copie.
std::vector<std::string> CreatingLines(const std::vector<std::string>& lines) {
  std::vector<std::string> out;
  for (const auto& l0 : lines) {
    std::string c = Trim(l0);
    size_t i = 0;
    while (i < c.size() && (c[i] == '_' || c[i] == '-' || c[i] == '!')) i++;
    c = c.substr(i, c.find_first_of(" \t", i) == std::string::npos ? std::string::npos : c.find_first_of(" \t", i) - i);
    const CommandSpec* spec = FindSpec(c);
    if (!spec || !spec->creates) continue;
    std::string l = l0;
    if (const size_t a = l.find("=>"); a != std::string::npos) l.erase(a);
    std::istringstream in(l);
    std::string t, o;
    while (in >> t)
      if (t[0] != '@') o += (o.empty() ? "" : " ") + Lower(t);
    out.push_back(o);
  }
  return out;
}
bool BoxesTouch(const double* a, const double* b, double tol) {
  for (int k = 0; k < 3; k++)
    if (a[k] > b[k + 3] + tol || b[k] > a[k + 3] + tol) return false;
  return true;
}
double OverallSize(const Plan& plan) {
  double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
  for (const auto& q : plan.parts)
    for (int k = 0; k < 3; k++) { lo[k] = std::min(lo[k], q.box[k]); hi[k] = std::max(hi[k], q.box[k + 3]); }
  double overall = 0;
  for (int k = 0; k < 3; k++) overall = std::max(overall, hi[k] - lo[k]);
  return overall;
}
}  // namespace

bool SameObject(const std::string& request, const char* exampleId) {
  const ObjectWords* ow = nullptr;
  for (const auto& o : kObjects)
    if (std::string(o.id) == exampleId) ow = &o;
  if (!ow) return false;
  const auto words = Words(request);
  auto has = [&](const char* list) {
    std::istringstream in(list);
    for (std::string r; in >> r;)
      for (const auto& w : words)
        if (WordIs(w, r)) return true;
    return false;
  };
  return has(ow->same) && !has(ow->differ);
}

std::string CopiedExample(const std::vector<std::string>& lines, const std::string& request, const std::string& exclude,
                          int* copied) {
  const auto mine = CreatingLines(lines);
  if (copied) *copied = 0;
  if (mine.empty()) return {};
  for (const auto& ex : Examples()) {
    if (exclude == ex.id || SameObject(request, ex.id)) continue;
    const auto theirs = CreatingLines(ExampleScript(ex));
    const std::set<std::string> set(theirs.begin(), theirs.end());
    int n = 0;
    for (const auto& l : mine) n += (int)set.count(l);
    if (n >= 2 && 2 * n >= (int)mine.size()) {
      if (copied) *copied = n;
      return ex.id;
    }
  }
  return {};
}

std::string CheckPlan(const Plan& plan, const std::string& request, const std::string& exclude) {
  // copia del piano di un altro oggetto della libreria
  for (const auto& ex : Examples()) {
    if (exclude == ex.id || SameObject(request, ex.id)) continue;
    Plan other;
    if (!ParsePlan(std::string("```plan\n") + ex.plan + "```", other)) continue;
    std::vector<std::string> same;
    for (const auto& p : plan.parts)
      for (const auto& q : other.parts) {
        bool box = true;
        for (int k = 0; k < 6; k++) box = box && std::fabs(p.box[k] - q.box[k]) < 0.5;
        if (box || Normalize(p.what) == Normalize(q.what)) { same.push_back(p.name); break; }
      }
    if (same.size() >= 2 && same.size() * 10 >= plan.parts.size() * 4) {
      std::string names;
      for (const auto& n : same) names += (names.empty() ? "" : ", ") + n;
      return "this plan copies the example \"" + std::string(ex.request) + "\" (parts " + names + "), but \"" + request +
             "\" is a different object. Write its own plan: the parts that make it recognisable, with their real shapes "
             "and sizes";
    }
  }
  // ogni parte deve toccare (o quasi) almeno un'altra parte: niente pezzi sospesi nel vuoto
  if (plan.parts.size() >= 2) {
    const double tol = 0.03 * OverallSize(plan) + 5;
    for (const auto& p : plan.parts) {
      bool touch = false;
      for (const auto& q : plan.parts)
        if (&p != &q && BoxesTouch(p.box, q.box, tol)) touch = true;
      if (!touch)
        return "part " + p.name + " (" + BoxText(p.box) + ") does not touch any other part: every part must be attached "
               "to the others (add the part that holds it, or move its box)";
    }
  }
  return {};
}

std::string PlanSystemPrompt(const std::string& /*request*/, const std::string& /*exclude*/) {
  // uguale per tutte le richieste: llama-server la tiene in memoria (cache_prompt) tra una richiesta e l'altra
  std::ostringstream o;
  o << R"(You are the modelling assistant of Kurva3D, a NURBS CAD program similar to Rhino 8.
The user describes an object (often in Italian). You build it in two steps.
Step 1, the plan: answer with one ```plan block. First a line "# ..." with the overall size, the orientation
(length along Y, Z up, standing on z = 0 or lying on its back) and the main proportions. Then a line
"# Distinctive: ..." with what makes THIS object recognisable and different from similar objects (for a cello
and a guitar: shape of the body, sound holes, number of strings, head, size). Then one line per part:
PART Name | what it is and which commands build it | x0,y0,z0 x1,y1,z1
The two points are the minimum and maximum corners of the box that the part fills (mm). Use 2 to 14 parts,
from the main body to the details; every distinctive feature needs its part. Every part touches or overlaps
another part (nothing floats); real proportions.
Step 2, the parts: you are asked for one part at a time. Answer with one ```kurva block with the commands of
ONLY that part, every coordinate computed for its box, and give its final objects the part name with => Name.
The earlier parts already exist with their names: use them with @Name (to join, cut or colour them).
The examples you are shown belong to OTHER objects unless they say otherwise: learn the technique from them,
never copy their parts, coordinates or sizes.

)" << CommandReference(true);
  return o.str();
}

std::string PlanRequest(const std::string& request, const std::string& exclude) {
  std::ostringstream o;
  for (const Example* e : SelectExamples(request, 2, exclude)) {
    if (SameObject(request, e->id))
      o << "Plan of the same kind of object (follow it, with the sizes the user asks for), for \"" << e->request << "\":\n";
    else
      o << "Plan of a DIFFERENT object (\"" << e->request << "\"), only to show the format: do not copy its parts or sizes.\n";
    o << "```plan\n" << e->plan;
    if (!std::string(e->plan).empty() && std::string(e->plan).back() != '\n') o << "\n";
    o << "```\n\n";
  }
  o << "Request: " << request
    << "\n\nStep 1: think about what makes this object recognisable, then write only the plan in a ```plan block: the "
       "\"# ...\" line, the \"# Distinctive: ...\" line and one PART line per part with its box in mm.";
  return o.str();
}

std::string PartRequest(const Plan& plan, int i, const std::string& request, const std::string& exclude) {
  const PlanPart& p = plan.parts[i];
  std::ostringstream o;
  // tecniche: le parti degli esempi piu' simili a questa (per nome e descrizione)
  if (!request.empty()) {
    const auto mine = Words(p.name + " " + p.what);
    struct Cand { int score; const Example* ex; PlanPart part; const char* script; bool same; };
    std::vector<Cand> cands;
    for (const auto& ex : Examples()) {
      if (exclude == ex.id) continue;
      Plan ep;
      if (!ParsePlan(std::string("```plan\n") + ex.plan + "```", ep)) continue;
      const bool same = SameObject(request, ex.id);
      for (const auto& q : ep.parts) {
        const char* script = nullptr;
        for (const auto& sp : ex.parts)
          if (Lower(sp.name) == Lower(q.name)) script = sp.script;
        if (!script) continue;
        std::set<std::string> theirs;
        for (const auto& w : Words(q.name + " " + q.what)) theirs.insert(Stem(w));
        int score = 0;
        std::set<std::string> seen;
        for (const auto& w : mine)
          if (theirs.count(Stem(w)) && seen.insert(Stem(w)).second) score++;
        if (Lower(q.name) == Lower(p.name)) score += 3;
        if (same && Lower(q.name) == Lower(p.name)) score += 100;
        if (score >= 2) cands.push_back({score, &ex, q, script, same});
      }
    }
    std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.score > b.score; });
    std::set<const Example*> used;
    int shown = 0;
    for (const auto& c : cands) {
      if (shown >= 2 || used.count(c.ex)) continue;
      used.insert(c.ex);
      shown++;
      if (c.same)
        o << "The same part in the example of this object (\"" << c.ex->request << "\", box " << BoxText(c.part.box)
          << "): adapt it to your box.\n";
      else
        o << "Technique from part " << c.part.name << " of ANOTHER object (\"" << c.ex->request << "\", box "
          << BoxText(c.part.box) << "): its coordinates belong to that object, compute yours from your box.\n";
      o << "```kurva\n" << c.script;
      if (!std::string(c.script).empty() && std::string(c.script).back() != '\n') o << "\n";
      o << "```\n";
    }
    if (shown) o << "\n";
  }
  o << "Part " << (i + 1) << " of " << plan.parts.size() << ": " << p.name << " - " << p.what << ". Its box goes from "
    << BoxText(p.box) << ". Write ONLY the commands of this part in one ```kurva block, with every coordinate "
    << "inside that box, and name its final objects => " << p.name << ".";
  if (i > 0) {
    o << " Already built:";
    for (int k = 0; k < i; k++) o << " " << plan.parts[k].name;
    o << " (use @Name to work on them).";
  }
  return o.str();
}

Validation CheckPart(Engine& e, const std::vector<std::string>& before, const std::vector<std::string>& part,
                     const Plan& plan, int i, const std::string& request, const std::string& exclude) {
  std::vector<std::string> all = before;
  all.insert(all.end(), part.begin(), part.end());
  ON_BoundingBox got;
  int objects = 0;
  const std::string want = Lower(plan.parts[i].name);
  auto measure = [&](Engine& se) {
    objects = (int)se.AllObjectIds(true).size();
    for (const auto& id : se.AllObjectIds(true))
      if (const auto* o = se.Object(id)) {
        const ON_3dmObjectAttributes* a = o->Attributes(nullptr);
        if (!a || Lower(ToUtf8(a->m_name)) != want) continue;
        if (const ON_Geometry* g = o->Geometry(nullptr)) {
          const ON_BoundingBox b = SampledBox(*g);
          if (b.IsValid()) got.Union(b);
        }
      }
  };
  Validation v = part.empty() ? Validation{false, -1, "no commands for this part", 0} : Validate(e, all, measure);
  if (!v.ok) {
    v.line = v.line >= (int)before.size() ? v.line - (int)before.size() : -1;
    return v;
  }
  if (!request.empty()) {
    const std::string from = CopiedExample(part, request, exclude);
    if (!from.empty()) {
      std::string what = from;
      for (const auto& ex : Examples())
        if (from == ex.id) what = ex.request;
      v.ok = false;
      v.line = -1;
      v.error = "these commands are copied from the example \"" + what + "\", which is another object: compute the " +
                "coordinates and sizes of part " + plan.parts[i].name + " from its own box " + BoxText(plan.parts[i].box);
      return v;
    }
  }
  if (!got.IsValid()) return v;  // niente col nome della parte (es. un taglio in un'altra parte): nessuna misura
  // ingombro: il centro deve cadere nel box del piano (con un margine) e le misure non devono esagerare
  const PlanPart& p = plan.parts[i];
  const double overall = OverallSize(plan);
  const double maxPs = std::max({p.box[3] - p.box[0], p.box[4] - p.box[1], p.box[5] - p.box[2]});
  const char* axis = "XYZ";
  // prima la posizione e le misure eccessive (su tutti gli assi), poi le parti troppo piccole
  for (int pass = 0; pass < 2; pass++)
    for (int k = 0; k < 3; k++) {
      const double pc = 0.5 * (p.box[k] + p.box[k + 3]), ps = p.box[k + 3] - p.box[k];
      const double gc = 0.5 * (got.m_min[k] + got.m_max[k]), gs = got.m_max[k] - got.m_min[k];
      const double tol = 0.08 * overall + 2.0;
      std::string why;
      if (pass == 0 && std::fabs(gc - pc) > 0.5 * ps + tol)
        why = std::string("its centre along ") + axis[k] + " is " + F(std::round(gc - pc)) + " mm away from the box of the plan";
      else if (pass == 0 && gs > 1.6 * ps + 2 * tol)
        why = std::string("it is ") + F(std::round(gs)) + " mm long along " + axis[k] + " but the box of the plan is " +
              F(std::round(ps)) + " mm";
      // troppo piccola rispetto al suo box (solo lungo le misure principali della parte)
      else if (pass == 1 && ps >= 0.3 * maxPs && ps > 0.04 * overall + 8 && gs < 0.25 * ps)
        why = std::string("it is only ") + F(std::round(gs)) + " mm along " + axis[k] + " but its box in the plan is " +
              F(std::round(ps)) + " mm: make the part fill its box";
      if (!why.empty()) {
        const double gb[6] = {got.m_min.x, got.m_min.y, got.m_min.z, got.m_max.x, got.m_max.y, got.m_max.z};
        v.ok = false;
        v.line = -1;
        v.error = "part " + p.name + " ended up from " + BoxText(gb) + ", but the plan puts it from " + BoxText(p.box) +
                  ": " + why + ". Recompute the coordinates of this part";
        return v;
      }
    }
  return v;
}

std::string PartRepairRequest(const Plan& plan, int i, const std::vector<std::string>& part, const Validation& v) {
  std::ostringstream o;
  if (v.line >= 0 && v.line < (int)part.size())
    o << "Line " << (v.line + 1) << " of part " << plan.parts[i].name << " fails (" << part[v.line] << "): " << v.error;
  else
    o << v.error;
  o << ". Answer again with all the commands of part " << plan.parts[i].name << " in one ```kurva block.";
  return o.str();
}

}  // namespace cad::ai
