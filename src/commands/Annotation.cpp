#include "commands/Annotation.h"
#include "core/Document3dm.h"
#include <algorithm>
#include <cmath>

namespace cad {
namespace {

// Freccia chiusa con la punta in tip, rivolta lungo dir (dal corpo verso la punta).
void Arrow(AnnotationDisplay& out, const ON_3dPoint& tip, ON_3dVector dir, const ON_3dVector& normal, double size) {
  if (!dir.Unitize() || size <= 0) return;
  ON_3dVector side = ON_CrossProduct(normal, dir);
  if (!side.Unitize()) return;
  const ON_3dPoint base = tip - dir * size;
  const ON_3dPoint a = base + side * (size / 6.0), b = base - side * (size / 6.0);
  out.wires.push_back({tip, a, b, tip});
  out.triangles.insert(out.triangles.end(), {tip, a, b});
}

void Line(AnnotationDisplay& out, const ON_3dPoint& a, const ON_3dPoint& b) {
  if (a.IsValid() && b.IsValid() && a.DistanceTo(b) > ON_ZERO_TOLERANCE) out.wires.push_back({a, b});
}

void ArcWire(AnnotationDisplay& out, const ON_Arc& arc) {
  if (!arc.IsValid()) return;
  const int n = std::max(8, int(std::ceil(arc.AngleRadians() / (ON_PI / 48))));
  std::vector<ON_3dPoint> w;
  for (int i = 0; i <= n; i++) w.push_back(arc.PointAt(arc.Domain().ParameterAt(double(i) / n)));
  out.wires.push_back(std::move(w));
}

std::string Text(const ON_Annotation& a) {
  std::string s = ToUtf8(a.PlainText());
  s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
  return s;
}

int LineCount(const std::string& s) { return 1 + int(std::count(s.begin(), s.end(), '\n')); }

// Etichetta di un testo (ON_Text o il testo di una direttrice): punto di
// allineamento dal piano del testo, allineamento dalle impostazioni dell'oggetto.
void TextLabel(AnnotationDisplay& out, const ON_Annotation& a, const ON_DimStyle& style, double h) {
  AnnotationLabel L;
  L.text = Text(a);
  if (L.text.empty()) return;
  ON_Xform xf;
  if (!a.GetTextXform(nullptr, &style, a.DimScale(&style), xf)) return;
  L.xdir = xf * ON_3dVector::XAxis;
  L.ydir = xf * ON_3dVector::YAxis;
  const double sx = L.xdir.Length();
  if (!L.xdir.Unitize() || !L.ydir.Unitize()) return;
  L.height = h;
  ON_2dPoint c[4];
  double xmin = 0, xmax = 0, ymax = 0;
  if (a.Text() && a.Text()->Get2dCorners(c)) {
    xmin = std::min({c[0].x, c[1].x, c[2].x, c[3].x});
    xmax = std::max({c[0].x, c[1].x, c[2].x, c[3].x});
    ymax = std::max({c[0].y, c[1].y, c[2].y, c[3].y});
  }
  double ax = xmin;
  double top = ymax * sx;
  if (xmax - xmin > 1e-9) {  // i caratteri sono stati misurati: centriamo nel riquadro
    ax = 0.5 * (xmin + xmax);
    L.halign = 1;
  } else {
    // caratteri non misurati: il bordo superiore della prima riga dipende dall'allineamento verticale
    ON::TextHorizontalAlignment ha; ON::TextVerticalAlignment va;
    a.GetAlignment(ha, va);
    L.halign = ha == ON::TextHorizontalAlignment::Center ? 1 : ha == ON::TextHorizontalAlignment::Right ? 2 : 0;
    const double block = (LineCount(L.text) - 1) * 1.6 * h;  // righe sotto la prima
    if (va == ON::TextVerticalAlignment::Bottom || va == ON::TextVerticalAlignment::BottomOfBoundingBox ||
        va == ON::TextVerticalAlignment::BottomOfTop)
      top = h + block;  // l'origine e' sulla linea di base dell'ultima riga
    else if (va == ON::TextVerticalAlignment::Middle || va == ON::TextVerticalAlignment::MiddleOfTop)
      top = 0.5 * (h + block);
    else top = 0;  // in alto
  }
  // top e' il bordo superiore del riquadro; la prima linea di base sta un'altezza sotto
  L.origin = xf * ON_3dPoint(ax, 0, 0) + L.ydir * (top - h);
  out.labels.push_back(L);
}

}  // namespace

const ON_DimStyle& ParentDimStyle(const ONX_Model& model, const ON_Annotation& a) {
  const auto* ds = ON_DimStyle::Cast(
      model.ComponentFromId(ON_ModelComponent::Type::DimStyle, a.DimensionStyleId()).ModelComponent());
  if (ds) return *ds;
  const auto* def = ON_DimStyle::Cast(model.DefaultDimensionStyle().ModelComponent());
  return def ? *def : ON_DimStyle::Default;
}

bool BuildAnnotationDisplay(const ONX_Model& model, const ON_Annotation& a, AnnotationDisplay& out) {
  const ON_DimStyle& style = a.DimensionStyle(ParentDimStyle(model, a));
  const double scale = a.DimScale(&style) > 0 ? a.DimScale(&style) : 1.0;
  const double h = a.TextHeight(&style) * scale;
  const double arrow = a.ArrowSize(&style) * scale;
  const double gap = style.TextGap() * scale;
  const ON_3dVector n = a.Plane().zaxis;

  // Testo della quota lungo la direzione dir, girato per essere leggibile
  // (da sinistra a destra o dal basso in alto nel piano dell'annotazione).
  auto dimLabel = [&](const ON_3dPoint& textPt, ON_3dVector dir, const std::string& text) {
    AnnotationLabel L;
    L.text = text;
    const ON_Plane& pl = a.Plane();
    if (!dir.Unitize()) dir = pl.xaxis;
    const double dx = dir * pl.xaxis, dy = dir * pl.yaxis;
    if (dx < -1e-9 || (std::fabs(dx) <= 1e-9 && dy < 0)) dir = -dir;
    L.xdir = dir;
    L.ydir = ON_CrossProduct(n, dir);
    if (!L.ydir.Unitize()) return;
    L.height = h;
    L.halign = 1;
    // testo sopra la linea di quota, staccato di TextGap
    L.origin = textPt + L.ydir * (gap + (LineCount(text) - 1) * 1.6 * h);
    out.labels.push_back(L);
  };
  auto dimText = [&](const ON_Dimension& d) {
    if (const auto* an = ON_DimAngular::Cast(&d)) an->UpdateDimensionText(&style);
    else d.UpdateDimensionText(model.m_settings.m_ModelUnitsAndTolerances.m_unit_system.UnitSystem(), &style);
    std::string s = Text(d);
    return s;
  };

  if (const auto* t = ON_Text::Cast(&a)) {
    TextLabel(out, *t, style, h);
  } else if (const auto* ld = ON_Leader::Cast(&a)) {
    if (const ON_NurbsCurve* c = ld->Curve(&style)) {
      std::vector<ON_3dPoint> w;
      const int k = 64;
      for (int i = 0; i <= k; i++) w.push_back(c->PointAt(c->Domain().ParameterAt(double(i) / k)));
      out.wires.push_back(std::move(w));
      if (ld->PointCount() > 1) {
        ON_3dPoint p0, p1;
        ld->Point3d(0, p0);
        ld->Point3d(1, p1);
        Arrow(out, p0, p0 - p1, n, a.LeaderArrowSize(&style) * scale);
      }
    }
    TextLabel(out, *ld, style, h);
  } else if (const auto* lin = ON_DimLinear::Cast(&a)) {
    ON_3dPoint d1, d2, a1, a2, dl, tp;
    if (!lin->Get3dPoints(&d1, &d2, &a1, &a2, &dl, &tp)) return false;
    const double eo = style.ExtOffset() * scale, ee = style.ExtExtension() * scale;
    auto ext = [&](const ON_3dPoint& def, const ON_3dPoint& arr) {
      ON_3dVector v = arr - def;
      if (v.Length() < ON_ZERO_TOLERANCE) return;
      v.Unitize();
      Line(out, def + v * eo, arr + v * ee);
    };
    ext(d1, a1);
    ext(d2, a2);
    Line(out, a1, a2);
    Arrow(out, a1, a1 - a2, n, arrow);
    Arrow(out, a2, a2 - a1, n, arrow);
    dimLabel(0.5 * (a1 + a2), a2 - a1, dimText(*lin));
  } else if (const auto* rad = ON_DimRadial::Cast(&a)) {
    ON_3dPoint c, r, dl, kn;
    if (!rad->Get3dPoints(&c, &r, &dl, &kn)) return false;
    const bool diam = rad->Type() == ON::AnnotationType::Diameter;
    if (diam) Line(out, c - (r - c), r);
    Line(out, r, kn);
    Line(out, kn, dl);
    Arrow(out, r, r - kn, n, arrow);
    if (diam) Arrow(out, c - (r - c), c - r, n, arrow);
    // segno del centro
    const double cm = style.CenterMark() * scale;
    if (cm > 0) {
      Line(out, c - a.Plane().xaxis * cm, c + a.Plane().xaxis * cm);
      Line(out, c - a.Plane().yaxis * cm, c + a.Plane().yaxis * cm);
    }
    // testo dopo la piega, sulla linea orizzontale del piano
    AnnotationLabel L;
    L.text = dimText(*rad);
    L.xdir = a.Plane().xaxis;
    L.ydir = a.Plane().yaxis;
    L.height = h;
    L.halign = (dl - kn) * L.xdir >= 0 ? 0 : 2;
    L.origin = dl + L.xdir * ((L.halign == 0 ? 1 : -1) * gap) - L.ydir * (0.5 * h);
    out.labels.push_back(L);
  } else if (const auto* ang = ON_DimAngular::Cast(&a)) {
    ON_3dPoint c, d1, d2, a1, a2, dl, tp;
    if (!ang->Get3dPoints(&c, &d1, &d2, &a1, &a2, &dl, &tp)) return false;
    ON_Arc arc;
    if (arc.Create(a1, dl, a2)) {
      ArcWire(out, arc);
      ON_3dVector t0 = arc.TangentAt(arc.Domain()[0]), t1 = arc.TangentAt(arc.Domain()[1]);
      Arrow(out, a1, -t0, n, arrow);
      Arrow(out, a2, t1, n, arrow);
    }
    const double ee = style.ExtExtension() * scale;
    auto ext = [&](const ON_3dPoint& def, const ON_3dPoint& arr) {
      ON_3dVector v = arr - c;
      if (!v.Unitize()) return;
      if (def.DistanceTo(c) < arr.DistanceTo(c)) Line(out, def, arr + v * ee);
    };
    ext(d1, a1);
    ext(d2, a2);
    {
      const ON_3dPoint at = tp.IsValid() ? tp : dl;
      dimLabel(at, ON_CrossProduct(n, at - c), dimText(*ang));
    }
  } else if (const auto* od = ON_DimOrdinate::Cast(&a)) {
    ON_3dPoint bp, dp, lp, k1, k2;
    if (!od->Get3dPoints(&bp, &dp, &lp, &k1, &k2, 2.0 * h)) return false;
    // direttrice spezzata dal punto quotato (staccata di ExtOffset) alla fine
    ON_3dVector v = k1 - dp;
    const double eo = style.ExtOffset() * scale;
    const ON_3dPoint start = v.Unitize() && dp.DistanceTo(k1) > eo ? dp + v * eo : dp;
    out.wires.push_back({start, k1, k2, lp});
    // testo in fondo alla direttrice, allineato con l'ultimo tratto
    AnnotationLabel L;
    L.text = dimText(*od);
    ON_3dVector dir = lp - k2;
    if (!dir.Unitize()) dir = lp - dp;
    if (!dir.Unitize()) dir = a.Plane().xaxis;
    const bool flip = (dir * a.Plane().xaxis < -1e-9) || (std::fabs(dir * a.Plane().xaxis) <= 1e-9 && dir * a.Plane().yaxis < 0);
    L.xdir = flip ? -dir : dir;
    L.ydir = ON_CrossProduct(n, L.xdir);
    if (L.ydir.Unitize()) {
      L.height = h;
      L.halign = flip ? 2 : 0;
      L.origin = lp + dir * gap - L.ydir * (0.5 * h);
      out.labels.push_back(L);
    }
  } else if (const auto* cm = ON_Centermark::Cast(&a)) {
    ON_Line lines[6];
    bool isl[6] = {};
    if (cm->GetDisplayLines(&style, scale, lines, isl, 6))
      for (int i = 0; i < 6; i++) if (isl[i]) Line(out, lines[i].from, lines[i].to);
  } else {
    return false;
  }
  // carattere dello stile con le sostituzioni dell'annotazione (Font del pannello Proprieta')
  const ON_Font& f = a.Font(&style);
  ON_wString fam = f.FamilyName();
  if (fam.IsEmpty()) fam = f.QuartetName();
  const std::string family = fam.IsEmpty() ? std::string() : std::string(ON_String(fam).Array());
  for (auto& L : out.labels) {
    if (L.dot) continue;
    L.font = family;
    L.bold = f.IsBold();
    L.italic = f.IsItalic();
  }
  return true;
}

}  // namespace cad
