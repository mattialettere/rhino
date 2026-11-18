#include "core/SubDLimit.h"
#include <algorithm>
#include <map>
#include <unordered_map>

namespace cad {
namespace {

enum VTag : int { Smooth = 0, Crease = 1, Corner = 2 };

struct Net {
  std::vector<ON_3dPoint> P;
  std::vector<int> vtag;
  std::vector<std::vector<int>> F;
  // spigoli marcati: chiave (a<b) -> (vivo, spigolo originale o -1)
  std::map<std::pair<int, int>, std::pair<bool, int>> sharp;
};

std::pair<int, int> Key(int a, int b) { return a < b ? std::make_pair(a, b) : std::make_pair(b, a); }

struct EdgeInfo {
  int a, b;
  int f0 = -1, f1 = -1;
  bool crease = false;
  int orig = -1;
};

struct Topo {
  std::vector<EdgeInfo> E;
  std::map<std::pair<int, int>, int> index;
  std::vector<std::vector<int>> vEdges, vFaces;
};

Topo BuildTopo(const Net& n) {
  Topo t;
  t.vEdges.resize(n.P.size());
  t.vFaces.resize(n.P.size());
  for (int fi = 0; fi < (int)n.F.size(); fi++) {
    const auto& f = n.F[fi];
    for (size_t i = 0; i < f.size(); i++) {
      const int a = f[i], b = f[(i + 1) % f.size()];
      t.vFaces[a].push_back(fi);
      auto k = Key(a, b);
      auto it = t.index.find(k);
      if (it == t.index.end()) {
        EdgeInfo e{k.first, k.second};
        e.f0 = fi;
        auto s = n.sharp.find(k);
        if (s != n.sharp.end()) { e.crease = s->second.first; e.orig = s->second.second; }
        t.index[k] = (int)t.E.size();
        t.vEdges[a].push_back((int)t.E.size());
        t.vEdges[b].push_back((int)t.E.size());
        t.E.push_back(e);
      } else {
        t.E[it->second].f1 = fi;
      }
    }
  }
  for (auto& e : t.E) if (e.f1 < 0) e.crease = true;  // bordo = spigolo vivo
  return t;
}

ON_3dPoint FaceCenter(const Net& n, int fi) {
  ON_3dPoint c(0, 0, 0);
  for (int v : n.F[fi]) c += n.P[v];
  return c / double(n.F[fi].size());
}

int Other(const EdgeInfo& e, int v) { return e.a == v ? e.b : e.a; }

// Tipo effettivo del vertice: angolo, piega (2 spigoli vivi) o liscio.
int EffectiveTag(const Net& n, const Topo& t, int v, int* c0 = nullptr, int* c1 = nullptr) {
  int creases = 0, first = -1, second = -1;
  for (int ei : t.vEdges[v])
    if (t.E[ei].crease) { if (creases == 0) first = ei; else if (creases == 1) second = ei; creases++; }
  if (c0) *c0 = first;
  if (c1) *c1 = second;
  if (n.vtag[v] == Corner || creases > 2) return Corner;
  if (creases == 2) return Crease;
  if (creases == 1) return Smooth;  // dart
  return Smooth;
}

Net Subdivide(const Net& n) {
  const Topo t = BuildTopo(n);
  Net out;
  const int nv = (int)n.P.size(), ne = (int)t.E.size(), nf = (int)n.F.size();
  out.P.resize(nv + ne + nf);
  out.vtag.assign(nv + ne + nf, Smooth);
  std::vector<ON_3dPoint> fp(nf);
  for (int fi = 0; fi < nf; fi++) fp[fi] = out.P[nv + ne + fi] = FaceCenter(n, fi);
  for (int ei = 0; ei < ne; ei++) {
    const EdgeInfo& e = t.E[ei];
    ON_3dPoint p = 0.5 * (n.P[e.a] + n.P[e.b]);
    if (!e.crease && e.f1 >= 0) p = 0.25 * (n.P[e.a] + n.P[e.b] + fp[e.f0] + fp[e.f1]);
    out.P[nv + ei] = p;
    if (e.crease) out.vtag[nv + ei] = Smooth;  // diventa piega per i 2 spigoli vivi
  }
  for (int v = 0; v < nv; v++) {
    int c0, c1;
    const int tag = EffectiveTag(n, t, v, &c0, &c1);
    out.vtag[v] = n.vtag[v] == Corner ? Corner : Smooth;
    if (tag == Corner) { out.P[v] = n.P[v]; out.vtag[v] = Corner; continue; }
    if (tag == Crease) {
      out.P[v] = 0.75 * n.P[v] + 0.125 * (n.P[Other(t.E[c0], v)] + n.P[Other(t.E[c1], v)]);
      continue;
    }
    const int val = (int)t.vEdges[v].size();
    if (val < 3 || t.vFaces[v].empty()) { out.P[v] = n.P[v]; continue; }
    ON_3dPoint Q(0, 0, 0), R(0, 0, 0);
    for (int fi : t.vFaces[v]) Q += fp[fi];
    Q = Q / double(t.vFaces[v].size());
    for (int ei : t.vEdges[v]) R += 0.5 * (n.P[t.E[ei].a] + n.P[t.E[ei].b]);
    R = R / double(val);
    out.P[v] = (Q + 2.0 * R + (val - 3.0) * n.P[v]) / double(val);
  }
  for (int fi = 0; fi < nf; fi++) {
    const auto& f = n.F[fi];
    const int k = (int)f.size();
    for (int i = 0; i < k; i++) {
      const int v = f[i], vn = f[(i + 1) % k], vp = f[(i + k - 1) % k];
      const int eNext = nv + t.index.at(Key(v, vn)), ePrev = nv + t.index.at(Key(vp, v));
      out.F.push_back({v, eNext, nv + ne + fi, ePrev});
    }
  }
  for (int ei = 0; ei < ne; ei++) {
    const EdgeInfo& e = t.E[ei];
    if (!e.crease && e.orig < 0) continue;
    out.sharp[Key(e.a, nv + ei)] = {e.crease, e.orig};
    out.sharp[Key(nv + ei, e.b)] = {e.crease, e.orig};
  }
  return out;
}

// Posizioni limite di una mesh di quad (dopo almeno una suddivisione).
std::vector<ON_3dPoint> LimitPoints(const Net& n) {
  const Topo t = BuildTopo(n);
  std::vector<ON_3dPoint> L(n.P.size());
  for (int v = 0; v < (int)n.P.size(); v++) {
    int c0, c1;
    const int tag = EffectiveTag(n, t, v, &c0, &c1);
    if (tag == Corner) { L[v] = n.P[v]; continue; }
    if (tag == Crease) { L[v] = (n.P[Other(t.E[c0], v)] + 4.0 * n.P[v] + n.P[Other(t.E[c1], v)]) / 6.0; continue; }
    const int val = (int)t.vEdges[v].size();
    if (val < 3 || (int)t.vFaces[v].size() != val) { L[v] = n.P[v]; continue; }
    ON_3dPoint es(0, 0, 0), fs(0, 0, 0);
    for (int ei : t.vEdges[v]) es += n.P[Other(t.E[ei], v)];
    for (int fi : t.vFaces[v]) {
      const auto& f = n.F[fi];
      const auto it = std::find(f.begin(), f.end(), v);
      fs += n.P[f[(std::distance(f.begin(), it) + 2) % f.size()]];  // vertice opposto del quad
    }
    L[v] = (double(val) * val * n.P[v] + 4.0 * es + fs) / (double(val) * (val + 5));
  }
  return L;
}

}  // namespace

SubDLimitDisplay BuildSubDLimit(const ON_SubD& subd, int levels) {
  SubDLimitDisplay out;
  Net n;
  std::unordered_map<unsigned, int> vid;
  ON_SubDVertexIterator vit(subd);
  for (const ON_SubDVertex* v = vit.FirstVertex(); v; v = vit.NextVertex()) {
    vid[v->m_id] = (int)n.P.size();
    n.P.push_back(v->ControlNetPoint());
    n.vtag.push_back(v->m_vertex_tag == ON_SubDVertexTag::Corner ? Corner : Smooth);
  }
  ON_SubDFaceIterator fit(subd);
  for (const ON_SubDFace* f = fit.FirstFace(); f; f = fit.NextFace()) {
    std::vector<int> face;
    for (unsigned i = 0; i < f->EdgeCount(); i++)
      if (const ON_SubDVertex* v = f->Vertex(i)) face.push_back(vid[v->m_id]);
    if (face.size() >= 3) n.F.push_back(face);
  }
  int orig = 0;
  std::vector<unsigned> ids;
  ON_SubDEdgeIterator eit(subd);
  for (const ON_SubDEdge* e = eit.FirstEdge(); e; e = eit.NextEdge(), orig++) {
    ids.push_back(e->m_id);
    if (e->m_vertex[0] && e->m_vertex[1])
      n.sharp[Key(vid[e->m_vertex[0]->m_id], vid[e->m_vertex[1]->m_id])] = {e->m_edge_tag == ON_SubDEdgeTag::Crease, orig};
  }
  if (n.F.empty()) return out;
  for (int i = 0; i < std::max(1, levels); i++) n = Subdivide(n);
  const std::vector<ON_3dPoint> L = LimitPoints(n);

  auto mesh = std::make_unique<ON_Mesh>();
  for (const auto& p : L) mesh->m_V.Append(ON_3fPoint(p));
  for (const auto& f : n.F) mesh->SetQuad(mesh->m_F.Count(), f[0], f[1], f[2], f[3]);
  mesh->ComputeVertexNormals();
  out.mesh = std::move(mesh);

  // spigoli originali: catene dei segmenti con lo stesso numero d'origine
  std::map<int, std::vector<std::pair<int, int>>> segs;
  for (const auto& [k, v] : n.sharp) if (v.second >= 0) segs[v.second].push_back(k);
  for (auto& [id, list] : segs) {
    std::unordered_map<int, std::vector<int>> adj;
    for (const auto& s : list) { adj[s.first].push_back(s.second); adj[s.second].push_back(s.first); }
    int start = list.front().first;
    for (const auto& [v, nb] : adj) if (nb.size() == 1) { start = v; break; }
    std::vector<ON_3dPoint> w{L[start]};
    int prev = -1, cur = start;
    for (size_t step = 0; step < list.size(); step++) {
      const auto& nb = adj[cur];
      int next = -1;
      for (int x : nb) if (x != prev) { next = x; break; }
      if (next < 0) break;
      w.push_back(L[next]);
      prev = cur;
      cur = next;
      if (cur == start) break;
    }
    out.edges.push_back(std::move(w));
    out.edgeIds.push_back(ids[id]);
  }
  return out;
}

SubDLimitDisplay BuildSubDFlat(const ON_SubD& subd) {
  SubDLimitDisplay out;
  out.mesh.reset(subd.GetControlNetMesh(nullptr, ON_SubDGetControlNetMeshPriority::Geometry));
  if (out.mesh) out.mesh->ComputeVertexNormals();
  ON_SubDEdgeIterator eit(subd);
  for (const ON_SubDEdge* e = eit.FirstEdge(); e; e = eit.NextEdge())
    if (e->m_vertex[0] && e->m_vertex[1]) {
      out.edges.push_back({e->m_vertex[0]->ControlNetPoint(), e->m_vertex[1]->ControlNetPoint()});
      out.edgeIds.push_back(e->m_id);
    }
  return out;
}

}  // namespace cad
