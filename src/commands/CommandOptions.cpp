// Opzioni dei comandi mostrate sulla riga di comando (cliccabili, come in Rhino 8).
// Formato: "Nome=A|B|C" (scelta, il primo valore e' il predefinito) o "Nome=#valore" (numero).
// Le opzioni si scrivono subito dopo il nome del comando: "_ExtrudeCrv Solid=Yes".
#include "commands/Engine.h"

namespace cad {

void RegisterCommandOptions(Engine& e) {
  const std::vector<std::pair<const char*, std::vector<std::string>>> table = {
      {"ExtrudeCrv", {"Solid=No|Yes"}},
      {"Offset", {"Distance=#1"}},
      {"Rebuild", {"PointCount=#10", "Degree=3|1|2|5"}},
      {"Divide", {"Length=#10"}},
      {"Helix", {"Turns=#5"}},
      {"Match", {"Continuity=Tangency|Position|Curvature"}},
      {"MatchCrv", {"Continuity=Tangency|Position|Curvature"}},
      {"Loft", {"Style=Normal|Straight"}},
      {"Pipe", {"Cap=Flat|None"}},
      {"Shell", {"Thickness=#1"}},
      {"ArrayCrv", {"Orientation=Freeform|NoRotation"}},
      {"Rotate", {"Copy=No|Yes"}},
      {"Fillet", {"Radius=#1"}},
      {"Make2D", {"View=Current|Top|Front|Right|Back|Left|Bottom", "ShowHidden=No|Yes", "ShowTangents=No|Yes"}},
      {"Mesh", {"MaxDistance=#0.1"}},
      {"FilletEdge", {"EndRadius=#1"}},
      {"ChamferEdge", {"EndDistance=#1"}},
      {"MergeSrf", {"Smooth=Yes|No"}},
      {"SplitFace", {"Direction=U|V"}},
      {"ShrinkWrap", {"TargetEdgeLength=#", "Offset=#0", "SmoothingIterations=#3"}},
      {"OffsetMesh", {"Solid=No|Yes"}},
      {"MeshRepair", {"Repair=Yes|No"}},
      {"GCon", {"End1=Auto|Start|End", "End2=Auto|Start|End"}},
      {"EdgeContinuity", {"SamplePoints=#20", "Tolerance=#"}},
      {"PointDeviation", {"GoodPoint=#0.1", "BadPoint=#1", "IgnorePoint=#10"}},
      {"CurvatureAnalysis", {"Style=Gaussian|Mean|MinRadius|MaxRadius"}},
      {"Text", {"Height=#"}},
      {"Dim", {"Height=#"}},
      {"DimAligned", {"Height=#"}},
      {"DimArc", {"Height=#"}},
      {"DimCurveLength", {"Height=#"}},
  };
  for (const auto& [name, opts] : table) e.SetCommandOptions(name, opts);
}

}  // namespace cad
