// Nomi italiani dei comandi (alias dei nomi inglesi di Rhino 8). Come in Rhino localizzato:
// il nome senza "_" puo' essere quello italiano, quello con "_" e' sempre quello inglese.
// Gli script e la cronologia usano sempre il nome inglese.
#include "commands/Engine.h"

namespace cad {

const std::vector<std::pair<const char*, const char*>>& ItalianCommandNames() {
  static const std::vector<std::pair<const char*, const char*>> names = {
      // curve
      {"Linea", "Line"}, {"Linee", "Lines"}, {"Polilinea", "Polyline"}, {"Cerchio", "Circle"}, {"Arco", "Arc"},
      {"Ellisse", "Ellipse"}, {"Rettangolo", "Rectangle"}, {"Poligono", "Polygon"}, {"Curva", "Curve"},
      {"CurvaInterpolata", "InterpCrv"}, {"Elica", "Helix"}, {"Spirale", "Spiral"}, {"Punto", "Point"}, {"Punti", "Points"},
      {"Raccordo", "Fillet"}, {"Smusso", "Chamfer"}, {"Raccorda", "Match"}, {"RaccordaCurva", "MatchCrv"},
      {"Ricostruisci", "Rebuild"}, {"CambiaGrado", "ChangeDegree"}, {"Dividi", "Split"}, {"Taglia", "Trim"},
      {"Estendi", "Extend"}, {"Unisci", "Join"}, {"Esplodi", "Explode"}, {"Inverti", "Flip"}, {"Direzione", "Dir"},
      {"Proietta", "Project"}, {"Tira", "Pull"}, {"Interseca", "Intersect"}, {"Contorno", "Contour"},
      {"Sezione", "Section"}, {"DuplicaBordo", "DupBorder"}, {"DuplicaSpigolo", "DupEdge"}, {"Disegno2D", "Make2D"},
      {"Suddividi", "Divide"}, {"Misura", "Measure"}, {"Semplifica", "SimplifyCrv"},
      // superfici e solidi
      {"Piano", "Plane"}, {"SuperficiePlanare", "PlanarSrf"}, {"EstrudiCurva", "ExtrudeCrv"}, {"EstrudiSuperficie", "ExtrudeSrf"},
      {"Rivoluzione", "Revolve"}, {"RivoluzioneSuBinario", "RailRevolve"}, {"Loft", "Loft"}, {"Sweep1Binario", "Sweep1"},
      {"Sweep2Binari", "Sweep2"}, {"ReteDiCurve", "NetworkSrf"}, {"SuperficieDaPunti", "SrfPt"},
      {"SuperficieDaBordi", "EdgeSrf"}, {"Toppa", "Patch"}, {"OffsetSuperficie", "OffsetSrf"}, {"UnisciSuperfici", "MergeSrf"},
      {"EstendiSuperficie", "ExtendSrf"}, {"RaccordoSuperfici", "FilletSrf"}, {"RaccordaSuperficie", "MatchSrf"},
      {"Parallelepipedo", "Box"}, {"Sfera", "Sphere"}, {"Cilindro", "Cylinder"}, {"Cono", "Cone"}, {"Toro", "Torus"},
      {"Tubo", "Pipe"}, {"Ellissoide", "Ellipsoid"}, {"Piramide", "Pyramid"}, {"Chiudi", "Cap"}, {"Svuota", "Shell"},
      {"UnioneBooleana", "BooleanUnion"}, {"DifferenzaBooleana", "BooleanDifference"},
      {"IntersezioneBooleana", "BooleanIntersection"}, {"DivisioneBooleana", "BooleanSplit"},
      {"RaccordaSpigoli", "FilletEdge"}, {"SmussaSpigoli", "ChamferEdge"}, {"DividiFaccia", "SplitFace"},
      {"EstraiSuperficie", "ExtractSrf"}, {"SpostaFaccia", "MoveFace"}, {"UnisciFacce", "MergeAllFaces"},
      // trasformazioni
      {"Sposta", "Move"}, {"Copia", "Copy"}, {"Ruota", "Rotate"}, {"Ruota3D", "Rotate3D"}, {"Scala", "Scale"},
      {"Scala1D", "Scale1D"}, {"Scala2D", "Scale2D"}, {"Specchia", "Mirror"}, {"Orienta", "Orient"},
      {"SerieRettangolare", "Array"}, {"SerieLineare", "ArrayLinear"}, {"SeriePolare", "ArrayPolar"},
      {"SerieLungoCurva", "ArrayCrv"}, {"Inclina", "Shear"}, {"Torci", "Twist"}, {"Rastrema", "Taper"}, {"Piega", "Bend"},
      {"Allinea", "Align"}, {"Scorri", "Flow"}, {"Offset", "Offset"},
      // modifica e selezione
      {"Annulla", "Undo"}, {"Ripeti", "Redo"}, {"Elimina", "Delete"}, {"Cancella", "Delete"}, {"Raggruppa", "Group"},
      {"Separa", "Ungroup"}, {"Nascondi", "Hide"}, {"Mostra", "Show"}, {"Blocca", "Lock"}, {"Sblocca", "Unlock"},
      {"SelezionaTutto", "SelAll"}, {"Deseleziona", "SelNone"}, {"SelezionaUltimo", "SelLast"},
      {"SelezionaCurve", "SelCrv"}, {"SelezionaSuperfici", "SelSrf"}, {"SelezionaPolisuperfici", "SelPolysrf"},
      {"SelezionaMesh", "SelMesh"}, {"SelezionaPunti", "SelPt"}, {"InvertiSelezione", "Invert"},
      {"PuntiOn", "PointsOn"}, {"PuntiOff", "PointsOff"},
      // mesh e SubD
      {"MeshDaNurbs", "Mesh"}, {"Salda", "Weld"}, {"ChiudiFori", "FillMeshHoles"}, {"RiparaMesh", "MeshRepair"},
      {"ControllaMesh", "CheckMesh"}, {"OffsetMesh", "OffsetMesh"}, {"Avvolgi", "ShrinkWrap"},
      {"RiduciMesh", "ReduceMesh"}, {"EstraiMeshDiRendering", "ExtractRenderMesh"},
      // annotazioni
      {"Testo", "Text"}, {"Quota", "Dim"}, {"QuotaAllineata", "DimAligned"}, {"QuotaRuotata", "DimRotated"},
      {"QuotaRaggio", "DimRadius"}, {"QuotaDiametro", "DimDiameter"}, {"QuotaAngolo", "DimAngle"},
      {"QuotaArco", "DimArc"}, {"QuotaLunghezzaCurva", "DimCurveLength"}, {"QuotaCoordinata", "DimOrdinate"},
      {"Direttrice", "Leader"}, {"SegnoDelCentro", "Centermark"}, {"Retino", "Hatch"},
      // blocchi, livelli, documento
      {"Blocco", "Block"}, {"Unita", "Units"}, {"Inserisci", "Insert"}, {"EsplodiBlocco", "ExplodeBlock"}, {"Livello", "Layer"},
      {"TipiDiLinea", "Linetypes"}, {"Materiale", "SetMaterial"}, {"Proprieta", "Properties"}, {"Apri", "Open"},
      {"Salva", "Save"}, {"SalvaConNome", "SaveAs"}, {"Importa", "Import"}, {"Esporta", "Export"}, {"Nuovo", "New"},
      {"Stampa", "Print"}, {"SessioneDiLavoro", "Worksession"},
      // analisi
      {"Lunghezza", "Length"}, {"Distanza", "Distance"}, {"Angolo", "Angle"}, {"Raggio", "Radius"},
      {"Volume", "Volume"}, {"CosE", "What"}, {"Ingombro", "BoundingBox"}, {"SegnaCurvatura", "MarkCurvature"},
      {"AnalisiCurvatura", "CurvatureAnalysis"}, {"ContinuitaCurve", "GCon"}, {"ContinuitaSpigoli", "EdgeContinuity"},
      {"DeviazionePunti", "PointDeviation"}, {"Controlla", "Check"},
      // viste
      {"VistaConNome", "NamedView"}, {"PianoDiCostruzione", "CPlane"}, {"PianoDiSezione", "ClippingPlane"},
      {"Istantanee", "Snapshots"}, {"Tavola", "Layout"}, {"Renderizza", "Render"},
  };
  return names;
}

}  // namespace cad
