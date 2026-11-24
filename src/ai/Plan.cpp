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
    if (l.rfind("#", 0) == 0 && plan.summary.empty() && plan.parts.empty()) { plan.summary = Trim(l.substr(1)); continue; }
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
  return "root ::= \"```plan\\n# \" [^\\n]{1,200} \"\\n\" part{1,16} \"```\"\n"
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

std::string PlanSystemPrompt(const std::string& request, const std::string& exclude) {
  std::ostringstream o;
  o << R"(You are the modelling assistant of Kurva3D, a NURBS CAD program similar to Rhino 8.
The user describes an object (often in Italian). You build it in two steps.
Step 1, the plan: answer with one ```plan block. First a line "# ..." with the overall size, the orientation
(length along Y, Z up, standing on z = 0 or lying on its back) and the main proportions. Then one line per part:
PART Name | what it is and which commands build it | x0,y0,z0 x1,y1,z1
The two points are the minimum and maximum corners of the box that the part fills (mm). Use 2 to 14 parts,
from the main body to the details. Neighbouring parts touch or overlap a little; real proportions.
Step 2, the parts: you are asked for one part at a time. Answer with one ```kurva block with the commands of
ONLY that part, every coordinate inside its box, and give its final objects the part name with => Name.
The earlier parts already exist with their names: use them with @Name (to join, cut or colour them).

)" << CommandReference(true);
  const auto ex = SelectExamples(request, 2, exclude);
  for (const Example* e : ex) o << "\n" << FormatExample(*e);
  return o.str();
}

std::string PlanRequest(const std::string& request) {
  return request +
         "\n\nStep 1: write only the plan in a ```plan block (one PART line per part with its box in mm).";
}

std::string PartRequest(const Plan& plan, int i) {
  const PlanPart& p = plan.parts[i];
  std::ostringstream o;
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
                     const Plan& plan, int i) {
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
  if (!got.IsValid()) return v;  // niente col nome della parte (es. un taglio in un'altra parte): nessuna misura
  // ingombro: il centro deve cadere nel box del piano (con un margine) e le misure non devono esagerare
  const PlanPart& p = plan.parts[i];
  double overall = 0;
  {
    double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
    for (const auto& q : plan.parts)
      for (int k = 0; k < 3; k++) { lo[k] = std::min(lo[k], q.box[k]); hi[k] = std::max(hi[k], q.box[k + 3]); }
    for (int k = 0; k < 3; k++) overall = std::max(overall, hi[k] - lo[k]);
  }
  const char* axis = "XYZ";
  for (int k = 0; k < 3; k++) {
    const double pc = 0.5 * (p.box[k] + p.box[k + 3]), ps = p.box[k + 3] - p.box[k];
    const double gc = 0.5 * (got.m_min[k] + got.m_max[k]), gs = got.m_max[k] - got.m_min[k];
    const double tol = 0.08 * overall + 2.0;
    std::string why;
    if (std::fabs(gc - pc) > 0.5 * ps + tol)
      why = std::string("its centre along ") + axis[k] + " is " + F(std::round(gc - pc)) + " mm away from the box of the plan";
    else if (gs > 1.6 * ps + 2 * tol)
      why = std::string("it is ") + F(std::round(gs)) + " mm long along " + axis[k] + " but the box of the plan is " +
            F(std::round(ps)) + " mm";
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
