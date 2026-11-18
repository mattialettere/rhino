// Analisi di curvatura a falsi colori (CurvatureAnalysis di Rhino 8) e funzioni comuni
// alle analisi delle superfici. Le curvature principali sono calcolate in modo esatto sulle
// superfici (OpenCASCADE, nei nodi della mesh di analisi) e stimate per le mesh.
#pragma once
#include "opennurbs.h"
#include <string>
#include <vector>

namespace cad {

enum class CurvatureStyle { Gaussian, Mean, MinRadius, MaxRadius };

struct CurvatureData {
  ON_Mesh mesh;               // mesh di analisi (vertici non saldati tra le facce delle polisuperfici)
  std::vector<double> k1, k2;  // curvature principali con segno (k1 >= k2), una per vertice
  bool exact = false;          // true: dalla superficie; false: stima dalla mesh
};

// Superfici, polisuperfici ed estrusioni: curvatura esatta; mesh (o displayMesh per SubD):
// stima discreta dalle normali dei vertici. Restituisce false se l'oggetto non e' analizzabile.
bool ComputeCurvature(const ON_Geometry& g, double tolerance, CurvatureData& out, const ON_Mesh* displayMesh = nullptr);

double CurvatureValue(CurvatureStyle s, double k1, double k2);
CurvatureStyle ParseCurvatureStyle(const std::string& name, CurvatureStyle def = CurvatureStyle::Gaussian);
const char* CurvatureStyleName(CurvatureStyle s);       // nome inglese dell'opzione (Gaussian, Mean...)
const char* CurvatureStyleLabel(CurvatureStyle s);      // nome italiano

// Intervallo dei valori: autoRange = 5..95 percentile (come "Intervallo automatico", ignora i
// picchi locali), altrimenti minimo..massimo ("Intervallo massimo").
bool CurvatureRange(const std::vector<const CurvatureData*>& data, CurvatureStyle s, bool autoRange, double& lo, double& hi);

// Colore del valore: rosso -> verde -> blu. Gaussiana e media: rosso = valori alti;
// raggi: rosso = raggi piccoli (curvatura forte), come la legenda di Rhino.
void CurvatureColor(CurvatureStyle s, double value, double lo, double hi, float rgb[3]);

}  // namespace cad
