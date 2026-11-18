// Superficie limite di un SubD (Catmull-Clark con spigoli vivi), per la
// visualizzazione. openNURBS pubblico non genera i frammenti di mesh della
// superficie limite, quindi la gabbia si suddivide qui alcune volte e i
// vertici si portano sulla posizione limite.
#pragma once
#include "opennurbs.h"
#include <memory>
#include <vector>

namespace cad {

struct SubDLimitDisplay {
  std::unique_ptr<ON_Mesh> mesh;                  // superficie limite (quad)
  std::vector<std::vector<ON_3dPoint>> edges;     // spigoli del SubD sulla superficie limite
};

// levels = numero di suddivisioni (3 = 64 quad per faccia quadrata).
SubDLimitDisplay BuildSubDLimit(const ON_SubD& subd, int levels = 3);

}  // namespace cad
