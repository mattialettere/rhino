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
#include <filesystem>

namespace cad {

namespace {
const char* UnitName(ON::LengthUnitSystem u) {
  switch (u) {
    case ON::LengthUnitSystem::Inches: return "IN";
    case ON::LengthUnitSystem::Feet: return "FT";
    case ON::LengthUnitSystem::Centimeters: return "CM";
    case ON::LengthUnitSystem::Meters: return "M";
    case ON::LengthUnitSystem::Kilometers: return "KM";
    case ON::LengthUnitSystem::Microns: return "UM";
    case ON::LengthUnitSystem::Mils: return "MIL";
    default: return "MM";
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

void AddShape(const TopoDS_Shape& s, double tol, std::vector<ImportedObject>& out) {
  if (ON_Brep* b = occt::ShapeToBrep(s, tol)) {
    ImportedObject o;
    o.geom.reset(b);
    out.push_back(std::move(o));
  }
}
}  // namespace

bool ReadStepIges(const std::string& path, ON::LengthUnitSystem docUnits, double tol,
                  std::vector<ImportedObject>& out, std::string& err) {
  Quiet();
  const std::string unit = UnitName(docUnits);
  // le coordinate arrivano gia' nelle unita' del documento
  Interface_Static::SetCVal("xstep.cad.unit", unit.c_str());
  TopoDS_Shape shape;
  try {
    if (IsStep(path)) {
      STEPControl_Reader r;
      if (r.ReadFile(path.c_str()) != IFSelect_RetDone) { err = "impossibile leggere il file STEP"; return false; }
      r.TransferRoots();
      shape = r.OneShape();
    } else {
      IGESControl_Controller::Init();
      IGESControl_Reader r;
      if (r.ReadFile(path.c_str()) != IFSelect_RetDone) { err = "impossibile leggere il file IGES"; return false; }
      r.TransferRoots();
      shape = r.OneShape();
    }
  } catch (const Standard_Failure&) {
    err = "errore di OpenCASCADE durante la lettura";
    return false;
  }
  if (shape.IsNull()) { err = "il file non contiene geometria"; return false; }
  // un oggetto per solido, per guscio libero e per faccia libera; curve libere a parte
  int solids = 0;
  for (TopExp_Explorer x(shape, TopAbs_SOLID); x.More(); x.Next()) { AddShape(x.Current(), tol, out); solids++; }
  for (TopExp_Explorer x(shape, TopAbs_SHELL, TopAbs_SOLID); x.More(); x.Next()) AddShape(x.Current(), tol, out);
  for (TopExp_Explorer x(shape, TopAbs_FACE, TopAbs_SHELL); x.More(); x.Next()) AddShape(x.Current(), tol, out);
  for (TopExp_Explorer x(shape, TopAbs_EDGE, TopAbs_FACE); x.More(); x.Next())
    for (ON_Curve* c : occt::ShapeToCurves(x.Current(), tol)) {
      ImportedObject o;
      o.geom.reset(c);
      out.push_back(std::move(o));
    }
  (void)solids;
  if (out.empty()) { err = "nessuna geometria convertibile nel file"; return false; }
  return true;
}

bool WriteStepIges(const std::string& path, const std::vector<ExportItem>& items, ON::LengthUnitSystem docUnits,
                   double tol, std::string& err) {
  Quiet();
  const std::string unit = UnitName(docUnits);
  BRep_Builder bb;
  TopoDS_Compound comp;
  bb.MakeCompound(comp);
  int n = 0;
  for (const auto& it : items) {
    if (ON_Mesh::Cast(it.geom) || ON_SubD::Cast(it.geom) || ON_Point::Cast(it.geom)) continue;
    TopoDS_Shape s;
    try { s = occt::GeometryToShape(*it.geom, tol); } catch (const Standard_Failure&) { continue; }
    if (s.IsNull()) continue;
    bb.Add(comp, s);
    n++;
  }
  if (!n) { err = "nessuna curva, superficie o solido da esportare (mesh e SubD non vanno in STEP/IGES)"; return false; }
  try {
    if (IsStep(path)) {
      Interface_Static::SetCVal("write.step.unit", unit.c_str());
      Interface_Static::SetCVal("write.step.schema", "AP214IS");
      STEPControl_Writer w;
      if (w.Transfer(comp, STEPControl_AsIs) != IFSelect_RetDone) { err = "conversione STEP non riuscita"; return false; }
      if (w.Write(path.c_str()) != IFSelect_RetDone) { err = "impossibile scrivere " + path; return false; }
    } else {
      IGESControl_Controller::Init();
      IGESControl_Writer w(unit.c_str(), 0);  // 0: superfici tagliate (tipo 144), le piu' compatibili
      if (!w.AddShape(comp)) { err = "conversione IGES non riuscita"; return false; }
      w.ComputeModel();
      if (!w.Write(path.c_str())) { err = "impossibile scrivere " + path; return false; }
    }
  } catch (const Standard_Failure&) {
    err = "errore di OpenCASCADE durante la scrittura";
    return false;
  }
  return true;
}

}  // namespace cad
