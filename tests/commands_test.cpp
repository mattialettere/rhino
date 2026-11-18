// Test dei comandi: ogni caso esegue uno script e controlla il modello.
// Alla fine ogni modello viene salvato in formato Rhino 8, riletto e
// confrontato (nessuna differenza ammessa).
#include "commands/Annotation.h"
#include "ai/Assistant.h"
#include "commands/Engine.h"
#include "python/PythonHost.h"
#include "nodes/Graph.h"
#include "collab/Patch.h"
#include "commands/Grips.h"
#include "commands/Materials.h"
#include "commands/Exchange.h"
#include "print/Page.h"
#include "render/Render.h"
#include "ui/Scene.h"
#include "core/SubDLimit.h"
#include "core/ModelCompare.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>

using namespace cad;

static int g_failures = 0, g_checks = 0;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failures++; \
  std::printf("  FALLITO %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static bool Near(double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol; }
static bool NearPt(const ON_3dPoint& a, const ON_3dPoint& b, double tol = 1e-9) { return a.DistanceTo(b) <= tol; }

struct Ctx {
  Document3dm doc;
  std::ostringstream log;
  Engine e{doc, log};
  std::vector<const ON_Geometry*> Objects() {
    std::vector<const ON_Geometry*> out;
    for (auto& id : e.AllObjectIds(true)) out.push_back(e.Object(id)->Geometry(nullptr));
    return out;
  }
  const ON_Geometry* Last() { auto o = Objects(); return o.empty() ? nullptr : o.back(); }
  ON_BoundingBox BBox() {
    ON_BoundingBox b;
    for (auto* g : Objects()) { ON_BoundingBox t; g->GetTightBoundingBox(t); b.Union(t); }
    return b;
  }
};

static std::string g_outDir;

static double MeshVolume(const ON_Mesh& m) {
  double v = 0;
  for (int i = 0; i < m.FaceCount(); i++) {
    const ON_MeshFace& f = m.m_F[i];
    const ON_3dPoint a = m.Vertex(f.vi[0]);
    for (int k = 1; k + 1 < (f.IsQuad() ? 4 : 3); k++) {
      const ON_3dPoint b = m.Vertex(f.vi[k]), c = m.Vertex(f.vi[k + 1]);
      v += ON_DotProduct(ON_3dVector(a), ON_CrossProduct(ON_3dVector(b), ON_3dVector(c))) / 6.0;
    }
  }
  return std::fabs(v);
}

static double Len(const ON_Geometry* g) {
  const ON_Curve* c = ON_Curve::Cast(g);
  if (!c) return -1;
  double L = 0;
  const ON_Interval d = c->Domain();
  ON_3dPoint prev = c->PointAt(d[0]);
  for (int i = 1; i <= 20000; i++) {
    ON_3dPoint q = c->PointAt(d.ParameterAt(i / 20000.0));
    L += prev.DistanceTo(q);
    prev = q;
  }
  return L;
}
// Lunghezze di tutte le curve, ordinate
static std::vector<double> Lens(const std::vector<const ON_Geometry*>& objs) {
  std::vector<double> v;
  for (auto* g : objs) if (ON_Curve::Cast(g)) v.push_back(Len(g));
  std::sort(v.begin(), v.end());
  return v;
}

static void Case(const char* name, const std::string& script, const std::function<void(Ctx&)>& check,
                 bool expectOk = true) {
  std::printf("%s\n", name);
  Ctx c;
  const bool ok = c.e.Run(script);
  if (ok != expectOk) {
    g_failures++;
    std::printf("  FALLITO: esecuzione %s\n%s", ok ? "riuscita (attesa errore)" : "fallita", c.log.str().c_str());
    return;
  }
  check(c);
  // Ogni B-rep prodotto deve essere valido per openNURBS (come lo richiede Rhino).
  for (auto* g : c.Objects())
    if (auto* b = ON_Brep::Cast(g)) {
      ON_wString w; ON_TextLog tl(w);
      const bool valid = b->IsValid(&tl);
      if (!valid) std::printf("  B-rep non valido:\n%s\n", ToUtf8(w).substr(0, 800).c_str());
      CHECK(valid);
    }
  // Salvataggio Rhino 8 + rilettura + confronto
  std::string path = g_outDir + "/" + name + ".3dm";
  ModelSnapshot before = TakeSnapshot(c.doc.Model());
  CHECK(c.doc.Save(path));
  Document3dm back;
  CHECK(back.Load(path));
  CompareResult r = CompareSnapshots(before, TakeSnapshot(back.Model()));
  for (auto& d : r.differences) std::printf("  differenza dopo il salvataggio: %s\n", d.c_str());
  CHECK(r.Equal());
  CHECK(back.Summarize().rhinoVersion == 8);
}

int main(int argc, char** argv) {
  // senza buffer: se il processo si interrompe si vede l'ultimo gruppo di controlli
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::set_terminate([] {
    try {
      if (auto ex = std::current_exception()) std::rethrow_exception(ex);
      std::printf("terminate senza eccezione\n");
    } catch (const std::exception& x) {
      std::printf("eccezione non gestita: %s\n", x.what());
    } catch (...) {
      std::printf("eccezione non gestita\n");
    }
    std::fflush(stdout);
    std::_Exit(3);
  });
  g_outDir = argc > 1 ? argv[1] : "commands_out";
  std::filesystem::create_directories(g_outDir);
  InitLibrary();

  Case("Line", "_Line 0,0,0 10,0,0", [](Ctx& c) {
    auto* l = ON_LineCurve::Cast(c.Last());
    CHECK(l && Near(l->m_line.Length(), 10.0));
  });
  Case("Line_relative_polar", "_Line 1,1 r10<90", [](Ctx& c) {
    auto* l = ON_LineCurve::Cast(c.Last());
    CHECK(l && NearPt(l->m_line.to, ON_3dPoint(1, 11, 0), 1e-9));
  });
  Case("Polyline_close", "_Polyline 0,0 10,0 10,10 _Close", [](Ctx& c) {
    auto* p = ON_PolylineCurve::Cast(c.Last());
    CHECK(p && p->IsClosed() && p->PointCount() == 4);
  });
  Case("Rectangle", "_Rectangle 0,0 20,10", [](Ctx& c) {
    auto* p = ON_PolylineCurve::Cast(c.Last());
    CHECK(p && p->IsClosed() && p->PointCount() == 5);
    double len = p ? p->m_pline.Length() : 0;
    CHECK(Near(len, 60.0));
  });
  Case("Polygon", "_Polygon NumSides=6 0,0 10,0", [](Ctx& c) {
    auto* p = ON_PolylineCurve::Cast(c.Last());
    CHECK(p && p->PointCount() == 7 && p->IsClosed());
  });
  Case("Circle", "_Circle 0,0,0 5", [](Ctx& c) {
    auto* a = ON_ArcCurve::Cast(c.Last());
    CHECK(a && a->IsCircle() && Near(a->Radius(), 5.0));
  });
  Case("Circle_diameter_point", "_Circle _Diameter 0,0 10,0", [](Ctx& c) {
    auto* a = ON_ArcCurve::Cast(c.Last());
    CHECK(a && Near(a->Radius(), 5.0));
  });
  Case("Circle_3point", "_Circle _3Point 1,0 0,1 -1,0", [](Ctx& c) {
    auto* a = ON_ArcCurve::Cast(c.Last());
    CHECK(a && Near(a->Radius(), 1.0, 1e-12));
  });
  Case("Arc", "_Arc 0,0 10,0 0,10", [](Ctx& c) {
    auto* a = ON_ArcCurve::Cast(c.Last());
    double len = a ? a->m_arc.Length() : 0;
    CHECK(a && !a->IsCircle() && Near(len, 5.0 * ON_PI, 1e-9));
  });
  Case("Ellipse", "_Ellipse 0,0 10,0 0,4", [](Ctx& c) {
    auto* n = ON_NurbsCurve::Cast(c.Last());
    CHECK(n && n->IsClosed());
    ON_BoundingBox b = c.BBox();
    CHECK(NearPt(b.m_min, ON_3dPoint(-10, -4, 0), 1e-9) && NearPt(b.m_max, ON_3dPoint(10, 4, 0), 1e-9));
  });
  Case("Curve", "_Curve 0,0 5,5 10,0 15,5", [](Ctx& c) {
    auto* n = ON_NurbsCurve::Cast(c.Last());
    CHECK(n && n->Degree() == 3 && n->CVCount() == 4);
    CHECK(n && NearPt(n->PointAtStart(), ON_3dPoint(0, 0, 0)) && NearPt(n->PointAtEnd(), ON_3dPoint(15, 5, 0)));
  });
  Case("InterpCrv", "_InterpCrv 0,0 5,5 10,0 15,5 20,0", [](Ctx& c) {
    auto* n = ON_NurbsCurve::Cast(c.Last());
    CHECK(n && n->Degree() == 3 && n->CVCount() == 7);
    if (!n) return;
    ON_3dPoint pts[] = {ON_3dPoint(0, 0, 0), ON_3dPoint(5, 5, 0), ON_3dPoint(10, 0, 0), ON_3dPoint(15, 5, 0), ON_3dPoint(20, 0, 0)};
    // I nodi interni coincidono con i parametri dei punti.
    for (int i = 0; i < 5; i++) CHECK(NearPt(n->PointAt(n->m_knot[2 + i]), pts[i], 1e-9));
    ON_3dVector d2[3];
    ON_3dPoint p; ON_3dVector d1;
    n->Ev2Der(n->Domain()[0], p, d1, d2[0]);
    CHECK(d2[0].Length() < 1e-9);  // estremo naturale
  });
  Case("Points", "_Points 0,0 1,1 2,2 _Enter", [](Ctx& c) { CHECK(c.Objects().size() == 3); });

  Case("Box", "_Box 0,0,0 10,20 30", [](Ctx& c) {
    auto* x = ON_Extrusion::Cast(c.Last());
    CHECK(x && x->IsCapped());
    ON_BoundingBox b = c.BBox();
    CHECK(NearPt(b.m_min, ON_3dPoint(0, 0, 0)) && NearPt(b.m_max, ON_3dPoint(10, 20, 30)));
    if (x) { ON_Brep* br = x->BrepForm(); CHECK(br && br->IsSolid() && br->m_F.Count() == 6); delete br; }
  });
  Case("Sphere", "_Sphere 0,0,0 5", [](Ctx& c) {
    auto* b = ON_Brep::Cast(c.Last());
    CHECK(b && b->IsSolid());
    ON_BoundingBox bb = c.BBox();
    CHECK(NearPt(bb.m_max, ON_3dPoint(5, 5, 5), 1e-6));
  });
  Case("Cylinder", "_Cylinder 0,0,0 3 10", [](Ctx& c) {
    auto* x = ON_Extrusion::Cast(c.Last());
    CHECK(x && x->IsCapped());
    ON_BoundingBox bb = c.BBox();
    CHECK(NearPt(bb.m_min, ON_3dPoint(-3, -3, 0), 1e-6) && NearPt(bb.m_max, ON_3dPoint(3, 3, 10), 1e-6));
  });
  Case("Cone", "_Cone 0,0,0 4 8", [](Ctx& c) {
    auto* b = ON_Brep::Cast(c.Last());
    CHECK(b && b->IsSolid());
    ON_BoundingBox bb = c.BBox();
    CHECK(Near(bb.m_min.z, 0, 1e-6) && Near(bb.m_max.z, 8, 1e-6) && Near(bb.m_max.x, 4, 1e-6));
  });
  Case("Torus", "_Torus 0,0,0 10 2", [](Ctx& c) {
    auto* b = ON_Brep::Cast(c.Last());
    CHECK(b && b->IsSolid());
    ON_BoundingBox bb = c.BBox();
    CHECK(Near(bb.m_max.x, 12, 1e-6) && Near(bb.m_max.z, 2, 1e-6));
  });
  Case("Plane", "_Plane 0,0 10,5", [](Ctx& c) {
    auto* b = ON_Brep::Cast(c.Last());
    CHECK(b && b->m_F.Count() == 1 && !b->IsSolid());
  });
  Case("PlanarSrf", "_Circle 0,0 5\n_SelCrv _PlanarSrf", [](Ctx& c) {
    CHECK(c.Objects().size() == 2);
    CHECK(ON_Brep::Cast(c.Last()) != nullptr);
  });
  Case("ExtrudeCrv_solid", "_Circle 0,0 5\n_SelCrv _ExtrudeCrv _Solid=_Yes 10", [](Ctx& c) {
    auto* x = ON_Extrusion::Cast(c.Last());
    CHECK(x && x->IsCapped());
  });
  Case("ExtrudeCrv_open", "_Line 0,0 10,0\n_SelCrv _ExtrudeCrv 5", [](Ctx& c) {
    auto* x = ON_Extrusion::Cast(c.Last());
    CHECK(x && !x->IsCapped());
  });
  Case("Revolve", "_Line 5,0,0 5,0,10\n_SelCrv _Revolve 0,0,0 0,0,1", [](Ctx& c) {
    auto* b = ON_Brep::Cast(c.Last());
    CHECK(b && b->m_F.Count() == 1);
    ON_BoundingBox bb; if (b) b->GetTightBoundingBox(bb);
    CHECK(Near(bb.m_min.x, -5, 1e-6) && Near(bb.m_max.y, 5, 1e-6));
  });
  Case("SubDBox", "_SubDBox 0,0,0 10,10 10", [](Ctx& c) {
    auto* s = ON_SubD::Cast(c.Last());
    CHECK(s && s->FaceCount() == 6);
  });

  Case("Move", "_Line 0,0 10,0\n_SelAll _Move 0,0 0,5", [](Ctx& c) {
    auto* l = ON_LineCurve::Cast(c.Last());
    CHECK(l && NearPt(l->m_line.from, ON_3dPoint(0, 5, 0)));
  });
  Case("Copy", "_Point 0,0\n_SelAll _Copy 0,0 1,0 2,0 _Enter", [](Ctx& c) { CHECK(c.Objects().size() == 3); });
  Case("Rotate", "_Line 0,0 10,0\n_SelAll _Rotate 0,0 90", [](Ctx& c) {
    auto* l = ON_LineCurve::Cast(c.Last());
    CHECK(l && NearPt(l->m_line.to, ON_3dPoint(0, 10, 0), 1e-9));
  });
  Case("Scale", "_Line 1,0 2,0\n_SelAll _Scale 0,0 3", [](Ctx& c) {
    auto* l = ON_LineCurve::Cast(c.Last());
    CHECK(l && NearPt(l->m_line.to, ON_3dPoint(6, 0, 0)));
  });
  Case("Mirror", "_Point 2,3\n_SelAll _Mirror 0,0 0,10", [](Ctx& c) {
    auto o = c.Objects();
    CHECK(o.size() == 2 && NearPt(ON_Point::Cast(o.back())->point, ON_3dPoint(-2, 3, 0)));
  });
  Case("ArrayLinear", "_Point 0,0\n_SelAll _ArrayLinear 5 0,0 2,0", [](Ctx& c) {
    CHECK(c.Objects().size() == 5);
    CHECK(Near(c.BBox().m_max.x, 8.0));
  });
  Case("Array", "_Point 0,0\n_SelAll _Array 3 2 1 5 5 5", [](Ctx& c) { CHECK(c.Objects().size() == 6); });
  Case("ArrayPolar", "_Point 10,0\n_SelAll _ArrayPolar 0,0 4", [](Ctx& c) {
    CHECK(c.Objects().size() == 4);
    CHECK(NearPt(ON_Point::Cast(c.Objects()[2])->point, ON_3dPoint(-10, 0, 0), 1e-9));
  });

  Case("Join", "_Line 0,0 10,0\n_Line 10,0 10,10\n_Line 10,10 0,10\n_SelCrv _Join", [](Ctx& c) {
    CHECK(c.Objects().size() == 1);
    auto* pc = ON_PolyCurve::Cast(c.Last());
    CHECK(pc && pc->Count() == 3);
  });
  Case("Explode_polyline", "_Rectangle 0,0 10,10\n_SelCrv _Explode", [](Ctx& c) {
    CHECK(c.Objects().size() == 4);
  });
  Case("Explode_box", "_Box 0,0,0 10,10 10\n_SelAll _Explode", [](Ctx& c) {
    CHECK(c.Objects().size() == 6);
  });
  Case("Delete_Undo_Redo", "_Line 0,0 1,0\n_SelAll _Delete\n_Undo", [](Ctx& c) {
    CHECK(c.Objects().size() == 1);
    CHECK(c.e.Run("_Redo"));
    CHECK(c.Objects().empty());
  });
  Case("Flip", "_Line 0,0 10,0\n_SelAll _Flip", [](Ctx& c) {
    auto* l = ON_LineCurve::Cast(c.Last());
    CHECK(l && NearPt(l->PointAtStart(), ON_3dPoint(10, 0, 0)));
  });
  Case("Hide_Show", "_Point 0,0\n_Point 1,1\n_SelLast _Hide", [](Ctx& c) {
    CHECK(c.e.AllObjectIds(false).size() == 1);
    CHECK(c.e.Run("_Show"));
    CHECK(c.e.AllObjectIds(false).size() == 2);
  });
  Case("Layers", "-_Layer _New Muri\n-_Layer _Current Muri\n_Line 0,0 5,0\n_SelNone\n_SelLayer Muri\n_What", [](Ctx& c) {
    CHECK(c.e.Selection().size() == 1);
    CHECK(c.log.str().find("livello: Muri") != std::string::npos);
  });
  Case("ChangeLayer", "-_Layer _New A\n_Point 0,0\n_SelAll _ChangeLayer A\n_SelNone _SelLayer A", [](Ctx& c) {
    CHECK(c.e.Selection().size() == 1);
  });
#ifdef CAD_WITH_OCCT
  auto number = [](Ctx& c, const char* label) {
    const std::string log = c.log.str();
    auto p = log.rfind(label);
    return p == std::string::npos ? -1.0 : std::atof(log.c_str() + p + std::strlen(label));
  };
  auto isSolid = [](const ON_Geometry* g) { auto* b = ON_Brep::Cast(g); return b && b->IsSolid(); };
  Case("BooleanUnion", "_Box 0,0,0 10,10 10\n_Box 5,5,5 15,15 10\n_SelAll _BooleanUnion\n_SelAll _Volume", [&](Ctx& c) {
    CHECK(c.Objects().size() == 1 && isSolid(c.Last()));
    CHECK(Near(number(c, "Volume = "), 2000.0 - 125.0, 1e-6));
  });
  Case("BooleanDifference", "_Box 0,0,0 10,10 10\n_Cylinder 5,5,-1 2 12\n_SelAll _BooleanDifference\n_SelAll _Volume", [&](Ctx& c) {
    CHECK(c.Objects().size() == 1 && isSolid(c.Last()));
    CHECK(Near(number(c, "Volume = "), 1000.0 - ON_PI * 4.0 * 10.0, 1e-4));
  });
  Case("BooleanIntersection", "_Box 0,0,0 10,10 10\n_Box 5,5,5 15,15 10\n_SelAll _BooleanIntersection\n_SelAll _Volume", [&](Ctx& c) {
    CHECK(Near(number(c, "Volume = "), 125.0, 1e-6));
  });
  Case("FilletEdge", "_Box 0,0,0 10,10 10\n_SelAll _FilletEdge 1\n_SelAll _Volume", [&](Ctx& c) {
    CHECK(isSolid(c.Last()));
    // cubo con spigoli raccordati: 1000 - 12*(1 - pi/4)*10 ... meno gli angoli
    // 1000 - 12 spigoli * 8 * (1 - pi/4) - 8 angoli * (1 - pi/6)
    CHECK(Near(number(c, "Volume = "), 1000.0 - 96.0 * (1.0 - ON_PI / 4.0) - 8.0 * (1.0 - ON_PI / 6.0), 1e-4));
  });
  Case("ChamferEdge", "_Box 0,0,0 10,10 10\n_SelAll _ChamferEdge 1\n_SelAll _Volume", [&](Ctx& c) {
    CHECK(isSolid(c.Last()));
    // 1000 - 12 spigoli * 8 * 1/2 - 8 angoli * 5/6
    CHECK(Near(number(c, "Volume = "), 1000.0 - 48.0 - 8.0 * 5.0 / 6.0, 1e-6));
  });
  Case("Offset_circle_out", "_Circle 0,0 5\n_SelAll _Offset 2 20,0", [](Ctx& c) {
    CHECK(c.Objects().size() == 2);
    ON_BoundingBox b; c.Last()->GetTightBoundingBox(b);
    CHECK(Near(b.m_max.x, 7.0, 1e-6));
  });
  Case("Offset_circle_in", "_Circle 0,0 5\n_SelAll _Offset 2 0,0", [](Ctx& c) {
    ON_BoundingBox b; c.Last()->GetTightBoundingBox(b);
    CHECK(Near(b.m_max.x, 3.0, 1e-6));
  });
  Case("Offset_rectangle", "_Rectangle 0,0 10,10\n_SelAll _Offset 1 20,20", [](Ctx& c) {
    ON_BoundingBox b; c.Last()->GetTightBoundingBox(b);
    CHECK(Near(b.m_max.x, 11.0, 1e-6) && Near(b.m_min.y, -1.0, 1e-6));
  });
  Case("Loft", "_Circle 0,0,0 5\n_Circle 0,0,10 3\n_SelCrv _Loft", [](Ctx& c) {
    auto* b = ON_Brep::Cast(c.Last());
    CHECK(b && b->m_F.Count() == 1);
  });
  Case("Pipe", "_Line 0,0,0 0,0,10\n_SelAll _Pipe 1\n_SelPolysrf _Volume", [&](Ctx& c) {
    CHECK(isSolid(c.Last()));
    CHECK(Near(number(c, "Volume = "), ON_PI * 10.0, 1e-3));
  });
  Case("Sweep1", "_Line 0,0,0 0,20,0\n_Circle 0,0,0 3\n_SelCrv _Sweep1", [](Ctx& c) {
    CHECK(ON_Brep::Cast(c.Last()) != nullptr);
  });
  Case("Intersect", "_Sphere 0,0,0 5\n_Plane -10,-10,0 10,10\n_SelAll _Intersect", [](Ctx& c) {
    auto* cc = ON_Curve::Cast(c.Last());
    CHECK(cc && cc->IsClosed());
    ON_BoundingBox b; if (cc) cc->GetTightBoundingBox(b);
    CHECK(Near(b.m_max.x, 5.0, 1e-3));
  });
  Case("Split", "_Box 0,0,0 10,10 10\n_Plane -5,-5,5 15,15\n_SelPolysrf _SelSrf _Split", [](Ctx& c) {
    int solids = 0;
    for (auto* g : c.Objects()) if (auto* b = ON_Brep::Cast(g); b && b->IsSolid()) solids++;
    CHECK(solids == 2);
  });
  Case("Mesh", "_Sphere 0,0,0 5\n_SelAll _Mesh", [](Ctx& c) {
    auto* m = ON_Mesh::Cast(c.Last());
    CHECK(m && m->FaceCount() > 50 && m->IsClosed());
  });
  Case("Area_sphere", "_Sphere 0,0,0 5\n_SelAll _Area", [&](Ctx& c) {
    CHECK(Near(number(c, "Area = "), 4.0 * ON_PI * 25.0, 1e-3));
  });
  Case("Volume_torus", "_Torus 0,0,0 10 2\n_SelAll _Volume", [&](Ctx& c) {
    CHECK(Near(number(c, "Volume = "), 2.0 * ON_PI * ON_PI * 10.0 * 4.0, 1e-2));
  });
  // --- Trim, Extend, Fillet, Chamfer, Cap, Shell, BooleanSplit ---
  Case("Trim_line", "_Line 0,0 10,0\n_Line 5,-5 5,5\n_SelLast _Trim 8,0", [](Ctx& c) {
    auto L = Lens(c.Objects());
    CHECK(L.size() == 2 && Near(L[0], 5.0, 1e-6) && Near(L[1], 10.0, 1e-6));
    CHECK(ON_LineCurve::Cast(c.Objects()[0]) || ON_LineCurve::Cast(c.Objects()[1]));
  });
  Case("Trim_middle", "_Line 0,0 10,0\n_Line 3,-5 3,5\n_Line 7,-5 7,5\n_SelNone _SelAll _Trim 5,0", [](Ctx& c) {
    auto L = Lens(c.Objects());
    CHECK(L.size() == 4 && Near(L[0], 3.0, 1e-6) && Near(L[1], 3.0, 1e-6));
  });
  Case("Trim_circle_seam", "_Circle 0,0 5\n_Line -10,0 10,0\n_SelLast _Trim 0,-5", [](Ctx& c) {
    const ON_ArcCurve* arc = nullptr;
    for (auto* g : c.Objects()) if (auto* a = ON_ArcCurve::Cast(g)) arc = a;
    CHECK(arc && Near(Len(arc), 5.0 * ON_PI, 1e-4));
    if (arc) CHECK(arc->PointAt(arc->Domain().Mid()).y > 4.9);
  });
  Case("Trim_circle_inner", "_Circle 0,0 5\n_Line -10,3 10,3\n_Line -10,-3 10,-3\n_SelCrv _Trim 5,0", [](Ctx& c) {
    // tolto l'arco destro tra le due linee: resta un pezzo unico che passa a sinistra
    int arcs = 0; double len = 0;
    for (auto* g : c.Objects()) if (!ON_LineCurve::Cast(g) && ON_Curve::Cast(g)) { arcs++; len = Len(g); }
    CHECK(arcs == 1 && Near(len, 10.0 * ON_PI - 10.0 * std::atan2(3.0, 4.0), 1e-4));
  });
  Case("Trim_by_surface", "_Line 0,0,-5 0,0,5\n_Plane -5,-5 5,5\n_SelSrf _Trim 0,0,3", [](Ctx& c) {
    auto L = Lens(c.Objects());
    CHECK(L.size() == 1 && Near(L[0], 5.0, 1e-6));
  });
  Case("Extend_line", "_Line 0,0 5,0\n_Line 10,-5 10,5\n_SelLast _Extend 4,0", [](Ctx& c) {
    auto L = Lens(c.Objects());
    CHECK(L.size() == 2 && Near(L[0], 10.0, 1e-6) && Near(L[1], 10.0, 1e-6));
  });
  Case("Extend_start", "_Line 5,0 10,0\n_Line 0,-5 0,5\n_SelLast _Extend 6,0", [](Ctx& c) {
    auto L = Lens(c.Objects());
    CHECK(L.size() == 2 && Near(L[0], 10.0, 1e-6) && Near(L[1], 10.0, 1e-6));
  });
  Case("Extend_arc", "_Arc 0,0 10,0 0,10\n_Line -5,0 -5,20\n_SelLast _Extend 1,10", [](Ctx& c) {
    const ON_ArcCurve* arc = nullptr;
    for (auto* g : c.Objects()) if (auto* a = ON_ArcCurve::Cast(g)) arc = a;
    CHECK(arc && Near(Len(arc), 10.0 * 2.0 * ON_PI / 3.0, 1e-4));
  });
  Case("Fillet_lines", "_Line 0,0 10,0\n_Line 0,0 0,10\n_Fillet Radius=2 8,0 0,8", [](Ctx& c) {
    auto L = Lens(c.Objects());
    CHECK(L.size() == 3 && Near(L[0], ON_PI, 1e-4) && Near(L[1], 8.0, 1e-6) && Near(L[2], 8.0, 1e-6));
    int arcs = 0;
    for (auto* g : c.Objects()) if (auto* a = ON_ArcCurve::Cast(g)) { arcs++; CHECK(Near(a->m_arc.Radius(), 2.0, 1e-9)); }
    CHECK(arcs == 1);
  });
  Case("Fillet_radius0_extends", "_Line 0,0 10,0\n_Line 12,2 12,10\n_Fillet Radius=0 5,0 12,5", [](Ctx& c) {
    auto L = Lens(c.Objects());
    CHECK(L.size() == 2 && Near(L[0], 10.0, 1e-6) && Near(L[1], 12.0, 1e-6));
  });
  Case("Chamfer_lines", "_Line 0,0 10,0\n_Line 0,0 0,10\n_Chamfer Distances=2,3 8,0 0,8", [](Ctx& c) {
    auto L = Lens(c.Objects());
    CHECK(L.size() == 3 && Near(L[0], std::sqrt(13.0), 1e-6) && Near(L[1], 7.0, 1e-6) && Near(L[2], 8.0, 1e-6));
  });
  Case("Cap", "_Circle 0,0 5\n_SelCrv _ExtrudeCrv 10\n_SelNone _SelSrf _SelPolysrf _Cap\n_SelAll _Volume", [&](Ctx& c) {
    CHECK(isSolid(c.Last()));
    CHECK(Near(number(c, "Volume = "), ON_PI * 25.0 * 10.0, 1e-3));
  });
  Case("Shell", "_Box 0,0,0 10,10 10\n_SelAll _Shell Thickness=1 5,5,10 _Enter\n_SelAll _Volume", [&](Ctx& c) {
    CHECK(c.Objects().size() == 1 && isSolid(c.Last()));
    CHECK(Near(number(c, "Volume = "), 1000.0 - 8.0 * 8.0 * 9.0, 1e-4));
  });
  Case("BooleanSplit", "_Box 0,0,0 10,10 10\n_Box 5,5,5 15,15 10\n_SelAll _BooleanSplit\n_SelNone _SelPolysrf _Volume", [&](Ctx& c) {
    int solids = 0;
    for (auto* g : c.Objects()) if (isSolid(g)) solids++;
    CHECK(solids == 2 && c.Objects().size() == 3);  // due pezzi B-rep del primo box + il box di taglio (estrusione)
    CHECK(Near(number(c, "Volume = "), 1000.0 + 1000.0, 1e-4));
  });
  Case("DupBorder", "_Plane 0,0 10,5\n_SelSrf _DupBorder", [](Ctx& c) {
    auto L = Lens(c.Objects());
    if (L.size() != 1 || !Near(L[0], 30.0, 1e-2)) std::printf("%s n=%zu L=%g\n", c.log.str().c_str(), L.size(), L.empty() ? 0.0 : L[0]);
    CHECK(L.size() == 1 && Near(L[0], 30.0, 1e-2));  // campionamento: gli angoli vengono smussati
  });
  Case("FilletEdge_picked", "_Box 0,0,0 10,10 10\n_SelAll _FilletEdge 1 10,5,10 _Enter\n_SelAll _Volume", [&](Ctx& c) {
    CHECK(isSolid(c.Last()));
    // un solo spigolo raccordato: 1000 - 10*(1 - pi/4)
    CHECK(Near(number(c, "Volume = "), 1000.0 - 10.0 * (1.0 - ON_PI / 4.0), 1e-4));
  });
  Case("ChamferEdge_two", "_Box 0,0,0 10,10 10\n_SelAll _ChamferEdge 2 10,5,10 0,5,10 _Enter\n_SelAll _Volume", [&](Ctx& c) {
    CHECK(Near(number(c, "Volume = "), 1000.0 - 2 * 10.0 * 2.0, 1e-4));
  });
  Case("ExtractSrf_brep", "_Box 0,0,0 10,10 10\n_Box 5,5,5 15,15 10\n_SelAll _BooleanUnion\n_ExtractSrf 7,7,15 _Enter", [](Ctx& c) {
    int breps = 0, single = 0;
    for (auto* g : c.Objects()) if (auto* b = ON_Brep::Cast(g)) { breps++; if (b->m_F.Count() == 1) single++; }
    CHECK(breps == 2 && single == 1);
  });
  Case("DupEdge", "_Box 0,0,0 10,10 10\n_DupEdge 5,0,0 _Enter", [](Ctx& c) {
    auto L = Lens(c.Objects());
    CHECK(L.size() == 1 && Near(L[0], 10.0, 1e-6));
  });
  Case("ExtrudeSrf", "_Plane 0,0 10,5\n_SelAll _ExtrudeSrf 4\n_SelPolysrf _Volume", [&](Ctx& c) {
    CHECK(Near(number(c, "Volume = "), 200.0, 1e-6));
  });
  Case("Contour", "_Sphere 0,0,0 5\n_SelAll _Contour 0,0,0 0,0,1 2", [](Ctx& c) {
    int crv = 0;
    for (auto* g : c.Objects()) if (ON_Curve::Cast(g)) crv++;
    CHECK(crv == 5);  // z = -4, -2, 0, 2, 4
  });
  Case("Section", "_Box 0,0,0 10,10 10\n_SelAll _Section 5,-5 5,15", [](Ctx& c) {
    auto L = Lens(c.Objects());
    CHECK(L.size() == 1 && Near(L[0], 40.0, 1e-2));
  });
  Case("Project", "_Circle 5,5,20 2\n_Box 0,0,0 10,10 10\n_SelAll _Project", [](Ctx& c) {
    int onTop = 0;
    for (auto* g : c.Objects()) if (auto* cv = ON_Curve::Cast(g)) { ON_BoundingBox bb; cv->GetTightBoundingBox(bb); if (Near(bb.m_min.z, 10.0, 1e-3)) onTop++; }
    CHECK(onTop >= 1);
  });
#endif



  // --- Analisi, curve, gruppi, blocchi ---
  {
    auto num = [](Ctx& c, const char* label) {
      const std::string log = c.log.str();
      auto p = log.rfind(label);
      return p == std::string::npos ? -1.0 : std::atof(log.c_str() + p + std::strlen(label));
    };
    Case("Length_circle", "_Circle 0,0 5\n_SelAll _Length", [&](Ctx& c) { CHECK(Near(num(c, "Lunghezza = "), 10 * ON_PI, 1e-6)); });
    Case("Length_interp", "_InterpCrv 0,0 5,5 10,0\n_SelAll _Length", [&](Ctx& c) {
      CHECK(Near(num(c, "Lunghezza = "), Len(c.Last()), 1e-3));
    });
    Case("Distance", "_Distance 0,0 3,4", [&](Ctx& c) { CHECK(Near(num(c, "Distanza = "), 5.0, 1e-12)); });
    Case("Angle", "_Angle 0,0 10,0 0,10", [&](Ctx& c) { CHECK(Near(num(c, "Angolo = "), 90.0, 1e-9)); });
    Case("Radius", "_Circle 0,0 7\n_SelAll _Radius", [&](Ctx& c) { CHECK(Near(num(c, "Raggio = "), 7.0, 1e-12)); });
    Case("BoundingBox", "_Sphere 0,0,0 5\n_SelAll _BoundingBox", [&](Ctx& c) {
      auto* b = ON_Brep::Cast(c.Last());
      CHECK(b && b->IsSolid() && b->m_F.Count() == 6);
      ON_BoundingBox bb; if (b) b->GetTightBoundingBox(bb);
      CHECK(Near(bb.Diagonal().x, 10.0, 1e-9) && Near(bb.Diagonal().z, 10.0, 1e-9));
    });
    Case("Divide_line", "_Line 0,0 10,0\n_SelAll _Divide 4", [](Ctx& c) {
      int pts = 0; bool ok = true;
      for (auto* g : c.Objects()) if (auto* p = ON_Point::Cast(g)) { pts++; ok = ok && Near(p->point.x / 2.5, std::round(p->point.x / 2.5), 1e-6); }
      CHECK(pts == 5 && ok);
    });
    Case("Divide_circle", "_Circle 0,0 5\n_SelAll _Divide 4", [](Ctx& c) {
      int pts = 0;
      for (auto* g : c.Objects()) if (ON_Point::Cast(g)) pts++;
      CHECK(pts == 4);
    });
    Case("Divide_length", "_Line 0,0 10,0\n_SelAll _Divide Length=3", [](Ctx& c) {
      int pts = 0;
      for (auto* g : c.Objects()) if (ON_Point::Cast(g)) pts++;
      CHECK(pts == 4);
    });
    Case("Rebuild", "_InterpCrv 0,0 5,5 10,0 15,5 20,0\n_SelAll _Rebuild PointCount=8 Degree=3", [](Ctx& c) {
      auto* n = ON_NurbsCurve::Cast(c.Last());
      CHECK(n && n->CVCount() == 8 && n->Degree() == 3);
      if (n) { CHECK(NearPt(n->PointAtStart(), ON_3dPoint(0, 0, 0), 1e-9)); CHECK(NearPt(n->PointAtEnd(), ON_3dPoint(20, 0, 0), 1e-9)); }
      if (n) CHECK(std::fabs(n->PointAt(n->Domain().Mid()).y) < 6.0);
    });
    Case("ChangeDegree_up", "_Line 0,0 10,0\n_SelAll _ChangeDegree 3", [](Ctx& c) {
      auto* n = ON_NurbsCurve::Cast(c.Last());
      CHECK(n && n->Degree() == 3 && Near(Len(n), 10.0, 1e-6));
    });
    Case("ProjectToCPlane", "_Line 0,0,5 10,0,7\n_SelAll _ProjectToCPlane", [](Ctx& c) {
      auto* l = ON_LineCurve::Cast(c.Last());
      CHECK(l && Near(l->m_line.from.z, 0) && Near(l->m_line.to.z, 0) && Near(l->m_line.Length(), 10.0));
    });
    Case("Helix", "_Helix Turns=5 0,0,0 0,0,10 3,0,0", [](Ctx& c) {
      auto* n = ON_NurbsCurve::Cast(c.Last());
      CHECK(n != nullptr);
      if (n) {
        CHECK(NearPt(n->PointAtStart(), ON_3dPoint(3, 0, 0), 1e-9));
        CHECK(NearPt(n->PointAtEnd(), ON_3dPoint(3, 0, 10), 1e-9));
        ON_BoundingBox bb; n->GetTightBoundingBox(bb);
        CHECK(Near(bb.m_max.x, 3.0, 0.05) && Near(bb.m_min.y, -3.0, 0.05));
      }
    });
    Case("Spiral_flat", "_Spiral Flat Turns=3 0,0 1,0 5", [](Ctx& c) {
      auto* n = ON_NurbsCurve::Cast(c.Last());
      CHECK(n && NearPt(n->PointAtStart(), ON_3dPoint(1, 0, 0), 1e-9) && NearPt(n->PointAtEnd(), ON_3dPoint(5, 0, 0), 1e-9));
    });
    Case("Group", "_Line 0,0 1,0\n_Line 0,1 1,1\n_SelAll _Group", [](Ctx& c) {
      int g = 0;
      for (auto& id : c.e.AllObjectIds()) g += c.e.Object(id)->Attributes(nullptr)->GroupCount();
      CHECK(g == 2);
      CHECK(c.doc.Model().ActiveComponentCount(ON_ModelComponent::Type::Group) == 1);
    });
    Case("Block_Insert", "_Box 0,0,0 2,2 2\n_Circle 1,1 3\n_SelAll _Block Name=Pezzo 1,1,0\n_Insert Pezzo 20,0,0 2 90", [](Ctx& c) {
      int refs = 0;
      for (auto* g : c.Objects()) if (ON_InstanceRef::Cast(g)) refs++;
      CHECK(refs == 2 && c.Objects().size() == 2);
      CHECK(c.doc.Model().ActiveComponentCount(ON_ModelComponent::Type::InstanceDefinition) == 1);
      ON_BoundingBox bb = c.BBox();
      CHECK(Near(bb.m_max.x, 26.0, 1e-6));  // seconda istanza: cerchio raggio 3*2=6 attorno a x=20
    });
    Case("ReplaceBlock", "_Box 0,0,0 2,2 2\n_SelAll _Block Name=A 0,0,0\n_SelNone\n_Sphere 10,0,0 1\n_SelLast _Block Name=B 10,0,0\n"
                         "_SelNone\n_Insert A 20,0,0\n_SelLast _ReplaceBlock B", [](Ctx& c) {
      int toB = 0;
      const auto* b = ON_InstanceDefinition::Cast(c.doc.Model().ComponentFromName(ON_ModelComponent::Type::InstanceDefinition, ON_nil_uuid, L"B").ModelComponent());
      for (auto* g : c.Objects()) if (const auto* ir = ON_InstanceRef::Cast(g)) if (b && ir->m_instance_definition_uuid == b->Id()) toB++;
      CHECK(toB == 2);
    });
    Case("RenameBlock", "_Box 0,0,0 2,2 2\n_SelAll _Block Name=Vecchio 0,0,0\n_RenameBlock Vecchio Nuovo\n_Insert Nuovo 10,0,0", [](Ctx& c) {
      int refs = 0;
      for (auto* g : c.Objects()) if (ON_InstanceRef::Cast(g)) refs++;
      CHECK(refs == 2 && c.doc.Model().ActiveComponentCount(ON_ModelComponent::Type::InstanceDefinition) == 1);
    });
    Case("ExplodeBlock", "_Box 0,0,0 2,2 2\n_SelAll _Block 0,0,0\n_SelAll _ExplodeBlock", [](Ctx& c) {
      CHECK(c.Objects().size() == 1 && ON_Extrusion::Cast(c.Last()));
    });
  }

  // Annotazioni: oggetti openNURBS, misure, salvataggio e geometria di visualizzazione
  {
    std::printf("Annotazioni\n");
    auto display = [](Ctx& c, const ON_Geometry* g) {
      AnnotationDisplay d;
      const auto* a = ON_Annotation::Cast(g);
      CHECK(a && BuildAnnotationDisplay(c.doc.Model(), *a, d));
      return d;
    };
    Case("Text", "_Text 5,5 \"Pianta piano terra\" Height=3", [&](Ctx& c) {
      const auto* t = ON_Text::Cast(c.Last());
      CHECK(t && ToUtf8(t->PlainText()) == "Pianta piano terra");
      CHECK(t && Near(t->TextHeight(&ON_DimStyle::Default), 3.0));
      auto d = display(c, c.Last());
      CHECK(d.labels.size() == 1 && d.labels[0].text == "Pianta piano terra");
      // testo allineato in alto a sinistra: linea di base un'altezza sotto il punto
      // (su Windows openNURBS misura i caratteri: il punto e' il centro del riquadro, quindi x varia)
      CHECK(!d.labels.empty() && Near(d.labels[0].origin.y, 2.0, 0.6) && Near(d.labels[0].height, 3.0));
      CHECK(!d.labels.empty() && d.labels[0].origin.x >= 5.0 - 1e-9);
    });
    Case("Text_multiriga", "_Text 0,0 \"riga uno\\nriga due\"", [](Ctx& c) {
      const auto* t = ON_Text::Cast(c.Last());
      CHECK(t && ToUtf8(t->PlainText()).find('\n') != std::string::npos);
    });
    Case("Dim_orizzontale", "_Dim 0,0 40,5 20,-8", [&](Ctx& c) {
      const auto* d = ON_DimLinear::Cast(c.Last());
      CHECK(d && Near(d->Measurement(), 40.0, 1e-9));
      auto v = display(c, c.Last());
      CHECK(v.labels.size() == 1 && v.labels[0].text == "40.00" && v.wires.size() == 5);  // 2 estensioni, linea, 2 frecce
      CHECK(!v.labels.empty() && Near(v.labels[0].xdir.x, 1.0, 1e-9));
    });
    Case("Dim_verticale", "_Dim 0,0 5,20 30,10", [&](Ctx& c) {
      const auto* d = ON_DimLinear::Cast(c.Last());
      CHECK(d && Near(d->Measurement(), 20.0, 1e-9));
      auto v = display(c, c.Last());
      CHECK(!v.labels.empty() && Near(v.labels[0].xdir.y, 1.0, 1e-9));  // si legge dal basso in alto
    });
    Case("DimAligned", "_DimAligned 0,0 30,40 20,10", [](Ctx& c) {
      const auto* d = ON_DimLinear::Cast(c.Last());
      CHECK(d && Near(d->Measurement(), 50.0, 1e-9) && d->Type() == ON::AnnotationType::Aligned);
    });
    Case("DimRotated", "_DimRotated 45 0,0 10,0 0,10", [](Ctx& c) {
      const auto* d = ON_DimLinear::Cast(c.Last());
      CHECK(d && Near(d->Measurement(), 10.0 / std::sqrt(2.0), 1e-9));
    });
    Case("DimRadius", "_Circle 0,0 8\n_SelAll _DimRadius 10,10", [&](Ctx& c) {
      const auto* d = ON_DimRadial::Cast(c.Last());
      CHECK(d && Near(d->Measurement(), 8.0, 1e-9) && d->Type() == ON::AnnotationType::Radius);
      auto v = display(c, c.Last());
      CHECK(v.labels.size() == 1 && v.labels[0].text == "R8.00");
    });
    Case("DimDiameter", "_Arc 0,0 5,0 0,5\n_SelAll _DimDiameter 10,2", [](Ctx& c) {
      const auto* d = ON_DimRadial::Cast(c.Last());
      CHECK(d && Near(d->Measurement(), 10.0, 1e-9) && d->Type() == ON::AnnotationType::Diameter);
    });
    Case("DimAngle", "_DimAngle 0,0 10,0 0,10 5,5", [&](Ctx& c) {
      const auto* d = ON_DimAngular::Cast(c.Last());
      CHECK(d && Near(d->Measurement(), ON_PI / 2, 1e-9));
      auto v = display(c, c.Last());
      if (!v.labels.empty()) std::printf("  testo angolo: [%s]\n", v.labels[0].text.c_str());
      CHECK(!v.labels.empty() && v.labels[0].text.rfind("90", 0) == 0);
    });
    Case("Leader", "_Leader 0,0 5,5 10,5 _Enter \"Nota\"", [&](Ctx& c) {
      const auto* l = ON_Leader::Cast(c.Last());
      CHECK(l && l->PointCount() == 3 && ToUtf8(l->PlainText()) == "Nota");
      auto v = display(c, c.Last());
      CHECK(v.wires.size() == 2 && v.labels.size() == 1);  // curva + freccia
    });
    Case("Centermark", "_Circle 3,4 6\n_SelAll _Centermark", [&](Ctx& c) {
      CHECK(ON_Centermark::Cast(c.Last()) != nullptr);
      CHECK(!display(c, c.Last()).wires.empty());
    });
    Case("DimOrdinate_X", "_DimOrdinate 7,3 7,10", [&](Ctx& c) {
      const auto* d = ON_DimOrdinate::Cast(c.Last());
      CHECK(d && Near(d->Measurement(), 7.0, 1e-9));
      CHECK(c.log.str().find("Quota coordinata X = 7") != std::string::npos);
      if (d) {
        auto ad = display(c, d);
        CHECK(!ad.wires.empty() && ad.labels.size() == 1 && !ad.labels.empty() && ad.labels[0].text.rfind("7", 0) == 0);
      }
    });
    Case("DimOrdinate_Y", "_DimOrdinate 7,3 15,3", [&](Ctx& c) {
      const auto* d = ON_DimOrdinate::Cast(c.Last());
      CHECK(d && Near(d->Measurement(), 3.0, 1e-9));
    });
  }

  // Altri comandi (CmdExtra)
  {
    std::printf("Altri comandi\n");
    Case("Circle3Pt", "_Circle3Pt 10,0 0,10 -10,0", [](Ctx& c) {
      const auto* a = ON_ArcCurve::Cast(c.Last());
      CHECK(a && a->IsCircle() && Near(a->Radius(), 10.0, 1e-9) && NearPt(a->m_arc.Center(), ON_3dPoint::Origin, 1e-9));
    });
    Case("Arc3Pt", "_Arc3Pt 10,0 -10,0 0,10", [](Ctx& c) {
      const auto* a = ON_ArcCurve::Cast(c.Last());
      CHECK(a && Near(a->m_arc.AngleDegrees(), 180.0, 1e-9) && NearPt(a->PointAtStart(), ON_3dPoint(10, 0, 0), 1e-9));
    });
    Case("Rectangle3Pt", "_Rectangle3Pt 0,0 10,10 0,20", [](Ctx& c) {
      const auto* p = ON_PolylineCurve::Cast(c.Last());
      CHECK(p && p->PointCount() == 5 && p->IsClosed());
      // larghezza = distanza di (0,20) dalla retta y=x
      CHECK(p && Near(p->m_pline[2].DistanceTo(p->m_pline[1]), 20.0 / std::sqrt(2.0), 1e-9));
    });
    Case("PolygonEdge", "_PolygonEdge NumSides=6 0,0 10,0", [](Ctx& c) {
      const auto* p = ON_PolylineCurve::Cast(c.Last());
      CHECK(p && p->PointCount() == 7 && p->IsClosed() && Near(Len(p), 60.0, 0.05));
    });
    Case("PolygonStar", "_PolygonStar NumSides=5 0,0 10,0 4", [](Ctx& c) {
      const auto* p = ON_PolylineCurve::Cast(c.Last());
      CHECK(p && p->PointCount() == 11 && p->IsClosed());
    });
    Case("EllipseDiameter", "_EllipseDiameter -10,0 10,0 0,5", [](Ctx& c) {
      ON_BoundingBox bb = c.BBox();
      CHECK(Near(bb.m_max.x, 10.0, 1e-6) && Near(bb.m_max.y, 5.0, 1e-6));
    });
    Case("PointGrid", "_PointGrid 3 4 2 5 1,1", [](Ctx& c) {
      CHECK(c.Objects().size() == 12);
      CHECK(NearPt(c.BBox().m_max, ON_3dPoint(5, 16, 0), 1e-9));
    });
    Case("ExtractControlPoints", "_Curve 0,0 5,5 10,0 15,5 _Enter\n_SelAll _ExtractControlPoints", [](Ctx& c) {
      int pts = 0;
      for (auto* g : c.Objects()) if (ON_Point::Cast(g)) pts++;
      CHECK(pts == 4);
    });
    Case("TruncatedCone", "_TruncatedCone 0,0,0 5 10 2", [](Ctx& c) {
      const auto* b = ON_Brep::Cast(c.Last());
      CHECK(b && b->IsSolid());
      CHECK(NearPt(c.BBox().m_max, ON_3dPoint(5, 5, 10), 1e-6));
    });
    Case("Ellipsoid", "_Ellipsoid 0,0,0 3 4 5", [](Ctx& c) {
      const auto* b = ON_Brep::Cast(c.Last());
      CHECK(b && b->IsSolid());
      CHECK(NearPt(c.BBox().m_max, ON_3dPoint(3, 4, 5), 1e-6));
    });
    Case("Tube", "_Tube 0,0,0 5 3 10", [](Ctx& c) {
      const auto* x = ON_Extrusion::Cast(c.Last());
      CHECK(x && x->ProfileCount() == 2 && x->IsCapped() == 3);
      std::unique_ptr<ON_Brep> b(x ? x->BrepForm() : nullptr);
      CHECK(b && b->IsSolid());
    });
    Case("MeshBox", "_MeshBox 0,0 10,5 3", [](Ctx& c) {
      const auto* m = ON_Mesh::Cast(c.Last());
      CHECK(m && m->FaceCount() == 6 && m->IsClosed());
      CHECK(m && std::fabs(MeshVolume(*m) - 150.0) < 1e-6);  // facce non intrecciate
    });
    Case("MeshSphere", "_MeshSphere 0,0,0 5", [](Ctx& c) {
      const auto* m = ON_Mesh::Cast(c.Last());
      CHECK(m && m->IsClosed() && m->IsManifold());
      CHECK(Near(c.BBox().m_max.z, 5.0, 1e-5));
    });
    Case("MeshCylinder", "_MeshCylinder 0,0,0 5 10", [](Ctx& c) {
      const auto* m = ON_Mesh::Cast(c.Last());
      CHECK(m && m->IsClosed());
    });
    Case("MeshCone", "_MeshCone 0,0,0 5 10", [](Ctx& c) {
      const auto* m = ON_Mesh::Cast(c.Last());
      CHECK(m && m->IsClosed() && Near(c.BBox().m_max.z, 10.0, 1e-5));
    });
    Case("MeshTorus", "_MeshTorus 0,0,0 10 2", [](Ctx& c) {
      const auto* m = ON_Mesh::Cast(c.Last());
      CHECK(m && m->IsClosed() && Near(c.BBox().m_max.x, 12.0, 1e-5));
    });
    Case("MeshPlane", "_MeshPlane 0,0 4,3", [](Ctx& c) {
      const auto* m = ON_Mesh::Cast(c.Last());
      CHECK(m && m->FaceCount() == 1);
    });
    Case("Reverse", "_Line 0,0 10,0\n_SelAll _Reverse", [](Ctx& c) {
      CHECK(NearPt(ON_Curve::Cast(c.Last())->PointAtStart(), ON_3dPoint(10, 0, 0), 1e-12));
    });
    Case("CloseCrv", "_Polyline 0,0 10,0 10,10 _Enter\n_SelAll _CloseCrv", [](Ctx& c) {
      const auto* cr = ON_Curve::Cast(c.Last());
      CHECK(c.Objects().size() == 1 && cr && cr->IsClosed() && Near(Len(cr), 20 + std::sqrt(200.0), 0.05));
    });
    Case("Isolate_Unisolate", "_Line 0,0 1,0\n_Line 0,1 1,1\n_SelLast _Isolate\n_SelNone\n_Unisolate", [](Ctx& c) {
      int hidden = 0;
      for (auto& id : c.e.AllObjectIds(true))
        if (c.e.Object(id)->Attributes(nullptr)->Mode() == ON::object_mode::hidden_object) hidden++;
      CHECK(hidden == 0 && c.e.AllObjectIds(false).size() == 2);
    });
    Case("Isolate", "_Line 0,0 1,0\n_Line 0,1 1,1\n_SelLast _Isolate", [](Ctx& c) {
      CHECK(c.e.AllObjectIds(false).size() == 1 && c.e.AllObjectIds(true).size() == 2);
    });
    Case("DupFaceBorder", "_Box 0,0,0 10,10 10\n_SelAll _DupFaceBorder", [](Ctx& c) {
      int closed = 0;
      for (auto* g : c.Objects()) if (auto* cr = ON_Curve::Cast(g); cr && cr->IsClosed() && Near(Len(cr), 40.0, 0.05)) closed++;
      CHECK(closed == 6);
    });
    Case("SelClosedCrv", "_Circle 0,0 3\n_Line 0,0 5,5\n_SelClosedCrv _Delete", [](Ctx& c) {
      CHECK(c.Objects().size() == 1 && ON_LineCurve::Cast(c.Last()));
    });
    Case("SelDup", "_Line 0,0 5,5\n_Line 0,0 5,5\n_Line 0,0 6,5\n_SelDup _Delete", [](Ctx& c) {
      CHECK(c.Objects().size() == 2);
    });
    Case("LayerOff_On", "_-Layer _New Muri\n_-Layer _Current Muri\n_Line 0,0 1,1\n_-Layer _Current Default\n_SelAll _LayerOff\n_LayerOn Muri", [](Ctx& c) {
      int vis = 0;
      ONX_ModelComponentIterator it(c.doc.Model(), ON_ModelComponent::Type::Layer);
      for (const ON_ModelComponent* m = it.FirstComponent(); m; m = it.NextComponent()) if (ON_Layer::Cast(m)->IsVisible()) vis++;
      CHECK(vis == 2);
    });
    Case("LayerOff", "_-Layer _New Muri\n_-Layer _Current Muri\n_Line 0,0 1,1\n_-Layer _Current Default\n_SelAll _LayerOff", [](Ctx& c) {
      int off = 0;
      ONX_ModelComponentIterator it(c.doc.Model(), ON_ModelComponent::Type::Layer);
      for (const ON_ModelComponent* m = it.FirstComponent(); m; m = it.NextComponent()) if (!ON_Layer::Cast(m)->IsVisible()) off++;
      CHECK(off == 1);
    });
    Case("Rotate3D", "_Line 0,0,0 10,0,0\n_SelAll _Rotate3D 0,0,0 0,1,0 90", [](Ctx& c) {
      CHECK(NearPt(ON_Curve::Cast(c.Last())->PointAtEnd(), ON_3dPoint(0, 0, -10), 1e-9));
    });
    Case("Scale1D", "_Line 0,0 10,10\n_SelAll _Scale1D 0,0 2 1,0", [](Ctx& c) {
      CHECK(NearPt(ON_Curve::Cast(c.Last())->PointAtEnd(), ON_3dPoint(20, 10, 0), 1e-9));
    });
    Case("ScaleNU", "_Line 0,0,0 1,1,1\n_SelAll _ScaleNU 0,0,0 2 3 4", [](Ctx& c) {
      CHECK(NearPt(ON_Curve::Cast(c.Last())->PointAtEnd(), ON_3dPoint(2, 3, 4), 1e-9));
    });
    Case("Orient", "_Line 0,0 10,0\n_SelAll _Orient 0,0 10,0 5,5 5,15", [](Ctx& c) {
      const auto* cr = ON_Curve::Cast(c.Last());
      CHECK(NearPt(cr->PointAtStart(), ON_3dPoint(5, 5, 0), 1e-9) && NearPt(cr->PointAtEnd(), ON_3dPoint(5, 15, 0), 1e-9));
    });
    Case("Orient_Scale", "_Line 0,0 10,0\n_SelAll _Orient Scale=Yes 0,0 10,0 0,0 0,20", [](Ctx& c) {
      CHECK(NearPt(ON_Curve::Cast(c.Last())->PointAtEnd(), ON_3dPoint(0, 20, 0), 1e-9));
    });
    Case("Orient3Pt", "_Line 0,0 10,0\n_SelAll _Orient3Pt 0,0 10,0 0,10 0,0,5 0,10,5 -10,0,5", [](Ctx& c) {
      CHECK(NearPt(ON_Curve::Cast(c.Last())->PointAtEnd(), ON_3dPoint(0, 10, 5), 1e-9));
    });
    Case("Dot", "_Dot 3,4 \"A1\"", [](Ctx& c) {
      const auto* d = ON_TextDot::Cast(c.Last());
      CHECK(d && ToUtf8(d->PrimaryText()) == "A1" && NearPt(d->CenterPoint(), ON_3dPoint(3, 4, 0)));
    });
    Case("Hatch", "_Rectangle 0,0 10,10\n_Circle 5,5 2\n_SelAll _Hatch", [](Ctx& c) {
      const ON_Hatch* h = nullptr;
      for (auto* g : c.Objects()) if (auto* x = ON_Hatch::Cast(g)) h = x;
      CHECK(h && h->LoopCount() == 2);
    });
    Case("EvaluatePt", "_EvaluatePt 1,2,3", [](Ctx& c) { CHECK(c.log.str().find("1,2,3") != std::string::npos); });
  }

  // Modalita' interattiva: il comando resta in attesa finche' mancano dati.
  {
    std::printf("Interactive\n");
    Ctx c;
    c.e.interactive = true;
    std::string pending;
    CHECK(c.e.RunInteractive("_Line", &pending) == Engine::Result::NeedsInput);
    CHECK(pending == "_Line" && c.e.Prompt() == "inizio della linea");
    CHECK(c.e.RunInteractive(pending + " 0,0", &pending) == Engine::Result::NeedsInput);
    CHECK(c.e.Prompt() == "fine della linea" && c.Objects().empty());
    CHECK(c.e.RunInteractive(pending + " 10,0", &pending) == Engine::Result::Done);
    CHECK(c.Objects().size() == 1 && pending.empty());
    // Polyline: i punti si accumulano finche' non arriva Invio
    CHECK(c.e.RunInteractive("_Polyline 0,0 5,0 5,5", &pending) == Engine::Result::NeedsInput);
    CHECK(c.Objects().size() == 1);
    CHECK(c.e.RunInteractive(pending + " _Enter", &pending) == Engine::Result::Done);
    CHECK(c.Objects().size() == 2);
    // un comando completato prima di uno in sospeso non viene ripetuto
    CHECK(c.e.RunInteractive("_Point 1,1 _Circle 0,0", &pending) == Engine::Result::NeedsInput);
    CHECK(pending == "_Circle 0,0" && c.Objects().size() == 3);
    CHECK(c.e.RunInteractive("_NonEsiste", &pending) == Engine::Result::Error);
    // Senza selezione il comando chiede gli oggetti (come Rhino) e riparte dopo la selezione
    c.e.ClearSelection();
    CHECK(c.e.RunInteractive("_Move", &pending) == Engine::Result::NeedsInput);
    CHECK(pending == "_Move" && c.e.PendingInputKind() == Engine::InputKind::Objects);
    c.e.Select(c.e.AllObjectIds().front());
    CHECK(c.e.RunInteractive(pending, &pending) == Engine::Result::NeedsInput);
    CHECK(c.e.PendingInputKind() == Engine::InputKind::Point);
#ifdef CAD_WITH_OCCT
    // Trim chiede un clic sull'oggetto (Pick), non un punto qualsiasi
    CHECK(c.e.RunInteractive("_Trim", &pending) == Engine::Result::NeedsInput);
    CHECK(c.e.PendingInputKind() == Engine::InputKind::Pick);
#endif
  }

  // Altri comandi (CmdExtra2)
  {
    std::printf("Altri comandi (2)\n");
    Case("Lines", "_Lines 0,0 10,0 0,5 10,5 _Enter", [](Ctx& c) {
      CHECK(c.Objects().size() == 2 && ON_LineCurve::Cast(c.Last()));
    });
    Case("PolylineThroughPoints", "_Point 0,0\n_Point 10,0\n_Point 10,10\n_SelAll _PolylineThroughPoints", [](Ctx& c) {
      const auto* p = ON_PolylineCurve::Cast(c.Last());
      CHECK(p && p->PointCount() == 3 && Near(Len(p), 20.0, 0.01));
    });
    Case("CurveThroughPt", "_Point 0,0\n_Point 5,5\n_Point 10,0\n_SelAll _CurveThroughPt", [](Ctx& c) {
      const auto* cv = ON_Curve::Cast(c.Last());
      CHECK(cv && NearPt(cv->PointAtEnd(), ON_3dPoint(10, 0, 0), 1e-9));
      double best = 1e9;
      for (int i = 0; cv && i <= 20000; i++) best = std::min(best, cv->PointAt(cv->Domain().ParameterAt(i / 20000.0)).DistanceTo(ON_3dPoint(5, 5, 0)));
      CHECK(best < 1e-3);
    });
    Case("Bisector", "_Bisector 0,0 10,0 0,10 5", [](Ctx& c) {
      const auto* l = ON_LineCurve::Cast(c.Last());
      CHECK(l && NearPt(l->PointAtEnd(), ON_3dPoint(5 / std::sqrt(2.0), 5 / std::sqrt(2.0), 0), 1e-9));
    });
    Case("AngleLine", "_AngleLine 0,0 10,0 90 4", [](Ctx& c) {
      const auto* l = ON_LineCurve::Cast(c.Last());
      CHECK(l && NearPt(l->PointAtEnd(), ON_3dPoint(0, 4, 0), 1e-9));
    });
    Case("RoundedRectangle", "_RoundedRectangle 0,0 10,6 1", [](Ctx& c) {
      const auto* p = ON_PolyCurve::Cast(c.Last());
      CHECK(p && p->IsClosed() && p->Count() == 8);
      CHECK(Near(Len(p), 2 * (8 + 4) + 2 * ON_PI, 0.01));
    });
    Case("Parabola", "_Parabola 0,0 0,1 4", [](Ctx& c) {
      const auto* cv = ON_Curve::Cast(c.Last());
      CHECK(cv && NearPt(cv->PointAtStart(), ON_3dPoint(4, 4, 0), 1e-9) || cv && NearPt(cv->PointAtStart(), ON_3dPoint(-4, 4, 0), 1e-9));
      // ogni punto soddisfa y = x^2 / 4f
      bool ok = cv != nullptr;
      for (int i = 0; cv && i <= 10; i++) { ON_3dPoint p = cv->PointAt(cv->Domain().ParameterAt(i / 10.0)); ok = ok && std::fabs(p.y - p.x * p.x / 4) < 1e-9; }
      CHECK(ok);
    });
    Case("Hyperbola", "_Hyperbola 0,0 2,0 1 3", [](Ctx& c) {
      const auto* cv = ON_Curve::Cast(c.Last());
      bool ok = cv != nullptr;
      for (int i = 0; cv && i <= 10; i++) { ON_3dPoint p = cv->PointAt(cv->Domain().ParameterAt(i / 10.0)); ok = ok && std::fabs(p.x * p.x / 4 - p.y * p.y - 1) < 1e-9; }
      CHECK(ok);
      CHECK(cv && NearPt(cv->PointAt(cv->Domain().Mid()), ON_3dPoint(2, 0, 0), 1e-9));
    });
    Case("Smooth", "_Polyline 0,0 5,10 10,0 _Enter\n_SelAll _Smooth Factor=1", [](Ctx& c) {
      const auto* n = ON_NurbsCurve::Cast(c.Last());
      ON_3dPoint p;
      CHECK(n && n->GetCV(1, p) && NearPt(p, ON_3dPoint(5, 0, 0), 1e-9));
    });
    Case("InsertKnot", "_Curve 0,0 5,5 10,0 15,5 _Enter\n_SelAll _InsertKnot 7,2 _Enter", [](Ctx& c) {
      const auto* n = ON_NurbsCurve::Cast(c.Last());
      CHECK(n && n->CVCount() == 5);
    });
    Case("CreateUVCrv", "_Plane 0,0 10,10\n_SelAll _CreateUVCrv", [](Ctx& c) {
      CHECK(c.Objects().size() == 5);
    });
    Case("Box3Pt", "_Box3Pt 0,0 10,0 0,5 3", [](Ctx& c) {
      const auto* b = ON_Brep::Cast(c.Last());
      CHECK(b && b->IsSolid() && NearPt(c.BBox().m_max, ON_3dPoint(10, 5, 3), 1e-9));
    });
    Case("Sphere3Pt", "_Sphere3Pt 5,0 -5,0 0,5", [](Ctx& c) {
      CHECK(NearPt(c.BBox().m_max, ON_3dPoint(5, 5, 5), 1e-6));
    });
    Case("SubDSphere", "_SubDSphere 0,0,0 10", [](Ctx& c) {
      const auto* s = ON_SubD::Cast(c.Last());
      CHECK(s && s->FaceCount() == 32);
      if (!s) return;
      SubDLimitDisplay lim = BuildSubDLimit(*s, 2);
      CHECK(lim.mesh != nullptr);
      if (lim.mesh) {
        ON_BoundingBox bb = lim.mesh->BoundingBox();
        CHECK(Near(bb.m_max.x, 10, 1e-3) && Near(bb.m_max.z, 10, 1e-3) && Near(bb.m_min.y, -10, 1e-3));
      }
    });
    Case("SubDLimit_punti", "_SubDBox 0,0,0 10,10 10", [](Ctx& c) {
      // i punti limite dei vertici della gabbia coincidono con quelli di openNURBS
      const auto* s = ON_SubD::Cast(c.Last());
      if (!s) { CHECK(false); return; }
      SubDLimitDisplay lim = BuildSubDLimit(*s, 3);
      CHECK(lim.mesh && lim.mesh->m_F.Count() == 6 * 64);
      CHECK(lim.edges.size() == 12);
      int i = 0;
      double worst = 0;
      ON_SubDVertexIterator vit(*s);
      for (const ON_SubDVertex* v = vit.FirstVertex(); v && lim.mesh; v = vit.NextVertex(), i++)
        worst = std::max(worst, ON_3dPoint(lim.mesh->m_V[i]).DistanceTo(v->SurfacePoint()));
      CHECK(worst < 1e-4);
    });
    Case("SubDPlane", "_SubDPlane 0,0 10,10", [](Ctx& c) {
      CHECK(ON_SubD::Cast(c.Last()) && ON_SubD::Cast(c.Last())->FaceCount() == 1);
    });
    Case("SubDCylinder", "_SubDCylinder 0,0,0 5 10", [](Ctx& c) {
      CHECK(ON_SubD::Cast(c.Last()) && ON_SubD::Cast(c.Last())->FaceCount() == 16);
    });
    Case("SubDTorus", "_SubDTorus 0,0,0 10 2", [](Ctx& c) {
      CHECK(ON_SubD::Cast(c.Last()) && ON_SubD::Cast(c.Last())->FaceCount() == 32);
    });
    Case("SubDFromMesh", "_MeshBox 0,0,0 10,10 10\n_SelAll _SubDFromMesh", [](Ctx& c) {
      CHECK(ON_SubD::Cast(c.Last()) && ON_SubD::Cast(c.Last())->FaceCount() == 6);
    });
    Case("Weld", "_MeshBox 0,0,0 10,10 10\n_SelAll _Weld", [](Ctx& c) {
      const auto* m = ON_Mesh::Cast(c.Last());
      CHECK(m && m->VertexCount() == 8);
    });
    Case("MeshPolyline", "_Polygon NumSides=6 0,0 5,0\n_SelAll _MeshPolyline", [](Ctx& c) {
      const auto* m = ON_Mesh::Cast(c.Last());
      CHECK(m && m->FaceCount() == 6);
    });
    Case("MeshToNURB", "_MeshBox 0,0,0 10,10 10\n_SelAll _MeshToNURB", [](Ctx& c) {
      const auto* b = ON_Brep::Cast(c.Last());
      CHECK(b && b->m_F.Count() == 6 && b->IsSolid());
    });
    Case("Shear", "_Line 0,0 0,10\n_SelAll _Shear 0,0 10,0 45", [](Ctx& c) {
      const auto* l = ON_LineCurve::Cast(c.Last());
      CHECK(l && (NearPt(l->PointAtEnd(), ON_3dPoint(-10, 10, 0), 1e-9) || NearPt(l->PointAtEnd(), ON_3dPoint(10, 10, 0), 1e-9)));
    });
    Case("SetPt", "_Line 0,0,1 10,0,5\n_SelAll _SetPt 0,0,2", [](Ctx& c) {
      const auto* l = ON_LineCurve::Cast(c.Last());
      CHECK(l && Near(l->PointAtStart().z, 2, 1e-12) && Near(l->PointAtEnd().z, 2, 1e-12) && Near(l->PointAtEnd().x, 10, 1e-12));
    });
    Case("Align", "_Circle 0,0 1\n_Circle 10,5 2\n_SelAll _Align Mode=Left", [](Ctx& c) {
      for (auto* g : c.Objects()) { ON_BoundingBox b = g->BoundingBox(); CHECK(Near(b.m_min.x, -1, 1e-6)); }
    });
    Case("SelColor", "_Point 0,0\n_Point 1,1\n_SelAll _SetObjectColor 255,0,0\n_Point 2,2\n_SelNone _SelColor 255,0,0", [](Ctx& c) {
      CHECK(c.e.Selection().size() == 2);
    });
    Case("SelSmall", "_Circle 0,0 1\n_Circle 0,0 10\n_SelSmall 5", [](Ctx& c) {
      CHECK(c.e.Selection().size() == 1);
    });
    Case("MatchProperties", "-_Layer _New A\n_CurrentLayer A\n_Point 20,20\n_SelLast _SetObjectColor 0,255,0\n_CurrentLayer Default\n_Line 0,0 10,0\n_SelNone _SelLast _MatchProperties 20,20", [](Ctx& c) {
      const auto& ids = c.e.AllObjectIds();
      const auto* a = c.e.Object(ids.back())->Attributes(nullptr);
      const auto* b = c.e.Object(ids.front())->Attributes(nullptr);
      CHECK(a && b && a->m_layer_index == b->m_layer_index && a->m_color == b->m_color);
    });
    Case("SetUserText", "_Point 0,0\n_SelAll _SetUserText Peso 12\n_GetUserText", [](Ctx& c) {
      CHECK(c.log.str().find("Peso = 12") != std::string::npos);
    });
    Case("PointsOn_GripMove", "_Curve 0,0 5,5 10,0 15,5 _Enter\n_SelAll _PointsOn", [](Ctx& c) {
      CHECK(c.e.pointsOn.size() == 1);
      const ON_UUID id = *c.e.pointsOn.begin();
      char s[64];
      ON_UuidToString(id, s);
      CHECK(c.e.Run(std::string("_GripMove ") + s + " 1 -1 5,20,0"));
      const auto* n = ON_NurbsCurve::Cast(c.e.Object(id)->Geometry(nullptr));
      ON_3dPoint p;
      CHECK(n && n->GetCV(1, p) && NearPt(p, ON_3dPoint(5, 20, 0), 1e-12));
      CHECK(c.e.pointsOn.count(id) == 1 && c.e.AllObjectIds().size() == 1);
      CHECK(c.e.Undo());
      const auto* u = ON_NurbsCurve::Cast(c.e.Object(id) ? c.e.Object(id)->Geometry(nullptr) : nullptr);
      CHECK(u && u->GetCV(1, p) && NearPt(p, ON_3dPoint(5, 5, 0), 1e-12));
    });
    Case("GripMove_superficie_mesh_subd", "_Plane 0,0 10,10\n_MeshPlane 20,0 30,10\n_SubDBox 40,0,0 50,10 10\n_SelAll _PointsOn", [](Ctx& c) {
      CHECK(c.e.pointsOn.size() == 3);
      int moved = 0;
      for (const auto& id : c.e.AllObjectIds()) {
        GripSet gs;
        const ON_Geometry* g = c.e.Object(id)->Geometry(nullptr);
        if (!GetGrips(g, gs) || gs.grips.empty()) continue;
        const Grip gp = gs.grips.front();
        char s[64];
        ON_UuidToString(id, s);
        const ON_3dPoint target = gp.p + ON_3dVector(0, 0, 7);
        if (c.e.Run(std::string("_GripMove ") + s + " " + std::to_string(gp.i) + " " + std::to_string(gp.j) + " " +
                    std::to_string(target.x) + "," + std::to_string(target.y) + "," + std::to_string(target.z))) {
          GripSet after;
          GetGrips(c.e.Object(id)->Geometry(nullptr), after);
          bool found = false;
          for (const auto& q : after.grips) if (q.p.DistanceTo(target) < 1e-6) found = true;
          if (found) moved++;
        }
      }
      CHECK(moved == 3);
    });
    Case("PointsOff", "_Line 0,0 1,1\n_SelAll _PointsOn\n_PointsOff", [](Ctx& c) {
      CHECK(c.e.pointsOn.empty());
    });
    Case("Export_Import", "-_Layer _New Rosso\n_CurrentLayer Rosso\n_Circle 0,0 5\n_Line 0,0 9,9\n_SelNone _SelCrv", [](Ctx& c) {
      const std::string f = g_outDir + "/export_test.3dm";
      std::remove(f.c_str());
      c.e.ClearSelection();
      c.e.Select(c.e.AllObjectIds().front());
      CHECK(c.e.Run("_Export \"" + f + "\""));
      Ctx d;
      CHECK(d.e.Run("_Import \"" + f + "\""));
      CHECK(d.Objects().size() == 1 && ON_ArcCurve::Cast(d.Last()));
      const auto* a = d.e.Object(d.e.AllObjectIds().front())->Attributes(nullptr);
      const auto* l = a ? ON_Layer::Cast(d.e.Model().LayerFromIndex(a->m_layer_index).ModelComponent()) : nullptr;
      CHECK(l && ON_wString::EqualOrdinal(l->Name(), L"Rosso", true));
    });
    Case("IncrementalSave", "_Point 0,0", [](Ctx& c) {
      const std::string base = g_outDir + "/incr";
      std::remove((base + " 001.3dm").c_str());
      std::remove((base + " 002.3dm").c_str());
      c.e.currentPath = base + ".3dm";
      CHECK(c.e.Run("_IncrementalSave"));
      CHECK(c.e.Run("_IncrementalSave"));
      CHECK(c.e.currentPath == base + " 002.3dm");
    });
    Case("SelPrev", "_Point 0,0\n_Point 1,1\n_SelAll\n_SelNone\n_SelPrev", [](Ctx& c) {
      CHECK(c.e.Selection().size() == 2);
    });
    Case("Crease", "_SubDBox 0,0,0 10,10 10\n_SelAll _Crease 5,0,10 _Enter", [](Ctx& c) {
      const auto* s = ON_SubD::Cast(c.Last());
      int creases = 0;
      ON_SubDEdgeIterator it(*s);
      for (const ON_SubDEdge* e = it.FirstEdge(); s && e; e = it.NextEdge()) if (e->m_edge_tag == ON_SubDEdgeTag::Crease) creases++;
      CHECK(creases == 1);
      // lo spigolo vivo passa per la superficie limite piu' in alto (verso la gabbia)
      SubDLimitDisplay lim = BuildSubDLimit(*s, 3);
      CHECK(lim.mesh && lim.mesh->BoundingBox().m_max.z > 9.0);
    });
    Case("Uncrease", "_SubDBox 0,0,0 10,10 10\n_SelAll _Crease 5,0,10 _Enter\n_SelAll _Uncrease 5,0,10 _Enter", [](Ctx& c) {
      const auto* s = ON_SubD::Cast(c.Last());
      int creases = 0;
      ON_SubDEdgeIterator it(*s);
      for (const ON_SubDEdge* e = it.FirstEdge(); s && e; e = it.NextEdge()) if (e->m_edge_tag == ON_SubDEdgeTag::Crease) creases++;
      CHECK(creases == 0);
    });
    Case("SubDDisplayToggle", "_SubDDisplayToggle", [](Ctx& c) { CHECK(c.e.subdFlat); });
    Case("SelBadObjects", "_Line 0,0 1,1\n_SelBadObjects", [](Ctx& c) { CHECK(c.e.Selection().empty()); });
    Case("ConicCurve", "_ConicCurve 0,0 10,0 5,5 0.5", [](Ctx& c) {
      const auto* cv = ON_Curve::Cast(c.Last());
      // rho 0.5: parabola, il punto medio sta a meta' tra la corda e il vertice
      CHECK(cv && NearPt(cv->PointAt(cv->Domain().Mid()), ON_3dPoint(5, 2.5, 0), 1e-9));
    });
    Case("ConvertToBezier", "_Curve 0,0 5,5 10,0 15,5 20,0 _Enter\n_SelAll _ConvertToBezier", [](Ctx& c) {
      const auto* pc = ON_PolyCurve::Cast(c.Last());
      CHECK(pc && pc->Count() == 2);
      CHECK(pc && NearPt(pc->PointAtEnd(), ON_3dPoint(20, 0, 0), 1e-9));
    });
    Case("Purge", "-_Layer _New A\n-_Layer _New B\n_CurrentLayer B\n_Point 0,0\n_Purge", [](Ctx& c) {
      CHECK(c.log.str().find("Eliminati") != std::string::npos);
      int layers = 0;
      ONX_ModelComponentIterator it(c.e.Model(), ON_ModelComponent::Type::Layer);
      for (auto* x = it.FirstComponent(); x; x = it.NextComponent()) layers++;
      CHECK(layers == 1);
    });
  }

  // Deformazioni e altri comandi (CmdExtra3)
  {
    std::printf("Altri comandi (3)\n");
    Case("Tangent", "_Circle 0,0 5\n_Tangent 5,0 5,7", [](Ctx& c) {
      const auto* l = ON_LineCurve::Cast(c.Last());
      CHECK(l && NearPt(l->m_line.from, ON_3dPoint(5, 0, 0), 1e-6) && NearPt(l->m_line.to, ON_3dPoint(5, 7, 0), 1e-6));
    });
    Case("Perpendicular", "_Circle 0,0 5\n_Perpendicular 0,5 3,9", [](Ctx& c) {
      const auto* l = ON_LineCurve::Cast(c.Last());
      CHECK(l && NearPt(l->m_line.from, ON_3dPoint(0, 5, 0), 1e-6) && NearPt(l->m_line.to, ON_3dPoint(0, 9, 0), 1e-6));
    });
    Case("Reduce", "_MeshSphere 0,0,0 10\n_SelAll _Reduce ReductionPercent=75", [](Ctx& c) {
      const auto* m = ON_Mesh::Cast(c.Last());
      CHECK(m && m->IsValid() && c.Objects().size() == 1);
      // la forma resta una sfera: tutti i vertici vicini al raggio
      bool ok = m != nullptr;
      for (int i = 0; m && i < m->VertexCount(); i++) ok = ok && std::fabs(ON_3dPoint(m->m_V[i]).DistanceTo(ON_origin) - 10) < 1.0;
      CHECK(ok);
      CHECK(m && m->IsClosed());
    });
    Case("SubDCone", "_SubDCone 0,0,0 5 10", [](Ctx& c) {
      const auto* s = ON_SubD::Cast(c.Last());
      CHECK(s && s->IsValid() && s->FaceCount() == 8 + 8);
    });
    Case("SimplifyCrv", "_Polyline 0,0 10,0 20,0 20,10\n_SelAll _SimplifyCrv", [](Ctx& c) {
      const auto* pc = ON_PolyCurve::Cast(c.Last());
      CHECK(pc && pc->Count() == 2 && ON_LineCurve::Cast(pc->SegmentCurve(0)) && ON_LineCurve::Cast(pc->SegmentCurve(1)));
      CHECK(pc && NearPt(pc->SegmentCurve(0)->PointAtEnd(), ON_3dPoint(20, 0, 0), 1e-9));
    });
    Case("Match_tangenza", "_Line 0,0 10,0\n_Line 12,2 20,6\n_Match 10,0 12,2", [](Ctx& c) {
      const ON_Curve* m = nullptr;
      for (auto* g : c.Objects()) if (ON_NurbsCurve::Cast(g)) m = ON_Curve::Cast(g);
      CHECK(m && NearPt(m->PointAtEnd(), ON_3dPoint(12, 2, 0), 1e-9) && NearPt(m->PointAtStart(), ON_3dPoint(0, 0, 0), 1e-9));
      ON_3dVector d(8, 4, 0);
      d.Unitize();
      CHECK(m && (m->TangentAt(m->Domain()[1]) - d).Length() < 1e-9);
    });
    Case("Match_curvatura", "_Line 0,0 10,0\n_Arc3Pt 12,0 22,0 17,5\n_Match Continuity=Curvature 10,0 12,0", [](Ctx& c) {
      const ON_Curve* m = nullptr;
      for (auto* g : c.Objects()) if (ON_NurbsCurve::Cast(g)) m = ON_Curve::Cast(g);
      CHECK(m && NearPt(m->PointAtEnd(), ON_3dPoint(12, 0, 0), 1e-9));
      CHECK(m && (m->TangentAt(m->Domain()[1]) - ON_3dVector(0, 1, 0)).Length() < 1e-9);
      CHECK(m && (m->CurvatureAt(m->Domain()[1]) - ON_3dVector(0.2, 0, 0)).Length() < 1e-6);
    });
    Case("CircleTangent", "_Line -10,0 10,0\n_Line -10,10 10,10\n_Line -5,-10 -5,20\n_CircleTangent 1,0 1,10 -5,4", [](Ctx& c) {
      const auto* a = ON_ArcCurve::Cast(c.Last());
      CHECK(a && a->IsCircle() && Near(a->m_arc.Radius(), 5, 1e-6) && NearPt(a->m_arc.Center(), ON_3dPoint(0, 5, 0), 1e-6));
    });
    Case("ArcTangent", "_Line 0,0 10,0\n_Line 0,0 0,10\n_ArcTangent 5,0 0,5 2", [](Ctx& c) {
      const auto* a = ON_ArcCurve::Cast(c.Last());
      CHECK(a && Near(a->m_arc.Radius(), 2, 1e-6) && NearPt(a->m_arc.Center(), ON_3dPoint(2, 2, 0), 1e-6));
      CHECK(a && Near(a->m_arc.AngleDegrees(), 90, 1e-6));
    });
    Case("PolylineOnMesh", "_MeshSphere 0,0,0 10\n_PolylineOnMesh 10,0,0 10,0,0 0,10,0 _Enter", [](Ctx& c) {
      const auto* pl = ON_PolylineCurve::Cast(c.Last());
      CHECK(pl && pl->PointCount() > 5);
      bool ok = pl != nullptr;
      for (int i = 0; pl && i < pl->PointCount(); i++) ok = ok && pl->m_pline[i].DistanceTo(ON_origin) > 9 && pl->m_pline[i].DistanceTo(ON_origin) < 10.001;
      CHECK(ok);
    });
    Case("ExtractMeshPart", "_MeshBox 0,0 1,1 1\n_MeshBox 5,0 6,1 1\n_SelMesh _Join\n_ExtractMeshPart 5.5,0.5,1", [](Ctx& c) {
      CHECK(c.Objects().size() == 2);
      for (auto* g : c.Objects()) if (const auto* m = ON_Mesh::Cast(g)) CHECK(m->FaceCount() == 6);
    });
    Case("Unweld", "_MeshSphere 0,0,0 10\n_SelAll _Unweld Angle=0", [](Ctx& c) {
      const auto* m = ON_Mesh::Cast(c.Last());
      // senza saldature: ogni faccia ha i suoi vertici
      CHECK(m && m->VertexCount() > 0 && m->IsValid());
      int corners = 0;
      for (int i = 0; m && i < m->FaceCount(); i++) corners += m->m_F[i].IsQuad() ? 4 : 3;
      CHECK(m && m->VertexCount() == corners);
    });
    Case("UnweldEdge", "_MeshBox 0,0 10,10 10\n_UnweldEdge 5,0,0 10,5,0 5,10,0 0,5,0 _Enter", [](Ctx& c) {
      const auto* m = ON_Mesh::Cast(c.Last());
      CHECK(m && m->VertexCount() == 12);  // la faccia di fondo ha vertici suoi
    });
    Case("ToSubD_mesh", "_MeshBox 0,0 10,10 10\n_SelAll _ToSubD", [](Ctx& c) {
      const auto* s = ON_SubD::Cast(c.Last());
      CHECK(s && s->IsValid() && s->FaceCount() == 6 && c.Objects().size() == 1);
    });
    Case("ToSubD_superficie", "_SrfPt 0,0 10,0 10,10 0,10\n_SelAll _ToSubD", [](Ctx& c) {
      const auto* s = ON_SubD::Cast(c.Last());
      CHECK(s && s->IsValid() && s->FaceCount() == 1);
    });
    Case("Twist_linea", "_Line 5,0,0 5,0,10\n_SelAll _Twist 0,0,0 0,0,10 90", [](Ctx& c) {
      const auto* cv = ON_Curve::Cast(c.Last());
      // la cima ruota di 90 gradi attorno a z: (5,0,10) -> (0,5,10)
      CHECK(cv && NearPt(cv->PointAtEnd(), ON_3dPoint(0, 5, 10), 1e-9) && NearPt(cv->PointAtStart(), ON_3dPoint(5, 0, 0), 1e-9));
      // a meta' altezza la rotazione e' di 45 gradi (raggio 5 conservato)
      bool ok = cv != nullptr;
      for (int i = 0; cv && i <= 20; i++) {
        const ON_3dPoint p = cv->PointAt(cv->Domain().ParameterAt(i / 20.0));
        ok = ok && std::fabs(std::hypot(p.x, p.y) - 5) < 0.05;
      }
      CHECK(ok);
    });
    Case("Twist_box", "_Box 0,0,0 10,10 20\n_SelAll _Twist 5,5,0 5,5,20 45", [](Ctx& c) {
      const auto* b = ON_Brep::Cast(c.Last());
      CHECK(b && b->IsValid() && b->m_F.Count() == 6);
    });
    Case("Taper", "_Line 5,0,0 5,0,10\n_SelAll _Taper 0,0,0 0,0,10 5 1", [](Ctx& c) {
      const auto* cv = ON_Curve::Cast(c.Last());
      CHECK(cv && NearPt(cv->PointAtEnd(), ON_3dPoint(1, 0, 10), 1e-9));
    });
    Case("Bend", "_Line 0,0 10,0\n_SelAll _Bend 0,0 10,0 6.366197723675814,6.366197723675814", [](Ctx& c) {
      // il punto sta sul quarto di cerchio di raggio 2L/pi: la linea diventa un quarto di cerchio
      const auto* cv = ON_Curve::Cast(c.Last());
      const double R = 20.0 / ON_PI;
      CHECK(cv && NearPt(cv->PointAtEnd(), ON_3dPoint(R, R, 0), 1e-3));
    });
    Case("ArrayCrv", "_Line 0,0 20,0\n_Point 0,0,1\n_SelNone _SelPt _ArrayCrv Count=5 10,0", [](Ctx& c) {
      int pts = 0;
      double maxx = 0;
      for (auto* g : c.Objects()) if (auto* p = ON_Point::Cast(g)) { pts++; maxx = std::max(maxx, p->point.x); }
      CHECK(pts == 5 && Near(maxx, 20.0, 1e-6));
    });
    Case("ConvertToPolyline", "_Circle 0,0 10\n_SelAll _ConvertToPolyline Tolerance=0.01", [](Ctx& c) {
      const auto* p = ON_PolylineCurve::Cast(c.Last());
      CHECK(p && p->IsClosed() && p->PointCount() > 20);
      bool ok = p != nullptr;
      for (int i = 0; p && i + 1 < p->PointCount(); i++) {
        const ON_3dPoint m = 0.5 * (p->m_pline[i] + p->m_pline[i + 1]);
        ok = ok && std::fabs(m.DistanceTo(ON_3dPoint::Origin) - 10) <= 0.0101;
      }
      CHECK(ok);
    });
    Case("FilletCorners", "_Rectangle 0,0 10,6\n_SelAll _FilletCorners 1", [](Ctx& c) {
      const auto* pc = ON_PolyCurve::Cast(c.Last());
      CHECK(pc && pc->IsClosed() && pc->Count() == 8);
      CHECK(Near(Len(pc), 2 * (8 + 4) + 2 * ON_PI, 0.01));
    });
    Case("BlendCrv", "_Line 0,0 10,0\n_Arc 20,5 25,5 90\n_BlendCrv 9,0 25,5 Continuity=Curvature", [](Ctx& c) {
      const auto* n = ON_NurbsCurve::Cast(c.Last());
      CHECK(n && n->Degree() == 5 && NearPt(n->PointAtStart(), ON_3dPoint(10, 0, 0), 1e-9));
    });
    Case("BlendCrv_G1", "_Line 0,0 10,0\n_Line 20,5 30,5\n_BlendCrv 9,0 21,5 Continuity=Tangency", [](Ctx& c) {
      const auto* n = ON_NurbsCurve::Cast(c.Last());
      CHECK(n && NearPt(n->PointAtEnd(), ON_3dPoint(20, 5, 0), 1e-9));
      ON_3dVector t0 = n ? n->TangentAt(n->Domain()[0]) : ON_3dVector::ZeroVector;
      CHECK(Near(t0.x, 1.0, 1e-9));
    });
    Case("UnifyMeshNormals", "_MeshBox 0,0,0 10,10 10\n_SelAll _UnifyMeshNormals", [](Ctx& c) {
      const auto* m = ON_Mesh::Cast(c.Last());
      CHECK(m && m->IsClosed() && m->IsManifold());
    });
    Case("FillMeshHoles", "_MeshCylinder 0,0,0 5 10\n_SelAll _Explode", [](Ctx&) {}, false);
    Case("FillMeshHoles_piano", "_MeshPlane 0,0 10,10\n_SelAll _FillMeshHoles", [](Ctx& c) {
      // una mesh piana aperta: il bordo diventa un foro chiuso (la mesh si chiude su se stessa)
      const auto* m = ON_Mesh::Cast(c.Last());
      CHECK(m && m->IsClosed());
    });
  }

#ifdef CAD_WITH_OCCT
  // Superfici, terza parte (ExtrudeCrvTapered, Slab, ExtendSrf, Pull, Sweep2, Flow, ArraySrf)
  {
    std::printf("Superfici (3)\n");
    auto vol = [](Ctx& c) {
      const std::string log = c.log.str();
      auto p = log.rfind("Volume = ");
      return p == std::string::npos ? -1.0 : std::atof(log.c_str() + p + 9);
    };
    Case("ExtrudeCrvTapered_chiusa", "_Rectangle 0,0 10,10\n_SelAll _ExtrudeCrvTapered DraftAngle=45 2", [](Ctx& c) {
      const auto* b = ON_Brep::Cast(c.Last());
      CHECK(b && b->IsValid() && b->IsSolid());
      ON_BoundingBox bb;
      if (b) b->GetTightBoundingBox(bb);
      CHECK(b && Near(bb.m_max.z, 2, 1e-6));
      // la cima e' il quadrato ristretto di 2 per lato: 6 x 6 a z=2
      bool topOk = false;
      for (int i = 0; b && i < b->m_V.Count(); i++)
        if (NearPt(b->m_V[i].point, ON_3dPoint(2, 2, 2), 1e-6) || NearPt(b->m_V[i].point, ON_3dPoint(8, 8, 2), 1e-6)) topOk = true;
      CHECK(topOk);
    });
    Case("ExtrudeCrvTapered_volume", "_Rectangle 0,0 10,10\n_SelAll _ExtrudeCrvTapered DraftAngle=45 2\n_SelAll _Volume", [&](Ctx& c) {
      // tronco di piramide: h/3 (A1 + A2 + sqrt(A1 A2)) = 2/3 (100 + 36 + 60)
      CHECK(Near(vol(c), 2.0 / 3.0 * 196.0, 1e-3));
    });
    Case("ExtrudeCrvTapered_aperta", "_Line 0,0 10,0\n_SelAll _ExtrudeCrvTapered DraftAngle=45 3", [](Ctx& c) {
      const auto* b = ON_Brep::Cast(c.Last());
      CHECK(b && b->IsValid());
      ON_BoundingBox bb;
      if (b) b->GetTightBoundingBox(bb);
      // lato sinistro della linea verso +x: sale a y=3
      CHECK(b && Near(bb.m_max.z, 3, 1e-9) && Near(bb.m_max.y, 3, 1e-9));
    });
    Case("Slab", "_Line 0,0 10,0\n_SelAll _Slab 2 5,1 3\n_SelAll _Volume", [&](Ctx& c) {
      CHECK(Near(vol(c), 60.0, 1e-3));
      ON_BoundingBox bb = c.BBox();
      CHECK(Near(bb.m_max.y, 2, 1e-6) && Near(bb.m_min.y, 0, 1e-6));
    });
    Case("ExtendSrf", "_Plane 0,0 10,10\n_ExtendSrf 10,5 3", [](Ctx& c) {
      const auto* b = ON_Brep::Cast(c.Last());
      CHECK(b && b->IsValid() && c.Objects().size() == 1);
      ON_BoundingBox bb;
      if (b) b->GetTightBoundingBox(bb);
      CHECK(b && Near(bb.m_max.x, 13, 1e-6) && Near(bb.m_max.y, 10, 1e-6));
    });
    Case("Pull_cerchio", "_Sphere 0,0,0 10\n_Circle 0,0,20 3\n_SelCrv _Pull 0,0,10", [](Ctx& c) {
      const auto* cv = ON_Curve::Cast(c.Last());
      CHECK(cv != nullptr);
      bool ok = cv != nullptr;
      for (int i = 0; cv && i <= 20; i++) ok = ok && Near(cv->PointAt(cv->Domain().ParameterAt(i / 20.0)).DistanceTo(ON_origin), 10, 1e-3);
      CHECK(ok);
    });
    Case("Pull_punto", "_Plane 0,0 10,10\n_Point 3,4,5\n_SelPt _Pull 5,5,0", [](Ctx& c) {
      const auto* p = ON_Point::Cast(c.Last());
      CHECK(p && NearPt(p->point, ON_3dPoint(3, 4, 0), 1e-6));
    });
    Case("Sweep2", "_Line 0,0,0 20,0,0\n_Line 0,10,0 20,10,0\n_Arc3Pt 0,0,0 0,10,0 0,5,3\n_SelCrv _Sweep2", [](Ctx& c) {
      const auto* b = ON_Brep::Cast(c.Last());
      CHECK(b && b->IsValid());
      ON_BoundingBox bb;
      if (b) b->GetTightBoundingBox(bb);
      CHECK(b && Near(bb.m_max.x, 20, 1e-3) && Near(bb.m_max.z, 3, 1e-3) && Near(bb.m_max.y, 10, 1e-3));
    });
    Case("Sweep2_scala", "_Line 0,0,0 20,0,0\n_Line 0,10,0 20,20,0\n_Arc3Pt 0,0,0 0,10,0 0,5,3\n_SelCrv _Sweep2", [](Ctx& c) {
      const auto* b = ON_Brep::Cast(c.Last());
      ON_BoundingBox bb;
      if (b) b->GetTightBoundingBox(bb);
      // in fondo la larghezza e' sqrt(20^2+20^2)... l'altezza scala con la larghezza (20 / 10)
      CHECK(b && bb.m_max.z > 5.0 && bb.m_max.z < 6.5);
    });
    Case("Flow", "_Line 0,0 10,0\n_Line 0,20 0,30\n_Point 5,1,2\n_SelPt _Flow 0,0 0,20", [](Ctx& c) {
      // (5,1,2) a 5 dalla base, 1 a sinistra, 2 in alto -> sulla linea verticale: (-1,25,2)
      const auto* p = ON_Point::Cast(c.Last());
      CHECK(p && NearPt(p->point, ON_3dPoint(-1, 25, 2), 1e-3));
    });
    Case("Flow_stretch", "_Line 0,0 10,0\n_Line 0,20 0,40\n_Point 5,0,0\n_SelPt _Flow Stretch=Yes 0,0 0,20", [](Ctx& c) {
      const auto* p = ON_Point::Cast(c.Last());
      CHECK(p && NearPt(p->point, ON_3dPoint(0, 30, 0), 1e-3));
    });
    Case("LineSrfNormal", "_Sphere 0,0,0 10\n_LineSrfNormal 0,0.1,10.2 5", [](Ctx& c) {
      const auto* l = ON_LineCurve::Cast(c.Last());
      // punto della sfera piu' vicino al clic e normale radiale
      CHECK(l && std::fabs(l->m_line.from.DistanceTo(ON_origin) - 10) < 1e-6 && std::fabs(l->m_line.to.DistanceTo(ON_origin) - 15) < 1e-6);
      CHECK(l && l->m_line.from.y > 0.05 && l->m_line.from.y < 0.15);
    });
    Case("NetworkSrf", "_Line 0,0,0 10,0,0\n_Line 10,0,0 10,10,0\n_Line 10,10,0 0,10,0\n_Line 0,10,0 0,0,0\n"
                       "_Arc3Pt 5,0,0 5,10,0 5,5,3\n_SelCrv _NetworkSrf", [](Ctx& c) {
      const auto* b = ON_Brep::Cast(c.Last());
      CHECK(b && b->IsValid() && b->m_F.Count() == 1);
      // la superficie passa vicino al colmo della curva interna
      ON_BoundingBox bb;
      if (b) b->GetTightBoundingBox(bb);
      CHECK(b && bb.m_max.z > 2.7 && bb.m_max.z < 3.3);
    });
    Case("MeshBooleanUnion", "_MeshBox 0,0 10,10 10\n_MeshBox 5,5,5 15,15,5 10\n_SelMesh _MeshBooleanUnion", [](Ctx& c) {
      const auto* m = ON_Mesh::Cast(c.Last());
      CHECK(m && c.Objects().size() == 1 && m->IsClosed());
      ON_BoundingBox bb = c.BBox();
      CHECK(NearPt(bb.m_min, ON_3dPoint(0, 0, 0), 1e-6) && NearPt(bb.m_max, ON_3dPoint(15, 15, 15), 1e-6));
      CHECK(m && std::fabs(MeshVolume(*m) - (1000.0 * 2 - 125.0)) < 1e-3);
    });
    Case("MeshBooleanDifference", "_MeshBox 0,0 10,10 10\n_MeshBox 5,5,5 15,15,5 10\n_SelMesh _MeshBooleanDifference", [](Ctx& c) {
      const auto* m = ON_Mesh::Cast(c.Last());
      CHECK(m && std::fabs(MeshVolume(*m) - 875.0) < 1e-3);
    });
    Case("MeshBooleanIntersection", "_MeshBox 0,0 10,10 10\n_MeshBox 5,5,5 15,15,5 10\n_SelMesh _MeshBooleanIntersection", [](Ctx& c) {
      const auto* m = ON_Mesh::Cast(c.Last());
      CHECK(m && std::fabs(MeshVolume(*m) - 125.0) < 1e-3);
    });
    Case("RailRevolve", "_Line 0,0,10 5,0,0\n_Circle 0,0,0 10\n_RailRevolve 2.5,0,5 10,0,0 0,0,0 0,0,1", [](Ctx& c) {
      const auto* b = ON_Brep::Cast(c.Last());
      CHECK(b && b->IsValid());
      ON_BoundingBox bb;
      if (b) b->GetTightBoundingBox(bb);
      CHECK(b && Near(bb.m_max.x, 10, 0.05) && Near(bb.m_max.y, 10, 0.05) && Near(bb.m_max.z, 10, 1e-3));
    });
    Case("InterpCrvOnSrf", "_Plane 0,0 10,10\n_InterpCrvOnSrf 5,5 1,1 5,8 9,2 _Enter", [](Ctx& c) {
      const auto* cv = ON_Curve::Cast(c.Last());
      CHECK(cv && NearPt(cv->PointAtStart(), ON_3dPoint(1, 1, 0), 1e-4) && NearPt(cv->PointAtEnd(), ON_3dPoint(9, 2, 0), 1e-4));
    });
    Case("FitCrv", "_Circle 0,0 10\n_SelAll _FitCrv Tolerance=0.001", [](Ctx& c) {
      const auto* nc = ON_NurbsCurve::Cast(c.Last());
      CHECK(nc && !nc->IsRational() && nc->Degree() == 3);
      bool ok = nc != nullptr;
      for (int i = 0; nc && i <= 100; i++) ok = ok && std::fabs(nc->PointAt(nc->Domain().ParameterAt(i / 100.0)).DistanceTo(ON_origin) - 10) < 0.002;
      CHECK(ok);
    });
    Case("Make2D_front", "_Box 0,0,0 10,4 6\n_SelAll _Make2D View=Front", [](Ctx& c) {
      // il rettangolo della vista frontale: x 0..10, y = z 0..6, sul piano xy
      ON_BoundingBox bb;
      int curves = 0;
      for (auto* g : c.Objects()) if (const auto* cv = ON_Curve::Cast(g)) { ON_BoundingBox t; cv->GetTightBoundingBox(t); bb.Union(t); curves++; }
      CHECK(curves >= 1 && NearPt(bb.m_min, ON_3dPoint(0, 0, 0), 1e-6) && NearPt(bb.m_max, ON_3dPoint(10, 6, 0), 1e-6));
    });
    Case("Make2D_nascoste", "_Cylinder 0,0,0 5 10\n_SelAll _Make2D View=Top ShowHidden=Yes", [](Ctx& c) {
      int curves = 0;
      for (auto* g : c.Objects()) if (ON_Curve::Cast(g)) curves++;
      CHECK(curves >= 1);
    });
    Case("MoveFace", "_Box 0,0,0 10,10 10\n_MoveFace 10,5,5 0,0,0 5,0,0\n_SelAll _Volume", [](Ctx& c) {
      const std::string log = c.log.str();
      const auto p = log.rfind("Volume = ");
      CHECK(p != std::string::npos && Near(std::atof(log.c_str() + p + 9), 1500, 1e-3));
      const auto* b = ON_Brep::Cast(c.Last());
      CHECK(b && b->m_F.Count() == 6);
    });
    Case("MoveFace_dentro", "_Box 0,0,0 10,10 10\n_MoveFace 5,5,10 0,0,0 0,0,-4\n_SelAll _Volume", [](Ctx& c) {
      const std::string log = c.log.str();
      const auto p = log.rfind("Volume = ");
      CHECK(p != std::string::npos && Near(std::atof(log.c_str() + p + 9), 600, 1e-3));
    });
    Case("FlowAlongSrf", "_Plane 0,0 10,10\n_Plane 0,20 20,40\n_Point 5,2,1\n_SelPt _FlowAlongSrf 1,1 1,21", [](Ctx& c) {
      const auto* p = ON_Point::Cast(c.Last());
      CHECK(p && NearPt(p->point, ON_3dPoint(10, 24, 1), 1e-4));
    });
    Case("OrientOnSrf", "_Sphere 0,0,0 10\n_Point 0,0,1\n_SelPt _OrientOnSrf 0,0,0 10,0,0 10,0,0 _Enter", [](Ctx& c) {
      // il punto alto 1 sopra la base finisce a distanza 1 lungo la normale
      bool found = false;
      for (auto* g : c.Objects())
        if (const auto* p = ON_Point::Cast(g)) if (Near(p->point.DistanceTo(ON_origin), 11, 1e-4) || Near(p->point.DistanceTo(ON_origin), 9, 1e-4)) found = true;
      CHECK(found);
    });
    Case("Join_superfici", "_Plane 0,0 10,10\n_Plane 10,0 20,10\n_SelAll _Join", [](Ctx& c) {
      const auto* b = ON_Brep::Cast(c.Last());
      CHECK(c.Objects().size() == 1 && b && b->m_F.Count() == 2);
    });
    Case("Join_solido", "_Box 0,0,0 10,10 10\n_SelAll _Explode\n_SelAll _Join\n_SelAll _Volume", [](Ctx& c) {
      const auto* b = ON_Brep::Cast(c.Last());
      CHECK(c.Objects().size() == 1 && b && b->IsSolid());
      const std::string log = c.log.str();
      const auto p = log.rfind("Volume = ");
      CHECK(p != std::string::npos && Near(std::atof(log.c_str() + p + 9), 1000, 1e-3));
    });
    Case("Join_mesh", "_MeshBox 0,0 1,1 1\n_MeshBox 5,0 6,1 1\n_SelMesh _Join", [](Ctx& c) {
      const auto* m = ON_Mesh::Cast(c.Last());
      CHECK(c.Objects().size() == 1 && m && m->FaceCount() == 12);
    });
    Case("ArraySrf", "_Plane 0,0 10,10\n_Point 0,0,1\n_SelPt _ArraySrf 0,0,0 5,5 3 2", [](Ctx& c) {
      CHECK(c.Objects().size() == 2 + 6);
      bool found = false;
      for (auto* g : c.Objects())
        if (const auto* p = ON_Point::Cast(g)) if (NearPt(p->point, ON_3dPoint(10, 10, 1), 1e-6)) found = true;
      CHECK(found);
    });
  }
#endif

#ifdef CAD_WITH_OCCT
  // Superfici e solidi, seconda parte
  {
    std::printf("Superfici e solidi (2)\n");
    auto num = [](Ctx& c, const char* label) {
      const std::string log = c.log.str();
      auto p = log.rfind(label);
      return p == std::string::npos ? -1.0 : std::atof(log.c_str() + p + std::strlen(label));
    };
    Case("SrfPt", "_SrfPt 0,0 10,0 10,5 0,5\n_SelAll _Area", [&](Ctx& c) {
      CHECK(Near(num(c, "Area = "), 50.0, 1e-6));
    });
    Case("SrfPt_3", "_SrfPt 0,0 10,0 0,10 _Enter\n_SelAll _Area", [&](Ctx& c) {
      CHECK(Near(num(c, "Area = "), 50.0, 1e-4));
    });
    Case("Plane3Pt", "_Plane3Pt 0,0 10,0 3,4\n_SelAll _Area", [&](Ctx& c) {
      CHECK(Near(num(c, "Area = "), 40.0, 1e-6));
    });
    Case("PlaneVertical", "_PlaneVertical 0,0 3,4 2", [](Ctx& c) {
      CHECK(NearPt(c.BBox().m_max, ON_3dPoint(3, 4, 2), 1e-9));
    });
    Case("ExtrudeCrvToPoint", "_Rectangle 0,0 10,10\n_SelAll _ExtrudeCrvToPoint 5,5,10\n_SelNone _SelSrf _Area", [&](Ctx& c) {
      // 4 triangoli di base 10 e altezza obliqua sqrt(125)
      CHECK(Near(num(c, "Area = "), 2.0 * 10.0 * std::sqrt(125.0), 1e-3));
    });
    Case("ExtrudeCrvAlongCrv", "_Line 20,0,0 20,0,5\n_Circle 0,0 2\n_SelNone _SelLast _ExtrudeCrvAlongCrv 20,0,0.1\n_SelNone _SelSrf _Area", [&](Ctx& c) {
      CHECK(Near(num(c, "Area = "), 2.0 * ON_PI * 2.0 * 5.0, 1e-3));
      CHECK(NearPt(c.BBox().m_max, ON_3dPoint(20, 2, 5), 1e-6));
    });
    Case("UntrimAll", "_Box 0,0,0 10,10 10\n_Cylinder 5,5,-1 2 12\n_SelAll _BooleanDifference\n_SelAll _UntrimAll", [](Ctx& c) {
      int faces = 0, loops = 0;
      for (auto* g : c.Objects()) if (auto* b = ON_Brep::Cast(g)) { faces += b->m_F.Count(); loops += b->m_L.Count(); }
      CHECK(faces == 7 && loops == 7 && c.Objects().size() == 7);
    });
    Case("ShrinkTrimmedSrf", "_Box 0,0,0 10,10 10\n_SelAll _Explode\n_SelNone _SelSrf _ShrinkTrimmedSrf", [](Ctx& c) {
      CHECK(c.Objects().size() == 6);
    });
    Case("EdgeSrf_4", "_Line 0,0 10,0\n_Line 10,0 10,10\n_Line 10,10 0,10\n_Line 0,10 0,0\n_SelAll _EdgeSrf\n_SelNone _SelSrf _Area", [&](Ctx& c) {
      CHECK(Near(num(c, "Area = "), 100.0, 1e-3));
    });
    Case("EdgeSrf_2", "_Line 0,0 10,0\n_Line 0,10 10,10\n_SelAll _EdgeSrf\n_SelNone _SelSrf _Area", [&](Ctx& c) {
      CHECK(Near(num(c, "Area = "), 100.0, 1e-3));
    });
    Case("Patch", "_Circle 0,0 5\n_SelAll _Patch\n_SelNone _SelSrf _Area", [&](Ctx& c) {
      CHECK(Near(num(c, "Area = "), ON_PI * 25.0, 0.5));
    });
    Case("Pyramid", "_Pyramid NumSides=4 0,0,0 5,5 6\n_SelAll _Volume", [&](Ctx& c) {
      CHECK(ON_Brep::Cast(c.Last()) && ON_Brep::Cast(c.Last())->IsSolid());
      // base quadrata di diagonale 2*sqrt(50) -> lato 10, area 100
      CHECK(Near(num(c, "Volume = "), 100.0 * 6.0 / 3.0, 1e-3));
    });
    Case("MergeAllFaces", "_Box 0,0,0 10,10 10\n_Box 10,0,0 20,10 10\n_SelAll _BooleanUnion\n_SelAll _MergeAllFaces\n_SelAll _Volume", [&](Ctx& c) {
      const auto* b = ON_Brep::Cast(c.Last());
      CHECK(b && b->m_F.Count() == 6 && b->IsSolid());
      CHECK(Near(num(c, "Volume = "), 2000.0, 1e-6));
    });
    Case("ExtractWireframe", "_Sphere 0,0,0 5\n_SelAll _ExtractWireframe", [](Ctx& c) {
      int curves = 0;
      for (auto* g : c.Objects()) if (ON_Curve::Cast(g)) curves++;
      CHECK(curves > 4);
    });
    Case("ExtractIsocurve", "_Plane 0,0 10,10\n_SelAll _ExtractIsocurve Direction=Both 3,4,0 _Enter", [](Ctx& c) {
      std::vector<const ON_Curve*> cs;
      for (auto* g : c.Objects()) if (auto* cv = ON_Curve::Cast(g)) cs.push_back(cv);
      CHECK(cs.size() == 2);
      bool hasU = false, hasV = false;
      for (auto* cv : cs) {
        if (NearPt(cv->PointAtStart(), ON_3dPoint(0, 4, 0), 1e-6) && NearPt(cv->PointAtEnd(), ON_3dPoint(10, 4, 0), 1e-6)) hasU = true;
        if (NearPt(cv->PointAtStart(), ON_3dPoint(3, 0, 0), 1e-6) && NearPt(cv->PointAtEnd(), ON_3dPoint(3, 10, 0), 1e-6)) hasV = true;
      }
      CHECK(hasU && hasV);
    });
    Case("ExtractIsocurve_tagliata", "_Box 0,0,0 10,10 10\n_Cylinder 5,5,-1 2 12\n_SelAll _BooleanDifference\n_SelAll _ExtractIsocurve Direction=U 5,1,10 _Enter", [](Ctx& c) {
      // la linea y=1 sulla faccia superiore non tocca il foro: un solo pezzo lungo 10
      double L = 0;
      int n = 0;
      for (auto* g : c.Objects()) if (auto* cv = ON_Curve::Cast(g)) { L += Len(cv); n++; }
      CHECK(n == 1 && Near(L, 10.0, 1e-3));
    });
    Case("ExtractIsocurve_foro", "_Box 0,0,0 10,10 10\n_Cylinder 5,5,-1 2 12\n_SelAll _BooleanDifference\n_SelAll _ExtractIsocurve Direction=U 1,5,10 _Enter", [](Ctx& c) {
      // la linea y=5 attraversa il foro di raggio 2: due pezzi lunghi 3
      double L = 0;
      int n = 0;
      for (auto* g : c.Objects()) if (auto* cv = ON_Curve::Cast(g)) { L += Len(cv); n++; }
      CHECK(n == 2 && Near(L, 6.0, 1e-3));
    });
    Case("RemoveKnot", "_Curve 0,0 5,5 10,0 15,5 20,0 25,5 _Enter\n_SelAll _RemoveKnot 12,2 _Enter", [](Ctx& c) {
      const auto* n = ON_NurbsCurve::Cast(c.Last());
      CHECK(n && n->CVCount() == 5 && NearPt(n->PointAtEnd(), ON_3dPoint(25, 5, 0), 1e-9));
    });
    Case("Wedge", "_Wedge 0,0,0 10,4 3\n_SelAll _Volume", [&](Ctx& c) {
      CHECK(Near(num(c, "Volume = "), 10.0 * 4.0 * 3.0 / 2.0, 1e-6));
    });
    // --- v0.6 ---
    Case("FilletSrf", "_Plane 0,0,0 10,10\n_PlaneVertical 0,0,0 0,10,0 10\n_FilletSrf Radius=2 5,5,0 0,5,5\n_SelNone _SelLast _Area", [&](Ctx& c) {
      CHECK(Near(num(c, "Area = "), ON_PI * 2.0 / 2.0 * 10.0, 1e-3));
    });
    Case("ChamferSrf", "_Plane 0,0,0 10,10\n_PlaneVertical 0,0,0 0,10,0 10\n_ChamferSrf Distance=2 5,5,0 0,5,5\n_SelNone _SelLast _Area", [&](Ctx& c) {
      CHECK(Near(num(c, "Area = "), std::sqrt(8.0) * 10.0, 1e-3));
    });
    Case("BlendSrf", "_Plane 0,0,0 10,10\n_Plane 15,0,5 25,10\n_BlendSrf 10,5,0 15,5,5", [](Ctx& c) {
      const auto* b = ON_Brep::Cast(c.Last());
      CHECK(b && b->m_F.Count() == 1);
      if (b) {
        ON_BoundingBox bb = b->BoundingBox();
        CHECK(Near(bb.m_min.x, 10, 1e-3) && Near(bb.m_max.x, 15, 1e-3) && Near(bb.m_min.z, 0, 1e-3) && Near(bb.m_max.z, 5, 1e-3));
      }
    });
    Case("UnrollSrf_cilindro", "_Cylinder 0,0,0 5 10\n_SelAll _UnrollSrf\n_SelNone _SelLast _Area", [&](Ctx& c) {
      CHECK(Near(num(c, "Area = "), 2 * ON_PI * 5 * 10 + 2 * ON_PI * 25, 1.0));
    });
    Case("UnrollSrf_cono", "_Cone 0,0,0 5 10\n_SelAll _UnrollSrf\n_SelNone _SelLast _Area", [&](Ctx& c) {
      CHECK(Near(num(c, "Area = "), ON_PI * 5 * std::sqrt(125.0) + ON_PI * 25, 1.0));
    });
    Case("Silhouette", "_Sphere 0,0,0 5\n_SelAll _Silhouette View=Top\n_SelNone _SelCrv _Length", [&](Ctx& c) {
      CHECK(Near(num(c, "Lunghezza = "), 2 * ON_PI * 5, 0.1));
    });
    Case("RebuildCrvNonUniform", "_InterpCrv 0,0 3,2 6,-1 10,0 _Enter\n_SelAll _RebuildCrvNonUniform RequestedTolerance=0.001", [](Ctx& c) {
      const auto* n = ON_NurbsCurve::Cast(c.Last());
      CHECK(n && NearPt(n->PointAtStart(), ON_3dPoint(0, 0, 0), 1e-3) && NearPt(n->PointAtEnd(), ON_3dPoint(10, 0, 0), 1e-3));
    });
    Case("ExtendCrvOnSrf", "_Plane 0,0,0 10,10\n_Line 2,2,0 5,5,0\n_ExtendCrvOnSrf 5,5,0 1,8,0\n_SelNone _SelCrv _Length", [&](Ctx& c) {
      const double L = num(c, "Lunghezza = ");
      if (!Near(L, 8 * std::sqrt(2.0), 1e-3)) std::printf("  lunghezza ottenuta %.9f\n", L);
      CHECK(Near(L, 8 * std::sqrt(2.0), 1e-3));
    });
    Case("OffsetCrvOnSrf", "_Plane 0,0,0 10,10\n_Line 2,2,0 8,2,0\n_OffsetCrvOnSrf Distance=3 5,2,0 1,8,0 5,9,0", [](Ctx& c) {
      const auto* cv = ON_Curve::Cast(c.Last());
      CHECK(cv && NearPt(cv->PointAtStart(), ON_3dPoint(2, 5, 0), 1e-3) && NearPt(cv->PointAtEnd(), ON_3dPoint(8, 5, 0), 1e-3));
    });
  }
#endif

  // --- v0.6 (solo openNURBS) ---
  Case("MarkCurvature", "_Circle 0,0 5\n_MarkCurvature 5,0,0", [](Ctx& c) {
    const auto* a = ON_ArcCurve::Cast(c.Last());
    CHECK(a && Near(a->Radius(), 5.0, 1e-6));
  });
  Case("Dir_Flip", "_Line 0,0 10,0\n_SelAll _Dir Flip=Yes", [](Ctx& c) {
    const auto* cv = ON_Curve::Cast(c.Last());
    CHECK(cv && NearPt(cv->PointAtStart(), ON_3dPoint(10, 0, 0)));
  });
  Case("Seam", "_Circle 0,0 5\n_Seam 0,5,0", [](Ctx& c) {
    const auto* cv = ON_Curve::Cast(c.Last());
    CHECK(cv && NearPt(cv->PointAtStart(), ON_3dPoint(0, 5, 0), 1e-6));
  });
  Case("InsertControlPoint", "_Curve 0,0 5,5 10,0 15,5 _Enter\n_InsertControlPoint 7,2.5,0 _Enter", [](Ctx& c) {
    const auto* n = ON_NurbsCurve::Cast(c.Last());
    CHECK(n && n->CVCount() == 5);
  });
  Case("MakePeriodic", "_Rectangle 0,0 10,10\n_SelAll _MakePeriodic", [](Ctx& c) {
    const auto* n = ON_NurbsCurve::Cast(c.Last());
    CHECK(n && n->IsPeriodic() && n->IsClosed());
  });
  Case("Fair", "_InterpCrv 0,0 2,1 4,-1 6,1.5 8,-1 10,0 _Enter\n_SelAll _Fair Tolerance=0.5", [](Ctx& c) {
    const auto* n = ON_NurbsCurve::Cast(c.Last());
    CHECK(n && NearPt(n->PointAtStart(), ON_3dPoint(0, 0, 0), 1e-9) && NearPt(n->PointAtEnd(), ON_3dPoint(10, 0, 0), 1e-9));
  });
  Case("SubDLoft_Fill", "_Circle 0,0,0 5\n_Circle 0,0,10 5\n_SelAll _SubDLoft\n_SelNone _SelSubD _Fill", [](Ctx& c) {
    const ON_SubD* s = nullptr;
    for (auto* g : c.Objects()) if (auto* x = ON_SubD::Cast(g)) s = x;
    CHECK(s && s->FaceCount() > 0);
    CHECK(c.log.str().find("Chiusi 2 fori") != std::string::npos);
  });
  Case("MeshPatch", "_Point 0,0,0\n_Point 10,0,0\n_Point 10,10,1\n_Point 0,10,0\n_Point 5,5,3\n_SelAll _MeshPatch", [](Ctx& c) {
    const auto* m = ON_Mesh::Cast(c.Last());
    CHECK(m && m->FaceCount() == 4 && m->VertexCount() == 5);
  });
  Case("Measure", "_Line 0,0 10,0\n_SelAll _Measure 2.5", [](Ctx& c) {
    int pts = 0;
    for (auto* g : c.Objects()) if (ON_Point::Cast(g)) pts++;
    CHECK(pts == 5);
  });
  Case("ToNURBS", "_Cylinder 0,0,0 5 10\n_SelAll _ToNURBS", [](Ctx& c) {
    CHECK(ON_Brep::Cast(c.Last()) != nullptr);
  });

  // --- v0.7 ---
  Case("HandleCurve", "_HandleCurve 0,0 3,0 10,0 13,0 _Enter", [](Ctx& c) {
    const auto* n = ON_NurbsCurve::Cast(c.Last());
    CHECK(n && n->CVCount() == 4 && Near(Len(n), 10.0, 1e-6));
  });
  Case("HandleCurve_tangenza", "_HandleCurve 0,0 0,5 10,0 13,3 20,0 20,-5 _Enter", [](Ctx& c) {
    const auto* n = ON_NurbsCurve::Cast(c.Last());
    CHECK(n && n->CVCount() == 7);
    if (n) {
      ON_3dVector t0 = n->TangentAt(n->Domain().ParameterAt(0.5 - 1e-7)), t1 = n->TangentAt(n->Domain().ParameterAt(0.5 + 1e-7));
      CHECK(t0 * t1 > 0.9999);
      CHECK(NearPt(n->PointAt(n->Domain().ParameterAt(0.5)), ON_3dPoint(10, 0, 0), 1e-9));
    }
  });
  Case("Reflect", "_SubDBox 0,0,0 10,10 10\n_SelAll _Reflect Axis=X Origin=0", [](Ctx& c) {
    const auto* s = ON_SubD::Cast(c.Last());
    CHECK(s && s->FaceCount() == 10);
    const ON_BoundingBox b = c.BBox();
    CHECK(Near(b.m_min.x, -10.0, 1e-6) && Near(b.m_max.x, 10.0, 1e-6));
  });
  Case("SubDExtrude", "_SubDBox 0,0,0 10,10 10\n_SubDExtrude 5,5,10 5", [](Ctx& c) {
    const auto* s = ON_SubD::Cast(c.Last());
    CHECK(s && s->FaceCount() == 10);
    CHECK(Near(c.BBox().m_max.z, 15.0, 1.0));
  });
  Case("InsertEdge", "_SubDBox 0,0,0 10,10 10\n_InsertEdge 5,0,0", [](Ctx& c) {
    const auto* s = ON_SubD::Cast(c.Last());
    CHECK(s && s->FaceCount() == 10);
  });
  Case("Stitch", "_SubDPlane 0,0 10,10\n_SubDPlane 10,0 20,10\n_SelAll _Stitch", [](Ctx& c) {
    CHECK(c.Objects().size() == 1);
    const auto* s = ON_SubD::Cast(c.Last());
    CHECK(s && s->FaceCount() == 2 && s->VertexCount() == 6);
  });
  Case("Append", "_SubDPlane 0,0 10,10\n_Append 10,0,0 15,0,0 15,10,0 10,10,0 _Enter", [](Ctx& c) {
    const auto* s = ON_SubD::Cast(c.Last());
    CHECK(s && s->FaceCount() == 2 && s->VertexCount() == 6);
  });
  Case("Bridge", "_SubDPlane 0,0 10,10\n_SubDPlane 0,0,10 10,10,10\n_Bridge Segments=2 0,5,0 0,5,10", [](Ctx& c) {
    CHECK(c.Objects().size() == 1);
    const auto* s = ON_SubD::Cast(c.Last());
    CHECK(s && s->FaceCount() == 10);
  });
  Case("SubDSweep", "_Line 0,0,0 0,0,20\n_Circle 0,0,0 3\n_SelAll _SubDSweep", [](Ctx& c) {
    const ON_SubD* s = nullptr;
    for (auto* g : c.Objects()) if (auto* x = ON_SubD::Cast(g)) s = x;
    CHECK(s && s->FaceCount() > 8);
    if (s) {
      const ON_BoundingBox b = s->BoundingBox();
      CHECK(b.m_max.z > 15 && b.m_max.x < 3.5);
    }
  });
  Case("EditPtOn", "_Curve 0,0 3,5 7,-2 10,0 _Enter\n_SelAll _EditPtOn", [](Ctx& c) {
    CHECK(c.e.editPointsOn.size() == 1);
  });
  Case("DimStyle", "_DimStyle Name=\"Mio Stile\" TextHeight=3.5 Precision=1\n_Dim 0,0 40,5 20,-8", [](Ctx& c) {
    const ON_DimStyle& ds = *ON_DimStyle::FromModelComponentRef(c.e.Model().CurrentDimensionStyle(), &ON_DimStyle::Default);
    CHECK(ON_wString(ds.Name()) == L"Mio Stile");
    CHECK(Near(ds.TextHeight(), 3.5));
    CHECK(ds.LengthResolution() == 1);
  });
  {
    const std::string pgm = g_outDir + "/heightfield.pgm";
    std::ofstream(pgm) << "P2\n2 2\n255\n255 255\n0 0\n";
    Case("Heightfield", "_Heightfield \"" + pgm + "\" 0,0,0 10,10,0 4 PointCount=5x5", [](Ctx& c) {
      const auto* b = ON_Brep::Cast(c.Last());
      CHECK(b && b->m_F.Count() == 1);
      const ON_BoundingBox bb = c.BBox();
      CHECK(Near(bb.m_max.z, 4.0, 1e-6) && Near(bb.m_min.z, 0.0, 1e-6));
      if (b) {
        // la riga in alto dell'immagine (bianca) sta a y=10
        const ON_Surface* s = b->m_S[0];
        ON_3dPoint top = s->PointAt(s->Domain(0).Mid(), s->Domain(1)[1]);
        ON_3dPoint bottom = s->PointAt(s->Domain(0).Mid(), s->Domain(1)[0]);
        if (top.y < bottom.y) std::swap(top, bottom);
        CHECK(top.z > bottom.z);
      }
    });
  }
  Case("TextObject_senza_finestra", "_TextObject \"A\" 0,0,0", [](Ctx&) {}, false);
  Case("BlockEdit", "_Box 0,0,0 2,2 2\n_SelAll _Block Name=B 0,0,0\n_SelAll _Move 0,0,0 10,0,0\n_SelAll _BlockEdit\n_Move 0,0,0 0,0,5\n_BlockEdit", [](Ctx& c) {
    CHECK(c.Objects().size() == 1);
    CHECK(ON_InstanceRef::Cast(c.Last()) != nullptr);
    const ON_BoundingBox b = c.BBox();
    CHECK(Near(b.m_min.x, 10.0, 1e-6) && Near(b.m_min.z, 5.0, 1e-6) && Near(b.m_max.z, 7.0, 1e-6));
  });
  // --- v0.8 ---
  Case("Bevel", "_SubDBox 0,0,0 10,10 10\n_Bevel Distance=2 5,0,0 _Enter", [](Ctx& c) {
    const auto* s = ON_SubD::Cast(c.Last());
    CHECK(s && s->FaceCount() == 7 && s->VertexCount() == 10);
  });
  Case("Bevel_due_spigoli", "_SubDBox 0,0,0 10,10 10\n_Bevel Distance=1 5,0,0 5,10,10 _Enter", [](Ctx& c) {
    const auto* s = ON_SubD::Cast(c.Last());
    CHECK(s && s->FaceCount() == 8);
  });
  Case("MultiPipe", "_Line 0,0,0 10,0,0\n_Line 10,0,0 10,10,0\n_Line 50,0,0 50,0,10\n_SelAll _MultiPipe Radius=1", [](Ctx& c) {
    int subds = 0;
    for (auto* g : c.Objects()) if (ON_SubD::Cast(g)) subds++;
    CHECK(subds == 2);  // le prime due linee si toccano: un solo tubo
    const ON_SubD* s = nullptr;
    for (auto* g : c.Objects()) if (auto* x = ON_SubD::Cast(g)) { ON_BoundingBox b = x->BoundingBox(); if (b.m_min.x > 40) s = x; }
    CHECK(s != nullptr);
    if (s) {
      // gabbia di 4 lati: raggio 1,5 perche' il limite passi a 1 dall'asse
      const ON_BoundingBox b = s->BoundingBox();
      CHECK(Near(b.m_max.x - 50.0, 1.5, 1e-6) && Near(b.m_min.x - 50.0, -1.5, 1e-6));
    }
  });
  Case("CageEdit", "_Box 0,0,0 10,10 10\n_SelAll _CageEdit PointCount=2,2,2 Degree=1", [](Ctx& c) {
    const ON_MorphControl* mc = nullptr;
    ON_UUID cid = ON_nil_uuid;
    for (const auto& id : c.e.AllObjectIds(true))
      if (auto* m = ON_MorphControl::Cast(c.e.Object(id)->Geometry(nullptr))) { mc = m; cid = id; }
    CHECK(mc && mc->m_captive_id.Count() == 1 && c.e.pointsOn.count(cid) == 1);
    if (!mc) return;
    char s[64];
    ON_UuidToString(cid, s);
    // angolo (10,10,10) = indice 7: portato a (20,20,20) la scatola si allunga
    CHECK(c.e.Run(std::string("_GripMove ") + s + " 7 -3 20,20,20"));
    ON_BoundingBox b;
    for (const auto& id : c.e.AllObjectIds(true)) {
      const ON_Geometry* g = c.e.Object(id)->Geometry(nullptr);
      if (!ON_MorphControl::Cast(g)) g->GetTightBoundingBox(b, true);
    }
    CHECK(Near(b.m_max.x, 20.0, 1e-3) && Near(b.m_max.z, 20.0, 1e-3) && Near(b.m_min.x, 0.0, 1e-3));
    // seconda modifica dallo stato originale (non cumulativa)
    CHECK(c.e.Run(std::string("_GripMove ") + s + " 7 -3 10,10,10"));
    ON_BoundingBox b2;
    for (const auto& id : c.e.AllObjectIds(true)) {
      const ON_Geometry* g = c.e.Object(id)->Geometry(nullptr);
      if (!ON_MorphControl::Cast(g)) g->GetTightBoundingBox(b2, true);
    }
    CHECK(Near(b2.m_max.x, 10.0, 1e-3));
  });
#ifdef CAD_WITH_OCCT
  Case("Sketch", "_Sketch 0,0 1,1 2,1.5 3,1 4,0 5,-1 _Enter", [](Ctx& c) {
    const auto* cv = ON_Curve::Cast(c.Last());
    CHECK(cv && NearPt(cv->PointAtStart(), ON_3dPoint(0, 0, 0), 1e-3) && NearPt(cv->PointAtEnd(), ON_3dPoint(5, -1, 0), 1e-3));
  });
  Case("DevLoft", "_Curve 0,0 3,5 7,-2 10,0 _Enter\n_Curve 0,10,3 4,12,4 10,10,2 _Enter\n_SelAll _DevLoft", [](Ctx& c) {
    const auto* b = ON_Brep::Cast(c.Last());
    CHECK(b && b->m_F.Count() == 1);
    if (b) {
      // gli angoli della superficie sono gli estremi delle due curve
      const ON_Surface* sf = b->m_S[0];
      const ON_Interval du = sf->Domain(0), dv = sf->Domain(1);
      const ON_3dPoint want[4] = {ON_3dPoint(0, 0, 0), ON_3dPoint(10, 0, 0), ON_3dPoint(0, 10, 3), ON_3dPoint(10, 10, 2)};
      for (const auto& w : want) {
        bool found = false;
        for (int i = 0; i < 2; i++) for (int j = 0; j < 2; j++) found = found || NearPt(sf->PointAt(du[i], dv[j]), w, 1e-6);
        CHECK(found);
      }
    }
  });
  Case("QuadRemesh", "_Plane 0,0,0 10,10\n_SelAll _QuadRemesh TargetQuadCount=100", [](Ctx& c) {
    const auto* m = ON_Mesh::Cast(c.Last());
    CHECK(m && m->QuadCount() == m->FaceCount() && m->FaceCount() == 100);
  });
  Case("Splop", "_Sphere 0,0,0 10\n_Box 30,0,0 32,2 1\n_SelNone _SelLast _Splop 31,1,0 0,0,10", [](Ctx& c) {
    CHECK(c.Objects().size() == 3);
    ON_BoundingBox b;
    for (auto* g : c.Objects()) { ON_BoundingBox t; g->GetTightBoundingBox(t); if (t.m_min.z > 5) b = t; }
    // la copia sta in cima alla sfera, alta 1 lungo la normale
    CHECK(Near(b.m_max.z, 11.0, 0.05) && b.m_min.z > 9.7 && std::fabs(b.Center().x) < 1.5 && std::fabs(b.Center().y) < 1.5);
  });
  Case("MatchSrf", "_Plane 0,0,0 10,10\n_PlaneVertical 12,0,0 12,10,0 10\n_MatchSrf Continuity=Position 10,5,0 12,5,0", [](Ctx& c) {
    const ON_Brep* moved = nullptr;
    for (auto* g : c.Objects()) if (auto* b = ON_Brep::Cast(g)) { ON_BoundingBox bb; b->GetTightBoundingBox(bb); if (bb.m_max.z < 1e-6) moved = b; }
    CHECK(moved != nullptr);
    if (moved) { ON_BoundingBox bb; moved->GetTightBoundingBox(bb); CHECK(Near(bb.m_max.x, 12.0, 1e-3)); }
  });
#endif

  // --- Assistente AI: istruzioni, lettura delle risposte, verifica, scelta del modello ---
  {
    std::printf("AI_esempi_delle_istruzioni\n");
    Ctx c;
    for (const auto& line : ai::ReferenceExamples()) {
      const auto v = ai::Validate(c.e, {line});
      if (!v.ok) std::printf("  %s -> %s\n", line.c_str(), v.error.c_str());
      CHECK(v.ok && v.created >= 1);
    }
    // gli script di esempio nelle istruzioni devono funzionare
    const std::string sp = ai::SystemPrompt();
    int blocks = 0;
    for (size_t a = 0; (a = sp.find("\n```kurva\n", a)) != std::string::npos; a += 8) {
      const size_t b = sp.find("```", a + 9);
      const auto lines = ai::ExtractScript(sp.substr(a, b + 3 - a));
      const auto v = ai::Validate(c.e, lines);
      if (!v.ok) std::printf("  esempio %d, riga %d: %s\n", blocks + 1, v.line + 1, v.error.c_str());
      CHECK(v.ok && v.created == 1);
      blocks++;
    }
    CHECK(blocks == 3);
    CHECK(c.e.AllObjectIds(true).empty());  // la verifica non tocca il documento
  }
  {
    std::printf("Scambio_file\n");
    namespace fs = std::filesystem;
    const std::string dir = g_outDir + "/scambio";
    fs::create_directories(dir);
    auto bboxOf = [](Ctx& c) { return c.BBox(); };
    auto nearBox = [](const ON_BoundingBox& a, const ON_BoundingBox& b, double t) {
      return a.m_min.DistanceTo(b.m_min) < t && a.m_max.DistanceTo(b.m_max) < t;
    };
    auto count = [](Ctx& c, auto pred) { int n = 0; for (auto* g : c.Objects()) if (pred(g)) n++; return n; };
    Ctx src;
    CHECK(src.e.Run("_Box 0,0,0 10,20 5\n_Sphere 30,0,0 4"));
    const ON_BoundingBox b0 = bboxOf(src);
    for (const char* ext : {"stp", "igs", "stl", "obj", "dxf"}) {
      const std::string f = dir + "/solidi." + ext;
      CHECK(src.e.Run(std::string("_SelAll\n_Export \"") + f + "\""));
      Ctx back;
      const bool ok = back.e.Run(std::string("_Import \"") + f + "\"");
      if (!ok) std::printf("  import %s: %s\n", ext, back.log.str().c_str());
      CHECK(ok);
      const ON_BoundingBox b1 = bboxOf(back);
      const bool exact = std::string(ext) == "stp" || std::string(ext) == "igs";
      if (!nearBox(b0, b1, exact ? 1e-3 : 0.05)) std::printf("  bbox %s: %g %g %g - %g %g %g\n", ext, b1.m_min.x, b1.m_min.y, b1.m_min.z, b1.m_max.x, b1.m_max.y, b1.m_max.z);
      CHECK(nearBox(b0, b1, exact ? 1e-3 : 0.05));
      if (exact) CHECK(count(back, [](const ON_Geometry* g) { return ON_Brep::Cast(g) != nullptr; }) >= 2);
      else CHECK(count(back, [](const ON_Geometry* g) { return ON_Mesh::Cast(g) != nullptr; }) >= 1);
    }
    // STEP: i solidi restano solidi chiusi
    {
      Ctx back;
      CHECK(back.e.Run("_Import \"" + dir + "/solidi.stp\""));
      int solids = 0;
      for (auto* g : back.Objects()) if (auto* b = ON_Brep::Cast(g); b && b->IsSolid()) solids++;
      CHECK(solids == 2);
    }
    // STL ASCII e mesh chiusa
    {
      CHECK(src.e.Run("_SelAll\n_Export \"" + dir + "/ascii.stl\" Ascii"));
      std::ifstream f(dir + "/ascii.stl");
      std::string head;
      std::getline(f, head);
      CHECK(head.rfind("solid", 0) == 0);
      Ctx back;
      CHECK(back.e.Run("_Import \"" + dir + "/ascii.stl\""));
      const ON_Mesh* m = ON_Mesh::Cast(back.Last());
      CHECK(m && m->IsClosed());
    }
    // DXF: curve, punti e livelli
    {
      Ctx c;
      CHECK(c.e.Run("-_Layer New Muri\n-_Layer Current Muri\n_Line 0,0,0 10,0,0\n_Circle 20,0,0 5\n_Arc 40,0,0 5,0,0 0,5,0\n"
                    "_Polyline 0,10,0 10,10,0 10,20,0 _Enter\n_Point 1,2,3\n_InterpCrv 0,30,0 10,35,0 20,30,0 _Enter"));
      const std::string f = dir + "/curve.dxf";
      CHECK(c.e.Run("_SelAll\n_Export \"" + f + "\""));
      Ctx back;
      CHECK(back.e.Run("_Import \"" + f + "\""));
      CHECK(count(back, [](const ON_Geometry* g) { return ON_LineCurve::Cast(g) != nullptr; }) == 1);
      CHECK(count(back, [](const ON_Geometry* g) { return ON_ArcCurve::Cast(g) != nullptr; }) == 2);
      CHECK(count(back, [](const ON_Geometry* g) { return ON_Point::Cast(g) != nullptr; }) == 1);
      CHECK(count(back, [](const ON_Geometry* g) { return ON_PolyCurve::Cast(g) != nullptr || ON_PolylineCurve::Cast(g) != nullptr; }) == 2);
      CHECK(nearBox(c.BBox(), back.BBox(), 1e-6));
      bool layer = false;
      for (const auto& id : back.e.AllObjectIds(true)) {
        const auto* l = ON_Layer::Cast(back.e.Model().LayerFromIndex(back.e.Object(id)->Attributes(nullptr)->m_layer_index).ModelComponent());
        if (l && ToUtf8(l->Name()) == "Muri") layer = true;
      }
      CHECK(layer);
    }
    // DXF scritto a mano: LWPOLYLINE con rigonfiamento (semicerchio), SPLINE, 3DFACE, unita' in metri
    {
      const std::string f = dir + "/mano.dxf";
      std::ofstream o(f);
      o << "0\nSECTION\n2\nHEADER\n9\n$INSUNITS\n70\n6\n0\nENDSEC\n0\nSECTION\n2\nENTITIES\n"
           "0\nLWPOLYLINE\n8\nA\n90\n2\n70\n0\n10\n0\n20\n0\n42\n1\n10\n2\n20\n0\n"
           "0\nSPLINE\n8\nA\n70\n8\n71\n3\n72\n8\n73\n4\n40\n0\n40\n0\n40\n0\n40\n0\n40\n1\n40\n1\n40\n1\n40\n1\n"
           "10\n0\n20\n5\n30\n0\n10\n1\n20\n6\n30\n0\n10\n2\n20\n4\n30\n0\n10\n3\n20\n5\n30\n0\n"
           "0\n3DFACE\n8\nB\n10\n0\n20\n0\n30\n1\n11\n1\n21\n0\n31\n1\n12\n1\n22\n1\n32\n1\n13\n0\n23\n1\n33\n1\n"
           "0\nENDSEC\n0\nEOF\n";
      o.close();
      Ctx back;  // documento in millimetri: metri x 1000
      CHECK(back.e.Run("_Import \"" + f + "\""));
      const ON_Curve* arc = nullptr;
      const ON_NurbsCurve* nc = nullptr;
      const ON_Mesh* m = nullptr;
      for (auto* g : back.Objects()) {
        if (auto* a = ON_ArcCurve::Cast(g)) arc = a;
        if (auto* n = ON_NurbsCurve::Cast(g)) nc = n;
        if (auto* mm = ON_Mesh::Cast(g)) m = mm;
      }
      // semicerchio di raggio 1 m sotto la corda (rigonfiamento positivo: antiorario)
      CHECK(arc && std::fabs(Len(arc) - ON_PI * 1000) < 1e-3);
      if (arc) CHECK(std::fabs(arc->PointAt(arc->Domain().Mid()).y + 1000) < 1e-6);
      CHECK(nc && nc->Order() == 4 && nc->CVCount() == 4 && std::fabs(nc->PointAtEnd().x - 3000) < 1e-6);
      CHECK(m && m->m_F.Count() == 1 && m->m_F[0].IsQuad());
    }
    // formato sconosciuto ed errori
    Ctx bad;
    CHECK(!bad.e.Run("_Import \"" + dir + "/manca.stl\""));
  }
  {
    std::printf("Texture\n");
    namespace fs = std::filesystem;
    const std::string dir = g_outDir + "/texture";
    fs::create_directories(dir);
    const std::string img = dir + "/legno chiaro.png";
    { std::ofstream f(img, std::ios::binary); f << "immagine di prova 0123456789"; }
    Ctx c;
    CHECK(c.e.Run("_Box 0,0,0 10,20 5\n_SubDBox 30,0,0 40,10 10\n_SelAll\n_ApplyTexture File= \"" + img + "\" Repeat=2"));
    const auto ids = c.e.AllObjectIds(true);
    ObjectTexture t0, t1;
    // openNURBS su Windows salva il percorso con le barre rovesciate
    auto same = [](std::string a, std::string b) {
      for (auto* x : {&a, &b}) for (auto& ch : *x) if (ch == '\\') ch = '/';
      return a == b;
    };
    CHECK(ids.size() == 2 && GetObjectTexture(c.e.Model(), *c.e.Object(ids[0])->Attributes(nullptr), t0) &&
          GetObjectTexture(c.e.Model(), *c.e.Object(ids[1])->Attributes(nullptr), t1));
    CHECK(same(t0.file, img) && t0.uvw[0][0] == 2.0 && t0.hasMapping && t1.hasMapping);
    // solido NURBS: parametri della superficie; SubD: box
    CHECK(t0.mapping.m_type == ON_TextureMapping::TYPE::srfp_mapping);
    CHECK(t1.mapping.m_type == ON_TextureMapping::TYPE::box_mapping);
    // un solo materiale anche applicando di nuovo la stessa immagine
    CHECK(c.e.Run("_SelAll\n_ApplyTexture File= \"" + img + "\" Repeat=2 Mapping=Planar"));
    int nmat = 0;
    { ONX_ModelComponentIterator it(c.e.Model(), ON_ModelComponent::Type::RenderMaterial);
      for (auto* m = it.FirstComponent(); m; m = it.NextComponent()) nmat++; }
    CHECK(nmat == 1);
    CHECK(GetObjectTexture(c.e.Model(), *c.e.Object(ids[0])->Attributes(nullptr), t0) &&
          t0.mapping.m_type == ON_TextureMapping::TYPE::plane_mapping);
    // la mappatura piana porta l'ingombro dell'oggetto in 0..1
    ON_3dPoint T;
    t0.mapping.Evaluate(ON_3dPoint(10, 20, 5), ON_3dVector::ZAxis, &T);
    CHECK(std::fabs(T.x - 1) < 1e-6 && std::fabs(T.y - 1) < 1e-6);
    CHECK(c.e.Run("_SelAll\n_ApplyCylindricalMapping"));
    CHECK(GetObjectTexture(c.e.Model(), *c.e.Object(ids[1])->Attributes(nullptr), t1) &&
          t1.mapping.m_type == ON_TextureMapping::TYPE::cylinder_mapping);
    // salvataggio: materiale, mappatura e immagine incorporata restano nel .3dm
    const std::string path = dir + "/texture.3dm";
    CHECK(c.doc.Save(path));
    fs::remove(img);
    Document3dm back;
    CHECK(back.Load(path));
    ObjectTexture tb;
    const auto* ob = back.Model().ComponentFromId(ON_ModelComponent::Type::ModelGeometry, ids[1]).ModelComponent();
    const auto* mgc = ON_ModelGeometryComponent::Cast(ob);
    CHECK(mgc && GetObjectTexture(back.Model(), *mgc->Attributes(nullptr), tb) && same(tb.file, img) &&
          tb.hasMapping && tb.mapping.m_type == ON_TextureMapping::TYPE::cylinder_mapping);
    const std::string res = ResolveTextureFile(back.Model(), img, dir + "/cache");
    std::ifstream rf(res, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(rf)), std::istreambuf_iterator<char>());
    CHECK(!res.empty() && bytes == "immagine di prova 0123456789");
    CHECK(c.e.Run("_SelAll\n_RemoveTexture"));
    CHECK(!GetObjectTexture(c.e.Model(), *c.e.Object(ids[0])->Attributes(nullptr), t0));
    Ctx bad;
    CHECK(!bad.e.Run("_Box 0,0,0 1,1 1\n_SelAll\n_ApplyTexture File=nessuna.png"));
  }
  {
    std::printf("Viste_e_sezioni\n");
    namespace fs = std::filesystem;
    const std::string dir = g_outDir + "/viste";
    fs::create_directories(dir);
    Ctx c;
    // finta finestra: una vista con il suo CPlane
    ON_3dmView cur;
    cur.m_vp.ChangeToParallelProjection(true);
    cur.m_vp.SetCameraLocation(ON_3dPoint(0, 0, 100));
    cur.m_vp.SetCameraDirection(-ON_3dVector::ZAxis);
    cur.m_vp.SetCameraUp(ON_3dVector::YAxis);
    cur.m_vp.SetFrustum(-10, 10, -10, 10, 1, 1000);
    cur.m_cplane.m_plane = ON_Plane::World_xy;
    cur.m_name = L"Top";
    c.e.getActiveView = [&](ON_3dmView& v) { v = cur; return true; };
    c.e.setActiveView = [&](const ON_3dmView& v) { const ON_wString n = cur.m_name; cur = v; cur.m_name = n; return true; };
    c.e.setActiveCPlane = [&](const ON_Plane* p) { cur.m_cplane.m_plane = p ? *p : ON_Plane::World_xy; return true; };
    ON_UUID vp1, vp2;
    ON_CreateUuid(vp1);
    ON_CreateUuid(vp2);
    c.e.activeViewportId = vp1;
    c.e.viewportIds = {vp1, vp2};
    // CPlane
    CHECK(c.e.Run("_CPlane World Front"));
    CHECK(cur.m_cplane.m_plane.zaxis.IsParallelTo(ON_3dVector::YAxis) != 0);
    CHECK(c.e.Run("_CPlane Reset\n_CPlane Elevation 5"));
    CHECK(Near(cur.m_cplane.m_plane.origin.z, 5));
    CHECK(c.e.Run("_CPlane Origin 1,2,3"));
    CHECK(NearPt(cur.m_cplane.m_plane.origin, ON_3dPoint(1, 2, 3)));
    CHECK(c.e.Run("_CPlane 3Point 0,0,0 0,1,0 0,0,1"));
    CHECK(cur.m_cplane.m_plane.zaxis.IsParallelTo(ON_3dVector::XAxis) != 0);
    CHECK(!c.e.Run("_CPlane 3Point 0,0,0 1,0,0 2,0,0"));
    // CPlane con nome
    CHECK(c.e.Run("_NamedCPlane Save Parete\n_CPlane Reset"));
    CHECK(cur.m_cplane.m_plane.zaxis.IsParallelTo(ON_3dVector::ZAxis) != 0);
    CHECK(c.e.Run("_NamedCPlane Restore Parete"));
    CHECK(cur.m_cplane.m_plane.zaxis.IsParallelTo(ON_3dVector::XAxis) != 0);
    CHECK(c.e.Run("_NamedCPlane Rename Parete Muro"));
    CHECK(!c.e.Run("_NamedCPlane Restore Parete"));
    CHECK(c.e.Model().m_settings.m_named_cplanes.Count() == 1);
    // vista con nome
    CHECK(c.e.Run("_NamedView Save \"Pianta alta\""));
    cur.m_vp.SetCameraLocation(ON_3dPoint(50, 50, 100));
    CHECK(c.e.Run("_NamedView Restore \"Pianta alta\""));
    CHECK(NearPt(cur.m_vp.CameraLocation(), ON_3dPoint(0, 0, 100)));
    CHECK(c.e.Model().m_settings.m_named_views.Count() == 1);
    CHECK(c.e.Run("_NamedView Save Seconda\n_NamedView Delete Seconda"));
    CHECK(c.e.Model().m_settings.m_named_views.Count() == 1);
    CHECK(!c.e.Run("_NamedView Restore Seconda"));
    // piano di sezione nella vista attiva
    CHECK(c.e.Run("_CPlane Reset\n_Box -5,-5,0 5,5 10\n_ClippingPlane -8,-8,0 8,8,0"));
    const ON_ClippingPlaneSurface* cps = ON_ClippingPlaneSurface::Cast(c.Last());
    CHECK(cps && cps->m_clipping_plane.m_bEnabled && cps->m_clipping_plane.m_viewport_ids.Count() == 1 &&
          cps->m_clipping_plane.m_viewport_ids.FindUuid(vp1));
    // la parte sopra il piano (verso la normale) e' nascosta, come una pianta
    if (cps) {
      const ON_PlaneEquation eq = cps->m_clipping_plane.m_plane.plane_equation;
      CHECK(eq.ValueAt(ON_3dPoint(0, 0, 5)) > 0 && eq.ValueAt(ON_3dPoint(0, 0, -5)) < 0);
    }
    CHECK(c.e.Run("_EnableClippingPlane All"));
    cps = ON_ClippingPlaneSurface::Cast(c.Last());
    CHECK(cps && cps->m_clipping_plane.m_viewport_ids.Count() == 2);
    CHECK(c.e.Run("_DisableClippingPlane All"));
    cps = ON_ClippingPlaneSurface::Cast(c.Last());
    CHECK(cps && cps->m_clipping_plane.m_viewport_ids.Count() == 0 && !cps->m_clipping_plane.m_bEnabled);
    CHECK(c.e.Run("_EnableClippingPlane"));
    cps = ON_ClippingPlaneSurface::Cast(c.Last());
    CHECK(cps && cps->m_clipping_plane.m_bEnabled && cps->m_clipping_plane.m_viewport_ids.Count() == 1);
    // istantanee: livelli e stato degli oggetti
    CHECK(c.e.Run("-_Layer New Arredi\n_CurrentLayer Arredi\n_Point 20,20\n_CurrentLayer Default\n_SelNone\n_Snapshots Save Prima"));
    const auto ids = c.e.AllObjectIds(true);
    CHECK(c.e.Run("_SelNone\n_SelLayer Arredi\n_LayerOff\n_SelAll\n_Lock"));
    cur.m_vp.SetCameraLocation(ON_3dPoint(7, 7, 100));
    CHECK(c.e.Run("_Snapshots Restore Prima"));
    CHECK(NearPt(cur.m_vp.CameraLocation(), ON_3dPoint(0, 0, 100), 1e-6));
    bool unlocked = true;
    for (const auto& id : ids) if (c.e.Object(id)->Attributes(nullptr)->Mode() != ON::object_mode::normal_object) unlocked = false;
    CHECK(unlocked);
    bool arrediVisible = false;
    { ONX_ModelComponentIterator it(c.e.Model(), ON_ModelComponent::Type::Layer);
      for (auto* m = it.FirstComponent(); m; m = it.NextComponent())
        if (auto* l = ON_Layer::Cast(m); l && ToUtf8(l->Name()) == "Arredi") arrediVisible = l->IsVisible(); }
    CHECK(arrediVisible);
    CHECK(c.e.Run("_Snapshots List") && c.log.str().find("Prima") != std::string::npos);
    CHECK(!c.e.Run("_Snapshots Restore Nessuna"));
    // tutto resta nel .3dm
    const std::string path = dir + "/viste.3dm";
    CHECK(c.doc.Save(path));
    Document3dm back;
    CHECK(back.Load(path));
    const auto& st = back.Model().m_settings;
    CHECK(st.m_named_views.Count() == 1 && ToUtf8(st.m_named_views[0].m_name) == "Pianta alta");
    CHECK(st.m_named_cplanes.Count() == 1 && ToUtf8(st.m_named_cplanes[0].m_name) == "Muro" &&
          st.m_named_cplanes[0].m_plane.zaxis.IsParallelTo(ON_3dVector::XAxis) != 0);
    ON_wString snap;
    CHECK(back.Model().GetDocumentUserString(L"Kurva3D.Snapshot:Prima", snap) && !snap.IsEmpty());
    bool clipBack = false;
    { ONX_ModelComponentIterator it(back.Model(), ON_ModelComponent::Type::ModelGeometry);
      for (auto* m = it.FirstComponent(); m; m = it.NextComponent())
        if (auto* g = ON_ModelGeometryComponent::Cast(m))
          if (auto* s = ON_ClippingPlaneSurface::Cast(g->Geometry(nullptr)))
            clipBack = s->m_clipping_plane.m_viewport_ids.FindUuid(vp1); }
    CHECK(clipBack);
    // senza finestra: nessun crash, errori chiari
    Ctx bare;
    CHECK(!bare.e.Run("_CPlane World Top"));
    CHECK(!bare.e.Run("_NamedView Save X"));
    CHECK(bare.e.Run("_ClippingPlane 0,0,0 1,1,0"));
  }
  {
    std::printf("Tavole_e_PDF\n");
    namespace fs = std::filesystem;
    const std::string dir = g_outDir + "/tavole";
    fs::create_directories(dir);
    Ctx c;
    CHECK(c.e.Run("_Box 0,0,0 40,30 20\n_Sphere 80,0,10 10\n_Text 0,-20,0 \"Pianta\" Height=5"));
    const size_t modelObjs = c.e.AllObjectIds(true).size();
    CHECK(c.e.Run("_Layout \"Tavola 1\" Size=A3 Orientation=Landscape Details=4"));
    const int li = FindLayout(c.e.Model(), "Tavola 1");
    CHECK(li >= 0);
    if (li >= 0) {
      const auto& v = c.e.Model().m_settings.m_views[li];
      CHECK(Near(v.m_page_settings.m_width_mm, 420) && Near(v.m_page_settings.m_height_mm, 297));
      CHECK(c.e.activeLayoutId == v.m_vp.ViewportId());
    }
    // nello spazio della tavola ci sono solo i 4 dettagli
    auto ids = c.e.AllObjectIds(true);
    int det = 0;
    for (const auto& id : ids) if (ON_DetailView::Cast(c.e.Object(id)->Geometry(nullptr))) det++;
    CHECK(ids.size() == 4 && det == 4);
    // oggetti disegnati nella tavola: spazio pagina
    CHECK(c.e.Run("_Rectangle 5,5 415,292\n_Text 330,12,0 \"Kurva3D - Tavola 1\" Height=4"));
    CHECK(c.e.AllObjectIds(true).size() == 6);
    CHECK(c.e.ObjectIdsInSpace(ON_nil_uuid, true).size() == modelObjs);
    // scala di un dettaglio
    CHECK(c.e.Run("_SelNone"));
    for (const auto& id : c.e.AllObjectIds(true))
      if (auto* d = ON_DetailView::Cast(c.e.Object(id)->Geometry(nullptr)); d && d->m_view.m_vp.IsParallelProjection()) {
        c.e.Select(id);
        break;
      }
    CHECK(c.e.Run("_Detail Scale 1:5"));
    {
      const auto sel = c.e.Selection();
      const auto* d = sel.empty() ? nullptr : ON_DetailView::Cast(c.e.Object(sel[0])->Geometry(nullptr));
      CHECK(d && Near(d->m_page_per_model_ratio, 0.2));
      if (d) {
        double l, r, b, t, n, f;
        d->m_view.m_vp.GetFrustum(&l, &r, &b, &t, &n, &f);
        const ON_BoundingBox bb = d->m_boundary.BoundingBox();
        CHECK(Near(r - l, (bb.m_max.x - bb.m_min.x) * 5, 1e-6));
      }
    }
    CHECK(c.e.Run("_Detail Add 20,20 120,100 Projection=Front"));
    CHECK(c.e.AllObjectIds(true).size() == 7);
    // seconda tavola copiata, poi tutte in un PDF
    CHECK(c.e.Run("_CopyLayout \"Tavola 2\""));
    CHECK(c.e.AllObjectIds(true).size() == 7 && FindLayout(c.e.Model(), "Tavola 2") >= 0);
    const std::string pdf = dir + "/tavole.pdf";
    CHECK(c.e.Run("_Print Destination=PDF File=\"" + pdf + "\" View=All"));
    {
      std::ifstream f(pdf, std::ios::binary);
      std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      CHECK(data.rfind("%PDF-1.4", 0) == 0 && data.find("%%EOF") != std::string::npos);
      CHECK(data.find("/Count 2") != std::string::npos);
      CHECK(data.find("/Subtype /Image") != std::string::npos);  // dettaglio prospettico ombreggiato
      CHECK(data.find("/MediaBox [0 0 1190.551 841.890]") != std::string::npos);
    }
    // composizione: linee dentro il foglio, testo della tavola presente
    {
      PageDrawing pg;
      CHECK(ComposeLayout(c.e, FindLayout(c.e.Model(), "Tavola 1"), PrintOptions(), pg));
      bool inside = !pg.lines.empty();
      for (const auto& l : pg.lines)
        for (const auto& p : l.pts) if (p.x < -1e-6 || p.y < -1e-6 || p.x > 420 + 1e-6 || p.y > 297 + 1e-6) inside = false;
      CHECK(inside);
      bool title = false;
      for (const auto& t : pg.texts) if (t.text == "Kurva3D - Tavola 1" && std::fabs(t.heightMm - 4) < 1e-6) title = true;
      CHECK(title);
    }
    // vista del modello su A4, in bianco e nero
    CHECK(c.e.Run("_Layouts Activate Model\n_Print File=\"" + dir + "/vista.pdf\" View=Top Size=A4 Color=BlackWhite"));
    CHECK(fs::file_size(dir + "/vista.pdf") > 500);
    CHECK(c.e.AllObjectIds(true).size() == modelObjs);
    // annulla la creazione: la tavola attiva sparisce e si torna al modello
    CHECK(c.e.Run("_Layouts Activate \"Tavola 2\"\n_Layouts Rename \"Tavola 2\" Dettagli\n_Layouts Delete Dettagli"));
    CHECK(FindLayout(c.e.Model(), "Dettagli") < 0 && ON_UuidIsNil(c.e.activeLayoutId));
    CHECK(!c.e.Run("_Detail Add 0,0 10,10"));
    CHECK(!c.e.Run("_Print Destination=PDF"));
    CHECK(!c.e.Run("_Layout X Size=B7"));
    // salvataggio: tavole, dettagli e oggetti di pagina nel .3dm
    const std::string path = dir + "/tavole.3dm";
    CHECK(c.doc.Save(path));
    Document3dm back;
    CHECK(back.Load(path));
    CHECK(LayoutIndices(back.Model()).size() == 1);
    int pageObjs = 0, dets = 0;
    { ONX_ModelComponentIterator it(back.Model(), ON_ModelComponent::Type::ModelGeometry);
      for (auto* m = it.FirstComponent(); m; m = it.NextComponent())
        if (auto* g = ON_ModelGeometryComponent::Cast(m); g && g->Attributes(nullptr)->m_space == ON::page_space) {
          pageObjs++;
          if (ON_DetailView::Cast(g->Geometry(nullptr))) dets++;
        } }
    CHECK(pageObjs == 7 && dets == 5);
  }
  {
    std::printf("Rendering\n");
    namespace fs = std::filesystem;
    const std::string dir = g_outDir + "/render";
    fs::create_directories(dir);
    Ctx c;
    CHECK(c.e.Run("_Box 0,0,0 40,30 20\n_Sphere 70,10,10 10\n_SelLast\n_SetMaterial Color=200,40,40 Gloss=0.8 Name=Rosso\n_SelNone"));
    CHECK(c.e.Run("_PointLight 20,15,60\n_DirectionalLight 0,0,0 -10,-10,20\n_SpotLight 20,15,0 10 20,15,50\n_RectangularLight 0,0,80 10,0,80 10,10,80"));
    int lights = 0, spots = 0;
    for (auto* g : c.Objects()) if (auto* l = ON_Light::Cast(g)) { lights++; if (l->IsSpotLight()) spots++; }
    CHECK(lights == 4 && spots == 1);
    CHECK(c.e.Run("_Sun On Azimuth=135 Altitude=40\n_GroundPlane On Altitude=Auto ShadowOnly=No\n_Skylight On"));
    const auto& rs = c.e.Model().m_settings.m_RenderSettings;
    CHECK(rs.Sun().EnableOn() && Near(rs.Sun().Azimuth(), 135) && Near(rs.Sun().Altitude(), 40));
    CHECK(rs.GroundPlane().Enabled() && rs.Skylight().Enabled());
    // materiale assegnato all'oggetto
    bool red = false;
    for (const auto& id : c.e.AllObjectIds(true)) {
      const auto* a = c.e.Object(id)->Attributes(nullptr);
      if (a->MaterialSource() != ON::material_from_object) continue;
      const auto* m = ON_Material::Cast(c.e.Model().ComponentFromIndex(ON_ModelComponent::Type::RenderMaterial, a->m_material_index).ModelComponent());
      if (m && ToUtf8(m->Name()) == "Rosso" && m->Diffuse() == ON_Color(200, 40, 40)) red = true;
    }
    CHECK(red);
    // immagine: PNG valido, la sfera rossa e' rossa, le ombre piu' scure del piano
    const std::string png = dir + "/render.png";
    CHECK(c.e.Run("_Render Width=160 Height=120 Samples=8 View=Top File=\"" + png + "\""));
    {
      std::ifstream f(png, std::ios::binary);
      std::string d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      CHECK(d.size() > 100 && d.compare(1, 3, "PNG") == 0);
    }
    // path tracer diretto: pixel sopra la sfera rossa
    {
      ui::Scene sc(c.e);
      sc.fixedSpace = true;
      sc.Rebuild();
      RenderOptions ro;
      ro.width = 64;
      ro.height = 64;
      RenderScene rsc;
      rsc.Build(c.e, sc, ro);
      CHECK(rsc.TriangleCount() > 100 && rsc.Lights().size() == 5);  // 4 luci + sole
      ON_3dmView v = StandardView("Top", ON_BoundingBox(ON_3dPoint(60, 0, 0), ON_3dPoint(80, 20, 20)), 1);
      RenderCamera cam;
      CHECK(cam.Init(v, 1));
      // raggio dal centro: colpisce la sfera dall'alto
      Ray r = cam.Generate(0.5, 0.5);
      double t, bu, bv;
      int tri;
      CHECK(rsc.Intersect(r, 1e30, t, tri, bu, bv) && std::fabs((r.o + r.d * t).z - 20) < 0.5);
      PathTracer pt(rsc, cam, ro);
      for (int i = 0; i < 4; i++) CHECK(pt.Pass());
      std::vector<unsigned char> img;
      pt.Image(img);
      const size_t k = (32 * 64 + 32) * 3;
      CHECK(img.size() == 64 * 64 * 3 && img[k] > img[k + 1] + 40 && img[k] > img[k + 2] + 40);
    }
    // impostazioni e luci restano nel .3dm
    const std::string path = dir + "/luci.3dm";
    CHECK(c.doc.Save(path));
    Document3dm back;
    CHECK(back.Load(path));
    CHECK(back.Model().m_settings.m_RenderSettings.Sun().EnableOn() &&
          Near(back.Model().m_settings.m_RenderSettings.Sun().Altitude(), 40));
    CHECK(!c.e.Run("_SpotLight 0,0,0 5 0,0,0"));
    CHECK(!c.e.Run("_SetDisplayMode Rendered"));  // senza finestra
  }
  {
    std::printf("AI_lettura_risposta\n");
    const auto l = ai::ExtractScript("<think>devo fare un cubo</think>Ecco:\n```kurva\n_Box 0,0,0 10,10 10 => Cubo\n\n# commento\nSphere 0,0,0 3\n```\nFatto.");
    CHECK(l.size() == 2 && l[0] == "_Box 0,0,0 10,10 10 => Cubo" && l[1] == "_Sphere 0,0,0 3");
    std::vector<int> origin;
    const auto x = ai::ExpandScript(l, &origin);
    CHECK(x.size() == 6 && x[3] == "_SetObjectName \"Cubo\"" && origin[5] == 1 && origin[4] == 0);
    Ctx c;
    auto v = ai::Validate(c.e, {"_Box 0,0,0 10,10 10", "_Volare 1"});
    CHECK(!v.ok && v.line == 1);
    v = ai::Validate(c.e, {"_Save \"x.3dm\""});
    CHECK(!v.ok && v.error.find("non permesso") != std::string::npos);
    v = ai::Validate(c.e, {"_Sphere 0,0,0"});
    CHECK(!v.ok && v.line == 0);
    CHECK(ai::RepairRequest({"_Sphere 0,0,0"}, v).find("Line 1") != std::string::npos);
    // nomi inventati dai modelli piccoli e nomi con spazi (dalla prova reale con Qwen3 0.6B)
    const auto al = ai::ExtractScript("```kurva\n_Cube 0,0,0 20 20 20 => Cylindrical Hole\nDifference\n```");
    CHECK(al.size() == 2 && al[0] == "_Box 0,0,0 20 20 20 => Cylindrical Hole" && al[1] == "_BooleanDifference");
    const auto ax = ai::ExpandScript({al[0]}, nullptr);
    CHECK(ax.size() == 5 && ax[3] == "_SetObjectName \"CylindricalHole\"");
  }
  {
    std::printf("AI_scelta_del_modello\n");
    auto pick = [](double ram, double vram) {
      ai::HardwareInfo hw;
      hw.ramGB = ram;
      if (vram > 0) hw.gpus.push_back({"GPU", vram, true});
      return ai::Catalog()[ai::RecommendModel(hw)].id;
    };
    CHECK(pick(4, 0) == "qwen3-1.7b");
    CHECK(pick(8, 0) == "qwen3-4b");
    CHECK(pick(16, 0) == "qwen3-8b");
    CHECK(pick(32, 0) == "qwen3-14b");
    {  // Mac con Apple Silicon: memoria unificata, la GPU (Metal) usa la RAM
      ai::HardwareInfo mac;
      mac.ramGB = 16;
      mac.unifiedMemory = true;
      bool gpu = false;
      ai::ModelBudgetGB(mac, &gpu);
      CHECK(gpu && ai::Catalog()[ai::RecommendModel(mac)].id == "qwen3-8b");
    }
    CHECK(pick(64, 0) == "qwen3-32b");
    CHECK(pick(16, 24) == "qwen3-14b");
    for (const auto& m : ai::Catalog()) CHECK(ai::LicenseAllowed(m.license));
    CHECK(!ai::LicenseAllowed("llama3.1") && !ai::LicenseAllowed("gemma") && !ai::LicenseAllowed("other"));
  }

  {
    std::printf("Storia\n");
    Ctx c;
    CHECK(c.e.Run("_History Record=Yes\n_Circle 0,0,0 5\n_Circle 0,0,10 3\n_SelAll _Loft\n_SelNone"));
    std::vector<ON_UUID> circles;
    ON_UUID loft = ON_nil_uuid;
    for (const auto& id : c.e.AllObjectIds(true)) {
      const auto* g = c.e.Object(id)->Geometry(nullptr);
      if (ON_Curve::Cast(g)) circles.push_back(id);
      else loft = id;
    }
    CHECK(circles.size() == 2 && ON_UuidIsNotNil(loft));
    ON_wString rec;
    CHECK(c.e.Model().GetDocumentUserString(L"Kurva3D.History", rec) && rec.Length() > 0);
    auto height = [&](const ON_UUID& id) {
      ON_BoundingBox b;
      c.e.Object(id)->Geometry(nullptr)->GetTightBoundingBox(b);
      return b.m_max.z - b.m_min.z;
    };
    CHECK(Near(height(loft), 10, 1e-6));
    // l'ingresso si sposta: il loft si ricalcola (stesso id) e Annulla torna indietro in un passo
    c.e.Select(circles[1]);
    CHECK(c.e.Run("_Move 0,0,0 0,0,5\n_SelNone"));
    CHECK(c.e.Object(loft) && Near(height(loft), 15, 1e-6));
    CHECK(c.e.Run("_Undo") && Near(height(loft), 10, 1e-6));
    CHECK(c.e.Run("_Redo") && Near(height(loft), 15, 1e-6));
    // a cascata: un'estrusione del bordo del loft... qui un secondo loft che usa il primo cerchio
    // e una copia: SelChildren / SelParents
    c.e.ClearSelection();
    c.e.Select(circles[0]);
    CHECK(c.e.Run("_SelChildren"));
    CHECK(c.e.IsSelected(loft));
    c.e.ClearSelection();
    c.e.Select(loft);
    CHECK(c.e.Run("_SelParents") && c.e.IsSelected(circles[0]) && c.e.IsSelected(circles[1]));
    // salvataggio e riapertura: la storia resta e funziona ancora
    const std::string f = g_outDir + "/storia.3dm";
    CHECK(c.doc.Save(f));
    Ctx d;
    CHECK(d.doc.Load(f));
    d.e.Select(circles[0]);
    CHECK(d.e.Run("_Scale 0,0,0 2\n_SelNone"));
    { ON_BoundingBox b; d.e.Object(loft)->Geometry(nullptr)->GetTightBoundingBox(b); CHECK(Near(b.m_max.x, 10, 1e-3)); }
    // modificare a mano il figlio interrompe la storia
    d.e.Select(loft);
    CHECK(d.e.Run("_Move 0,0,0 100,0,0\n_SelNone"));
    ON_wString after;
    CHECK(!d.e.Model().GetDocumentUserString(L"Kurva3D.History", after) || after.IsEmpty());
    d.e.Select(circles[1]);
    CHECK(d.e.Run("_Move 0,0,0 0,0,5\n_SelNone"));
    { ON_BoundingBox b; d.e.Object(loft)->Geometry(nullptr)->GetTightBoundingBox(b); CHECK(Near(b.m_min.x, 90, 1e-3)); }
    // senza Record niente storia; HistoryPurge la toglie
    Ctx e3;
    CHECK(e3.e.Run("_Line 0,0 10,0\n_SelAll _ExtrudeCrv 5\n_SelNone"));
    CHECK(!e3.e.Model().GetDocumentUserString(L"Kurva3D.History", after));
    CHECK(e3.e.Run("_History Record=Yes\n_Line 0,20 10,20\n_SelLast _ExtrudeCrv 5\n_SelNone _SelObjectsWithHistory"));
    CHECK(e3.e.Selection().size() == 1);
    CHECK(e3.e.Run("_HistoryPurge All") && !e3.e.Model().GetDocumentUserString(L"Kurva3D.History", after));
  }
  {
    std::printf("Nodi\n");
    using namespace cad::nodes;
    Ctx c;
    Graph g;
    // cursore -> raggio del cerchio; serie -> punti -> polilinea
    const int sl = g.Add("Slider");
    g.Get(sl)->params["value"] = "3.25";
    const int circ = g.Add("Circle");
    CHECK(g.Connect(sl, 0, circ, 1));
    const int ser = g.Add("Series");
    const int pt = g.Add("ConstructPoint");
    CHECK(g.Connect(ser, 0, pt, 0));
    const int expr = g.Add("Expression");
    g.Get(expr)->params["expr"] = "sin(x/9*pi)*4";
    CHECK(g.Connect(ser, 0, expr, 0) && g.Connect(expr, 0, pt, 1));
    const int pl = g.Add("Polyline");
    CHECK(g.Connect(pt, 0, pl, 0));
    CHECK(!g.Connect(pl, 0, ser, 0));  // niente cicli
    g.Evaluate(&c.e);
    const auto* cn = g.Get(circ);
    CHECK(cn->error.empty() && cn->outputs[0].size() == 1);
    { ON_Arc a; CHECK(ON_Curve::Cast(cn->outputs[0][0].geo.get())->IsArc(nullptr, &a) && Near(a.Radius(), 3.25)); }
    CHECK(g.Get(pt)->outputs[0].size() == 10 && Near(g.Get(pt)->outputs[0][9].pt.x, 9));
    CHECK(Near(g.Get(pt)->outputs[0][3].pt.y, std::sin(3.0 / 9 * ON_PI) * 4));
    const auto* poly = ON_PolylineCurve::Cast(g.Get(pl)->outputs[0][0].geo.get());
    CHECK(poly && poly->PointCount() == 10);
    // lista piu' lunga: 3 raggi -> 3 cerchi
    const int pan = g.Add("Panel");
    g.Get(pan)->params["text"] = "1\n2\n4";
    const int circ2 = g.Add("Circle");
    CHECK(g.Connect(pan, 0, circ2, 1));
    // cerchi -> estrusione chiusa -> volume; loft tra due cerchi
    const int ex = g.Add("Extrude");
    CHECK(g.Connect(circ2, 0, ex, 0));
    g.Evaluate(&c.e);
    CHECK(g.Get(circ2)->outputs[0].size() == 3 && g.Get(ex)->outputs[0].size() == 3);
    const auto* solid = ON_Brep::Cast(g.Get(ex)->outputs[0][0].geo.get());
    CHECK(solid && solid->IsSolid());
    // salvataggio e rilettura del grafo
    const std::string text = g.Save();
    Graph h;
    CHECK(h.Load(text) && h.nodes.size() == g.nodes.size() && h.wires.size() == g.wires.size());
    CHECK(h.Get(expr)->params["expr"] == "sin(x/9*pi)*4" && h.Get(pan)->params["text"] == "1\n2\n4");
    h.Evaluate(&c.e);
    CHECK(h.Get(ex)->outputs[0].size() == 3);
    // anteprima e bake
    std::vector<std::shared_ptr<const ON_Geometry>> prev;
    h.Preview(prev);
    CHECK(prev.size() >= 3 + 3 + 1 + 10);
    CHECK(h.Bake(ex, c.e) == 3 && c.e.AllObjectIds(true).size() == 3);
    // geometria dal modello: le curve del documento entrano nel grafo
    CHECK(c.e.Run("_Circle 0,0,0 5\n_Circle 0,0,10 3"));
    std::string ids;
    for (const auto& id : c.e.AllObjectIds(true)) {
      if (!ON_Curve::Cast(c.e.Object(id)->Geometry(nullptr))) continue;
      char b[64];
      ON_UuidToString(id, b);
      ids += std::string(ids.empty() ? "" : ",") + b;
    }
    Graph k;
    const int ref = k.Add("GeometryRef");
    k.Get(ref)->params["ids"] = ids;
    const int len = k.Add("CurveLength");
    const int sum = k.Add("MassAddition");
    CHECK(k.Connect(ref, 0, len, 0) && k.Connect(len, 0, sum, 0));
    const int item = k.Add("ListItem");
    CHECK(k.Connect(len, 0, item, 0));
    k.Get(item)->params.clear();
    k.Evaluate(&c.e);
    CHECK(Near(k.Get(sum)->outputs[0][0].num, 2 * ON_PI * 8, 1e-6));
    CHECK(Near(k.Get(item)->outputs[0][0].num, 2 * ON_PI * 5, 1e-6));
    // ingresso mancante: avviso, nessun risultato
    const int line = k.Add("Line");
    k.Evaluate(&c.e);
    CHECK(!k.Get(line)->warning.empty() && k.Get(line)->outputs[0].empty());
#ifdef CAD_WITH_OCCT
    const int loft = k.Add("Loft");
    CHECK(k.Connect(ref, 0, loft, 0));
    const int bx = k.Add("CenterBox");
    const int vol = k.Add("Volume");
    CHECK(k.Connect(bx, 0, vol, 0));
    const int rect = k.Add("Rectangle");
    const int area = k.Add("Area");
    CHECK(k.Connect(rect, 0, area, 0));
    k.Evaluate(&c.e);
    CHECK(k.Get(loft)->error.empty() && k.Get(loft)->outputs[0].size() == 1 && ON_Brep::Cast(k.Get(loft)->outputs[0][0].geo.get()));
    CHECK(Near(k.Get(vol)->outputs[0][0].num, 1, 1e-6) && Near(k.Get(area)->outputs[0][0].num, 1, 1e-6));
    CHECK(NearPt(k.Get(area)->outputs[1][0].pt, ON_3dPoint(0.5, 0.5, 0), 1e-6));
    // solidi su un piano inclinato e volumi
    Graph v;
    const int pn = v.Add("PlaneNormal");
    const int vx = v.Add("VectorXYZ");
    v.Get(vx)->params.clear();
    const int one = v.Add("Panel");
    v.Get(one)->params["text"] = "1";
    CHECK(v.Connect(one, 0, vx, 0) && v.Connect(one, 0, vx, 2) && v.Connect(vx, 0, pn, 1));
    const int cy = v.Add("Cylinder"), co = v.Add("Cone"), sp = v.Add("Sphere");
    CHECK(v.Connect(pn, 0, cy, 0) && v.Connect(pn, 0, co, 0));
    const int v1 = v.Add("Volume"), v2 = v.Add("Volume"), v3 = v.Add("Volume");
    CHECK(v.Connect(cy, 0, v1, 0) && v.Connect(co, 0, v2, 0) && v.Connect(sp, 0, v3, 0));
    v.Evaluate(&c.e);
    CHECK(v.Get(v1)->outputs[0].size() == 1 && Near(v.Get(v1)->outputs[0][0].num, ON_PI * 10, 1e-5));
    CHECK(v.Get(v2)->outputs[0].size() == 1 && Near(v.Get(v2)->outputs[0][0].num, ON_PI * 10 / 3, 1e-5));
    CHECK(v.Get(v3)->outputs[0].size() == 1 && Near(v.Get(v3)->outputs[0][0].num, ON_PI * 4 / 3, 1e-5));
    { ON_BoundingBox b; v.Get(cy)->outputs[0][0].geo->GetTightBoundingBox(b); CHECK(b.m_max.x > 7 && b.m_max.z > 7); }
#endif
    // espressioni
    double r = 0;
    std::string err;
    CHECK(EvalExpression("2^3 + max(1, 4) * -2 + sqrt(16)", {}, r, err) && Near(r, 4));
    CHECK(!EvalExpression("2 +", {}, r, err) && !err.empty());
  }
#ifdef CAD_WITH_PYTHON
  {
    std::printf("Python\n");
    namespace fs = std::filesystem;
    const std::string dir = g_outDir + "/python";
    fs::create_directories(dir);
    const std::string py = dir + "/script.py";
    {
      std::ofstream f(py, std::ios::binary);
      f << "import rhinoscriptsyntax as rs\n"
           "rs.AddLayer('Rosso', (255, 0, 0))\n"
           "rs.CurrentLayer('Rosso')\n"
           "a = rs.AddLine((0, 0, 0), (10, 0, 0))\n"
           "c = rs.AddCircle(rs.WorldXYPlane(), 5)\n"
           "ids = [rs.AddPoint(i * 2, 0, 0) for i in range(3)]\n"
           "rs.ObjectName(a, 'asse')\n"
           "rs.ObjectColor(c, (0, 128, 255))\n"
           "m = rs.CopyObject(c, (20, 0, 0))\n"
           "rs.RotateObject(m, (20, 0, 0), 90)\n"
           "rs.ScaleObject(m, (20, 0, 0), (2, 2, 2))\n"
           "t = rs.CurveClosestPoint(a, (3, 4, 0))\n"
           "print('lunghezza %.6f %.6f %.9f' % (rs.CurveLength(a), rs.CurveLength(c), t))\n"
           "print('tipi', rs.ObjectType(a), rs.ObjectType(ids[0]), rs.IsCurveClosed(c))\n"
           "print('mid', rs.CurveMidPoint(a), rs.VectorCrossProduct((1, 0, 0), (0, 1, 0)))\n"
           "rs.Command('_Line 0,0,0 0,10,0')\n"
           "print('creati', len(rs.LastCreatedObjects()))\n";
    }
    Ctx c;
    CHECK(c.e.Run("_RunPythonScript \"" + py + "\""));
    const std::string log = c.log.str();
    CHECK(log.find("lunghezza 10.000000 31.415927 3.000000000") != std::string::npos);
    CHECK(log.find("tipi 4 1 True") != std::string::npos);
    CHECK(log.find("mid 5,0,0 0,0,1") != std::string::npos);
    CHECK(log.find("creati 1") != std::string::npos);
    int curves = 0, points = 0, red = 0;
    bool named = false, colored = false, scaled = false;
    for (const auto& id : c.e.AllObjectIds(true)) {
      const auto* o = c.e.Object(id);
      const auto* g = o->Geometry(nullptr);
      const auto* a = o->Attributes(nullptr);
      if (ON_Curve::Cast(g)) curves++;
      if (ON_Point::Cast(g)) points++;
      const auto* l = ON_Layer::Cast(c.e.Model().ComponentFromIndex(ON_ModelComponent::Type::Layer, a->m_layer_index).ModelComponent());
      if (l && ToUtf8(l->Name()) == "Rosso") red++;
      if (ToUtf8(a->m_name) == "asse") named = true;
      if (a->ColorSource() == ON::color_from_object && a->m_color == ON_Color(0, 128, 255)) colored = true;
      ON_BoundingBox b;
      if (const auto* arc = ON_ArcCurve::Cast(g); arc && g->GetTightBoundingBox(b) && Near(b.Center().x, 20) && Near(arc->Radius(), 10)) scaled = true;
    }
    CHECK(curves == 4 && points == 3 && red == 7 && named && colored && scaled);
    // un solo passo di annullamento per tutto lo script
    CHECK(c.e.Run("_Undo") && c.e.AllObjectIds(true).empty());
    // codice in linea (virgolette, parentesi e punto e virgola restano nel codice)
    Ctx d;
    CHECK(d.e.Run("-_RunPythonScript (import rhinoscriptsyntax as rs; print(\"a (b); c\"); rs.AddPoint((1, 2, 3)))"));
    CHECK(d.log.str().find("a (b); c") != std::string::npos && d.Objects().size() == 1);
    // errore: traccia nel messaggio, quanto creato prima resta (come in Rhino)
    Ctx e2;
    CHECK(!e2.e.Run("-_RunPythonScript (import rhinoscriptsyntax as rs; rs.AddPoint((0, 0, 0)); 1 / 0)"));
    CHECK(e2.e.LastError().find("ZeroDivisionError") != std::string::npos && e2.Objects().size() == 1);
    // solidi, booleane e misure (motore geometrico)
    Ctx f;
    CHECK(f.e.Run("-_RunPythonScript (import rhinoscriptsyntax as rs; "
                  "b = rs.AddBox([(0,0,0),(10,0,0),(10,10,0),(0,10,0),(0,0,10),(10,0,10),(10,10,10),(0,10,10)]); "
                  "s = rs.AddSphere((30, 0, 0), 2); cy = rs.AddCylinder((0, 0, 0), (0, 0, 20), 1); "
                  "k = rs.AddCone((50, 0, 0), 6, 3); tor = rs.AddTorus((70, 0, 0), 5, 1); "
                  "print('bb', rs.BoundingBox([s])[6]); print('tipo', rs.ObjectType(b), rs.IsPolysurface(b)))"));
    CHECK(f.log.str().find("bb 32,2,2") != std::string::npos && f.log.str().find("tipo 16 True") != std::string::npos);
    CHECK(f.Objects().size() == 5);
#ifdef CAD_WITH_OCCT
    CHECK(f.e.Run("-_RunPythonScript (import rhinoscriptsyntax as rs; ids = rs.AllObjects(); "
                  "print('vol %.4f %.4f %.4f' % (rs.SurfaceVolume(ids[0])[0], rs.SurfaceVolume(ids[1])[0], rs.SurfaceVolume(ids[3])[0])); "
                  "u = rs.BooleanUnion([ids[0], ids[2]]); print('unione', len(u), '%.3f' % rs.SurfaceVolume(u[0])[0]))"));
    const std::string fl = f.log.str();
    if (fl.find("vol 1000.0000") == std::string::npos) std::printf("%s\n%s\n", f.e.LastError().c_str(), fl.c_str());
    CHECK(fl.find("vol 1000.0000 33.5103 56.5487") != std::string::npos);
    // cilindro (r=1, h=20) sullo spigolo del cubo: dentro solo un quarto della parte z<10
    char want[64];
    std::snprintf(want, sizeof want, "unione 1 %.3f", 1000 + ON_PI * (0.75 * 10 + 10));
    CHECK(fl.find(want) != std::string::npos);
#endif
  }
#endif

  {
    std::printf("Collaborazione (patch)\n");
    using namespace cad::collab;
    auto same = [](const collab::Snapshot& x, const collab::Snapshot& y) {
      return x.objects == y.objects && x.tables == y.tables && x.userStrings == y.userStrings;
    };
    Ctx a, b;
    CHECK(a.e.Run("_Line 0,0 10,0\n_Circle 0,0 5\n_Point 3,3"));
    CHECK(b.doc.FromBuffer(a.doc.ToBuffer()));
    collab::Snapshot sa = collab::TakeSnapshot(a.e.Model()), sb = collab::TakeSnapshot(b.e.Model());
    CHECK(same(sa, sb) && sa.objects.size() == 3);
    PatchInfo pi;
    std::string p0 = MakePatch(a.e.Model(), sa, &pi);
    CHECK(pi.Empty());
    // in B un gruppo in piu': gli indici dei gruppi non coincidono piu'
    CHECK(b.e.Run("_Point 50,50\n_SelAll _Group\n_SelNone"));
    collab::Snapshot sb2 = collab::TakeSnapshot(b.e.Model());
    std::string pb = MakePatch(b.e.Model(), sb, &pi);
    CHECK(pi.upserted == 4 && pi.tables == 1);  // 3 oggetti nel gruppo + 1 nuovo, 1 gruppo
    // modifiche in A: livello nuovo, oggetto spostato su quel livello, gruppo, eliminazione, testo utente
    const int li = a.e.Model().AddLayer(L"Muri", ON_Color(200, 30, 30));
    CHECK(li > 0);
    auto ids = a.e.AllObjectIds();
    CHECK(ids.size() == 3);
    a.e.EditableAttributes(ids[0])->m_layer_index = li;
    CHECK(a.e.Run("_SelNone\n_Line 0,5 10,5\n_Line 0,6 10,6\n_SelLast\n_Circle 20,0 1"));
    CHECK(a.e.Run("_SelAll _SelNone"));
    a.e.Select(ids[2]);
    CHECK(a.e.Run("_Delete"));
    auto ids2 = a.e.AllObjectIds();
    for (const auto& id : ids2)
      if (id != ids[0] && id != ids[1]) a.e.Select(id);
    CHECK(a.e.Run("_Group\n_SelNone"));
    a.e.Model().SetDocumentUserString(L"Kurva3D.Prova", L"ciao");
    std::string pa = MakePatch(a.e.Model(), sa, &pi);
    CHECK(pi.deleted == 1 && pi.upserted == 4 && pi.tables == 2 && pi.strings == 1);
    Document3dm copy;
    CHECK(copy.FromBuffer(b.doc.ToBuffer()));
    // B riceve le modifiche di A
    std::string err;
    if (!ApplyPatch(b.e.Model(), pa, &pi, &err)) { std::printf("err %s\n", err.c_str()); CHECK(false); }
    CHECK(pi.deleted == 1 && pi.upserted == 4);
    {
      const ON_ModelComponent* l = b.e.Model().ComponentFromId(ON_ModelComponent::Type::Layer,
          a.e.Model().ComponentFromIndex(ON_ModelComponent::Type::Layer, li).ModelComponent()->Id()).ModelComponent();
      CHECK(l && ON_Layer::Cast(l)->Color() == ON_Color(200, 30, 30));
      const auto* o = b.e.Object(ids[0]);
      CHECK(o && o->Attributes(nullptr)->m_layer_index == l->Index());
      CHECK(!b.e.Object(ids[2]));
      ON_wString v;
      CHECK(b.e.Model().GetDocumentUserString(L"Kurva3D.Prova", v) && v == L"ciao");
      // il gruppo nuovo di A in B ha un altro indice (B aveva gia' un gruppo)
      int newGroupObjs = 0;
      for (const auto& id : b.e.AllObjectIds()) {
        ON_SimpleArray<int> gl;
        b.e.Object(id)->Attributes(nullptr)->GetGroupList(gl);
        if (gl.Count() == 1 && gl[0] == 1) newGroupObjs++;
      }
      CHECK(newGroupObjs == 3);
    }
    // A riceve le modifiche di B fatte in parallelo (punto e gruppo): vince l'ultima, il punto
    // eliminato in A torna perche' B lo aveva modificato (gruppo)
    CHECK(ApplyPatch(a.e.Model(), pb, &pi, &err));
    CHECK(a.e.AllObjectIds().size() == 7 && a.e.Object(ids[2]) != nullptr);
    // modifica di una voce esistente: il colore del livello cambia sul posto, l'indice resta
    collab::Snapshot s1 = collab::TakeSnapshot(b.e.Model());
    {
      auto* l = const_cast<ON_Layer*>(ON_Layer::Cast(b.e.Model().ComponentFromIndex(ON_ModelComponent::Type::Layer, li).ModelComponent()));
      CHECK(l != nullptr);
      if (l) l->SetColor(ON_Color(0, 0, 255));
    }
    std::string pc = MakePatch(b.e.Model(), s1, &pi);
    CHECK(pi.tables == 1 && pi.upserted == 0);
    CHECK(ApplyPatch(a.e.Model(), pc, &pi, &err));
    {
      const auto* l = ON_Layer::Cast(a.e.Model().ComponentFromIndex(ON_ModelComponent::Type::Layer, li).ModelComponent());
      CHECK(l && l->Color() == ON_Color(0, 0, 255) && std::wstring(static_cast<const wchar_t*>(l->Name())) == L"Muri");
    }
    // dati non validi
    CHECK(!ApplyPatch(a.e.Model(), "spazzatura", &pi, &err) && !err.empty());
    // il modello che ha ricevuto le patch si salva e si rilegge
    CHECK(!a.doc.ToBuffer().empty() && copy.FromBuffer(a.doc.ToBuffer()));
    (void)sb2; (void)p0;
    // Annulla dei propri comandi non cancella le modifiche ricevute
    {
      Ctx x, y;
      CHECK(y.doc.FromBuffer(x.doc.ToBuffer()));
      const collab::Snapshot sy = collab::TakeSnapshot(y.e.Model());
      CHECK(y.e.Run("_Circle 0,0 3"));
      const std::string py = MakePatch(y.e.Model(), sy);
      CHECK(x.e.Run("_Line 0,0 10,0"));
      CHECK(x.e.ApplyRemote([&](ONX_Model& mm) { return ApplyPatch(mm, py); }));
      CHECK(x.e.AllObjectIds().size() == 2);
      CHECK(x.e.Run("_Undo") && x.e.AllObjectIds().size() == 1 && ON_ArcCurve::Cast(x.Last()) != nullptr);
      CHECK(x.e.Run("_Redo") && x.e.AllObjectIds().size() == 2);
    }
  }

  Case("Error_unknown_command", "_NonEsiste", [](Ctx&) {}, false);
  Case("Error_no_selection", "_Move 0,0 1,0", [](Ctx&) {}, false);

  ShutdownLibrary();
  std::printf("\n%d controlli, %d falliti\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
