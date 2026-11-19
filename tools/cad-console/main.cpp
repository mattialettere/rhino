// cad-console: riga di comando in stile Rhino 8.
//   cad-console                  sessione interattiva ("Comando:")
//   cad-console script.txt       esegue uno script di comandi
//   cad-console -o out.3dm ...   salva il risultato alla fine
//   cad-console --verify -o out.3dm script.txt   dopo il salvataggio rilegge il file e lo
//                                confronta con il modello in memoria (test dei file per Rhino 8)
// Comandi speciali della console: "?" elenca i comandi, "esci" termina.
#include "commands/Engine.h"
#include "core/ModelCompare.h"
#include "python/PythonHost.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

int main(int argc, char** argv) {
  cad::InitLibrary();
  {
    // Python incluso accanto al programma (Windows) o in Kurva3D.app/Contents/Resources/python (Mac)
    std::error_code ec;
    const auto exe = std::filesystem::weakly_canonical(std::filesystem::path(argv[0]), ec);
    if (!ec && exe.has_parent_path()) {
      const auto u8 = exe.parent_path().u8string();  // PythonHost legge il percorso come UTF-8
      cad::SetPythonProgramDir(std::string(u8.begin(), u8.end()));
    }
  }
  int rc = 0;
  {
    std::string script, outPath, openPath;
    bool verify = false;
    for (int i = 1; i < argc; i++) {
      std::string a = argv[i];
      if (a == "--verify") verify = true;
      else if (a == "-o" && i + 1 < argc) outPath = argv[++i];
      else if (a == "-i" && i + 1 < argc) openPath = argv[++i];
      else {
        std::ifstream f(a);
        if (!f) { std::cerr << "File non trovato: " << a << "\n"; return 2; }
        std::stringstream ss; ss << f.rdbuf(); script += ss.str() + "\n";
      }
    }
    cad::Document3dm doc;
    if (!openPath.empty() && !doc.Load(openPath)) { std::cerr << "Impossibile aprire " << openPath << "\n"; return 1; }
    cad::Engine engine(doc, std::cout);
    // Vista prospettica simulata (non c'e' una finestra): permette NamedView, CPlane e
    // NamedCPlane negli script. L'inquadratura segue l'ingombro del modello finche'
    // uno script non ripristina una vista.
    ON_3dmView view;
    bool viewSet = false;
    view.m_name = L"Perspective";
    view.m_vp = ON_Viewport::DefaultPerspectiveViewZUp;
    view.m_vp.SetScreenPort(0, 800, 600, 0, 0, 0xff);
    view.m_vp.SetFrustumAspect(800.0 / 600.0);
    view.m_cplane.m_plane = ON_Plane::World_xy;
    engine.getActiveView = [&](ON_3dmView& v) {
      if (!viewSet) {
        ON_BoundingBox bb;
        for (const auto& id : engine.AllObjectIds())
          if (const ON_Geometry* g = engine.Object(id)->Geometry(nullptr)) bb.Union(g->BoundingBox());
        if (!bb.IsValid()) bb = ON_BoundingBox(ON_3dPoint(-10, -10, -10), ON_3dPoint(10, 10, 10));
        view.m_vp.Extents(ON_PI / 8.0, bb);
      }
      v = view;
      return true;
    };
    engine.setActiveView = [&](const ON_3dmView& v) {
      view.m_vp = v.m_vp;
      viewSet = true;
      return true;
    };
    engine.setActiveCPlane = [&](const ON_Plane* p) {
      view.m_cplane.m_plane = p ? *p : ON_Plane::World_xy;
      return view.m_cplane.m_plane.IsValid();
    };
    if (!openPath.empty()) engine.currentPath = openPath;
    if (!script.empty()) {
      rc = engine.Run(script) ? 0 : 1;
    } else {
      std::string line;
      std::cout << "Comando: " << std::flush;
      while (std::getline(std::cin, line)) {
        if (line == "esci" || line == "exit") break;
        if (line == "?") {
          std::map<std::string, std::vector<const cad::CommandInfo*>> byCat;
          for (auto& [k, c] : engine.Commands()) byCat[c.category].push_back(&c);
          for (auto& [cat, list] : byCat) {
            std::cout << cat << ":\n";
            for (auto* c : list) std::cout << "  " << c->name << " - " << c->description << "\n";
          }
        } else {
          engine.Run(line);
        }
        std::cout << "Comando: " << std::flush;
      }
    }
    if (rc == 0 && !outPath.empty()) {
      {
        std::error_code ec;  // crea la cartella di uscita se manca
        const auto parent = std::filesystem::u8path(outPath).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, ec);
      }
      cad::SyncRenderMaterials(doc.Model());
      const cad::ModelSnapshot before = cad::TakeSnapshot(doc.Model());
      if (!doc.Save(outPath)) { std::cerr << "Salvataggio non riuscito: " << outPath << "\n"; rc = 1; }
      else std::cout << "Salvato " << outPath << "\n";
      if (rc == 0 && verify) {
        cad::Document3dm back;
        if (!back.Load(outPath)) { std::cerr << "Rilettura non riuscita: " << outPath << "\n"; rc = 1; }
        else {
          const cad::CompareResult r = cad::CompareSnapshots(before, cad::TakeSnapshot(back.Model()));
          std::cout << "Verifica: " << r.objectsCompared << " oggetti confrontati, " << r.differences.size() << " differenze\n";
          for (auto& d : r.differences) std::cout << "  - " << d << "\n";
          if (!r.Equal()) rc = 1;
        }
      }
    }
  }
  cad::ShutdownLibrary();
  return rc;
}
