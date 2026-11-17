// 3dm-roundtrip: legge A, salva come Rhino 8 in B, rilegge B e confronta.
// Codice di uscita 0 solo se non ci sono differenze.
#include "core/Document3dm.h"
#include "core/ModelCompare.h"
#include <cstdio>
#include <filesystem>

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "Uso: 3dm-roundtrip input.3dm output.3dm\n"); return 2; }
  cad::InitLibrary();
  int rc = 0;
  {
    std::filesystem::path out(argv[2]);
    if (out.has_parent_path()) std::filesystem::create_directories(out.parent_path());
    // L'istantanea dell'originale si prende PRIMA di salvare: la scrittura
    // normalizza il modello in memoria (es. quote), e una seconda lettura
    // dei file antichi genera id nuovi per gli oggetti che non ne hanno.
    cad::Document3dm a, b;
    cad::ModelSnapshot snapA;
    std::string log;
    if (!a.Load(argv[1], &log)) {
      std::fprintf(stderr, "ERRORE lettura %s\n%s\n", argv[1], log.c_str()); rc = 1;
    } else if (snapA = cad::TakeSnapshot(a.Model()); !a.Save(argv[2], cad::kRhino8ArchiveVersion, &log)) {
      std::fprintf(stderr, "ERRORE scrittura %s\n%s\n", argv[2], log.c_str()); rc = 1;
    } else if (!b.Load(argv[2], &log)) {
      std::fprintf(stderr, "ERRORE rilettura %s\n%s\n", argv[2], log.c_str()); rc = 1;
    } else {
      cad::CompareResult r = cad::CompareSnapshots(snapA, cad::TakeSnapshot(b.Model()));
      const auto sb = b.Summarize();
      std::printf("%s -> %s (file Rhino %d): %d oggetti confrontati, %zu differenze\n",
                  argv[1], argv[2], sb.rhinoVersion, r.objectsCompared, r.differences.size());
      for (auto& d : r.differences) std::printf("  - %s\n", d.c_str());
      if (sb.rhinoVersion != cad::kRhino8ArchiveVersion) {
        std::printf("  - scritto file Rhino %d invece di Rhino 8\n", sb.rhinoVersion); rc = 1;
      }
      if (!r.Equal()) rc = 1;
    }
  }
  cad::ShutdownLibrary();
  return rc;
}
