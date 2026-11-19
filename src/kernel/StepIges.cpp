// STEP e IGES con il modulo DataExchange di OpenCASCADE.
#include "commands/Exchange.h"
#include "kernel/OcctConvert.h"
#include <BRep_Builder.hxx>
#include <IGESControl_Controller.hxx>
#include <IGESControl_Reader.hxx>
#include <IGESControl_Writer.hxx>
#include <Interface_Static.hxx>
#include <Message.hxx>
#include <Message_Messenger.hxx>
#include <Message_PrinterOStream.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPControl_Writer.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <BRep_Tool.hxx>
#include <IGESData_GlobalSection.hxx>
#include <IGESData_IGESModel.hxx>
#include <TColStd_SequenceOfAsciiString.hxx>
#include <TopoDS_Vertex.hxx>
#include <algorithm>
#include <cmath>
#include <filesystem>
#ifdef CAD_WITH_XCAF
#include <IGESCAFControl_Reader.hxx>
#include <IGESCAFControl_Writer.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <Quantity_Color.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <TColStd_HSequenceOfExtendedString.hxx>
#include <TCollection_AsciiString.hxx>
#include <TDF_Label.hxx>
#include <TDF_LabelSequence.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_LayerTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#endif

namespace cad {

namespace {
// Unita' per il writer IGES (nome del Global Section)
const char* UnitName(ON::LengthUnitSystem u) {
  switch (u) {
    case ON::LengthUnitSystem::Inches: return "IN";
    case ON::LengthUnitSystem::Feet: return "FT";
    case ON::LengthUnitSystem::Centimeters: return "CM";
    case ON::LengthUnitSystem::Meters: return "M";
    case ON::LengthUnitSystem::Kilometers: return "KM";
    case ON::LengthUnitSystem::Microns: return "UM";
    case ON::LengthUnitSystem::Mils: return "MIL";
    case ON::LengthUnitSystem::Miles: return "MI";
    case ON::LengthUnitSystem::Microinches: return "UIN";
    default: return "MM";
  }
}
// Unita' per i parametri xstep.cad.unit e write.step.unit (enumerazione di OpenCASCADE)
const char* XstepUnit(ON::LengthUnitSystem u) {
  return u == ON::LengthUnitSystem::Inches ? "INCH" : UnitName(u);
}

// Unita' supportate da OpenCASCADE (le altre: millimetri)
ON::LengthUnitSystem Supported(ON::LengthUnitSystem u) {
  switch (u) {
    case ON::LengthUnitSystem::Inches: case ON::LengthUnitSystem::Feet: case ON::LengthUnitSystem::Centimeters:
    case ON::LengthUnitSystem::Meters: case ON::LengthUnitSystem::Kilometers: case ON::LengthUnitSystem::Microns:
    case ON::LengthUnitSystem::Mils: case ON::LengthUnitSystem::Miles: case ON::LengthUnitSystem::Microinches:
    case ON::LengthUnitSystem::Millimeters: return u;
    default: return ON::LengthUnitSystem::Millimeters;
  }
}

// Nome dell'unita' di lunghezza letto da OpenCASCADE nel file STEP
ON::LengthUnitSystem UnitFromName(std::string n) {
  for (auto& c : n) c = (char)std::toupper((unsigned char)c);
  if (n == "MM" || n.find("MILLIM") != std::string::npos) return ON::LengthUnitSystem::Millimeters;
  if (n == "CM" || n.find("CENTIM") != std::string::npos) return ON::LengthUnitSystem::Centimeters;
  if (n == "KM" || n.find("KILOM") != std::string::npos) return ON::LengthUnitSystem::Kilometers;
  if (n == "UM" || n.find("MICROM") != std::string::npos || n.find("MICRON") != std::string::npos) return ON::LengthUnitSystem::Microns;
  if (n == "M" || n.find("METRE") != std::string::npos || n.find("METER") != std::string::npos) return ON::LengthUnitSystem::Meters;
  if (n == "IN" || n.find("INCH") != std::string::npos) return ON::LengthUnitSystem::Inches;
  if (n == "FT" || n.find("FOOT") != std::string::npos || n.find("FEET") != std::string::npos) return ON::LengthUnitSystem::Feet;
  if (n == "MI" || n.find("MILE") != std::string::npos) return ON::LengthUnitSystem::Miles;
  if (n == "MIL") return ON::LengthUnitSystem::Mils;
  return ON::LengthUnitSystem::None;
}

ON::LengthUnitSystem IgesUnit(int flag) {
  switch (flag) {
    case 1: return ON::LengthUnitSystem::Inches;
    case 2: return ON::LengthUnitSystem::Millimeters;
    case 4: return ON::LengthUnitSystem::Feet;
    case 5: return ON::LengthUnitSystem::Miles;
    case 6: return ON::LengthUnitSystem::Meters;
    case 7: return ON::LengthUnitSystem::Kilometers;
    case 8: return ON::LengthUnitSystem::Mils;
    case 9: return ON::LengthUnitSystem::Microns;
    case 10: return ON::LengthUnitSystem::Centimeters;
    case 11: return ON::LengthUnitSystem::Microinches;
    default: return ON::LengthUnitSystem::None;
  }
}

// i rapporti di trasferimento di OpenCASCADE non vanno sulla console
void Quiet() {
  static bool done = false;
  if (done) return;
  done = true;
  Message::DefaultMessenger()->RemovePrinters(STANDARD_TYPE(Message_PrinterOStream));
}

bool IsStep(const std::string& path) {
  const std::string e = FileExtension(path);
  return e == "step" || e == "stp";
}

// Proprieta' ereditate lungo l'albero dell'assieme.
struct Props {
  std::string name, layer;
  bool hasColor = false;
  ON_Color color;
};

// Oggetti di una forma: uno per solido, guscio libero, faccia libera e curva libera.
void AddShape(const TopoDS_Shape& shape, double tol, const Props& pr, std::vector<ImportedObject>& out) {
  auto emit = [&](ON_Geometry* g, int k) {
    ImportedObject o;
    o.geom.reset(g);
    o.layer = pr.layer;
    o.name = pr.name;
    if (k > 0 && !o.name.empty()) o.name += " (" + std::to_string(k + 1) + ")";
    if (pr.hasColor) { o.colorSource = ImportedObject::Source::Object; o.objectColor = pr.color; }
    out.push_back(std::move(o));
  };
  int k = 0;
  auto add = [&](const TopoDS_Shape& s) { if (ON_Brep* b = occt::ShapeToBrep(s, tol)) emit(b, k++); };
  for (TopExp_Explorer x(shape, TopAbs_SOLID); x.More(); x.Next()) add(x.Current());
  for (TopExp_Explorer x(shape, TopAbs_SHELL, TopAbs_SOLID); x.More(); x.Next()) add(x.Current());
  for (TopExp_Explorer x(shape, TopAbs_FACE, TopAbs_SHELL); x.More(); x.Next()) add(x.Current());
  for (TopExp_Explorer x(shape, TopAbs_EDGE, TopAbs_FACE); x.More(); x.Next())
    for (ON_Curve* c : occt::ShapeToCurves(x.Current(), tol)) emit(c, k++);
  for (TopExp_Explorer x(shape, TopAbs_VERTEX, TopAbs_EDGE); x.More(); x.Next()) {
    const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(x.Current()));
    emit(new ON_Point(ON_3dPoint(p.X(), p.Y(), p.Z())), k++);
  }
}

#ifdef CAD_WITH_XCAF
std::string LabelName(const TDF_Label& l) {
  Handle(TDataStd_Name) n;
  if (!l.FindAttribute(TDataStd_Name::GetID(), n)) return "";
  TCollection_AsciiString a(n->Get());  // UTF-8
  std::string s = a.ToCString();
  // nomi generati automaticamente (NAUO1, SOLID, =>[0:1:1:2]...) non servono
  if (s.rfind("=>", 0) == 0 || s.rfind("NAUO", 0) == 0) return "";
  return s;
}

bool LabelColor(const Handle(XCAFDoc_ColorTool)& ct, const TDF_Label& l, ON_Color& c) {
  Quantity_Color q;
  if (!ct->GetColor(l, XCAFDoc_ColorSurf, q) && !ct->GetColor(l, XCAFDoc_ColorGen, q) && !ct->GetColor(l, XCAFDoc_ColorCurv, q))
    return false;
  double r, g, b;
  q.Values(r, g, b, Quantity_TOC_sRGB);
  c = ON_Color((int)std::lround(r * 255), (int)std::lround(g * 255), (int)std::lround(b * 255));
  return true;
}
bool ShapeColor(const Handle(XCAFDoc_ColorTool)& ct, const TopoDS_Shape& s, ON_Color& c) {
  Quantity_Color q;
  if (!ct->GetColor(s, XCAFDoc_ColorSurf, q) && !ct->GetColor(s, XCAFDoc_ColorGen, q) && !ct->GetColor(s, XCAFDoc_ColorCurv, q))
    return false;
  double r, g, b;
  q.Values(r, g, b, Quantity_TOC_sRGB);
  c = ON_Color((int)std::lround(r * 255), (int)std::lround(g * 255), (int)std::lround(b * 255));
  return true;
}

std::string LabelLayer(const Handle(XCAFDoc_LayerTool)& lt, const TDF_Label& l) {
  Handle(TColStd_HSequenceOfExtendedString) layers = new TColStd_HSequenceOfExtendedString();
  if (!lt->GetLayers(l, layers) || layers->IsEmpty()) return "";
  return TCollection_AsciiString(layers->Value(1)).ToCString();
}

void Visit(const Handle(XCAFDoc_ShapeTool)& st, const Handle(XCAFDoc_ColorTool)& ct, const Handle(XCAFDoc_LayerTool)& lt,
           const TDF_Label& label, const TopLoc_Location& loc, Props pr, double tol, std::vector<ImportedObject>& out,
           int depth) {
  if (depth > 64) return;
  const std::string nm = LabelName(label);
  ON_Color c;
  if (LabelColor(ct, label, c)) { pr.color = c; pr.hasColor = true; }
  if (std::string l = LabelLayer(lt, label); !l.empty()) pr.layer = l;
  if (XCAFDoc_ShapeTool::IsReference(label)) {
    TDF_Label ref;
    XCAFDoc_ShapeTool::GetReferredShape(label, ref);
    // nome: quello del prodotto, altrimenti quello dell'istanza
    Props p2 = pr;
    const std::string pn = LabelName(ref);
    p2.name = !pn.empty() ? pn : (!nm.empty() ? nm : pr.name);
    Visit(st, ct, lt, ref, loc * XCAFDoc_ShapeTool::GetLocation(label), p2, tol, out, depth + 1);
    return;
  }
  if (!nm.empty()) pr.name = nm;
  if (XCAFDoc_ShapeTool::IsAssembly(label)) {
    TDF_LabelSequence comps;
    XCAFDoc_ShapeTool::GetComponents(label, comps);
    for (int i = 1; i <= comps.Length(); i++) Visit(st, ct, lt, comps.Value(i), loc, pr, tol, out, depth + 1);
    return;
  }
  TopoDS_Shape shape = XCAFDoc_ShapeTool::GetShape(label);
  if (shape.IsNull()) return;
  if (!pr.hasColor) {
    // colore assegnato alle sole facce: quello della prima faccia colorata
    for (TopExp_Explorer x(shape, TopAbs_FACE); x.More(); x.Next())
      if (ShapeColor(ct, x.Current(), c)) { pr.color = c; pr.hasColor = true; break; }
  }
  if (!loc.IsIdentity()) shape = shape.Moved(loc);
  AddShape(shape, tol, pr, out);
}
#endif
}  // namespace

bool ReadStepIges(const std::string& path, ON::LengthUnitSystem docUnits, double tol, ImportedModel& model,
                  std::string& err) {
  Quiet();
  std::vector<ImportedObject>& out = model.objects;
  const bool step = IsStep(path);
  ON::LengthUnitSystem fileUnits = ON::LengthUnitSystem::None;
  // le coordinate arrivano nelle unita' del documento (Importa) o del file (Apri)
  auto setUnit = [&](ON::LengthUnitSystem u) {
    Interface_Static::SetCVal("xstep.cad.unit", XstepUnit(Supported(u)));
  };
  try {
#ifdef CAD_WITH_XCAF
    Handle(TDocStd_Document) doc;
    Handle(XCAFApp_Application) app = XCAFApp_Application::GetApplication();
    app->NewDocument("MDTV-XCAF", doc);
    if (step) {
      STEPCAFControl_Reader r;
      r.SetColorMode(true);
      r.SetNameMode(true);
      r.SetLayerMode(true);
      if (r.ReadFile(path.c_str()) != IFSelect_RetDone) { err = "impossibile leggere il file STEP"; return false; }
      TColStd_SequenceOfAsciiString len, ang, sol;
      r.ChangeReader().FileUnits(len, ang, sol);
      if (len.Length() > 0) fileUnits = UnitFromName(len.Value(1).ToCString());
      const ON::LengthUnitSystem target = Supported(docUnits != ON::LengthUnitSystem::None ? docUnits
                                                    : fileUnits != ON::LengthUnitSystem::None ? fileUnits
                                                    : ON::LengthUnitSystem::Millimeters);
      setUnit(target);
      XCAFDoc_DocumentTool::SetLengthUnit(doc, ON::UnitScale(target, ON::LengthUnitSystem::Meters));
      if (!r.Transfer(doc)) { err = "conversione STEP non riuscita"; return false; }
    } else {
      IGESControl_Controller::Init();
      IGESCAFControl_Reader r;
      r.SetColorMode(true);
      r.SetNameMode(true);
      r.SetLayerMode(true);
      if (r.ReadFile(path.c_str()) != IFSelect_RetDone) { err = "impossibile leggere il file IGES"; return false; }
      if (!r.IGESModel().IsNull()) fileUnits = IgesUnit(r.IGESModel()->GlobalSection().UnitFlag());
      const ON::LengthUnitSystem target = Supported(docUnits != ON::LengthUnitSystem::None ? docUnits
                                                    : fileUnits != ON::LengthUnitSystem::None ? fileUnits
                                                    : ON::LengthUnitSystem::Millimeters);
      setUnit(target);
      XCAFDoc_DocumentTool::SetLengthUnit(doc, ON::UnitScale(target, ON::LengthUnitSystem::Meters));
      if (!r.Transfer(doc)) { err = "conversione IGES non riuscita"; return false; }
    }
    Handle(XCAFDoc_ShapeTool) st = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    Handle(XCAFDoc_ColorTool) ct = XCAFDoc_DocumentTool::ColorTool(doc->Main());
    Handle(XCAFDoc_LayerTool) lt = XCAFDoc_DocumentTool::LayerTool(doc->Main());
    TDF_LabelSequence roots;
    st->GetFreeShapes(roots);
    for (int i = 1; i <= roots.Length(); i++) Visit(st, ct, lt, roots.Value(i), TopLoc_Location(), Props(), tol, out, 0);
    app->Close(doc);
#else
    TopoDS_Shape shape;
    if (step) {
      STEPControl_Reader r;
      if (r.ReadFile(path.c_str()) != IFSelect_RetDone) { err = "impossibile leggere il file STEP"; return false; }
      TColStd_SequenceOfAsciiString len, ang, sol;
      r.FileUnits(len, ang, sol);
      if (len.Length() > 0) fileUnits = UnitFromName(len.Value(1).ToCString());
      setUnit(docUnits != ON::LengthUnitSystem::None ? docUnits : fileUnits);
      r.TransferRoots();
      shape = r.OneShape();
    } else {
      IGESControl_Controller::Init();
      IGESControl_Reader r;
      if (r.ReadFile(path.c_str()) != IFSelect_RetDone) { err = "impossibile leggere il file IGES"; return false; }
      if (!r.IGESModel().IsNull()) fileUnits = IgesUnit(r.IGESModel()->GlobalSection().UnitFlag());
      setUnit(docUnits != ON::LengthUnitSystem::None ? docUnits : fileUnits);
      r.TransferRoots();
      shape = r.OneShape();
    }
    if (shape.IsNull()) { err = "il file non contiene geometria"; return false; }
    AddShape(shape, tol, Props(), out);
#endif
  } catch (const Standard_Failure&) {
    err = "errore di OpenCASCADE durante la lettura";
    return false;
  }
  if (docUnits == ON::LengthUnitSystem::None)
    model.units = Supported(fileUnits != ON::LengthUnitSystem::None ? fileUnits : ON::LengthUnitSystem::Millimeters);
  // livelli usati (STEP/IGES non danno il colore del livello)
  for (const auto& o : out)
    if (!o.layer.empty() && std::none_of(model.layers.begin(), model.layers.end(),
                                         [&](const ImportedLayer& l) { return l.name == o.layer; })) {
      ImportedLayer l;
      l.name = o.layer;
      model.layers.push_back(l);
    }
  if (out.empty()) { err = "nessuna geometria convertibile nel file"; return false; }
  return true;
}

bool WriteStepIges(const std::string& path, const std::vector<ExportItem>& items, ON::LengthUnitSystem docUnits,
                   double tol, std::string& err) {
  Quiet();
  const std::string unit = UnitName(Supported(docUnits));
  const std::string xunit = XstepUnit(Supported(docUnits));
  // forme con nome, colore e livello dell'oggetto
  struct Item { TopoDS_Shape shape; const ExportItem* src; };
  std::vector<Item> shapes;
  for (const auto& it : items) {
    if (ON_Mesh::Cast(it.geom) || ON_SubD::Cast(it.geom) || ON_Point::Cast(it.geom)) continue;
    TopoDS_Shape s;
    try { s = occt::GeometryToShape(*it.geom, tol); } catch (const Standard_Failure&) { continue; }
    if (s.IsNull()) continue;
    shapes.push_back({s, &it});
  }
  if (shapes.empty()) { err = "nessuna curva, superficie o solido da esportare (mesh e SubD non vanno in STEP/IGES)"; return false; }
  const bool step = IsStep(path);
  try {
    Interface_Static::SetCVal("write.step.unit", xunit.c_str());
    Interface_Static::SetCVal("write.step.schema", "AP214IS");
    Interface_Static::SetCVal("write.iges.unit", xunit.c_str());
    Interface_Static::SetIVal("write.iges.brep.mode", 0);  // superfici tagliate (tipo 144), le piu' compatibili
#ifdef CAD_WITH_XCAF
    Handle(TDocStd_Document) doc;
    Handle(XCAFApp_Application) app = XCAFApp_Application::GetApplication();
    app->NewDocument("MDTV-XCAF", doc);
    XCAFDoc_DocumentTool::SetLengthUnit(doc, ON::UnitScale(Supported(docUnits), ON::LengthUnitSystem::Meters));
    Handle(XCAFDoc_ShapeTool) st = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    Handle(XCAFDoc_ColorTool) ct = XCAFDoc_DocumentTool::ColorTool(doc->Main());
    Handle(XCAFDoc_LayerTool) lt = XCAFDoc_DocumentTool::LayerTool(doc->Main());
    for (const auto& s : shapes) {
      const TDF_Label l = st->AddShape(s.shape, false);
      if (!s.src->name.empty()) TDataStd_Name::Set(l, TCollection_ExtendedString(s.src->name.c_str(), true));
      const ON_Color c = s.src->color;
      if (c != ON_UNSET_COLOR)
        ct->SetColor(l, Quantity_Color(c.Red() / 255.0, c.Green() / 255.0, c.Blue() / 255.0, Quantity_TOC_sRGB), XCAFDoc_ColorGen);
      if (!s.src->layer.empty()) lt->SetLayer(l, TCollection_ExtendedString(s.src->layer.c_str(), true));
    }
    bool ok;
    if (step) {
      STEPCAFControl_Writer w;
      w.SetColorMode(true);
      w.SetNameMode(true);
      w.SetLayerMode(true);
      ok = w.Transfer(doc, STEPControl_AsIs);
      if (!ok) err = "conversione STEP non riuscita";
      else if (w.Write(path.c_str()) != IFSelect_RetDone) { ok = false; err = "impossibile scrivere " + path; }
    } else {
      IGESControl_Controller::Init();
      IGESCAFControl_Writer w;
      w.SetColorMode(true);
      w.SetNameMode(true);
      w.SetLayerMode(true);
      ok = w.Transfer(doc);
      if (!ok) err = "conversione IGES non riuscita";
      else if (!w.Write(path.c_str())) { ok = false; err = "impossibile scrivere " + path; }
    }
    app->Close(doc);
    if (!ok) return false;
#else
    BRep_Builder bb;
    TopoDS_Compound comp;
    bb.MakeCompound(comp);
    for (const auto& s : shapes) bb.Add(comp, s.shape);
    if (step) {
      STEPControl_Writer w;
      if (w.Transfer(comp, STEPControl_AsIs) != IFSelect_RetDone) { err = "conversione STEP non riuscita"; return false; }
      if (w.Write(path.c_str()) != IFSelect_RetDone) { err = "impossibile scrivere " + path; return false; }
    } else {
      IGESControl_Controller::Init();
      IGESControl_Writer w(unit.c_str(), 0);
      if (!w.AddShape(comp)) { err = "conversione IGES non riuscita"; return false; }
      w.ComputeModel();
      if (!w.Write(path.c_str())) { err = "impossibile scrivere " + path; return false; }
    }
#endif
  } catch (const Standard_Failure&) {
    err = "errore di OpenCASCADE durante la scrittura";
    return false;
  }
  return true;
}

}  // namespace cad
