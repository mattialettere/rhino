#pragma once
// Ponte tra openNURBS (formato .3dm) e OpenCASCADE (motore geometrico).
// Tutta la geometria viene convertita in forma NURBS esatta: nessuna
// approssimazione oltre a quella della riparametrizzazione delle curve di trim.
#include "opennurbs.h"
#include <TopoDS_Shape.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_BSplineSurface.hxx>
#include <Geom2d_BSplineCurve.hxx>
#include <memory>

namespace cad::occt {

Handle(Geom_BSplineCurve) ToOcct(const ON_NurbsCurve& c);
Handle(Geom2d_BSplineCurve) ToOcct2d(const ON_NurbsCurve& c);
Handle(Geom_BSplineSurface) ToOcct(const ON_NurbsSurface& s);
ON_NurbsCurve* FromOcct(const Handle(Geom_BSplineCurve)& c);
ON_NurbsCurve* FromOcct2d(const Handle(Geom2d_BSplineCurve)& c);
ON_NurbsSurface* FromOcct(const Handle(Geom_BSplineSurface)& s);

// Curva qualsiasi di openNURBS -> edge OpenCASCADE (forma NURBS).
TopoDS_Shape CurveToShape(const ON_Curve& c);
// Geometria qualsiasi (curva, superficie, B-rep, estrusione) -> shape.
TopoDS_Shape GeometryToShape(const ON_Geometry& g, double tolerance);
TopoDS_Shape BrepToShape(const ON_Brep& b, double tolerance);

// Shape OpenCASCADE -> B-rep openNURBS (facce e solidi). nullptr se vuoto.
ON_Brep* ShapeToBrep(const TopoDS_Shape& shape, double tolerance);
// Edge/wire OpenCASCADE -> curve openNURBS (una per edge, poi unite).
std::vector<ON_Curve*> ShapeToCurves(const TopoDS_Shape& shape, double tolerance);

// Mesh di visualizzazione/esportazione di una shape.
// weld = vertici coincidenti tra facce saldati (mesh chiusa); senza saldatura ogni
// faccia ha i suoi vertici e le normali esatte della superficie (spigoli vivi
// netti nella vista ombreggiata, come in Rhino).
ON_Mesh* MeshShape(const TopoDS_Shape& shape, double linearDeflection, double angularDeflectionRad, bool weld = true);

}  // namespace cad::occt
