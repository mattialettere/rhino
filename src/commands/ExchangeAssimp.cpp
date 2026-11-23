// FBX, Collada (.dae), 3DS, DirectX (.x) e altri formati mesh con la libreria Open Asset
// Import (Assimp, licenza BSD a 3 clausole): mesh con nomi, materiali (colore, trasparenza,
// texture diffusa), gerarchia dei nodi, unita' e asse verticale del file.
#include "commands/Exchange.h"
#include <assimp/Exporter.hpp>
#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/metadata.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <cmath>
#include <filesystem>
#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <memory>

namespace cad {
namespace fs = std::filesystem;

namespace {

double MetersPerUnit(ON::LengthUnitSystem u) {
  if (u == ON::LengthUnitSystem::None) return 0.001;
  return ON::UnitScale(u, ON::LengthUnitSystem::Meters);
}

std::string U8(const fs::path& p) {
  const auto s = p.u8string();
  return std::string(s.begin(), s.end());
}

ON_Xform ToXform(const aiMatrix4x4& m) {
  ON_Xform x;
  const float* a = &m.a1;
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++) x.m_xform[r][c] = a[r * 4 + c];
  return x;
}

ON_Color ToColor(const aiColor4D& c) {
  auto b = [](float v) { return (int)std::lround(std::clamp(v, 0.0f, 1.0f) * 255); };
  return ON_Color(b(c.r), b(c.g), b(c.b));
}

// Rotazione che porta l'asse Y in alto del file sull'asse Z di Kurva3D.
ON_Xform YUpToZUp() {
  ON_Xform r;
  r.Rotation(ON_PI / 2, ON_3dVector::XAxis, ON_3dPoint::Origin);
  return r;
}

// "fbx" -> id dell'esportatore di Assimp
const char* ExporterId(const std::string& ext) {
  if (ext == "fbx") return "fbx";
  if (ext == "dae") return "collada";
  if (ext == "3ds") return "3ds";
  if (ext == "x") return "x";
  return nullptr;
}

}  // namespace

bool IsAssimpFormat(const std::string& ext, bool forExport) {
  if (forExport) return ExporterId(ext) != nullptr;
  return ext == "fbx" || ext == "dae" || ext == "3ds" || ext == "x" || ext == "lwo" || ext == "off" || ext == "ase" ||
         ext == "blend" || ext == "x3d";
}

bool ReadAssimp(const std::string& path, ON::LengthUnitSystem docUnits, std::vector<ImportedObject>& out,
                ON::LengthUnitSystem& fileUnits, std::string& err) {
  Assimp::Importer imp;
  const aiScene* sc = imp.ReadFile(path.c_str(), aiProcess_Triangulate | aiProcess_JoinIdenticalVertices |
                                                     aiProcess_SortByPType | aiProcess_ValidateDataStructure);
  if (!sc || !sc->mRootNode) {
    err = std::string("file non letto: ") + imp.GetErrorString();
    return false;
  }
  const std::string ext = FileExtension(path);
  // unita' del file in metri e asse verticale
  double fileMeters = 0;  // 0 = sconosciute (si usano le unita' del documento)
  // l'importatore di Assimp porta in Y in alto anche i formati con Z in alto (3DS, Collada Z_UP)
  bool yUp = ext == "dae" || ext == "x" || ext == "blend" || ext == "x3d" || ext == "3ds";
  if (ext == "fbx") {
    fileMeters = 0.01;  // FBX: centimetri, salvo UnitScaleFactor
    yUp = true;
    if (sc->mMetaData) {
      double f = 0;
      float ff = 0;
      if (sc->mMetaData->Get("UnitScaleFactor", f) && f > 0) fileMeters = f * 0.01;
      else if (sc->mMetaData->Get("UnitScaleFactor", ff) && ff > 0) fileMeters = ff * 0.01;
      int32_t up = 1;
      if (sc->mMetaData->Get("UpAxis", up)) yUp = up == 1;
    }
  } else if (ext == "dae") {
    fileMeters = 1.0;  // Assimp applica gia' <unit meter=...> alla scena
    if (sc->mMetaData) {
      double f = 0;
      float ff = 0;
      if (sc->mMetaData->Get("UnitScaleFactor", f) && f > 0) fileMeters = f;
      else if (sc->mMetaData->Get("UnitScaleFactor", ff) && ff > 0) fileMeters = ff;
    }
  } else if (ext == "blend" || ext == "x3d") {
    fileMeters = 1.0;
  }
  const double docMeters = MetersPerUnit(docUnits);
  double scale = 1.0;
  if (fileMeters > 0 && docUnits != ON::LengthUnitSystem::None) scale = fileMeters / docMeters;
  fileUnits = ON::LengthUnitSystem::None;
  if (fileMeters > 0) {
    if (std::fabs(fileMeters - 0.01) < 1e-9) fileUnits = ON::LengthUnitSystem::Centimeters;
    else if (std::fabs(fileMeters - 1) < 1e-9) fileUnits = ON::LengthUnitSystem::Meters;
    else if (std::fabs(fileMeters - 0.001) < 1e-9) fileUnits = ON::LengthUnitSystem::Millimeters;
    else if (std::fabs(fileMeters - 0.0254) < 1e-9) fileUnits = ON::LengthUnitSystem::Inches;
  }
  ON_Xform base = ON_Xform::DiagonalTransformation(scale);
  if (yUp) base = base * YUpToZUp();
  // materiali
  const fs::path dir = fs::u8path(path).parent_path();
  std::vector<std::shared_ptr<ExchangeMaterial>> mats(sc->mNumMaterials);
  for (unsigned i = 0; i < sc->mNumMaterials; i++) {
    const aiMaterial* m = sc->mMaterials[i];
    auto em = std::make_shared<ExchangeMaterial>();
    aiString name;
    if (m->Get(AI_MATKEY_NAME, name) == AI_SUCCESS) em->name = name.C_Str();
    aiColor4D c;
    if (m->Get(AI_MATKEY_COLOR_DIFFUSE, c) == AI_SUCCESS) em->diffuse = ToColor(c);
    if (m->Get(AI_MATKEY_COLOR_SPECULAR, c) == AI_SUCCESS) em->specular = ToColor(c);
    if (m->Get(AI_MATKEY_COLOR_EMISSIVE, c) == AI_SUCCESS) em->emission = ToColor(c);
    float op = 1;
    if (m->Get(AI_MATKEY_OPACITY, op) == AI_SUCCESS) em->transparency = std::clamp(1.0 - op, 0.0, 1.0);
    float sh = 0;
    if (m->Get(AI_MATKEY_SHININESS, sh) == AI_SUCCESS) em->shine = std::clamp(sh / 128.0, 0.0, 1.0);
    aiString tex;
    if (m->GetTexture(aiTextureType_DIFFUSE, 0, &tex) == AI_SUCCESS && tex.length) {
      const std::string t = tex.C_Str();
      if (!t.empty() && t[0] == '*') {  // texture incorporata (FBX, glTF)
        const aiTexture* at = sc->GetEmbeddedTexture(t.c_str());
        if (at && at->mHeight == 0) {
          const auto* b = reinterpret_cast<const unsigned char*>(at->pcData);
          em->textureData.assign(b, b + at->mWidth);
          const std::string hint = at->achFormatHint;
          em->textureMime = hint == "png" ? "image/png" : "image/jpeg";
        }
      } else {
        fs::path tp = fs::u8path(t);
        if (tp.is_relative()) tp = dir / tp;
        std::error_code ec;
        if (!fs::exists(tp, ec)) tp = dir / fs::u8path(t).filename();
        em->texture = U8(tp);
      }
    }
    mats[i] = em;
  }
  // nodi con trasformazioni accumulate
  int count = 0;
  std::function<void(const aiNode*, const ON_Xform&, const std::string&)> visit =
      [&](const aiNode* n, const ON_Xform& parent, const std::string& parentName) {
        const ON_Xform x = parent * ToXform(n->mTransformation);
        const std::string nodeName = n->mName.length ? n->mName.C_Str() : parentName;
        for (unsigned k = 0; k < n->mNumMeshes; k++) {
          const aiMesh* am = sc->mMeshes[n->mMeshes[k]];
          if (!(am->mPrimitiveTypes & aiPrimitiveType_TRIANGLE) || am->mNumFaces == 0) continue;
          auto mesh = std::make_unique<ON_Mesh>();
          const bool hasN = am->HasNormals(), hasT = am->HasTextureCoords(0), hasC = am->HasVertexColors(0);
          for (unsigned v = 0; v < am->mNumVertices; v++) {
            ON_3dPoint p(am->mVertices[v].x, am->mVertices[v].y, am->mVertices[v].z);
            p.Transform(x);
            mesh->m_V.Append(ON_3fPoint(p));
            if (hasT) mesh->m_T.Append(ON_2fPoint(am->mTextureCoords[0][v].x, am->mTextureCoords[0][v].y));
            if (hasC) {
              const aiColor4D& c = am->mColors[0][v];
              mesh->m_C.Append(ToColor(c));
            }
          }
          for (unsigned f = 0; f < am->mNumFaces; f++) {
            const aiFace& fc = am->mFaces[f];
            if (fc.mNumIndices != 3) continue;
            mesh->SetTriangle(mesh->m_F.Count(), fc.mIndices[0], fc.mIndices[1], fc.mIndices[2]);
          }
          (void)hasN;
          if (mesh->m_F.Count() == 0) continue;
          if (x.Determinant() < 0) mesh->Flip();
          mesh->ComputeVertexNormals();
          mesh->Compact();
          ImportedObject o;
          o.geom = std::move(mesh);
          o.name = am->mName.length ? am->mName.C_Str() : nodeName;
          if (o.name.empty()) o.name = nodeName;
          if (am->mMaterialIndex < mats.size()) o.material = mats[am->mMaterialIndex];
          out.push_back(std::move(o));
          count++;
        }
        for (unsigned c = 0; c < n->mNumChildren; c++) visit(n->mChildren[c], x, nodeName);
      };
  visit(sc->mRootNode, base, "");
  if (!count) {
    err = "nessuna mesh nel file";
    return false;
  }
  return true;
}

bool WriteAssimp(const std::string& path, const std::vector<ExportItem>& items, ON::LengthUnitSystem docUnits,
                 double tolerance, std::string& err) {
  const std::string ext = FileExtension(path);
  const char* id = ExporterId(ext);
  if (!id) { err = "formato non esportabile: " + ext; return false; }
  // unita' e asse: FBX in centimetri, Collada in metri, Y in alto (come l'esportazione di Rhino)
  const double docMeters = MetersPerUnit(docUnits);
  double scale = 1.0;
  if (ext == "fbx") scale = docMeters / 0.01;
  else if (ext == "dae") scale = docMeters;
  // il 3DS ha Z in alto come Kurva3D (e l'esportatore di Assimp non lo ruota)
  const bool yUp = ext == "fbx" || ext == "dae" || ext == "x";
  ON_Xform xf = ON_Xform::DiagonalTransformation(scale);
  if (yUp) {
    ON_Xform r;
    r.Rotation(-ON_PI / 2, ON_3dVector::XAxis, ON_3dPoint::Origin);
    xf = r * xf;
  }
  struct Part {
    std::unique_ptr<ON_Mesh> mesh;
    int material;
    std::string name;
  };
  std::vector<Part> parts;
  std::map<const ExchangeMaterial*, int> matIndex;
  std::vector<std::shared_ptr<ExchangeMaterial>> materials;
  auto defaultMat = std::make_shared<ExchangeMaterial>();
  defaultMat->name = "Predefinito";
  for (const auto& it : items) {
    auto m = ExportMeshFor(it, tolerance);
    if (!m || m->m_F.Count() == 0) continue;
    std::shared_ptr<ExchangeMaterial> mat = it.material;
    if (!mat) {
      // colore dell'oggetto come materiale semplice
      auto key = std::make_shared<ExchangeMaterial>();
      key->name = "Colore_" + std::to_string(it.color.Red()) + "_" + std::to_string(it.color.Green()) + "_" +
                  std::to_string(it.color.Blue());
      key->diffuse = it.color;
      for (auto& mm : materials)
        if (mm->name == key->name) key = mm;
      mat = key;
    }
    auto f = matIndex.find(mat.get());
    int mi;
    if (f == matIndex.end()) {
      mi = (int)materials.size();
      materials.push_back(mat);
      matIndex[mat.get()] = mi;
    } else mi = f->second;
    m->Transform(xf);
    parts.push_back({std::move(m), mi, it.name.empty() ? "Oggetto_" + std::to_string(parts.size() + 1) : it.name});
  }
  if (parts.empty()) { err = "nessun oggetto esportabile come mesh"; return false; }
  if (materials.empty()) materials.push_back(defaultMat);
  aiScene sc;
  sc.mRootNode = new aiNode("Kurva3D");
  sc.mNumMaterials = (unsigned)materials.size();
  sc.mMaterials = new aiMaterial*[sc.mNumMaterials];
  for (unsigned i = 0; i < sc.mNumMaterials; i++) {
    auto* am = new aiMaterial;
    const ExchangeMaterial& em = *materials[i];
    aiString nm(em.name.empty() ? "Materiale_" + std::to_string(i + 1) : em.name);
    am->AddProperty(&nm, AI_MATKEY_NAME);
    aiColor3D d(em.diffuse.FractionRed(), em.diffuse.FractionGreen(), em.diffuse.FractionBlue());
    am->AddProperty(&d, 1, AI_MATKEY_COLOR_DIFFUSE);
    aiColor3D s(em.specular.FractionRed(), em.specular.FractionGreen(), em.specular.FractionBlue());
    am->AddProperty(&s, 1, AI_MATKEY_COLOR_SPECULAR);
    aiColor3D e(em.emission.FractionRed(), em.emission.FractionGreen(), em.emission.FractionBlue());
    am->AddProperty(&e, 1, AI_MATKEY_COLOR_EMISSIVE);
    const float op = (float)(1.0 - em.transparency);
    am->AddProperty(&op, 1, AI_MATKEY_OPACITY);
    const float sh = (float)(em.shine * 128.0);
    am->AddProperty(&sh, 1, AI_MATKEY_SHININESS);
    if (!em.texture.empty()) {
      aiString t(U8(fs::u8path(em.texture)));
      am->AddProperty(&t, AI_MATKEY_TEXTURE_DIFFUSE(0));
    }
    sc.mMaterials[i] = am;
  }
  sc.mNumMeshes = (unsigned)parts.size();
  sc.mMeshes = new aiMesh*[sc.mNumMeshes];
  sc.mRootNode->mNumChildren = sc.mNumMeshes;
  sc.mRootNode->mChildren = new aiNode*[sc.mNumMeshes];
  for (unsigned i = 0; i < sc.mNumMeshes; i++) {
    const ON_Mesh& m = *parts[i].mesh;
    auto* am = new aiMesh;
    am->mName = aiString(parts[i].name);
    am->mMaterialIndex = (unsigned)parts[i].material;
    am->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
    am->mNumVertices = (unsigned)m.m_V.Count();
    am->mVertices = new aiVector3D[am->mNumVertices];
    const bool hasN = m.HasVertexNormals(), hasT = m.HasTextureCoordinates();
    if (hasN) am->mNormals = new aiVector3D[am->mNumVertices];
    if (hasT) {
      am->mTextureCoords[0] = new aiVector3D[am->mNumVertices];
      am->mNumUVComponents[0] = 2;
    }
    for (unsigned v = 0; v < am->mNumVertices; v++) {
      am->mVertices[v] = aiVector3D(m.m_V[v].x, m.m_V[v].y, m.m_V[v].z);
      if (hasN) am->mNormals[v] = aiVector3D(m.m_N[v].x, m.m_N[v].y, m.m_N[v].z);
      if (hasT) am->mTextureCoords[0][v] = aiVector3D(m.m_T[v].x, m.m_T[v].y, 0);
    }
    std::vector<std::array<int, 3>> tri;
    for (int f = 0; f < m.m_F.Count(); f++) {
      const ON_MeshFace& fc = m.m_F[f];
      tri.push_back({fc.vi[0], fc.vi[1], fc.vi[2]});
      if (fc.IsQuad()) tri.push_back({fc.vi[0], fc.vi[2], fc.vi[3]});
    }
    am->mNumFaces = (unsigned)tri.size();
    am->mFaces = new aiFace[am->mNumFaces];
    for (unsigned f = 0; f < am->mNumFaces; f++) {
      am->mFaces[f].mNumIndices = 3;
      am->mFaces[f].mIndices = new unsigned[3]{(unsigned)tri[f][0], (unsigned)tri[f][1], (unsigned)tri[f][2]};
    }
    sc.mMeshes[i] = am;
    auto* node = new aiNode(parts[i].name);
    node->mParent = sc.mRootNode;
    node->mNumMeshes = 1;
    node->mMeshes = new unsigned[1]{i};
    sc.mRootNode->mChildren[i] = node;
  }
  Assimp::Exporter ex;
  if (ex.Export(&sc, id, path.c_str()) != AI_SUCCESS) {
    err = std::string("esportazione non riuscita: ") + ex.GetErrorString();
    return false;
  }
  return true;
}

}  // namespace cad
