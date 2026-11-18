// Punti di controllo (PointsOn/PointsOff di Rhino 8): quali punti mostra un
// oggetto e come cambia la geometria quando se ne sposta uno.
#pragma once
#include "opennurbs.h"
#include <vector>

namespace cad {

struct Grip {
  ON_3dPoint p;
  int i = 0, j = -1;  // curva: indice del punto; superficie: (i,j); mesh/SubD: indice o id del vertice
};

struct GripSet {
  std::vector<Grip> grips;
  std::vector<std::vector<ON_3dPoint>> cage;  // poligono o rete di controllo
};

// false se l'oggetto non ha punti di controllo modificabili.
bool GetGrips(const ON_Geometry* g, GripSet& out);
bool SupportsGrips(const ON_Geometry* g);

// Nuova geometria con il punto (i,j) portato in p; nullptr se non riesce.
// Linee e polilinee restano tali, le altre curve diventano NURBS.
ON_Geometry* MoveGrip(const ON_Geometry* g, int i, int j, const ON_3dPoint& p);

}  // namespace cad
