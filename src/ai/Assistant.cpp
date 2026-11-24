#include "ai/Assistant.h"
#include "commands/Engine.h"
#include "core/Document3dm.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>

namespace cad::ai {

// Qwen3 (Alibaba): tutte le taglie sono rilasciate con licenza Apache 2.0, anche i
// file GGUF ufficiali del repository Qwen. Non usiamo Llama, Gemma o Qwen2.5-3B/72B
// (licenze con restrizioni). La licenza viene ricontrollata su Hugging Face prima del download.
const std::vector<ModelInfo>& Catalog() {
  static const std::vector<ModelInfo> c = {
      {"qwen3-0.6b", "Qwen3 0.6B (solo prove)", "Qwen/Qwen3-0.6B-GGUF", {"Q8_0"}, 0.64, "apache-2.0", false, 28},
      {"qwen3-1.7b", "Qwen3 1.7B (computer con meno di 8 GB)", "Qwen/Qwen3-1.7B-GGUF", {"Q8_0"}, 1.83, "apache-2.0", true, 28},
      {"qwen3-4b", "Qwen3 4B (8 GB di memoria)", "Qwen/Qwen3-4B-GGUF", {"Q4_K_M", "Q5_K_M", "Q8_0"}, 2.5, "apache-2.0", true, 36},
      {"qwen3-8b", "Qwen3 8B (16 GB di memoria)", "Qwen/Qwen3-8B-GGUF", {"Q4_K_M", "Q5_K_M", "Q8_0"}, 5.0, "apache-2.0", true, 36},
      {"qwen3-14b", "Qwen3 14B (32 GB di memoria)", "Qwen/Qwen3-14B-GGUF", {"Q4_K_M", "Q5_K_M", "Q8_0"}, 9.0, "apache-2.0", true, 40},
      {"qwen3-32b", "Qwen3 32B (workstation, 64 GB o GPU da 24 GB)", "Qwen/Qwen3-32B-GGUF", {"Q4_K_M", "Q5_K_M", "Q8_0"}, 19.8, "apache-2.0", true, 64},
      // Qwen2.5-Coder, addestrati sul codice: 0.5B, 1.5B, 7B, 14B e 32B sono Apache 2.0
      // (il 3B no: licenza Qwen Research, quindi escluso). Alcuni file GGUF sono divisi in parti.
      {"qwen2.5-coder-1.5b", "Qwen2.5-Coder 1.5B (codice, meno di 8 GB)", "Qwen/Qwen2.5-Coder-1.5B-Instruct-GGUF", {"Q8_0"}, 1.9, "apache-2.0", true, 28, false},
      {"qwen2.5-coder-7b", "Qwen2.5-Coder 7B (codice, 16 GB di memoria)", "Qwen/Qwen2.5-Coder-7B-Instruct-GGUF", {"Q4_K_M", "Q5_K_M"}, 4.7, "apache-2.0", true, 28, false},
      {"qwen2.5-coder-14b", "Qwen2.5-Coder 14B (codice, 32 GB di memoria)", "Qwen/Qwen2.5-Coder-14B-Instruct-GGUF", {"Q4_K_M", "Q5_K_M"}, 9.0, "apache-2.0", true, 48, false},
      {"qwen2.5-coder-32b", "Qwen2.5-Coder 32B (codice, workstation)", "Qwen/Qwen2.5-Coder-32B-Instruct-GGUF", {"Q4_K_M", "Q5_K_M"}, 19.9, "apache-2.0", true, 64, false},
      // Qwen3 4B addestrato sui comandi di Kurva3D (tools/ai-train, 19/11/2026): per ora uso
      // personale, file su Google Drive (si puo' anche scegliere il .gguf dal disco); istruzioni brevi
      {"kurva3d-qwen3-4b", "Kurva3D 4B addestrato (sperimentale)",
       "https://drive.usercontent.google.com/download?id=12LYyog5LdxLz5KY0feduORTc92vPr6hZ&export=download&confirm=t",
       {"Q4_K_M"}, 2.5, "apache-2.0", true, 36, false, true, 2497281056LL, ""},
  };
  return c;
}

const ModelInfo* FindModel(const std::string& id) {
  for (const auto& m : Catalog()) if (m.id == id) return &m;
  return nullptr;
}

bool LicenseAllowed(const std::string& license) {
  std::string l = license;
  for (auto& ch : l) ch = (char)std::tolower((unsigned char)ch);
  return l == "apache-2.0" || l == "mit";
}

std::string DescribeHardware(const HardwareInfo& hw) {
  std::ostringstream o;
  o.precision(3);
  o << "Memoria: " << hw.ramGB << " GB, " << hw.threads << " thread";
  if (hw.unifiedMemory) o << " (memoria unificata con la GPU)";
  for (const auto& g : hw.gpus) {
    o << "\nGPU: " << g.name;
    if (g.vramGB > 0) o << ", " << g.vramGB << " GB dedicati";
    if (!g.discrete) o << " (integrata)";
  }
  if (hw.gpus.empty()) o << "\nNessuna GPU dedicata trovata: il modello usera' il processore";
  return o.str();
}

double ModelBudgetGB(const HardwareInfo& hw, bool* useGpu) {
  double vram = 0;
  for (const auto& g : hw.gpus) if (g.discrete) vram = std::max(vram, g.vramGB);
  const double cpu = hw.unifiedMemory ? hw.ramGB * 0.65 : hw.ramGB * 0.6;
  const double gpu = vram > 2 ? vram - 1.0 : 0;
  if (useGpu) *useGpu = gpu > 0 || hw.unifiedMemory;
  return std::max(cpu, gpu);
}

bool ModelFits(const ModelInfo& m, const HardwareInfo& hw) { return m.sizeGB * 1.2 + 1.0 <= ModelBudgetGB(hw); }

int RecommendModel(const HardwareInfo& hw) {
  const auto& c = Catalog();
  int best = -1, smallest = -1;
  for (int i = 0; i < (int)c.size(); i++) {
    if (!c[i].listed || !c[i].preferred) continue;
    if (smallest < 0) smallest = i;
    if (ModelFits(c[i], hw)) best = i;
  }
  return best >= 0 ? best : smallest;
}

// --- Comandi conosciuti dall'assistente ---------------------------------------------------
const std::vector<CommandSpec>& CommandSpecs() {
  static const std::vector<CommandSpec> s = {
      // comandi che creano oggetti
      {"Line", "P P", true, true, "_Line 0,0,0 100,0,0", "line from point to point"},
      {"Polyline", "P P+ _Close|_Enter", true, true, "_Polyline 0,0 50,0 50,30 0,30 _Close",
       "polyline through points, _Close closes it (or _Enter to leave it open)"},
      {"Rectangle", "P P", true, true, "_Rectangle 0,0 80,40", "rectangle on the XY plane from corner to opposite corner"},
      {"RoundedRectangle", "P P N", true, true, "_RoundedRectangle 0,0 80,40 5",
       "rectangle with rounded corners (last number = corner radius)"},
      {"Circle", "P N", true, true, "_Circle 0,0,0 25", "circle: centre, radius"},
      {"Arc", "P P P", true, true, "_Arc 0,0 10,0 0,10", "arc: centre, start point, end point (counterclockwise)"},
      {"Ellipse", "P P P", true, true, "_Ellipse 0,0 30,0 0,15", "ellipse: centre, end of first axis, end of second axis"},
      {"Polygon", "NumSides=I P P", true, true, "_Polygon NumSides=6 0,0 20,0", "regular polygon: centre, a corner"},
      {"InterpCrv", "P P+ _Enter", true, true, "_InterpCrv 0,0 20,10 40,0 60,10 _Enter",
       "smooth curve through the points (3D points allowed), _Enter to finish"},
      {"Helix", "Turns=N P P P", true, true, "_Helix Turns=5 0,0,0 0,0,50 10,0,0",
       "helix: axis start, axis end, point at the radius"},
      {"Box", "P P N", true, true, "_Box 0,0,0 100,50 30", "box: base corner, opposite base corner (x,y), height"},
      {"Sphere", "P N", true, true, "_Sphere 0,0,50 25", "sphere: centre, radius"},
      {"Cylinder", "P N N", true, true, "_Cylinder 0,0,0 20 60", "cylinder: base centre, radius, height (along +Z)"},
      {"Cone", "P N N", true, true, "_Cone 0,0,0 20 40", "cone: base centre, base radius, height (along +Z)"},
      {"TruncatedCone", "P N N N", true, true, "_TruncatedCone 0,0,0 20 30 10",
       "truncated cone: base centre, base radius, height, top radius"},
      {"Tube", "P N N N", true, true, "_Tube 0,0,0 20 17 50",
       "tube (hollow cylinder): base centre, outer radius, inner radius, height"},
      {"Torus", "P N N", true, true, "_Torus 0,0,0 40 5", "torus lying on the XY plane: centre, major radius, minor radius"},
      {"Pyramid", "NumSides=I P P N", true, true, "_Pyramid NumSides=4 0,0,0 20,20 30", "pyramid: base centre, base corner, height"},
      {"Ellipsoid", "P N N N", true, true, "_Ellipsoid 0,0,0 30 20 10", "ellipsoid: centre, radius x, radius y, radius z"},
      {"Wedge", "P P N", true, true, "_Wedge 0,0,0 40,20 15", "wedge: base corner, opposite base corner, height"},
      {"Text", "P S Height=N", true, true, "_Text 0,0,0 \"Kurva3D\" Height=10", "text: insertion point, text, height"},
      // forme composte (estensioni per l'AI: diventano comandi normali di Kurva3D, vedi ExpandScript)
      {"SymCurve", "P P P+ _Enter", true, true, "_SymCurve 0,200 60,180 70,100 40,30 0,0 _Enter",
       "closed smooth outline, symmetric left/right: give only the right half, from the top point ON the axis to the "
       "bottom point ON the axis (same x as the first point); the left half is mirrored. Then extrude it"},
      {"SectionLoft", "X X X+ _Enter", true, true, "_SectionLoft 0,0,30 40 30 0,80,32 30 24 0,150,34 26 20 _Enter",
       "smooth solid through elliptical cross-sections in order along the object: centre, width, height of each "
       "(necks, handles, fuselages, bottles, horns, legs). Sections along a horizontal line are vertical ellipses; "
       "sections stacked in Z are horizontal (width along X, height along Y)"},
      {"Lathe", "P P P+ _Enter", true, true, "_Lathe 0,0,0 40,0 45,60 30,200 12,240 12,280 _Enter",
       "round solid turned around the vertical axis through the first point: then radius,height pairs from bottom to "
       "top, heights always increasing (vases, bottles, cups, knobs, turned legs, columns, lamp shades)"},
      {"Plate", "N P P P+ _Close", true, true, "_Plate 4 0,0,0 0,300,0 0,150,400 _Close",
       "flat part of the given thickness, centred on a closed outline of corner points lying in one plane of ANY "
       "orientation (sails, fins, panels, brackets, blades, signs, the bridge of a violin)"},
      {"PipeThrough", "N P P+ _Enter", true, true, "_PipeThrough 4 0,0,0 0,50,100 0,150,120 _Enter",
       "round rod of the given radius along a smooth path through 3D points (strings, handles, bent tubes, wires, "
       "spokes; with 2 points it is straight)"},
      {"Wheel", "P N N x|y|z I", true, true, "_Wheel 0,0,330 330 60 x 16",
       "wheel standing on its axle: centre, radius, width, axle direction (x, y or z), number of spokes "
       "(0 = solid wheel like a car wheel; with spokes: tyre, hub and spokes, like a bicycle wheel)"},
      {"Blades", "P x|y|z I N N N", true, true, "_Blades 0,0,3000 z 4 5000 300 40",
       "blades radiating from a hub, flat in the plane perpendicular to the axis: centre, axis (x, y or z), number "
       "of blades, length of each blade, width, thickness (helicopter rotors, propellers, fans, windmills)"},
      // selezione
      {"SelNone", "", false, false, "_SelNone", "clear the selection"},
      {"SelName", "W+", false, false, "_SelName Body Hole", "select objects by name (in this order)"},
      {"SelLast", "", false, false, "_SelLast", "select what the previous command created"},
      {"SelAll", "", false, false, "_SelAll", "select everything"},
      // comandi che agiscono sugli oggetti selezionati
      {"BooleanUnion", "", true, false, "_BooleanUnion", "join the selected solids into one"},
      {"BooleanDifference", "", true, false, "_BooleanDifference",
       "subtract: the FIRST selected object is kept, the others are cut away"},
      {"BooleanIntersection", "", true, false, "_BooleanIntersection", "keep only the common part"},
      {"FilletEdge", "N", false, false, "_FilletEdge 2", "round all edges of the selected solid with radius 2"},
      {"ChamferEdge", "N", false, false, "_ChamferEdge 1", "bevel all edges of the selected solid"},
      {"Move", "P P", false, false, "_Move 0,0,0 10,0,0", "move from point to point"},
      {"Copy", "P P+ _Enter", true, false, "_Copy 0,0,0 50,0,0 100,0,0 _Enter", "copies (one per destination point)"},
      {"Rotate", "P N", false, false, "_Rotate 0,0,0 45", "rotate around the vertical axis through the point (degrees)"},
      {"Rotate3D", "P P N", false, false, "_Rotate3D 0,0,0 1,0,0 90", "rotate around the axis from point to point"},
      {"Scale", "P N", false, false, "_Scale 0,0,0 2", "scale from the point"},
      {"Mirror", "P P", true, false, "_Mirror 0,0,0 0,10,0",
       "mirror across the vertical plane through the two points (makes a copy)"},
      {"ArrayPolar", "P I", false, false, "_ArrayPolar 0,0,0 6", "6 copies around the vertical axis through the point"},
      {"ArrayLinear", "I P P", false, false, "_ArrayLinear 4 0,0,0 30,0,0",
       "4 copies, spacing from the first to the second point"},
      {"ExtrudeCrv", "_Solid=_Yes N", true, false, "_ExtrudeCrv _Solid=_Yes 20",
       "extrude the selected closed curve into a solid 20 high"},
      {"Revolve", "P P", true, false, "_Revolve 0,0,0 0,0,1", "revolve the selected curve 360 degrees around the axis"},
      {"Loft", "", true, false, "_Loft", "surface through the selected curves (in order)"},
      {"Cap", "", false, false, "_Cap", "close the open planar ends of the selected loft or extrusion into a solid"},
      {"Pipe", "N", true, false, "_Pipe 2", "pipe of radius 2 around the selected curve"},
      {"SetObjectColor", "C", false, false, "_SetObjectColor 200,30,30", "colour as r,g,b"},
      {"Delete", "", false, false, "_Delete", "delete the selected objects"},
  };
  return s;
}

namespace {
std::string LowerAscii(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}
std::vector<std::string> Words(const std::string& s) {
  std::vector<std::string> w;
  std::istringstream in(s);
  for (std::string t; in >> t;) w.push_back(t);
  return w;
}
bool IsValueCode(const std::string& c) { return c == "N" || c == "I" || c == "S"; }

// Stessi elementi in GBNF e come espressione regolare (per il controllo e i test).
const char* kNumRe = "-?[0-9]{1,8}(\\.[0-9]{1,6})?";
const char* kIntRe = "[0-9]{1,4}";
const char* kStrRe = "\"[^\"\\n]{1,400}\"";  // byte, non caratteri: piu' largo della grammatica
const char* kNameRe = "[A-Za-z][A-Za-z0-9_]{0,31}";
std::string PointRe() { return std::string(kNumRe) + "," + kNumRe + "(," + kNumRe + ")?"; }

std::string QuoteGbnf(const std::string& s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    o += c;
  }
  return o + "\"";
}
std::string EscapeRe(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (std::strchr("\\^$.|?*+()[]{}", c)) o += '\\';
    o += c;
  }
  return o;
}

// Un codice d'argomento in GBNF (gbnf=true) o in espressione regolare.
std::string ArgPattern(const std::string& code, bool gbnf) {
  auto alt = [&](const std::vector<std::string>& v) {
    std::string o = "(";
    for (size_t i = 0; i < v.size(); i++) o += (i ? " | " : "") + (gbnf ? QuoteGbnf(v[i]) : EscapeRe(v[i]));
    if (!gbnf) o.erase(std::remove(o.begin(), o.end(), ' '), o.end());
    return o + ")";
  };
  if (code == "P") return gbnf ? "pt" : "(" + PointRe() + ")";
  if (code == "X") return gbnf ? "sec" : "(" + PointRe() + " " + kNumRe + " " + kNumRe + ")";
  if (code == "X+")
    return gbnf ? "sec (\" \" sec)*" : "(" + PointRe() + " " + kNumRe + " " + kNumRe + ")( " + PointRe() + " " + kNumRe + " " + kNumRe + ")*";
  if (code == "P+") return gbnf ? "pt (\" \" pt)*" : "(" + PointRe() + ")( " + PointRe() + ")*";
  if (code == "N") return gbnf ? "num" : std::string("(") + kNumRe + ")";
  if (code == "I") return gbnf ? "int" : std::string("(") + kIntRe + ")";
  if (code == "S") return gbnf ? "str" : std::string("(") + kStrRe + ")";
  if (code == "W+") return gbnf ? "name (\" \" name)*" : std::string("(") + kNameRe + ")( " + kNameRe + ")*";
  if (code == "C") return gbnf ? "int \",\" int \",\" int" : std::string(kIntRe) + "," + kIntRe + "," + kIntRe;
  const size_t eq = code.find('=');
  if (eq != std::string::npos && IsValueCode(code.substr(eq + 1))) {
    const std::string key = code.substr(0, eq + 1);
    return (gbnf ? QuoteGbnf(key) + " " : EscapeRe(key)) + ArgPattern(code.substr(eq + 1), gbnf);
  }
  if (code.find('|') != std::string::npos) {
    std::vector<std::string> v;
    std::istringstream in(code);
    for (std::string t; std::getline(in, t, '|');) v.push_back(t);
    return alt(v);
  }
  return gbnf ? QuoteGbnf(code) : EscapeRe(code);
}

std::string ArgsPattern(const CommandSpec& c, bool gbnf) {
  std::string o;
  for (const auto& w : Words(c.args)) o += gbnf ? " \" \" " + ArgPattern(w, true) : " " + ArgPattern(w, false);
  return o;
}
}  // namespace

bool AcceptsTargets(const CommandSpec& c) { return !c.creates && std::string(c.name).rfind("Sel", 0) != 0; }

const CommandSpec* FindSpec(const std::string& commandName) {
  std::string n = LowerAscii(commandName);
  n.erase(0, n.find_first_not_of("_-!"));
  for (const auto& c : CommandSpecs())
    if (LowerAscii(c.name) == n) return &c;
  return nullptr;
}

bool ArgumentsMatch(const std::string& lineIn) {
  std::string line = lineIn;
  const size_t arrow = line.find("=>");
  if (arrow != std::string::npos) line = line.substr(0, arrow);
  auto w = Words(line);  // spazi multipli ridotti a uno
  if (w.empty()) return false;
  const CommandSpec* c = FindSpec(w[0]);
  if (!c) return true;
  // destinazioni "@Nome" alla fine dei comandi che agiscono sugli oggetti
  if (AcceptsTargets(*c))
    while (w.size() > 1 && w.back().size() > 1 && w.back()[0] == '@') {
      if (!std::regex_match(w.back().substr(1), std::regex(kNameRe))) return false;
      w.pop_back();
    }
  std::string norm;
  for (size_t i = 1; i < w.size(); i++) norm += " " + w[i];
  static std::map<std::string, std::regex> cache;
  auto it = cache.find(c->name);
  if (it == cache.end())
    it = cache.emplace(c->name, std::regex(ArgsPattern(*c, false), std::regex::ECMAScript | std::regex::icase)).first;
  return std::regex_match(norm, it->second);
}

std::string Grammar() {
  std::ostringstream o;
  o << "root ::= \"```kurva\\n\" (item \"\\n\"){1,120} \"```\"\n";
  o << "item ::= comment | command\n";
  o << "comment ::= \"# \" [^\\n]{1,160}\n";
  o << "command ::= ";
  const auto& s = CommandSpecs();
  for (size_t i = 0; i < s.size(); i++) o << (i ? " | " : "") << "cmd" << i;
  o << "\n";
  for (size_t i = 0; i < s.size(); i++) {
    o << "cmd" << i << " ::= " << QuoteGbnf(std::string("_") + s[i].name) << ArgsPattern(s[i], true);
    if (AcceptsTargets(s[i])) o << " targets?";
    if (s[i].named) o << " named?";
    o << "\n";
  }
  o << "named ::= \" => \" name\n";
  o << "targets ::= (\" @\" name){1,12}\n";
  o << "sec ::= pt \" \" num \" \" num\n";
  o << "name ::= [A-Za-z] [A-Za-z0-9_]{0,31}\n";
  o << "num ::= \"-\"? [0-9]{1,6} (\".\" [0-9]{1,3})?\n";
  o << "int ::= [0-9]{1,4}\n";
  o << "pt ::= num \",\" num (\",\" num)?\n";
  o << "str ::= \"\\\"\" [^\"\\n]{1,80} \"\\\"\"\n";
  return o.str();
}

// --- Istruzioni per il modello -----------------------------------------------------------
std::vector<std::string> ReferenceExamples() {
  std::vector<std::string> v;
  for (const auto& c : CommandSpecs())
    if (c.creates) v.push_back(c.example);
  return v;
}

std::string CompactSystemPrompt() {
  const std::string full = SystemPrompt();
  const size_t ex = full.find("\nExample: ");
  return ex == std::string::npos ? full : full.substr(0, ex + 1);
}

std::string SystemPromptFor(const ModelInfo* m) { return m && m->finetuned ? CompactSystemPrompt() : SystemPrompt(); }

bool IsShapeHelper(const CommandSpec& c) {
  static const std::set<std::string> h = {"SymCurve", "SectionLoft", "Lathe", "Plate", "PipeThrough", "Wheel", "Blades", "Cap"};
  return h.count(c.name) > 0;
}

// Regole e comandi del linguaggio. helpers: anche le forme composte, le destinazioni @Nome
// e i consigli per gli oggetti complessi (le istruzioni classiche restano uguali a quelle
// usate per addestrare il modello Kurva3D).
std::string CommandReference(bool helpers) {
  std::ostringstream o;
  o << R"(Rules of the command language:
- One command per line. Units are millimetres. Points are x,y,z (x,y means z=0). Z is up.
- Numbers use a dot for decimals. Text with spaces goes in double quotes.
- Lines starting with "# " are comments: use a few of them first to plan the parts.
- Commands that create objects:
)";
  auto pad = [](const std::string& s, size_t w) { return s.size() >= w ? s + "  " : s + std::string(w - s.size(), ' '); };
  for (const auto& c : CommandSpecs())
    if (c.creates && !IsShapeHelper(c)) o << "  " << pad(c.example, 40) << "(" << c.note << ")\n";
  if (helpers) {
    o << "- Shape commands for complex and organic objects (they create one solid each):\n";
    for (const auto& c : CommandSpecs())
      if (c.creates && IsShapeHelper(c)) o << "  " << c.example << "\n      " << c.note << "\n";
  }
  o << R"(- Naming: add " => Name" (one word, no spaces) at the end of a creating command to name what it creates, e.g.
  _Box 0,0,0 100,50 30 => Body
- Commands that act on objects need a selection first:
)";
  bool acting = false;
  for (const auto& c : CommandSpecs()) {
    if (c.creates || (!helpers && IsShapeHelper(c))) continue;
    const bool sel = std::string(c.name).rfind("Sel", 0) == 0;
    if (!sel && !acting) { o << "  then one of:\n"; acting = true; }
    o << "  " << pad(c.example, 40) << c.note << "\n";
  }
  if (helpers)
    o << R"(- Shortcut: end an acting command with @Name for each object it works on; it selects them first
  (same as _SelNone then _SelName). Examples: _BooleanUnion @Seat @Legs => Chair   _Delete @Path
  _Mirror 0,0,0 0,1,0 @WingR => WingL   _Copy 0,0,0 0,0,30 _Enter @Outline => Rim   _SetObjectColor 120,70,30 @Neck
)";
  o << R"(- Never use commands for files, undo, layers or settings.
- Build solids from simple primitives, then combine them with the boolean commands.
- Place parts so that solids to be joined overlap a little (at least 0.5 mm).
- Compute every coordinate from the requested size: each part must be at its final position.
  Never create several identical objects at the same place: _SelLast selects only the last one.
  Name each part (=> Name) and select parts by name, or create one part and _Copy it.
- Flat parts with a non-rectangular outline (rhombus, triangle, L shape, ...): draw the closed outline
  with _Polyline ... _Close at the right height, then _ExtrudeCrv _Solid=_Yes thickness. Then delete the curve.
- _Revolve is only for round objects (vases, bottles, wheels); never revolve a flat shape to make a plate.
- Bent rods and stems: draw the path with _InterpCrv through 3D points, then _Pipe radius. Then delete the path.
  Leave bent pipes as separate parts: do not use boolean commands on them.
- Use _BooleanUnion only on solids that really touch or overlap; if unsure, leave the parts separate.
- Before writing, plan the parts with real proportions and their height range in Z: the base on the floor
  (z = 0), what stands on it above, the top part at the top. Every part has its own Z: never put all the
  parts at 0,0,0. A part that is completely inside another one adds nothing.
- Typical sizes when the user gives none: table height 750 (top 25-40 thick), dining table top 1200x800 or
  diameter 900-1200, coffee table height 400, chair seat height 450 and backrest top 850-900, desk 1400x700,
  shelf thickness 20, table leg 40-60 wide, central column diameter 80-120 with a base diameter 400-600,
  desk lamp height 400-550 with a base diameter 150-250.
)";
  if (helpers)
    o << R"(  More sizes: violin 595 long (body 356 long, 208 wide, ribs 32 high), cello 1250, acoustic guitar 1000 long
  (body 490x380x100), electric guitar 980, wine bottle 300 high and 75 wide, mug 95 high and 80 wide, teapot 200
  high, glass 120 high, bicycle 1750 long and 1000 high (wheels diameter 700), car 4300x1800x1450 (wheels
  diameter 660), sofa 2000x900x850 (seat 450), bed 2000x1600 (mattress top 550), sailboat 8000 long (mast
  10000), airliner 38000 long (wingspan 34000), helicopter 12000, house door 2100x900, person 1750.
- How to build complex objects: split them into parts with real proportions and build each part with the
  command that fits its shape:
  curvy outline seen from above (guitar or violin body, boat deck, car body, leaf): _SymCurve, then
    _ExtrudeCrv _Solid=_Yes height @Outline (for a domed top: _Copy the outline up, _Scale it smaller,
    _Loft @Low @High, _Cap, _BooleanUnion with the body);
  long rounded parts (necks, handles, fuselages, horns, arms, legs, tails): _SectionLoft;
  round turned parts standing on their axis (vases, bottles, glasses, lamp shades, knobs, columns): _Lathe;
  thin flat parts in any orientation (wings, fins, sails, blades, panels, bridges): _Plate;
  rods, strings, wires, tubes and frames: _PipeThrough;
  wheels (cars, bicycles, carts, trains): _Wheel; rotors, propellers, fans: _Blades;
  holes and slots: make a cutting solid, then _BooleanDifference @Body @Cutter => Body;
  symmetric pairs (wings, wheels, legs, eyes): make one, then _Mirror 0,0,0 0,1,0 @Part => Part
    (mirror across the plane x = 0);
  colours make the parts readable: _SetObjectColor r,g,b @Part.
)";
  return o.str();
}

std::string SystemPrompt() {
  std::ostringstream o;
  o << R"(You are the modelling assistant of Kurva3D, a NURBS CAD program similar to Rhino 8.
The user describes an object (often in Italian). You answer ONLY with a script of Kurva3D commands
inside one code block that starts with ```kurva and ends with ```. No explanations outside the block.

)" << CommandReference(false) << R"(
Example: "un tavolo 120x80 alto 75 cm" (cm -> mm)
```kurva
# Top 1200x800, z 710-750. Four legs 60x60, z 0-710, at the corners
_Box 0,0,710 1200,800 40 => Top
_Box 40,40,0 100,100 710 => Leg
_SelNone
_SelName Leg
_Copy 0,0,0 1060,0,0 1060,660,0 0,660,0 _Enter
_SelAll
_BooleanUnion
```

Example: "un tavolo a forma di rombo con le gambe sotto ogni vertice" (diagonals 1200 and 800, height 750)
```kurva
# Rhombus top z 720-750 from a closed outline. Four round legs z 0-725 near the vertices
_Polyline 600,0,720 0,400,720 -600,0,720 0,-400,720 _Close => Outline
_SelNone
_SelName Outline
_ExtrudeCrv _Solid=_Yes 30 => Top
_SelNone
_SelName Outline
_Delete
_Cylinder 520,0,0 25 725 => Leg1
_Cylinder 0,330,0 25 725 => Leg2
_Cylinder -520,0,0 25 725 => Leg3
_Cylinder 0,-330,0 25 725 => Leg4
_SelNone
_SelName Top Leg1 Leg2 Leg3 Leg4
_BooleanUnion
```

Example: "un tavolo rotondo con una gamba centrale a cilindro e la base larga"
```kurva
# Top diameter 900, z 720-750. Column diameter 80, z 50-725. Base disc diameter 500, z 0-55
_Cylinder 0,0,720 450 30 => Top
_Cylinder 0,0,50 40 675 => Column
_Cylinder 0,0,0 250 55 => Base
_SelNone
_SelName Top Column Base
_BooleanUnion
```

Example: "una sedia"
```kurva
# Seat 450x450, z 420-450. Four legs 40x40, z 0-425. Two rear posts and a back panel up to z 900
_Box 0,0,420 450,450 30 => Seat
_Box 0,0,0 40,40 425 => Leg
_SelNone
_SelName Leg
_Copy 0,0,0 410,0,0 410,410,0 0,410,0 _Enter
_Box 0,410,445 40,450 455 => PostL
_Box 410,410,445 450,450 455 => PostR
_Box 0,420,650 450,445 250 => Back
_SelAll
_BooleanUnion
```

Example: "una lampada da tavolo ad arco"
```kurva
# Base disc diameter 220, z 0-20. Bent stem: pipe along a curve from the base up and forward.
# Shade: open tube (diameter 180) at the end of the stem, z 320-440. The bent stem stays a separate part
_Cylinder 0,0,0 110 20 => Base
_InterpCrv 0,0,10 0,40,250 0,160,430 0,300,440 _Enter => Path
_SelNone
_SelName Path
_Pipe 8 => Stem
_SelNone
_SelName Path
_Delete
_Tube 0,300,320 90 86 120 => Shade
```

Example: "una tazza"
```kurva
# Body diameter 80, z 0-95, hollowed from z 6. Handle: torus turned upright on the side
_Cylinder 0,0,0 40 95 => Body
_Cylinder 0,0,6 35 95 => Inside
_Torus 0,0,0 22 5 => Handle
_SelNone
_SelName Handle
_Rotate3D 0,0,0 1,0,0 90
_Move 0,0,0 42,0,50
_SelNone
_SelName Body Handle
_BooleanUnion => Mug
_SelNone
_SelName Mug Inside
_BooleanDifference
```
)";
  return o.str();
}

// --- Lettura della risposta ----------------------------------------------------------------
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
std::string CommandName(const std::string& line) {
  std::string t = Trim(line);
  size_t i = 0;
  while (i < t.size() && (t[i] == '_' || t[i] == '-' || t[i] == '!')) i++;
  size_t j = i;
  while (j < t.size() && !std::isspace((unsigned char)t[j])) j++;
  return Lower(t.substr(i, j - i));
}
// Nomi che i modelli inventano spesso, ricondotti al comando giusto.
const char* Alias(const std::string& n) {
  static const std::pair<const char*, const char*> a[] = {
      {"cube", "Box"}, {"cuboid", "Box"}, {"block", "Box"}, {"ball", "Sphere"}, {"cyl", "Cylinder"},
      {"union", "BooleanUnion"}, {"booleanadd", "BooleanUnion"}, {"difference", "BooleanDifference"},
      {"subtract", "BooleanDifference"}, {"booleansubtract", "BooleanDifference"},
      {"intersection", "BooleanIntersection"}, {"booleanintersect", "BooleanIntersection"},
      {"extrude", "ExtrudeCrv"}, {"fillet3d", "FilletEdge"}, {"selectname", "SelName"}, {"selectall", "SelAll"},
      {"selectnone", "SelNone"}, {"deselectall", "SelNone"}, {"rect", "Rectangle"}, {"polygonal", "Polygon"}};
  for (const auto& [k, v] : a) if (n == k) return v;
  return nullptr;
}
}  // namespace

std::vector<std::string> ExtractScript(const std::string& replyIn) {
  std::string reply = replyIn;
  // ragionamento dei modelli "thinking"
  for (size_t a; (a = reply.find("<think>")) != std::string::npos;) {
    const size_t b = reply.find("</think>", a);
    reply.erase(a, b == std::string::npos ? std::string::npos : b + 8 - a);
  }
  std::string body = reply;
  const size_t f = reply.find("```");
  if (f != std::string::npos) {
    const size_t nl = reply.find('\n', f);
    const size_t end = nl == std::string::npos ? std::string::npos : reply.find("```", nl);
    if (nl != std::string::npos) body = reply.substr(nl + 1, end == std::string::npos ? std::string::npos : end - nl - 1);
  }
  std::vector<std::string> out;
  std::istringstream in(body);
  for (std::string l; std::getline(in, l);) {
    l = Trim(l);
    if (l.empty() || l[0] == '#' || l.rfind("//", 0) == 0 || l[0] == ';') continue;
    if (l[0] != '_' && l[0] != '-' && l[0] != '!') {
      // nome di comando senza il trattino basso
      if (!std::isalpha((unsigned char)l[0])) continue;
      l = "_" + l;
    }
    if (const char* real = Alias(CommandName(l))) {
      size_t i = 0;
      while (i < l.size() && (l[i] == '_' || l[i] == '-' || l[i] == '!')) i++;
      size_t j = i;
      while (j < l.size() && !std::isspace((unsigned char)l[j])) j++;
      l = l.substr(0, i) + real + l.substr(j);
    }
    out.push_back(l);
  }
  return out;
}

namespace {
std::string Fmt(double v) {
  if (std::fabs(v) < 5e-7) v = 0;
  char b[48];
  std::snprintf(b, sizeof b, "%.6g", v);
  return b;
}
std::string Pt(const ON_3dPoint& p) { return Fmt(p.x) + "," + Fmt(p.y) + "," + Fmt(p.z); }
// Legge "x,y[,z]"
bool ParsePt(const std::string& s, ON_3dPoint& p) {
  double v[3] = {0, 0, 0};
  int n = 0;
  std::istringstream in(s);
  for (std::string t; std::getline(in, t, ',') && n < 3;) {
    char* e = nullptr;
    v[n++] = std::strtod(t.c_str(), &e);
    if (e == t.c_str() || *e) return false;
  }
  if (n < 2 || !in.eof()) return false;
  p = ON_3dPoint(v[0], v[1], v[2]);
  return true;
}
bool ParseNum(const std::string& s, double& v) {
  char* e = nullptr;
  v = std::strtod(s.c_str(), &e);
  return e != s.c_str() && !*e;
}
bool IsEnterWord(const std::string& w) { const std::string l = LowerAscii(w); return l == "_enter" || l == "enter"; }

// Comandi normali per una forma composta. tmp: prefisso dei nomi provvisori.
// name: nome del risultato (vuoto: nessuno). Restituisce false se gli argomenti non vanno.
bool ExpandShape(const std::string& cmd, const std::vector<std::string>& args, const std::string& tmp,
                 const std::string& name, std::vector<std::string>& out) {
  std::vector<std::string> a;
  for (const auto& w : args) if (!IsEnterWord(w)) a.push_back(w);
  auto nameLast = [&](const std::string& n) {
    out.push_back("_SelNone");
    out.push_back("_SelLast");
    out.push_back("_SetObjectName \"" + n + "\"");
    out.push_back("_SelNone");
  };
  auto deleteNamed = [&](const std::string& names) {
    out.push_back("_SelNone");
    out.push_back("_SelName " + names);
    out.push_back("_Delete");
  };
  const std::string res = name.empty() ? tmp + "R" : name;
  if (cmd == "symcurve") {
    std::vector<ON_3dPoint> p(a.size());
    for (size_t i = 0; i < a.size(); i++) if (!ParsePt(a[i], p[i])) return false;
    if (p.size() < 3) return false;
    const double ax = p[0].x;
    std::string l = "_InterpCrv";
    for (const auto& q : p) l += " " + Pt(q);
    for (size_t i = p.size() - 1; i >= 1; i--)
      if (std::fabs(p[i].x - ax) > 1e-6) l += " " + Pt(ON_3dPoint(2 * ax - p[i].x, p[i].y, p[i].z));
    out.push_back(l + " _Close");
    if (!name.empty()) nameLast(name);
    return true;
  }
  if (cmd == "plate") {
    double t;
    if (a.size() < 4 || !ParseNum(a[0], t) || t <= 0) return false;
    std::vector<ON_3dPoint> p;
    for (size_t i = 1; i < a.size(); i++) {
      if (LowerAscii(a[i]) == "_close" || LowerAscii(a[i]) == "close") continue;
      ON_3dPoint q;
      if (!ParsePt(a[i], q)) return false;
      p.push_back(q);
    }
    if (p.size() < 3) return false;
    ON_3dVector n(0, 0, 0);  // normale (metodo di Newell)
    for (size_t i = 0; i < p.size(); i++) {
      const ON_3dPoint& u = p[i];
      const ON_3dPoint& v = p[(i + 1) % p.size()];
      n.x += (u.y - v.y) * (u.z + v.z);
      n.y += (u.z - v.z) * (u.x + v.x);
      n.z += (u.x - v.x) * (u.y + v.y);
    }
    if (!n.Unitize()) return false;
    if (n.z < 0) n = -n;
    const ON_3dPoint o = p[0];
    ON_3dVector axis = ON_CrossProduct(n, ON_3dVector::ZAxis);
    const bool flat = axis.Length() < 1e-6;
    const double ang = std::acos(std::max(-1.0, std::min(1.0, n.z))) * 180.0 / ON_PI;
    std::string l = "_Polyline";
    for (const auto& q : p) l += " " + Pt(flat ? q - ON_3dVector(0, 0, t / 2) : q);
    out.push_back(l + " _Close");
    nameLast(tmp + "P");
    out.push_back("_SelName " + tmp + "P");
    if (!flat) {
      axis.Unitize();
      // il contorno ruotato in orizzontale attorno a un asse per il primo punto, poi abbassato
      out.push_back("_Rotate3D " + Pt(o) + " " + Pt(o + axis) + " " + Fmt(ang));
      out.push_back("_Move 0,0,0 0,0," + Fmt(-t / 2));
    }
    out.push_back("_ExtrudeCrv _Solid=_Yes " + Fmt(t));
    if (!flat) {
      out.push_back("_SelNone");
      out.push_back("_SelLast");
      out.push_back("_Rotate3D " + Pt(o) + " " + Pt(o + axis) + " " + Fmt(-ang));
    }
    nameLast(res);
    deleteNamed(tmp + "P");
    return true;
  }
  if (cmd == "pipethrough") {
    double r;
    if (a.size() < 3 || !ParseNum(a[0], r)) return false;
    std::vector<ON_3dPoint> p(a.size() - 1);
    for (size_t i = 1; i < a.size(); i++) if (!ParsePt(a[i], p[i - 1])) return false;
    std::string l = p.size() == 2 ? "_Line" : "_InterpCrv";
    for (const auto& q : p) l += " " + Pt(q);
    out.push_back(p.size() == 2 ? l : l + " _Enter");
    nameLast(tmp + "P");
    out.push_back("_SelName " + tmp + "P");
    out.push_back("_Pipe " + Fmt(r));
    nameLast(res);
    deleteNamed(tmp + "P");
    return true;
  }
  // assi per _Wheel e _Blades: direzione dell'asse e due direzioni perpendicolari
  auto axisOf = [](const std::string& w, ON_3dVector& ax, ON_3dVector& u, ON_3dVector& v) {
    const std::string l = LowerAscii(w);
    if (l == "x") { ax = ON_3dVector(1, 0, 0); u = ON_3dVector(0, 1, 0); v = ON_3dVector(0, 0, 1); return true; }
    if (l == "y") { ax = ON_3dVector(0, 1, 0); u = ON_3dVector(1, 0, 0); v = ON_3dVector(0, 0, 1); return true; }
    if (l == "z") { ax = ON_3dVector(0, 0, 1); u = ON_3dVector(1, 0, 0); v = ON_3dVector(0, 1, 0); return true; }
    return false;
  };
  if (cmd == "wheel") {
    ON_3dPoint c;
    ON_3dVector ax, u, v;
    double r, w, nd;
    if (a.size() != 5 || !ParsePt(a[0], c) || !ParseNum(a[1], r) || !ParseNum(a[2], w) || !axisOf(a[3], ax, u, v) ||
        !ParseNum(a[4], nd) || r <= 0 || w <= 0 || nd < 0 || nd > 64)
      return false;
    const int n = (int)nd;
    if (n == 0)  // ruota piena: cilindro sull'asse
      return ExpandShape("pipethrough", {Fmt(r), Pt(c - ax * (w / 2)), Pt(c + ax * (w / 2))}, tmp + "w", res, out);
    // copertone (toro attorno all'asse), mozzo e raggi
    const double t = std::min(w / 2, r * 0.12);
    out.push_back("_Torus " + Pt(c) + " " + Fmt(r - t) + " " + Fmt(t));
    if (std::fabs(ax.z) < 0.5) {
      out.push_back("_SelNone");
      out.push_back("_SelLast");
      const ON_3dVector rot = ax.x > 0.5 ? ON_3dVector(0, 1, 0) : ON_3dVector(1, 0, 0);
      out.push_back("_Rotate3D " + Pt(c) + " " + Pt(c + rot) + " 90");
    }
    nameLast(res);
    const double hub = std::max(r * 0.07, t);
    if (!ExpandShape("pipethrough", {Fmt(hub), Pt(c - ax * (w / 2)), Pt(c + ax * (w / 2))}, tmp + "h", res, out))
      return false;
    const double sr = std::max(r * 0.008, 0.5);
    for (int i = 0; i < n; i++) {
      const double ang = 2 * ON_PI * i / n;
      const ON_3dVector d = u * std::cos(ang) + v * std::sin(ang);
      if (!ExpandShape("pipethrough", {Fmt(sr), Pt(c + d * (hub * 0.8)), Pt(c + d * (r - t))}, tmp + "s", res, out))
        return false;
    }
    return true;
  }
  if (cmd == "blades") {
    ON_3dPoint c;
    ON_3dVector ax, u, v;
    double nd, len, chord, th;
    if (a.size() != 6 || !ParsePt(a[0], c) || !axisOf(a[1], ax, u, v) || !ParseNum(a[2], nd) || !ParseNum(a[3], len) ||
        !ParseNum(a[4], chord) || !ParseNum(a[5], th) || nd < 1 || nd > 16 || len <= 0 || chord <= 0 || th <= 0)
      return false;
    const int n = (int)nd;
    const double hub = std::min(chord * 0.6, len * 0.3);
    if (!ExpandShape("pipethrough", {Fmt(hub), Pt(c - ax * (th * 1.5)), Pt(c + ax * (th * 1.5))}, tmp + "h", res, out))
      return false;
    const double r0 = std::min(chord * 0.5, len * 0.15);
    for (int i = 0; i < n; i++) {
      const double ang = 2 * ON_PI * i / n;
      const ON_3dVector d = u * std::cos(ang) + v * std::sin(ang);
      const ON_3dVector s = ON_CrossProduct(ax, d);
      const std::vector<std::string> pa = {Fmt(th), Pt(c + d * r0 - s * (chord * 0.3)), Pt(c + d * len - s * (chord / 2)),
                                           Pt(c + d * len + s * (chord / 2)), Pt(c + d * r0 + s * (chord * 0.3))};
      if (!ExpandShape("plate", pa, tmp + "b" + std::to_string(i), res, out)) return false;
    }
    return true;
  }
  if (cmd == "lathe") {
    // sezioni circolari impilate in Z e loft (solido di OpenCASCADE: le booleane funzionano)
    ON_3dPoint c;
    if (a.size() < 3 || !ParsePt(a[0], c)) return false;
    std::vector<std::string> sec;
    double lastH = -1e300, maxR = 0;
    std::vector<ON_3dPoint> rh;
    for (size_t i = 1; i < a.size(); i++) {
      ON_3dPoint q;
      if (!ParsePt(a[i], q) || q.y <= lastH + 1e-6) return false;  // altezze crescenti
      lastH = q.y;
      maxR = std::max(maxR, std::fabs(q.x));
      rh.push_back(q);
    }
    if (maxR <= 0) return false;
    // profilo liscio (curva interpolata raggio/altezza) campionato fitto: il loft segue il profilo
    if (rh.size() >= 3) {
      std::vector<ON_3dPoint> pr;
      for (const auto& q : rh) pr.push_back(ON_3dPoint(std::fabs(q.x), q.y, 0));
      std::unique_ptr<ON_NurbsCurve> nc(InterpolateCubic(pr));
      if (nc) {
        const int n = std::min(48, 4 * ((int)rh.size() - 1) + 1);
        std::vector<ON_3dPoint> dense;
        bool mono = true;
        for (int i = 0; i < n; i++) {
          const ON_3dPoint q = nc->PointAt(nc->Domain().ParameterAt(double(i) / (n - 1)));
          if (!dense.empty() && q.y <= dense.back().y + 1e-6) { mono = false; break; }
          dense.push_back(ON_3dPoint(std::max(0.0, q.x), q.y, 0));
        }
        if (mono) rh = dense;
      }
    }
    std::vector<std::string> args;
    for (const auto& q : rh) {
      const double d = 2 * std::max(std::fabs(q.x), maxR * 0.01);
      args.push_back(Pt(c + ON_3dVector(0, 0, q.y)));
      args.push_back(Fmt(d));
      args.push_back(Fmt(d));
    }
    return ExpandShape("sectionloft", args, tmp, name, out);
  }
  if (cmd == "sectionloft") {
    if (a.size() < 6 || a.size() % 3) return false;
    struct Sec { ON_3dPoint c; double w, h; };
    std::vector<Sec> sec(a.size() / 3);
    for (size_t i = 0; i < sec.size(); i++)
      if (!ParsePt(a[3 * i], sec[i].c) || !ParseNum(a[3 * i + 1], sec[i].w) || !ParseNum(a[3 * i + 2], sec[i].h) ||
          sec[i].w <= 0 || sec[i].h <= 0)
        return false;
    std::string names;
    for (size_t i = 0; i < sec.size(); i++) {
      ON_3dVector d = sec[std::min(i + 1, sec.size() - 1)].c - sec[i > 0 ? i - 1 : 0].c;
      if (!d.Unitize()) return false;
      const ON_3dPoint& c = sec[i].c;
      std::string l;
      if (std::fabs(d.z) > 0.7) {  // sezioni una sopra l'altra: ellissi orizzontali
        l = "_Ellipse " + Pt(c) + " " + Pt(c + ON_3dVector(sec[i].w / 2, 0, 0)) + " " + Pt(c + ON_3dVector(0, sec[i].h / 2, 0));
      } else {  // sezioni lungo una linea orizzontale: ellissi verticali
        ON_3dVector u = ON_CrossProduct(ON_3dVector::ZAxis, ON_3dVector(d.x, d.y, 0));
        if (!u.Unitize()) return false;
        l = "_Ellipse _Vertical " + Pt(c) + " " + Pt(c + u * (sec[i].w / 2)) + " " + Pt(c + ON_3dVector(0, 0, sec[i].h / 2));
      }
      out.push_back(l);
      const std::string sn = tmp + "S" + std::to_string(i + 1);
      nameLast(sn);
      names += (names.empty() ? "" : " ") + sn;
    }
    out.push_back("_SelName " + names);
    out.push_back("_Loft");
    out.push_back("_SelNone");
    out.push_back("_SelLast");
    out.push_back("_Cap");
    nameLast(res);
    deleteNamed(names);
    return true;
  }
  return false;
}
}  // namespace

std::vector<std::string> ExpandScript(const std::vector<std::string>& lines, std::vector<int>* origin) {
  std::vector<std::string> out;
  if (origin) origin->clear();
  for (int k = 0; k < (int)lines.size(); k++) {
    const size_t before = out.size();
    std::string l = lines[k];
    std::string name;
    const size_t arrow = l.find("=>");
    if (arrow != std::string::npos) {
      name = Trim(l.substr(arrow + 2));
      if (name.size() >= 2 && name.front() == '"' && name.back() == '"') name = name.substr(1, name.size() - 2);
      // un nome e' una parola sola (SelName separa i nomi con gli spazi)
      name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char c) { return std::isspace(c) || c == '"'; }),
                 name.end());
      l = Trim(l.substr(0, arrow));
    }
    auto w = Words(l);
    // destinazioni "@A @B" alla fine: selezione per nome prima del comando
    std::vector<std::string> targets;
    while (w.size() > 1 && w.back().size() > 1 && w.back()[0] == '@') {
      targets.insert(targets.begin(), w.back().substr(1));
      w.pop_back();
    }
    if (!targets.empty()) {
      std::string sel = "_SelName";
      for (const auto& t : targets) sel += " " + t;
      out.push_back("_SelNone");
      out.push_back(sel);
      l.clear();
      for (const auto& x : w) l += (l.empty() ? "" : " ") + x;
    }
    const std::string cmd = w.empty() ? std::string() : CommandName(w[0]);
    bool done = false;
    if (cmd == "symcurve" || cmd == "pipethrough" || cmd == "lathe" || cmd == "sectionloft" || cmd == "plate" ||
        cmd == "wheel" || cmd == "blades") {
      std::vector<std::string> args(w.begin() + 1, w.end());
      std::vector<std::string> ex;
      if (ExpandShape(cmd, args, "zzK" + std::to_string(k + 1), name, ex)) {
        out.insert(out.end(), ex.begin(), ex.end());
        done = true;
      }
      // argomenti sbagliati: la riga resta com'e' e Kurva3D la rifiuta con un errore
    }
    if (!done) {
      out.push_back(l);
      if (!name.empty()) {
        out.push_back("_SelNone");
        out.push_back("_SelLast");
        out.push_back("_SetObjectName \"" + name + "\"");
        out.push_back("_SelNone");
      }
    }
    if (origin) origin->insert(origin->end(), out.size() - before, k);
  }
  return out;
}

bool CommandAllowed(const std::string& raw) {
  static const char* blocked[] = {"new", "open", "save", "saveas", "export", "import", "incrementalsave", "purge",
                                  "undo", "redo", "gripmove", "layer", "layeron", "layeroff", "layerlock",
                                  "layerunlock", "currentlayer", "blockedit", "heightfield", "textobject"};
  const std::string n = CommandName(raw);
  for (const char* b : blocked) if (n == b) return false;
  return true;
}

Validation Validate(Engine& e, const std::vector<std::string>& lines, const std::function<void(Engine&)>& after) {
  Validation v;
  if (lines.empty()) { v.error = "nessun comando nella risposta"; return v; }
  // prima la forma di ogni riga scritta dal modello (comandi della tabella)
  for (size_t k = 0; k < lines.size(); k++) {
    const auto w = Words(lines[k]);
    const CommandSpec* spec = w.empty() ? nullptr : FindSpec(w[0]);
    if (spec && !ArgumentsMatch(lines[k])) {
      v.line = (int)k;
      v.error = std::string("wrong arguments for ") + spec->name + ": write it like " + spec->example + " (" + spec->note + ")";
      return v;
    }
    // forma composta con valori impossibili (es. altezze di _Lathe non crescenti)
    if (spec && IsShapeHelper(*spec) && std::string(spec->name) != "Cap") {
      const auto x = ExpandScript({lines[k]});
      if (!x.empty() && LowerAscii(CommandName(x[0])) == LowerAscii(spec->name)) {
        v.line = (int)k;
        v.error = std::string("impossible values for ") + spec->name + " (" + spec->note + "); example: " + spec->example;
        return v;
      }
    }
  }
  std::vector<int> origin;
  const auto expanded = ExpandScript(lines, &origin);
  Document3dm scratch;
  if (!scratch.FromBuffer(e.Document().ToBuffer())) { v.error = "copia del documento non riuscita"; return v; }
  std::ostringstream log;
  Engine se(scratch, log);
  const size_t before = se.AllObjectIds(true).size();
  auto boxOf = [&](const ON_UUID& id) {
    ON_BoundingBox b;
    if (const auto* o = se.Object(id)) if (const ON_Geometry* g = o->Geometry(nullptr)) b = g->BoundingBox();
    return b;
  };
  auto nameOf = [&](const ON_UUID& id) {
    const auto* o = se.Object(id);
    const ON_3dmObjectAttributes* a = o ? o->Attributes(nullptr) : nullptr;
    return a && !a->m_name.IsEmpty() ? ToUtf8(a->m_name) : std::string("an object");
  };
  const double tol = 0.01;
  auto inside = [&](const ON_BoundingBox& a, const ON_BoundingBox& b) {  // a dentro b
    return a.IsValid() && b.IsValid() && a.m_min.x >= b.m_min.x - tol && a.m_min.y >= b.m_min.y - tol &&
           a.m_min.z >= b.m_min.z - tol && a.m_max.x <= b.m_max.x + tol && a.m_max.y <= b.m_max.y + tol &&
           a.m_max.z <= b.m_max.z + tol;
  };
  for (size_t i = 0; i < expanded.size(); i++) {
    const std::string name = CommandName(expanded[i]);
    // pezzi da unire uno dentro l'altro: l'unione non aggiunge niente (di solito sono tutti a 0,0,0)
    if (name == "booleanunion") {
      const auto sel = se.Selection();
      for (size_t a = 0; a < sel.size(); a++)
        for (size_t b = 0; b < sel.size(); b++)
          if (a != b && inside(boxOf(sel[a]), boxOf(sel[b]))) {
            v.line = origin[i];
            v.error = nameOf(sel[a]) + " is completely inside " + nameOf(sel[b]) +
                      ": give every part its own position and Z height (base on the floor, top above), then join";
            return v;
          }
    }
    const auto idsBefore = se.AllObjectIds(true);
    if (!se.Commands().count(name)) {
      v.line = origin[i];
      v.error = "unknown command " + name + ": use only the commands listed in the reference";
      return v;
    }
    if (!CommandAllowed(expanded[i])) { v.line = origin[i]; v.error = "comando non permesso: " + name; return v; }
    if (!se.Run(expanded[i])) {
      v.line = origin[i];
      v.error = se.LastError().empty() ? "il comando non e' riuscito" : se.LastError();
      return v;
    }
    // un oggetto nuovo identico per posizione e misure a uno gia' esistente
    const std::set<ON_UUID, UuidLess> old(idsBefore.begin(), idsBefore.end());
    for (const auto& id : se.AllObjectIds(true)) {
      if (old.count(id)) continue;
      const ON_BoundingBox nb = boxOf(id);
      for (const auto& o : idsBefore)
        if (inside(nb, boxOf(o)) && inside(boxOf(o), nb) && name != "booleanunion" && name != "booleandifference" &&
            name != "booleanintersection" && name != "selname") {
          v.line = origin[i];
          v.error = "this creates an object in exactly the same place and size as " + nameOf(o) +
                    ": compute the real position of each part";
          return v;
        }
    }
  }
  v.created = (int)se.AllObjectIds(true).size() - (int)before;
  if (se.AllObjectIds(true).empty()) {
    v.line = (int)lines.size() - 1;
    v.error = "the script leaves the model empty: create the parts and do not delete them all";
    return v;
  }
  v.ok = true;
  if (after) after(se);
  return v;
}

std::string ErrorHint(const std::string& error) {
  // messaggi di Kurva3D (in italiano) spiegati al modello in inglese
  static const std::pair<const char*, const char*> hints[] = {
      {"parallelepipedo degenere",
       "the box is flat: _Box needs a base corner x0,y0,z0, the opposite base corner x1,y1 that differs in x AND in y, "
       "then the height as a number"},
      {"tubo non riuscito", "the pipe failed: use a path whose points are not all in the same place and a smaller radius"},
      {"nessuna curva selezionata", "no curve was selected: name the curve with => Name and use it with @Name"},
      {"seleziona prima l'oggetto da cui sottrarre",
       "_BooleanDifference needs @Body first and then the cutting objects: _BooleanDifference @Body @Cutter => Body"},
      {"nessun oggetto selezionato", "no object was selected: use @Name with the name of an existing object"},
  };
  for (const auto& [it, en] : hints)
    if (error.find(it) != std::string::npos) return std::string(" (") + en + ")";
  return {};
}

std::string RepairRequest(const std::vector<std::string>& lines, const Validation& v) {
  std::ostringstream o;
  o << "Line " << (v.line + 1) << " of your script fails";
  if (v.line >= 0 && v.line < (int)lines.size()) o << " (" << lines[v.line] << ")";
  o << ": " << v.error << ErrorHint(v.error) << ". Answer again with the whole corrected script in a ```kurva block.";
  return o.str();
}

}  // namespace cad::ai
