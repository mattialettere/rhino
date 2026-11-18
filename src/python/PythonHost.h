// Script Python (CPython incorporato) con un sottoinsieme di rhinoscriptsyntax.
#pragma once
#include "opennurbs.h"
#include <functional>
#include <string>

namespace cad {

class Engine;

// true se il programma e' compilato con Python
bool PythonAvailable();
// cartella del programma: li' si cerca la distribuzione "embeddable" (pythonXY.zip)
void SetPythonProgramDir(const std::string& dir);
// Esegue codice Python; l'output di print() va in output, l'errore (traccia) in err.
bool RunPython(Engine& e, const std::string& code, const std::string& filename, std::string& output, std::string& err);
// Area/volume e baricentro di un oggetto (fornita dal motore geometrico, se presente)
extern std::function<bool(Engine&, const ON_UUID& id, bool volume, double& value, ON_3dPoint& centroid)> MassPropertiesFn;
// Comando RunPythonScript
void RegisterPythonCommands(Engine& e);

}  // namespace cad
