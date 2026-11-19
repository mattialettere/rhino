// Comandi di trasformazione (menu Trasforma di Rhino 8).
// Agiscono sugli oggetti selezionati (SelAll, SelLast, SelCrv, ...).
#include "commands/Engine.h"
#include <cmath>
#include <memory>

namespace cad {
namespace {

bool LooksLikePoint(const std::string& t) {
  return t.find(',') != std::string::npos || t.find('<') != std::string::npos;
}

bool TransformInPlace(Engine& e, const std::vector<ON_UUID>& ids, const ON_Xform& x) {
  int n = 0;
  for (const auto& id : ids)
    if (ON_Geometry* g = e.EditableGeometry(id); g && g->Transform(x)) n++;
  e.Out() << "  Trasformati " << n << " oggetti\n";
  return n > 0 || e.Fail("nessun oggetto trasformato");
}

// Copia trasformata di ogni oggetto, con gli stessi attributi (id nuovo).
int CopyTransformed(Engine& e, const std::vector<ON_UUID>& ids, const ON_Xform& x) {
  int n = 0;
  for (const auto& id : ids) {
    const auto* o = e.Object(id);
    if (!o || !o->Geometry(nullptr)) continue;
    std::unique_ptr<ON_Geometry> g(static_cast<ON_Geometry*>(o->Geometry(nullptr)->Duplicate()));
    if (!g || !g->Transform(x)) continue;
    ON_3dmObjectAttributes attrs = *o->Attributes(&ON_3dmObjectAttributes::DefaultAttributes);
    if (!ON_UuidIsNil(e.AddObject(g.release(), &attrs))) n++;
  }
  return n;
}

ON_Xform Rotation(double degrees, const ON_3dPoint& center) {
  ON_Xform r;
  r.Rotation(degrees * ON_PI / 180.0, ON_3dVector::ZAxis, center);
  return r;
}

// Move: punto di partenza, punto di arrivo.
bool CmdMove(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("Move");
  if (sel.empty()) return false;
  auto a = e.ReadPoint(ts, "punto di partenza");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "punto di arrivo");
  if (!b) return false;
  return TransformInPlace(e, sel, ON_Xform::TranslationTransformation(*b - *a));
}

// Copy: punto di partenza, poi uno o piu' punti di arrivo fino a Invio.
bool CmdCopy(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("Copy");
  if (sel.empty()) return false;
  auto a = e.ReadPoint(ts, "punto di partenza");
  if (!a) return false;
  int n = 0, targets = 0;
  while (e.MoreInput(ts)) {
    ON_3dPoint base = *a;
    auto b = e.ReadPoint(ts, "punto di arrivo");
    if (!b) return false;
    n += CopyTransformed(e, sel, ON_Xform::TranslationTransformation(*b - base));
    targets++;
  }
  e.SkipEnter(ts);
  if (!targets) return e.Fail("manca punto di arrivo");
  e.Out() << "  Creati " << n << " oggetti copiati\n";
  return true;
}

// Rotate: centro, angolo in gradi (antiorario) oppure due punti di riferimento.
bool CmdRotate(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts, {"copy"});
  auto sel = e.RequireSelection("Rotate");
  if (sel.empty()) return false;
  auto c = e.ReadPoint(ts, "centro di rotazione");
  if (!c) return false;
  double deg;
  if (!ts.AtEnd() && LooksLikePoint(ts.Peek())) {
    auto p1 = e.ReadPoint(ts, "primo punto di riferimento"); if (!p1) return false;
    auto p2 = e.ReadPoint(ts, "secondo punto di riferimento"); if (!p2) return false;
    deg = (std::atan2(p2->y - c->y, p2->x - c->x) - std::atan2(p1->y - c->y, p1->x - c->x)) * 180.0 / ON_PI;
  } else {
    auto v = e.ReadNumber(ts, "angolo"); if (!v) return false;
    deg = *v;
  }
  if (opts.count("copy")) {
    int n = CopyTransformed(e, sel, Rotation(deg, *c));
    e.Out() << "  Creati " << n << " oggetti ruotati\n";
    return n > 0;
  }
  return TransformInPlace(e, sel, Rotation(deg, *c));
}

// Scale: punto base e fattore di scala (uniforme in 3D).
bool CmdScale(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("Scale");
  if (sel.empty()) return false;
  auto c = e.ReadPoint(ts, "punto base");
  if (!c) return false;
  auto f = e.ReadNumber(ts, "fattore di scala");
  if (!f) return false;
  if (std::fabs(*f) <= ON_ZERO_TOLERANCE) return e.Fail("fattore di scala nullo");
  return TransformInPlace(e, sel, ON_Xform::ScaleTransformation(*c, *f));
}

// Scale2D: come Scale ma solo in X e Y.
bool CmdScale2D(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("Scale2D");
  if (sel.empty()) return false;
  auto c = e.ReadPoint(ts, "punto base");
  if (!c) return false;
  auto f = e.ReadNumber(ts, "fattore di scala");
  if (!f) return false;
  return TransformInPlace(e, sel, ON_Xform::ScaleTransformation(*c, *f, *f, 1.0));
}

// Mirror: copia speculare rispetto alla linea (inizio, fine) nel piano XY.
bool CmdMirror(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("Mirror");
  if (sel.empty()) return false;
  auto a = e.ReadPoint(ts, "inizio del piano di specchiatura");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "fine del piano di specchiatura");
  if (!b) return false;
  ON_3dVector d = *b - *a;
  d.z = 0;
  if (d.Length() <= e.Tolerance()) return e.Fail("linea di specchiatura nulla");
  ON_3dVector normal = ON_CrossProduct(d, ON_3dVector::ZAxis);
  normal.Unitize();
  ON_Xform m = ON_Xform::MirrorTransformation(ON_Plane(*a, normal).plane_equation);
  int n = CopyTransformed(e, sel, m);
  e.Out() << "  Creati " << n << " oggetti specchiati\n";
  return n > 0 || e.Fail("nessun oggetto specchiato");
}

// ArrayLinear: numero di elementi (incluso l'originale), primo e secondo punto.
bool CmdArrayLinear(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("ArrayLinear");
  if (sel.empty()) return false;
  auto count = e.ReadInt(ts, "numero di elementi");
  if (!count) return false;
  if (*count < 2) return e.Fail("servono almeno 2 elementi");
  auto a = e.ReadPoint(ts, "primo punto di riferimento");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "secondo punto di riferimento");
  if (!b) return false;
  int n = 0;
  for (int i = 1; i < *count; i++)
    n += CopyTransformed(e, sel, ON_Xform::TranslationTransformation((*b - *a) * i));
  e.Out() << "  Creati " << n << " oggetti\n";
  return true;
}

// Array: numero in X, Y, Z e passo in X, Y, Z.
bool CmdArray(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("Array");
  if (sel.empty()) return false;
  auto nx = e.ReadInt(ts, "numero in X"); if (!nx) return false;
  auto ny = e.ReadInt(ts, "numero in Y"); if (!ny) return false;
  auto nz = e.ReadInt(ts, "numero in Z"); if (!nz) return false;
  auto sx = e.ReadNumber(ts, "passo in X"); if (!sx) return false;
  auto sy = e.ReadNumber(ts, "passo in Y"); if (!sy) return false;
  auto sz = e.ReadNumber(ts, "passo in Z"); if (!sz) return false;
  if (*nx < 1 || *ny < 1 || *nz < 1) return e.Fail("i numeri devono essere almeno 1");
  int n = 0;
  for (int k = 0; k < *nz; k++)
    for (int j = 0; j < *ny; j++)
      for (int i = 0; i < *nx; i++) {
        if (!i && !j && !k) continue;
        n += CopyTransformed(e, sel, ON_Xform::TranslationTransformation(ON_3dVector(i * *sx, j * *sy, k * *sz)));
      }
  e.Out() << "  Creati " << n << " oggetti\n";
  return true;
}

// ArrayPolar: centro, numero di elementi, angolo totale (predefinito 360).
bool CmdArrayPolar(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("ArrayPolar");
  if (sel.empty()) return false;
  auto c = e.ReadPoint(ts, "centro della serie polare");
  if (!c) return false;
  auto count = e.ReadInt(ts, "numero di elementi");
  if (!count) return false;
  if (*count < 2) return e.Fail("servono almeno 2 elementi");
  double total = 360.0;
  if (!ts.AtEnd() && !IsEnter(ts.Peek()) && !e.IsCommand(ts.Peek())) {
    auto v = e.ReadNumber(ts, "angolo totale"); if (!v) return false;
    total = *v;
  }
  // Come Rhino: con 360 gradi gli elementi non si sovrappongono all'ultimo passo.
  const double step = (std::fabs(std::fabs(total) - 360.0) < 1e-9) ? total / *count : total / (*count - 1);
  int n = 0;
  for (int i = 1; i < *count; i++) n += CopyTransformed(e, sel, Rotation(step * i, *c));
  e.Out() << "  Creati " << n << " oggetti\n";
  return true;
}

}  // namespace

void RegisterTransformCommands(Engine& e) {
  e.Register({"Move", "Trasforma", "Sposta (da, a)", CmdMove});
  e.Register({"Copy", "Trasforma", "Copia (da, a...)", CmdCopy});
  e.Register({"Rotate", "Trasforma", "Ruota (centro, angolo o due punti; Copy)", CmdRotate});
  e.Register({"Scale", "Trasforma", "Scala 3D (base, fattore)", CmdScale});
  e.Register({"Scale2D", "Trasforma", "Scala 2D (base, fattore)", CmdScale2D});
  e.Register({"Mirror", "Trasforma", "Specchia (inizio, fine)", CmdMirror});
  e.Register({"ArrayLinear", "Trasforma", "Serie lineare (numero, da, a)", CmdArrayLinear});
  e.Register({"Array", "Trasforma", "Serie rettangolare (nx ny nz, passi)", CmdArray});
  e.Register({"ArrayPolar", "Trasforma", "Serie polare (centro, numero, angolo)", CmdArrayPolar});
}

}  // namespace cad
