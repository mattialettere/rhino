#include "core/DynamicInput.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace cad {

namespace {
std::string Lower(std::string s) {
  s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) { return c == '_' || c == '-' || c == ' '; }), s.end());
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
double Norm360(double a) {
  a = std::fmod(a, 360.0);
  if (a < 0) a += 360.0;
  if (a >= 360.0 - 1e-9) a = 0;
  return a;
}
bool FullNumber(const std::string& s, double& v) {
  if (s.empty()) return false;
  char* end = nullptr;
  v = std::strtod(s.c_str(), &end);
  return end && *end == '\0' && std::isfinite(v);
}
std::string Trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
}  // namespace

DynResult SolveDynamicPoint(const ON_Plane& cp, const ON_3dPoint& anchor, const ON_3dPoint& cursor,
                            DynMode mode, const DynFields& f, const DynOptions& o, bool snapped) {
  DynResult r;
  const ON_3dVector d = cursor - anchor;
  const double cu = d * cp.xaxis, cv = d * cp.yaxis;
  auto finish = [&](double u, double v) {
    r.dx = u;
    r.dy = v;
    r.length = std::hypot(u, v);
    r.angle = r.length > 1e-12 ? Norm360(std::atan2(v, u) * 180.0 / ON_PI) : (f.locked[1] && mode == DynMode::Polar ? Norm360(f.value[1]) : 0.0);
    r.point = anchor + cp.xaxis * u + cp.yaxis * v;
  };
  if (snapped && !f.AnyLocked()) {
    // un osnap vince: il punto e' esattamente quello agganciato (anche fuori dal piano)
    finish(cu, cv);
    r.point = cursor;
    return r;
  }
  if (mode == DynMode::Delta) {
    // valore digitato positivo: segue il lato del cursore (come trascinare l'angolo del rettangolo)
    auto signedValue = [](double typed, double cur) { return typed >= 0 && cur < 0 ? -typed : typed; };
    const double u = f.locked[0] ? signedValue(f.value[0], cu) : cu;
    const double v = f.locked[1] ? signedValue(f.value[1], cv) : cv;
    finish(u, v);
    return r;
  }
  // --- Polar ---
  const double curLen = std::hypot(cu, cv);
  const double curAng = curLen > 1e-12 ? Norm360(std::atan2(cv, cu) * 180.0 / ON_PI) : 0.0;
  double ang = curAng;
  bool angleFixed = false;
  if (f.locked[1]) {
    ang = Norm360(f.value[1]);
    angleFixed = true;
  } else if (curLen > 1e-12 && (o.ortho || (o.polarTracking && o.polarStep > 0))) {
    const double step = o.ortho ? 90.0 : o.polarStep;
    const double snappedAng = std::round(curAng / step) * step;
    const double diff = std::fabs(curAng - snappedAng);
    if (o.ortho || diff <= o.polarTolerance) {
      ang = Norm360(snappedAng);
      angleFixed = true;
      r.tracked = true;
      r.trackAngle = ang;
    }
  }
  const double ar = ang * ON_PI / 180.0;
  const double ux = std::cos(ar), uy = std::sin(ar);
  double len;
  if (f.locked[0]) len = f.value[0];
  else if (angleFixed) len = cu * ux + cv * uy;  // proiezione del cursore sulla direzione
  else len = curLen;
  finish(len * ux, len * uy);
  if (r.length < 1e-12) r.angle = ang;
  return r;
}

DynTyped ParseDynamicText(const std::string& text, double& value) {
  const std::string t = Trim(text);
  value = 0;
  if (t.empty()) return DynTyped::Empty;
  if (t[0] == '<') return FullNumber(Trim(t.substr(1)), value) ? DynTyped::Angle : DynTyped::Other;
  return FullNumber(t, value) ? DynTyped::Number : DynTyped::Other;
}

DynMode DynamicModeFor(const std::string& commandName) {
  const std::string n = Lower(commandName);
  if (n == "rectangle" || n == "box" || n == "plane") return DynMode::Delta;
  return DynMode::Polar;
}

const char* DynamicLengthLabel(const std::string& commandName) {
  const std::string n = Lower(commandName);
  if (n == "circle" || n == "sphere" || n == "cylinder" || n == "cone" || n == "polygon" || n == "arc" ||
      n == "torus" || n == "tcone")
    return "R";
  return "L";
}

std::string PointToken(const ON_3dPoint& p) {
  char buf[128];
  auto c = [](double v) { return std::fabs(v) < 1e-12 ? 0.0 : v; };
  std::snprintf(buf, sizeof(buf), "%.12g,%.12g,%.12g", c(p.x), c(p.y), c(p.z));
  return buf;
}

bool AutoDimensionCommand(const std::string& commandName) {
  const std::string n = Lower(commandName);
  return n == "line" || n == "polyline" || n == "rectangle" || n == "polygon" || n == "lines";
}

std::vector<std::string> AutoDimensionCommands(const ON_Geometry* g, double offset) {
  std::vector<std::string> out;
  const ON_Curve* c = ON_Curve::Cast(g);
  if (!c || offset <= 0) return out;
  ON_SimpleArray<ON_3dPoint> pts;
  if (!c->IsPolyline(&pts) || pts.Count() < 2) {
    if (const auto* lc = ON_LineCurve::Cast(c)) {
      pts.Empty();
      pts.Append(lc->m_line.from);
      pts.Append(lc->m_line.to);
    } else {
      return out;
    }
  }
  const bool closed = c->IsClosed() && pts.Count() > 3;
  const int n = closed ? pts.Count() - 1 : pts.Count();
  // area con segno nel piano XY: per piu' di due punti le quote vanno verso l'esterno della figura
  double area = 0;
  for (int i = 0; i < n; i++) {
    const ON_3dPoint& a = pts[i];
    const ON_3dPoint& b = pts[(i + 1) % n];
    area += a.x * b.y - b.x * a.y;
  }
  const bool outward = n >= 3 && std::fabs(area) > 1e-12;
  const int segs = closed ? n : n - 1;
  for (int i = 0; i < segs; i++) {
    const ON_3dPoint a = pts[i], b = pts[(i + 1) % pts.Count()];
    ON_3dVector dir = b - a;
    dir.z = 0;
    if (dir.Length() < 1e-9) continue;
    dir.Unitize();
    ON_3dVector nrm(-dir.y, dir.x, 0);  // a sinistra della direzione
    if (outward) {
      if (area > 0) nrm = -nrm;  // antiorario: l'esterno e' a destra
    } else if (nrm.y < -1e-9 || (std::fabs(nrm.y) <= 1e-9 && nrm.x > 0)) {
      nrm = -nrm;  // segmento singolo: quota sopra (orizzontale) o a sinistra (verticale)
    }
    const ON_3dPoint mid = 0.5 * (a + b) + nrm * offset;
    // testo leggibile: la quota va da sinistra a destra (o dal basso verso l'alto)
    const bool flip = dir.x < -1e-9 || (std::fabs(dir.x) <= 1e-9 && dir.y < 0);
    const ON_3dPoint& p1 = flip ? b : a;
    const ON_3dPoint& p2 = flip ? a : b;
    out.push_back("-_DimAligned " + PointToken(p1) + " " + PointToken(p2) + " " + PointToken(mid));
  }
  return out;
}

}  // namespace cad
