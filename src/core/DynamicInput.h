#pragma once
// Input dinamico: misure accanto al cursore, modificabili digitando (come il
// "Dynamic Input" di AutoCAD) sopra ai vincoli di distanza e angolo di Rhino 8
// ("10" Invio = distanza, "<45" Invio = angolo). Solo matematica, senza Qt:
// l'interfaccia (Viewport/MainWindow) la usa e il test la verifica.
#include "opennurbs.h"
#include <string>
#include <vector>

namespace cad {

// Polar: lunghezza e angolo dall'ultimo punto (Line, Polyline, Circle...).
// Delta: larghezza e altezza nel CPlane (secondo angolo di Rectangle, Box, Plane).
enum class DynMode { Polar, Delta };

// Comportamento di un numero seguito da Invio:
//  Rhino   = vincolo di distanza (poi si clicca la direzione), come Rhino 8
//  AutoCAD = il punto viene messo subito a quella distanza nella direzione del cursore
enum class DynEnter { Rhino, AutoCAD };

struct DynFields {
  // campo 0: lunghezza (Polar) o larghezza dX (Delta); campo 1: angolo in gradi o altezza dY
  bool locked[2] = {false, false};
  double value[2] = {0, 0};
  int active = 0;  // campo in cui va il testo digitato (Tab passa all'altro)
  bool AnyLocked() const { return locked[0] || locked[1]; }
  bool AllLocked() const { return locked[0] && locked[1]; }
  void Clear() { *this = DynFields(); }
};

struct DynOptions {
  bool polarTracking = true;   // aggancio alle direzioni multiple di polarStep
  double polarStep = 15.0;     // gradi
  double polarTolerance = 3.0; // gradi entro cui il cursore viene agganciato
  bool ortho = false;          // Orto: solo multipli di 90 gradi (vince sul polare)
};

struct DynResult {
  ON_3dPoint point;            // punto risultante (sul piano parallelo al CPlane che passa per l'ancora)
  double length = 0, angle = 0;  // angolo in gradi nel CPlane, 0..360
  double dx = 0, dy = 0;         // componenti nel CPlane
  bool tracked = false;        // agganciato a una direzione polare (o Orto)
  double trackAngle = 0;       // gradi
};

// Punto vincolato a partire dal cursore. snapped = il cursore e' su un osnap:
// senza vincoli il punto resta esattamente quello dell'osnap.
DynResult SolveDynamicPoint(const ON_Plane& cplane, const ON_3dPoint& anchor, const ON_3dPoint& cursor,
                            DynMode mode, const DynFields& f, const DynOptions& o, bool snapped = false);

// Cosa e' stato digitato nella riga di comando durante una richiesta di punto.
//  Number  = "25"  (valore per il campo attivo)
//  Angle   = "<45" (vincolo di angolo, come Rhino)
//  Other   = coordinate ("10,5", "@3,4", "r10<30"...), opzioni o comandi: passano al comando
//  Empty   = Invio senza testo
enum class DynTyped { Empty, Number, Angle, Other };
DynTyped ParseDynamicText(const std::string& text, double& value);

// Modalita' delle misure per il comando in corso (nome inglese del comando).
DynMode DynamicModeFor(const std::string& commandName);
// Etichetta del primo campo in modalita' Polar ("L" o "R" per i cerchi).
const char* DynamicLengthLabel(const std::string& commandName);

// Punto come testo per la riga di comando (coordinate assolute, 12 cifre significative).
std::string PointToken(const ON_3dPoint& p);

// Quote "a lato" generate dopo il disegno: una quota allineata per ogni segmento
// di linee e polilinee, spostata di offset verso l'esterno (curve chiuse) oppure
// sopra/a sinistra (curve aperte). Restituisce i comandi DimAligned da eseguire.
std::vector<std::string> AutoDimensionCommands(const ON_Geometry* g, double offset);
// I comandi dopo i quali si generano le quote automatiche.
bool AutoDimensionCommand(const std::string& commandName);

}  // namespace cad
