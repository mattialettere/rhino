// Comandi di Rhino 8, gruppo 6 (solo openNURBS): HandleCurve; SubD: Reflect,
// SubDSweep, SubDExtrude, Bridge, InsertEdge, Stitch, Append; EditPtOn;
// DimStyle; Heightfield; TextObject.
#include "commands/Deform.h"
#include "commands/Engine.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace cad {
double CurveLengthOf(const ON_Curve& c);
double ParamAtLengthOf(const ON_Curve& c, double s);

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

std::string Unquote(std::string n) {
  if (n.size() >= 2 && n.front() == '"' && n.back() == '"') n = n.substr(1, n.size() - 2);
  return n;
}

// --- HandleCurve ------------------------------------------------------------------
// Curva a maniglie: coppie (punto, maniglia). La maniglia fissa la tangente in uscita;
// quella in entrata e' simmetrica. Tratti di Bezier cubici uniti con tangenza.
bool CmdHandleCurve(Engine& e, TokenStream& ts) {
  std::vector<ON_3dPoint> P, H;
  while (true) {
    auto p = e.ReadPoint(ts, P.size() < 2 ? "punto della curva" : "punto della curva (Invio per finire)");
    if (!p) { if (e.NeedsInput()) return false; break; }
    auto h = e.ReadPoint(ts, "maniglia (direzione e forza della tangente)");
    if (!h) return false;
    P.push_back(*p);
    H.push_back(*h);
    if (!e.MoreInput(ts)) break;
  }
  if (P.size() < 2) return e.Fail("servono almeno 2 punti con maniglia");
  const int n = (int)P.size() - 1;
  auto* c = new ON_NurbsCurve(3, false, 4, 3 * n + 1);
  for (int i = 0; i < n; i++) {
    c->SetCV(3 * i, P[i]);
    c->SetCV(3 * i + 1, H[i]);
    c->SetCV(3 * i + 2, ON_3dPoint(P[i + 1] + (P[i + 1] - H[i + 1])));
  }
  c->SetCV(3 * n, P[n]);
  int k = 0;
  for (int s = 0; s <= n; s++) for (int r = 0; r < 3; r++) c->m_knot[k++] = s;
  e.AddObject(c);
  e.Out() << "  Creata curva a maniglie con " << P.size() << " punti\n";
  return true;
}

// --- Gabbie SubD come poligoni ------------------------------------------------------
struct Poly {
  std::vector<ON_3dPoint> V;
  std::vector<std::vector<int>> F;
};

Poly FromSubD(const ON_SubD& s) {
  Poly p;
  std::map<unsigned, int> idx;
  ON_SubDVertexIterator vit = s.VertexIterator();
  for (const ON_SubDVertex* v = vit.FirstVertex(); v; v = vit.NextVertex()) {
    idx[v->m_id] = (int)p.V.size();
    p.V.push_back(v->ControlNetPoint());
  }
  ON_SubDFaceIterator fit = s.FaceIterator();
  for (const ON_SubDFace* f = fit.FirstFace(); f; f = fit.NextFace()) {
    std::vector<int> face;
    for (unsigned i = 0; i < f->EdgeCount(); i++) face.push_back(idx[f->Vertex(i)->m_id]);
    p.F.push_back(face);
  }
  return p;
}

// Toglie i vertici non usati e le facce degeneri.
void Compact(Poly& p) {
  std::vector<std::vector<int>> F;
  for (auto f : p.F) {
    std::vector<int> g;
    for (int v : f) if (g.empty() || g.back() != v) g.push_back(v);
    while (g.size() > 1 && g.front() == g.back()) g.pop_back();
    if (g.size() >= 3) F.push_back(g);
  }
  std::vector<int> map(p.V.size(), -1);
  std::vector<ON_3dPoint> V;
  for (auto& f : F) for (int& v : f) { if (map[v] < 0) { map[v] = (int)V.size(); V.push_back(p.V[v]); } v = map[v]; }
  p.V = V;
  p.F = F;
}

ON_SubD* ToSubD(Poly p) {
  Compact(p);
  if (p.F.empty()) return nullptr;
  ON_Mesh m;
  for (const auto& q : p.V) m.m_V.Append(ON_3fPoint(q));
  for (const auto& f : p.F) {
    if (f.size() == 3) m.SetTriangle(m.m_F.Count(), f[0], f[1], f[2]);
    else if (f.size() == 4) m.SetQuad(m.m_F.Count(), f[0], f[1], f[2], f[3]);
    else {  // n-agono: ventaglio di triangoli raccolti in un ngon
      ON_SimpleArray<unsigned int> fi;
      for (size_t k = 1; k + 1 < f.size(); k++) {
        fi.Append((unsigned)m.m_F.Count());
        m.SetTriangle(m.m_F.Count(), f[0], f[k], f[k + 1]);
      }
      m.AddNgon(fi);
    }
  }
  ON_SubD* s = ON_SubD::CreateFromMesh(&m, nullptr, nullptr);
  if (s && !s->IsValid()) { delete s; return nullptr; }
  return s;
}

using EdgeKey = std::pair<int, int>;
EdgeKey Key(int a, int b) { return {std::min(a, b), std::max(a, b)}; }

// Anelli di bordo: ogni anello va nel verso opposto a quello delle facce.
std::vector<std::vector<int>> BoundaryLoops(const Poly& p) {
  std::set<std::pair<int, int>> half;
  for (const auto& f : p.F) for (size_t k = 0; k < f.size(); k++) half.insert({f[k], f[(k + 1) % f.size()]});
  std::map<int, int> next;
  for (const auto& [a, b] : half) if (!half.count({b, a})) next[b] = a;
  std::vector<std::vector<int>> loops;
  std::set<int> done;
  for (const auto& [a, b] : next) {
    if (done.count(a)) continue;
    std::vector<int> loop;
    int v = a;
    while (!done.count(v) && next.count(v)) { done.insert(v); loop.push_back(v); v = next[v]; }
    if (loop.size() >= 2) loops.push_back(loop);
  }
  return loops;
}

// SubD selezionato o il piu' vicino al punto.
bool SubDNear(Engine& e, const ON_3dPoint& p, ON_UUID& id) {
  double best = 1e300;
  for (const auto& oid : e.AllObjectIds(false)) {
    const auto* s = ON_SubD::Cast(Geo(e, oid));
    if (!s) continue;
    Poly q = FromSubD(*s);
    for (const auto& v : q.V) if (v.DistanceTo(p) < best) { best = v.DistanceTo(p); id = oid; }
    for (const auto& f : q.F) {
      ON_3dPoint c(0, 0, 0);
      for (int v : f) c += q.V[v];
      c = c / double(f.size());
      if (c.DistanceTo(p) < best) { best = c.DistanceTo(p); id = oid; }
    }
  }
  return best < 1e299;
}

double SegDist(const ON_3dPoint& p, const ON_3dPoint& a, const ON_3dPoint& b) {
  ON_Line l(a, b);
  double t;
  l.ClosestPointTo(p, &t);
  t = std::clamp(t, 0.0, 1.0);
  return l.PointAt(t).DistanceTo(p);
}

// Reflect: SubD speculare rispetto al piano Axis=X (piano YZ), Y o Z, per Origin=;
// la meta' originale e quella riflessa si saldano sul piano.
bool CmdReflect(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("Reflect", "seleziona i SubD, poi Invio");
  if (sel.empty()) return false;
  opts.merge(e.ReadOptions(ts));
  const std::string ax = opts.count("axis") ? opts["axis"] : "x";
  ON_3dVector n = ax == "y" ? ON_3dVector::YAxis : ax == "z" ? ON_3dVector::ZAxis : ON_3dVector::XAxis;
  double o = OptNum(opts, "origin", 0.0);
  int k = 0;
  for (const auto& id : sel) {
    const auto* s = ON_SubD::Cast(Geo(e, id));
    if (!s) continue;
    Poly p = FromSubD(*s);
    double tol = 1e-6;
    {
      ON_BoundingBox bb = s->BoundingBox();
      tol = std::max(e.Tolerance(), bb.Diagonal().Length() * 1e-6);
    }
    const int nv = (int)p.V.size();
    std::vector<int> mir(nv);
    for (int i = 0; i < nv; i++) {
      const double d = ON_3dVector(p.V[i]) * n - o;
      if (std::fabs(d) <= tol) { mir[i] = i; p.V[i] = p.V[i] - n * d; continue; }  // sul piano: si salda
      mir[i] = (int)p.V.size();
      p.V.push_back(p.V[i] - n * (2 * d));
    }
    // le facce che stanno sul piano di riflessione spariscono (diventerebbero interne)
    std::vector<std::vector<int>> keep;
    for (const auto& f : p.F) {
      bool allOn = true;
      for (int v : f) if (mir[v] != v) { allOn = false; break; }
      if (!allOn) keep.push_back(f);
    }
    p.F = keep;
    const size_t nf = p.F.size();
    for (size_t f = 0; f < nf; f++) {
      std::vector<int> g;
      for (auto it = p.F[f].rbegin(); it != p.F[f].rend(); ++it) g.push_back(mir[*it]);
      p.F.push_back(g);
    }
    ON_SubD* r = ToSubD(p);
    if (!r) continue;
    e.ReplaceObject(id, r);
    k++;
  }
  if (!k) return e.Fail("nessun SubD riflesso");
  e.Out() << "  Riflessi " << k << " SubD\n";
  return true;
}

// Punti a distanze uguali (per lunghezza) lungo la curva.
std::vector<ON_3dPoint> Sample(const ON_Curve& c, int count, bool closed, std::vector<double>* params = nullptr) {
  std::vector<ON_3dPoint> pts;
  const double L = CurveLengthOf(c);
  const int segs = closed ? count : count - 1;
  for (int i = 0; i < count; i++) {
    const double t = i == 0 ? c.Domain()[0] : ParamAtLengthOf(c, L * double(i) / segs);
    if (params) params->push_back(t);
    pts.push_back(c.PointAt(t));
  }
  return pts;
}

// SubDSweep: SubD che segue un binario (primo oggetto) con una sezione (secondo).
bool CmdSubDSweep(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts, {"closed"});
  auto sel = e.RequireSelection("SubDSweep", "seleziona il binario e poi la sezione, poi Invio");
  if (sel.empty()) return false;
  std::vector<const ON_Curve*> cs;
  for (const auto& id : sel) if (const auto* c = ON_Curve::Cast(Geo(e, id))) cs.push_back(c);
  if (cs.size() < 2) return e.Fail("seleziona un binario e una sezione");
  const ON_Curve& rail = *cs[0];
  const ON_Curve& prof = *cs[1];
  const bool railClosed = rail.IsClosed(), profClosed = prof.IsClosed();
  ON_NurbsCurve np;
  prof.GetNurbForm(np);
  const int M = std::max(4, (int)OptNum(opts, "profilepoints", std::min(16, std::max(4, np.CVCount()))));
  const int N = std::max(3, (int)OptNum(opts, "railpoints", 12));
  std::vector<double> rt;
  const auto rp = Sample(rail, N, railClosed, &rt);
  // riferimenti che minimizzano la rotazione (doppia riflessione)
  std::vector<ON_3dVector> T(N), X(N);
  for (int i = 0; i < N; i++) T[i] = rail.TangentAt(rt[i]);
  ON_3dVector x0 = ON_CrossProduct(T[0], ON_3dVector::ZAxis);
  if (!x0.Unitize()) { x0 = ON_CrossProduct(T[0], ON_3dVector::XAxis); x0.Unitize(); }
  X[0] = x0;
  for (int i = 0; i + 1 < N; i++) {
    ON_3dVector v1 = rp[i + 1] - rp[i];
    const double c1 = v1 * v1;
    if (c1 < 1e-24) { X[i + 1] = X[i]; continue; }
    ON_3dVector rL = X[i] - (2 / c1) * (v1 * X[i]) * v1, tL = T[i] - (2 / c1) * (v1 * T[i]) * v1;
    ON_3dVector v2 = T[i + 1] - tL;
    const double c2 = v2 * v2;
    X[i + 1] = c2 < 1e-24 ? rL : rL - (2 / c2) * (v2 * rL) * v2;
    X[i + 1].Unitize();
  }
  // sezione nelle coordinate del primo riferimento
  const auto pp = Sample(prof, M, profClosed);
  const ON_3dVector Y0 = ON_CrossProduct(T[0], X[0]);
  std::vector<ON_3dVector> loc;
  for (const auto& q : pp) { ON_3dVector d = q - rp[0]; loc.emplace_back(d * X[0], d * Y0, d * T[0]); }
  Poly p;
  for (int i = 0; i < N; i++) {
    const ON_3dVector Y = ON_CrossProduct(T[i], X[i]);
    for (const auto& l : loc) p.V.push_back(rp[i] + X[i] * l.x + Y * l.y + T[i] * l.z);
  }
  auto id = [&](int i, int j) { return (i % N) * M + (j % M); };
  for (int i = 0; i < N - 1 + (railClosed ? 1 : 0); i++)
    for (int j = 0; j < M - 1 + (profClosed ? 1 : 0); j++) p.F.push_back({id(i, j), id(i, j + 1), id(i + 1, j + 1), id(i + 1, j)});
  ON_SubD* s = ToSubD(p);
  if (!s) return e.Fail("SubD non creato");
  e.AddObject(s);
  e.Out() << "  Creato SubD con " << p.F.size() << " facce\n";
  return true;
}

ON_3dVector FaceNormal(const Poly& p, const std::vector<int>& f) {
  ON_3dVector n(0, 0, 0);
  for (size_t k = 0; k < f.size(); k++) {
    const ON_3dPoint &a = p.V[f[k]], &b = p.V[f[(k + 1) % f.size()]];
    n.x += (a.y - b.y) * (a.z + b.z);
    n.y += (a.z - b.z) * (a.x + b.x);
    n.z += (a.x - b.x) * (a.y + b.y);
  }
  n.Unitize();
  return n;
}

int NearestFace(const Poly& p, const ON_3dPoint& q) {
  int best = -1;
  double bd = 1e300;
  for (size_t f = 0; f < p.F.size(); f++) {
    ON_3dPoint c(0, 0, 0);
    for (int v : p.F[f]) c += p.V[v];
    c = c / double(p.F[f].size());
    if (c.DistanceTo(q) < bd) { bd = c.DistanceTo(q); best = (int)f; }
  }
  return best;
}

// SubDExtrude: estrude la faccia della gabbia piu' vicina al punto lungo la sua normale.
bool CmdSubDExtrude(Engine& e, TokenStream& ts) {
  auto pick = e.ReadPick(ts, "faccia del SubD da estrudere");
  if (!pick) return false;
  auto d = e.ReadNumber(ts, "distanza di estrusione");
  if (!d) return false;
  ON_UUID id;
  if (!SubDNear(e, *pick, id)) return e.Fail("nessun SubD");
  Poly p = FromSubD(*ON_SubD::Cast(Geo(e, id)));
  const int f = NearestFace(p, *pick);
  if (f < 0) return e.Fail("nessuna faccia");
  const std::vector<int> face = p.F[f];
  const ON_3dVector n = FaceNormal(p, face);
  std::vector<int> top;
  for (int v : face) { top.push_back((int)p.V.size()); p.V.push_back(p.V[v] + n * *d); }
  p.F[f] = top;
  for (size_t k = 0; k < face.size(); k++) {
    const size_t k1 = (k + 1) % face.size();
    p.F.push_back({face[k], face[k1], top[k1], top[k]});
  }
  ON_SubD* s = ToSubD(p);
  if (!s) return e.Fail("estrusione non riuscita");
  e.ReplaceObject(id, s);
  e.Out() << "  Estrusa una faccia di " << *d << "\n";
  return true;
}

// Anello di bordo piu' vicino al punto.
int NearestLoop(const Poly& p, const std::vector<std::vector<int>>& loops, const ON_3dPoint& q, int exclude = -1) {
  int best = -1;
  double bd = 1e300;
  for (int l = 0; l < (int)loops.size(); l++) {
    if (l == exclude) continue;
    const auto& L = loops[l];
    for (size_t k = 0; k < L.size(); k++) {
      const double dd = SegDist(q, p.V[L[k]], p.V[L[(k + 1) % L.size()]]);
      if (dd < bd) { bd = dd; best = l; }
    }
  }
  return best;
}

// Bridge: unisce con una fascia di facce due bordi (anelli) di SubD, anche di SubD diversi.
bool CmdBridge(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto p1 = e.ReadPick(ts, "primo bordo");
  if (!p1) return false;
  auto p2 = e.ReadPick(ts, "secondo bordo");
  if (!p2) return false;
  opts.merge(e.ReadOptions(ts));
  const int segs = std::max(1, (int)OptNum(opts, "segments", 1));
  ON_UUID ida, idb;
  if (!SubDNear(e, *p1, ida) || !SubDNear(e, *p2, idb)) return e.Fail("nessun SubD");
  Poly p = FromSubD(*ON_SubD::Cast(Geo(e, ida)));
  if (ON_UuidCompare(ida, idb) != 0) {  // due SubD: si uniscono le gabbie
    Poly q = FromSubD(*ON_SubD::Cast(Geo(e, idb)));
    const int off = (int)p.V.size();
    p.V.insert(p.V.end(), q.V.begin(), q.V.end());
    for (auto f : q.F) { for (int& v : f) v += off; p.F.push_back(f); }
  }
  const auto loops = BoundaryLoops(p);
  const int la = NearestLoop(p, loops, *p1), lb = NearestLoop(p, loops, *p2, la);
  if (la < 0 || lb < 0) return e.Fail("servono due bordi aperti");
  std::vector<int> A = loops[la], B(loops[lb].rbegin(), loops[lb].rend());
  if (A.size() != B.size()) return e.Fail("i due bordi devono avere lo stesso numero di spigoli (" + std::to_string(A.size()) + " e " + std::to_string(B.size()) + ")");
  // allineamento: B ruotato per la minima distanza complessiva
  const size_t n = A.size();
  size_t bestShift = 0;
  double bd = 1e300;
  for (size_t s = 0; s < n; s++) {
    double sum = 0;
    for (size_t k = 0; k < n; k++) sum += p.V[A[k]].DistanceTo(p.V[B[(k + s) % n]]);
    if (sum < bd) { bd = sum; bestShift = s; }
  }
  std::rotate(B.begin(), B.begin() + bestShift, B.end());
  // anelli intermedi
  std::vector<std::vector<int>> rings{A};
  for (int s = 1; s < segs; s++) {
    std::vector<int> r;
    for (size_t k = 0; k < n; k++) {
      r.push_back((int)p.V.size());
      const double t = double(s) / segs;
      p.V.push_back((1 - t) * p.V[A[k]] + t * p.V[B[k]]);
    }
    rings.push_back(r);
  }
  rings.push_back(B);
  for (size_t r = 0; r + 1 < rings.size(); r++)
    for (size_t k = 0; k < n; k++) {
      const size_t k1 = (k + 1) % n;
      p.F.push_back({rings[r][k], rings[r][k1], rings[r + 1][k1], rings[r + 1][k]});
    }
  ON_SubD* s = ToSubD(p);
  if (!s) return e.Fail("ponte non creato");
  e.ReplaceObject(ida, s);
  if (ON_UuidCompare(ida, idb) != 0) e.DeleteObject(idb);
  e.Out() << "  Creato ponte con " << n * segs << " facce\n";
  return true;
}

// InsertEdge: nuovo anello di spigoli attraverso la fascia di quadrilateri
// che contiene lo spigolo piu' vicino al punto (Location= tra 0 e 1, default 0,5).
bool CmdInsertEdge(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto pick = e.ReadPick(ts, "spigolo del SubD");
  if (!pick) return false;
  opts.merge(e.ReadOptions(ts));
  const double loc = std::clamp(OptNum(opts, "location", 0.5), 0.01, 0.99);
  ON_UUID id;
  if (!SubDNear(e, *pick, id)) return e.Fail("nessun SubD");
  Poly p = FromSubD(*ON_SubD::Cast(Geo(e, id)));
  // spigolo piu' vicino
  EdgeKey start{-1, -1};
  double bd = 1e300;
  std::map<EdgeKey, std::vector<int>> edgeFaces;
  for (size_t f = 0; f < p.F.size(); f++)
    for (size_t k = 0; k < p.F[f].size(); k++) {
      const int a = p.F[f][k], b = p.F[f][(k + 1) % p.F[f].size()];
      edgeFaces[Key(a, b)].push_back((int)f);
      const double d = SegDist(*pick, p.V[a], p.V[b]);
      if (d < bd) { bd = d; start = Key(a, b); }
    }
  if (start.first < 0) return e.Fail("nessuno spigolo");
  // percorso della fascia: ogni quadrilatero porta allo spigolo opposto
  std::map<EdgeKey, int> mid;          // spigolo -> nuovo vertice
  std::map<EdgeKey, std::pair<int, int>> dir;  // verso con cui si misura loc
  std::set<int> crossed;
  auto newVertex = [&](EdgeKey k, int from, int to) {
    if (mid.count(k)) return;
    mid[k] = (int)p.V.size();
    p.V.push_back((1 - loc) * p.V[from] + loc * p.V[to]);
    dir[k] = {from, to};
  };
  newVertex(start, start.first, start.second);
  std::vector<std::pair<EdgeKey, std::pair<int, int>>> queue{{start, {start.first, start.second}}};
  while (!queue.empty()) {
    auto [ek, ab] = queue.back();
    queue.pop_back();
    for (int f : edgeFaces[ek]) {
      if (crossed.count(f) || p.F[f].size() != 4) continue;
      crossed.insert(f);
      const auto& F = p.F[f];
      int i = 0;
      while (Key(F[i], F[(i + 1) % 4]) != ek) i++;
      // spigolo opposto: F[i+2], F[i+3]; il vertice "corrispondente" ad ab.first sta dallo stesso lato
      const int a = F[i], b = F[(i + 1) % 4], c = F[(i + 2) % 4], d = F[(i + 3) % 4];
      const int fromOpp = ab.first == a ? d : c, toOpp = ab.first == a ? c : d;
      const EdgeKey ok = Key(c, d);
      newVertex(ok, fromOpp, toOpp);
      queue.push_back({ok, dir[ok]});
      (void)b;
    }
  }
  // nuove facce
  std::vector<std::vector<int>> F2;
  for (size_t f = 0; f < p.F.size(); f++) {
    const auto& F = p.F[f];
    if (crossed.count((int)f)) {
      int i = 0;
      while (!(mid.count(Key(F[i], F[(i + 1) % 4])) && mid.count(Key(F[(i + 2) % 4], F[(i + 3) % 4])))) i++;
      const int a = F[i], b = F[(i + 1) % 4], c = F[(i + 2) % 4], d = F[(i + 3) % 4];
      const int m1 = mid[Key(a, b)], m2 = mid[Key(c, d)];
      F2.push_back({a, m1, m2, d});
      F2.push_back({m1, b, c, m2});
    } else {
      std::vector<int> g;  // facce non attraversate: il nuovo vertice entra nel lato
      for (size_t k = 0; k < F.size(); k++) {
        g.push_back(F[k]);
        auto it = mid.find(Key(F[k], F[(k + 1) % F.size()]));
        if (it != mid.end()) g.push_back(it->second);
      }
      F2.push_back(g);
    }
  }
  p.F = F2;
  ON_SubD* s = ToSubD(p);
  if (!s) return e.Fail("spigoli non inseriti");
  e.ReplaceObject(id, s);
  e.Out() << "  Inserito un anello di " << mid.size() << " vertici\n";
  return true;
}

// Stitch: unisce i SubD selezionati saldando i vertici di bordo piu' vicini di Distance=.
bool CmdStitch(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("Stitch", "seleziona i SubD da cucire, poi Invio");
  if (sel.empty()) return false;
  opts.merge(e.ReadOptions(ts));
  Poly p;
  std::vector<ON_UUID> used;
  for (const auto& id : sel) {
    const auto* s = ON_SubD::Cast(Geo(e, id));
    if (!s) continue;
    Poly q = FromSubD(*s);
    const int off = (int)p.V.size();
    p.V.insert(p.V.end(), q.V.begin(), q.V.end());
    for (auto f : q.F) { for (int& v : f) v += off; p.F.push_back(f); }
    used.push_back(id);
  }
  if (used.empty()) return e.Fail("nessun SubD selezionato");
  ON_BoundingBox bb;
  for (const auto& v : p.V) bb.Set(v, bb.IsValid());
  const double dist = OptNum(opts, "distance", bb.Diagonal().Length() * 0.01);
  std::set<int> boundary;
  for (const auto& L : BoundaryLoops(p)) boundary.insert(L.begin(), L.end());
  std::vector<int> rep(p.V.size());
  for (size_t i = 0; i < rep.size(); i++) rep[i] = (int)i;
  std::function<int(int)> root = [&](int i) { return rep[i] == i ? i : rep[i] = root(rep[i]); };
  int merged = 0;
  std::vector<int> bv(boundary.begin(), boundary.end());
  for (size_t a = 0; a < bv.size(); a++)
    for (size_t b = a + 1; b < bv.size(); b++) {
      const int ra = root(bv[a]), rb = root(bv[b]);
      if (ra == rb || p.V[bv[a]].DistanceTo(p.V[bv[b]]) > dist) continue;
      p.V[ra] = 0.5 * (p.V[ra] + p.V[rb]);
      rep[rb] = ra;
      merged++;
    }
  for (auto& f : p.F) for (int& v : f) v = root(v);
  ON_SubD* s = ToSubD(p);
  if (!s) return e.Fail("cucitura non riuscita");
  e.ReplaceObject(used[0], s);
  for (size_t i = 1; i < used.size(); i++) e.DeleteObject(used[i]);
  e.Out() << "  Saldati " << merged << " vertici\n";
  return true;
}

// Append: aggiunge una faccia al SubD per i punti dati (i punti vicini ai vertici esistenti li usano).
bool CmdAppend(Engine& e, TokenStream& ts) {
  auto first = e.ReadPick(ts, "SubD a cui aggiungere la faccia (primo vertice)");
  if (!first) return false;
  ON_UUID id;
  if (!SubDNear(e, *first, id)) return e.Fail("nessun SubD");
  Poly p = FromSubD(*ON_SubD::Cast(Geo(e, id)));
  ON_BoundingBox bb;
  for (const auto& v : p.V) bb.Set(v, bb.IsValid());
  const double snap = std::max(e.Tolerance() * 10, bb.Diagonal().Length() * 1e-3);
  std::vector<ON_3dPoint> pts{*first};
  while (true) {
    auto q = e.ReadPoint(ts, pts.size() < 3 ? "vertice successivo" : "vertice successivo (Invio per finire)");
    if (!q) { if (e.NeedsInput()) return false; break; }
    pts.push_back(*q);
    if (!e.MoreInput(ts)) break;
  }
  if (pts.size() < 3) return e.Fail("servono almeno 3 vertici");
  std::vector<int> face;
  for (const auto& q : pts) {
    int best = -1;
    double bd = snap;
    for (size_t i = 0; i < p.V.size(); i++) if (p.V[i].DistanceTo(q) <= bd) { bd = p.V[i].DistanceTo(q); best = (int)i; }
    if (best < 0) { best = (int)p.V.size(); p.V.push_back(q); }
    face.push_back(best);
  }
  // verso coerente con le facce vicine: uno spigolo condiviso va percorso al contrario
  std::set<std::pair<int, int>> half;
  for (const auto& f : p.F) for (size_t k = 0; k < f.size(); k++) half.insert({f[k], f[(k + 1) % f.size()]});
  for (size_t k = 0; k < face.size(); k++)
    if (half.count({face[k], face[(k + 1) % face.size()]})) { std::reverse(face.begin(), face.end()); break; }
  p.F.push_back(face);
  ON_SubD* s = ToSubD(p);
  if (!s) return e.Fail("faccia non aggiunta");
  e.ReplaceObject(id, s);
  e.Out() << "  Aggiunta una faccia con " << face.size() << " vertici\n";
  return true;
}

// EditPtOn: punti di modifica (sulla curva) invece dei punti di controllo.
bool CmdEditPtOn(Engine& e, TokenStream&) {
  auto sel = e.RequireSelection("EditPtOn", "seleziona le curve, poi Invio");
  if (sel.empty()) return false;
  int n = 0;
  for (const auto& id : sel) {
    const auto* c = ON_Curve::Cast(Geo(e, id));
    if (!c || c->IsPeriodic()) continue;
    e.pointsOn.insert(id);
    e.editPointsOn.insert(id);
    n++;
  }
  if (!n) return e.Fail("nessuna curva aperta o chiusa non periodica selezionata");
  e.ClearSelection();
  e.Out() << "  Punti di modifica attivi su " << n << " curve\n";
  return true;
}

// DimStyle: crea o modifica uno stile di quota e lo rende corrente.
// Name= (default "Kurva3D"), TextHeight=, ArrowSize=, Precision=, ExtExtension=, ExtOffset=, DimScale=.
bool CmdDimStyle(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  ONX_Model& m = e.Model();
  const std::string name = opts.count("name") ? Unquote(opts["name"]) : "Kurva3D";
  const ON_wString wname(name.c_str());
  // DimensionStyleFromName restituisce lo stile di sistema se il nome non c'e'
  const ON_DimStyle* existing = ON_DimStyle::Cast(m.DimensionStyleFromName(wname).ModelComponent());
  if (existing && (existing->IsSystemComponent() || ON_wString::CompareOrdinal(existing->Name(), wname, true) != 0)) existing = nullptr;
  const ON_DimStyle* base = existing;
  if (!base && !ON_UuidIsNil(m.CurrentDimensionStyleId())) {
    base = ON_DimStyle::Cast(m.CurrentDimensionStyle().ModelComponent());
    ON_wString mark;
    if (base && !base->GetUserString(L"Kurva3D.DimStyle", mark)) base = nullptr;  // come le quote: stile predefinito
  }
  if (!base) base = ON_DimStyle::Cast(m.DefaultDimensionStyle().ModelComponent());
  auto* ds = new ON_DimStyle(base ? *base : ON_DimStyle::Default);
  ds->SetUserString(L"Kurva3D.DimStyle", L"1");
  if (opts.count("textheight")) ds->SetTextHeight(OptNum(opts, "textheight", ds->TextHeight()));
  if (opts.count("arrowsize")) ds->SetArrowSize(OptNum(opts, "arrowsize", ds->ArrowSize()));
  if (opts.count("precision")) ds->SetLengthResolution((int)OptNum(opts, "precision", ds->LengthResolution()));
  if (opts.count("extextension")) ds->SetExtExtension(OptNum(opts, "extextension", ds->ExtExtension()));
  if (opts.count("extoffset")) ds->SetExtOffset(OptNum(opts, "extoffset", ds->ExtOffset()));
  if (opts.count("dimscale")) ds->SetDimScale(OptNum(opts, "dimscale", ds->DimScale()));
  ON_UUID id = ON_nil_uuid;
  if (existing) {
    id = existing->Id();
    m.RemoveModelComponent(ON_ModelComponent::Type::DimStyle, id);
    ds->SetId(id);
  } else {
    ds->ClearId();
    ds->ClearIndex();
    ds->ClearParentId();
    ds->SetName(wname);
  }
  // con l'id vuoto openNURBS ne assegna uno nuovo
  if (!m.AddManagedModelComponent(ds, existing == nullptr).ModelComponent()) return e.Fail("stile non salvato");
  id = ds->Id();
  if (!m.SetCurrentDimensionStyleId(id)) e.Out() << "  (stile non impostato come corrente)\n";
  const auto* cur = ON_DimStyle::Cast(m.DimensionStyleFromId(id).ModelComponent());
  e.Out() << "  Stile di quota \"" << name << "\" corrente: altezza testo " << (cur ? cur->TextHeight() : 0)
          << ", frecce " << (cur ? cur->ArrowSize() : 0) << ", decimali " << (cur ? cur->LengthResolution() : 0) << "\n";
  return true;
}

// Immagine in toni di grigio (0..1) da PGM o BMP non compresso; l'interfaccia
// grafica ne fornisce un lettore piu' completo (PNG, JPG...) con Engine::imageLoader.
bool LoadGray(const std::string& path, int& w, int& h, std::vector<double>& g) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::string magic;
  f >> magic;
  if (magic == "P2" || magic == "P5") {
    auto next = [&]() { std::string t; while (f >> t) { if (t[0] == '#') { std::getline(f, t); continue; } return std::stoi(t); } return -1; };
    w = next(); h = next();
    const int mx = next();
    if (w <= 0 || h <= 0 || mx <= 0) return false;
    g.assign(size_t(w) * h, 0);
    if (magic == "P5") {
      f.get();
      for (auto& v : g) { const int c = f.get(); v = double(c) / mx; }
    } else for (auto& v : g) v = double(next()) / mx;
    return true;
  }
  f.clear();
  f.seekg(0);
  unsigned char hd[54];
  if (!f.read(reinterpret_cast<char*>(hd), 54) || hd[0] != 'B' || hd[1] != 'M') return false;
  auto u32 = [&](int o) { return unsigned(hd[o]) | unsigned(hd[o + 1]) << 8 | unsigned(hd[o + 2]) << 16 | unsigned(hd[o + 3]) << 24; };
  const unsigned off = u32(10);
  w = int(u32(18));
  int hh = int(u32(22));
  const int bpp = hd[28] | hd[29] << 8;
  if (bpp != 24 && bpp != 32) return false;
  const bool bottomUp = hh > 0;
  h = std::abs(hh);
  const int row = ((w * bpp / 8) + 3) & ~3;
  std::vector<unsigned char> data(size_t(row) * h);
  f.seekg(off);
  if (!f.read(reinterpret_cast<char*>(data.data()), data.size())) return false;
  g.assign(size_t(w) * h, 0);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      const unsigned char* px = &data[size_t(bottomUp ? h - 1 - y : y) * row + size_t(x) * (bpp / 8)];
      g[size_t(y) * w + x] = (0.114 * px[0] + 0.587 * px[1] + 0.299 * px[2]) / 255.0;
    }
  return true;
}

// Heightfield: superficie dai toni di grigio di un'immagine.
// Nome del file, poi due angoli del rettangolo, poi l'altezza massima.
// PointCount=UxV (default 20x20 o la dimensione dell'immagine se piu' piccola).
bool CmdHeightfield(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto file = e.ReadString(ts, "file immagine");
  if (!file) return false;
  auto a = e.ReadPoint(ts, "primo angolo");
  if (!a) return false;
  auto b = e.ReadPoint(ts, "angolo opposto");
  if (!b) return false;
  auto hmax = e.ReadNumber(ts, "altezza massima");
  if (!hmax) return false;
  opts.merge(e.ReadOptions(ts));
  int w = 0, h = 0;
  std::vector<double> g;
  const std::string path = Unquote(*file);
  bool ok = e.imageLoader ? e.imageLoader(path, w, h, g) : false;
  if (!ok) ok = LoadGray(path, w, h, g);
  if (!ok || w < 2 || h < 2) return e.Fail("immagine non leggibile: " + path);
  int nu = std::min(w, 20), nv = std::min(h, 20);
  if (opts.count("pointcount")) {
    int x = 0, y = 0;
    if (std::sscanf(opts["pointcount"].c_str(), "%dx%d", &x, &y) == 2 && x >= 2 && y >= 2) { nu = x; nv = y; }
  }
  nu = std::max(nu, 4);
  nv = std::max(nv, 4);
  const double x0 = std::min(a->x, b->x), x1 = std::max(a->x, b->x), y0 = std::min(a->y, b->y), y1 = std::max(a->y, b->y);
  if (x1 - x0 <= 0 || y1 - y0 <= 0) return e.Fail("rettangolo nullo");
  auto gray = [&](double u, double v) {  // bilineare; v=0 in basso (riga in fondo all'immagine)
    const double fx = u * (w - 1), fy = (1 - v) * (h - 1);
    const int ix = std::min(w - 2, int(fx)), iy = std::min(h - 2, int(fy));
    const double tx = fx - ix, ty = fy - iy;
    auto G = [&](int x, int y) { return g[size_t(y) * w + x]; };
    return (1 - ty) * ((1 - tx) * G(ix, iy) + tx * G(ix + 1, iy)) + ty * ((1 - tx) * G(ix, iy + 1) + tx * G(ix + 1, iy + 1));
  };
  auto* ns = new ON_NurbsSurface(3, false, 4, 4, nu, nv);
  ns->MakeClampedUniformKnotVector(0);
  ns->MakeClampedUniformKnotVector(1);
  for (int i = 0; i < nu; i++)
    for (int j = 0; j < nv; j++) {
      const double u = double(i) / (nu - 1), v = double(j) / (nv - 1);
      ns->SetCV(i, j, ON_3dPoint(x0 + u * (x1 - x0), y0 + v * (y1 - y0), a->z + *hmax * gray(u, v)));
    }
  ON_Brep* br = ON_Brep::New();
  ON_Surface* sp = ns;
  if (!br->Create(sp)) { delete br; delete ns; return e.Fail("superficie non creata"); }
  e.AddObject(br);
  e.Out() << "  Creata superficie da immagine " << w << "x" << h << " con " << nu << "x" << nv << " punti\n";
  return true;
}

// TextObject: testo trasformato in curve (contorni dei caratteri). Testo tra
// virgolette, punto di inserimento; Height= (default 5), Font=.
bool CmdTextObject(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto text = e.ReadString(ts, "testo");
  if (!text) return false;
  auto p = e.ReadPoint(ts, "punto di inserimento");
  if (!p) return false;
  opts.merge(e.ReadOptions(ts));
  if (!e.textOutliner) return e.Fail("TextObject richiede i caratteri del sistema: usalo dalla finestra di Kurva3D");
  const double height = OptNum(opts, "height", 5.0);
  const std::string font = opts.count("font") ? Unquote(opts["font"]) : "Arial";
  std::vector<std::vector<std::vector<ON_3dPoint>>> contours;
  if (!e.textOutliner(Unquote(*text), font, height, contours) || contours.empty()) return e.Fail("nessun contorno per questo testo");
  int n = 0;
  for (const auto& contour : contours) {
    auto* pc = new ON_PolyCurve;
    for (const auto& seg : contour) {
      if (seg.size() == 2) {
        if (seg[0].DistanceTo(seg[1]) > 1e-9) pc->Append(new ON_LineCurve(seg[0] + ON_3dVector(*p), seg[1] + ON_3dVector(*p)));
      } else if (seg.size() == 4) {
        auto* c = new ON_NurbsCurve(3, false, 4, 4);
        for (int k = 0; k < 4; k++) c->SetCV(k, seg[k] + ON_3dVector(*p));
        c->MakeClampedUniformKnotVector(1.0);
        pc->Append(c);
      }
    }
    if (pc->Count() == 0) { delete pc; continue; }
    if (pc->Count() == 1) { ON_Curve* single = pc->HarvestSegment(0); delete pc; e.AddObject(single); }
    else e.AddObject(pc);
    n++;
  }
  e.Out() << "  Creati " << n << " contorni\n";
  return true;
}


// --- Bevel ---------------------------------------------------------------------------
// Smussa gli spigoli del SubD cliccati (un punto per spigolo, Invio per finire):
// lo spigolo diventa una faccia larga 2 x Distance= (default 1/5 dello spigolo).
bool BevelEdge(Poly& p, int a, int b, double dist) {
  auto findDir = [&](int x, int y) -> int {  // faccia con lo spigolo orientato x->y
    for (size_t f = 0; f < p.F.size(); f++) {
      const auto& F = p.F[f];
      for (size_t k = 0; k < F.size(); k++) if (F[k] == x && F[(k + 1) % F.size()] == y) return (int)f;
    }
    return -1;
  };
  auto pos = [](const std::vector<int>& F, int v) { return (int)(std::find(F.begin(), F.end(), v) - F.begin()); };
  const int f1 = findDir(a, b), f2 = findDir(b, a);
  if (f1 < 0 || f2 < 0) return false;  // spigolo di bordo
  const auto F1 = p.F[f1], F2 = p.F[f2];
  const int n1 = (int)F1.size(), n2 = (int)F2.size();
  const int pa1 = F1[(pos(F1, a) + n1 - 1) % n1], nb1 = F1[(pos(F1, b) + 1) % n1];
  const int na2 = F2[(pos(F2, a) + 1) % n2], pb2 = F2[(pos(F2, b) + n2 - 1) % n2];
  auto along = [&](int from, int to) {
    ON_3dVector d = p.V[to] - p.V[from];
    const double L = d.Length();
    d.Unitize();
    return p.V[from] + d * std::min(dist, 0.45 * L);
  };
  const int a1 = (int)p.V.size(); p.V.push_back(along(a, pa1));
  const int b1 = (int)p.V.size(); p.V.push_back(along(b, nb1));
  const int a2 = (int)p.V.size(); p.V.push_back(along(a, na2));
  const int b2 = (int)p.V.size(); p.V.push_back(along(b, pb2));
  for (int& v : p.F[f1]) { if (v == a) v = a1; else if (v == b) v = b1; }
  for (int& v : p.F[f2]) { if (v == a) v = a2; else if (v == b) v = b2; }
  // le facce vicine ricevono i nuovi vertici sugli spigoli; dove servono, triangoli di raccordo
  auto insertAfter = [&](int f, int x, int nv) { auto& F = p.F[f]; F.insert(F.begin() + pos(F, x) + 1, nv); };
  auto corner = [&](int v, int g1, int x1, int n1v, int g2, int x2, int n2v, bool firstAfterV) {
    // g1 contiene l'arco v->x1 (si inserisce n1v dopo v); g2 contiene x2->v (si inserisce n2v dopo x2)
    (void)x1; (void)firstAfterV;
    if (g1 >= 0) insertAfter(g1, v, n1v);
    if (g2 >= 0) insertAfter(g2, x2, n2v);
  };
  // vertice a: G1 ha a->pa1, G2 ha na2->a
  const int G1 = findDir(a, pa1), G2 = findDir(na2, a);
  corner(a, G1, pa1, a1, G2, na2, a2, true);
  if (G1 >= 0 && G1 == G2) { auto& F = p.F[G1]; F.erase(F.begin() + pos(F, a)); }
  else p.F.push_back({a1, a, a2});
  // vertice b: H1 ha nb1->b, H2 ha b->pb2
  const int H1 = findDir(nb1, b), H2 = findDir(b, pb2);
  if (H1 >= 0) insertAfter(H1, nb1, b1);
  if (H2 >= 0) insertAfter(H2, b, b2);
  if (H1 >= 0 && H1 == H2) { auto& F = p.F[H1]; F.erase(F.begin() + pos(F, b)); }
  else p.F.push_back({b, b1, b2});
  p.F.push_back({b1, a1, a2, b2});
  return true;
}

bool CmdBevel(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto first = e.ReadPick(ts, "spigolo del SubD da smussare");
  if (!first) return false;
  ON_UUID id;
  if (!SubDNear(e, *first, id)) return e.Fail("nessun SubD");
  Poly p = FromSubD(*ON_SubD::Cast(Geo(e, id)));
  std::vector<ON_3dPoint> picks{*first};
  while (e.MoreInput(ts)) {
    opts.merge(e.ReadOptions(ts));
    auto q = e.ReadPoint(ts, "altro spigolo (Invio per finire)");
    if (!q) { if (e.NeedsInput()) return false; break; }
    picks.push_back(*q);
  }
  opts.merge(e.ReadOptions(ts));
  int done = 0;
  for (const auto& q : picks) {
    int ba = -1, bb = -1;
    double best = 1e300;
    for (const auto& F : p.F)
      for (size_t k = 0; k < F.size(); k++) {
        const int x = F[k], y = F[(k + 1) % F.size()];
        const double d = SegDist(q, p.V[x], p.V[y]);
        if (d < best) { best = d; ba = x; bb = y; }
      }
    if (ba < 0) continue;
    const double L = p.V[ba].DistanceTo(p.V[bb]);
    if (BevelEdge(p, ba, bb, OptNum(opts, "distance", 0.2 * L))) done++;
  }
  if (!done) return e.Fail("nessuno spigolo interno smussato (gli spigoli di bordo non si smussano)");
  ON_SubD* s = ToSubD(p);
  if (!s) return e.Fail("SubD non valido dopo lo smusso");
  e.ReplaceObject(id, s);
  e.Out() << "  Smussati " << done << " spigoli\n";
  return true;
}

// --- MultiPipe ------------------------------------------------------------------------
// Tubi SubD lungo le curve selezionate (Radius=, Sides=, Cap=None/Flat). Le curve
// che si toccano agli estremi a due a due diventano un unico tubo continuo.
bool CmdMultiPipe(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("MultiPipe", "seleziona le curve, poi Invio");
  if (sel.empty()) return false;
  opts.merge(e.ReadOptions(ts));
  const double r = OptNum(opts, "radius", 1.0);
  const int sides = std::max(3, (int)OptNum(opts, "sides", 4));
  const bool cap = !(opts.count("cap") && opts["cap"] == "none");
  if (r <= 0) return e.Fail("raggio non valido");
  ON_SimpleArray<const ON_Curve*> in;
  for (const auto& id : sel) if (const auto* c = ON_Curve::Cast(Geo(e, id))) in.Append(c);
  if (in.Count() == 0) return e.Fail("nessuna curva selezionata");
  ON_SimpleArray<ON_Curve*> joined;
  ON_JoinCurves(in, joined, std::max(e.Tolerance(), r * 1e-3), false);
  int made = 0;
  for (int ci = 0; ci < joined.Count(); ci++) {
    std::unique_ptr<ON_Curve> c(joined[ci]);
    const bool closed = c->IsClosed();
    const double L = CurveLengthOf(*c);
    const int N = std::clamp((int)std::ceil(L / (1.5 * r)) + 1, 3, 200);
    std::vector<double> t;
    const auto rp = Sample(*c, N, closed, &t);
    std::vector<ON_3dVector> T(N), X(N);
    for (int i = 0; i < N; i++) { T[i] = c->TangentAt(t[i]); }
    ON_3dVector x0 = ON_CrossProduct(T[0], ON_3dVector::ZAxis);
    if (!x0.Unitize()) { x0 = ON_CrossProduct(T[0], ON_3dVector::XAxis); x0.Unitize(); }
    X[0] = x0;
    for (int i = 0; i + 1 < N; i++) {  // riferimenti a rotazione minima
      ON_3dVector v1 = rp[i + 1] - rp[i];
      const double c1 = v1 * v1;
      if (c1 < 1e-24) { X[i + 1] = X[i]; continue; }
      ON_3dVector rL = X[i] - (2 / c1) * (v1 * X[i]) * v1, tL = T[i] - (2 / c1) * (v1 * T[i]) * v1;
      ON_3dVector v2 = T[i + 1] - tL;
      const double c2 = v2 * v2;
      X[i + 1] = c2 < 1e-24 ? rL : rL - (2 / c2) * (v2 * rL) * v2;
      X[i + 1].Unitize();
    }
    Poly p;
    // la gabbia e' piu' larga del tubo: il limite di Catmull-Clark di un anello di
    // lati regolari passa ai vertici a (4 + 2 cos(2 pi/n)) / 6 volte il raggio della gabbia
    const double rc = r * 6.0 / (4.0 + 2.0 * std::cos(2 * ON_PI / sides));
    for (int i = 0; i < N; i++) {
      const ON_3dVector Y = ON_CrossProduct(T[i], X[i]);
      for (int j = 0; j < sides; j++) {
        const double a = 2 * ON_PI * j / sides;
        p.V.push_back(rp[i] + X[i] * (rc * std::cos(a)) + Y * (rc * std::sin(a)));
      }
    }
    auto id = [&](int i, int j) { return (i % N) * sides + (j % sides); };
    for (int i = 0; i < N - 1 + (closed ? 1 : 0); i++)
      for (int j = 0; j < sides; j++) p.F.push_back({id(i, j), id(i + 1, j), id(i + 1, j + 1), id(i, j + 1)});
    if (cap && !closed) {
      std::vector<int> c0, c1;
      for (int j = sides - 1; j >= 0; j--) c0.push_back(id(0, j));
      for (int j = 0; j < sides; j++) c1.push_back(id(N - 1, j));
      p.F.push_back(c0);
      p.F.push_back(c1);
    }
    if (ON_SubD* s = ToSubD(p)) { e.AddObject(s); made++; }
  }
  if (!made) return e.Fail("nessun tubo creato");
  e.Out() << "  Creati " << made << " tubi SubD\n";
  return true;
}

}  // namespace

// --- CageEdit -------------------------------------------------------------------------
// Gabbia di controllo (oggetto ON_MorphControl, come in Rhino) intorno agli oggetti
// selezionati: spostando i suoi punti (PointsOn) gli oggetti catturati si deformano.
namespace {

ON_3dPoint CagePoint(const ON_MorphControl& mc, const ON_3dPoint& p) {
  const ON_3dPoint rst = mc.m_nurbs_cage0 * p;
  return mc.m_nurbs_cage.PointAt(rst.x, rst.y, rst.z);
}

// Inverso della deformazione (Newton con jacobiano numerico): serve quando la
// geometria originale non e' in memoria (file riaperto).
ON_3dPoint CageInverse(const ON_MorphControl& mc, const ON_3dPoint& q) {
  ON_Xform inv = mc.m_nurbs_cage0;
  if (!inv.Invert()) return q;
  ON_3dPoint rst = mc.m_nurbs_cage0 * q;
  for (int it = 0; it < 30; it++) {
    const ON_3dPoint f = mc.m_nurbs_cage.PointAt(rst.x, rst.y, rst.z);
    const ON_3dVector r = q - f;
    if (r.Length() < 1e-10) break;
    double J[3][3];
    const double h = 1e-6;
    for (int k = 0; k < 3; k++) {
      ON_3dPoint d = rst;
      d[k] += h;
      const ON_3dVector col = (mc.m_nurbs_cage.PointAt(d.x, d.y, d.z) - f) / h;
      for (int m = 0; m < 3; m++) J[m][k] = col[m];
    }
    ON_Matrix M(3, 3);
    for (int m = 0; m < 3; m++) for (int k = 0; k < 3; k++) M[m][k] = J[m][k];
    if (!M.Invert(1e-14)) break;
    for (int m = 0; m < 3; m++) rst[m] += M[m][0] * r.x + M[m][1] * r.y + M[m][2] * r.z;
  }
  return inv * rst;
}

bool CmdCageEdit(Engine& e, TokenStream& ts) {
  auto opts = e.ReadOptions(ts);
  auto sel = e.RequireSelection("CageEdit", "seleziona gli oggetti da catturare, poi Invio");
  if (sel.empty()) return false;
  opts.merge(e.ReadOptions(ts));
  int cnt[3] = {4, 4, 4};
  if (opts.count("pointcount")) std::sscanf(opts["pointcount"].c_str(), "%d,%d,%d", &cnt[0], &cnt[1], &cnt[2]);
  const int deg = std::clamp((int)OptNum(opts, "degree", 3), 1, 5);
  ON_BoundingBox bb;
  std::vector<ON_UUID> caps;
  for (const auto& id : sel) {
    const ON_Geometry* g = Geo(e, id);
    if (!g || ON_MorphControl::Cast(g)) continue;
    ON_BoundingBox t;
    if (g->GetTightBoundingBox(t)) { bb.Union(t); caps.push_back(id); }
  }
  if (caps.empty() || !bb.IsValid()) return e.Fail("nessun oggetto da catturare");
  // spessore minimo (oggetti piani)
  const double pad = std::max(bb.Diagonal().Length() * 0.01, e.Tolerance() * 10);
  for (int k = 0; k < 3; k++) if (bb.m_max[k] - bb.m_min[k] < pad) { bb.m_min[k] -= pad / 2; bb.m_max[k] += pad / 2; }
  for (int k = 0; k < 3; k++) cnt[k] = std::clamp(cnt[k], 2, 30);
  auto* mc = new ON_MorphControl;
  mc->m_varient = 3;
  if (!mc->m_nurbs_cage.Create(bb, std::min(deg, cnt[0] - 1) + 1, std::min(deg, cnt[1] - 1) + 1, std::min(deg, cnt[2] - 1) + 1,
                               cnt[0], cnt[1], cnt[2])) {
    delete mc;
    return e.Fail("gabbia non creata");
  }
  // trasformazione dal mondo ai parametri della gabbia
  ON_Xform x = ON_Xform::IdentityTransformation;
  for (int k = 0; k < 3; k++) {
    const ON_Interval d = mc->m_nurbs_cage.Domain(k);
    const double sc = d.Length() / (bb.m_max[k] - bb.m_min[k]);
    x.m_xform[k][k] = sc;
    x.m_xform[k][3] = d[0] - bb.m_min[k] * sc;
  }
  mc->m_nurbs_cage0 = x;
  for (const auto& id : caps) mc->m_captive_id.AddUuid(id);
  for (const auto& id : caps) e.cageOriginals[id].reset(Geo(e, id)->Duplicate());
  const ON_UUID cid = e.AddObject(mc);
  e.ClearSelection();
  e.Select(cid);
  e.pointsOn.insert(cid);
  e.Out() << "  Gabbia " << cnt[0] << "x" << cnt[1] << "x" << cnt[2] << " di grado " << deg << " con " << caps.size()
          << " oggetti catturati: sposta i suoi punti per deformarli\n";
  return true;
}

}  // namespace

// Ricalcola gli oggetti catturati dalla gabbia (dopo che un suo punto e' stato spostato).
int UpdateCageCaptives(Engine& e, const ON_UUID& cageId) {
  const auto* mc = ON_MorphControl::Cast(Geo(e, cageId));
  if (!mc || mc->m_varient != 3) return 0;
  int n = 0;
  for (int i = 0; i < mc->m_captive_id.Count(); i++) {
    const ON_UUID id = mc->m_captive_id.Array()[i];
    const ON_Geometry* cur = Geo(e, id);
    if (!cur) continue;
    auto& orig = e.cageOriginals[id];
    if (!orig) {
      // file riaperto: l'originale si ricava togliendo la deformazione precedente
      const ON_MorphControl* prev = e.cageBefore.get();
      if (!prev) continue;
      ON_MorphControl pc(*prev);
      orig.reset(DeformGeometry(cur, [&](const ON_3dPoint& q) { return CageInverse(pc, q); }));
      if (!orig) continue;
    }
    ON_Geometry* g = DeformGeometry(orig.get(), [&](const ON_3dPoint& q) { return CagePoint(*mc, q); });
    if (g && !ON_UuidIsNil(e.ReplaceObject(id, g))) n++;
  }
  return n;
}

namespace {
}  // namespace

void RegisterExtra6Commands(Engine& e) {
  auto reg = [&](const char* n, const char* cat, const char* d, CommandFn f) {
    CommandInfo i{n, cat, d, f};
    i.modifiesModel = true;
    e.Register(i);
  };
  reg("HandleCurve", "Curve", "Curva per punti e maniglie di tangenza", CmdHandleCurve);
  reg("Reflect", "SubD", "Riflette i SubD rispetto a un piano (Axis=X/Y/Z, Origin=) saldando le due meta'", CmdReflect);
  reg("SubDSweep", "SubD", "SubD lungo un binario con una sezione", CmdSubDSweep);
  reg("SubDExtrude", "SubD", "Estrude una faccia del SubD", CmdSubDExtrude);
  reg("Bridge", "SubD", "Ponte di facce tra due bordi di SubD (Segments=)", CmdBridge);
  reg("InsertEdge", "SubD", "Inserisce un anello di spigoli nel SubD (Location=)", CmdInsertEdge);
  reg("Stitch", "SubD", "Cuce i SubD saldando i vertici di bordo vicini (Distance=)", CmdStitch);
  reg("Append", "SubD", "Aggiunge una faccia al SubD per i punti dati", CmdAppend);
  reg("EditPtOn", "Curve", "Mostra i punti di modifica delle curve", CmdEditPtOn);
  reg("DimStyle", "Annotazioni", "Crea o modifica lo stile di quota corrente (TextHeight=, ArrowSize=, Precision=...)", CmdDimStyle);
  reg("Heightfield", "Superfici", "Superficie dai toni di grigio di un'immagine", CmdHeightfield);
  reg("TextObject", "Annotazioni", "Testo trasformato in curve", CmdTextObject);
  reg("Bevel", "SubD", "Smussa gli spigoli del SubD (Distance=)", CmdBevel);
  reg("MultiPipe", "SubD", "Tubi SubD lungo le curve (Radius=, Sides=, Cap=None/Flat)", CmdMultiPipe);
  reg("CageEdit", "Trasforma", "Gabbia di controllo per deformare gli oggetti (PointCount=X,Y,Z, Degree=)", CmdCageEdit);
}

}  // namespace cad
