// Python 3 incorporato: comando RunPythonScript e modulo rhinoscriptsyntax (un sottoinsieme
// con gli stessi nomi di Rhino 8). Il modulo "_kurva" (C++) dà accesso al motore; il modulo
// rhinoscriptsyntax e' scritto in Python sopra di esso (testo qui sotto).
#include "python/PythonHost.h"
#include "commands/Engine.h"
#include "core/Document3dm.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef CAD_WITH_PYTHON
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#endif

namespace cad {

std::function<bool(Engine&, const ON_UUID&, bool, double&, ON_3dPoint&)> MassPropertiesFn;
extern const char* const kRhinoscriptsyntax;  // RsSource.cpp (generato da CMake)

#ifdef CAD_WITH_PYTHON
namespace {

Engine* g_engine = nullptr;
std::string* g_output = nullptr;
std::string g_programDir;

std::string IdStr(const ON_UUID& id) {
  char s[64];
  ON_UuidToString(id, s);
  return s;
}

bool ParseId(PyObject* o, ON_UUID& id) {
  const char* s = PyUnicode_Check(o) ? PyUnicode_AsUTF8(o) : nullptr;
  if (!s) { PyErr_SetString(PyExc_TypeError, "id dell'oggetto (stringa) atteso"); return false; }
  id = ON_UuidFromString(s);
  return true;
}

PyObject* IdList(const std::vector<ON_UUID>& ids) {
  PyObject* l = PyList_New((Py_ssize_t)ids.size());
  for (size_t i = 0; i < ids.size(); i++) PyList_SET_ITEM(l, (Py_ssize_t)i, PyUnicode_FromString(IdStr(ids[i]).c_str()));
  return l;
}

bool ParsePoint(PyObject* o, ON_3dPoint& p) {
  PyObject* seq = PySequence_Fast(o, "punto (x, y, z) atteso");
  if (!seq) return false;
  const Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
  if (n < 2 || n > 3) { Py_DECREF(seq); PyErr_SetString(PyExc_ValueError, "punto con 2 o 3 coordinate atteso"); return false; }
  double c[3] = {0, 0, 0};
  for (Py_ssize_t i = 0; i < n; i++) {
    c[i] = PyFloat_AsDouble(PySequence_Fast_GET_ITEM(seq, i));
    if (PyErr_Occurred()) { Py_DECREF(seq); return false; }
  }
  Py_DECREF(seq);
  p = ON_3dPoint(c[0], c[1], c[2]);
  return true;
}

bool ParsePoints(PyObject* o, std::vector<ON_3dPoint>& pts) {
  PyObject* seq = PySequence_Fast(o, "lista di punti attesa");
  if (!seq) return false;
  for (Py_ssize_t i = 0; i < PySequence_Fast_GET_SIZE(seq); i++) {
    ON_3dPoint p;
    if (!ParsePoint(PySequence_Fast_GET_ITEM(seq, i), p)) { Py_DECREF(seq); return false; }
    pts.push_back(p);
  }
  Py_DECREF(seq);
  return true;
}

PyObject* Pt(const ON_3dPoint& p) { return Py_BuildValue("(ddd)", p.x, p.y, p.z); }

PyObject* Added(const ON_UUID& id) {
  if (ON_UuidIsNil(id)) Py_RETURN_NONE;
  return PyUnicode_FromString(IdStr(id).c_str());
}

const ON_Geometry* Geo(const ON_UUID& id) {
  const auto* o = g_engine->Object(id);
  return o ? o->Geometry(nullptr) : nullptr;
}

// --- funzioni del modulo _kurva ---
PyObject* k_write(PyObject*, PyObject* a) {
  const char* s;
  if (!PyArg_ParseTuple(a, "s", &s)) return nullptr;
  // le righe complete vanno subito nella cronologia (in ordine con i comandi eseguiti)
  if (g_output) {
    *g_output += s;
    size_t nl;
    while ((nl = g_output->find('\n')) != std::string::npos) {
      g_engine->Out() << "  " << g_output->substr(0, nl) << "\n";
      g_output->erase(0, nl + 1);
    }
  }
  Py_RETURN_NONE;
}

PyObject* k_command(PyObject*, PyObject* a) {
  const char* s;
  if (!PyArg_ParseTuple(a, "s", &s)) return nullptr;
  const bool interactive = g_engine->interactive;
  g_engine->interactive = false;  // negli script mancano i clic: un input mancante e' un errore
  const bool ok = g_engine->Run(s);
  g_engine->interactive = interactive;
  if (ok) Py_RETURN_TRUE;
  Py_RETURN_FALSE;
}

PyObject* k_last_error(PyObject*, PyObject*) { return PyUnicode_FromString(g_engine->LastError().c_str()); }
PyObject* k_last_created(PyObject*, PyObject*) { return IdList(g_engine->LastCreated()); }
PyObject* k_all_objects(PyObject*, PyObject* a) {
  int hidden = 0;
  if (!PyArg_ParseTuple(a, "|p", &hidden)) return nullptr;
  return IdList(g_engine->AllObjectIds(hidden != 0));
}
PyObject* k_selected(PyObject*, PyObject*) { return IdList(g_engine->Selection()); }
PyObject* k_select(PyObject*, PyObject* a) {
  PyObject* ids;
  int add = 1;
  if (!PyArg_ParseTuple(a, "O|p", &ids, &add)) return nullptr;
  if (!add) g_engine->ClearSelection();
  PyObject* seq = PySequence_Fast(ids, "lista di id attesa");
  if (!seq) return nullptr;
  int n = 0;
  for (Py_ssize_t i = 0; i < PySequence_Fast_GET_SIZE(seq); i++) {
    ON_UUID id;
    if (!ParseId(PySequence_Fast_GET_ITEM(seq, i), id)) { Py_DECREF(seq); return nullptr; }
    if (g_engine->Object(id)) { g_engine->Select(id); n++; }
  }
  Py_DECREF(seq);
  return PyLong_FromLong(n);
}
PyObject* k_unselect_all(PyObject*, PyObject*) {
  g_engine->ClearSelection();
  Py_RETURN_NONE;
}
PyObject* k_delete(PyObject*, PyObject* a) {
  PyObject* o;
  if (!PyArg_ParseTuple(a, "O", &o)) return nullptr;
  ON_UUID id;
  if (!ParseId(o, id)) return nullptr;
  if (g_engine->DeleteObject(id)) Py_RETURN_TRUE;
  Py_RETURN_FALSE;
}

// Tipi come rhinoscriptsyntax.ObjectType
PyObject* k_object_type(PyObject*, PyObject* a) {
  PyObject* o;
  if (!PyArg_ParseTuple(a, "O", &o)) return nullptr;
  ON_UUID id;
  if (!ParseId(o, id)) return nullptr;
  const ON_Geometry* g = Geo(id);
  long t = 0;
  if (!g) t = 0;
  else if (ON_Point::Cast(g)) t = 1;
  else if (ON_PointCloud::Cast(g)) t = 2;
  else if (ON_Curve::Cast(g)) t = 4;
  else if (const auto* b = ON_Brep::Cast(g)) t = b->m_F.Count() == 1 ? 8 : 16;
  else if (ON_Extrusion::Cast(g)) t = 1073741824;
  else if (ON_Surface::Cast(g)) t = 8;
  else if (ON_Mesh::Cast(g)) t = 32;
  else if (ON_Light::Cast(g)) t = 256;
  else if (ON_TextDot::Cast(g)) t = 8192;
  else if (ON_Annotation::Cast(g)) t = 512;
  else if (ON_InstanceRef::Cast(g)) t = 4096;
  else if (ON_SubD::Cast(g)) t = 262144;
  else if (ON_ClippingPlaneSurface::Cast(g)) t = 536870912;
  else if (ON_DetailView::Cast(g)) t = 32768;
  return PyLong_FromLong(t);
}

PyObject* k_bbox(PyObject*, PyObject* a) {
  PyObject* ids;
  if (!PyArg_ParseTuple(a, "O", &ids)) return nullptr;
  PyObject* seq = PySequence_Fast(ids, "lista di id attesa");
  if (!seq) return nullptr;
  ON_BoundingBox b;
  for (Py_ssize_t i = 0; i < PySequence_Fast_GET_SIZE(seq); i++) {
    ON_UUID id;
    if (!ParseId(PySequence_Fast_GET_ITEM(seq, i), id)) { Py_DECREF(seq); return nullptr; }
    if (const ON_Geometry* g = Geo(id)) { ON_BoundingBox t; if (g->GetTightBoundingBox(t)) b.Union(t); }
  }
  Py_DECREF(seq);
  if (!b.IsValid()) Py_RETURN_NONE;
  return Py_BuildValue("(dddddd)", b.m_min.x, b.m_min.y, b.m_min.z, b.m_max.x, b.m_max.y, b.m_max.z);
}

const ON_Curve* CurveArg(PyObject* o) {
  ON_UUID id;
  if (!ParseId(o, id)) return nullptr;
  const ON_Curve* c = ON_Curve::Cast(Geo(id));
  if (!c) PyErr_SetString(PyExc_ValueError, "l'oggetto non e' una curva");
  return c;
}

PyObject* k_curve_info(PyObject*, PyObject* a) {
  PyObject* o;
  if (!PyArg_ParseTuple(a, "O", &o)) return nullptr;
  const ON_Curve* c = CurveArg(o);
  if (!c) return nullptr;
  const double len = CurveLengthOf(*c);
  const ON_Interval d = c->Domain();
  return Py_BuildValue("{s:d,s:d,s:d,s:O,s:i,s:N,s:N}", "length", len, "t0", d[0], "t1", d[1], "closed",
                       c->IsClosed() ? Py_True : Py_False, "degree", c->Degree(), "start", Pt(c->PointAtStart()), "end",
                       Pt(c->PointAtEnd()));
}

PyObject* k_curve_eval(PyObject*, PyObject* a) {
  PyObject* o;
  double t;
  if (!PyArg_ParseTuple(a, "Od", &o, &t)) return nullptr;
  const ON_Curve* c = CurveArg(o);
  if (!c) return nullptr;
  ON_3dPoint p;
  ON_3dVector tan;
  if (!c->EvTangent(t, p, tan)) tan = ON_3dVector::ZeroVector;
  return Py_BuildValue("(NN)", Pt(p), Py_BuildValue("(ddd)", tan.x, tan.y, tan.z));
}

PyObject* k_curve_points(PyObject*, PyObject* a) {
  PyObject* o;
  if (!PyArg_ParseTuple(a, "O", &o)) return nullptr;
  const ON_Curve* c = CurveArg(o);
  if (!c) return nullptr;
  ON_NurbsCurve nc;
  if (!c->GetNurbForm(nc)) Py_RETURN_NONE;
  PyObject* l = PyList_New(nc.CVCount());
  for (int i = 0; i < nc.CVCount(); i++) {
    ON_3dPoint p;
    nc.GetCV(i, p);
    PyList_SET_ITEM(l, i, Pt(p));
  }
  return l;
}

PyObject* k_curve_closest(PyObject*, PyObject* a) {
  PyObject *o, *po;
  if (!PyArg_ParseTuple(a, "OO", &o, &po)) return nullptr;
  const ON_Curve* c = CurveArg(o);
  ON_3dPoint p;
  if (!c || !ParsePoint(po, p)) return nullptr;
  // campionamento e poi raffinamento a sezione aurea sull'intervallo migliore
  const ON_Interval d = c->Domain();
  const int n = 256;
  int best = 0;
  double bestD = ON_DBL_MAX;
  for (int i = 0; i <= n; i++) {
    const double dd = c->PointAt(d.ParameterAt((double)i / n)).DistanceTo(p);
    if (dd < bestD) { bestD = dd; best = i; }
  }
  double a0 = d.ParameterAt(std::max(0, best - 1) / (double)n), b0 = d.ParameterAt(std::min(n, best + 1) / (double)n);
  const double g = 0.5 * (std::sqrt(5.0) - 1);
  for (int it = 0; it < 100 && b0 - a0 > 1e-14 * (1 + std::fabs(b0)); it++) {
    const double x1 = b0 - g * (b0 - a0), x2 = a0 + g * (b0 - a0);
    if (c->PointAt(x1).DistanceTo(p) < c->PointAt(x2).DistanceTo(p)) b0 = x2; else a0 = x1;
  }
  // Newton su (C(t) - p) . C'(t) = 0
  double t = 0.5 * (a0 + b0);
  for (int it = 0; it < 8; it++) {
    ON_3dPoint q;
    ON_3dVector d1, d2;
    if (!c->Ev2Der(t, q, d1, d2)) break;
    const ON_3dVector r = q - p;
    const double f = r * d1, df = d1 * d1 + r * d2;
    if (std::fabs(df) < 1e-300) break;
    const double tn = std::clamp(t - f / df, d[0], d[1]);
    if (c->PointAt(tn).DistanceTo(p) > c->PointAt(t).DistanceTo(p) + 1e-15) break;
    const bool done = std::fabs(tn - t) < 1e-15 * (1 + std::fabs(t));
    t = tn;
    if (done) break;
  }
  return PyFloat_FromDouble(t);
}

PyObject* k_point_coords(PyObject*, PyObject* a) {
  PyObject* o;
  if (!PyArg_ParseTuple(a, "O", &o)) return nullptr;
  ON_UUID id;
  if (!ParseId(o, id)) return nullptr;
  if (const auto* p = ON_Point::Cast(Geo(id))) return Pt(p->point);
  Py_RETURN_NONE;
}

// attributi: nome, colore, livello
ON_3dmObjectAttributes* Attr(PyObject* o) {
  ON_UUID id;
  if (!ParseId(o, id)) return nullptr;
  ON_3dmObjectAttributes* a = g_engine->EditableAttributes(id);
  if (!a) PyErr_SetString(PyExc_ValueError, "oggetto inesistente");
  return a;
}

std::string LayerPath(const ON_Layer* l) {
  if (!l) return "";
  std::string name = ToUtf8(l->Name());
  ON_UUID parent = l->ParentLayerId();
  for (int depth = 0; depth < 64 && ON_UuidIsNotNil(parent); depth++) {
    const auto* pl = ON_Layer::Cast(g_engine->Model().ComponentFromId(ON_ModelComponent::Type::Layer, parent).ModelComponent());
    if (!pl) break;
    name = ToUtf8(pl->Name()) + "::" + name;
    parent = pl->ParentLayerId();
  }
  return name;
}

const ON_Layer* LayerByName(const std::string& name) {
  ONX_ModelComponentIterator it(g_engine->Model(), ON_ModelComponent::Type::Layer);
  for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent())
    if (const auto* l = ON_Layer::Cast(c))
      if (ON_wString::EqualOrdinal(l->Name(), ON_wString(name.c_str()), true) ||
          ON_wString::EqualOrdinal(ON_wString(LayerPath(l).c_str()), ON_wString(name.c_str()), true))
        return l;
  return nullptr;
}

PyObject* k_object_name(PyObject*, PyObject* a) {
  PyObject* o;
  const char* name = nullptr;
  if (!PyArg_ParseTuple(a, "O|z", &o, &name)) return nullptr;
  ON_3dmObjectAttributes* at = Attr(o);
  if (!at) return nullptr;
  const std::string old = ToUtf8(at->m_name);
  if (name) at->m_name = ON_wString(name);
  return PyUnicode_FromString(old.c_str());
}

PyObject* k_object_color(PyObject*, PyObject* a) {
  PyObject* o;
  int r = -1, g = -1, b = -1;
  if (!PyArg_ParseTuple(a, "O|iii", &o, &r, &g, &b)) return nullptr;
  ON_3dmObjectAttributes* at = Attr(o);
  if (!at) return nullptr;
  ON_Color c = at->m_color;
  if (at->ColorSource() != ON::color_from_object) {
    if (const auto* l = ON_Layer::Cast(g_engine->Model().ComponentFromIndex(ON_ModelComponent::Type::Layer, at->m_layer_index).ModelComponent()))
      c = l->Color();
  }
  if (r >= 0) {
    at->m_color = ON_Color(r, g, b);
    at->SetColorSource(ON::color_from_object);
  }
  return Py_BuildValue("(iii)", c.Red(), c.Green(), c.Blue());
}

PyObject* k_object_layer(PyObject*, PyObject* a) {
  PyObject* o;
  const char* name = nullptr;
  if (!PyArg_ParseTuple(a, "O|z", &o, &name)) return nullptr;
  ON_3dmObjectAttributes* at = Attr(o);
  if (!at) return nullptr;
  const auto* cur = ON_Layer::Cast(g_engine->Model().ComponentFromIndex(ON_ModelComponent::Type::Layer, at->m_layer_index).ModelComponent());
  const std::string old = LayerPath(cur);
  if (name) {
    const ON_Layer* l = LayerByName(name);
    if (!l) { PyErr_SetString(PyExc_ValueError, "livello inesistente"); return nullptr; }
    at->m_layer_index = l->Index();
  }
  return PyUnicode_FromString(old.c_str());
}

PyObject* k_add_layer(PyObject*, PyObject* a) {
  const char* name;
  int r = 0, g = 0, b = 0;
  if (!PyArg_ParseTuple(a, "s|iii", &name, &r, &g, &b)) return nullptr;
  if (!LayerByName(name)) g_engine->Model().AddLayer(ON_wString(name), ON_Color(r, g, b));
  return PyUnicode_FromString(name);
}

PyObject* k_current_layer(PyObject*, PyObject* a) {
  const char* name = nullptr;
  if (!PyArg_ParseTuple(a, "|z", &name)) return nullptr;
  ONX_Model& m = g_engine->Model();
  const auto* cur = ON_Layer::Cast(m.ComponentFromId(ON_ModelComponent::Type::Layer, m.m_settings.CurrentLayerId()).ModelComponent());
  const std::string old = LayerPath(cur);
  if (name) {
    const ON_Layer* l = LayerByName(name);
    if (!l) { PyErr_SetString(PyExc_ValueError, "livello inesistente"); return nullptr; }
    m.m_settings.SetCurrentLayerId(l->Id());
  }
  return PyUnicode_FromString(old.c_str());
}

PyObject* k_layer_names(PyObject*, PyObject*) {
  PyObject* l = PyList_New(0);
  ONX_ModelComponentIterator it(g_engine->Model(), ON_ModelComponent::Type::Layer);
  for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent())
    if (const auto* L = ON_Layer::Cast(c)) {
      PyObject* s = PyUnicode_FromString(LayerPath(L).c_str());
      PyList_Append(l, s);
      Py_DECREF(s);
    }
  return l;
}

// --- geometria diretta ---
PyObject* Add(ON_Object* g) { return Added(g_engine->AddObject(g)); }

PyObject* k_add_point(PyObject*, PyObject* a) {
  PyObject* po;
  ON_3dPoint p;
  if (!PyArg_ParseTuple(a, "O", &po) || !ParsePoint(po, p)) return nullptr;
  return Add(new ON_Point(p));
}

PyObject* k_add_polyline(PyObject*, PyObject* a) {
  PyObject* po;
  std::vector<ON_3dPoint> pts;
  if (!PyArg_ParseTuple(a, "O", &po) || !ParsePoints(po, pts)) return nullptr;
  if (pts.size() < 2) { PyErr_SetString(PyExc_ValueError, "servono almeno 2 punti"); return nullptr; }
  if (pts.size() == 2) return Add(new ON_LineCurve(pts[0], pts[1]));
  ON_Polyline pl;
  for (const auto& p : pts) pl.Append(p);
  return Add(new ON_PolylineCurve(pl));
}

bool ParsePlane(PyObject* o, ON_Plane& pl) {
  PyObject* seq = PySequence_Fast(o, "piano (origine, asse x, asse y) atteso");
  if (!seq) return false;
  if (PySequence_Fast_GET_SIZE(seq) < 3) { Py_DECREF(seq); PyErr_SetString(PyExc_ValueError, "piano non valido"); return false; }
  ON_3dPoint o0, x, y;
  const bool ok = ParsePoint(PySequence_Fast_GET_ITEM(seq, 0), o0) && ParsePoint(PySequence_Fast_GET_ITEM(seq, 1), x) &&
                  ParsePoint(PySequence_Fast_GET_ITEM(seq, 2), y);
  Py_DECREF(seq);
  if (!ok) return false;
  pl = ON_Plane(o0, ON_3dVector(x), ON_3dVector(y));
  if (!pl.IsValid()) { PyErr_SetString(PyExc_ValueError, "piano non valido"); return false; }
  return true;
}

PyObject* k_add_circle(PyObject*, PyObject* a) {
  PyObject* po;
  double r;
  ON_Plane pl;
  if (!PyArg_ParseTuple(a, "Od", &po, &r) || !ParsePlane(po, pl)) return nullptr;
  if (r <= 0) { PyErr_SetString(PyExc_ValueError, "raggio non positivo"); return nullptr; }
  return Add(new ON_ArcCurve(ON_Circle(pl, r)));
}

PyObject* k_add_arc(PyObject*, PyObject* a) {
  PyObject* po;
  double r, ang;
  ON_Plane pl;
  if (!PyArg_ParseTuple(a, "Odd", &po, &r, &ang) || !ParsePlane(po, pl)) return nullptr;
  return Add(new ON_ArcCurve(ON_Arc(pl, r, ang * ON_PI / 180.0)));
}

PyObject* k_add_arc3pt(PyObject*, PyObject* a) {
  PyObject *p0, *p1, *p2;
  ON_3dPoint s, e, m;
  if (!PyArg_ParseTuple(a, "OOO", &p0, &p1, &p2) || !ParsePoint(p0, s) || !ParsePoint(p1, e) || !ParsePoint(p2, m)) return nullptr;
  ON_Arc arc;
  if (!arc.Create(s, m, e)) { PyErr_SetString(PyExc_ValueError, "punti allineati"); return nullptr; }
  return Add(new ON_ArcCurve(arc));
}

PyObject* k_add_curve(PyObject*, PyObject* a) {
  PyObject* po;
  int degree = 3, interp = 0;
  std::vector<ON_3dPoint> pts;
  if (!PyArg_ParseTuple(a, "O|ip", &po, &degree, &interp) || !ParsePoints(po, pts)) return nullptr;
  if (pts.size() < 2) { PyErr_SetString(PyExc_ValueError, "servono almeno 2 punti"); return nullptr; }
  if (interp) {
    // curva interpolata: usa il comando InterpCrv (stesso risultato della riga di comando)
    std::string s = "_InterpCrv";
    char b[96];
    for (const auto& p : pts) { std::snprintf(b, sizeof b, " %.17g,%.17g,%.17g", p.x, p.y, p.z); s += b; }
    s += " _Enter";
    g_engine->interactive = false;
    if (!g_engine->Run(s)) { PyErr_SetString(PyExc_RuntimeError, g_engine->LastError().c_str()); return nullptr; }
    const auto& c = g_engine->LastCreated();
    return Added(c.empty() ? ON_nil_uuid : c.back());
  }
  const int order = std::max(2, std::min(degree, (int)pts.size() - 1) + 1);
  auto* nc = new ON_NurbsCurve();
  if (!nc->CreateClampedUniformNurbs(3, order, (int)pts.size(), pts.data())) { delete nc; Py_RETURN_NONE; }
  return Add(nc);
}

PyObject* k_add_box(PyObject*, PyObject* a) {
  PyObject* po;
  std::vector<ON_3dPoint> pts;
  if (!PyArg_ParseTuple(a, "O", &po) || !ParsePoints(po, pts)) return nullptr;
  if (pts.size() != 8) { PyErr_SetString(PyExc_ValueError, "servono 8 angoli"); return nullptr; }
  ON_Brep* b = ON_BrepBox(pts.data());
  if (!b) Py_RETURN_NONE;
  return Add(b);
}

PyObject* k_add_sphere(PyObject*, PyObject* a) {
  PyObject* po;
  double r;
  ON_3dPoint c;
  if (!PyArg_ParseTuple(a, "Od", &po, &r) || !ParsePoint(po, c)) return nullptr;
  if (r <= 0) { PyErr_SetString(PyExc_ValueError, "raggio non positivo"); return nullptr; }
  ON_Brep* b = ON_BrepSphere(ON_Sphere(c, r));
  return b ? Add(b) : Py_BuildValue("");
}

PyObject* k_add_cylinder(PyObject*, PyObject* a) {
  PyObject *po, *pv;
  double r;
  int cap = 1;
  ON_3dPoint base, axis;
  if (!PyArg_ParseTuple(a, "OOd|p", &po, &pv, &r, &cap) || !ParsePoint(po, base) || !ParsePoint(pv, axis)) return nullptr;
  ON_3dVector ax(axis);
  const double h = ax.Length();
  if (h <= 0 || r <= 0) { PyErr_SetString(PyExc_ValueError, "cilindro non valido"); return nullptr; }
  ON_Plane pl(base, ax);
  ON_Cylinder cyl(ON_Circle(pl, r), h);
  ON_Brep* b = ON_BrepCylinder(cyl, cap != 0, cap != 0);
  return b ? Add(b) : Py_BuildValue("");
}

PyObject* k_add_cone(PyObject*, PyObject* a) {
  PyObject *po, *pv;
  double r;
  int cap = 1;
  ON_3dPoint base, axis;
  if (!PyArg_ParseTuple(a, "OOd|p", &po, &pv, &r, &cap) || !ParsePoint(po, base) || !ParsePoint(pv, axis)) return nullptr;
  ON_3dVector ax(axis);
  const double h = ax.Length();
  if (h <= 0 || r <= 0) { PyErr_SetString(PyExc_ValueError, "cono non valido"); return nullptr; }
  // ON_Cone: vertice nell'origine del piano, base a distanza "height" lungo -z
  ON_Plane pl(base + ax, -ax);
  ON_Cone cone(pl, h, r);
  ON_Brep* b = ON_BrepCone(cone, cap != 0);
  return b ? Add(b) : Py_BuildValue("");
}

PyObject* k_add_torus(PyObject*, PyObject* a) {
  PyObject *po, *pn;
  double R, r;
  ON_3dPoint c, n;
  if (!PyArg_ParseTuple(a, "OOdd", &po, &pn, &R, &r) || !ParsePoint(po, c) || !ParsePoint(pn, n)) return nullptr;
  ON_Brep* b = ON_BrepTorus(ON_Torus(ON_Plane(c, ON_3dVector(n)), R, r));
  return b ? Add(b) : Py_BuildValue("");
}

PyObject* k_add_plane_surface(PyObject*, PyObject* a) {
  PyObject* po;
  double u, v;
  ON_Plane pl;
  if (!PyArg_ParseTuple(a, "Odd", &po, &u, &v) || !ParsePlane(po, pl)) return nullptr;
  auto* s = new ON_PlaneSurface(pl);
  s->SetExtents(0, ON_Interval(0, u), true);
  s->SetExtents(1, ON_Interval(0, v), true);
  return Add(s);
}

PyObject* k_add_text_dot(PyObject*, PyObject* a) {
  const char* text;
  PyObject* po;
  ON_3dPoint p;
  if (!PyArg_ParseTuple(a, "sO", &text, &po) || !ParsePoint(po, p)) return nullptr;
  auto* d = new ON_TextDot();
  d->SetCenterPoint(p);
  d->SetPrimaryText(ON_wString(text));
  return Add(d);
}

// Trasformazione (matrice 4x4 per righe) di oggetti; copy=True crea copie
PyObject* k_transform(PyObject*, PyObject* a) {
  PyObject *ids, *mx;
  int copy = 0;
  if (!PyArg_ParseTuple(a, "OO|p", &ids, &mx, &copy)) return nullptr;
  PyObject* seq = PySequence_Fast(mx, "matrice di 16 numeri attesa");
  if (!seq) return nullptr;
  if (PySequence_Fast_GET_SIZE(seq) != 16) { Py_DECREF(seq); PyErr_SetString(PyExc_ValueError, "matrice di 16 numeri attesa"); return nullptr; }
  ON_Xform x;
  for (int i = 0; i < 16; i++) x.m_xform[i / 4][i % 4] = PyFloat_AsDouble(PySequence_Fast_GET_ITEM(seq, i));
  Py_DECREF(seq);
  if (PyErr_Occurred()) return nullptr;
  PyObject* s2 = PySequence_Fast(ids, "lista di id attesa");
  if (!s2) return nullptr;
  std::vector<ON_UUID> out;
  for (Py_ssize_t i = 0; i < PySequence_Fast_GET_SIZE(s2); i++) {
    ON_UUID id;
    if (!ParseId(PySequence_Fast_GET_ITEM(s2, i), id)) { Py_DECREF(s2); return nullptr; }
    const auto* o = g_engine->Object(id);
    if (!o || !o->Geometry(nullptr)) continue;
    if (copy) {
      ON_Geometry* g = ON_Geometry::Cast(o->Geometry(nullptr)->Duplicate());
      if (!g || !g->Transform(x)) { delete g; continue; }
      ON_3dmObjectAttributes at = *o->Attributes(nullptr);
      out.push_back(g_engine->AddObject(g, &at));
    } else if (ON_Geometry* g = g_engine->EditableGeometry(id)) {
      if (g->Transform(x)) out.push_back(id);
    }
  }
  Py_DECREF(s2);
  return IdList(out);
}

PyObject* k_extrude(PyObject*, PyObject* a) {
  PyObject *o, *pv;
  ON_3dPoint v;
  if (!PyArg_ParseTuple(a, "OO", &o, &pv) || !ParsePoint(pv, v)) return nullptr;
  const ON_Curve* c = CurveArg(o);
  if (!c) return nullptr;
  if (ON_3dVector(v).Length() <= 0) { PyErr_SetString(PyExc_ValueError, "direzione nulla"); return nullptr; }
  auto* srf = new ON_SumSurface();
  if (!srf->Create(*c, ON_3dVector(v))) { delete srf; Py_RETURN_NONE; }
  ON_Brep* b = new ON_Brep();
  ON_Surface* s = srf;
  if (!b->Create(s)) { delete b; delete s; Py_RETURN_NONE; }
  return Add(b);
}

PyObject* k_mass_props(PyObject*, PyObject* a) {
  PyObject* o;
  int volume = 0;
  if (!PyArg_ParseTuple(a, "O|p", &o, &volume)) return nullptr;
  ON_UUID id;
  if (!ParseId(o, id)) return nullptr;
  double v = 0;
  ON_3dPoint c;
  if (!MassPropertiesFn || !MassPropertiesFn(*g_engine, id, volume != 0, v, c)) Py_RETURN_NONE;
  return Py_BuildValue("(dN)", v, Pt(c));
}

PyMethodDef kMethods[] = {
    {"mass_props", k_mass_props, METH_VARARGS, nullptr},
    {"extrude", k_extrude, METH_VARARGS, nullptr},
    {"write", k_write, METH_VARARGS, nullptr},
    {"command", k_command, METH_VARARGS, nullptr},
    {"last_error", k_last_error, METH_NOARGS, nullptr},
    {"last_created", k_last_created, METH_NOARGS, nullptr},
    {"all_objects", k_all_objects, METH_VARARGS, nullptr},
    {"selected", k_selected, METH_NOARGS, nullptr},
    {"select", k_select, METH_VARARGS, nullptr},
    {"unselect_all", k_unselect_all, METH_NOARGS, nullptr},
    {"delete", k_delete, METH_VARARGS, nullptr},
    {"object_type", k_object_type, METH_VARARGS, nullptr},
    {"bbox", k_bbox, METH_VARARGS, nullptr},
    {"curve_info", k_curve_info, METH_VARARGS, nullptr},
    {"curve_eval", k_curve_eval, METH_VARARGS, nullptr},
    {"curve_points", k_curve_points, METH_VARARGS, nullptr},
    {"curve_closest", k_curve_closest, METH_VARARGS, nullptr},
    {"point_coords", k_point_coords, METH_VARARGS, nullptr},
    {"object_name", k_object_name, METH_VARARGS, nullptr},
    {"object_color", k_object_color, METH_VARARGS, nullptr},
    {"object_layer", k_object_layer, METH_VARARGS, nullptr},
    {"add_layer", k_add_layer, METH_VARARGS, nullptr},
    {"current_layer", k_current_layer, METH_VARARGS, nullptr},
    {"layer_names", k_layer_names, METH_NOARGS, nullptr},
    {"add_point", k_add_point, METH_VARARGS, nullptr},
    {"add_polyline", k_add_polyline, METH_VARARGS, nullptr},
    {"add_circle", k_add_circle, METH_VARARGS, nullptr},
    {"add_arc", k_add_arc, METH_VARARGS, nullptr},
    {"add_arc3pt", k_add_arc3pt, METH_VARARGS, nullptr},
    {"add_curve", k_add_curve, METH_VARARGS, nullptr},
    {"add_box", k_add_box, METH_VARARGS, nullptr},
    {"add_sphere", k_add_sphere, METH_VARARGS, nullptr},
    {"add_cylinder", k_add_cylinder, METH_VARARGS, nullptr},
    {"add_cone", k_add_cone, METH_VARARGS, nullptr},
    {"add_torus", k_add_torus, METH_VARARGS, nullptr},
    {"add_plane_surface", k_add_plane_surface, METH_VARARGS, nullptr},
    {"add_text_dot", k_add_text_dot, METH_VARARGS, nullptr},
    {"transform", k_transform, METH_VARARGS, nullptr},
    {nullptr, nullptr, 0, nullptr}};

PyModuleDef kModule = {PyModuleDef_HEAD_INIT, "_kurva", "Motore di Kurva3D", -1, kMethods,
                       nullptr, nullptr, nullptr, nullptr};

PyObject* InitKurva() { return PyModule_Create(&kModule); }

bool g_initialized = false;
std::string g_initError;

bool EnsurePython() {
  if (g_initialized) return true;
  if (!g_initError.empty()) return false;
  PyImport_AppendInittab("_kurva", &InitKurva);
  PyConfig config;
  PyConfig_InitIsolatedConfig(&config);
  config.install_signal_handlers = 0;
  config.write_bytecode = 0;
  // distribuzione "embeddable" accanto al programma (Windows): pythonXY.zip con la libreria standard
  namespace fs = std::filesystem;
  std::error_code ec;
  if (!g_programDir.empty()) {
    for (const auto& ent : fs::directory_iterator(fs::path((const char8_t*)g_programDir.c_str()), ec)) {
      const std::string n = ent.path().filename().string();
      if (n.rfind("python3", 0) == 0 && n.size() > 4 && n.substr(n.size() - 4) == ".zip") {
        config.module_search_paths_set = 1;
        PyWideStringList_Append(&config.module_search_paths, ent.path().wstring().c_str());
        PyWideStringList_Append(&config.module_search_paths, fs::path((const char8_t*)g_programDir.c_str()).wstring().c_str());
        PyConfig_SetString(&config, &config.home, fs::path((const char8_t*)g_programDir.c_str()).wstring().c_str());
        break;
      }
    }
  }
  if (const char* home = std::getenv("KURVA3D_PYTHONHOME")) {
    PyConfig_SetBytesString(&config, &config.home, home);
  }
  PyStatus st = Py_InitializeFromConfig(&config);
  PyConfig_Clear(&config);
  if (PyStatus_Exception(st)) {
    g_initError = std::string("Python non avviato: ") + (st.err_msg ? st.err_msg : "errore sconosciuto");
    return false;
  }
  // rhinoscriptsyntax come modulo importabile
  PyObject* mod = PyImport_AddModule("rhinoscriptsyntax");  // riferimento preso in prestito
  PyObject* dict = PyModule_GetDict(mod);
  PyDict_SetItemString(dict, "__builtins__", PyEval_GetBuiltins());
  PyObject* r = PyRun_String(kRhinoscriptsyntax, Py_file_input, dict, dict);
  if (!r) {
    PyErr_Print();
    g_initError = "modulo rhinoscriptsyntax non caricato";
    return false;
  }
  Py_DECREF(r);
  g_initialized = true;
  return true;
}

}  // namespace

bool PythonAvailable() { return true; }
void SetPythonProgramDir(const std::string& dir) { g_programDir = dir; }

bool RunPython(Engine& e, const std::string& code, const std::string& filename, std::string& output, std::string& err) {
  g_engine = &e;
  g_output = &output;
  if (!EnsurePython()) { err = g_initError; g_output = nullptr; return false; }
  const bool interactive = e.interactive;
  e.interactive = false;
  // stdout/stderr dello script nella cronologia dei comandi
  PyObject* main = PyImport_AddModule("__main__");
  PyObject* globals = PyDict_New();
  PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins());
  PyDict_SetItemString(globals, "__name__", PyUnicode_FromString("__main__"));
  PyDict_SetItemString(globals, "__file__", PyUnicode_FromString(filename.c_str()));
  (void)main;
  PyRun_String("import sys, _kurva\n"
               "class _Out:\n"
               "    def write(self, s): _kurva.write(s)\n"
               "    def flush(self): pass\n"
               "sys.stdout = _Out(); sys.stderr = _Out()\n",
               Py_file_input, globals, globals);
  PyObject* compiled = Py_CompileString(code.c_str(), filename.c_str(), Py_file_input);
  bool ok = false;
  if (compiled) {
    PyObject* r = PyEval_EvalCode(compiled, globals, globals);
    Py_DECREF(compiled);
    if (r) { Py_DECREF(r); ok = true; }
  }
  if (!ok) {
    // traccia dell'errore come in Python
    PyObject *type, *value, *tb;
    PyErr_Fetch(&type, &value, &tb);
    PyErr_NormalizeException(&type, &value, &tb);
    std::string msg = "errore nello script";
    if (value) {
      PyObject* tbmod = PyImport_ImportModule("traceback");
      PyObject* fmt = tbmod ? PyObject_CallMethod(tbmod, "format_exception", "OOO", type, value, tb ? tb : Py_None) : nullptr;
      if (fmt) {
        PyObject* empty = PyUnicode_FromString("");
        PyObject* joined = PyUnicode_Join(empty, fmt);
        if (joined) { msg = PyUnicode_AsUTF8(joined); Py_DECREF(joined); }
        Py_DECREF(empty);
        Py_DECREF(fmt);
      } else if (PyObject* s = PyObject_Str(value)) {
        msg = PyUnicode_AsUTF8(s);
        Py_DECREF(s);
      }
      Py_XDECREF(tbmod);
      PyErr_Clear();
    }
    Py_XDECREF(type);
    Py_XDECREF(value);
    Py_XDECREF(tb);
    err = msg;
  }
  PyRun_String("import sys\nsys.stdout = sys.__stdout__; sys.stderr = sys.__stderr__\n", Py_file_input, globals, globals);
  Py_DECREF(globals);
  e.interactive = interactive;
  g_output = nullptr;
  return ok;
}

#else
bool PythonAvailable() { return false; }
void SetPythonProgramDir(const std::string&) {}
bool RunPython(Engine&, const std::string&, const std::string&, std::string&, std::string& err) {
  err = "questa versione di Kurva3D e' compilata senza Python";
  return false;
}
#endif

namespace {

std::string Unq(std::string s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
  return s;
}

// RunPythonScript "file.py"  oppure  -RunPythonScript (codice)
bool CmdRunPythonScript(Engine& e, TokenStream& ts) {
  auto a = e.ReadString(ts, "script Python (.py) o codice tra parentesi");
  if (!a) return false;
  std::string code, file = Unq(*a);
  if (!file.empty() && file.front() == '(') {
    // codice in linea: tutto fino alla parentesi di chiusura (anche su piu' token)
    std::string s = file;
    int depth = 0;
    for (char c : s) depth += c == '(' ? 1 : c == ')' ? -1 : 0;
    while (depth > 0 && !ts.AtEnd()) {
      const std::string t = ts.Next();
      s += " " + t;
      for (char c : t) depth += c == '(' ? 1 : c == ')' ? -1 : 0;
    }
    code = s.substr(1, s.size() >= 2 && s.back() == ')' ? s.size() - 2 : s.size() - 1);
    file = "<riga di comando>";
  } else {
    std::ifstream f(std::filesystem::path((const char8_t*)file.c_str()), std::ios::binary);
    if (!f) return e.Fail("impossibile leggere " + file);
    std::stringstream ss;
    ss << f.rdbuf();
    code = ss.str();
  }
  std::string out, err;
  const bool ok = RunPython(e, code, file, out, err);
  if (!out.empty()) e.Out() << "  " << out << "\n";  // ultima riga senza a capo
  if (!ok) {
    e.keepChangesOnFailure = true;  // come in Rhino: quanto fatto prima dell'errore resta
    return e.Fail(err);
  }
  return true;
}

// ScriptEditor ["file.py"]: editor di script della finestra
bool CmdScriptEditor(Engine& e, TokenStream& ts) {
  std::string file;
  if (!ts.AtEnd() && !IsEnter(ts.Peek())) file = Unq(ts.Next());
  if (!e.showScriptEditor) return e.Fail("l'editor di script e' disponibile solo nella finestra di Kurva3D");
  return e.showScriptEditor(file);
}

}  // namespace

void RegisterPythonCommands(Engine& e) {
  CommandInfo i{"RunPythonScript", "Script", "Esegue uno script Python (rhinoscriptsyntax)", CmdRunPythonScript};
  e.Register(i);
  CommandInfo ed{"ScriptEditor", "Script", "Editor di script Python", CmdScriptEditor, false};
  e.Register(ed);
}

}  // namespace cad
