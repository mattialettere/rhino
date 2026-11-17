// cad-console: riga di comando in stile Rhino 8.
//   cad-console                  sessione interattiva ("Comando:")
//   cad-console script.txt       esegue uno script di comandi
//   cad-console -o out.3dm ...   salva il risultato alla fine
// Comandi speciali della console: "?" elenca i comandi, "esci" termina.
#include "commands/Engine.h"
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

int main(int argc, char** argv) {
  cad::InitLibrary();
  int rc = 0;
  {
    std::string script, outPath, openPath;
    for (int i = 1; i < argc; i++) {
      std::string a = argv[i];
      if (a == "-o" && i + 1 < argc) outPath = argv[++i];
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
      if (!doc.Save(outPath)) { std::cerr << "Salvataggio non riuscito: " << outPath << "\n"; rc = 1; }
      else std::cout << "Salvato " << outPath << "\n";
    }
  }
  cad::ShutdownLibrary();
  return rc;
}
