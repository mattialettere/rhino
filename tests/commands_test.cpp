// Test dei comandi: ogni caso esegue uno script e controlla il modello.
// Alla fine ogni modello viene salvato in formato Rhino 8, riletto e
// confrontato (nessuna differenza ammessa).
#include "commands/Annotation.h"
#include "commands/Engine.h"
#include "core/ModelCompare.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <filesystem>
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

  Case("Error_unknown_command", "_NonEsiste", [](Ctx&) {}, false);
  Case("Error_no_selection", "_Move 0,0 1,0", [](Ctx&) {}, false);

  ShutdownLibrary();
  std::printf("\n%d controlli, %d falliti\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
