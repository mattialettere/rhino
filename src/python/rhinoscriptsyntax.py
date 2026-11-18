# rhinoscriptsyntax per Kurva3D: sottoinsieme con gli stessi nomi e argomenti di Rhino 8.
# Gli id degli oggetti sono stringhe (in Rhino sono System.Guid); punti e vettori sono
# Point3d/Vector3d con X, Y, Z, indici e operazioni aritmetiche come in RhinoCommon.
import math as _math
import sys as _sys
import types as _types
import _kurva


# ------------------------------------------------------------------ tipi geometrici
class Point3d(object):
    __slots__ = ("X", "Y", "Z")

    def __init__(self, x=0.0, y=0.0, z=0.0):
        self.X = float(x); self.Y = float(y); self.Z = float(z)

    def __getitem__(self, i):
        return (self.X, self.Y, self.Z)[i]

    def __setitem__(self, i, v):
        if i == 0 or i == -3: self.X = float(v)
        elif i == 1 or i == -2: self.Y = float(v)
        elif i == 2 or i == -1: self.Z = float(v)
        else: raise IndexError(i)

    def __len__(self):
        return 3

    def __iter__(self):
        return iter((self.X, self.Y, self.Z))

    def __add__(self, o):
        o = _xyz(o); return self.__class__(self.X + o[0], self.Y + o[1], self.Z + o[2])
    __radd__ = __add__

    def __sub__(self, o):
        # punto - punto = vettore, punto - vettore = punto (come RhinoCommon)
        cls = Vector3d if not isinstance(self, Vector3d) and not isinstance(o, Vector3d) else self.__class__
        o = _xyz(o)
        return cls(self.X - o[0], self.Y - o[1], self.Z - o[2])

    def __rsub__(self, o):
        o = _xyz(o); return self.__class__(o[0] - self.X, o[1] - self.Y, o[2] - self.Z)

    def __mul__(self, f):
        if isinstance(f, (int, float)):
            return self.__class__(self.X * f, self.Y * f, self.Z * f)
        o = _xyz(f); return self.X * o[0] + self.Y * o[1] + self.Z * o[2]
    __rmul__ = __mul__

    def __truediv__(self, f):
        return self.__class__(self.X / f, self.Y / f, self.Z / f)

    def __neg__(self):
        return self.__class__(-self.X, -self.Y, -self.Z)

    def __eq__(self, o):
        try:
            o = _xyz(o)
        except Exception:
            return False
        return self.X == o[0] and self.Y == o[1] and self.Z == o[2]

    def __ne__(self, o):
        return not self.__eq__(o)

    def __hash__(self):
        return hash((self.X, self.Y, self.Z))

    def __repr__(self):
        return "%s,%s,%s" % (_fmt(self.X), _fmt(self.Y), _fmt(self.Z))
    __str__ = __repr__

    def DistanceTo(self, o):
        o = _xyz(o)
        return _math.sqrt((self.X - o[0]) ** 2 + (self.Y - o[1]) ** 2 + (self.Z - o[2]) ** 2)


class Vector3d(Point3d):
    __slots__ = ()

    @property
    def Length(self):
        return _math.sqrt(self.X * self.X + self.Y * self.Y + self.Z * self.Z)

    def Unitize(self):
        l = self.Length
        if l == 0: return False
        self.X /= l; self.Y /= l; self.Z /= l
        return True


class Plane(object):
    def __init__(self, origin, xaxis, yaxis):
        self.Origin = Point3d(*_xyz(origin))
        x = VectorUnitize(xaxis)
        z = VectorUnitize(VectorCrossProduct(x, yaxis))
        y = VectorCrossProduct(z, x)
        self.XAxis, self.YAxis, self.ZAxis = x, y, z

    @property
    def Normal(self):
        return self.ZAxis

    def PointAt(self, u, v, w=0.0):
        return self.Origin + self.XAxis * u + self.YAxis * v + self.ZAxis * w

    # sequenza (origine, asse x, asse y) per il motore
    def __iter__(self):
        return iter((self.Origin, self.XAxis, self.YAxis))

    def __len__(self):
        return 3

    def __getitem__(self, i):
        return (self.Origin, self.XAxis, self.YAxis, self.ZAxis)[i]

    def __repr__(self):
        return "Origin=%r XAxis=%r, YAxis=%r, ZAxis=%r" % (self.Origin, self.XAxis, self.YAxis, self.ZAxis)


def _fmt(v):
    s = "%.15g" % v
    return "0" if s == "-0" else s


def _xyz(p):
    if isinstance(p, Point3d):
        return (p.X, p.Y, p.Z)
    if isinstance(p, str):
        v = [float(t) for t in p.split(",")]
        p = v
    p = list(p)
    if len(p) == 2:
        p.append(0.0)
    if len(p) != 3:
        raise ValueError("punto o vettore non valido: %r" % (p,))
    return (float(p[0]), float(p[1]), float(p[2]))


def _pt(p):
    return None if p is None else Point3d(*_xyz(p))


def _ids(ids):
    if ids is None:
        return []
    if isinstance(ids, str):
        return [ids]
    return [str(i) for i in ids]


def _plane(p):
    if isinstance(p, Plane):
        return p
    if p is None:
        return WorldXYPlane()
    p = list(p)
    if len(p) >= 3 and not isinstance(p[0], (int, float)):
        return Plane(p[0], p[1], p[2])
    # un punto: piano XY del mondo con origine nel punto
    pl = WorldXYPlane()
    pl.Origin = _pt(p)
    return pl


# ------------------------------------------------------------------ punti e vettori
def CreatePoint(point, y=None, z=None):
    if y is not None:
        return Point3d(point, y, z or 0.0)
    return _pt(point)


def CreateVector(vector, y=None, z=None):
    if y is not None:
        return Vector3d(vector, y, z or 0.0)
    return Vector3d(*_xyz(vector))


def CreatePlane(plane_or_origin, x_axis=None, y_axis=None, ignored=None):
    if x_axis is None:
        return _plane(plane_or_origin)
    return Plane(plane_or_origin, x_axis, y_axis)


def VectorAdd(vector1, vector2):
    a, b = _xyz(vector1), _xyz(vector2); return Vector3d(a[0] + b[0], a[1] + b[1], a[2] + b[2])


def VectorSubtract(vector1, vector2):
    a, b = _xyz(vector1), _xyz(vector2); return Vector3d(a[0] - b[0], a[1] - b[1], a[2] - b[2])


def VectorScale(vector, scale):
    a = _xyz(vector); return Vector3d(a[0] * scale, a[1] * scale, a[2] * scale)


def VectorLength(vector):
    a = _xyz(vector); return _math.sqrt(a[0] ** 2 + a[1] ** 2 + a[2] ** 2)


def VectorUnitize(vector):
    v = Vector3d(*_xyz(vector))
    return v if v.Unitize() else None


def VectorReverse(vector):
    return -Vector3d(*_xyz(vector))


def VectorCreate(to_point, from_point):
    return VectorSubtract(to_point, from_point)


def VectorCrossProduct(vector1, vector2):
    a, b = _xyz(vector1), _xyz(vector2)
    return Vector3d(a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def VectorDotProduct(vector1, vector2):
    a, b = _xyz(vector1), _xyz(vector2); return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def VectorAngle(vector1, vector2):
    a, b = VectorUnitize(vector1), VectorUnitize(vector2)
    if a is None or b is None: return None
    d = max(-1.0, min(1.0, VectorDotProduct(a, b)))
    return _math.degrees(_math.acos(d))


def VectorRotate(vector, angle_degrees, axis):
    v, k = Vector3d(*_xyz(vector)), VectorUnitize(axis)
    if k is None: return None
    t = _math.radians(angle_degrees)
    c, s = _math.cos(t), _math.sin(t)
    return v * c + VectorCrossProduct(k, v) * s + k * (VectorDotProduct(k, v) * (1 - c))


def PointAdd(point1, point2):
    a, b = _xyz(point1), _xyz(point2); return Point3d(a[0] + b[0], a[1] + b[1], a[2] + b[2])


def PointSubtract(point1, point2):
    a, b = _xyz(point1), _xyz(point2); return Vector3d(a[0] - b[0], a[1] - b[1], a[2] - b[2])


def PointScale(point, scale):
    a = _xyz(point); return Point3d(a[0] * scale, a[1] * scale, a[2] * scale)


def PointCompare(point1, point2, tolerance=None):
    return Distance(point1, point2) <= (tolerance if tolerance is not None else 1.0e-12)


def Distance(point1, point2):
    return _pt(point1).DistanceTo(point2)


def WorldXYPlane():
    return Plane((0, 0, 0), (1, 0, 0), (0, 1, 0))


def WorldYZPlane():
    return Plane((0, 0, 0), (0, 1, 0), (0, 0, 1))


def WorldZXPlane():
    return Plane((0, 0, 0), (0, 0, 1), (1, 0, 0))


def PlaneFromPoints(origin, x, y):
    return Plane(origin, VectorSubtract(x, origin), VectorSubtract(y, origin))


def PlaneFromNormal(origin, normal, xaxis=None):
    n = VectorUnitize(normal)
    if xaxis is None:
        ref = (1, 0, 0) if abs(n[0]) < 0.9 else (0, 1, 0)
        xaxis = VectorCrossProduct(VectorCrossProduct(n, ref), n)
    else:
        xaxis = VectorSubtract(xaxis, VectorScale(n, VectorDotProduct(xaxis, n)))
    return Plane(origin, xaxis, VectorCrossProduct(n, xaxis))


def MovePlane(plane, origin):
    pl = _plane(plane)
    pl.Origin = _pt(origin)
    return pl


# ------------------------------------------------------------------ comandi e documento
def Command(commandString, echo=True):
    return _kurva.command(commandString)


def LastCommandResult():
    return 0


def LastCreatedObjects(select=False):
    ids = _kurva.last_created()
    if select and ids:
        _kurva.select(ids, True)
    return ids or None


def Print(message=None):
    print("" if message is None else message)


def Redraw():
    pass


def EnableRedraw(enable=True):
    return True


def Sleep(milliseconds):
    pass


def _run_on(ids, cmd):
    _kurva.unselect_all()
    _kurva.select(_ids(ids), True)
    ok = _kurva.command(cmd)
    _kurva.unselect_all()
    if not ok:
        return None
    return list(_kurva.last_created())


# ------------------------------------------------------------------ selezione
def AllObjects(select=False, include_lights=False, include_grips=False, include_references=False):
    ids = [i for i in _kurva.all_objects(False) if include_lights or _kurva.object_type(i) != 256]
    if select:
        _kurva.select(ids, True)
    return ids


def NormalObjects(include_lights=False, include_grips=False):
    return AllObjects(False, include_lights)


def HiddenObjects(include_lights=False, include_grips=False, include_references=True):
    visible = set(_kurva.all_objects(False))
    return [i for i in _kurva.all_objects(True) if i not in visible]


def SelectedObjects(include_lights=False, include_grips=False):
    return list(_kurva.selected())


def FirstObject(select=False, include_lights=False, include_grips=False):
    ids = AllObjects(False, include_lights)
    if not ids: return None
    if select: _kurva.select([ids[0]], True)
    return ids[0]


def LastObject(select=False, include_lights=False, include_grips=False):
    ids = AllObjects(False, include_lights)
    if not ids: return None
    if select: _kurva.select([ids[-1]], True)
    return ids[-1]


def SelectObject(object_id, redraw=True):
    return _kurva.select([str(object_id)], True) == 1


def SelectObjects(object_ids):
    return _kurva.select(_ids(object_ids), True)


def UnselectAllObjects():
    n = len(_kurva.selected())
    _kurva.unselect_all()
    return n


def UnselectObject(object_id):
    rest = [i for i in _kurva.selected() if i != str(object_id)]
    _kurva.select(rest, False)
    return True


def UnselectObjects(object_ids):
    out = set(_ids(object_ids))
    rest = [i for i in _kurva.selected() if i not in out]
    _kurva.select(rest, False)
    return len(out)


def IsObjectSelected(object_id):
    return 1 if str(object_id) in _kurva.selected() else 0


def _filtered(ids, filter):
    if not filter:
        return ids
    return [i for i in ids if _kurva.object_type(i) & filter]


# Negli script non c'e' un clic: GetObject/GetObjects restituiscono gli oggetti gia' selezionati.
def GetObject(message=None, filter=0, preselect=False, select=False, custom_filter=None, subobjects=False):
    ids = _filtered(_kurva.selected(), filter)
    return ids[0] if ids else None


def GetObjects(message=None, filter=0, group=True, preselect=False, select=False, objects=None,
               minimum_count=1, maximum_count=0, custom_filter=None):
    ids = _filtered(_kurva.selected(), filter)
    if objects is not None:
        allowed = set(_ids(objects))
        ids = [i for i in ids if i in allowed]
    return ids or None


def ObjectsByType(geometry_type, select=False, state=0):
    ids = _filtered(_kurva.all_objects(state == 0 or bool(state & 4)), geometry_type)
    if select: _kurva.select(ids, True)
    return ids


def ObjectsByLayer(layer_name, select=False):
    ids = [i for i in _kurva.all_objects(False) if _kurva.object_layer(i).lower() == layer_name.lower()]
    if select: _kurva.select(ids, True)
    return ids


def ObjectsByName(name, select=False, include_lights=True, include_references=False):
    ids = [i for i in _kurva.all_objects(False) if _kurva.object_name(i) == name]
    if select: _kurva.select(ids, True)
    return ids


# ------------------------------------------------------------------ oggetti
filter = _types.SimpleNamespace(allobjects=0, point=1, pointcloud=2, curve=4, surface=8, polysurface=16,
                                mesh=32, light=256, annotation=512, instance=4096, textdot=8192,
                                grip=16384, detail=32768, hatch=65536, morph=131072, subd=262144,
                                cage=134217728, phantom=268435456, clippingplane=536870912,
                                extrusion=1073741824)


def ObjectType(object_id):
    return _kurva.object_type(str(object_id))


def IsObject(object_id):
    return str(object_id) in _kurva.all_objects(True)


def IsObjectHidden(object_id):
    return IsObject(object_id) and str(object_id) not in _kurva.all_objects(False)


def IsPoint(object_id): return ObjectType(object_id) == 1
def IsPointCloud(object_id): return ObjectType(object_id) == 2
def IsCurve(object_id): return ObjectType(object_id) == 4
def IsSurface(object_id): return ObjectType(object_id) in (8, 1073741824)
def IsPolysurface(object_id): return ObjectType(object_id) == 16
def IsBrep(object_id): return ObjectType(object_id) in (8, 16, 1073741824)
def IsMesh(object_id): return ObjectType(object_id) == 32
def IsLight(object_id): return ObjectType(object_id) == 256
def IsTextDot(object_id): return ObjectType(object_id) == 8192
def IsBlockInstance(object_id): return ObjectType(object_id) == 4096


def IsPolysurfaceClosed(object_id):
    r = _kurva.mass_props(str(object_id), True)
    return bool(r and abs(r[0]) > 0)


def DeleteObject(object_id):
    return _kurva.delete(str(object_id))


def DeleteObjects(object_ids):
    return sum(1 for i in _ids(object_ids) if _kurva.delete(i))


def ObjectName(object_id, name=None):
    old = _kurva.object_name(str(object_id), name)
    return old if old else None


def ObjectColor(object_ids, color=None):
    ids = _ids(object_ids)
    if color is not None:
        c = _color(color)
        for i in ids:
            _kurva.object_color(i, c[0], c[1], c[2])
        return len(ids)
    return _kurva.object_color(ids[0])


def ObjectLayer(object_id, layer=None):
    if isinstance(object_id, (list, tuple)):
        for i in object_id:
            _kurva.object_layer(str(i), layer)
        return len(object_id)
    return _kurva.object_layer(str(object_id), layer)


def _color(c):
    if isinstance(c, int):
        return (c & 255, (c >> 8) & 255, (c >> 16) & 255)
    if hasattr(c, "R"):
        return (c.R, c.G, c.B)
    c = list(c)
    return (int(c[0]), int(c[1]), int(c[2]))


def CreateColor(color, g=None, b=None, a=None):
    if g is not None:
        return (int(color), int(g), int(b))
    return _color(color)


def BoundingBox(objects, view_or_plane=None, in_world_coords=True):
    b = _kurva.bbox(_ids(objects))
    if b is None:
        return None
    x0, y0, z0, x1, y1, z1 = b
    return [Point3d(x0, y0, z0), Point3d(x1, y0, z0), Point3d(x1, y1, z0), Point3d(x0, y1, z0),
            Point3d(x0, y0, z1), Point3d(x1, y0, z1), Point3d(x1, y1, z1), Point3d(x0, y1, z1)]


# ------------------------------------------------------------------ livelli
def AddLayer(name=None, color=None, visible=True, locked=False, parent=None):
    if name is None:
        n = 1
        names = set(l.lower() for l in _kurva.layer_names())
        while ("livello %02d" % n) in names: n += 1
        name = "Livello %02d" % n
    if parent:
        name = parent + "::" + name
    c = _color(color) if color is not None else (0, 0, 0)
    if "::" in name:
        _kurva.command('-_Layer _New "%s" _Enter' % name)
    else:
        _kurva.add_layer(name, c[0], c[1], c[2])
    return name


def CurrentLayer(layer=None):
    return _kurva.current_layer(layer)


def LayerNames(sort=False):
    names = list(_kurva.layer_names())
    return sorted(names) if sort else names


def IsLayer(layer):
    l = layer.lower()
    return any(n.lower() == l or n.lower().split("::")[-1] == l for n in _kurva.layer_names())


def LayerCount():
    return len(_kurva.layer_names())


# ------------------------------------------------------------------ creazione
def AddPoint(point, y=None, z=None):
    p = CreatePoint(point, y, z)
    return _kurva.add_point(p)


def AddPoints(points):
    return [AddPoint(p) for p in points]


def AddLine(start, end):
    return _kurva.add_polyline([_pt(start), _pt(end)])


def AddPolyline(points, replace_id=None):
    pts = [_pt(p) for p in points]
    if len(pts) < 2:
        return None
    if len(pts) == 2:
        # una polilinea di due punti resta una polilinea (in Rhino AddPolyline non crea linee)
        pts = [pts[0], pts[1]]
    r = _kurva.add_polyline(pts)
    if replace_id is not None and r:
        _kurva.delete(str(replace_id))
    return r


def AddCircle(plane_or_center, radius):
    return _kurva.add_circle(_plane(plane_or_center), float(radius))


def AddCircle3Pt(first, second, third):
    a, b, c = _pt(first), _pt(second), _pt(third)
    ab, ac = b - a, c - a
    n = VectorCrossProduct(ab, ac)
    nn = VectorDotProduct(n, n)
    if nn == 0:
        return None
    center = a + (VectorCrossProduct(n, ab) * VectorDotProduct(ac, ac) + VectorCrossProduct(ac, n) * VectorDotProduct(ab, ab)) / (2 * nn)
    pl = Plane(center, a - center, VectorCrossProduct(n, a - center))
    return _kurva.add_circle(pl, Distance(center, a))


def AddArc(plane, radius, angle_degrees):
    return _kurva.add_arc(_plane(plane), float(radius), float(angle_degrees))


def AddArc3Pt(start, end, point_on_arc):
    return _kurva.add_arc3pt(_pt(start), _pt(end), _pt(point_on_arc))


def AddEllipse(plane, radiusX, radiusY):
    pl = _plane(plane)
    c, x, y = pl.Origin, pl.XAxis, pl.YAxis
    return _cmd_created('_Ellipse %s %s %s' % (_p(c), _p(c + x * radiusX), _p(c + y * radiusY)))


def AddCurve(points, degree=3):
    return _kurva.add_curve([_pt(p) for p in points], int(degree), False)


def AddInterpCurve(points, degree=3, knotstyle=0, start_tangent=None, end_tangent=None):
    return _kurva.add_curve([_pt(p) for p in points], int(degree), True)


def AddRectangle(plane, width, height):
    pl = _plane(plane)
    o, x, y = pl.Origin, pl.XAxis * width, pl.YAxis * height
    return _kurva.add_polyline([o, o + x, o + x + y, o + y, o])


def AddBox(corners):
    pts = [_pt(p) for p in corners]
    return _kurva.add_box(pts)


def AddSphere(center_or_plane, radius):
    c = center_or_plane.Origin if isinstance(center_or_plane, Plane) else _pt(center_or_plane)
    return _kurva.add_sphere(c, float(radius))


def _base_axis(base, height):
    if isinstance(base, Plane):
        o, z = base.Origin, base.ZAxis
    else:
        o, z = _pt(base), Vector3d(0, 0, 1)
    if isinstance(height, (int, float)):
        axis = z * float(height)
    else:
        axis = _pt(height) - o
    return o, axis


def AddCylinder(base, height, radius, cap=True):
    o, axis = _base_axis(base, height)
    return _kurva.add_cylinder(o, axis, float(radius), bool(cap))


def AddCone(base, height, radius, cap=True):
    o, axis = _base_axis(base, height)
    return _kurva.add_cone(o, axis, float(radius), bool(cap))


def AddTorus(base, major_radius, minor_radius, direction=None):
    if isinstance(base, Plane):
        o, n = base.Origin, base.ZAxis
    else:
        o, n = _pt(base), Vector3d(0, 0, 1)
    if direction is not None:
        n = _pt(direction) - o
    return _kurva.add_torus(o, n, float(major_radius), float(minor_radius))


def AddPlaneSurface(plane, u_dir, v_dir):
    return _kurva.add_plane_surface(_plane(plane), float(u_dir), float(v_dir))


def AddTextDot(text, point):
    return _kurva.add_text_dot(str(text), _pt(point))


def _p(p):
    p = _xyz(p)
    return "%.17g,%.17g,%.17g" % p


def _cmd_created(cmd):
    if not _kurva.command(cmd):
        return None
    ids = _kurva.last_created()
    return ids[-1] if ids else None


def AddText(text, point_or_plane, height=1.0, font=None, font_style=0, justification=None):
    pt = point_or_plane.Origin if isinstance(point_or_plane, Plane) else _pt(point_or_plane)
    t = str(text).replace('"', "'").replace("\n", "\\n")
    return _cmd_created('_Text %s "%s" Height=%.17g' % (_p(pt), t, float(height)))


def AddLoftSrf(object_ids, start=None, end=None, loft_type=0, simplify_method=0, value=0, closed=False):
    style = " _Style=_Straight" if loft_type == 2 else ""
    return _run_on(object_ids, "_Loft" + style)


def ExtrudeCurveStraight(curve_id, start_point, end_point):
    return _kurva.extrude(str(curve_id), _pt(end_point) - _pt(start_point))


def CapPlanarHoles(surface_id):
    r = _run_on([surface_id], "_Cap")
    return bool(r)


def _first(ids):
    return ids[0] if ids else None


def BooleanUnion(input, delete_input=True):
    return _run_on(input, "_BooleanUnion")


def BooleanDifference(input0, input1, delete_input=True):
    return _run_on(_ids(input0) + _ids(input1), "_BooleanDifference")


def BooleanIntersection(input0, input1, delete_input=True):
    return _run_on(_ids(input0) + _ids(input1), "_BooleanIntersection")


def JoinCurves(object_ids, delete_input=False, tolerance=None):
    return _run_on(object_ids, "_Join")


def ExplodeCurves(curve_ids, delete_input=False):
    return _run_on(curve_ids, "_Explode")


def OffsetCurve(object_id, direction, distance, normal=None, style=1):
    return _run_on([object_id], "_Offset %.17g %s" % (float(distance), _p(direction)))


# ------------------------------------------------------------------ trasformazioni
def XformTranslation(vector):
    v = _xyz(vector)
    return [1, 0, 0, v[0], 0, 1, 0, v[1], 0, 0, 1, v[2], 0, 0, 0, 1]


def XformRotation2(angle_degrees, rotation_axis, center_point):
    k = VectorUnitize(rotation_axis)
    t = _math.radians(angle_degrees)
    c, s, C = _math.cos(t), _math.sin(t), 1 - _math.cos(t)
    x, y, z = k
    r = [[c + x * x * C, x * y * C - z * s, x * z * C + y * s],
         [y * x * C + z * s, c + y * y * C, y * z * C - x * s],
         [z * x * C - y * s, z * y * C + x * s, c + z * z * C]]
    p = _xyz(center_point)
    m = []
    for i in range(3):
        m += r[i] + [p[i] - (r[i][0] * p[0] + r[i][1] * p[1] + r[i][2] * p[2])]
    return m + [0, 0, 0, 1]


def XformScale(scale, point=None):
    if isinstance(scale, (int, float)):
        scale = (scale, scale, scale)
    sx, sy, sz = _xyz(scale)
    p = _xyz(point) if point is not None else (0, 0, 0)
    return [sx, 0, 0, p[0] * (1 - sx), 0, sy, 0, p[1] * (1 - sy), 0, 0, sz, p[2] * (1 - sz), 0, 0, 0, 1]


def XformMirror(mirror_plane_point, mirror_plane_normal):
    n = VectorUnitize(mirror_plane_normal)
    p = _xyz(mirror_plane_point)
    d = VectorDotProduct(n, p)
    m = []
    for i in range(3):
        row = [(1.0 if i == j else 0.0) - 2 * n[i] * n[j] for j in range(3)]
        m += row + [2 * d * n[i]]
    return m + [0, 0, 0, 1]


def _flat(m):
    if hasattr(m, "__len__") and len(m) == 4 and hasattr(m[0], "__len__"):
        return [float(v) for row in m for v in row]
    return [float(v) for v in m]


def TransformObjects(object_ids, matrix, copy=False):
    return list(_kurva.transform(_ids(object_ids), _flat(matrix), bool(copy)))


def TransformObject(object_id, matrix, copy=False):
    return _first(TransformObjects([object_id], matrix, copy))


def MoveObjects(object_ids, translation):
    return TransformObjects(object_ids, XformTranslation(translation))


def MoveObject(object_id, translation):
    return _first(MoveObjects([object_id], translation))


def CopyObjects(object_ids, translation=None):
    return TransformObjects(object_ids, XformTranslation(translation or (0, 0, 0)), True)


def CopyObject(object_id, translation=None):
    return _first(CopyObjects([object_id], translation))


def RotateObjects(object_ids, center_point, rotation_angle, axis=None, copy=False):
    return TransformObjects(object_ids, XformRotation2(rotation_angle, axis or (0, 0, 1), center_point), copy)


def RotateObject(object_id, center_point, rotation_angle, axis=None, copy=False):
    return _first(RotateObjects([object_id], center_point, rotation_angle, axis, copy))


def ScaleObjects(object_ids, origin, scale, copy=False):
    return TransformObjects(object_ids, XformScale(scale, origin), copy)


def ScaleObject(object_id, origin, scale, copy=False):
    return _first(ScaleObjects([object_id], origin, scale, copy))


def MirrorObjects(object_ids, start_point, end_point, copy=False):
    a, b = _pt(start_point), _pt(end_point)
    n = VectorCrossProduct(b - a, (0, 0, 1))
    return TransformObjects(object_ids, XformMirror(a, n), copy)


def MirrorObject(object_id, start_point, end_point, copy=False):
    return _first(MirrorObjects([object_id], start_point, end_point, copy))


# ------------------------------------------------------------------ curve
def _curve(curve_id):
    return _kurva.curve_info(str(curve_id))


def CurveLength(curve_id, segment_index=-1, sub_domain=None):
    return _curve(curve_id)["length"]


def CurveDomain(curve_id, segment_index=-1):
    c = _curve(curve_id)
    return [c["t0"], c["t1"]]


def CurveStartPoint(curve_id, segment_index=-1, point=None):
    return Point3d(*_curve(curve_id)["start"])


def CurveEndPoint(curve_id, segment_index=-1):
    return Point3d(*_curve(curve_id)["end"])


def CurveMidPoint(curve_id, segment_index=-1):
    c = _curve(curve_id)
    return EvaluateCurve(curve_id, 0.5 * (c["t0"] + c["t1"]))


def CurveDegree(curve_id, segment_index=-1):
    return _curve(curve_id)["degree"]


def IsCurveClosed(object_id):
    return bool(_curve(object_id)["closed"])


def EvaluateCurve(curve_id, t, segment_index=-1):
    return Point3d(*_kurva.curve_eval(str(curve_id), float(t))[0])


def CurveTangent(curve_id, parameter, segment_index=-1):
    return Vector3d(*_kurva.curve_eval(str(curve_id), float(parameter))[1])


def CurvePoints(curve_id, segment_index=-1):
    pts = _kurva.curve_points(str(curve_id))
    return None if pts is None else [Point3d(*p) for p in pts]


def CurveClosestPoint(curve_id, point, segment_index=-1):
    return _kurva.curve_closest(str(curve_id), _pt(point))


def DivideCurve(curve_id, segments, create_points=False, return_points=True):
    c = _curve(curve_id)
    n = int(segments)
    length = c["length"]
    # parametri a lunghezza d'arco uguale (bisezione sulla lunghezza parziale)
    pts = [Point3d(*c["start"])]
    if n > 0:
        t0, t1 = c["t0"], c["t1"]
        samples = 400
        ts = [t0 + (t1 - t0) * i / samples for i in range(samples + 1)]
        ps = [EvaluateCurve(curve_id, t) for t in ts]
        acc = [0.0]
        for i in range(1, len(ps)):
            acc.append(acc[-1] + ps[i].DistanceTo(ps[i - 1]))
        scale = length / acc[-1] if acc[-1] > 0 else 1.0
        j = 0
        for k in range(1, n):
            target = length * k / n / scale
            while j < samples and acc[j + 1] < target:
                j += 1
            seg = acc[j + 1] - acc[j]
            f = (target - acc[j]) / seg if seg > 0 else 0
            pts.append(EvaluateCurve(curve_id, ts[j] + f * (ts[j + 1] - ts[j])))
        pts.append(Point3d(*c["end"]) if not c["closed"] else Point3d(*c["start"]))
        if c["closed"]:
            pts.pop()
    if create_points:
        for p in pts:
            AddPoint(p)
    return pts if return_points else None


def PointCoordinates(object_id, point=None):
    old = _kurva.point_coords(str(object_id))
    if old is None:
        return None
    if point is not None:
        d = _pt(point) - Point3d(*old)
        MoveObject(object_id, d)
    return Point3d(*old)


# ------------------------------------------------------------------ misure
def Area(object_id):
    r = _kurva.mass_props(str(object_id), False)
    return None if r is None else r[0]


def SurfaceArea(object_id):
    r = _kurva.mass_props(str(object_id), False)
    return None if r is None else [r[0], 0.0]


def SurfaceAreaCentroid(object_id):
    r = _kurva.mass_props(str(object_id), False)
    return None if r is None else [Point3d(*r[1]), (0.0, 0.0, 0.0)]


def SurfaceVolume(object_id):
    r = _kurva.mass_props(str(object_id), True)
    return None if r is None else [r[0], 0.0]


def SurfaceVolumeCentroid(object_id):
    r = _kurva.mass_props(str(object_id), True)
    return None if r is None else [Point3d(*r[1]), (0.0, 0.0, 0.0)]


# ------------------------------------------------------------------ varie
def UnitSystem(unit_system=None, scale=False, in_model_units=True):
    return 2  # millimetri


def UnitAbsoluteTolerance(tolerance=None, in_model_units=True):
    return 0.001


def coerce3dpoint(point, raise_on_error=False):
    try:
        return _pt(point)
    except Exception:
        if raise_on_error: raise
        return None


def coerce3dvector(vector, raise_on_error=False):
    try:
        return Vector3d(*_xyz(vector))
    except Exception:
        if raise_on_error: raise
        return None


def coerceplane(plane, raise_on_error=False):
    try:
        return _plane(plane)
    except Exception:
        if raise_on_error: raise
        return None


def coerceguid(id, raise_on_error=False):
    return str(id) if id is not None else None


# moduli accessori: "import scriptcontext" e "import Rhino" minimi
_sc = _types.ModuleType("scriptcontext")
_sc.doc = None
_sc.id = 1
_sc.sticky = {}
_sc.escape_test = lambda throw_exception=True, reset=False: False
_sys.modules["scriptcontext"] = _sc
_geometry = _types.ModuleType("Rhino.Geometry")
_geometry.Point3d = Point3d
_geometry.Vector3d = Vector3d
_geometry.Plane = Plane
_rhino = _types.ModuleType("Rhino")
_rhino.Geometry = _geometry
_sys.modules["Rhino"] = _rhino
_sys.modules["Rhino.Geometry"] = _geometry
