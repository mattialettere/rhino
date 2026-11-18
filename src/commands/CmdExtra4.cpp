// Comandi di Rhino 8, gruppo 4: linee tangenti e perpendicolari a una curva
// (Tangent, Perpendicular) e semplificazione delle mesh (Reduce).
#include "commands/Engine.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <vector>

namespace cad {
namespace {

const ON_Geometry* Geo(Engine& e, const ON_UUID& id) {
  const auto* o = e.Object(id);
  return o ? o->Geometry(nullptr) : nullptr;
}

double OptNum(const std::map<std::string, std::string>& o, const char* k, double def) {
  auto it = o.find(k);
  if (it == o.end()) return def;
  try { return std::stod(it->second); } catch (...) { return def; }
}

// Curva piu' vicina al punto e parametro del punto piu' vicino su di essa.
const ON_Curve* PickCurve(Engine& e, const ON_3dPoint& p, double& tOut) {
  double best = 1e300;
  const ON_Curve* out = nullptr;
  for (const auto& id : e.AllObjectIds(false)) {
    const auto* c = ON_Curve::Cast(Geo(e, id));
    if (!c) continue;
    const ON_Interval d = c->Domain();
    for (int i = 0; i <= 1000; i++) {
      const double t = d.ParameterAt(i / 1000.0);
      const double dd = c->PointAt(t).DistanceTo(p);
      if (dd < best) { best = dd; out = c; tOut = t; }
    }
  }
  if (!out) return nullptr;
  const ON_Interval d = out->Domain();
  double a = std::max(d[0], tOut - d.Length() / 1000), b = std::min(d[1], tOut + d.Length() / 1000);
  for (int k = 0; k < 60; k++) {
    const double m1 = a + (b - a) * 0.381966, m2 = a + (b - a) * 0.618034;
    if (out->PointAt(m1).DistanceTo(p) < out->PointAt(m2).DistanceTo(p)) b = m2; else a = m1;
  }
  tOut = 0.5 * (a + b);
  return out;
}

bool AddLine(Engine& e, const ON_3dPoint& a, const ON_3dPoint& b, const char* what) {
  if (a.DistanceTo(b) <= e.Tolerance()) return e.Fail("linea di lunghezza nulla");
  e.AddObject(new ON_LineCurve(a, b));
  e.Out() << "  Creata: " << what << "\n";
  return true;
}

// Tangent: linea tangente alla curva nel punto cliccato; la fine si proietta sulla tangente.
bool CmdTangent(Engine& e, TokenStream& ts) {
  auto p = e.ReadPick(ts, "punto sulla curva");
  if (!p) return false;
  double t = 0;
  const ON_Curve* c = PickCurve(e, *p, t);
  if (!c) return e.Fail("nessuna curva");
  const ON_3dPoint P = c->PointAt(t);
  const ON_3dVector T = c->TangentAt(t);
  auto q = e.ReadPoint(ts, "fine della linea");
  if (!q) return false;
  return AddLine(e, P, P + T * ((*q - P) * T), "linea tangente");
}

// Perpendicular: linea perpendicolare alla curva nel punto cliccato, verso la fine.
bool CmdPerpendicular(Engine& e, TokenStream& ts) {
  auto p = e.ReadPick(ts, "punto sulla curva");
  if (!p) return false;
  double t = 0;
  const ON_Curve* c = PickCurve(e, *p, t);
  if (!c) return e.Fail("nessuna curva");
  const ON_3dPoint P = c->PointAt(t);
  const ON_3dVector T = c->TangentAt(t);
  auto q = e.ReadPoint(ts, "fine della linea");
  if (!q) return false;
  const ON_3dVector v = *q - P;
  return AddLine(e, P, P + (v - T * (v * T)), "linea perpendicolare");
}

// --- Reduce: semplificazione per contrazione degli spigoli (errore quadratico) ---
struct Quadric {
  double a[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};  // xx xy xz xw yy yz yw zz zw ww
  void AddPlane(double nx, double ny, double nz, double d, double w) {
    a[0] += w * nx * nx; a[1] += w * nx * ny; a[2] += w * nx * nz; a[3] += w * nx * d;
    a[4] += w * ny * ny; a[5] += w * ny * nz; a[6] += w * ny * d;
    a[7] += w * nz * nz; a[8] += w * nz * d; a[9] += w * d * d;
  }
  Quadric operator+(const Quadric& o) const { Quadric q; for (int i = 0; i < 10; i++) q.a[i] = a[i] + o.a[i]; return q; }
  double Eval(const ON_3dPoint& p) const {
    const double x = p.x, y = p.y, z = p.z;
    return a[0] * x * x + 2 * a[1] * x * y + 2 * a[2] * x * z + 2 * a[3] * x + a[4] * y * y + 2 * a[5] * y * z +
           2 * a[6] * y + a[7] * z * z + 2 * a[8] * z + a[9];
  }
  bool Optimum(ON_3dPoint& p) const {
    const double m[3][3] = {{a[0], a[1], a[2]}, {a[1], a[4], a[5]}, {a[2], a[5], a[7]}};
    const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                       m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    const double scale = std::fabs(m[0][0]) + std::fabs(m[1][1]) + std::fabs(m[2][2]);
    if (std::fabs(det) < 1e-9 * scale * scale * scale || scale == 0) return false;
    const double b[3] = {-a[3], -a[6], -a[8]};
    auto D = [&](int col) {
      double mm[3][3];
      for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) mm[i][j] = j == col ? b[i] : m[i][j];
      return mm[0][0] * (mm[1][1] * mm[2][2] - mm[1][2] * mm[2][1]) - mm[0][1] * (mm[1][0] * mm[2][2] - mm[1][2] * mm[2][0]) +
             mm[0][2] * (mm[1][0] * mm[2][1] - mm[1][1] * mm[2][0]);
    };
    p = ON_3dPoint(D(0) / det, D(1) / det, D(2) / det);
    return true;
  }
};

ON_3dVector TriNormal(const ON_3dPoint& a, const ON_3dPoint& b, const ON_3dPoint& c) {
  return ON_CrossProduct(b - a, c - a);
}

// Riduce la mesh a circa target triangoli. Restituisce la nuova mesh.
ON_Mesh* ReduceMesh(const ON_Mesh& in, int target) {
  ON_Mesh m(in);
  m.m_N.Destroy();
  m.m_FN.Destroy();
  m.m_T.Destroy();
  m.m_C.Destroy();
  m.m_S.Destroy();
  m.CombineIdenticalVertices(true, true);
  m.ConvertQuadsToTriangles();
  const int nv = m.VertexCount();
  std::vector<ON_3dPoint> P(nv);
  for (int i = 0; i < nv; i++) P[i] = m.Vertex(i);
  std::vector<std::array<int, 3>> F;
  for (int i = 0; i < m.FaceCount(); i++) {
    const ON_MeshFace& f = m.m_F[i];
    if (f.vi[0] == f.vi[1] || f.vi[1] == f.vi[2] || f.vi[0] == f.vi[2]) continue;
    F.push_back({f.vi[0], f.vi[1], f.vi[2]});
  }
  std::vector<bool> alive(F.size(), true);
  std::vector<std::set<int>> vf(nv);
  for (int fi = 0; fi < (int)F.size(); fi++) for (int k = 0; k < 3; k++) vf[F[fi][k]].insert(fi);
  // quadriche dei piani delle facce; i bordi aperti con piani perpendicolari pesanti
  std::vector<Quadric> Q(nv);
  std::map<std::pair<int, int>, int> edgeUse;
  for (const auto& f : F) {
    ON_3dVector n = TriNormal(P[f[0]], P[f[1]], P[f[2]]);
    const double area = 0.5 * n.Length();
    if (!n.Unitize()) continue;
    const double d = -(n * ON_3dVector(P[f[0]]));
    for (int k = 0; k < 3; k++) Q[f[k]].AddPlane(n.x, n.y, n.z, d, area);
    for (int k = 0; k < 3; k++) {
      const int a = f[k], b = f[(k + 1) % 3];
      edgeUse[{std::min(a, b), std::max(a, b)}]++;
    }
  }
  for (const auto& [e, cnt] : edgeUse) {
    if (cnt != 1) continue;
    for (int fi : vf[e.first]) {
      if (!vf[e.second].count(fi)) continue;
      ON_3dVector n = TriNormal(P[F[fi][0]], P[F[fi][1]], P[F[fi][2]]);
      ON_3dVector ed = P[e.second] - P[e.first];
      const double len2 = ed.LengthSquared();
      ON_3dVector bn = ON_CrossProduct(ed, n);
      if (!bn.Unitize()) continue;
      const double d = -(bn * ON_3dVector(P[e.first]));
      for (int v : {e.first, e.second}) Q[v].AddPlane(bn.x, bn.y, bn.z, d, 1000.0 * len2);
    }
  }
  std::vector<int> version(nv, 0);
  struct Cand { double cost; int a, b, va, vb; ON_3dPoint p; };
  auto cmp = [](const Cand& x, const Cand& y) { return x.cost > y.cost; };
  std::priority_queue<Cand, std::vector<Cand>, decltype(cmp)> pq(cmp);
  auto evaluate = [&](int a, int b) {
    const Quadric q = Q[a] + Q[b];
    ON_3dPoint best = 0.5 * (P[a] + P[b]);
    double cost = q.Eval(best);
    ON_3dPoint opt;
    if (q.Optimum(opt) && opt.DistanceTo(best) < 2 * P[a].DistanceTo(P[b])) {
      const double c = q.Eval(opt);
      if (c < cost) { cost = c; best = opt; }
    }
    for (const ON_3dPoint& cand : {P[a], P[b]}) {
      const double c = q.Eval(cand);
      if (c < cost) { cost = c; best = cand; }
    }
    pq.push({cost, a, b, version[a], version[b], best});
  };
  for (const auto& [e, cnt] : edgeUse) evaluate(e.first, e.second);
  int faces = (int)F.size();
  while (faces > target && !pq.empty()) {
    const Cand c = pq.top();
    pq.pop();
    if (version[c.a] != c.va || version[c.b] != c.vb) continue;
    // condizione di collegamento: i vicini comuni devono essere quelli delle facce dello spigolo
    std::set<int> na, nb;
    for (int fi : vf[c.a]) for (int k = 0; k < 3; k++) na.insert(F[fi][k]);
    for (int fi : vf[c.b]) for (int k = 0; k < 3; k++) nb.insert(F[fi][k]);
    int shared = 0, common = 0;
    for (int fi : vf[c.a]) if (vf[c.b].count(fi)) shared++;
    if (shared == 0) continue;
    for (int v : na) if (v != c.a && v != c.b && nb.count(v)) common++;
    if (common != shared) continue;
    // nessuna faccia deve capovolgersi
    bool flip = false;
    for (int v : {c.a, c.b}) {
      for (int fi : vf[v]) {
        if (vf[c.a].count(fi) && vf[c.b].count(fi)) continue;
        ON_3dPoint q[3];
        for (int k = 0; k < 3; k++) q[k] = (F[fi][k] == c.a || F[fi][k] == c.b) ? c.p : P[F[fi][k]];
        ON_3dVector n0 = TriNormal(P[F[fi][0]], P[F[fi][1]], P[F[fi][2]]), n1 = TriNormal(q[0], q[1], q[2]);
        if (!n0.Unitize() || !n1.Unitize() || n0 * n1 < 0.2) { flip = true; break; }
      }
      if (flip) break;
    }
    if (flip) continue;
    // contrazione: b confluisce in a
    for (int fi : std::vector<int>(vf[c.b].begin(), vf[c.b].end())) {
      if (vf[c.a].count(fi)) {
        alive[fi] = false;
        faces--;
        for (int k = 0; k < 3; k++) vf[F[fi][k]].erase(fi);
        continue;
      }
      for (int k = 0; k < 3; k++) if (F[fi][k] == c.b) F[fi][k] = c.a;
      vf[c.a].insert(fi);
    }
    vf[c.b].clear();
    P[c.a] = c.p;
    Q[c.a] = Q[c.a] + Q[c.b];
    version[c.a]++;
    version[c.b]++;
    std::set<int> nbrs;
    for (int fi : vf[c.a]) for (int k = 0; k < 3; k++) if (F[fi][k] != c.a) nbrs.insert(F[fi][k]);
    for (int v : nbrs) evaluate(c.a, v);
  }
  auto* out = new ON_Mesh();
  std::vector<int> remap(nv, -1);
  for (size_t fi = 0; fi < F.size(); fi++) {
    if (!alive[fi]) continue;
    int idx[3];
    for (int k = 0; k < 3; k++) {
      int& r = remap[F[fi][k]];
      if (r < 0) { r = out->m_V.Count(); out->m_V.Append(ON_3fPoint(P[F[fi][k]])); }
      idx[k] = r;
    }
    out->SetTriangle(out->m_F.Count(), idx[0], idx[1], idx[2]);
  }
  out->ComputeVertexNormals();
  out->Compact();
  return out;
}

// Reduce: ReductionPercent= (predefinito 50) oppure FaceCount=.
bool CmdReduce(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("Reduce", "seleziona le mesh da ridurre, poi Invio");
  if (sel.empty()) return false;
  opts.merge(e.ReadOptions(ts));
  int n = 0;
  for (const auto& id : sel) {
    const auto* m = ON_Mesh::Cast(Geo(e, id));
    if (!m) continue;
    int tri = m->TriangleCount() + 2 * m->QuadCount();
    int target = opts.count("facecount") ? int(OptNum(opts, "facecount", tri)) : int(tri * (1.0 - OptNum(opts, "reductionpercent", 50) / 100.0));
    target = std::max(4, target);
    if (target >= tri) continue;
    ON_Mesh* r = ReduceMesh(*m, target);
    if (!r || r->FaceCount() == 0) { delete r; continue; }
    e.Out() << "  Mesh ridotta da " << tri << " a " << r->FaceCount() << " triangoli\n";
    e.ReplaceObject(id, r);
    n++;
  }
  return n > 0 || e.Fail("nessuna mesh ridotta");
}

// --- SimplifyCrv -------------------------------------------------------------------------
// I tratti che sono linee o archi diventano linee e archi veri; le linee
// allineate si uniscono.
void SplitPieces(const ON_Curve& c, std::vector<std::unique_ptr<ON_Curve>>& out) {
  if (const auto* pc = ON_PolyCurve::Cast(&c)) {
    for (int i = 0; i < pc->Count(); i++) SplitPieces(*pc->SegmentCurve(i), out);
    return;
  }
  std::vector<double> br{c.Domain()[0]};
  double t = c.Domain()[0];
  int hint = 0;
  while (c.GetNextDiscontinuity(ON::continuity::C2_locus_continuous, t, c.Domain()[1], &t, &hint))
    if (t > br.back()) br.push_back(t);
  if (br.back() < c.Domain()[1]) br.push_back(c.Domain()[1]);
  for (size_t i = 0; i + 1 < br.size(); i++) {
    std::unique_ptr<ON_Curve> piece(c.DuplicateCurve());
    if (br.size() > 2 && !piece->Trim(ON_Interval(br[i], br[i + 1]))) continue;
    out.push_back(std::move(piece));
  }
}

bool CmdSimplifyCrv(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("SimplifyCrv", "seleziona le curve da semplificare, poi Invio");
  if (sel.empty()) return false;
  const double tol = e.Tolerance();
  int changed = 0;
  for (const auto& id : sel) {
    const auto* c = ON_Curve::Cast(Geo(e, id));
    if (!c || ON_LineCurve::Cast(c) || ON_ArcCurve::Cast(c)) continue;
    std::vector<std::unique_ptr<ON_Curve>> pieces;
    SplitPieces(*c, pieces);
    std::vector<std::unique_ptr<ON_Curve>> simple;
    int converted = 0;
    for (auto& p : pieces) {
      ON_Arc arc;
      if (!ON_LineCurve::Cast(p.get()) && p->IsLinear(tol)) {
        auto* l = new ON_LineCurve(p->PointAtStart(), p->PointAtEnd());
        // linee allineate consecutive: una sola
        if (!simple.empty()) {
          if (auto* prev = ON_LineCurve::Cast(simple.back().get())) {
            ON_3dVector a = prev->m_line.Direction(), b = l->m_line.Direction();
            if (a.Unitize() && b.Unitize() && a * b > 1 - 1e-9) { prev->m_line.to = l->m_line.to; prev->SetDomain(0, prev->m_line.Length()); delete l; converted++; continue; }
          }
        }
        simple.emplace_back(l);
        converted++;
      } else if (!ON_ArcCurve::Cast(p.get()) && p->IsArc(nullptr, &arc, tol)) {
        simple.emplace_back(new ON_ArcCurve(arc));
        converted++;
      } else {
        simple.push_back(std::move(p));
      }
    }
    if (!converted) continue;
    ON_Curve* res = nullptr;
    if (simple.size() == 1) {
      res = simple[0].release();
    } else {
      auto* pc = new ON_PolyCurve();
      for (auto& s : simple) pc->AppendAndMatch(s.release());
      res = pc;
    }
    e.ReplaceObject(id, res);
    changed++;
  }
  e.Out() << "  Semplificate " << changed << " curve\n";
  return true;
}

// --- Match (MatchCrv) ------------------------------------------------------------------
// Porta l'estremo di una curva su quello di un'altra con continuita' di
// posizione, tangenza o curvatura (Continuity=Position/Tangency/Curvature).
bool CmdMatch(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto p0 = e.ReadPick(ts, "curva da modificare (vicino all'estremo)");
  if (!p0) return false;
  double t0 = 0;
  const ON_Curve* c0 = PickCurve(e, *p0, t0);
  if (!c0) return e.Fail("curva non trovata");
  ON_UUID id0 = ON_nil_uuid;
  for (const auto& id : e.AllObjectIds(false)) if (Geo(e, id) == c0) id0 = id;
  opts.merge(e.ReadOptions(ts));
  auto p1 = e.ReadPick(ts, "curva di destinazione (vicino all'estremo)");
  if (!p1) return false;
  // la destinazione e' la curva piu' vicina diversa dalla prima
  const ON_Curve* c1 = nullptr;
  double best = 1e300;
  for (const auto& id : e.AllObjectIds(false)) {
    const auto* c = ON_Curve::Cast(Geo(e, id));
    if (!c || c == c0) continue;
    for (int i = 0; i <= 400; i++) {
      const double d = c->PointAt(c->Domain().ParameterAt(i / 400.0)).DistanceTo(*p1);
      if (d < best) { best = d; c1 = c; }
    }
  }
  if (!c1) return e.Fail("curva di destinazione non trovata");
  std::string cont = opts.count("continuity") ? opts["continuity"] : "tangency";
  const int level = cont == "position" ? 0 : cont == "curvature" ? 2 : 1;
  ON_NurbsCurve nc;
  if (!c0->GetNurbForm(nc)) return e.Fail("curva non convertibile");
  const bool atStart = p0->DistanceTo(nc.PointAtStart()) < p0->DistanceTo(nc.PointAtEnd());
  if (atStart) nc.Reverse();
  if (nc.Degree() < 3 && level > 0) nc.IncreaseDegree(3);
  while (nc.CVCount() < 2 * (level + 1)) {  // abbastanza punti per non toccare l'altro estremo
    double maxLen = 0, at = 0;
    for (int i = nc.Order() - 2; i < nc.CVCount() - 1; i++) {
      const double a = nc.m_knot[i], b = nc.m_knot[i + 1];
      if (b - a > maxLen) { maxLen = b - a; at = 0.5 * (a + b); }
    }
    if (maxLen <= 0 || !nc.InsertKnot(at, 1)) break;
  }
  if (nc.CVCount() < level + 2) return e.Fail("la curva ha troppo pochi punti di controllo");
  const bool tStart = p1->DistanceTo(c1->PointAtStart()) < p1->DistanceTo(c1->PointAtEnd());
  const double tt = tStart ? c1->Domain()[0] : c1->Domain()[1];
  const ON_3dPoint P = c1->PointAt(tt);
  ON_3dVector T = c1->TangentAt(tt);
  if (!tStart) T = -T;  // la curva modificata arriva in P e prosegue lungo la destinazione
  const int n = nc.CVCount();
  auto setCV = [&](int i, const ON_3dPoint& q) {
    if (nc.IsRational()) { const double w = nc.Weight(i); nc.SetCV(i, ON_4dPoint(q.x * w, q.y * w, q.z * w, w)); }
    else nc.SetCV(i, q);
  };
  auto getCV = [&](int i) { ON_3dPoint q; nc.GetCV(i, q); return q; };
  const double h1 = getCV(n - 1).DistanceTo(getCV(n - 2));
  const double h2 = getCV(n - 2).DistanceTo(getCV(n - 3 >= 0 ? n - 3 : 0));
  setCV(n - 1, P);
  if (level >= 1) setCV(n - 2, P - T * h1);
  if (level >= 2 && n >= 3) {
    const ON_3dVector K = c1->CurvatureAt(tt);
    const ON_3dPoint base = getCV(n - 2) - T * h2;
    ON_3dVector N = K;
    if (!N.Unitize()) {
      setCV(n - 3, base);  // destinazione diritta: curvatura nulla
    } else {
      const double tEnd = nc.Domain()[1];
      setCV(n - 3, base);
      const ON_3dVector k0 = nc.CurvatureAt(tEnd);
      setCV(n - 3, base + N);
      const ON_3dVector k1 = nc.CurvatureAt(tEnd);
      const double den = (k1 - k0) * N;
      const double s = std::fabs(den) > 1e-15 ? (K * N - k0 * N) / den : 0.0;
      setCV(n - 3, base + N * s);
    }
  }
  if (atStart) nc.Reverse();
  e.ReplaceObject(id0, new ON_NurbsCurve(nc));
  e.Out() << "  Curva raccordata (" << (level == 0 ? "posizione" : level == 1 ? "tangenza" : "curvatura") << ")\n";
  return true;
}

}  // namespace

void RegisterExtra4Commands(Engine& e) {
  auto reg = [&](const char* n, const char* cat, const char* d, CommandFn f) {
    CommandInfo i{n, cat, d, f};
    i.modifiesModel = true;
    e.Register(i);
  };
  reg("Tangent", "Curve", "Linea tangente a una curva nel punto cliccato", CmdTangent);
  reg("Perpendicular", "Curve", "Linea perpendicolare a una curva nel punto cliccato", CmdPerpendicular);
  reg("SimplifyCrv", "Curve", "Sostituisce i tratti rettilinei e circolari con linee e archi veri", CmdSimplifyCrv);
  reg("Simplify", "Curve", "Come SimplifyCrv", CmdSimplifyCrv);
  reg("Match", "Curve", "Raccorda l'estremo di una curva a un'altra (Continuity=Position/Tangency/Curvature)", CmdMatch);
  reg("Reduce", "Mesh", "Riduce il numero di facce delle mesh (ReductionPercent=, FaceCount=)", CmdReduce);
}

}  // namespace cad
