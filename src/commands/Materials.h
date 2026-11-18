// Materiali con texture (immagine) e mappature delle texture degli oggetti.
// I materiali sono ON_Material del .3dm con una texture bitmap (diffusa) e
// l'immagine incorporata nel file (ON_EmbeddedFile), come fa Rhino 8.
#pragma once
#include "opennurbs.h"
#include <string>
#include <vector>

namespace cad {

class Engine;

// Texture di un oggetto per la visualizzazione.
struct ObjectTexture {
  std::string file;                 // percorso dell'immagine (come salvato nel materiale)
  ON_Xform uvw = ON_Xform::IdentityTransformation;  // ripetizione della texture
  ON_TextureMapping mapping;        // mappatura dell'oggetto (se hasMapping)
  bool hasMapping = false;
  ON_Color diffuse = ON_Color(255, 255, 255);
};

// Materiale con texture dell'oggetto (dall'oggetto o dal suo livello).
bool GetObjectTexture(const ONX_Model& model, const ON_3dmObjectAttributes& a, ObjectTexture& out);

// Percorso leggibile dell'immagine: il file stesso se esiste, altrimenti la copia
// incorporata nel .3dm estratta in cacheDir. Vuoto se non si trova.
std::string ResolveTextureFile(const ONX_Model& model, const std::string& file, const std::string& cacheDir);

// Incorpora nel documento un'immagine da file (una sola copia per percorso).
void EmbedImageFile(ONX_Model& model, const std::string& path);
// Incorpora un'immagine dai suoi byte (es. texture dentro un .glb) con il nome di file
// indicato, che il materiale usera' come percorso della texture.
bool EmbedImageBuffer(ONX_Model& model, const std::string& path, const std::vector<unsigned char>& bytes);

// Id del motore di rendering a cui si associano le mappature degli oggetti.
const ON_UUID& MappingPlugInId();

}  // namespace cad
