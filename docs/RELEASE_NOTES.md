## Come provarlo

1. Scarica lo zip `rhino-windows-x64-….zip` qui sotto e estrailo in una cartella (per esempio `C:\RhinoCAD`).
2. Avvia **`rhino-ui.exe`**: è il programma con interfaccia grafica (4 viste, riga di comando in alto, barre a schede, pannelli Livelli e Proprietà).
3. Scrivi un comando nella riga di comando (per esempio `Box`, `Circle`, `Line`, `Dim`, `Text`) e premi Invio: poi clicca i punti nelle viste. `?` mostra l'elenco dei comandi.
4. File → Apri/Salva lavora con file `.3dm` in formato Rhino 8. Nella cartella `examples` ci sono script di esempio.

Non serve installare nulla: Qt, OpenCASCADE e il runtime di Visual C++ sono già nello zip.

## Cosa c'è

- 236 comandi di Rhino 8 su 303 inventariati (elenco in `docs/COMANDI.md`): curve, superfici, solidi e booleane (OpenCASCADE), trasformazioni, Trim/Extend/Fillet/Chamfer, blocchi e gruppi, quote e testi (Text, Dim, DimAligned, DimRotated, DimRadius, DimDiameter, DimAngle, Leader, Centermark), analisi.
- Osnap (Fine, Vicino, Punto, Medio, Centro, Intersezione, Perpendicolare, Quadrante, Nodo, Vertice), viste Top/Front/Right/Perspective e le altre dal menu del titolo, modalità Wireframe/Shaded/Ghosted/X-Ray.
- Pannello Proprietà modificabile (nome, livello, colore).
- Novità della 0.4: deformazioni **Twist, Taper, Bend, Flow** (anche su solidi, mesh e SubD); **Sweep2**, **NetworkSrf**, ExtrudeCrvTapered, ExtendSrf, Slab; Pull, LineSrfNormal, Tangent, Perpendicular; ArrayCrv, ArraySrf; ConvertToPolyline, FilletCorners, BlendCrv; mesh: booleane (MeshBooleanUnion/Difference/Intersection), Reduce, UnifyMeshNormals, FillMeshHoles; SubDCone, Crease/Uncrease, DimOrdinate. Corretto MeshBox (nella 0.3 creava facce intrecciate).
- Novità della 0.3: **punti di controllo** (F10 per mostrarli, F11 o Esc per nasconderli, si trascinano con il mouse); SubD visualizzati lisci (superficie limite e spigoli curvi come in Rhino); **Gumball** come in Rhino (frecce per spostare, archi per ruotare, quadratini per scalare; si attiva o spegne dalla barra di stato); superfici SrfPt, EdgeSrf, Patch, Plane3Pt, PlaneVertical, ExtrudeCrvToPoint, ExtrudeCrvAlongCrv, UntrimAll, ShrinkTrimmedSrf; solidi Pyramid, Wedge, Box3Pt, Sphere3Pt; SubD: sfera, cilindro, toro, piano, da mesh; curve Lines, RoundedRectangle, Parabola, Hyperbola, Bisector, AngleLine, CurveThroughPt, PolylineThroughPoints, Smooth, InsertKnot, CreateUVCrv; mesh Weld, MeshPolyline, MeshToNURB; Shear, SetPt, Align; SelColor, SelSmall, MatchProperties, SetUserText/GetUserText, Purge; Import, Export, IncrementalSave, SelPrev, MergeAllFaces, ExtractWireframe, ExtractIsocurve.
- Novità della 0.2: wireframe con isocurve e contorno apparente come Rhino; Circle3Pt, Arc3Pt, Rectangle3Pt, PolygonEdge, PolygonStar, EllipseDiameter; TruncatedCone, Ellipsoid, Tube; MeshBox/Sphere/Cylinder/Cone/Torus/Plane; Hatch e Dot; Isolate/Unisolate, LayerOn/Off/Lock/Unlock, CurrentLayer; Rotate3D, Scale1D, ScaleNU, Orient, Orient3Pt; SelClosedCrv, SelOpenCrv, SelDup, SelVisible; DupFaceBorder, ExtractControlPoints, PointGrid, Reverse, CloseCrv, Check.

## Limiti noti

È una versione di prova: nulla è ancora stato confrontato con Rhino 8 reale. Le differenze note sono in `docs/DIFFERENZE.md`, lo stato dei file `.3dm` in `docs/COMPATIBILITA.md`. Compilazione, test automatici e prova di avvio della finestra passati su Windows (GitHub Actions).
