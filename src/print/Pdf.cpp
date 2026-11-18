// Scrittura PDF 1.4 minima: linee, poligoni pieni, testo Helvetica e immagini RGB compresse.
#include "print/Page.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "zlib/zlib.h"  // zlib di openNURBS (nomi con prefisso z_)

namespace cad {

namespace {

// Larghezze di Helvetica (1/1000 em) per i caratteri 32..126
const short kHelv[95] = {278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278, 556, 556, 556, 556,
                         556, 556, 556, 556, 556, 556, 278, 278, 584, 584, 584, 556, 1015, 667, 667, 722, 722, 667, 611, 778,
                         722, 278, 500, 667, 556, 833, 722, 778, 667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 278,
                         278, 278, 469, 556, 333, 556, 556, 500, 556, 556, 278, 556, 556, 222, 222, 500, 222, 833, 556, 556,
                         556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500, 334, 260, 334, 584};

// UTF-8 -> WinAnsi (Latin-1 per le lettere accentate); il resto diventa '?'
std::string ToWinAnsi(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size();) {
    unsigned c = (unsigned char)s[i];
    unsigned cp = c;
    int n = 1;
    if (c >= 0xF0) { cp = c & 7; n = 4; }
    else if (c >= 0xE0) { cp = c & 15; n = 3; }
    else if (c >= 0xC0) { cp = c & 31; n = 2; }
    for (int k = 1; k < n && i + k < s.size(); k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 63);
    i += n;
    if (cp == 0x20AC) out += (char)0x80;          // euro
    else if (cp == 0x2019) out += (char)0x92;     // apostrofo tipografico
    else if (cp == 0x2013) out += (char)0x96;
    else if (cp == 0x2014) out += (char)0x97;
    else if (cp < 256 && cp >= 32) out += (char)cp;
    else out += '?';
  }
  return out;
}

double TextWidth(const std::string& winAnsi, double size) {
  double w = 0;
  for (unsigned char c : winAnsi) w += (c >= 32 && c <= 126) ? kHelv[c - 32] : 556;
  return w / 1000.0 * size;
}

std::string Escape(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '(' || c == ')' || c == '\\') o += '\\';
    o += c;
  }
  return o;
}

std::string Deflate(const std::string& in) {
  uLongf n = compressBound((uLong)in.size());
  std::string out(n, '\0');
  if (compress2(reinterpret_cast<Bytef*>(&out[0]), &n, reinterpret_cast<const Bytef*>(in.data()), (uLong)in.size(), 6) != Z_OK)
    return {};
  out.resize(n);
  return out;
}

const double kPt = 72.0 / 25.4;  // punti per millimetro

std::string Num(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%.3f", std::fabs(v) < 5e-4 ? 0.0 : v);
  return b;
}

std::string Rgb(const ON_Color& c, const char* op) {
  return Num(c.FractionRed()) + " " + Num(c.FractionGreen()) + " " + Num(c.FractionBlue()) + " " + op + "\n";
}

}  // namespace

bool WritePdf(const std::string& path, const std::vector<PageDrawing>& pages, std::string& err) {
  if (pages.empty()) { err = "nessuna pagina da stampare"; return false; }
  std::vector<std::string> objs;  // corpo degli oggetti, numerati da 1
  auto add = [&](std::string body) { objs.push_back(std::move(body)); return (int)objs.size(); };
  const int catalog = add(""), pagesObj = add(""), font = add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");
  std::vector<int> kids;
  for (const auto& pg : pages) {
    std::ostringstream c;
    std::string xobjs;
    int imgN = 0;
    // immagini (viste ombreggiate)
    for (const auto& im : pg.images) {
      if (im.pw <= 0 || im.ph <= 0 || (int)im.rgb.size() != im.pw * im.ph * 3) continue;
      const std::string data = Deflate(std::string(im.rgb.begin(), im.rgb.end()));
      const int id = add("<< /Type /XObject /Subtype /Image /Width " + std::to_string(im.pw) + " /Height " +
                         std::to_string(im.ph) + " /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /FlateDecode /Length " +
                         std::to_string(data.size()) + " >>\nstream\n" + data + "\nendstream");
      const std::string name = "Im" + std::to_string(++imgN);
      xobjs += "/" + name + " " + std::to_string(id) + " 0 R ";
      c << "q " << Num(im.w * kPt) << " 0 0 " << Num(im.h * kPt) << " " << Num(im.x * kPt) << " " << Num(im.y * kPt)
        << " cm /" << name << " Do Q\n";
    }
    c << "1 J 1 j\n";
    for (const auto& f : pg.fills) {
      if (f.pts.size() < 3) continue;
      c << Rgb(f.color, "rg");
      for (size_t i = 0; i < f.pts.size(); i++)
        c << Num(f.pts[i].x * kPt) << " " << Num(f.pts[i].y * kPt) << (i ? " l\n" : " m\n");
      c << "h f\n";
    }
    ON_Color lastColor = ON_UNSET_COLOR;
    double lastW = -1;
    for (const auto& l : pg.lines) {
      if (l.pts.size() < 2) continue;
      if (l.color != lastColor) { c << Rgb(l.color, "RG"); lastColor = l.color; }
      if (l.widthMm != lastW) { c << Num(std::max(0.0, l.widthMm) * kPt) << " w\n"; lastW = l.widthMm; }
      for (size_t i = 0; i < l.pts.size(); i++)
        c << Num(l.pts[i].x * kPt) << " " << Num(l.pts[i].y * kPt) << (i ? " l\n" : " m\n");
      c << "S\n";
    }
    for (const auto& t : pg.texts) {
      const std::string s = ToWinAnsi(t.text);
      const double size = t.heightMm / 0.718 * kPt;  // altezza delle maiuscole di Helvetica = 0.718 em
      const double w = TextWidth(s, size) * (t.halign == 1 ? 0.5 : t.halign == 2 ? 1.0 : 0.0);
      const double ca = std::cos(t.angle), sa = std::sin(t.angle);
      const double x = t.pos.x * kPt - w * ca, y = t.pos.y * kPt - w * sa;
      c << Rgb(t.color, "rg") << "BT /F1 " << Num(size) << " Tf " << Num(ca) << " " << Num(sa) << " " << Num(-sa) << " "
        << Num(ca) << " " << Num(x) << " " << Num(y) << " Tm (" << Escape(s) << ") Tj ET\n";
    }
    const std::string content = Deflate(c.str());
    const int cs = add("<< /Length " + std::to_string(content.size()) + " /Filter /FlateDecode >>\nstream\n" + content + "\nendstream");
    const int pg_ = add("<< /Type /Page /Parent " + std::to_string(pagesObj) + " 0 R /MediaBox [0 0 " + Num(pg.widthMm * kPt) +
                        " " + Num(pg.heightMm * kPt) + "] /Resources << /Font << /F1 " + std::to_string(font) +
                        " 0 R >> /XObject << " + xobjs + ">> >> /Contents " + std::to_string(cs) + " 0 R >>");
    kids.push_back(pg_);
  }
  objs[catalog - 1] = "<< /Type /Catalog /Pages " + std::to_string(pagesObj) + " 0 R >>";
  std::string k;
  for (int id : kids) k += std::to_string(id) + " 0 R ";
  objs[pagesObj - 1] = "<< /Type /Pages /Kids [" + k + "] /Count " + std::to_string(kids.size()) + " >>";
  const int info = add("<< /Producer (Kurva3D) /Title (" + Escape(ToWinAnsi(pages[0].name)) + ") >>");

  std::string out = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
  std::vector<size_t> offs;
  for (size_t i = 0; i < objs.size(); i++) {
    offs.push_back(out.size());
    out += std::to_string(i + 1) + " 0 obj\n" + objs[i] + "\nendobj\n";
  }
  const size_t xref = out.size();
  out += "xref\n0 " + std::to_string(objs.size() + 1) + "\n0000000000 65535 f \n";
  for (size_t o : offs) {
    char b[24];
    std::snprintf(b, sizeof b, "%010zu 00000 n \n", o);
    out += b;
  }
  out += "trailer\n<< /Size " + std::to_string(objs.size() + 1) + " /Root " + std::to_string(catalog) + " 0 R /Info " +
         std::to_string(info) + " 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
  std::ofstream f(std::filesystem::u8path(path), std::ios::binary);
  if (!f) { err = "impossibile scrivere " + path; return false; }
  f.write(out.data(), (std::streamsize)out.size());
  return (bool)f;
}

}  // namespace cad
