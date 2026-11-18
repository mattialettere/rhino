// Formati mesh aperti: glTF 2.0 (.gltf/.glb), 3MF, PLY. Scritti per Kurva3D a partire
// dalle specifiche pubbliche (Khronos glTF 2.0, 3MF Core Specification 1.3, PLY di Stanford).
#include "commands/Exchange.h"
#include "core/Document3dm.h"
#include "zlib/zlib.h"  // zlib di openNURBS (nomi con prefisso z_)
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <regex>
#include <sstream>

namespace cad {
namespace {
namespace fs = std::filesystem;

std::string U8(const fs::path& p) {
  const auto u = p.u8string();
  return std::string(u.begin(), u.end());
}

bool ReadFile(const std::string& path, std::vector<unsigned char>& data) {
  std::ifstream f(fs::u8path(path), std::ios::binary);
  if (!f) return false;
  data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

std::string MimeOf(const std::string& path) {
  std::string e = FileExtension(path);
  if (e == "jpg" || e == "jpeg") return "image/jpeg";
  if (e == "png") return "image/png";
  if (e == "bmp") return "image/bmp";
  return "image/png";
}

std::string ExtOfMime(const std::string& mime) {
  if (mime == "image/jpeg") return ".jpg";
  if (mime == "image/bmp") return ".bmp";
  return ".png";
}

// Metri per unita' del documento.
double MetersPerUnit(ON::LengthUnitSystem u) { return ON::UnitScale(u, ON::LengthUnitSystem::Meters); }

// --- base64 -------------------------------------------------------------------------------
const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64(const unsigned char* p, size_t n) {
  std::string out;
  out.reserve((n + 2) / 3 * 4);
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = p[i] << 16;
    if (i + 1 < n) v |= p[i + 1] << 8;
    if (i + 2 < n) v |= p[i + 2];
    out += kB64[(v >> 18) & 63];
    out += kB64[(v >> 12) & 63];
    out += i + 1 < n ? kB64[(v >> 6) & 63] : '=';
    out += i + 2 < n ? kB64[v & 63] : '=';
  }
  return out;
}

std::vector<unsigned char> Unbase64(const std::string& s) {
  std::vector<unsigned char> out;
  uint32_t v = 0;
  int bits = 0;
  for (char c : s) {
    const char* q = std::strchr(kB64, c);
    if (!q || c == 0) continue;
    v = (v << 6) | static_cast<uint32_t>(q - kB64);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<unsigned char>((v >> bits) & 0xFF));
    }
  }
  return out;
}

// --- JSON minimo ----------------------------------------------------------------------------
struct Json {
  enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<Json> a;
  std::vector<std::pair<std::string, Json>> o;

  const Json* Get(const std::string& k) const {
    if (kind != Object) return nullptr;
    for (auto& [key, v] : o)
      if (key == k) return &v;
    return nullptr;
  }
  const Json* At(size_t i) const { return kind == Array && i < a.size() ? &a[i] : nullptr; }
  double Num(const std::string& k, double def) const {
    const Json* v = Get(k);
    return v && v->kind == Number ? v->n : def;
  }
  int Int(const std::string& k, int def) const { return static_cast<int>(Num(k, def)); }
  std::string Str(const std::string& k) const {
    const Json* v = Get(k);
    return v && v->kind == String ? v->s : std::string();
  }
};

class JsonParser {
 public:
  explicit JsonParser(const std::string& t) : s(t) {}
  bool Parse(Json& out) {
    Ws();
    if (!Value(out, 0)) return false;
    Ws();
    return true;
  }

 private:
  void Ws() {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
  }
  bool Lit(const char* w) {
    const size_t n = std::strlen(w);
    if (s.compare(i, n, w) != 0) return false;
    i += n;
    return true;
  }
  static void Utf8(std::string& out, uint32_t c) {
    if (c < 0x80) out += static_cast<char>(c);
    else if (c < 0x800) { out += static_cast<char>(0xC0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 63)); }
    else if (c < 0x10000) {
      out += static_cast<char>(0xE0 | (c >> 12));
      out += static_cast<char>(0x80 | ((c >> 6) & 63));
      out += static_cast<char>(0x80 | (c & 63));
    } else {
      out += static_cast<char>(0xF0 | (c >> 18));
      out += static_cast<char>(0x80 | ((c >> 12) & 63));
      out += static_cast<char>(0x80 | ((c >> 6) & 63));
      out += static_cast<char>(0x80 | (c & 63));
    }
  }
  bool Str(std::string& out) {
    if (i >= s.size() || s[i] != '"') return false;
    i++;
    while (i < s.size() && s[i] != '"') {
      char c = s[i++];
      if (c != '\\') { out += c; continue; }
      if (i >= s.size()) return false;
      c = s[i++];
      switch (c) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u': {
          if (i + 4 > s.size()) return false;
          uint32_t cp = static_cast<uint32_t>(std::stoul(s.substr(i, 4), nullptr, 16));
          i += 4;
          if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
            const uint32_t lo = static_cast<uint32_t>(std::stoul(s.substr(i + 2, 4), nullptr, 16));
            i += 6;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          Utf8(out, cp);
          break;
        }
        default: out += c;
      }
    }
    if (i >= s.size()) return false;
    i++;
    return true;
  }
  bool Value(Json& v, int depth) {
    if (depth > 200 || i >= s.size()) return false;
    const char c = s[i];
    if (c == '{') {
      v.kind = Json::Object;
      i++;
      Ws();
      if (i < s.size() && s[i] == '}') { i++; return true; }
      while (true) {
        Ws();
        std::string k;
        if (!Str(k)) return false;
        Ws();
        if (i >= s.size() || s[i] != ':') return false;
        i++;
        Ws();
        Json item;
        if (!Value(item, depth + 1)) return false;
        v.o.emplace_back(std::move(k), std::move(item));
        Ws();
        if (i < s.size() && s[i] == ',') { i++; continue; }
        if (i < s.size() && s[i] == '}') { i++; return true; }
        return false;
      }
    }
    if (c == '[') {
      v.kind = Json::Array;
      i++;
      Ws();
      if (i < s.size() && s[i] == ']') { i++; return true; }
      while (true) {
        Ws();
        Json item;
        if (!Value(item, depth + 1)) return false;
        v.a.push_back(std::move(item));
        Ws();
        if (i < s.size() && s[i] == ',') { i++; continue; }
        if (i < s.size() && s[i] == ']') { i++; return true; }
        return false;
      }
    }
    if (c == '"') { v.kind = Json::String; return Str(v.s); }
    if (Lit("true")) { v.kind = Json::Bool; v.b = true; return true; }
    if (Lit("false")) { v.kind = Json::Bool; return true; }
    if (Lit("null")) return true;
    char* end = nullptr;
    v.n = std::strtod(s.c_str() + i, &end);
    if (end == s.c_str() + i) return false;
    v.kind = Json::Number;
    i = static_cast<size_t>(end - s.c_str());
    return true;
  }
  const std::string& s;
  size_t i = 0;
};

std::string JsonEscape(const std::string& t) {
  std::string o = "\"";
  for (unsigned char c : t) {
    if (c == '"' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
    else if (c < 0x20) {
      char b[8];
      std::snprintf(b, sizeof b, "\\u%04x", c);
      o += b;
    } else o += static_cast<char>(c);
  }
  return o + "\"";
}

std::string Fmt(double v) {
  char b[40];
  std::snprintf(b, sizeof b, "%.9g", v);
  return b;
}

// --- mesh comuni ------------------------------------------------------------------------------
// Triangoli (i quadrilateri divisi in due).
std::vector<std::array<int, 3>> Triangles(const ON_Mesh& m) {
  std::vector<std::array<int, 3>> t;
  t.reserve(static_cast<size_t>(m.m_F.Count()) * 2);
  for (int i = 0; i < m.m_F.Count(); i++) {
    const ON_MeshFace& f = m.m_F[i];
    t.push_back({f.vi[0], f.vi[1], f.vi[2]});
    if (f.IsQuad()) t.push_back({f.vi[0], f.vi[2], f.vi[3]});
  }
  return t;
}

std::unique_ptr<ON_Mesh> NewMesh(const std::vector<ON_3dPoint>& v, const std::vector<std::array<int, 3>>& tri) {
  auto m = std::make_unique<ON_Mesh>(static_cast<int>(tri.size()), static_cast<int>(v.size()), false, false);
  for (const auto& p : v) m->m_V.Append(ON_3fPoint(p));
  for (const auto& t : tri)
    if (t[0] >= 0 && t[1] >= 0 && t[2] >= 0 && t[0] < (int)v.size() && t[1] < (int)v.size() && t[2] < (int)v.size())
      m->SetTriangle(m->m_F.Count(), t[0], t[1], t[2]);
  return m;
}

// Byte dell'immagine della texture (dal file, o gia' presenti nel materiale).
bool TextureBytes(const ExchangeMaterial& mat, std::vector<unsigned char>& bytes, std::string& mime) {
  if (!mat.textureData.empty()) {
    bytes = mat.textureData;
    mime = mat.textureMime.empty() ? "image/png" : mat.textureMime;
    return true;
  }
  if (mat.texture.empty() || !ReadFile(mat.texture, bytes) || bytes.empty()) return false;
  mime = MimeOf(mat.texture);
  return true;
}

// --- glTF -------------------------------------------------------------------------------------
class GltfWriter {
 public:
  explicit GltfWriter(bool glb) : m_glb(glb) {}

  void Align() {
    while (m_bin.size() % 4) m_bin.push_back(0);
  }
  int View(const void* data, size_t n, int target) {
    Align();
    const size_t off = m_bin.size();
    const auto* p = static_cast<const unsigned char*>(data);
    m_bin.insert(m_bin.end(), p, p + n);
    std::ostringstream v;
    v << "{\"buffer\":0,\"byteOffset\":" << off << ",\"byteLength\":" << n;
    if (target) v << ",\"target\":" << target;
    v << "}";
    m_views.push_back(v.str());
    return static_cast<int>(m_views.size()) - 1;
  }
  int Accessor(int view, int componentType, size_t count, const char* type, const std::string& extra = "") {
    std::ostringstream a;
    a << "{\"bufferView\":" << view << ",\"componentType\":" << componentType << ",\"count\":" << count
      << ",\"type\":\"" << type << "\"" << extra << "}";
    m_accessors.push_back(a.str());
    return static_cast<int>(m_accessors.size()) - 1;
  }

  int Material(const ExchangeMaterial* mat) {
    if (!mat) return -1;
    if (auto f = m_matIndex.find(mat); f != m_matIndex.end()) return f->second;
    std::ostringstream m;
    const double a = 1.0 - std::clamp(mat->transparency, 0.0, 1.0);
    m << "{\"name\":" << JsonEscape(mat->name) << ",\"pbrMetallicRoughness\":{\"baseColorFactor\":["
      << Fmt(mat->diffuse.FractionRed()) << "," << Fmt(mat->diffuse.FractionGreen()) << ","
      << Fmt(mat->diffuse.FractionBlue()) << "," << Fmt(a) << "]";
    const double metal = mat->pbr ? mat->metallic : 0.0;
    const double rough = mat->pbr ? mat->roughness : std::clamp(1.0 - mat->shine, 0.0, 1.0);
    m << ",\"metallicFactor\":" << Fmt(metal) << ",\"roughnessFactor\":" << Fmt(rough);
    std::vector<unsigned char> img;
    std::string mime;
    if (TextureBytes(*mat, img, mime)) {
      std::ostringstream im;
      if (m_glb) {
        const int v = View(img.data(), img.size(), 0);
        im << "{\"bufferView\":" << v << ",\"mimeType\":\"" << mime << "\"}";
      } else {
        im << "{\"uri\":\"data:" << mime << ";base64," << Base64(img.data(), img.size()) << "\"}";
      }
      m_images.push_back(im.str());
      m_textures.push_back("{\"source\":" + std::to_string(m_images.size() - 1) + "}");
      m << ",\"baseColorTexture\":{\"index\":" << m_textures.size() - 1 << "}";
    }
    m << "}";
    if (mat->emission.Red() || mat->emission.Green() || mat->emission.Blue())
      m << ",\"emissiveFactor\":[" << Fmt(mat->emission.FractionRed()) << "," << Fmt(mat->emission.FractionGreen()) << ","
        << Fmt(mat->emission.FractionBlue()) << "]";
    if (a < 1.0) m << ",\"alphaMode\":\"BLEND\"";
    m << ",\"doubleSided\":true}";
    m_materials.push_back(m.str());
    return m_matIndex[mat] = static_cast<int>(m_materials.size()) - 1;
  }

  // mesh gia' convertita in Y in alto e metri
  void AddMesh(const std::string& name, const ON_Mesh& mesh, const ExchangeMaterial* mat) {
    const int nv = mesh.m_V.Count();
    std::vector<float> pos(static_cast<size_t>(nv) * 3), nor;
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (int i = 0; i < nv; i++)
      for (int k = 0; k < 3; k++) {
        const float c = mesh.m_V[i][k];
        pos[i * 3 + k] = c;
        mn[k] = std::min(mn[k], c);
        mx[k] = std::max(mx[k], c);
      }
    std::ostringstream ext;
    ext << ",\"min\":[" << Fmt(mn[0]) << "," << Fmt(mn[1]) << "," << Fmt(mn[2]) << "],\"max\":[" << Fmt(mx[0]) << ","
        << Fmt(mx[1]) << "," << Fmt(mx[2]) << "]";
    std::ostringstream prim;
    const int aPos = Accessor(View(pos.data(), pos.size() * 4, 34962), 5126, nv, "VEC3", ext.str());
    prim << "{\"attributes\":{\"POSITION\":" << aPos;
    if (mesh.m_N.Count() == nv) {
      nor.resize(static_cast<size_t>(nv) * 3);
      for (int i = 0; i < nv; i++) {
        ON_3fVector n = mesh.m_N[i];
        n.Unitize();
        nor[i * 3] = n.x, nor[i * 3 + 1] = n.y, nor[i * 3 + 2] = n.z;
      }
      prim << ",\"NORMAL\":" << Accessor(View(nor.data(), nor.size() * 4, 34962), 5126, nv, "VEC3");
    }
    if (mesh.m_T.Count() == nv) {
      std::vector<float> uv(static_cast<size_t>(nv) * 2);
      for (int i = 0; i < nv; i++) uv[i * 2] = mesh.m_T[i].x, uv[i * 2 + 1] = 1.0f - mesh.m_T[i].y;
      prim << ",\"TEXCOORD_0\":" << Accessor(View(uv.data(), uv.size() * 4, 34962), 5126, nv, "VEC2");
    }
    if (mesh.m_C.Count() == nv) {
      std::vector<unsigned char> col(static_cast<size_t>(nv) * 4);
      for (int i = 0; i < nv; i++) {
        col[i * 4] = static_cast<unsigned char>(mesh.m_C[i].Red());
        col[i * 4 + 1] = static_cast<unsigned char>(mesh.m_C[i].Green());
        col[i * 4 + 2] = static_cast<unsigned char>(mesh.m_C[i].Blue());
        col[i * 4 + 3] = 255;
      }
      prim << ",\"COLOR_0\":" << Accessor(View(col.data(), col.size(), 34962), 5121, nv, "VEC4", ",\"normalized\":true");
    }
    std::vector<uint32_t> idx;
    for (const auto& t : Triangles(mesh)) idx.insert(idx.end(), {(uint32_t)t[0], (uint32_t)t[1], (uint32_t)t[2]});
    prim << "},\"indices\":" << Accessor(View(idx.data(), idx.size() * 4, 34963), 5125, idx.size(), "SCALAR")
         << ",\"mode\":4";
    if (const int mi = Material(mat); mi >= 0) prim << ",\"material\":" << mi;
    prim << "}";
    m_meshes.push_back("{\"name\":" + JsonEscape(name) + ",\"primitives\":[" + prim.str() + "]}");
    m_nodes.push_back("{\"name\":" + JsonEscape(name) + ",\"mesh\":" + std::to_string(m_meshes.size() - 1) + "}");
  }

  bool Write(const std::string& path, std::string& err) {
    if (m_meshes.empty()) { err = "nessun oggetto esportabile in glTF (servono superfici, solidi, mesh o SubD)"; return false; }
    Align();
    auto list = [](const std::vector<std::string>& v) {
      std::string s = "[";
      for (size_t i = 0; i < v.size(); i++) s += (i ? "," : "") + v[i];
      return s + "]";
    };
    std::ostringstream j;
    j << "{\"asset\":{\"version\":\"2.0\",\"generator\":\"Kurva3D\"},\"scene\":0,\"scenes\":[{\"nodes\":[";
    for (size_t i = 0; i < m_nodes.size(); i++) j << (i ? "," : "") << i;
    j << "]}],\"nodes\":" << list(m_nodes) << ",\"meshes\":" << list(m_meshes);
    if (!m_materials.empty()) j << ",\"materials\":" << list(m_materials);
    if (!m_textures.empty()) j << ",\"textures\":" << list(m_textures) << ",\"images\":" << list(m_images);
    j << ",\"accessors\":" << list(m_accessors) << ",\"bufferViews\":" << list(m_views) << ",\"buffers\":[{\"byteLength\":"
      << m_bin.size();
    if (!m_glb) j << ",\"uri\":\"data:application/octet-stream;base64," << Base64(m_bin.data(), m_bin.size()) << "\"";
    j << "}]}";
    std::string json = j.str();
    std::ofstream f(fs::u8path(path), std::ios::binary);
    if (!f) { err = "impossibile scrivere " + path; return false; }
    if (!m_glb) {
      f << json;
      return (bool)f || (err = "errore di scrittura", false);
    }
    while (json.size() % 4) json += ' ';
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };  // little endian (x86/ARM)
    const uint32_t total = 12 + 8 + static_cast<uint32_t>(json.size()) + 8 + static_cast<uint32_t>(m_bin.size());
    u32(0x46546C67);  // "glTF"
    u32(2);
    u32(total);
    u32(static_cast<uint32_t>(json.size()));
    u32(0x4E4F534A);  // "JSON"
    f.write(json.data(), static_cast<std::streamsize>(json.size()));
    u32(static_cast<uint32_t>(m_bin.size()));
    u32(0x004E4942);  // "BIN"
    f.write(reinterpret_cast<const char*>(m_bin.data()), static_cast<std::streamsize>(m_bin.size()));
    return (bool)f || (err = "errore di scrittura", false);
  }

 private:
  bool m_glb;
  std::vector<unsigned char> m_bin;
  std::vector<std::string> m_views, m_accessors, m_meshes, m_nodes, m_materials, m_textures, m_images;
  std::map<const ExchangeMaterial*, int> m_matIndex;
};

// Accessore glTF letto come numeri (double), "comp" componenti per elemento.
bool ReadAccessor(const Json& doc, const std::vector<std::vector<unsigned char>>& buffers, int index,
                  std::vector<double>& out, int& comp) {
  const Json* acc = doc.Get("accessors") ? doc.Get("accessors")->At(static_cast<size_t>(index)) : nullptr;
  if (!acc) return false;
  const std::string type = acc->Str("type");
  comp = type == "SCALAR" ? 1 : type == "VEC2" ? 2 : type == "VEC3" ? 3 : type == "VEC4" ? 4 : type == "MAT4" ? 16 : 0;
  if (!comp) return false;
  const int ct = acc->Int("componentType", 5126);
  const size_t count = static_cast<size_t>(acc->Num("count", 0));
  const bool normalized = acc->Get("normalized") && acc->Get("normalized")->b;
  const int csize = ct == 5120 || ct == 5121 ? 1 : ct == 5122 || ct == 5123 ? 2 : 4;
  out.assign(count * comp, 0.0);
  if (!acc->Get("bufferView")) return true;  // tutto zero (accessori sparsi non gestiti)
  const Json* bv = doc.Get("bufferViews") ? doc.Get("bufferViews")->At(static_cast<size_t>(acc->Int("bufferView", 0))) : nullptr;
  if (!bv) return false;
  const size_t b = static_cast<size_t>(bv->Int("buffer", 0));
  if (b >= buffers.size()) return false;
  const auto& buf = buffers[b];
  const size_t base = static_cast<size_t>(bv->Num("byteOffset", 0) + acc->Num("byteOffset", 0));
  const size_t stride = bv->Num("byteStride", 0) > 0 ? static_cast<size_t>(bv->Num("byteStride", 0)) : csize * comp;
  for (size_t i = 0; i < count; i++)
    for (int k = 0; k < comp; k++) {
      const size_t at = base + i * stride + k * csize;
      if (at + csize > buf.size()) return false;
      const unsigned char* p = buf.data() + at;
      double v = 0;
      switch (ct) {
        case 5120: v = static_cast<int8_t>(p[0]); if (normalized) v = std::max(v / 127.0, -1.0); break;
        case 5121: v = p[0]; if (normalized) v /= 255.0; break;
        case 5122: { int16_t x; std::memcpy(&x, p, 2); v = x; if (normalized) v = std::max(v / 32767.0, -1.0); break; }
        case 5123: { uint16_t x; std::memcpy(&x, p, 2); v = x; if (normalized) v /= 65535.0; break; }
        case 5125: { uint32_t x; std::memcpy(&x, p, 4); v = x; break; }
        default: { float x; std::memcpy(&x, p, 4); v = x; break; }
      }
      out[i * comp + k] = v;
    }
  return true;
}

ON_Xform NodeXform(const Json& node) {
  ON_Xform x = ON_Xform::IdentityTransformation;
  if (const Json* m = node.Get("matrix"); m && m->kind == Json::Array && m->a.size() == 16) {
    for (int c = 0; c < 4; c++)
      for (int r = 0; r < 4; r++) x.m_xform[r][c] = m->a[static_cast<size_t>(c * 4 + r)].n;  // colonna per colonna
    return x;
  }
  ON_Xform t = ON_Xform::IdentityTransformation, rot = ON_Xform::IdentityTransformation, s = ON_Xform::IdentityTransformation;
  if (const Json* v = node.Get("translation"); v && v->a.size() == 3)
    t = ON_Xform::TranslationTransformation(v->a[0].n, v->a[1].n, v->a[2].n);
  if (const Json* v = node.Get("rotation"); v && v->a.size() == 4) {
    ON_Quaternion q(v->a[3].n, v->a[0].n, v->a[1].n, v->a[2].n);  // glTF: x, y, z, w
    q.Unitize();
    q.GetRotation(rot);
  }
  if (const Json* v = node.Get("scale"); v && v->a.size() == 3)
    s = ON_Xform::DiagonalTransformation(v->a[0].n, v->a[1].n, v->a[2].n);
  return t * rot * s;
}

// --- zip (per 3MF) ----------------------------------------------------------------------------
struct ZipEntry {
  std::string name;
  std::vector<unsigned char> data;
};

void Put16(std::vector<unsigned char>& b, uint16_t v) { b.push_back(v & 0xFF); b.push_back(v >> 8); }
void Put32(std::vector<unsigned char>& b, uint32_t v) {
  for (int i = 0; i < 4; i++) b.push_back(static_cast<unsigned char>((v >> (8 * i)) & 0xFF));
}
uint16_t Get16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t Get32(const unsigned char* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

bool Deflate(const std::vector<unsigned char>& in, std::vector<unsigned char>& out) {
  z_stream zs{};
  if (deflateInit2(&zs, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) return false;
  out.resize(deflateBound(&zs, static_cast<uLong>(in.size())) + 16);
  zs.next_in = const_cast<Bytef*>(in.data());
  zs.avail_in = static_cast<uInt>(in.size());
  zs.next_out = out.data();
  zs.avail_out = static_cast<uInt>(out.size());
  const int rc = deflate(&zs, Z_FINISH);
  out.resize(zs.total_out);
  deflateEnd(&zs);
  return rc == Z_STREAM_END;
}

bool Inflate(const unsigned char* in, size_t n, size_t expected, std::vector<unsigned char>& out) {
  z_stream zs{};
  if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return false;
  out.resize(expected ? expected : n * 4 + 1024);
  zs.next_in = const_cast<Bytef*>(in);
  zs.avail_in = static_cast<uInt>(n);
  int rc;
  do {
    if (zs.total_out >= out.size()) out.resize(out.size() * 2);
    zs.next_out = out.data() + zs.total_out;
    zs.avail_out = static_cast<uInt>(out.size() - zs.total_out);
    rc = inflate(&zs, Z_NO_FLUSH);
  } while (rc == Z_OK);
  out.resize(zs.total_out);
  inflateEnd(&zs);
  return rc == Z_STREAM_END;
}

bool WriteZip(const std::string& path, const std::vector<ZipEntry>& entries, std::string& err) {
  std::vector<unsigned char> out, central;
  for (const auto& e : entries) {
    std::vector<unsigned char> comp;
    const bool deflated = Deflate(e.data, comp) && comp.size() < e.data.size();
    const std::vector<unsigned char>& body = deflated ? comp : e.data;
    const uint32_t crc = static_cast<uint32_t>(crc32(0L, e.data.data(), static_cast<uInt>(e.data.size())));
    const uint32_t off = static_cast<uint32_t>(out.size());
    auto header = [&](std::vector<unsigned char>& b, bool centralDir) {
      Put32(b, centralDir ? 0x02014b50 : 0x04034b50);
      if (centralDir) Put16(b, 20);
      Put16(b, 20);                       // versione necessaria
      Put16(b, 0x0800);                   // nomi in UTF-8
      Put16(b, deflated ? 8 : 0);
      Put16(b, 0);                        // ora
      Put16(b, 0x21);                     // data (1980-01-01)
      Put32(b, crc);
      Put32(b, static_cast<uint32_t>(body.size()));
      Put32(b, static_cast<uint32_t>(e.data.size()));
      Put16(b, static_cast<uint16_t>(e.name.size()));
      Put16(b, 0);
      if (centralDir) {
        Put16(b, 0); Put16(b, 0); Put16(b, 0); Put32(b, 0);
        Put32(b, off);
      }
      b.insert(b.end(), e.name.begin(), e.name.end());
    };
    header(out, false);
    out.insert(out.end(), body.begin(), body.end());
    header(central, true);
  }
  const uint32_t cdOff = static_cast<uint32_t>(out.size());
  out.insert(out.end(), central.begin(), central.end());
  Put32(out, 0x06054b50);
  Put16(out, 0); Put16(out, 0);
  Put16(out, static_cast<uint16_t>(entries.size()));
  Put16(out, static_cast<uint16_t>(entries.size()));
  Put32(out, static_cast<uint32_t>(central.size()));
  Put32(out, cdOff);
  Put16(out, 0);
  std::ofstream f(fs::u8path(path), std::ios::binary);
  if (!f) { err = "impossibile scrivere " + path; return false; }
  f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
  return (bool)f || (err = "errore di scrittura", false);
}

bool ReadZip(const std::vector<unsigned char>& z, std::map<std::string, std::vector<unsigned char>>& files, std::string& err) {
  if (z.size() < 22) { err = "archivio non valido"; return false; }
  size_t eocd = std::string::npos;
  for (size_t i = z.size() - 22 + 1; i-- > 0 && z.size() - i < 70000;)
    if (Get32(&z[i]) == 0x06054b50) { eocd = i; break; }
  if (eocd == std::string::npos) { err = "archivio zip non valido"; return false; }
  const uint16_t n = Get16(&z[eocd + 10]);
  size_t p = Get32(&z[eocd + 16]);
  for (int k = 0; k < n; k++) {
    if (p + 46 > z.size() || Get32(&z[p]) != 0x02014b50) { err = "indice zip non valido"; return false; }
    const uint16_t method = Get16(&z[p + 10]);
    const uint32_t csize = Get32(&z[p + 20]), usize = Get32(&z[p + 24]);
    const uint16_t nl = Get16(&z[p + 28]), xl = Get16(&z[p + 30]), cl = Get16(&z[p + 32]);
    const uint32_t lo = Get32(&z[p + 42]);
    std::string name(reinterpret_cast<const char*>(&z[p + 46]), nl);
    p += 46 + nl + xl + cl;
    if (lo + 30 > z.size()) continue;
    const size_t data = lo + 30 + Get16(&z[lo + 26]) + Get16(&z[lo + 28]);
    if (data + csize > z.size()) continue;
    std::vector<unsigned char> out;
    if (method == 0) out.assign(z.begin() + static_cast<long>(data), z.begin() + static_cast<long>(data + csize));
    else if (method == 8) {
      if (!Inflate(&z[data], csize, usize, out)) continue;
    } else continue;
    files[name] = std::move(out);
  }
  return true;
}

// --- XML minimo (3MF) ----------------------------------------------------------------------
struct XmlTag {
  std::string name;  // senza prefisso dello spazio dei nomi
  std::map<std::string, std::string> attr;
  bool closing = false, selfClosing = false;
};

std::string LocalName(const std::string& n) {
  const size_t c = n.find(':');
  return c == std::string::npos ? n : n.substr(c + 1);
}

std::string XmlUnescape(std::string s) {
  const std::pair<const char*, const char*> rep[] = {{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}};
  for (auto& [a, b] : rep)
    for (size_t i = 0; (i = s.find(a, i)) != std::string::npos; i += std::strlen(b)) s.replace(i, std::strlen(a), b);
  return s;
}

std::string XmlEscape(const std::string& s) {
  std::string o;
  for (char c : s) {
    switch (c) {
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '&': o += "&amp;"; break;
      case '"': o += "&quot;"; break;
      default: o += c;
    }
  }
  return o;
}

// Visita i tag in ordine (senza testo: 3MF usa solo attributi).
void ForEachTag(const std::string& x, const std::function<void(const XmlTag&)>& f) {
  size_t i = 0;
  while ((i = x.find('<', i)) != std::string::npos) {
    if (x.compare(i, 4, "<!--") == 0) { i = x.find("-->", i); if (i == std::string::npos) return; continue; }
    const size_t e = x.find('>', i);
    if (e == std::string::npos) return;
    if (x[i + 1] == '?' || x[i + 1] == '!') { i = e + 1; continue; }
    XmlTag t;
    size_t p = i + 1;
    if (x[p] == '/') { t.closing = true; p++; }
    size_t q = p;
    while (q < e && !std::isspace(static_cast<unsigned char>(x[q])) && x[q] != '/') q++;
    t.name = LocalName(x.substr(p, q - p));
    t.selfClosing = x[e - 1] == '/';
    p = q;
    while (p < e) {
      while (p < e && (std::isspace(static_cast<unsigned char>(x[p])) || x[p] == '/')) p++;
      const size_t eq = x.find('=', p);
      if (eq == std::string::npos || eq >= e) break;
      std::string key = x.substr(p, eq - p);
      while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) key.pop_back();
      size_t v = eq + 1;
      while (v < e && std::isspace(static_cast<unsigned char>(x[v]))) v++;
      const char quote = x[v];
      if (quote != '"' && quote != '\'') break;
      const size_t ve = x.find(quote, v + 1);
      if (ve == std::string::npos) break;
      t.attr[LocalName(key)] = XmlUnescape(x.substr(v + 1, ve - v - 1));
      p = ve + 1;
    }
    f(t);
    i = e + 1;
  }
}

const char* Unit3MF(ON::LengthUnitSystem u) {
  switch (u) {
    case ON::LengthUnitSystem::Microns: return "micron";
    case ON::LengthUnitSystem::Centimeters: return "centimeter";
    case ON::LengthUnitSystem::Inches: return "inch";
    case ON::LengthUnitSystem::Feet: return "foot";
    case ON::LengthUnitSystem::Meters: return "meter";
    default: return "millimeter";
  }
}

ON::LengthUnitSystem UnitFrom3MF(const std::string& u) {
  if (u == "micron") return ON::LengthUnitSystem::Microns;
  if (u == "centimeter") return ON::LengthUnitSystem::Centimeters;
  if (u == "inch") return ON::LengthUnitSystem::Inches;
  if (u == "foot") return ON::LengthUnitSystem::Feet;
  if (u == "meter") return ON::LengthUnitSystem::Meters;
  return ON::LengthUnitSystem::Millimeters;
}

ON_Color ParseHexColor(const std::string& s) {
  if (s.size() < 7 || s[0] != '#') return ON_Color(204, 204, 204);
  auto h = [&](size_t i) { return static_cast<int>(std::strtol(s.substr(i, 2).c_str(), nullptr, 16)); };
  ON_Color c(h(1), h(3), h(5));
  if (s.size() >= 9) c.SetAlpha(255 - h(7));  // ON_Color: alpha 0 = opaco
  return c;
}

// Matrice 3MF "m00 m01 m02 m10 ... m32" (vettori riga: p' = p * M)
ON_Xform Xform3MF(const std::string& s) {
  ON_Xform x = ON_Xform::IdentityTransformation;
  std::istringstream in(s);
  double m[12];
  for (double& v : m)
    if (!(in >> v)) return x;
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 3; c++) x.m_xform[c][r] = m[r * 3 + c];
  x.m_xform[0][3] = m[9], x.m_xform[1][3] = m[10], x.m_xform[2][3] = m[11];
  return x;
}

// --- PLY -------------------------------------------------------------------------------------
struct PlyProp {
  std::string name, type, countType;  // countType non vuoto = lista
};
struct PlyElement {
  std::string name;
  size_t count = 0;
  std::vector<PlyProp> props;
};

int PlySize(const std::string& t) {
  if (t == "char" || t == "uchar" || t == "int8" || t == "uint8") return 1;
  if (t == "short" || t == "ushort" || t == "int16" || t == "uint16") return 2;
  if (t == "double" || t == "float64") return 8;
  return 4;
}

}  // namespace

// ============================================================================================
std::unique_ptr<ON_Mesh> ExportMeshFor(const ExportItem& item, double tolerance) {
  auto m = ExportMesh(*item.geom, tolerance);
  if (!m) return m;
  if (item.hasMapping) {
    ON_SimpleArray<ON_2fPoint> tc;
    ON_SimpleArray<ON_3fPoint> tc3;
    if (item.mapping.GetTextureCoordinates(*m, tc3, nullptr, false, nullptr) && tc3.Count() == m->m_V.Count()) {
      m->m_T.SetCount(0);
      for (int i = 0; i < tc3.Count(); i++) m->m_T.Append(ON_2fPoint(tc3[i].x, tc3[i].y));
    }
  }
  if (m->m_N.Count() != m->m_V.Count()) m->ComputeVertexNormals();
  return m;
}

// --- glTF ----------------------------------------------------------------------------------
bool WriteGLTF(const std::string& path, const std::vector<ExportItem>& items, ON::LengthUnitSystem docUnits,
               double tol, bool binary, std::string& err) {
  GltfWriter w(binary);
  const double s = MetersPerUnit(docUnits);
  // Z in alto (Rhino) -> Y in alto (glTF): (x, y, z) -> (x, z, -y), in metri
  ON_Xform toGltf(0.0);
  toGltf.m_xform[0][0] = s;
  toGltf.m_xform[1][2] = s;
  toGltf.m_xform[2][1] = -s;
  toGltf.m_xform[3][3] = 1.0;
  int skipped = 0;
  for (size_t i = 0; i < items.size(); i++) {
    auto m = ExportMeshFor(items[i], tol);
    if (!m || m->m_F.Count() == 0) { skipped++; continue; }
    m->Transform(toGltf);
    if (m->m_N.Count() == m->m_V.Count())
      for (int k = 0; k < m->m_N.Count(); k++) m->m_N[k].Unitize();
    const std::string name = items[i].name.empty() ? "Oggetto " + std::to_string(i + 1) : items[i].name;
    w.AddMesh(name, *m, items[i].material.get());
  }
  if (!w.Write(path, err)) return false;
  if (skipped) err = std::to_string(skipped) + " oggetti senza superfici (curve, punti, testi) non esportati";
  return true;
}

bool ReadGLTF(const std::string& path, ON::LengthUnitSystem docUnits, std::vector<ImportedObject>& out, std::string& err) {
  std::vector<unsigned char> file;
  if (!ReadFile(path, file)) { err = "impossibile aprire " + path; return false; }
  std::string jsonText;
  std::vector<unsigned char> glbBin;
  if (file.size() >= 20 && Get32(file.data()) == 0x46546C67) {
    size_t p = 12;
    while (p + 8 <= file.size()) {
      const uint32_t len = Get32(&file[p]), type = Get32(&file[p + 4]);
      if (p + 8 + len > file.size()) break;
      if (type == 0x4E4F534A) jsonText.assign(reinterpret_cast<const char*>(&file[p + 8]), len);
      else if (type == 0x004E4942) glbBin.assign(file.begin() + static_cast<long>(p + 8), file.begin() + static_cast<long>(p + 8 + len));
      p += 8 + len;
    }
  } else jsonText.assign(file.begin(), file.end());
  Json doc;
  if (jsonText.empty() || !JsonParser(jsonText).Parse(doc) || doc.kind != Json::Object) { err = "file glTF non valido"; return false; }
  if (const Json* ext = doc.Get("extensionsRequired"); ext && !ext->a.empty()) {
    std::string names;
    for (auto& e : ext->a) names += (names.empty() ? "" : ", ") + e.s;
    err = "il file richiede estensioni glTF non supportate (" + names + ")";
    return false;
  }
  const fs::path dir = fs::u8path(path).parent_path();
  auto loadUri = [&](const std::string& uri, std::vector<unsigned char>& data, std::string* mime) {
    if (uri.rfind("data:", 0) == 0) {
      const size_t comma = uri.find(',');
      if (comma == std::string::npos) return false;
      if (mime) *mime = uri.substr(5, uri.find(';') - 5);
      data = Unbase64(uri.substr(comma + 1));
      return true;
    }
    std::string file2 = uri;
    for (size_t k = 0; (k = file2.find("%20", k)) != std::string::npos;) file2.replace(k, 3, " ");
    if (mime) *mime = MimeOf(file2);
    return ReadFile(U8(dir / fs::u8path(file2)), data);
  };
  std::vector<std::vector<unsigned char>> buffers;
  if (const Json* bs = doc.Get("buffers"))
    for (size_t i = 0; i < bs->a.size(); i++) {
      std::vector<unsigned char> data;
      const std::string uri = bs->a[i].Str("uri");
      if (uri.empty()) data = glbBin;
      else if (!loadUri(uri, data, nullptr)) { err = "buffer glTF non trovato: " + uri; return false; }
      buffers.push_back(std::move(data));
    }
  // materiali
  std::vector<std::shared_ptr<ExchangeMaterial>> mats;
  if (const Json* ms = doc.Get("materials"))
    for (size_t i = 0; i < ms->a.size(); i++) {
      const Json& m = ms->a[i];
      auto em = std::make_shared<ExchangeMaterial>();
      em->name = m.Str("name").empty() ? "glTF " + std::to_string(i + 1) : m.Str("name");
      em->pbr = true;
      em->metallic = 1.0, em->roughness = 1.0;
      if (const Json* pr = m.Get("pbrMetallicRoughness")) {
        if (const Json* bc = pr->Get("baseColorFactor"); bc && bc->a.size() == 4) {
          em->diffuse = ON_Color(static_cast<int>(std::lround(std::clamp(bc->a[0].n, 0.0, 1.0) * 255)),
                                 static_cast<int>(std::lround(std::clamp(bc->a[1].n, 0.0, 1.0) * 255)),
                                 static_cast<int>(std::lround(std::clamp(bc->a[2].n, 0.0, 1.0) * 255)));
          if (m.Str("alphaMode") == "BLEND") em->transparency = 1.0 - std::clamp(bc->a[3].n, 0.0, 1.0);
        } else em->diffuse = ON_Color(255, 255, 255);
        em->metallic = pr->Num("metallicFactor", 1.0);
        em->roughness = pr->Num("roughnessFactor", 1.0);
        if (const Json* tex = pr->Get("baseColorTexture")) {
          const Json* t = doc.Get("textures") ? doc.Get("textures")->At(static_cast<size_t>(tex->Int("index", 0))) : nullptr;
          const Json* img = t && doc.Get("images") ? doc.Get("images")->At(static_cast<size_t>(t->Int("source", 0))) : nullptr;
          if (img) {
            std::vector<unsigned char> data;
            std::string mime = img->Str("mimeType");
            if (!img->Str("uri").empty() && img->Str("uri").rfind("data:", 0) != 0) {
              em->texture = U8(fs::absolute(dir / fs::u8path(img->Str("uri"))).lexically_normal());
            } else if (!img->Str("uri").empty()) {
              loadUri(img->Str("uri"), data, &mime);
            } else if (img->Get("bufferView")) {
              const Json* bv = doc.Get("bufferViews")->At(static_cast<size_t>(img->Int("bufferView", 0)));
              if (bv && static_cast<size_t>(bv->Int("buffer", 0)) < buffers.size()) {
                const auto& b = buffers[static_cast<size_t>(bv->Int("buffer", 0))];
                const size_t off = static_cast<size_t>(bv->Num("byteOffset", 0)), len = static_cast<size_t>(bv->Num("byteLength", 0));
                if (off + len <= b.size()) data.assign(b.begin() + static_cast<long>(off), b.begin() + static_cast<long>(off + len));
              }
            }
            if (!data.empty()) {
              em->textureData = std::move(data);
              em->textureMime = mime.empty() ? "image/png" : mime;
              std::string nm = img->Str("name").empty() ? em->name : img->Str("name");
              for (auto& c : nm) if (c == '/' || c == '\\' || c == ':') c = '_';
              em->texture = U8(fs::absolute(dir).lexically_normal() / fs::u8path(U8(fs::u8path(path).stem()) + "_" + nm + ExtOfMime(em->textureMime)));
            }
          }
        }
      }
      if (const Json* e = m.Get("emissiveFactor"); e && e->a.size() == 3)
        em->emission = ON_Color(static_cast<int>(e->a[0].n * 255), static_cast<int>(e->a[1].n * 255), static_cast<int>(e->a[2].n * 255));
      mats.push_back(em);
    }
  // Y in alto e metri -> Z in alto e unita' del documento
  const double s = 1.0 / MetersPerUnit(docUnits);
  ON_Xform fromGltf(0.0);
  fromGltf.m_xform[0][0] = s;
  fromGltf.m_xform[1][2] = -s;
  fromGltf.m_xform[2][1] = s;
  fromGltf.m_xform[3][3] = 1.0;
  const Json* nodes = doc.Get("nodes");
  const Json* meshes = doc.Get("meshes");
  int unsupported = 0;
  std::function<void(int, const ON_Xform&, int)> visit = [&](int ni, const ON_Xform& parent, int depth) {
    const Json* node = nodes ? nodes->At(static_cast<size_t>(ni)) : nullptr;
    if (!node || depth > 64) return;
    const ON_Xform xf = parent * NodeXform(*node);
    if (const Json* mi = node->Get("mesh"); mi && meshes) {
      const Json* mesh = meshes->At(static_cast<size_t>(mi->n));
      const Json* prims = mesh ? mesh->Get("primitives") : nullptr;
      std::string name = node->Str("name").empty() ? (mesh ? mesh->Str("name") : "") : node->Str("name");
      for (size_t p = 0; prims && p < prims->a.size(); p++) {
        const Json& pr = prims->a[p];
        const int mode = pr.Int("mode", 4);
        const Json* at = pr.Get("attributes");
        if ((mode != 4 && mode != 5 && mode != 6) || !at || !at->Get("POSITION")) { unsupported++; continue; }
        std::vector<double> pos, nor, uv, col, idx;
        int c = 0, cn = 0, cu = 0, cc = 0, ci = 0;
        if (!ReadAccessor(doc, buffers, static_cast<int>(at->Get("POSITION")->n), pos, c) || c != 3) { unsupported++; continue; }
        const size_t nv = pos.size() / 3;
        if (at->Get("NORMAL")) ReadAccessor(doc, buffers, static_cast<int>(at->Get("NORMAL")->n), nor, cn);
        if (at->Get("TEXCOORD_0")) ReadAccessor(doc, buffers, static_cast<int>(at->Get("TEXCOORD_0")->n), uv, cu);
        if (at->Get("COLOR_0")) ReadAccessor(doc, buffers, static_cast<int>(at->Get("COLOR_0")->n), col, cc);
        std::vector<int> ix;
        if (pr.Get("indices") && ReadAccessor(doc, buffers, static_cast<int>(pr.Get("indices")->n), idx, ci))
          for (double v : idx) ix.push_back(static_cast<int>(v));
        else
          for (size_t k = 0; k < nv; k++) ix.push_back(static_cast<int>(k));
        std::vector<std::array<int, 3>> tri;
        if (mode == 4)
          for (size_t k = 0; k + 2 < ix.size(); k += 3) tri.push_back({ix[k], ix[k + 1], ix[k + 2]});
        else if (mode == 5)
          for (size_t k = 0; k + 2 < ix.size(); k++)
            tri.push_back(k % 2 ? std::array<int, 3>{ix[k + 1], ix[k], ix[k + 2]} : std::array<int, 3>{ix[k], ix[k + 1], ix[k + 2]});
        else
          for (size_t k = 1; k + 1 < ix.size(); k++) tri.push_back({ix[0], ix[k], ix[k + 1]});
        std::vector<ON_3dPoint> V(nv);
        for (size_t k = 0; k < nv; k++) V[k] = ON_3dPoint(pos[k * 3], pos[k * 3 + 1], pos[k * 3 + 2]);
        auto m = NewMesh(V, tri);
        if (cn == 3 && nor.size() == nv * 3)
          for (size_t k = 0; k < nv; k++) m->m_N.Append(ON_3fVector((float)nor[k * 3], (float)nor[k * 3 + 1], (float)nor[k * 3 + 2]));
        if (cu == 2 && uv.size() == nv * 2)
          for (size_t k = 0; k < nv; k++) m->m_T.Append(ON_2fPoint((float)uv[k * 2], 1.0f - (float)uv[k * 2 + 1]));
        if ((cc == 3 || cc == 4) && col.size() == nv * cc)
          for (size_t k = 0; k < nv; k++)
            m->m_C.Append(ON_Color(static_cast<int>(std::lround(std::clamp(col[k * cc], 0.0, 1.0) * 255)),
                                   static_cast<int>(std::lround(std::clamp(col[k * cc + 1], 0.0, 1.0) * 255)),
                                   static_cast<int>(std::lround(std::clamp(col[k * cc + 2], 0.0, 1.0) * 255))));
        m->Transform(fromGltf * xf);
        if (m->m_N.Count() != m->m_V.Count()) { m->m_N.Destroy(); m->ComputeVertexNormals(); }
        if (m->m_F.Count() == 0) continue;
        ImportedObject o;
        o.geom = std::move(m);
        o.name = name;
        const int mat = pr.Int("material", -1);
        if (mat >= 0 && mat < (int)mats.size()) o.material = mats[static_cast<size_t>(mat)];
        out.push_back(std::move(o));
      }
    }
    if (const Json* ch = node->Get("children"))
      for (const auto& c : ch->a) visit(static_cast<int>(c.n), xf, depth + 1);
  };
  const Json* scenes = doc.Get("scenes");
  const Json* scene = scenes ? scenes->At(static_cast<size_t>(doc.Int("scene", 0))) : nullptr;
  if (scene && scene->Get("nodes")) {
    for (const auto& n : scene->Get("nodes")->a) visit(static_cast<int>(n.n), ON_Xform::IdentityTransformation, 0);
  } else if (nodes) {
    // senza scena: tutti i nodi che non sono figli di altri
    std::vector<bool> child(nodes->a.size(), false);
    for (const auto& n : nodes->a)
      if (const Json* ch = n.Get("children"))
        for (const auto& c : ch->a) if (c.n >= 0 && c.n < child.size()) child[static_cast<size_t>(c.n)] = true;
    for (size_t i = 0; i < nodes->a.size(); i++)
      if (!child[i]) visit(static_cast<int>(i), ON_Xform::IdentityTransformation, 0);
  }
  if (unsupported) err = std::to_string(unsupported) + " primitive non supportate (punti, linee o dati compressi) saltate";
  if (out.empty()) { if (err.empty()) err = "nessuna mesh nel file glTF"; return false; }
  return true;
}

// --- 3MF ------------------------------------------------------------------------------------
bool Write3MF(const std::string& path, const std::vector<ExportItem>& items, ON::LengthUnitSystem docUnits,
              double tol, std::string& err) {
  std::ostringstream x;
  x.precision(9);
  x << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    << "<model unit=\"" << Unit3MF(docUnits) << "\" xml:lang=\"it-IT\" "
    << "xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\">\n"
    << " <metadata name=\"Application\">Kurva3D</metadata>\n <resources>\n";
  // colori: un materiale base per materiale (o colore di visualizzazione)
  std::vector<std::pair<std::string, ON_Color>> bases;
  std::vector<int> baseOf(items.size(), -1);
  for (size_t i = 0; i < items.size(); i++) {
    const std::string nm = items[i].material ? items[i].material->name : "Colore " + std::to_string(i + 1);
    ON_Color c = items[i].material ? items[i].material->diffuse : items[i].color;
    if (items[i].material) c.SetAlpha(static_cast<int>(std::lround(std::clamp(items[i].material->transparency, 0.0, 1.0) * 255)));
    int found = -1;
    for (size_t k = 0; k < bases.size(); k++)
      if (bases[k].first == nm && bases[k].second == c) found = static_cast<int>(k);
    if (found < 0) { bases.emplace_back(nm, c); found = static_cast<int>(bases.size()) - 1; }
    baseOf[i] = found;
  }
  if (!bases.empty()) {
    x << "  <basematerials id=\"1\">\n";
    for (auto& [nm, c] : bases) {
      char hex[16];
      std::snprintf(hex, sizeof hex, "#%02X%02X%02X%02X", c.Red(), c.Green(), c.Blue(), 255 - c.Alpha());
      x << "   <base name=\"" << XmlEscape(nm) << "\" displaycolor=\"" << hex << "\"/>\n";
    }
    x << "  </basematerials>\n";
  }
  std::ostringstream build;
  int id = 2, n = 0, skipped = 0;
  for (size_t i = 0; i < items.size(); i++) {
    auto m = ExportMeshFor(items[i], tol);
    if (!m || m->m_F.Count() == 0) { skipped++; continue; }
    const std::string name = items[i].name.empty() ? "Oggetto " + std::to_string(i + 1) : items[i].name;
    x << "  <object id=\"" << id << "\" type=\"model\" name=\"" << XmlEscape(name) << "\" pid=\"1\" pindex=\"" << baseOf[i]
      << "\">\n   <mesh>\n    <vertices>\n";
    for (int k = 0; k < m->m_V.Count(); k++)
      x << "     <vertex x=\"" << m->m_V[k].x << "\" y=\"" << m->m_V[k].y << "\" z=\"" << m->m_V[k].z << "\"/>\n";
    x << "    </vertices>\n    <triangles>\n";
    for (const auto& t : Triangles(*m))
      if (t[0] != t[1] && t[1] != t[2] && t[0] != t[2])
        x << "     <triangle v1=\"" << t[0] << "\" v2=\"" << t[1] << "\" v3=\"" << t[2] << "\"/>\n";
    x << "    </triangles>\n   </mesh>\n  </object>\n";
    build << "  <item objectid=\"" << id << "\"/>\n";
    id++;
    n++;
  }
  if (!n) { err = "nessun oggetto esportabile in 3MF (servono superfici, solidi, mesh o SubD)"; return false; }
  x << " </resources>\n <build>\n" << build.str() << " </build>\n</model>\n";
  auto bytes = [](const std::string& s) { return std::vector<unsigned char>(s.begin(), s.end()); };
  std::vector<ZipEntry> z = {
      {"[Content_Types].xml",
       bytes("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
             "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
             "<Default Extension=\"model\" ContentType=\"application/vnd.ms-package.3dmanufacturing-3dmodel+xml\"/></Types>\n")},
      {"_rels/.rels",
       bytes("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
             "<Relationship Target=\"/3D/3dmodel.model\" Id=\"rel0\" Type=\"http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel\"/>"
             "</Relationships>\n")},
      {"3D/3dmodel.model", bytes(x.str())}};
  if (!WriteZip(path, z, err)) return false;
  if (skipped) err = std::to_string(skipped) + " oggetti senza superfici (curve, punti, testi) non esportati";
  return true;
}

bool Read3MF(const std::string& path, ON::LengthUnitSystem docUnits, std::vector<ImportedObject>& out, std::string& err) {
  std::vector<unsigned char> file;
  if (!ReadFile(path, file)) { err = "impossibile aprire " + path; return false; }
  std::map<std::string, std::vector<unsigned char>> files;
  if (!ReadZip(file, files, err)) return false;
  std::string modelPath = "3D/3dmodel.model";
  if (auto r = files.find("_rels/.rels"); r != files.end()) {
    ForEachTag(std::string(r->second.begin(), r->second.end()), [&](const XmlTag& t) {
      if (t.name == "Relationship" && t.attr.count("Type") && t.attr.at("Type").find("3dmodel") != std::string::npos) {
        std::string target = t.attr.count("Target") ? t.attr.at("Target") : "";
        if (!target.empty() && target[0] == '/') target.erase(0, 1);
        if (!target.empty()) modelPath = target;
      }
    });
  }
  auto mf = files.find(modelPath);
  if (mf == files.end()) { err = "modello 3D non trovato nel file 3MF"; return false; }
  const std::string xml(mf->second.begin(), mf->second.end());
  struct Obj {
    std::string name;
    std::vector<ON_3dPoint> v;
    std::vector<std::array<int, 3>> t;
    std::vector<std::pair<std::string, ON_Xform>> components;  // id oggetto, trasformazione
    std::string pid;
    int pindex = 0;
  };
  std::map<std::string, Obj> objects;
  std::map<std::string, std::vector<std::pair<std::string, ON_Color>>> materials;  // id gruppo -> basi
  std::vector<std::pair<std::string, ON_Xform>> build;
  ON::LengthUnitSystem units = ON::LengthUnitSystem::Millimeters;
  Obj* cur = nullptr;
  std::string curGroup;
  ForEachTag(xml, [&](const XmlTag& t) {
    auto a = [&](const char* k) { auto f = t.attr.find(k); return f == t.attr.end() ? std::string() : f->second; };
    if (t.closing) {
      if (t.name == "object") cur = nullptr;
      if (t.name == "basematerials" || t.name == "colorgroup") curGroup.clear();
      return;
    }
    if (t.name == "model") units = UnitFrom3MF(a("unit"));
    else if (t.name == "basematerials" || t.name == "colorgroup") curGroup = a("id");
    else if ((t.name == "base" || t.name == "color") && !curGroup.empty())
      materials[curGroup].emplace_back(a("name"), ParseHexColor(t.name == "base" ? a("displaycolor") : a("color")));
    else if (t.name == "object") {
      cur = &objects[a("id")];
      cur->name = a("name");
      cur->pid = a("pid");
      cur->pindex = std::atoi(a("pindex").c_str());
      if (t.selfClosing) cur = nullptr;
    } else if (t.name == "vertex" && cur)
      cur->v.emplace_back(std::atof(a("x").c_str()), std::atof(a("y").c_str()), std::atof(a("z").c_str()));
    else if (t.name == "triangle" && cur)
      cur->t.push_back({std::atoi(a("v1").c_str()), std::atoi(a("v2").c_str()), std::atoi(a("v3").c_str())});
    else if (t.name == "component" && cur)
      cur->components.emplace_back(a("objectid"), Xform3MF(a("transform")));
    else if (t.name == "item")
      build.emplace_back(a("objectid"), Xform3MF(a("transform")));
  });
  const double scale = ON::UnitScale(units, docUnits);
  const ON_Xform toDoc = ON_Xform::DiagonalTransformation(scale, scale, scale);
  std::map<std::string, std::shared_ptr<ExchangeMaterial>> matCache;
  std::function<void(const std::string&, const ON_Xform&, const std::string&, int)> emit =
      [&](const std::string& id, const ON_Xform& xf, const std::string& inheritedName, int depth) {
        auto f = objects.find(id);
        if (f == objects.end() || depth > 32) return;
        const Obj& o = f->second;
        const std::string name = o.name.empty() ? inheritedName : o.name;
        if (!o.v.empty() && !o.t.empty()) {
          auto m = NewMesh(o.v, o.t);
          m->Transform(toDoc * xf);
          m->ComputeVertexNormals();
          ImportedObject io;
          io.geom = std::move(m);
          io.name = name;
          if (auto g = materials.find(o.pid); g != materials.end() && o.pindex >= 0 && o.pindex < (int)g->second.size()) {
            const std::string key = o.pid + "/" + std::to_string(o.pindex);
            auto& em = matCache[key];
            if (!em) {
              em = std::make_shared<ExchangeMaterial>();
              const auto& [bn, col] = g->second[static_cast<size_t>(o.pindex)];
              em->name = bn.empty() ? "3MF " + key : bn;
              em->diffuse = ON_Color(col.Red(), col.Green(), col.Blue());
              em->transparency = col.Alpha() / 255.0;
            }
            io.material = em;
          }
          out.push_back(std::move(io));
        }
        for (const auto& [cid, cx] : o.components) emit(cid, xf * cx, name, depth + 1);
      };
  for (const auto& [id, xf] : build) emit(id, xf, "", 0);
  if (out.empty()) { err = "nessuna mesh nel file 3MF"; return false; }
  return true;
}

// --- PLY ------------------------------------------------------------------------------------
bool WritePLY(const std::string& path, const std::vector<ExportItem>& items, double tol, bool binary, std::string& err) {
  // un solo oggetto per file (come il formato): mesh unite, oppure nuvola di punti
  ON_Mesh all;
  std::vector<ON_3dPoint> points;
  std::vector<ON_Color> pointColors;
  bool colors = false;
  for (const auto& it : items) {
    if (const auto* pc = ON_PointCloud::Cast(it.geom)) {
      for (int k = 0; k < pc->m_P.Count(); k++) {
        points.push_back(pc->m_P[k]);
        pointColors.push_back(pc->m_C.Count() == pc->m_P.Count() ? pc->m_C[k] : it.color);
      }
      colors = colors || pc->m_C.Count() == pc->m_P.Count();
      continue;
    }
    if (const auto* pt = ON_Point::Cast(it.geom)) { points.push_back(pt->point); pointColors.push_back(it.color); continue; }
    auto m = ExportMeshFor(it, tol);
    if (!m || m->m_F.Count() == 0) continue;
    if (m->m_C.Count() == m->m_V.Count()) colors = true;
    else {
      // colore dell'oggetto (o del materiale) su tutti i vertici
      const ON_Color c = it.material ? it.material->diffuse : it.color;
      m->m_C.SetCount(0);
      for (int k = 0; k < m->m_V.Count(); k++) m->m_C.Append(c);
    }
    all.Append(*m);
  }
  const bool cloud = all.m_F.Count() == 0;
  if (cloud && points.empty()) { err = "nessun oggetto esportabile in PLY"; return false; }
  const int nv = cloud ? static_cast<int>(points.size()) : all.m_V.Count();
  const bool hasN = !cloud && all.m_N.Count() == nv;
  const bool hasC = cloud ? colors : all.m_C.Count() == nv;
  const bool hasT = !cloud && all.m_T.Count() == nv;
  std::ofstream f(fs::u8path(path), std::ios::binary);
  if (!f) { err = "impossibile scrivere " + path; return false; }
  f << "ply\nformat " << (binary ? "binary_little_endian" : "ascii") << " 1.0\ncomment Kurva3D\n"
    << "element vertex " << nv << "\nproperty float x\nproperty float y\nproperty float z\n";
  if (hasN) f << "property float nx\nproperty float ny\nproperty float nz\n";
  if (hasT) f << "property float s\nproperty float t\n";
  if (hasC) f << "property uchar red\nproperty uchar green\nproperty uchar blue\n";
  std::vector<std::array<int, 3>> tri;
  if (!cloud) {
    tri = Triangles(all);
    f << "element face " << tri.size() << "\nproperty list uchar int vertex_indices\n";
  }
  f << "end_header\n";
  auto wf = [&](float v) { f.write(reinterpret_cast<const char*>(&v), 4); };
  for (int i = 0; i < nv; i++) {
    const ON_3dPoint p = cloud ? points[static_cast<size_t>(i)] : ON_3dPoint(all.m_V[i]);
    const ON_Color c = cloud ? pointColors[static_cast<size_t>(i)] : (hasC ? all.m_C[i] : ON_Color::Black);
    if (binary) {
      wf((float)p.x), wf((float)p.y), wf((float)p.z);
      if (hasN) wf(all.m_N[i].x), wf(all.m_N[i].y), wf(all.m_N[i].z);
      if (hasT) wf(all.m_T[i].x), wf(all.m_T[i].y);
      if (hasC) {
        const unsigned char rgb[3] = {(unsigned char)c.Red(), (unsigned char)c.Green(), (unsigned char)c.Blue()};
        f.write(reinterpret_cast<const char*>(rgb), 3);
      }
    } else {
      f << Fmt(p.x) << " " << Fmt(p.y) << " " << Fmt(p.z);
      if (hasN) f << " " << Fmt(all.m_N[i].x) << " " << Fmt(all.m_N[i].y) << " " << Fmt(all.m_N[i].z);
      if (hasT) f << " " << Fmt(all.m_T[i].x) << " " << Fmt(all.m_T[i].y);
      if (hasC) f << " " << c.Red() << " " << c.Green() << " " << c.Blue();
      f << "\n";
    }
  }
  for (const auto& t : tri) {
    if (binary) {
      const unsigned char three = 3;
      f.write(reinterpret_cast<const char*>(&three), 1);
      for (int k = 0; k < 3; k++) { const int32_t v = t[static_cast<size_t>(k)]; f.write(reinterpret_cast<const char*>(&v), 4); }
    } else f << "3 " << t[0] << " " << t[1] << " " << t[2] << "\n";
  }
  return (bool)f || (err = "errore di scrittura", false);
}

bool ReadPLY(const std::string& path, std::vector<ImportedObject>& out, std::string& err) {
  std::vector<unsigned char> data;
  if (!ReadFile(path, data)) { err = "impossibile aprire " + path; return false; }
  const std::string head(data.begin(), data.begin() + static_cast<long>(std::min<size_t>(data.size(), 65536)));
  const size_t endh = head.find("end_header");
  if (head.compare(0, 3, "ply") != 0 || endh == std::string::npos) { err = "file PLY non valido"; return false; }
  size_t body = head.find('\n', endh);
  if (body == std::string::npos) { err = "file PLY non valido"; return false; }
  body++;
  std::istringstream hs(head.substr(0, endh));
  std::string format;
  std::vector<PlyElement> elems;
  for (std::string line; std::getline(hs, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream ls(line);
    std::string w;
    ls >> w;
    if (w == "format") ls >> format;
    else if (w == "element") { PlyElement e; ls >> e.name >> e.count; elems.push_back(e); }
    else if (w == "property" && !elems.empty()) {
      PlyProp p;
      std::string t;
      ls >> t;
      if (t == "list") ls >> p.countType >> p.type >> p.name;
      else { p.type = t; ls >> p.name; }
      elems.back().props.push_back(p);
    }
  }
  const bool ascii = format == "ascii", big = format == "binary_big_endian";
  if (!ascii && format != "binary_little_endian" && !big) { err = "formato PLY non supportato: " + format; return false; }
  size_t pos = body;
  std::istringstream as(ascii ? std::string(data.begin() + static_cast<long>(body), data.end()) : std::string());
  bool bad = false;
  auto readValue = [&](const std::string& type) -> double {
    if (ascii) { double v = 0; if (!(as >> v)) bad = true; return v; }
    const int sz = PlySize(type);
    if (pos + sz > data.size()) { bad = true; return 0; }
    unsigned char b[8];
    std::memcpy(b, &data[pos], sz);
    pos += sz;
    if (big) std::reverse(b, b + sz);
    if (type == "char" || type == "int8") return static_cast<int8_t>(b[0]);
    if (type == "uchar" || type == "uint8") return b[0];
    if (type == "short" || type == "int16") { int16_t v; std::memcpy(&v, b, 2); return v; }
    if (type == "ushort" || type == "uint16") { uint16_t v; std::memcpy(&v, b, 2); return v; }
    if (type == "int" || type == "int32") { int32_t v; std::memcpy(&v, b, 4); return v; }
    if (type == "uint" || type == "uint32") { uint32_t v; std::memcpy(&v, b, 4); return v; }
    if (type == "double" || type == "float64") { double v; std::memcpy(&v, b, 8); return v; }
    float v;
    std::memcpy(&v, b, 4);
    return v;
  };
  std::vector<ON_3dPoint> V;
  std::vector<ON_3fVector> N;
  std::vector<ON_2fPoint> T;
  std::vector<ON_Color> C;
  std::vector<std::vector<int>> faces;
  for (const auto& e : elems) {
    for (size_t i = 0; i < e.count && !bad; i++) {
      double x = 0, y = 0, z = 0, nx = 0, ny = 0, nz = 0, r = -1, g = -1, b = -1, s = -1, t = -1;
      bool hasN = false, colorFloat = false;
      std::vector<int> face;
      for (const auto& p : e.props) {
        if (!p.countType.empty()) {
          const int n = static_cast<int>(readValue(p.countType));
          for (int k = 0; k < n && !bad; k++) {
            const double v = readValue(p.type);
            if (p.name == "vertex_indices" || p.name == "vertex_index") face.push_back(static_cast<int>(v));
          }
          continue;
        }
        const double v = readValue(p.type);
        if (e.name != "vertex") continue;
        if (p.name == "x") x = v; else if (p.name == "y") y = v; else if (p.name == "z") z = v;
        else if (p.name == "nx") nx = v, hasN = true; else if (p.name == "ny") ny = v; else if (p.name == "nz") nz = v;
        else if (p.name == "red" || p.name == "r") { r = v; colorFloat = p.type == "float" || p.type == "double"; }
        else if (p.name == "green" || p.name == "g") g = v;
        else if (p.name == "blue" || p.name == "b") b = v;
        else if (p.name == "s" || p.name == "u" || p.name == "texture_u") s = v;
        else if (p.name == "t" || p.name == "v" || p.name == "texture_v") t = v;
      }
      if (e.name == "vertex") {
        V.emplace_back(x, y, z);
        if (hasN) N.emplace_back((float)nx, (float)ny, (float)nz);
        if (r >= 0 && g >= 0 && b >= 0) {
          const double k = colorFloat ? 255.0 : 1.0;
          C.emplace_back(static_cast<int>(std::clamp(r * k, 0.0, 255.0)), static_cast<int>(std::clamp(g * k, 0.0, 255.0)),
                         static_cast<int>(std::clamp(b * k, 0.0, 255.0)));
        }
        if (s >= 0 && t >= 0) T.emplace_back((float)s, (float)t);
      } else if (e.name == "face" && face.size() >= 3) faces.push_back(std::move(face));
    }
  }
  if (bad) { err = "file PLY incompleto o danneggiato"; return false; }
  if (V.empty()) { err = "nessun vertice nel file PLY"; return false; }
  ImportedObject o;
  o.name = U8(fs::u8path(path).stem());
  if (faces.empty()) {
    auto pc = std::make_unique<ON_PointCloud>();
    for (const auto& p : V) pc->m_P.Append(p);
    if (C.size() == V.size()) for (const auto& c : C) pc->m_C.Append(c);
    if (N.size() == V.size()) for (const auto& n : N) pc->m_N.Append(ON_3dVector(n));
    o.geom = std::move(pc);
  } else {
    auto m = std::make_unique<ON_Mesh>(static_cast<int>(faces.size()), static_cast<int>(V.size()), false, false);
    for (const auto& p : V) m->m_V.Append(ON_3fPoint(p));
    const int nv = static_cast<int>(V.size());
    for (const auto& f : faces) {
      bool ok = true;
      for (int k : f) ok = ok && k >= 0 && k < nv;
      if (!ok) continue;
      if (f.size() == 3) m->SetTriangle(m->m_F.Count(), f[0], f[1], f[2]);
      else if (f.size() == 4) m->SetQuad(m->m_F.Count(), f[0], f[1], f[2], f[3]);
      else for (size_t k = 1; k + 1 < f.size(); k++) m->SetTriangle(m->m_F.Count(), f[0], f[k], f[k + 1]);
    }
    if (N.size() == V.size()) for (const auto& n : N) m->m_N.Append(n);
    else m->ComputeVertexNormals();
    if (C.size() == V.size()) for (const auto& c : C) m->m_C.Append(c);
    if (T.size() == V.size()) for (const auto& t : T) m->m_T.Append(t);
    o.geom = std::move(m);
  }
  out.push_back(std::move(o));
  return true;
}

}  // namespace cad
