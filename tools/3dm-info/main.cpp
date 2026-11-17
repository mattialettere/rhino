// 3dm-info: legge un file .3dm e stampa un riepilogo per categoria.
#include "core/Document3dm.h"
#include <cstdio>

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "Uso: 3dm-info file.3dm [--log]\n"); return 2; }
  cad::InitLibrary();
  int rc = 0;
  {
    cad::Document3dm doc;
    std::string log;
    if (!doc.Load(argv[1], &log)) {
      std::fprintf(stderr, "ERRORE: impossibile leggere %s\n%s\n", argv[1], log.c_str());
      rc = 1;
    } else {
      const cad::ModelSummary s = doc.Summarize();
      std::printf("File: %s\nVersione file: Rhino %d (archivio %d)\nCreato da: %s\nUltima modifica: %s\n\n",
                  argv[1], s.rhinoVersion, s.archiveVersion, s.createdBy.c_str(), s.lastEditedBy.c_str());
      for (auto& [c, n] : s.counts)
        if (n) std::printf("  %-50s %d\n", cad::CategoryName(c), n);
      std::printf("\nClassi openNURBS:\n");
      for (auto& [c, n] : s.geometryClasses) std::printf("  %-50s %d\n", c.c_str(), n);
      if (argc > 2 && !log.empty()) std::printf("\nLog di lettura:\n%s\n", log.c_str());
    }
  }
  cad::ShutdownLibrary();
  return rc;
}
