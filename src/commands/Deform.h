// Deformazioni non lineari (Twist, Taper, Bend): la funzione si applica ai
// punti di controllo, dopo aver infittito le curve e le superfici perche' la
// forma segua la deformazione, come fa Rhino.
#pragma once
#include "opennurbs.h"
#include <functional>

namespace cad {

using PointMap = std::function<ON_3dPoint(const ON_3dPoint&)>;

// Nuova geometria deformata (nullptr se il tipo non e' supportato).
ON_Geometry* DeformGeometry(const ON_Geometry* g, const PointMap& f);

}  // namespace cad
