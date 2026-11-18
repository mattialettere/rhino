// Comandi di Rhino 8 con OpenCASCADE, gruppo 4: Make2D (linee nascoste),
// MoveFace, FlowAlongSrf, OrientOnSrf.
#include "commands/Deform.h"
#include "commands/Engine.h"
#include "kernel/OcctConvert.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepLib.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <HLRAlgo_Projector.hxx>
#include <HLRBRep_Algo.hxx>
#include <HLRBRep_HLRToShape.hxx>
#include <ShapeAnalysis_Surface.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace cad {
namespace {

const ON_Geometry* Geo(Engine& e, const ON_UUID& id) {
  const auto* o = e.Object(id);
  return o ? o->Geometry(nullptr) : nullptr;
}

ON_3dmObjectAttributes Attrs(Engine& e, const ON_UUID& id) {
  return *e.Object(id)->Attributes(&ON_3dmObjectAttributes::DefaultAttributes);
}

bool IsSurfaceLike(const ON_Geometry* g) {
  return g && (ON_Brep::Cast(g) || ON_Extrusion::Cast(g) || ON_Surface::Cast(g));
}

// Faccia (di qualunque superficie del modello, esclusi gli oggetti indicati) piu' vicina al punto.
bool NearestFace(Engine& e, const ON_3dPoint& p, const std::vector<ON_UUID>& exclude, ON_UUID& id, TopoDS_Shape& shape,
                 TopoDS_Face& face) {
  double best = 1e300;
  const TopoDS_Shape v = BRepBuilderAPI_MakeVertex(gp_Pnt(p.x, p.y, p.z)).Vertex();
  for (const auto& oid : e.AllObjectIds(false)) {
    if (std::find(exclude.begin(), exclude.end(), oid) != exclude.end()) continue;
    const ON_Geometry* g = Geo(e, oid);
    if (!IsSurfaceLike(g)) continue;
    try {
      TopoDS_Shape s = occt::GeometryToShape(*g, e.Tolerance());
      for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next()) {
        BRepExtrema_DistShapeShape d(v, ex.Current());
        if (d.IsDone() && d.Value() < best) { best = d.Value(); id = oid; shape = s; face = TopoDS::Face(ex.Current()); }
      }
    } catch (...) {
    }
  }
  return !face.IsNull();
}

// Punto (u,v) della faccia piu' vicino a p.
gp_Pnt2d FaceUV(const TopoDS_Face& f, const ON_3dPoint& p, double tol) {
  ShapeAnalysis_Surface sas(BRep_Tool::Surface(f));
  return sas.ValueOfUV(gp_Pnt(p.x, p.y, p.z), tol);
}

// Punto, tangente u e normale (verso della faccia) in (u,v).
bool FaceFrame(const TopoDS_Face& f, double u, double v, ON_3dPoint& P, ON_3dVector& Su, ON_3dVector& N) {
  BRepGProp_Face gf(f);
  gp_Pnt q;
  gp_Vec n;
  gf.Normal(u, v, q, n);
  Handle(Geom_Surface) s = BRep_Tool::Surface(f);
  gp_Pnt q2;
  gp_Vec du, dv;
  s->D1(u, v, q2, du, dv);
  if (n.Magnitude() < 1e-15 || du.Magnitude() < 1e-15) return false;
  n.Normalize();
  du.Normalize();
  P = ON_3dPoint(q.X(), q.Y(), q.Z());
  Su = ON_3dVector(du.X(), du.Y(), du.Z());
  N = ON_3dVector(n.X(), n.Y(), n.Z());
  return true;
}

// --- Make2D ------------------------------------------------------------------------------
// Disegno 2D con linee nascoste delle superfici e dei solidi selezionati, visti
// da View=Top (predefinita), Front, Right, Back, Left, Bottom. Le curve vanno
// sul piano xy del mondo; ShowHidden=Yes aggiunge le linee nascoste (grigie),
// ShowTangents=Yes gli spigoli tangenti.
bool CmdMake2D(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("Make2D", "seleziona gli oggetti da disegnare, poi Invio");
  if (sel.empty()) return false;
  opts.merge(e.ReadOptions(ts));
  const std::string view = opts.count("view") ? opts["view"] : "top";
  gp_Dir toViewer(0, 0, 1), xdir(1, 0, 0);
  if (view == "front") { toViewer = gp_Dir(0, -1, 0); xdir = gp_Dir(1, 0, 0); }
  else if (view == "back") { toViewer = gp_Dir(0, 1, 0); xdir = gp_Dir(-1, 0, 0); }
  else if (view == "right") { toViewer = gp_Dir(1, 0, 0); xdir = gp_Dir(0, 1, 0); }
  else if (view == "left") { toViewer = gp_Dir(-1, 0, 0); xdir = gp_Dir(0, -1, 0); }
  else if (view == "bottom") { toViewer = gp_Dir(0, 0, -1); xdir = gp_Dir(1, 0, 0); }
  else if (view != "top") return e.Fail("vista non valida (Top, Front, Right, Back, Left, Bottom)");
  auto yes = [&](const char* k) { auto it = opts.find(k); return it != opts.end() && (it->second == "yes" || it->second == "si"); };
  const bool hidden = yes("showhidden"), tangents = yes("showtangents");
  BRep_Builder bb;
  TopoDS_Compound all;
  bb.MakeCompound(all);
  int n = 0;
  for (const auto& id : sel) {
    const ON_Geometry* g = Geo(e, id);
    if (!IsSurfaceLike(g)) continue;
    TopoDS_Shape s = occt::GeometryToShape(*g, e.Tolerance());
    if (s.IsNull()) continue;
    bb.Add(all, s);
    n++;
  }
  if (!n) return e.Fail("seleziona superfici, polisuperfici o solidi");
  int created = 0;
  try {
    Handle(HLRBRep_Algo) algo = new HLRBRep_Algo();
    algo->Add(all);
    HLRAlgo_Projector proj(gp_Ax2(gp_Pnt(0, 0, 0), toViewer, xdir));
    algo->Projector(proj);
    algo->Update();
    algo->Hide();
    HLRBRep_HLRToShape h(algo);
    ON_3dmObjectAttributes vis;
    ON_3dmObjectAttributes hid;
    hid.SetColorSource(ON::color_from_object);
    hid.m_color = ON_Color(150, 150, 150);
    auto add = [&](const TopoDS_Shape& s, const ON_3dmObjectAttributes& a) {
      if (s.IsNull()) return;
      BRepLib::BuildCurves3d(s);  // gli edge di HLR hanno solo la curva nel piano di proiezione
      std::vector<ON_Curve*> cs = occt::ShapeToCurves(s, e.Tolerance());
      ON_SimpleArray<const ON_Curve*> in;
      for (auto* c : cs) in.Append(c);
      ON_SimpleArray<ON_Curve*> joined;
      ON_JoinCurves(in, joined, e.Tolerance() * 10, false);
      for (auto* c : cs) delete c;
      for (int i = 0; i < joined.Count(); i++) {
        // il proiettore da' coordinate (x,y,0) nel piano della vista
        e.AddObject(joined[i], &a);
        created++;
      }
    };
    add(h.VCompound(), vis);
    add(h.OutLineVCompound(), vis);
    if (tangents) add(h.Rg1LineVCompound(), vis);
    if (hidden) {
      add(h.HCompound(), hid);
      add(h.OutLineHCompound(), hid);
      if (tangents) add(h.Rg1LineHCompound(), hid);
    }
  } catch (...) {
    return e.Fail("Make2D non riuscito");
  }
  if (!created) return e.Fail("nessuna curva visibile");
  e.Out() << "  Make2D: " << created << " curve (vista " << view << ")\n";
  return true;
}

// --- MoveFace ----------------------------------------------------------------------------
// Sposta una faccia piana di un solido: il prisma spazzato dalla faccia si
// aggiunge (verso l'esterno) o si toglie (verso l'interno), e le facce
// complanari si uniscono.
bool CmdMoveFace(Engine& e, TokenStream& ts) {
  auto p = e.ReadPick(ts, "clicca la faccia da spostare");
  if (!p) return false;
  ON_UUID id = ON_nil_uuid;
  TopoDS_Shape solid;
  TopoDS_Face face;
  if (!NearestFace(e, *p, {}, id, solid, face)) return e.Fail("faccia non trovata");
  auto a = e.ReadPoint(ts, "punto di partenza");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "punto di arrivo");
  if (!b) return e.Fail("manca il punto di arrivo");
  const ON_3dVector mv = *b - *a;
  if (mv.Length() <= e.Tolerance()) return e.Fail("spostamento nullo");
  try {
    const gp_Pnt2d uv = FaceUV(face, *p, e.Tolerance());
    ON_3dPoint P;
    ON_3dVector Su, N;
    if (!FaceFrame(face, uv.X(), uv.Y(), P, Su, N)) return e.Fail("normale della faccia non definita");
    BRepPrimAPI_MakePrism prism(face, gp_Vec(mv.x, mv.y, mv.z));
    TopoDS_Shape r;
    if (mv * N > 0) r = BRepAlgoAPI_Fuse(solid, prism.Shape()).Shape();
    else r = BRepAlgoAPI_Cut(solid, prism.Shape()).Shape();
    ShapeUpgrade_UnifySameDomain u(r, Standard_True, Standard_True, Standard_True);
    u.Build();
    std::unique_ptr<ON_Brep> nb(occt::ShapeToBrep(u.Shape(), e.Tolerance()));
    if (!nb) return e.Fail("spostamento della faccia non riuscito");
    e.ReplaceObject(id, nb.release());
  } catch (...) {
    return e.Fail("spostamento della faccia non riuscito");
  }
  e.Out() << "  Faccia spostata\n";
  return true;
}

// --- FlowAlongSrf ----------------------------------------------------------------------------
// Gli oggetti seguono la superficie di destinazione come stavano sulla
// superficie di base: (u,v) normalizzati e distanza lungo la normale.
bool CmdFlowAlongSrf(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("FlowAlongSrf", "seleziona gli oggetti da far scorrere, poi Invio");
  if (sel.empty()) return false;
  auto pb = e.ReadPick(ts, "superficie di base");
  if (!pb) return false;
  ON_UUID bid = ON_nil_uuid, tid = ON_nil_uuid;
  TopoDS_Shape bs, ts2;
  TopoDS_Face bf, tf;
  if (!NearestFace(e, *pb, sel, bid, bs, bf)) return e.Fail("superficie di base non trovata");
  auto pt = e.ReadPick(ts, "superficie di destinazione");
  if (!pt) return false;
  std::vector<ON_UUID> ex = sel;
  ex.push_back(bid);
  if (!NearestFace(e, *pt, ex, tid, ts2, tf)) return e.Fail("superficie di destinazione non trovata");
  double bu0, bu1, bv0, bv1, tu0, tu1, tv0, tv1;
  BRepTools::UVBounds(bf, bu0, bu1, bv0, bv1);
  BRepTools::UVBounds(tf, tu0, tu1, tv0, tv1);
  auto sas = std::make_shared<ShapeAnalysis_Surface>(BRep_Tool::Surface(bf));
  const double tol = e.Tolerance();
  PointMap f = [&](const ON_3dPoint& p) {
    const gp_Pnt2d uv = sas->ValueOfUV(gp_Pnt(p.x, p.y, p.z), tol);
    ON_3dPoint P;
    ON_3dVector Su, N;
    if (!FaceFrame(bf, uv.X(), uv.Y(), P, Su, N)) return p;
    const double h = (p - P) * N;
    const double fu = (uv.X() - bu0) / (bu1 - bu0), fv = (uv.Y() - bv0) / (bv1 - bv0);
    ON_3dPoint Q;
    ON_3dVector Tu, M;
    if (!FaceFrame(tf, tu0 + fu * (tu1 - tu0), tv0 + fv * (tv1 - tv0), Q, Tu, M)) return p;
    return Q + M * h;
  };
  int n = 0;
  for (const auto& id : sel) {
    ON_Geometry* g = nullptr;
    try { g = DeformGeometry(Geo(e, id), f); } catch (...) {}
    if (!g) continue;
    e.ReplaceObject(id, g);
    n++;
  }
  if (!n) return e.Fail("nessun oggetto deformato");
  e.Out() << "  " << n << " oggetti fatti scorrere sulla superficie\n";
  return true;
}

// --- OrientOnSrf --------------------------------------------------------------------------
// Copie degli oggetti nei punti cliccati sulla superficie: il piano xy del
// mondo nel punto base va sul piano tangente (asse x lungo la direzione u).
bool CmdOrientOnSrf(Engine& e, TokenStream& ts) {
  auto sel = e.RequireSelection("OrientOnSrf", "seleziona gli oggetti da orientare, poi Invio");
  if (sel.empty()) return false;
  auto base = e.ReadPoint(ts, "punto base");
  if (!base) return false;
  auto ps = e.ReadPick(ts, "superficie");
  if (!ps) return false;
  ON_UUID sid = ON_nil_uuid;
  TopoDS_Shape s;
  TopoDS_Face face;
  if (!NearestFace(e, *ps, sel, sid, s, face)) return e.Fail("superficie non trovata");
  const ON_Plane from(*base, ON_3dVector(1, 0, 0), ON_3dVector(0, 1, 0));
  int n = 0;
  while (true) {
    if (n > 0 && !e.MoreInput(ts)) break;
    auto q = e.ReadPoint(ts, n == 0 ? "punto sulla superficie" : "punto successivo (Invio per finire)");
    if (!q) { if (n > 0) break; return false; }
    const gp_Pnt2d uv = FaceUV(face, *q, e.Tolerance());
    ON_3dPoint P;
    ON_3dVector Su, N;
    if (!FaceFrame(face, uv.X(), uv.Y(), P, Su, N)) continue;
    ON_Xform x;
    x.Rotation(from, ON_Plane(P, Su, ON_CrossProduct(N, Su)));
    for (const auto& id : sel) {
      const ON_Geometry* g = Geo(e, id);
      if (!g) continue;
      ON_Geometry* c = g->Duplicate();
      c->Transform(x);
      const ON_3dmObjectAttributes a = Attrs(e, id);
      e.AddObject(c, &a);
    }
    n++;
  }
  e.SkipEnter(ts);
  e.Out() << "  Oggetti orientati in " << n << " punti\n";
  return true;
}

}  // namespace

void RegisterKernel4Commands(Engine& e) {
  auto reg = [&](const char* n, const char* cat, const char* d, CommandFn f) {
    CommandInfo i{n, cat, d, f};
    i.modifiesModel = true;
    e.Register(i);
  };
  reg("Make2D", "Curve", "Disegno 2D con linee nascoste (View=, ShowHidden=, ShowTangents=)", CmdMake2D);
  reg("MoveFace", "Solidi", "Sposta una faccia piana di un solido", CmdMoveFace);
  reg("FlowAlongSrf", "Trasforma", "Fa scorrere gli oggetti da una superficie di base a una di destinazione", CmdFlowAlongSrf);
  reg("OrientOnSrf", "Trasforma", "Copie degli oggetti orientate nei punti di una superficie", CmdOrientOnSrf);
}

}  // namespace cad
