#include "ai/Assistant.h"
#include "commands/Engine.h"
#include "core/Document3dm.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
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
    if (!c[i].listed) continue;
    if (smallest < 0) smallest = i;
    if (ModelFits(c[i], hw)) best = i;
  }
  return best >= 0 ? best : smallest;
}

// --- Istruzioni per il modello -----------------------------------------------------------
std::vector<std::string> ReferenceExamples() {
  return {
      "_Line 0,0,0 100,0,0",
      "_Polyline 0,0 50,0 50,30 0,30 _Close",
      "_Rectangle 0,0 80,40",
      "_RoundedRectangle 0,0 80,40 5",
      "_Circle 0,0,0 25",
      "_Arc 0,0 10,0 0,10",
      "_Ellipse 0,0 30,0 0,15",
      "_Polygon NumSides=6 0,0 20,0",
      "_InterpCrv 0,0 20,10 40,0 60,10 _Enter",
      "_Helix Turns=5 0,0,0 0,0,50 10,0,0",
      "_Box 0,0,0 100,50 30",
      "_Sphere 0,0,50 25",
      "_Cylinder 0,0,0 20 60",
      "_Cone 0,0,0 20 40",
      "_TruncatedCone 0,0,0 20 30 10",
      "_Tube 0,0,0 20 17 50",
      "_Torus 0,0,0 40 5",
      "_Pyramid NumSides=4 0,0,0 20,20 30",
      "_Ellipsoid 0,0,0 30 20 10",
      "_Wedge 0,0,0 40,20 15",
      "_Text 0,0,0 \"Kurva3D\" Height=10",
  };
}

std::string SystemPrompt() {
  std::ostringstream o;
  o << R"(You are the modelling assistant of Kurva3D, a NURBS CAD program similar to Rhino 8.
The user describes an object (often in Italian). You answer ONLY with a script of Kurva3D commands
inside one code block that starts with ```kurva and ends with ```. No explanations outside the block.

Rules of the command language:
- One command per line. Units are millimetres. Points are x,y,z (x,y means z=0). Z is up.
- Numbers use a dot for decimals. Text with spaces goes in double quotes.
- Commands that create objects:
)";
  const char* notes[] = {
      "line from point to point",
      "polyline through points, _Close closes it (or _Enter to leave it open)",
      "rectangle on the XY plane from corner to opposite corner",
      "rectangle with rounded corners (last number = corner radius)",
      "circle: centre, radius",
      "arc: centre, start point, end point (counterclockwise)",
      "ellipse: centre, end of first axis, end of second axis",
      "regular polygon: centre, a corner",
      "smooth curve through the points, _Enter to finish",
      "helix: axis start, axis end, point at the radius",
      "box: base corner, opposite base corner (x,y), height",
      "sphere: centre, radius",
      "cylinder: base centre, radius, height (along +Z)",
      "cone: base centre, base radius, height (along +Z)",
      "truncated cone: base centre, base radius, height, top radius",
      "tube (hollow cylinder): base centre, outer radius, inner radius, height",
      "torus lying on the XY plane: centre, major radius, minor radius",
      "pyramid: base centre, base corner, height",
      "ellipsoid: centre, radius x, radius y, radius z",
      "wedge: base corner, opposite base corner, height",
      "text: insertion point, text, height",
  };
  const auto ex = ReferenceExamples();
  for (size_t i = 0; i < ex.size(); i++) o << "  " << ex[i] << "    (" << notes[i] << ")\n";
  o << R"(- Naming: add " => Name" (one word, no spaces) at the end of a creating command to name what it creates, e.g.
  _Box 0,0,0 100,50 30 => Body
- Commands that act on objects need a selection first:
  _SelNone                clear the selection
  _SelName Body Hole      select objects by name (in this order)
  _SelLast                select what the previous command created
  _SelAll                 select everything
  then one of:
  _BooleanUnion           join the selected solids into one
  _BooleanDifference      subtract: the FIRST selected object is kept, the others are cut away
  _BooleanIntersection    keep only the common part
  _FilletEdge 2           round all edges of the selected solid with radius 2
  _ChamferEdge 1          bevel all edges of the selected solid
  _Move 0,0,0 10,0,0      move from point to point
  _Copy 0,0,0 50,0,0 100,0,0 _Enter    copies (one per destination point)
  _Rotate 0,0,0 45        rotate around the vertical axis through the point (degrees)
  _Rotate3D 0,0,0 1,0,0 90             rotate around the axis from point to point
  _Scale 0,0,0 2          scale from the point
  _Mirror 0,0,0 0,10,0    mirror across the vertical plane through the two points (makes a copy)
  _ArrayPolar 0,0,0 6     6 copies around the vertical axis through the point
  _ArrayLinear 4 0,0,0 30,0,0          4 copies, spacing from the first to the second point
  _ExtrudeCrv _Solid=_Yes 20           extrude the selected closed curve into a solid 20 high
  _Revolve 0,0,0 0,0,1    revolve the selected curve 360 degrees around the axis
  _Loft                   surface through the selected curves (in order)
  _Pipe 2                 pipe of radius 2 around the selected curve
  _SetObjectColor 200,30,30            colour as r,g,b
  _Delete                 delete the selected objects
- Never use commands for files, undo, layers or settings.
- Build solids from simple primitives, then combine them with the boolean commands.
- Place parts so that solids to be joined overlap a little (at least 0.5 mm).
- Compute every coordinate from the requested size: each part must be at its final position.
  Never create several identical objects at the same place: _SelLast selects only the last one.
  Name each part (=> Name) and select parts by name, or create one part and _Copy it.
- Flat parts with a non-rectangular outline (rhombus, triangle, L shape, ...): draw the closed outline
  with _Polyline ... _Close at the right height, then _ExtrudeCrv _Solid=_Yes thickness. Then delete the curve.
- _Revolve is only for round objects (vases, bottles, wheels); never revolve a flat shape to make a plate.
- Use _BooleanUnion only on solids that really touch or overlap; if unsure, leave the parts separate.

Example: "un tavolo 120x80 alto 75 cm" (cm -> mm)
```kurva
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

Example: "una tazza"
```kurva
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

std::vector<std::string> ExpandScript(const std::vector<std::string>& lines, std::vector<int>* origin) {
  std::vector<std::string> out;
  if (origin) origin->clear();
  auto push = [&](const std::string& s, int k) { out.push_back(s); if (origin) origin->push_back(k); };
  for (int k = 0; k < (int)lines.size(); k++) {
    std::string l = lines[k];
    const size_t arrow = l.find("=>");
    if (arrow == std::string::npos) { push(l, k); continue; }
    std::string name = Trim(l.substr(arrow + 2));
    if (name.size() >= 2 && name.front() == '"' && name.back() == '"') name = name.substr(1, name.size() - 2);
    // un nome e' una parola sola (SelName separa i nomi con gli spazi)
    name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char c) { return std::isspace(c); }), name.end());
    push(Trim(l.substr(0, arrow)), k);
    if (name.empty()) continue;
    push("_SelNone", k);
    push("_SelLast", k);
    push("_SetObjectName \"" + name + "\"", k);
    push("_SelNone", k);
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

Validation Validate(Engine& e, const std::vector<std::string>& lines) {
  Validation v;
  if (lines.empty()) { v.error = "nessun comando nella risposta"; return v; }
  std::vector<int> origin;
  const auto expanded = ExpandScript(lines, &origin);
  Document3dm scratch;
  if (!scratch.FromBuffer(e.Document().ToBuffer())) { v.error = "copia del documento non riuscita"; return v; }
  std::ostringstream log;
  Engine se(scratch, log);
  const size_t before = se.AllObjectIds(true).size();
  for (size_t i = 0; i < expanded.size(); i++) {
    const std::string name = CommandName(expanded[i]);
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
  }
  v.created = (int)se.AllObjectIds(true).size() - (int)before;
  v.ok = true;
  return v;
}

std::string RepairRequest(const std::vector<std::string>& lines, const Validation& v) {
  std::ostringstream o;
  o << "Line " << (v.line + 1) << " of your script fails";
  if (v.line >= 0 && v.line < (int)lines.size()) o << " (" << lines[v.line] << ")";
  o << ": " << v.error << ". Answer again with the whole corrected script in a ```kurva block.";
  return o.str();
}

}  // namespace cad::ai
