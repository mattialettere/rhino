// DXF: lettura completa (testo ASCII e binario, R12...2018) e scrittura R12.
//
// In lettura, come Rhino 8, il file diventa un modello con:
//   livelli (colore, tipo di linea, spessore, acceso/spento, bloccato), tipi di linea,
//   blocchi veri (definizioni + istanze, annidati), testi e quote vere (ON_Text,
//   ON_DimLinear, ON_DimRadial, ON_DimAngular, ON_Leader), stili di quota,
//   retini (ON_Hatch con il motivo del file), colori/tipi di linea/spessori per oggetto,
//   gruppi, attributi dei blocchi (testo utente), entita' dello spazio carta (tavola).
// Le geometrie ACIS (3DSOLID, REGION, BODY), le immagini e i riferimenti esterni non
// sono letti: il messaggio finale le conta (vedi DIFFERENZE.md).
#include "commands/Annotation.h"
#include "commands/Engine.h"
#include "commands/Exchange.h"
#include "commands/ExchangeUtil.h"
#include "core/Document3dm.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>

namespace cad {
using namespace xu;
namespace {
namespace fs = std::filesystem;

struct DxfPair {
  int code;
  std::string value;
};

// --- Codifica dei testi -----------------------------------------------------------------------
void AppendUtf8(std::string& o, uint32_t cp) {
  if (cp < 0x80) o += (char)cp;
  else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
  else if (cp < 0x10000) {
    o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F));
  } else {
    o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F));
    o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F));
  }
}

bool IsValidUtf8(const std::string& s) {
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = (unsigned char)s[i];
    const int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
    if (n < 0 || i + n >= s.size() + (n == 0 ? 1 : 0)) return false;
    for (int k = 1; k <= n; k++)
      if (((unsigned char)s[i + k] >> 6) != 2) return false;
    i += n + 1;
  }
  return true;
}

// Windows-1252 (codepage dei DXF europei fino ad AutoCAD 2006) -> UTF-8.
std::string Cp1252ToUtf8(const std::string& s) {
  static const uint16_t hi[32] = {0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                  0x2039, 0x0152, 0x8D, 0x017D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                  0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178};
  std::string o;
  for (unsigned char c : s) {
    if (c < 0x80) o += (char)c;
    else if (c < 0xA0) AppendUtf8(o, hi[c - 0x80]);
    else AppendUtf8(o, c);
  }
  return o;
}

// Sequenze \U+XXXX (caratteri Unicode nei DXF) -> UTF-8.
std::string DecodeUnicodeEscapes(const std::string& s) {
  if (s.find("\\U+") == std::string::npos && s.find("\\u+") == std::string::npos) return s;
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\\' && i + 6 < s.size() + 0 && (s[i + 1] == 'U' || s[i + 1] == 'u') && s[i + 2] == '+') {
      const std::string hex = s.substr(i + 3, 4);
      if (std::all_of(hex.begin(), hex.end(), [](char c) { return std::isxdigit((unsigned char)c); })) {
        AppendUtf8(o, (uint32_t)std::stoul(hex, nullptr, 16));
        i += 6;
        continue;
      }
    }
    o += s[i];
  }
  return o;
}

double Num(const std::string& s) {
  try { return std::stod(Trim(s)); } catch (...) { return 0; }
}
int Int(const std::string& s) {
  try { return (int)std::stol(Trim(s)); } catch (...) { return 0; }
}

// --- Lettura delle coppie (codice, valore) ----------------------------------------------------
bool ParseTextPairs(const std::string& data, std::vector<DxfPair>& pairs, std::string& err) {
  size_t pos = 0;
  auto line = [&](std::string& out) {
    if (pos >= data.size()) return false;
    size_t e = data.find('\n', pos);
    if (e == std::string::npos) e = data.size();
    out.assign(data, pos, e - pos);
    if (!out.empty() && out.back() == '\r') out.pop_back();
    pos = e + 1;
    return true;
  };
  std::string a, b;
  while (line(a) && line(b)) {
    const std::string t = Trim(a);
    if (t.empty()) continue;
    char* end = nullptr;
    const long code = std::strtol(t.c_str(), &end, 10);
    if (!end || *end) { err = "file DXF non valido (codice di gruppo \"" + t.substr(0, 20) + "\")"; return false; }
    pairs.push_back({(int)code, b});
    if (code == 0 && Trim(b) == "EOF") break;
  }
  return true;
}

// DXF binario: codici a 1 byte (R12) o 2 byte (R13+), valori tipizzati per codice.
bool ParseBinaryPairs(const std::string& data, std::vector<DxfPair>& pairs, std::string& err) {
  size_t i = 22;
  // versione: dopo $ACADVER, 'A' subito = codici a 1 byte
  bool r12 = true;
  if (size_t v = data.find("$ACADVER", 22); v != std::string::npos && v < 1024) {
    size_t s = v + 9 + 1;  // stringa + terminatore, poi il codice
    if (s < data.size() && data[s] != 'A') s++;
    if (s + 6 <= data.size()) r12 = data.substr(s, 6) <= "AC1009";
  }
  auto in = [](int c, int a, int b) { return c >= a && c <= b; };
  auto need = [&](size_t n) { return i + n <= data.size(); };
  while (i < data.size()) {
    int code;
    if (r12) {
      if (!need(1)) break;
      code = (unsigned char)data[i];
      if (code == 255) {
        if (!need(3)) break;
        code = (unsigned char)data[i + 1] | ((unsigned char)data[i + 2] << 8);
        i += 3;
      } else i += 1;
    } else {
      if (!need(2)) break;
      code = (unsigned char)data[i] | ((unsigned char)data[i + 1] << 8);
      i += 2;
    }
    std::string value;
    char buf[64];
    if (in(code, 310, 319) || code == 1004) {  // dati binari: lunghezza + byte (non servono)
      if (!need(1)) break;
      i += 1 + (unsigned char)data[i];
      continue;
    } else if (in(code, 60, 79) || in(code, 170, 179) || in(code, 270, 289) || in(code, 370, 389) ||
               in(code, 400, 409) || in(code, 1060, 1070)) {
      if (!need(2)) break;
      int16_t v;
      std::memcpy(&v, data.data() + i, 2);
      i += 2;
      value = std::to_string(v);
    } else if (in(code, 10, 59) || in(code, 110, 149) || in(code, 210, 239) || in(code, 460, 469) ||
               in(code, 1010, 1059)) {
      if (!need(8)) break;
      double v;
      std::memcpy(&v, data.data() + i, 8);
      i += 8;
      std::snprintf(buf, sizeof buf, "%.17g", v);
      value = buf;
    } else if (in(code, 90, 99) || in(code, 420, 429) || in(code, 440, 459) || code == 1071) {
      if (!need(4)) break;
      int32_t v;
      std::memcpy(&v, data.data() + i, 4);
      i += 4;
      value = std::to_string(v);
    } else if (in(code, 160, 169)) {
      if (!need(8)) break;
      int64_t v;
      std::memcpy(&v, data.data() + i, 8);
      i += 8;
      value = std::to_string(v);
    } else if (in(code, 290, 299)) {
      if (!need(1)) break;
      value = std::to_string((int)(unsigned char)data[i]);
      i += 1;
    } else {
      const size_t e = data.find('\0', i);
      if (e == std::string::npos) { err = "DXF binario troncato"; return false; }
      value.assign(data, i, e - i);
      i = e + 1;
    }
    pairs.push_back({code, std::move(value)});
    if (code == 0 && pairs.back().value == "EOF") break;
  }
  if (pairs.empty()) { err = "DXF binario non valido"; return false; }
  return true;
}

bool ReadPairs(const std::string& path, std::vector<DxfPair>& pairs, std::string& err) {
  std::ifstream f(fs::u8path(path), std::ios::binary);
  if (!f) { err = "impossibile aprire " + path; return false; }
  std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  bool ok;
  if (data.compare(0, 18, "AutoCAD Binary DXF") == 0) ok = ParseBinaryPairs(data, pairs, err);
  else {
    if (data.size() >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF)
      data.erase(0, 3);  // BOM UTF-8
    ok = ParseTextPairs(data, pairs, err);
  }
  if (!ok) return false;
  // testi: UTF-8 dalla R2007 (AC1021), prima la codepage del file (quasi sempre ANSI_1252)
  for (auto& p : pairs) {
    if (p.value.empty()) continue;
    if (!IsValidUtf8(p.value)) p.value = Cp1252ToUtf8(p.value);
    p.value = DecodeUnicodeEscapes(p.value);
  }
  return true;
}

// --- Entita' ----------------------------------------------------------------------------------
struct Entity {
  std::string type;
  std::vector<DxfPair> p;
  const DxfPair* Find(int code) const {
    for (const auto& q : p) if (q.code == code) return &q;
    return nullptr;
  }
  bool Has(int code) const { return Find(code) != nullptr; }
  double D(int code, double def = 0) const { const DxfPair* q = Find(code); return q ? Num(q->value) : def; }
  int I(int code, int def = 0) const { const DxfPair* q = Find(code); return q ? Int(q->value) : def; }
  std::string S(int code) const { const DxfPair* q = Find(code); return q ? q->value : std::string(); }
  std::vector<double> All(int code) const {
    std::vector<double> v;
    for (const auto& q : p) if (q.code == code) v.push_back(Num(q.value));
    return v;
  }
  ON_3dVector Extrusion() const { return ON_3dVector(D(210, 0), D(220, 0), D(230, 1)); }
  ON_3dPoint P(int c) const { return ON_3dPoint(D(c), D(c + 10), D(c + 20)); }
  // Dati estesi di un'applicazione (codici >= 1000 dopo 1001 <app>).
  std::vector<DxfPair> XData(const std::string& app) const {
    std::vector<DxfPair> r;
    bool in = false;
    for (const auto& q : p) {
      if (q.code == 1001) { in = Upper(Trim(q.value)) == app; continue; }
      if (in && q.code >= 1000) r.push_back(q);
    }
    return r;
  }
};

ON::LengthUnitSystem InsUnits(int v) {
  switch (v) {
    case 1: return ON::LengthUnitSystem::Inches;
    case 2: return ON::LengthUnitSystem::Feet;
    case 3: return ON::LengthUnitSystem::Miles;
    case 4: return ON::LengthUnitSystem::Millimeters;
    case 5: return ON::LengthUnitSystem::Centimeters;
    case 6: return ON::LengthUnitSystem::Meters;
    case 7: return ON::LengthUnitSystem::Kilometers;
    case 8: return ON::LengthUnitSystem::Microinches;
    case 9: return ON::LengthUnitSystem::Mils;
    case 10: return ON::LengthUnitSystem::Yards;
    case 11: return ON::LengthUnitSystem::Angstroms;
    case 12: return ON::LengthUnitSystem::Nanometers;
    case 13: return ON::LengthUnitSystem::Microns;
    case 14: return ON::LengthUnitSystem::Decimeters;
    case 15: return ON::LengthUnitSystem::Dekameters;
    case 16: return ON::LengthUnitSystem::Hectometers;
    case 17: return ON::LengthUnitSystem::Gigameters;
    case 18: return ON::LengthUnitSystem::AstronomicalUnits;
    case 19: return ON::LengthUnitSystem::LightYears;
    case 20: return ON::LengthUnitSystem::Parsecs;
    default: return ON::LengthUnitSystem::None;
  }
}

int InsUnitsCode(ON::LengthUnitSystem u) {
  for (int v = 1; v <= 20; v++) if (InsUnits(v) == u) return v;
  return 0;
}

// Segmento di polilinea con rigonfiamento (bulge) -> arco.
ON_Curve* BulgeSegment(const ON_3dPoint& a, const ON_3dPoint& b, double bulge, const ON_Plane& ocs) {
  const ON_3dPoint A = OcsToWcs(ocs, a), B = OcsToWcs(ocs, b);
  if (std::fabs(bulge) < 1e-12 || a.DistanceTo(b) < 1e-12) return new ON_LineCurve(A, B);
  const ON_2dVector chord(b.x - a.x, b.y - a.y);
  const ON_2dVector perp(chord.y, -chord.x);  // a destra della corda
  const double sagitta = bulge * chord.Length() / 2.0;
  ON_2dVector pu = perp;
  pu.Unitize();
  const ON_3dPoint mid((a.x + b.x) / 2 + pu.x * sagitta, (a.y + b.y) / 2 + pu.y * sagitta, a.z);
  ON_Arc arc;
  if (!arc.Create(A, OcsToWcs(ocs, mid), B)) return new ON_LineCurve(A, B);
  return new ON_ArcCurve(arc);
}

ON_Curve* JoinSegments(std::vector<ON_Curve*>& segs) {
  if (segs.empty()) return nullptr;
  if (segs.size() == 1) { ON_Curve* c = segs[0]; segs.clear(); return c; }
  // solo segmenti rettilinei: una polilinea, come Rhino
  bool allLines = true;
  for (ON_Curve* c : segs) if (!ON_LineCurve::Cast(c)) { allLines = false; break; }
  if (allLines) {
    ON_Polyline pl;
    pl.Append(segs[0]->PointAtStart());
    for (ON_Curve* c : segs) { pl.Append(c->PointAtEnd()); delete c; }
    segs.clear();
    return new ON_PolylineCurve(pl);
  }
  auto* pc = new ON_PolyCurve();
  for (ON_Curve* c : segs) pc->Append(c);
  segs.clear();
  return pc;
}

ON_Xform OcsXform(const ON_Plane& ocs) {
  ON_Xform x(ON_Xform::IdentityTransformation);
  for (int r = 0; r < 3; r++) { x.m_xform[r][0] = ocs.xaxis[r]; x.m_xform[r][1] = ocs.yaxis[r]; x.m_xform[r][2] = ocs.zaxis[r]; }
  return x;
}

// Testo MTEXT/TEXT senza i codici di formattazione (\P a capo, {\fArial;...}, %%d gradi...).
std::string DxfPlainText(const std::string& in) {
  std::string o;
  for (size_t i = 0; i < in.size(); i++) {
    const char c = in[i];
    if (c == '\\' && i + 1 < in.size()) {
      const char k = in[i + 1];
      if (k == 'P' || k == 'n') { o += '\n'; i++; }
      else if (k == '~') { o += ' '; i++; }
      else if (k == '\\' || k == '{' || k == '}') { o += k; i++; }
      else if (std::strchr("LlOoKkNn", k)) { i++; }
      else if (k == 'S') {  // frazioni impilate \S1^2; -> 1/2
        size_t e = in.find(';', i + 2);
        if (e == std::string::npos) e = in.size();
        std::string f = in.substr(i + 2, e - i - 2);
        for (auto& ch : f) if (ch == '^' || ch == '#') ch = '/';
        o += f;
        i = e;
      } else if (std::strchr("fFHCcAWQTp", k)) {  // codici con parametro fino a ';'
        const size_t e = in.find(';', i + 2);
        i = e == std::string::npos ? in.size() : e;
      } else { o += c; }
    } else if (c == '{' || c == '}') {
    } else if (c == '%' && i + 2 < in.size() && in[i + 1] == '%') {
      const char k = (char)std::tolower((unsigned char)in[i + 2]);
      if (k == 'd') o += "\xC2\xB0";        // gradi
      else if (k == 'p') o += "\xC2\xB1";   // piu' o meno
      else if (k == 'c') o += "\xC3\x98";   // diametro
      else if (k == '%') o += '%';
      else if (k == 'u' || k == 'o') {}    // sottolineato/sopralineato: ignorati
      else if (std::isdigit((unsigned char)k) && i + 4 < in.size()) {  // %%nnn: codice del carattere
        AppendUtf8(o, (uint32_t)std::atoi(in.substr(i + 2, 3).c_str()));
        i += 2;
      } else { o += in.substr(i, 3); }
      i += 2;
    } else {
      o += c;
    }
  }
  return o;
}

// Punto dentro un poligono (pari-dispari).
bool InsidePolygon(const std::vector<ON_2dPoint>& poly, const ON_2dPoint& p) {
  bool in = false;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
    const ON_2dPoint& a = poly[i];
    const ON_2dPoint& b = poly[j];
    if (((a.y > p.y) != (b.y > p.y)) && (p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)) in = !in;
  }
  return in;
}

std::vector<ON_2dPoint> SampleLoop(const ON_Curve& c) {
  std::vector<ON_2dPoint> v;
  const ON_Interval d = c.Domain();
  const int n = std::max(16, 8 * std::max(1, c.SpanCount()));
  for (int k = 0; k < n; k++) {
    const ON_3dPoint p = c.PointAt(d.ParameterAt((double)k / n));
    v.emplace_back(p.x, p.y);
  }
  return v;
}

ON_DimStyle::suppress_zero ZeroSuppress(int dimzin) {
  const bool lead = dimzin & 4, trail = dimzin & 8;
  return lead && trail ? ON_DimStyle::suppress_zero::SuppressLeadingAndTrailing
       : lead ? ON_DimStyle::suppress_zero::SuppressLeading
       : trail ? ON_DimStyle::suppress_zero::SuppressTrailing : ON_DimStyle::suppress_zero::None;
}

// Variabile di quota DXF (codice del DIMSTYLE) applicata a uno stile.
void ApplyDimVar(ON_DimStyle& ds, int code, const std::string& v) {
  const double d = Num(v);
  switch (code) {
    case 40: if (d > 0) ds.SetDimScale(d); break;               // DIMSCALE
    case 41: if (d > 0) ds.SetArrowSize(d); break;              // DIMASZ
    case 42: ds.SetExtOffset(std::fabs(d)); break;              // DIMEXO
    case 44: ds.SetExtExtension(std::fabs(d)); break;           // DIMEXE
    case 140: if (d > 0) ds.SetTextHeight(d); break;            // DIMTXT
    case 147: ds.SetTextGap(std::fabs(d)); break;               // DIMGAP
    case 144: if (d != 0) ds.SetLengthFactor(std::fabs(d)); break;  // DIMLFAC
    case 271: ds.SetLengthResolution(std::clamp((int)d, 0, 7)); break;  // DIMDEC
    case 278: if ((int)d > 0) ds.SetDecimalSeparator((wchar_t)(int)d); break;  // DIMDSEP
    case 77: ds.SetDimTextLocation(d != 0 ? ON_DimStyle::TextLocation::AboveDimLine : ON_DimStyle::TextLocation::InDimLine); break;
    case 142: if (d > 0) ds.SetArrowType1And2(ON_Arrowhead::arrow_type::Tick); break;  // DIMTSZ
    case 78: ds.SetZeroSuppress(ZeroSuppress((int)d)); break;   // DIMZIN
    case 179: ds.SetAngleResolution(std::clamp((int)d, 0, 7)); break;  // DIMADEC
    case 3: {  // DIMPOST: prefisso<>suffisso
      const size_t k = v.find("<>");
      if (k == std::string::npos) ds.SetSuffix(ON_wString(v.c_str()));
      else { ds.SetPrefix(ON_wString(v.substr(0, k).c_str())); ds.SetSuffix(ON_wString(v.substr(k + 2).c_str())); }
      break;
    }
    default: break;
  }
}

// Stesse variabili come sostituzioni sulla singola annotazione (dati estesi ACAD DSTYLE).
void ApplyDimVar(ON_Annotation& a, const ON_DimStyle& parent, int code, const std::string& v) {
  const double d = Num(v);
  switch (code) {
    case 40: if (d > 0) a.SetDimScale(&parent, d); break;
    case 41: if (d > 0) a.SetArrowSize(&parent, d); break;
    case 42: a.SetExtensionLineOffset(&parent, std::fabs(d)); break;
    case 44: a.SetExtensionLineExtension(&parent, std::fabs(d)); break;
    case 140: if (d > 0) a.SetTextHeight(&parent, d); break;
    case 147: a.SetTextGap(&parent, std::fabs(d)); break;
    case 144: if (d != 0) a.SetLengthFactor(&parent, std::fabs(d)); break;
    case 271: a.SetLengthResolution(&parent, std::clamp((int)d, 0, 7)); break;
    case 278: if ((int)d > 0) a.SetDecimalSeparator(&parent, (wchar_t)(int)d); break;
    case 77: a.SetDimTextLocation(&parent, d != 0 ? ON_DimStyle::TextLocation::AboveDimLine : ON_DimStyle::TextLocation::InDimLine); break;
    case 142: if (d > 0) a.SetArrowType1And2(&parent, ON_Arrowhead::arrow_type::Tick); break;
    case 78: a.SetZeroSuppress(&parent, ZeroSuppress((int)d)); break;
    case 179: a.SetAngleResolution(&parent, std::clamp((int)d, 0, 7)); break;
    default: break;
  }
}

// Fattore di scala medio di una trasformazione (lunghezza dell'asse Y trasformato).
double XformScale(const ON_Xform& xf, const ON_3dVector& dir = ON_3dVector::YAxis) {
  ON_3dVector v = xf * dir;
  const double l = v.Length();
  return l > 0 ? l : 1.0;
}

// Lettore: stato condiviso tra le sezioni.
class DxfReader {
 public:
  DxfReader(ImportedModel& out, ON::LengthUnitSystem docUnits) : m_out(out), m_docUnits(docUnits) {}

  bool Read(const std::string& path, std::string& err) {
    std::vector<DxfPair> pairs;
    if (!ReadPairs(path, pairs, err)) return false;
    Split(pairs);
    m_out.units = m_fileUnits;
    m_scale = (m_fileUnits != ON::LengthUnitSystem::None && m_docUnits != ON::LengthUnitSystem::None)
                  ? ON::UnitScale(m_fileUnits, m_docUnits) : 1.0;
    m_S = ON_Xform::DiagonalTransformation(m_scale, m_scale, m_scale);
    m_Sinv = ON_Xform::DiagonalTransformation(1 / m_scale, 1 / m_scale, 1 / m_scale);
    MakeTables();
    Convert(m_entities, m_S, Parent(), 0, m_out.objects, false);
    AssignGroups();
    if (m_out.objects.empty() && m_out.blocks.empty()) {
      err = m_skipped.empty() ? "il file DXF non contiene entita'" : "nessuna entita' DXF supportata nel file";
      if (!m_skipped.empty()) err += " (" + SkippedText() + ")";
      return false;
    }
    std::string note = SkippedText();
    if (!note.empty()) m_out.note = "non importate: " + note;
    return true;
  }

 private:
  struct Block {
    std::string name, description;
    ON_3dPoint base;
    int flags = 0;
    std::vector<Entity> ents;
  };
  struct Style {  // stile di testo
    double height = 0, width = 1;
    std::string font;  // famiglia TrueType (vuoto = carattere predefinito)
  };
  // Contesto dell'inserimento (per il livello "0" e i colori DaBlocco)
  struct Parent {
    bool inBlock = false;
  };

  ImportedModel& m_out;
  ON::LengthUnitSystem m_docUnits;
  ON::LengthUnitSystem m_fileUnits = ON::LengthUnitSystem::None;
  double m_scale = 1;
  ON_Xform m_S, m_Sinv;
  std::string m_version = "AC1009";
  double m_textSize = 2.5;
  std::string m_dimStyleName;
  std::vector<Entity> m_entities;
  std::vector<Entity> m_layers, m_ltypes, m_styles, m_dimstyles, m_objects;
  std::map<std::string, Block> m_blocks;  // per nome in maiuscolo
  std::map<std::string, Style> m_textStyles;
  std::map<std::string, const ON_DimStyle*> m_dimStyleByName;
  const ON_DimStyle* m_defaultStyle = nullptr;
  std::map<std::string, int> m_blockState;  // 0 da fare, 1 in corso, 2 fatto, -1 non valido
  std::map<std::string, std::string> m_blockNames;  // maiuscolo -> nome nel modello
  std::map<std::string, std::string> m_hatchPatternKeys;  // definizione -> nome del motivo
  std::map<std::string, int> m_skipped;
  // gruppi: handle dell'entita' -> oggetti creati
  std::map<std::string, std::vector<size_t>> m_handleObjects;
  bool m_pageSpace = false;

  static std::string Key(const std::string& s) { return Upper(Trim(s)); }

  void Skip(const std::string& what, int n = 1) { m_skipped[what] += n; }
  std::string SkippedText() const {
    std::string s;
    for (const auto& [k, n] : m_skipped) s += (s.empty() ? "" : ", ") + std::to_string(n) + " " + k;
    return s;
  }

  // --- sezioni ---
  void Split(const std::vector<DxfPair>& pairs) {
    std::string section;
    Block* cur = nullptr;
    for (size_t i = 0; i < pairs.size(); i++) {
      const auto& q = pairs[i];
      if (q.code == 0 && q.value == "SECTION" && i + 1 < pairs.size()) { section = Trim(pairs[i + 1].value); i++; continue; }
      if (q.code == 0 && q.value == "ENDSEC") { section.clear(); cur = nullptr; continue; }
      if (section == "HEADER" && q.code == 9 && i + 1 < pairs.size()) {
        const std::string var = Trim(q.value);
        const DxfPair& v = pairs[i + 1];
        if (var == "$INSUNITS") { if (auto u = InsUnits(Int(v.value)); u != ON::LengthUnitSystem::None) m_fileUnits = u; }
        else if (var == "$MEASUREMENT" && m_fileUnits == ON::LengthUnitSystem::None)
          m_fileUnits = Int(v.value) == 1 ? ON::LengthUnitSystem::Millimeters : ON::LengthUnitSystem::Inches;
        else if (var == "$ACADVER") m_version = Trim(v.value);
        else if (var == "$TEXTSIZE") m_textSize = Num(v.value);
        else if (var == "$DIMSTYLE") m_dimStyleName = Trim(v.value);
        continue;
      }
      if (q.code != 0 || (section != "TABLES" && section != "ENTITIES" && section != "BLOCKS" && section != "OBJECTS"))
        continue;
      Entity e;
      e.type = Trim(q.value);
      size_t j = i + 1;
      for (; j < pairs.size() && pairs[j].code != 0; j++) e.p.push_back(pairs[j]);
      i = j - 1;
      if (section == "TABLES") {
        if (e.type == "LAYER") m_layers.push_back(std::move(e));
        else if (e.type == "LTYPE") m_ltypes.push_back(std::move(e));
        else if (e.type == "STYLE") m_styles.push_back(std::move(e));
        else if (e.type == "DIMSTYLE") m_dimstyles.push_back(std::move(e));
      } else if (section == "ENTITIES") {
        m_entities.push_back(std::move(e));
      } else if (section == "OBJECTS") {
        m_objects.push_back(std::move(e));
      } else if (section == "BLOCKS") {
        if (e.type == "BLOCK") {
          cur = &m_blocks[Key(e.S(2))];
          cur->ents.clear();
          cur->name = Trim(e.S(2));
          cur->description = e.S(4);
          cur->base = e.P(10);
          cur->flags = e.I(70);
        } else if (e.type == "ENDBLK") {
          cur = nullptr;
        } else if (cur) {
          cur->ents.push_back(std::move(e));
        }
      }
    }
  }

  void MakeTables() {
    // tipi di linea
    for (const auto& e : m_ltypes) {
      const std::string name = Trim(e.S(2));
      const std::string k = Key(name);
      if (k.empty() || k == "BYLAYER" || k == "BYBLOCK" || k == "CONTINUOUS") continue;
      ImportedLinetype lt;
      lt.name = name;
      lt.description = e.S(3);
      for (double d : e.All(49)) lt.pattern.push_back(d * m_scale);
      if (lt.pattern.empty()) continue;
      m_out.linetypes.push_back(std::move(lt));
    }
    // livelli
    for (const auto& e : m_layers) {
      ImportedLayer l;
      l.name = Trim(e.S(2));
      if (l.name.empty()) continue;
      const int aci = e.I(62, 7);
      l.color = e.Has(420) ? TrueColor(e.I(420)) : AciColor(aci);
      l.visible = aci >= 0 && !(e.I(70) & 1);  // spento o congelato
      l.locked = (e.I(70) & 4) != 0;
      const std::string lt = Trim(e.S(6));
      if (!lt.empty() && Key(lt) != "CONTINUOUS") l.linetype = lt;
      if (e.Has(370) && e.I(370) > 0) l.plotWeight = e.I(370) / 100.0;
      m_out.layers.push_back(std::move(l));
    }
    // stili di testo
    for (const auto& e : m_styles) {
      Style s;
      s.height = e.D(40) * m_scale;
      s.width = e.D(41, 1);
      std::string file = Trim(e.S(3));
      for (const auto& x : e.XData("ACAD"))
        if (x.code == 1000 && !Trim(x.value).empty()) { s.font = Trim(x.value); break; }
      if (s.font.empty()) {
        const std::string ext = Lower(fs::u8path(file).extension().string());
        if (ext == ".ttf" || ext == ".otf" || ext == ".ttc") s.font = U8(fs::u8path(file).stem());
        if (!s.font.empty()) s.font[0] = (char)std::toupper((unsigned char)s.font[0]);
      }
      m_textStyles[Key(e.S(2))] = s;
    }
    // stili di quota
    for (const auto& e : m_dimstyles) {
      const std::string name = Trim(e.S(2));
      if (name.empty()) continue;
      auto ds = NewDimStyle(name);
      bool first2 = true;  // il primo codice 2 e' il nome
      for (const auto& q : e.p) {
        if (q.code == 2 && first2) { first2 = false; continue; }
        if (q.code == 70 || q.code == 105 || q.code >= 1000) continue;
        ApplyDimVar(*ds, q.code, q.value);
      }
      ScaleStyle(*ds);
      m_dimStyleByName[Key(name)] = ds;
    }
    if (m_dimStyleByName.empty()) {
      auto ds = NewDimStyle("Standard");
      ds->SetTextHeight(m_textSize > 0 ? m_textSize * m_scale : ds->TextHeight());
      m_dimStyleByName["STANDARD"] = ds;
    }
    auto it = m_dimStyleByName.find(Key(m_dimStyleName));
    if (it == m_dimStyleByName.end()) it = m_dimStyleByName.find("STANDARD");
    if (it == m_dimStyleByName.end()) it = m_dimStyleByName.begin();
    m_defaultStyle = it->second;
  }

  ON_DimStyle* NewDimStyle(const std::string& name) {
    // valori ISO di AutoCAD (metrico) come base: come il DIMSTYLE "ISO-25"
    auto ds = std::make_unique<ON_DimStyle>(ON_DimStyle::DefaultMillimeterSmall);
    ds->ClearId();
    ds->ClearIndex();
    ds->SetId(ON_CreateId());
    ds->SetName(ON_wString(name.c_str()));
    // DIMSCALE di AutoCAD vale 1 se il file non lo dice (DefaultMillimeterSmall ha 10: testi 10 volte piu' grandi)
    ds->SetDimScale(1.0);
    ds->SetArrowSize(2.5);
    ds->SetTextHeight(2.5);
    ds->SetExtOffset(0.625);
    ds->SetExtExtension(1.25);
    ds->SetTextGap(0.625);
    ds->SetLengthResolution(2);
    ds->SetArrowType1And2(ON_Arrowhead::arrow_type::SolidTriangle);
    ds->SetDimTextLocation(ON_DimStyle::TextLocation::AboveDimLine);
    ON_DimStyle* r = ds.get();
    m_out.dimStyles.push_back(std::move(ds));
    return r;
  }
  // le lunghezze dello stile seguono le unita' del documento (DIMSCALE resta il fattore)
  void ScaleStyle(ON_DimStyle& ds) const {
    if (std::fabs(m_scale - 1) < 1e-12) return;
    ds.SetArrowSize(ds.ArrowSize() * m_scale);
    ds.SetTextHeight(ds.TextHeight() * m_scale);
    ds.SetExtOffset(ds.ExtOffset() * m_scale);
    ds.SetExtExtension(ds.ExtExtension() * m_scale);
    ds.SetTextGap(ds.TextGap() * m_scale);
    ds.SetLengthFactor(ds.LengthFactor() / m_scale);  // il valore misurato resta nelle unita' del file
  }

  static ON_Color TrueColor(int v) { return ON_Color((v >> 16) & 255, (v >> 8) & 255, v & 255); }

  const ON_DimStyle& StyleFor(const Entity& e) const {
    auto it = m_dimStyleByName.find(Key(e.S(3)));
    return it != m_dimStyleByName.end() ? *it->second : *m_defaultStyle;
  }

  // Attributi DXF comuni (livello, colore, tipo di linea, spessore, visibilita').
  void SetAttributes(ImportedObject& o, const Entity& e, bool inBlock) const {
    std::string layer = Trim(e.S(8));
    if (layer.empty()) layer = "0";
    o.layer = layer;
    const bool layer0InBlock = inBlock && layer == "0";
    // colore
    if (e.Has(420)) { o.colorSource = ImportedObject::Source::Object; o.objectColor = TrueColor(e.I(420)); }
    else if (e.Has(62)) {
      const int aci = e.I(62);
      if (aci == 0) o.colorSource = ImportedObject::Source::Parent;  // DaBlocco
      else if (aci >= 1 && aci <= 255) { o.colorSource = ImportedObject::Source::Object; o.objectColor = AciColor(aci); }
      else if (layer0InBlock) o.colorSource = ImportedObject::Source::Parent;  // DaLayer sul livello 0: del blocco
    } else if (layer0InBlock) o.colorSource = ImportedObject::Source::Parent;
    // tipo di linea
    const std::string lt = Trim(e.S(6));
    const std::string k = Key(lt);
    if (k == "BYBLOCK" || (layer0InBlock && (k.empty() || k == "BYLAYER"))) o.linetypeSource = ImportedObject::Source::Parent;
    else if (!k.empty() && k != "BYLAYER") { o.linetypeSource = ImportedObject::Source::Object; o.linetype = lt; }
    // spessore (1/100 mm): -1 DaLayer, -2 DaBlocco, -3 predefinito
    if (e.Has(370)) {
      const int w = e.I(370);
      if (w == -2 || (layer0InBlock && w == -1)) o.plotWeightSource = ImportedObject::Source::Parent;
      else if (w >= 0) { o.plotWeightSource = ImportedObject::Source::Object; o.plotWeight = w / 100.0; }
    } else if (layer0InBlock) o.plotWeightSource = ImportedObject::Source::Parent;
    if (e.I(60) == 1) o.hidden = true;
    o.pageSpace = m_pageSpace && !inBlock;
  }

  void Emit(std::vector<ImportedObject>& target, ON_Geometry* g, const Entity& e, bool inBlock, const ON_Xform& xf,
            bool transform = true) {
    if (!g) return;
    if (transform && !xf.IsIdentity()) g->Transform(xf);
    ImportedObject o;
    o.geom.reset(g);
    SetAttributes(o, e, inBlock);
    Track(target, e, inBlock);
    target.push_back(std::move(o));
  }
  void Track(std::vector<ImportedObject>& target, const Entity& e, bool inBlock) {
    if (inBlock || &target != &m_out.objects) return;
    const std::string h = Trim(e.S(5));
    if (!h.empty()) m_handleObjects[Key(h)].push_back(target.size());
  }

  // --- blocchi ---
  // Nome della definizione nel modello (creata la prima volta che serve); vuoto se non si puo'.
  std::string EnsureBlock(const std::string& rawName, int depth) {
    const std::string k = Key(rawName);
    auto bl = m_blocks.find(k);
    if (bl == m_blocks.end() || depth > 32) return "";
    if (bl->second.flags & 4) { Skip("riferimenti esterni (xref)"); return ""; }
    int& st = m_blockState[k];
    if (st == 2) return m_blockNames[k];
    if (st == 1 || st == -1) return "";  // ricorsivo o non valido
    st = 1;
    ImportedBlock b;
    b.name = bl->second.name;
    b.description = bl->second.description;
    // definizione: coordinate relative al punto base, nelle unita' del documento
    const ON_Xform toBase = m_S * ON_Xform::TranslationTransformation(ON_3dPoint::Origin - bl->second.base);
    Convert(bl->second.ents, toBase, Parent{true}, depth + 1, b.objects, true);
    if (b.objects.empty()) { st = -1; return ""; }
    m_blockNames[k] = b.name;
    m_out.blocks.push_back(std::move(b));
    st = 2;
    return m_blockNames[k];
  }

  // --- annotazioni ---
  ON_Plane AnnotationPlane(const ON_3dPoint& origin, const ON_3dVector& xdir, const ON_3dVector& normal) const {
    ON_3dVector z = normal;
    if (!z.Unitize()) z = ON_3dVector::ZAxis;
    ON_3dVector x = xdir - (xdir * z) * z;
    if (!x.Unitize()) x = OcsPlane(z).xaxis;
    ON_3dVector y = ON_CrossProduct(z, x);
    ON_Plane pl(origin, x, y);
    pl.UpdateEquation();
    return pl;
  }

  ON_Text* MakeText(const std::string& text, const ON_3dPoint& origin, const ON_3dVector& xdir, const ON_3dVector& normal,
                    double height, ON::TextHorizontalAlignment ha, ON::TextVerticalAlignment va, const Style* st) {
    if (text.empty()) return nullptr;
    const ON_DimStyle& ds = *m_defaultStyle;
    auto* t = new ON_Text();
    if (!t->Create(ON_wString(text.c_str()), &ds, AnnotationPlane(origin, xdir, normal))) { delete t; return nullptr; }
    t->SetDimensionStyleId(ds.Id());
    if (height > 0 && std::fabs(height - ds.TextHeight()) > 1e-9) t->SetTextHeight(&ds, height);
    if (ha != ds.TextHorizontalAlignment()) t->SetTextHorizontalAlignment(&ds, ha);
    if (va != ds.TextVerticalAlignment()) t->SetTextVerticalAlignment(&ds, va);
    if (st && !st->font.empty())
      if (const ON_Font* f = ON_Font::GetManagedFont(ON_wString(st->font.c_str())); f && !f->FamilyName().IsEmpty())
        t->SetFont(&ds, *f);
    return t;
  }

  const Style* TextStyle(const Entity& e) const {
    auto it = m_textStyles.find(Key(e.S(7).empty() ? "Standard" : e.S(7)));
    return it == m_textStyles.end() ? nullptr : &it->second;
  }

  // TEXT, ATTRIB, ATTDEF
  ON_Text* TextEntity(const Entity& e, const ON_Xform& xf, std::string value) {
    value = DxfPlainText(value);
    if (Trim(value).empty()) return nullptr;
    const ON_Plane ocs = OcsPlane(e.Extrusion());
    const int h72 = e.I(72), v73 = e.I(e.type == "TEXT" ? 73 : 74);
    ON_3dPoint p = e.P(10);
    if ((h72 != 0 || v73 != 0) && e.Has(11)) p = e.P(11);
    double rot = e.D(50) * ON_PI / 180;
    if ((h72 == 3 || h72 == 5) && e.Has(11)) {  // allineato / adattato: tra i due punti
      const ON_3dPoint a = e.P(10), b = e.P(11);
      rot = std::atan2(b.y - a.y, b.x - a.x);
      p = a;
    }
    const ON_3dPoint W = xf * OcsToWcs(ocs, p);
    const ON_3dVector X = xf * (std::cos(rot) * ocs.xaxis + std::sin(rot) * ocs.yaxis);
    const ON_3dVector N = xf * ocs.zaxis;
    const Style* st = TextStyle(e);
    double h = e.D(40);
    if (h <= 0 && st) h = st->height / std::max(m_scale, 1e-12);
    if (h <= 0) h = m_textSize;
    h *= XformScale(xf, ocs.yaxis);
    ON::TextHorizontalAlignment ha = ON::TextHorizontalAlignment::Left;
    if (h72 == 1 || h72 == 4) ha = ON::TextHorizontalAlignment::Center;
    else if (h72 == 2) ha = ON::TextHorizontalAlignment::Right;
    ON::TextVerticalAlignment va = ON::TextVerticalAlignment::Bottom;  // linea di base
    if (h72 == 4 || v73 == 2) va = ON::TextVerticalAlignment::Middle;
    else if (v73 == 1) va = ON::TextVerticalAlignment::BottomOfBoundingBox;
    else if (v73 == 3) va = ON::TextVerticalAlignment::Top;
    return MakeText(value, W, X, N, h, ha, va, st);
  }

  ON_Text* MTextEntity(const Entity& e, const ON_Xform& xf) {
    std::string t;
    for (const auto& q : e.p) if (q.code == 3) t += q.value;
    t = DxfPlainText(t + e.S(1));
    if (Trim(t).empty()) return nullptr;
    ON_3dVector n = e.Extrusion();
    const ON_Plane ocs = OcsPlane(n);
    ON_3dVector x = e.Has(11) ? ON_3dVector(e.D(11), e.D(21), e.D(31)) : ocs.xaxis;
    if (!e.Has(11) && e.Has(50)) {
      const double r = e.D(50) * ON_PI / 180;
      x = std::cos(r) * ocs.xaxis + std::sin(r) * ocs.yaxis;
    }
    const ON_3dPoint W = xf * e.P(10);
    const ON_3dVector X = xf * x, N = xf * n;
    const Style* st = TextStyle(e);
    double h = e.D(40);
    if (h <= 0) h = m_textSize;
    h *= XformScale(xf, ocs.yaxis);
    const int att = std::clamp(e.I(71, 1), 1, 9);
    const int col = (att - 1) % 3, row = (att - 1) / 3;
    const auto ha = col == 0 ? ON::TextHorizontalAlignment::Left
                  : col == 1 ? ON::TextHorizontalAlignment::Center : ON::TextHorizontalAlignment::Right;
    const auto va = row == 0 ? ON::TextVerticalAlignment::Top
                  : row == 1 ? ON::TextVerticalAlignment::Middle : ON::TextVerticalAlignment::BottomOfBoundingBox;
    return MakeText(t, W, X, N, h, ha, va, st);
  }

  // DIMENSION -> quota vera (o nullptr: si usa la geometria del blocco della quota)
  ON_Dimension* DimensionEntity(const Entity& e, const ON_Xform& xf) {
    const int type = e.I(70) & 7;
    const ON_DimStyle& ds = StyleFor(e);
    const ON_Plane ocs = OcsPlane(e.Extrusion());
    const ON_3dVector N0 = e.Extrusion();
    ON_3dVector N = xf * N0;
    if (!N.Unitize()) N = ON_3dVector::ZAxis;
    auto W = [&](int c) { return xf * e.P(c); };                     // punti WCS
    auto O = [&](int c) { return xf * OcsToWcs(ocs, e.P(c)); };     // punti OCS
    ON_Dimension* dim = nullptr;
    if (type == 0 || type == 1) {
      const ON_3dPoint a = W(13), b = W(14), dl = W(10);
      if (a.DistanceTo(b) < 1e-12) return nullptr;
      if (type == 1) dim = ON_DimLinear::CreateAligned(a, b, dl, N, ds.Id(), nullptr);
      else {
        const double r = e.D(50) * ON_PI / 180;
        const ON_3dVector dir = xf * (std::cos(r) * ocs.xaxis + std::sin(r) * ocs.yaxis);
        dim = ON_DimLinear::CreateRotated(a, b, ON_Line(dl, dl + dir), N, ds.Id(), nullptr);
      }
    } else if (type == 3 || type == 4) {
      ON_3dPoint center, rp;
      if (type == 4) { center = W(10); rp = W(15); }
      else { rp = W(15); center = 0.5 * (W(15) + W(10)); }
      if (center.DistanceTo(rp) < 1e-12) return nullptr;
      ON_3dPoint tp = e.Has(11) ? O(11) : rp;
      ON_Plane pl = AnnotationPlane(center, rp - center, N);
      auto* d = new ON_DimRadial();
      if (!d->Create(type == 3 ? ON::AnnotationType::Diameter : ON::AnnotationType::Radius, ds.Id(), pl, center, rp,
                     pl.ClosestPointTo(tp))) { delete d; return nullptr; }
      dim = d;
    } else if (type == 2) {  // angolare a due linee: 13-14 e 10-15, arco in 16 (OCS)
      const ON_Line l1(W(13), W(14)), l2(W(10), W(15));
      auto* d = new ON_DimAngular();
      if (!d->Create(&ds, l1, l1.to, l2, l2.to, O(16), true)) { delete d; return nullptr; }
      dim = d;
    } else if (type == 5) {  // angolare a tre punti: vertice 15, lati 13 e 14, arco 10
      const ON_3dPoint c = W(15);
      auto* d = new ON_DimAngular();
      ON_Plane pl = AnnotationPlane(c, W(13) - c, N);
      if (!d->Create(ds.Id(), pl, pl.xaxis, c, W(13), W(14), W(10))) { delete d; return nullptr; }
      dim = d;
    } else {
      return nullptr;  // coordinate: dalla geometria del blocco
    }
    if (!dim) return nullptr;
    dim->SetDimensionStyleId(ds.Id());
    // sostituzioni dello stile (dati estesi ACAD: DSTYLE { codice valore ... })
    const auto xd = e.XData("ACAD");
    bool inStyle = false;
    for (size_t k = 0; k < xd.size(); k++) {
      if (xd[k].code == 1000 && Upper(Trim(xd[k].value)) == "DSTYLE") { inStyle = true; continue; }
      if (!inStyle) continue;
      if (xd[k].code == 1002 && Trim(xd[k].value) == "}") break;
      if (xd[k].code == 1070 && k + 1 < xd.size()) {
        const int code = Int(xd[k].value);
        std::string v = xd[k + 1].value;
        // lunghezze nelle unita' del documento
        if (code == 41 || code == 42 || code == 44 || code == 140 || code == 147) v = std::to_string(Num(v) * m_scale);
        if (code == 144) v = std::to_string(Num(v) / m_scale);
        ApplyDimVar(*dim, ds, code, v);
        k++;
      }
    }
    // testo sostituito (vuoto o "<>" = valore misurato)
    const std::string over = e.S(1);
    if (!over.empty() && Trim(over) != "<>") {
      std::string t = DxfPlainText(over);
      if (Trim(t).empty()) t = " ";  // testo soppresso
      dim->SetUserText(ON_wString(t.c_str()));
    }
    // posizione del testo spostata dall'utente
    if ((e.I(70) & 128) && e.Has(11) && (type == 0 || type == 1)) {
      ON_2dPoint tp;
      if (dim->Plane().ClosestPointTo(O(11), &tp.x, &tp.y)) {
        dim->Set2dTextPoint(tp);
        dim->SetUseDefaultTextPoint(false);
      }
    }
    return dim;
  }

  // HATCH -> retini (uno per contorno esterno con i suoi fori)
  void HatchEntity(const Entity& e, const ON_Xform& xf, bool inBlock, std::vector<ImportedObject>& target) {
    const auto& p = e.p;
    size_t i = 0;
    auto at = [&](int code) { return i < p.size() && p[i].code == code; };
    auto num = [&]() { return Num(p[i++].value); };
    // fino al numero di contorni
    std::string pattern;
    bool solid = false;
    int nPaths = 0;
    for (; i < p.size(); i++) {
      if (p[i].code == 2) pattern = Trim(p[i].value);
      else if (p[i].code == 70) solid = Int(p[i].value) == 1;
      else if (p[i].code == 91) { nPaths = Int(p[i].value); i++; break; }
    }
    struct Loop { std::unique_ptr<ON_Curve> c; int flags = 0; };
    std::vector<Loop> loops;
    for (int k = 0; k < nPaths && i < p.size(); k++) {
      while (i < p.size() && p[i].code != 92) i++;
      if (i >= p.size()) break;
      const int flags = Int(p[i++].value);
      std::vector<ON_Curve*> segs;
      if (flags & 2) {  // polilinea
        bool bulge = false, closed = true;
        int nv = 0;
        while (i < p.size() && p[i].code != 10) {
          if (p[i].code == 72) bulge = Int(p[i].value) != 0;
          else if (p[i].code == 73) closed = Int(p[i].value) != 0;
          else if (p[i].code == 93) nv = Int(p[i].value);
          i++;
        }
        std::vector<ON_3dPoint> pts;
        std::vector<double> bul;
        for (int v = 0; v < nv && i < p.size(); v++) {
          double x = 0, y = 0, b = 0;
          if (at(10)) x = num();
          if (at(20)) y = num();
          if (bulge && at(42)) b = num();
          pts.emplace_back(x, y, 0);
          bul.push_back(b);
        }
        (void)closed;
        for (size_t s = 0; pts.size() >= 2 && s < pts.size(); s++) {
          const ON_3dPoint& a = pts[s];
          const ON_3dPoint& b = pts[(s + 1) % pts.size()];
          if (a.DistanceTo(b) < 1e-12) continue;
          segs.push_back(BulgeSegment(a, b, bul[s], ON_xy_plane));
        }
      } else {
        int ne = 0;
        if (at(93)) ne = Int(p[i++].value);
        for (int ed = 0; ed < ne && i < p.size(); ed++) {
          while (i < p.size() && p[i].code != 72) i++;
          if (i >= p.size()) break;
          const int et = Int(p[i++].value);
          std::map<int, double> v;  // valori dello spigolo (primo valore per codice)
          if (et == 1) {
            double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            if (at(10)) x0 = num();
            if (at(20)) y0 = num();
            if (at(11)) x1 = num();
            if (at(21)) y1 = num();
            segs.push_back(new ON_LineCurve(ON_3dPoint(x0, y0, 0), ON_3dPoint(x1, y1, 0)));
          } else if (et == 2 || et == 3) {
            while (i < p.size() && p[i].code != 72 && p[i].code != 92 && p[i].code != 97 && p[i].code != 75) {
              if (!v.count(p[i].code)) v[p[i].code] = Num(p[i].value);
              i++;
            }
            const bool ccw = v.count(73) ? v[73] != 0 : true;
            double a0 = v[50], a1 = v[51];
            if (!ccw) { const double t = a0; a0 = 360 - a1; a1 = 360 - t; }
            a0 *= ON_PI / 180;
            a1 *= ON_PI / 180;
            while (a1 <= a0) a1 += 2 * ON_PI;
            const ON_3dPoint c(v[10], v[20], 0);
            ON_Curve* crv = nullptr;
            if (et == 2) {
              if (v[40] > 0) crv = new ON_ArcCurve(ON_Arc(ON_Circle(ON_Plane(c, ON_3dVector::ZAxis), v[40]), ON_Interval(a0, a1)));
            } else {
              const ON_3dVector major(v[11], v[21], 0);
              const double ratio = v.count(40) ? v[40] : 1;
              ON_3dVector minor = ON_CrossProduct(ON_3dVector::ZAxis, major);
              if (major.Length() > 0 && minor.Unitize()) {
                ON_Ellipse el(ON_Plane(c, major, minor), major.Length(), major.Length() * ratio);
                auto param = [&](double ang) { return std::atan2(std::sin(ang) / std::max(ratio, 1e-12), std::cos(ang)); };
                double t0 = param(a0), t1 = param(a1);
                while (t1 <= t0 + 1e-12) t1 += 2 * ON_PI;
                ON_NurbsCurve nc;
                el.GetNurbForm(nc);
                auto* full = new ON_NurbsCurve(nc);
                if (std::fabs(t1 - t0 - 2 * ON_PI) < 1e-9) crv = full;
                else {
                  if (t0 < 0) { t0 += 2 * ON_PI; t1 += 2 * ON_PI; }
                  if (t1 > 2 * ON_PI + 1e-12) {
                    ON_Curve* a = full->DuplicateCurve();
                    a->Trim(ON_Interval(t0, 2 * ON_PI));
                    ON_Curve* b = full->DuplicateCurve();
                    b->Trim(ON_Interval(0, t1 - 2 * ON_PI));
                    delete full;
                    std::vector<ON_Curve*> two{a, b};
                    crv = JoinSegments(two);
                  } else {
                    full->Trim(ON_Interval(t0, t1));
                    crv = full;
                  }
                }
              }
            }
            if (crv) {
              if (!ccw) crv->Reverse();
              segs.push_back(crv);
            }
          } else if (et == 4) {
            int degree = 3, nk = 0, ncp = 0, nfit = 0;
            bool rational = false;
            std::vector<double> knots, weights;
            std::vector<ON_3dPoint> cps, fits;
            while (i < p.size() && p[i].code != 40 && p[i].code != 10) {
              if (p[i].code == 94) degree = Int(p[i].value);
              else if (p[i].code == 73) rational = Int(p[i].value) != 0;
              else if (p[i].code == 95) nk = Int(p[i].value);
              else if (p[i].code == 96) ncp = Int(p[i].value);
              i++;
            }
            for (int q = 0; q < nk && at(40); q++) knots.push_back(num());
            for (int q = 0; q < ncp && i < p.size(); q++) {
              double x = 0, y = 0;
              if (at(10)) x = num();
              if (at(20)) y = num();
              cps.emplace_back(x, y, 0);
              if (at(42)) weights.push_back(num());
            }
            if (at(97)) {
              nfit = Int(p[i++].value);
              for (int q = 0; q < nfit && i < p.size(); q++) {
                double x = 0, y = 0;
                if (at(11)) x = num();
                if (at(21)) y = num();
                fits.emplace_back(x, y, 0);
              }
            }
            while (at(12) || at(22) || at(13) || at(23)) i++;
            const int n = (int)cps.size();
            if (n >= degree + 1 && (int)knots.size() == n + degree + 1) {
              const bool rat = rational && (int)weights.size() == n;
              auto* nc = new ON_NurbsCurve(3, rat, degree + 1, n);
              for (int q = 0; q < n; q++) {
                if (rat) nc->SetCV(q, ON_4dPoint(cps[q].x * weights[q], cps[q].y * weights[q], 0, weights[q]));
                else nc->SetCV(q, cps[q]);
              }
              for (int q = 1; q + 1 < (int)knots.size(); q++) nc->m_knot[q - 1] = knots[q];
              if (nc->IsValid()) segs.push_back(nc); else delete nc;
            } else if (fits.size() >= 2) {
              ON_3dPointArray fp;
              for (auto& f : fits) fp.Append(f);
              segs.push_back(new ON_PolylineCurve(fp));
            }
          }
        }
      }
      // catena chiusa: i buchi tra gli spigoli si chiudono con segmenti
      std::vector<ON_Curve*> chain;
      for (size_t s = 0; s < segs.size(); s++) {
        if (!chain.empty()) {
          const ON_3dPoint a = chain.back()->PointAtEnd(), b = segs[s]->PointAtStart();
          if (a.DistanceTo(b) > 1e-6) chain.push_back(new ON_LineCurve(a, b));
        }
        chain.push_back(segs[s]);
      }
      if (chain.empty()) continue;
      const ON_3dPoint s0 = chain.front()->PointAtStart(), s1 = chain.back()->PointAtEnd();
      if (s0.DistanceTo(s1) > 1e-6) chain.push_back(new ON_LineCurve(s1, s0));
      std::unique_ptr<ON_Curve> c;
      if (chain.size() == 1) c.reset(chain[0]);
      else {
        auto* pc = new ON_PolyCurve();
        for (ON_Curve* x : chain) pc->Append(x);
        c.reset(pc);
      }
      loops.push_back({std::move(c), flags});
      // oggetti di origine (97 n, 330...)
      if (at(97)) { const int ns = Int(p[i++].value); for (int q = 0; q < ns && at(330); q++) i++; }
    }
    // motivo: angolo 52, scala 41, linee 53/43/44/45/46/79/49
    double angle = 0, scale = 1;
    std::vector<ON_HatchLine> lines;
    for (; i < p.size(); i++) {
      const int c = p[i].code;
      if (c == 52) angle = Num(p[i].value) * ON_PI / 180;
      else if (c == 41) scale = Num(p[i].value);
      else if (c == 78) {
        const int nl = Int(p[i].value);
        i++;
        for (int l = 0; l < nl && i < p.size(); l++) {
          double la = 0, bx = 0, by = 0, ox = 0, oy = 0;
          int nd = 0;
          if (at(53)) la = num() * ON_PI / 180;
          if (at(43)) bx = num();
          if (at(44)) by = num();
          if (at(45)) ox = num();
          if (at(46)) oy = num();
          if (at(79)) nd = Int(p[i++].value);
          ON_SimpleArray<double> dashes;
          for (int d = 0; d < nd && at(49); d++) dashes.Append(num());
          // le linee del file sono gia' ruotate e scalate: si riportano al motivo base
          const double s = scale != 0 ? scale : 1;
          const double ca = std::cos(-angle), sa = std::sin(-angle);
          auto rot = [&](double x, double y) { return ON_2dVector((x * ca - y * sa) / s, (x * sa + y * ca) / s); };
          const ON_2dVector base = rot(bx, by), off = rot(ox, oy);
          for (int d = 0; d < dashes.Count(); d++) dashes[d] /= s;
          lines.emplace_back(la - angle, ON_2dPoint(base.x, base.y), off, dashes);
        }
        i--;
      } else if (c == 450 || c == 1001) break;  // sfumatura o dati estesi: fine
    }
    if (loops.empty()) { Skip("retini senza contorno"); return; }
    // nome del motivo (stessa definizione = stesso motivo)
    std::string patName = solid || lines.empty() ? "Solid" : (pattern.empty() ? "DXF" : pattern);
    if (!(solid || lines.empty())) {
      std::ostringstream key;
      key.precision(10);
      for (const auto& l : lines) {
        key << l.AngleRadians() << ' ' << l.Base().x << ' ' << l.Base().y << ' ' << l.Offset().x << ' ' << l.Offset().y;
        for (int d = 0; d < l.DashCount(); d++) key << ' ' << l.Dash(d);
        key << ';';
      }
      auto f = m_hatchPatternKeys.find(key.str());
      if (f != m_hatchPatternKeys.end()) patName = f->second;
      else {
        std::string n = patName;
        for (int k = 2; std::any_of(m_out.hatchPatterns.begin(), m_out.hatchPatterns.end(),
                                    [&](const ImportedHatchPattern& h) { return Key(h.name) == Key(n); }); k++)
          n = patName + " (" + std::to_string(k) + ")";
        patName = n;
        m_hatchPatternKeys[key.str()] = patName;
        ImportedHatchPattern hp;
        hp.name = patName;
        hp.lines = lines;
        m_out.hatchPatterns.push_back(std::move(hp));
      }
    } else if (!std::any_of(m_out.hatchPatterns.begin(), m_out.hatchPatterns.end(),
                            [](const ImportedHatchPattern& h) { return h.solid; })) {
      ImportedHatchPattern hp;
      hp.name = "Solid";
      hp.solid = true;
      m_out.hatchPatterns.push_back(std::move(hp));
    }
    // annidamento dei contorni: profondita' pari = contorno esterno, dispari = foro
    const size_t n = loops.size();
    std::vector<std::vector<ON_2dPoint>> poly(n);
    for (size_t k = 0; k < n; k++) poly[k] = SampleLoop(*loops[k].c);
    std::vector<int> depth(n, 0);
    std::vector<int> parent(n, -1);
    for (size_t a = 0; a < n; a++)
      for (size_t b = 0; b < n; b++) {
        if (a == b || poly[a].empty() || poly[b].size() < 3) continue;
        if (InsidePolygon(poly[b], poly[a][0])) {
          depth[a]++;
          // genitore = il contenitore piu' interno (quello con piu' contenitori)
          if (parent[a] < 0 || InsidePolygon(poly[parent[a]], poly[b][0])) parent[a] = (int)b;
        }
      }
    const ON_Plane ocs = OcsPlane(e.Extrusion());
    ON_Plane hp = ocs;
    hp.origin = OcsToWcs(ocs, ON_3dPoint(0, 0, e.D(30)));
    hp.UpdateEquation();
    const double s = XformScale(xf, ocs.xaxis);
    for (size_t k = 0; k < n; k++) {
      if (depth[k] % 2) continue;
      ON_SimpleArray<const ON_Curve*> ls;
      ls.Append(loops[k].c.get());
      for (size_t h = 0; h < n; h++)
        if (depth[h] == depth[k] + 1 && parent[h] == (int)k) ls.Append(loops[h].c.get());
      auto* hatch = new ON_Hatch();
      if (!hatch->Create(hp, ls, 0, solid ? 0.0 : angle, solid ? 1.0 : (scale != 0 ? std::fabs(scale) : 1.0))) {
        delete hatch;
        Skip("retini non validi");
        continue;
      }
      if (!xf.IsIdentity()) {
        hatch->Transform(xf);
        if (std::fabs(s - 1) > 1e-12) hatch->SetPatternScale(hatch->PatternScale() * s);
      }
      ImportedObject o;
      o.geom.reset(hatch);
      SetAttributes(o, e, inBlock);
      o.hatchPattern = patName;
      Track(target, e, inBlock);
      target.push_back(std::move(o));
    }
  }

  // --- conversione delle entita' ---
  void Convert(const std::vector<Entity>& list, const ON_Xform& xf, Parent parent, int depth,
               std::vector<ImportedObject>& target, bool inBlock) {
    (void)parent;
    MeshBuilder faces;
    const Entity* facesEntity = nullptr;
    for (size_t i = 0; i < list.size(); i++) {
      const Entity& e = list[i];
      if (!inBlock && !m_pageSpace && e.I(67) == 1) {  // spazio carta
        if (e.type == "VIEWPORT") continue;
        const size_t before = target.size();
        std::vector<Entity> one{e};
        // POLYLINE/INSERT con le entita' successive (VERTEX, ATTRIB, SEQEND)
        size_t j = i + 1;
        while (j < list.size() && (list[j].type == "VERTEX" || list[j].type == "SEQEND" || list[j].type == "ATTRIB"))
          one.push_back(list[j++]);
        i = j - 1;
        m_pageSpace = true;
        Convert(one, xf, parent, depth, target, false);
        m_pageSpace = false;
        (void)before;
        continue;
      }
      const ON_Plane ocs = OcsPlane(e.Extrusion());
      const std::string& t = e.type;
      if (t == "LINE") Emit(target, new ON_LineCurve(e.P(10), e.P(11)), e, inBlock, xf);
      else if (t == "POINT") Emit(target, new ON_Point(e.P(10)), e, inBlock, xf);
      else if (t == "CIRCLE" || t == "ARC") {
        ON_Plane pl = ocs;
        pl.origin = OcsToWcs(ocs, e.P(10));
        pl.UpdateEquation();
        if (e.D(40) <= 0) { Skip("cerchi di raggio nullo"); continue; }
        ON_Circle circ(pl, e.D(40));
        if (t == "CIRCLE") Emit(target, new ON_ArcCurve(circ), e, inBlock, xf);
        else {
          double a0 = e.D(50) * ON_PI / 180, a1 = e.D(51) * ON_PI / 180;
          while (a1 <= a0) a1 += 2 * ON_PI;
          Emit(target, new ON_ArcCurve(ON_Arc(circ, ON_Interval(a0, a1))), e, inBlock, xf);
        }
      } else if (t == "ELLIPSE") {
        const ON_3dPoint c = e.P(10);
        const ON_3dVector major(e.D(11), e.D(21), e.D(31));
        ON_3dVector n = e.Extrusion();
        n.Unitize();
        ON_3dVector minor = ON_CrossProduct(n, major);
        minor.Unitize();
        minor *= major.Length() * e.D(40, 1);
        ON_NurbsCurve nc;
        ON_Ellipse el(ON_Plane(c, major, minor), major.Length(), minor.Length());
        el.GetNurbForm(nc);
        double t0 = e.D(41, 0), t1 = e.D(42, 2 * ON_PI);
        auto* crv = new ON_NurbsCurve(nc);
        if (std::fabs(t1 - t0 - 2 * ON_PI) > 1e-9) {
          while (t1 <= t0) t1 += 2 * ON_PI;
          if (t1 > 2 * ON_PI + 1e-12) {
            ON_Curve* a = crv->DuplicateCurve();
            a->Trim(ON_Interval(t0, 2 * ON_PI));
            ON_Curve* b = crv->DuplicateCurve();
            b->Trim(ON_Interval(0, t1 - 2 * ON_PI));
            delete crv;
            std::vector<ON_Curve*> segs{a, b};
            Emit(target, JoinSegments(segs), e, inBlock, xf);
            continue;
          }
          crv->Trim(ON_Interval(t0, t1));
        }
        Emit(target, crv, e, inBlock, xf);
      } else if (t == "LWPOLYLINE") {
        std::vector<ON_3dPoint> pts;
        std::vector<double> bulges;
        const double elev = e.D(38);
        double x = 0;
        bool haveX = false;
        for (const auto& q : e.p) {
          if (q.code == 10) { x = Num(q.value); haveX = true; }
          else if (q.code == 20 && haveX) { pts.emplace_back(x, Num(q.value), elev); bulges.push_back(0); haveX = false; }
          else if (q.code == 42 && !bulges.empty()) bulges.back() = Num(q.value);
        }
        const bool closed = e.I(70) & 1;
        std::vector<ON_Curve*> segs;
        for (size_t k = 0; k + 1 < pts.size() + (closed ? 1 : 0); k++) {
          if (pts[k].DistanceTo(pts[(k + 1) % pts.size()]) < 1e-12) continue;
          segs.push_back(BulgeSegment(pts[k], pts[(k + 1) % pts.size()], bulges[k], ocs));
        }
        if (segs.empty() && pts.size() == 1) Emit(target, new ON_Point(OcsToWcs(ocs, pts[0])), e, inBlock, xf);
        else Emit(target, JoinSegments(segs), e, inBlock, xf);
      } else if (t == "POLYLINE") {
        const int flags = e.I(70);
        std::vector<const Entity*> verts;
        size_t j = i + 1;
        for (; j < list.size() && list[j].type == "VERTEX"; j++) verts.push_back(&list[j]);
        i = (j < list.size() && list[j].type == "SEQEND") ? j : j - 1;
        if (flags & 64) {  // polyface mesh
          MeshBuilder mb;
          std::vector<int> idx;
          for (const Entity* v : verts)
            if (v->I(70) & 64) idx.push_back(mb.Vertex(xf * v->P(10)));
          for (const Entity* v : verts)
            if ((v->I(70) & 128) && !(v->I(70) & 64)) {
              int f[4] = {std::abs(v->I(71)) - 1, std::abs(v->I(72)) - 1, std::abs(v->I(73)) - 1, std::abs(v->I(74)) - 1};
              auto ok = [&](int k) { return k >= 0 && k < (int)idx.size(); };
              if (ok(f[0]) && ok(f[1]) && ok(f[2])) mb.Face(idx[f[0]], idx[f[1]], idx[f[2]], ok(f[3]) ? idx[f[3]] : -1);
            }
          if (!mb.Empty()) Emit(target, mb.Take().release(), e, inBlock, xf, false);
          continue;
        }
        if (flags & 16) {  // mesh poligonale M x N
          const int M = e.I(71), N = e.I(72);
          if (M < 2 || N < 2 || (int)verts.size() < M * N) { Skip("mesh poligonali non valide"); continue; }
          MeshBuilder mb;
          std::vector<int> idx;
          for (int k = 0; k < M * N; k++) idx.push_back(mb.Vertex(xf * verts[k]->P(10)));
          const bool cm = flags & 1, cn = flags & 32;
          for (int a = 0; a < M - 1 + (cm ? 1 : 0); a++)
            for (int b = 0; b < N - 1 + (cn ? 1 : 0); b++) {
              const int a1 = (a + 1) % M, b1 = (b + 1) % N;
              mb.Face(idx[a * N + b], idx[a1 * N + b], idx[a1 * N + b1], idx[a * N + b1]);
            }
          if (!mb.Empty()) Emit(target, mb.Take().release(), e, inBlock, xf, false);
          continue;
        }
        const bool is3d = flags & 8;
        std::vector<ON_3dPoint> pts;
        std::vector<double> bulges;
        for (const Entity* v : verts) {
          if (v->I(70) & 16) continue;  // punti di controllo della spline
          pts.push_back(v->P(10));
          bulges.push_back(is3d ? 0 : v->D(42));
        }
        if (!is3d) for (auto& pt : pts) pt.z = e.D(30, pt.z);
        const bool closed = flags & 1;
        std::vector<ON_Curve*> segs;
        const ON_Plane frame = is3d ? ON_Plane(ON_3dPoint::Origin, ON_3dVector::XAxis, ON_3dVector::YAxis) : ocs;
        for (size_t k = 0; pts.size() >= 2 && k + 1 < pts.size() + (closed ? 1 : 0); k++) {
          if (pts[k].DistanceTo(pts[(k + 1) % pts.size()]) < 1e-12) continue;
          segs.push_back(BulgeSegment(pts[k], pts[(k + 1) % pts.size()], bulges[k], frame));
        }
        Emit(target, JoinSegments(segs), e, inBlock, xf);
      } else if (t == "SPLINE") {
        const int degree = e.I(71, 3);
        const auto knots = e.All(40);
        const auto weights = e.All(41);
        std::vector<ON_3dPoint> cps, fits;
        {
          double x = 0, y = 0;
          for (const auto& q : e.p) {
            if (q.code == 10) x = Num(q.value);
            else if (q.code == 20) y = Num(q.value);
            else if (q.code == 30) cps.emplace_back(x, y, Num(q.value));
            else if (q.code == 11) x = Num(q.value);
            else if (q.code == 21) y = Num(q.value);
            else if (q.code == 31) fits.emplace_back(x, y, Num(q.value));
          }
        }
        const int n = (int)cps.size();
        if (n >= degree + 1 && (int)knots.size() == n + degree + 1) {
          const bool rational = (int)weights.size() == n;
          auto* nc = new ON_NurbsCurve(3, rational, degree + 1, n);
          for (int k = 0; k < n; k++) {
            if (rational) nc->SetCV(k, ON_4dPoint(cps[k].x * weights[k], cps[k].y * weights[k], cps[k].z * weights[k], weights[k]));
            else nc->SetCV(k, cps[k]);
          }
          for (int k = 1; k + 1 < (int)knots.size(); k++) nc->m_knot[k - 1] = knots[k];
          if (nc->IsValid()) Emit(target, nc, e, inBlock, xf); else { delete nc; Skip("spline non valide"); }
        } else if (fits.size() >= 2) {
          // curva per i punti di passaggio (interpolata, come le spline di AutoCAD senza punti di controllo)
          ON_3dPointArray fp;
          for (auto& f : fits) fp.Append(f);
          auto* nc = new ON_NurbsCurve();
          const int order = std::min(4, (int)fits.size());
          if (nc->CreateClampedUniformNurbs(3, order, fp.Count(), fp.Array())) Emit(target, nc, e, inBlock, xf);
          else { delete nc; Emit(target, new ON_PolylineCurve(fp), e, inBlock, xf); }
        } else Skip("spline non valide");
      } else if (t == "3DFACE" || t == "SOLID" || t == "TRACE") {
        ON_3dPoint q[4] = {e.P(10), e.P(11), e.P(12), e.P(13)};
        if (t != "3DFACE") { std::swap(q[2], q[3]); for (auto& pt : q) pt = OcsToWcs(ocs, pt); }
        if (t == "3DFACE") {
          // facce dello stesso livello e colore unite in una mesh, come Rhino
          if (facesEntity && (Key(facesEntity->S(8)) != Key(e.S(8)) || facesEntity->S(62) != e.S(62))) {
            if (!faces.Empty()) Emit(target, faces.Take().release(), *facesEntity, inBlock, xf, false);
          }
          facesEntity = &e;
          int v[4];
          for (int k = 0; k < 4; k++) v[k] = faces.Vertex(xf * q[k]);
          faces.Face(v[0], v[1], v[2], v[3] == v[2] ? -1 : v[3]);
        } else {
          // SOLID/TRACE 2D: retino pieno come in Rhino
          ON_Polyline pl;
          pl.Append(q[0]); pl.Append(q[1]); pl.Append(q[2]);
          if (q[3].DistanceTo(q[2]) > 1e-12) pl.Append(q[3]);
          pl.Append(q[0]);
          ON_Plane hp = ocs;
          hp.origin = OcsToWcs(ocs, ON_3dPoint(0, 0, e.D(30)));
          hp.UpdateEquation();
          ON_Xform to2d;
          to2d.Rotation(hp, ON_xy_plane);
          ON_PolylineCurve loop(pl);
          loop.Transform(to2d);
          ON_SimpleArray<const ON_Curve*> ls;
          ls.Append(&loop);
          auto* hatch = new ON_Hatch();
          if (pl.Count() >= 4 && hatch->Create(hp, ls, 0, 0, 1)) {
            if (!xf.IsIdentity()) hatch->Transform(xf);
            ImportedObject o;
            o.geom.reset(hatch);
            SetAttributes(o, e, inBlock);
            o.hatchPattern = "Solid";
            if (!std::any_of(m_out.hatchPatterns.begin(), m_out.hatchPatterns.end(), [](const ImportedHatchPattern& h) { return h.solid; })) {
              ImportedHatchPattern hpat;
              hpat.name = "Solid";
              hpat.solid = true;
              m_out.hatchPatterns.push_back(std::move(hpat));
            }
            Track(target, e, inBlock);
            target.push_back(std::move(o));
          } else delete hatch;
        }
      } else if (t == "TEXT") {
        if (ON_Text* tx = TextEntity(e, xf, e.S(1))) Emit(target, tx, e, inBlock, xf, false);
      } else if (t == "MTEXT") {
        if (ON_Text* tx = MTextEntity(e, xf)) Emit(target, tx, e, inBlock, xf, false);
      } else if (t == "ATTDEF") {
        // definizione di attributo nel blocco: in AutoCAD si vede solo il valore degli ATTRIB
        continue;
      } else if (t == "INSERT") {
        // attributi che seguono (66 = 1): testo utente dell'istanza e testi visibili
        std::vector<const Entity*> attribs;
        size_t j = i + 1;
        if (e.I(66) == 1) {
          for (; j < list.size() && list[j].type == "ATTRIB"; j++) attribs.push_back(&list[j]);
          if (j < list.size() && list[j].type == "SEQEND") i = j; else i = j - 1;
        }
        const std::string name = EnsureBlock(e.S(2), depth);
        if (name.empty()) {
          if (!m_blocks.count(Key(e.S(2)))) Skip("inserimenti di blocchi mancanti");
          continue;
        }
        const ON_3dPoint ins = e.P(10);
        const double sx = e.D(41, 1), sy = e.D(42, 1), sz = e.D(43, 1), rot = e.D(50) * ON_PI / 180;
        const int cols = std::max(1, e.I(70, 1)), rows = std::max(1, e.I(71, 1));
        const double dc = e.D(44), dr = e.D(45);
        const ON_Xform ocsXf = OcsXform(ocs);
        ON_Xform R;
        R.Rotation(rot, ON_3dVector::ZAxis, ON_3dPoint::Origin);
        const ON_Xform Sc = ON_Xform::DiagonalTransformation(sx, sy, sz);
        for (int r = 0; r < rows; r++)
          for (int c = 0; c < cols; c++) {
            const ON_3dVector off = R * ON_3dVector(c * dc, r * dr, 0);
            const ON_Xform T = ON_Xform::TranslationTransformation(ON_3dVector(ins.x, ins.y, ins.z) + off);
            ImportedObject o;
            SetAttributes(o, e, inBlock);
            o.block = name;
            o.xform = xf * ocsXf * T * R * Sc * m_Sinv;
            for (const Entity* a : attribs) {
              const std::string tag = Trim(a->S(2));
              if (!tag.empty()) o.userText.emplace_back(tag, DxfPlainText(a->S(1)));
            }
            Track(target, e, inBlock);
            target.push_back(std::move(o));
          }
        for (const Entity* a : attribs)
          if (!(a->I(70) & 1))
            if (ON_Text* tx = TextEntity(*a, xf, a->S(1))) Emit(target, tx, *a, inBlock, xf, false);
      } else if (t == "DIMENSION") {
        if (ON_Dimension* d = DimensionEntity(e, xf)) { Emit(target, d, e, inBlock, xf, false); continue; }
        // quote non convertibili (coordinate, dati incompleti): la geometria del blocco anonimo
        auto bl = m_blocks.find(Key(e.S(2)));
        if (bl == m_blocks.end() || depth > 32) { Skip("quote senza geometria"); continue; }
        std::vector<ImportedObject> parts;
        Convert(bl->second.ents, xf * OcsXform(ocs), parent, depth + 1, parts, true);
        for (auto& o : parts) {
          if (o.layer == "0") o.layer = Trim(e.S(8)).empty() ? "0" : Trim(e.S(8));
          if (o.colorSource == ImportedObject::Source::Parent) {
            ImportedObject tmp;
            SetAttributes(tmp, e, inBlock);
            o.colorSource = tmp.colorSource;
            o.objectColor = tmp.objectColor;
          }
          if (o.linetypeSource == ImportedObject::Source::Parent) o.linetypeSource = ImportedObject::Source::Layer;
          if (o.plotWeightSource == ImportedObject::Source::Parent) o.plotWeightSource = ImportedObject::Source::Layer;
          Track(target, e, inBlock);
          if (!o.block.empty() || o.geom) target.push_back(std::move(o));
        }
      } else if (t == "ACAD_TABLE") {
        // tabella: la sua geometria (blocco anonimo *T)
        auto bl = m_blocks.find(Key(e.S(2)));
        if (bl == m_blocks.end() || depth > 32) { Skip("tabelle"); continue; }
        const ON_3dPoint ins = e.P(10);
        std::vector<ImportedObject> parts;
        Convert(bl->second.ents, xf * ON_Xform::TranslationTransformation(ins - bl->second.base), parent, depth + 1, parts, true);
        for (auto& o : parts) {
          if (o.layer == "0") o.layer = Trim(e.S(8)).empty() ? "0" : Trim(e.S(8));
          Track(target, e, inBlock);
          target.push_back(std::move(o));
        }
      } else if (t == "LEADER") {
        ON_3dPointArray pts;
        double x = 0, y = 0;
        for (const auto& q : e.p) {
          if (q.code == 10) x = Num(q.value);
          else if (q.code == 20) y = Num(q.value);
          else if (q.code == 30) pts.Append(xf * ON_3dPoint(x, y, Num(q.value)));
        }
        if (pts.Count() < 2) { Skip("direttrici non valide"); continue; }
        const ON_DimStyle& ds = StyleFor(e);
        auto* l = new ON_Leader();
        ON_3dVector n = xf * e.Extrusion();
        if (l->Create(L"", &ds, pts.Count(), pts.Array(), AnnotationPlane(pts[0], pts[1] - pts[0], n), false, 0.0)) {
          l->SetDimensionStyleId(ds.Id());
          if (e.I(71, 1) == 0) l->SetLeaderHasLanding(&ds, false);
          Emit(target, l, e, inBlock, xf, false);
        } else {
          delete l;
          Emit(target, new ON_PolylineCurve(pts), e, inBlock, xf, false);
        }
      } else if (t == "MULTILEADER" || t == "MLEADER") {
        MLeader(e, xf, inBlock, target);
      } else if (t == "HATCH") {
        HatchEntity(e, xf, inBlock, target);
      } else if (t == "MESH") {
        // mesh di suddivisione (AutoCAD 2010+): livello 0 della gabbia
        std::vector<ON_3dPoint> v;
        size_t k = 0;
        const auto& p = e.p;
        while (k < p.size() && p[k].code != 92) k++;
        if (k < p.size()) {
          const int nv = Int(p[k++].value);
          for (int q = 0; q < nv && k + 2 < p.size() && p[k].code == 10; q++, k += 3)
            v.push_back(xf * ON_3dPoint(Num(p[k].value), Num(p[k + 1].value), Num(p[k + 2].value)));
        }
        while (k < p.size() && p[k].code != 93) k++;
        MeshBuilder mb;
        std::vector<int> idx;
        for (auto& pt : v) idx.push_back(mb.Vertex(pt));
        if (k < p.size()) {
          const int nf = Int(p[k++].value);
          for (int used = 0; used < nf && k < p.size() && p[k].code == 90;) {
            const int cnt = Int(p[k++].value);
            used++;
            std::vector<int> f;
            for (int q = 0; q < cnt && k < p.size() && p[k].code == 90; q++, used++) {
              const int vi = Int(p[k++].value);
              if (vi >= 0 && vi < (int)idx.size()) f.push_back(idx[vi]);
            }
            if (f.size() == 3) mb.Face(f[0], f[1], f[2]);
            else if (f.size() == 4) mb.Face(f[0], f[1], f[2], f[3]);
            else for (size_t q = 1; q + 1 < f.size(); q++) mb.Face(f[0], f[q], f[q + 1]);
          }
        }
        if (!mb.Empty()) Emit(target, mb.Take().release(), e, inBlock, xf, false);
        else Skip("mesh");
      } else if (t == "XLINE" || t == "RAY") {
        // linea infinita / semiretta: segmento lungo (Rhino non ha linee infinite)
        const ON_3dPoint a = e.P(10);
        ON_3dVector d(e.D(11), e.D(21), e.D(31));
        if (!d.Unitize()) continue;
        const double L = 1e4 / std::max(m_scale, 1e-12);
        Emit(target, new ON_LineCurve(t == "XLINE" ? a - L * d : a, a + L * d), e, inBlock, xf);
      } else if (t == "SEQEND" || t == "VERTEX" || t == "ATTRIB" || t == "VIEWPORT") {
      } else if (t == "3DSOLID" || t == "REGION" || t == "BODY" || t == "SURFACE" || t == "PLANESURFACE" ||
                 t == "EXTRUDEDSURFACE" || t == "LOFTEDSURFACE" || t == "REVOLVEDSURFACE" || t == "SWEPTSURFACE") {
        Skip("solidi/regioni ACIS");
      } else if (t == "IMAGE" || t == "WIPEOUT" || t == "OLE2FRAME" || t == "OLEFRAME") {
        Skip("immagini");
      } else if (t == "TOLERANCE") {
        Skip("tolleranze geometriche");
      } else if (t == "MLINE") {
        // multilinea: la linea di riferimento
        ON_3dPointArray pts;
        double x = 0, y = 0;
        for (const auto& q : e.p) {
          if (q.code == 11) x = Num(q.value);
          else if (q.code == 21) y = Num(q.value);
          else if (q.code == 31) pts.Append(ON_3dPoint(x, y, Num(q.value)));
        }
        if (pts.Count() >= 2) {
          if (e.I(71) & 2) pts.Append(pts[0]);
          Emit(target, new ON_PolylineCurve(pts), e, inBlock, xf);
        }
      } else {
        Skip("entita' " + t);
      }
    }
    if (facesEntity && !faces.Empty()) Emit(target, faces.Take().release(), *facesEntity, inBlock, xf, false);
  }

  // MULTILEADER: linee della direttrice e testo
  void MLeader(const Entity& e, const ON_Xform& xf, bool inBlock, std::vector<ImportedObject>& target) {
    std::string text;
    ON_3dPoint textPos = ON_3dPoint::UnsetPoint;
    double height = 0;
    std::vector<ON_3dPointArray> lines;
    ON_3dPoint landing = ON_3dPoint::UnsetPoint;
    int section = 0;  // 1 CONTEXT, 2 LEADER, 3 LEADER_LINE
    double x = 0, y = 0;
    for (const auto& q : e.p) {
      const std::string v = Trim(q.value);
      if (q.code == 300 && v == "CONTEXT_DATA{") { section = 1; continue; }
      if (q.code == 302 && v == "LEADER{") { section = 2; continue; }
      if (q.code == 304 && v == "LEADER_LINE{") { section = 3; lines.emplace_back(); continue; }
      if (q.code == 305) { section = 2; continue; }
      if (q.code == 303) { section = 1; continue; }
      if (q.code == 301) { section = 0; continue; }
      if (section == 1) {
        if (q.code == 304) text = q.value;
        else if (q.code == 41) height = Num(q.value);
        else if (q.code == 12) x = Num(q.value);
        else if (q.code == 22) y = Num(q.value);
        else if (q.code == 32) textPos = ON_3dPoint(x, y, Num(q.value));
      } else if (section == 2) {
        if (q.code == 10) x = Num(q.value);
        else if (q.code == 20) y = Num(q.value);
        else if (q.code == 30) landing = ON_3dPoint(x, y, Num(q.value));
      } else if (section == 3) {
        if (q.code == 10) x = Num(q.value);
        else if (q.code == 20) y = Num(q.value);
        else if (q.code == 30) lines.back().Append(ON_3dPoint(x, y, Num(q.value)));
      }
    }
    const ON_DimStyle& ds = *m_defaultStyle;
    for (auto& pts : lines) {
      if (landing.IsValid()) pts.Append(landing);
      for (int k = 0; k < pts.Count(); k++) pts[k] = xf * pts[k];
      if (pts.Count() < 2) continue;
      auto* l = new ON_Leader();
      if (l->Create(L"", &ds, pts.Count(), pts.Array(), AnnotationPlane(pts[0], pts[1] - pts[0], ON_3dVector::ZAxis), false, 0.0)) {
        l->SetDimensionStyleId(ds.Id());
        Emit(target, l, e, inBlock, xf, false);
      } else {
        delete l;
        Emit(target, new ON_PolylineCurve(pts), e, inBlock, xf, false);
      }
    }
    if (!text.empty() && textPos.IsValid()) {
      const ON_3dVector X = xf * ON_3dVector::XAxis, N = xf * ON_3dVector::ZAxis;
      if (ON_Text* tx = MakeText(DxfPlainText(text), xf * textPos, X, N, (height > 0 ? height : m_textSize) * XformScale(xf),
                                 ON::TextHorizontalAlignment::Left, ON::TextVerticalAlignment::Top, nullptr))
        Emit(target, tx, e, inBlock, xf, false);
    }
    if (lines.empty() && text.empty()) Skip("direttrici multiple");
  }

  // Gruppi (oggetti GROUP con il nome dal dizionario ACAD_GROUP)
  void AssignGroups() {
    std::map<std::string, std::string> names;  // handle -> nome nel dizionario
    for (const auto& o : m_objects) {
      if (o.type != "DICTIONARY") continue;
      std::string last;
      for (const auto& q : o.p) {
        if (q.code == 3) last = q.value;
        else if ((q.code == 350 || q.code == 360) && !last.empty()) { names[Key(q.value)] = last; last.clear(); }
      }
    }
    for (const auto& o : m_objects) {
      if (o.type != "GROUP") continue;
      std::string name = names.count(Key(o.S(5))) ? names[Key(o.S(5))] : "";
      if (name.empty() || name[0] == '*') name = "Gruppo " + Trim(o.S(5));  // gruppo senza nome
      for (const auto& q : o.p) {
        if (q.code != 340) continue;
        auto f = m_handleObjects.find(Key(q.value));
        if (f == m_handleObjects.end()) continue;
        for (size_t idx : f->second)
          if (idx < m_out.objects.size()) m_out.objects[idx].groups.push_back(name);
      }
    }
  }
};

}  // namespace

bool ReadDXF(const std::string& path, ON::LengthUnitSystem docUnits, ImportedModel& out, std::string& err) {
  DxfReader r(out, docUnits);
  return r.Read(path, err);
}

// --- Scrittura (DXF R12 / AC1009: letto da tutti i programmi) --------------------------------
namespace {

std::string DxfName(std::string l) {
  for (auto& c : l)
    if (c == '<' || c == '>' || c == '/' || c == '\\' || c == ':' || c == '"' || c == ';' || c == '?' || c == '*' ||
        c == '|' || c == '=' || c == ',' || c == '`')
      c = '_';
  return l.empty() ? "0" : l;
}

class DxfWriter {
 public:
  DxfWriter(const ONX_Model* model, double tol) : m_model(model), m_tol(tol) { m_s.precision(12); }

  const ONX_Model* m_model = nullptr;
  double m_tol = 0.001;
  std::ostringstream m_s;
  std::map<std::string, ON_Color> m_layers{{"0", ON_Color::Black}};
  std::map<std::string, const ON_Linetype*> m_linetypes;
  std::map<std::string, std::string> m_blockBodies;  // nome -> entita'
  std::set<ON_UUID, UuidLess> m_blocksDone;
  int m_count = 0;

  void Pt(std::ostringstream& s, int code, const ON_3dPoint& p) {
    s << code << "\n" << p.x << "\n" << code + 10 << "\n" << p.y << "\n" << code + 20 << "\n" << p.z << "\n";
  }
  struct Style {
    std::string layer;
    int color = 256;  // 256 DaLayer, 0 DaBlocco
    std::string linetype;  // vuoto = DaLayer
  };
  void Head(std::ostringstream& s, const char* type, const Style& st) {
    s << "0\n" << type << "\n8\n" << st.layer << "\n";
    if (!st.linetype.empty()) s << "6\n" << st.linetype << "\n";
    if (st.color != 256) s << "62\n" << st.color << "\n";
  }

  std::string LinetypeName(const ON_Linetype* lt) {
    if (!lt) return "";
    std::string n = DxfName(ToUtf8(lt->Name()));
    if (Upper(n) == "CONTINUOUS") return "CONTINUOUS";
    m_linetypes[n] = lt;
    return n;
  }

  Style StyleOf(const ON_3dmObjectAttributes* a, const std::string& layerName, const ON_Color& layerColor) {
    Style st;
    st.layer = DxfName(layerName);
    if (!m_layers.count(st.layer)) m_layers[st.layer] = layerColor;
    if (!a) return st;
    if (a->ColorSource() == ON::color_from_object) st.color = NearestAci(a->m_color);
    else if (a->ColorSource() == ON::color_from_parent) st.color = 0;
    if (a->LinetypeSource() == ON::linetype_from_object && m_model) {
      if (const auto* lt = ON_Linetype::Cast(m_model->LinePatternFromIndex(a->m_linetype_index).ModelComponent()))
        st.linetype = LinetypeName(lt);
      else st.linetype = "CONTINUOUS";
    } else if (a->LinetypeSource() == ON::linetype_from_parent) st.linetype = "BYBLOCK";
    return st;
  }

  void Polyline(std::ostringstream& s, const Style& st, const ON_Polyline& pl, bool closed) {
    if (pl.Count() == 2 && !closed) {
      Head(s, "LINE", st);
      Pt(s, 10, pl[0]);
      Pt(s, 11, pl[1]);
      return;
    }
    Head(s, "POLYLINE", st);
    s << "66\n1\n10\n0\n20\n0\n30\n0\n70\n" << (8 | (closed ? 1 : 0)) << "\n";
    for (int k = 0; k < pl.Count() - (closed ? 1 : 0); k++) {
      s << "0\nVERTEX\n8\n" << st.layer << "\n";
      Pt(s, 10, pl[k]);
      s << "70\n32\n";
    }
    s << "0\nSEQEND\n8\n" << st.layer << "\n";
  }

  void Curve(std::ostringstream& s, const Style& st, const ON_Curve& c) {
    ON_Arc arc;
    const bool planarZ = c.IsArc(nullptr, &arc, m_tol) && std::fabs(std::fabs(arc.plane.zaxis.z) - 1) < 1e-9;
    if (const auto* lc = ON_LineCurve::Cast(&c)) {
      Head(s, "LINE", st);
      Pt(s, 10, lc->m_line.from);
      Pt(s, 11, lc->m_line.to);
    } else if (planarZ) {
      const ON_3dVector nz = arc.plane.zaxis.z > 0 ? ON_3dVector::ZAxis : -ON_3dVector::ZAxis;
      const ON_Plane ocs = OcsPlane(nz);
      ON_3dPoint cc(arc.Center() - ON_3dPoint::Origin);
      const ON_3dPoint oc(cc * ocs.xaxis, cc * ocs.yaxis, cc * ocs.zaxis);
      Head(s, arc.IsCircle() ? "CIRCLE" : "ARC", st);
      Pt(s, 10, oc);
      s << "40\n" << arc.Radius() << "\n";
      if (nz.z < 0) s << "210\n0\n220\n0\n230\n-1\n";
      if (!arc.IsCircle()) {
        auto ang = [&](const ON_3dPoint& p) {
          const ON_3dVector v = p - arc.Center();
          double a = std::atan2(v * ocs.yaxis, v * ocs.xaxis) * 180 / ON_PI;
          return a < 0 ? a + 360 : a;
        };
        ON_3dPoint a0 = arc.StartPoint(), a1 = arc.EndPoint();
        if (ON_CrossProduct(arc.plane.xaxis, arc.plane.yaxis) * nz < 0) std::swap(a0, a1);
        s << "50\n" << ang(a0) << "\n51\n" << ang(a1) << "\n";
      }
    } else if (const auto* pc = ON_PolyCurve::Cast(&c); pc && !c.IsPolyline()) {
      for (int k = 0; k < pc->Count(); k++) if (const ON_Curve* seg = pc->SegmentCurve(k)) Curve(s, st, *seg);
    } else {
      ON_Polyline pl;
      if (c.IsPolyline(&pl) < 2) {
        pl.Empty();
        const ON_Interval d = c.Domain();
        const int segs = std::max(16, 8 * std::max(1, c.SpanCount()) * std::max(1, c.Degree()));
        for (int k = 0; k <= segs; k++) pl.Append(c.PointAt(d.ParameterAt((double)k / segs)));
      }
      Polyline(s, st, pl, c.IsClosed());
    }
  }

  void Text(std::ostringstream& s, const Style& st, const AnnotationLabel& L) {
    std::vector<std::string> lines;
    std::stringstream ss(L.text);
    for (std::string l; std::getline(ss, l);) lines.push_back(l);
    const double rot = std::atan2(L.xdir.y, L.xdir.x) * 180 / ON_PI;
    for (size_t k = 0; k < lines.size(); k++) {
      if (lines[k].empty()) continue;
      const ON_3dPoint p = L.origin - (double)k * 1.6 * L.height * L.ydir;
      Head(s, "TEXT", st);
      Pt(s, 10, p);
      s << "40\n" << L.height << "\n1\n" << lines[k] << "\n";
      if (std::fabs(rot) > 1e-9) s << "50\n" << rot << "\n";
      if (L.halign) { s << "72\n" << L.halign << "\n"; Pt(s, 11, p); }
    }
  }

  static bool IsTriangle(const std::vector<ON_3dPoint>& tris, const std::vector<ON_3dPoint>& w) {
    for (size_t k = 0; k + 2 < tris.size(); k += 3)
      if (tris[k].DistanceTo(w[0]) < 1e-12 && tris[k + 1].DistanceTo(w[1]) < 1e-12 && tris[k + 2].DistanceTo(w[2]) < 1e-12)
        return true;
    return false;
  }

  // Annotazioni (quote, testi, direttrici): la loro geometria di visualizzazione.
  void Annotation(std::ostringstream& s, const Style& st, const ON_Annotation& a) {
    AnnotationDisplay d;
    if (!m_model || !BuildAnnotationDisplay(*m_model, a, d)) return;
    for (const auto& w : d.wires) {
      // contorno di una freccia piena: basta il SOLID (come il blocco _CLOSEDFILLED di AutoCAD)
      if (w.size() == 4 && w.front().DistanceTo(w.back()) < 1e-12 && IsTriangle(d.triangles, w)) continue;
      ON_Polyline pl;
      for (const auto& p : w) pl.Append(p);
      if (pl.Count() >= 2) Polyline(s, st, pl, false);
    }
    for (size_t k = 0; k + 2 < d.triangles.size(); k += 3) {
      Head(s, "SOLID", st);
      Pt(s, 10, d.triangles[k]);
      Pt(s, 11, d.triangles[k + 1]);
      Pt(s, 12, d.triangles[k + 2]);
      Pt(s, 13, d.triangles[k + 2]);
    }
    for (const auto& L : d.labels) Text(s, st, L);
  }

  // Geometria di un oggetto. false = non esportabile.
  bool Geometry(std::ostringstream& s, const Style& st, const ON_Geometry* g, int depth) {
    if (const auto* p = ON_Point::Cast(g)) { Head(s, "POINT", st); Pt(s, 10, p->point); return true; }
    if (const auto* pc = ON_PointCloud::Cast(g)) {
      for (int k = 0; k < pc->m_P.Count(); k++) { Head(s, "POINT", st); Pt(s, 10, pc->m_P[k]); }
      return pc->m_P.Count() > 0;
    }
    if (const auto* c = ON_Curve::Cast(g)) { Curve(s, st, *c); return true; }
    if (const auto* dot = ON_TextDot::Cast(g)) {
      Head(s, "TEXT", st);
      Pt(s, 10, dot->CenterPoint());
      s << "40\n" << 1.0 << "\n1\n" << ToUtf8(dot->PrimaryText()) << "\n";
      return true;
    }
    if (const auto* an = ON_Annotation::Cast(g)) { Annotation(s, st, *an); return true; }
    if (const auto* h = ON_Hatch::Cast(g)) {
      // R12 non ha i retini: i contorni
      bool any = false;
      for (int k = 0; k < h->LoopCount(); k++)
        if (ON_Curve* c = h->LoopCurve3d(k)) { Curve(s, st, *c); delete c; any = true; }
      return any;
    }
    if (const auto* ir = ON_InstanceRef::Cast(g)) return Instance(s, st, *ir, depth);
    if (auto m = ExportMesh(*g, m_tol)) {
      for (int k = 0; k < m->m_F.Count(); k++) {
        const ON_MeshFace& f = m->m_F[k];
        Head(s, "3DFACE", st);
        for (int j = 0; j < 4; j++) Pt(s, 10 + j, ON_3dPoint(m->m_V[f.vi[j]]));
      }
      return true;
    }
    return false;
  }

  // Istanza di blocco: BLOCK + INSERT se la trasformazione si scompone in posizione, rotazione
  // attorno a Z e scala; altrimenti la geometria del blocco esplosa.
  bool Instance(std::ostringstream& s, const Style& st, const ON_InstanceRef& ir, int depth) {
    if (!m_model || depth > 16) return false;
    const auto* idef = ON_InstanceDefinition::Cast(
        m_model->ComponentFromId(ON_ModelComponent::Type::InstanceDefinition, ir.m_instance_definition_uuid).ModelComponent());
    if (!idef) return false;
    const ON_Xform& x = ir.m_xform;
    const ON_3dVector X(x.m_xform[0][0], x.m_xform[1][0], x.m_xform[2][0]);
    const ON_3dVector Y(x.m_xform[0][1], x.m_xform[1][1], x.m_xform[2][1]);
    const ON_3dVector Z(x.m_xform[0][2], x.m_xform[1][2], x.m_xform[2][2]);
    const bool simple = std::fabs(X.z) < 1e-9 && std::fabs(Y.z) < 1e-9 && std::fabs(Z.x) < 1e-9 && std::fabs(Z.y) < 1e-9 &&
                        std::fabs(X * Y) < 1e-9 * X.Length() * Y.Length() && x.m_xform[3][0] == 0 && x.m_xform[3][1] == 0 &&
                        x.m_xform[3][2] == 0 && std::fabs(x.m_xform[3][3] - 1) < 1e-12;
    if (simple) {
      const std::string name = Block(*idef, depth);
      if (name.empty()) return false;
      const double sx = X.Length(), sz = Z.z;
      const double sy = (ON_CrossProduct(X, Y).z < 0 ? -1 : 1) * Y.Length();
      const double rot = std::atan2(X.y, X.x) * 180 / ON_PI;
      Head(s, "INSERT", st);
      s << "2\n" << name << "\n";
      Pt(s, 10, ON_3dPoint(x.m_xform[0][3], x.m_xform[1][3], x.m_xform[2][3]));
      if (std::fabs(sx - 1) > 1e-12) s << "41\n" << sx << "\n";
      if (std::fabs(sy - 1) > 1e-12) s << "42\n" << sy << "\n";
      if (std::fabs(sz - 1) > 1e-12) s << "43\n" << sz << "\n";
      if (std::fabs(rot) > 1e-12) s << "50\n" << rot << "\n";
      return true;
    }
    bool any = false;
    const auto& ids = idef->InstanceGeometryIdList();
    for (int k = 0; k < ids.Count(); k++) {
      const auto* mg = ON_ModelGeometryComponent::Cast(m_model->ComponentFromId(ON_ModelComponent::Type::ModelGeometry, ids[k]).ModelComponent());
      const ON_Geometry* g = mg ? mg->Geometry(nullptr) : nullptr;
      if (!g) continue;
      std::unique_ptr<ON_Geometry> copy(static_cast<ON_Geometry*>(g->Duplicate()));
      copy->Transform(x);
      any |= Geometry(s, st, copy.get(), depth + 1);
    }
    return any;
  }

  std::string Block(const ON_InstanceDefinition& idef, int depth) {
    std::string name = DxfName(ToUtf8(idef.Name()));
    if (name == "0") name = "Blocco";
    if (m_blocksDone.count(idef.Id())) return name;
    m_blocksDone.insert(idef.Id());
    std::ostringstream body;
    body.precision(12);
    const auto& ids = idef.InstanceGeometryIdList();
    for (int k = 0; k < ids.Count(); k++) {
      const auto* mg = ON_ModelGeometryComponent::Cast(m_model->ComponentFromId(ON_ModelComponent::Type::ModelGeometry, ids[k]).ModelComponent());
      const ON_Geometry* g = mg ? mg->Geometry(nullptr) : nullptr;
      if (!g) continue;
      const ON_3dmObjectAttributes* a = mg->Attributes(nullptr);
      std::string layer = "0";
      ON_Color lc = ON_Color::Black;
      if (a) if (const auto* l = ON_Layer::Cast(m_model->LayerFromIndex(a->m_layer_index).ModelComponent())) { layer = ToUtf8(l->Name()); lc = l->Color(); }
      Geometry(body, StyleOf(a, layer, lc), g, depth + 1);
    }
    m_blockBodies[name] = body.str();
    return name;
  }
};

}  // namespace

bool WriteDXF(const std::string& path, const std::vector<ExportItem>& items, ON::LengthUnitSystem docUnits, double tol,
              std::string& err, const ONX_Model* model) {
  DxfWriter w(model, tol);
  std::map<std::string, const ON_Layer*> layerObjs;
  for (const auto& it : items) {
    const ON_Layer* layer = nullptr;
    if (model && it.attributes) layer = ON_Layer::Cast(model->LayerFromIndex(it.attributes->m_layer_index).ModelComponent());
    const std::string ln = it.layer.empty() ? "0" : it.layer;
    auto st = w.StyleOf(it.attributes, ln, layer ? layer->Color() : it.color);
    if (!it.attributes && it.color != ON_Color::Black && it.color != ON_UNSET_COLOR) w.m_layers[st.layer] = it.color;
    if (layer) layerObjs[st.layer] = layer;
    if (w.Geometry(w.m_s, st, it.geom, 0)) w.m_count++;
  }
  if (!w.m_count) { err = "nessun oggetto esportabile in DXF"; return false; }
  // anche i livelli senza oggetti esportati (come Rhino: la tabella dei livelli resta completa)
  if (model) {
    ONX_ModelComponentIterator li(*model, ON_ModelComponent::Type::Layer);
    for (const ON_ModelComponent* c = li.FirstComponent(); c; c = li.NextComponent()) {
      const auto* l = ON_Layer::Cast(c);
      if (!l || l->IsDeleted() || l->Name().IsEmpty()) continue;
      const std::string n = DxfName(ToUtf8(l->Name()));
      if (!w.m_layers.count(n)) w.m_layers[n] = l->Color();
      if (!layerObjs.count(n)) layerObjs[n] = l;
    }
  }
  // tipi di linea dei livelli
  std::map<std::string, std::string> layerLinetype;
  for (const auto& [name, l] : layerObjs)
    if (const auto* lt = ON_Linetype::Cast(model->LinePatternFromIndex(l->LinetypeIndex()).ModelComponent()))
      layerLinetype[name] = w.LinetypeName(lt);
  std::ofstream f(fs::u8path(path), std::ios::binary);
  if (!f) { err = "impossibile scrivere " + path; return false; }
  f.precision(12);
  const bool metric = docUnits != ON::LengthUnitSystem::Inches && docUnits != ON::LengthUnitSystem::Feet &&
                      docUnits != ON::LengthUnitSystem::Yards && docUnits != ON::LengthUnitSystem::Miles &&
                      docUnits != ON::LengthUnitSystem::Mils && docUnits != ON::LengthUnitSystem::Microinches;
  f << "0\nSECTION\n2\nHEADER\n9\n$ACADVER\n1\nAC1009\n9\n$INSUNITS\n70\n" << InsUnitsCode(docUnits)
    << "\n9\n$MEASUREMENT\n70\n" << (metric ? 1 : 0) << "\n0\nENDSEC\n";
  f << "0\nSECTION\n2\nTABLES\n";
  // tipi di linea
  f << "0\nTABLE\n2\nLTYPE\n70\n" << w.m_linetypes.size() + 1 << "\n";
  f << "0\nLTYPE\n2\nCONTINUOUS\n70\n0\n3\nSolid line\n72\n65\n73\n0\n40\n0.0\n";
  for (const auto& [name, lt] : w.m_linetypes) {
    double total = 0;
    std::vector<double> segs;
    for (int k = 0; k < lt->SegmentCount(); k++) {
      const ON_LinetypeSegment sg = lt->Segment(k);
      const double v = sg.m_seg_type == ON_LinetypeSegment::eSegType::stSpace ? -sg.m_length : sg.m_length;
      segs.push_back(v);
      total += std::fabs(v);
    }
    f << "0\nLTYPE\n2\n" << name << "\n70\n0\n3\n" << name << "\n72\n65\n73\n" << segs.size() << "\n40\n" << total << "\n";
    for (double v : segs) f << "49\n" << v << "\n";
  }
  f << "0\nENDTAB\n";
  // livelli
  f << "0\nTABLE\n2\nLAYER\n70\n" << w.m_layers.size() << "\n";
  for (const auto& [name, col] : w.m_layers) {
    const ON_Layer* l = layerObjs.count(name) ? layerObjs[name] : nullptr;
    int aci = NearestAci(l ? l->Color() : col);
    int flags = 0;
    if (l && !l->IsVisible()) aci = -aci;
    if (l && l->IsLocked()) flags |= 4;
    const std::string lt = layerLinetype.count(name) ? layerLinetype[name] : "CONTINUOUS";
    f << "0\nLAYER\n2\n" << name << "\n70\n" << flags << "\n62\n" << aci << "\n6\n" << lt << "\n";
  }
  f << "0\nENDTAB\n";
  f << "0\nTABLE\n2\nSTYLE\n70\n1\n0\nSTYLE\n2\nSTANDARD\n70\n0\n40\n0.0\n41\n1.0\n50\n0.0\n71\n0\n42\n2.5\n3\ntxt\n4\n\n0\nENDTAB\n";
  f << "0\nENDSEC\n";
  // blocchi
  f << "0\nSECTION\n2\nBLOCKS\n";
  for (const auto& [name, body] : w.m_blockBodies) {
    f << "0\nBLOCK\n8\n0\n2\n" << name << "\n70\n0\n10\n0.0\n20\n0.0\n30\n0.0\n3\n" << name << "\n" << body;
    f << "0\nENDBLK\n8\n0\n";
  }
  f << "0\nENDSEC\n";
  f << "0\nSECTION\n2\nENTITIES\n" << w.m_s.str() << "0\nENDSEC\n0\nEOF\n";
  if (!f) { err = "errore di scrittura"; return false; }
  return true;
}

}  // namespace cad
