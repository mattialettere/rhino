## Come provarlo

1. Scarica lo zip `kurva3d-windows-x64-….zip` qui sotto e estrailo in una cartella (per esempio `C:\Kurva3D`).
2. Avvia **`kurva3d.exe`**: è il programma con interfaccia grafica (4 viste, riga di comando in alto, barre a schede, pannelli Livelli e Proprietà).
3. Scrivi un comando nella riga di comando (per esempio `Box`, `Circle`, `Line`, `Dim`, `Text`) e premi Invio: poi clicca i punti nelle viste. `?` mostra l'elenco dei comandi.
4. File → Apri/Salva lavora con file `.3dm` in formato Rhino 8. Nella cartella `examples` ci sono script di esempio.

Non serve installare nulla: Qt, OpenCASCADE e il runtime di Visual C++ sono già nello zip.

## Cosa c'è

- 258 comandi di Rhino 8 su 303 inventariati (elenco in `docs/COMANDI.md`): curve, superfici, solidi e booleane (OpenCASCADE), trasformazioni, Trim/Extend/Fillet/Chamfer, blocchi e gruppi, quote e testi (Text, Dim, DimAligned, DimRotated, DimRadius, DimDiameter, DimAngle, Leader, Centermark), analisi.
- Osnap (Fine, Vicino, Punto, Medio, Centro, Intersezione, Perpendicolare, Quadrante, Nodo, Vertice), viste Top/Front/Right/Perspective e le altre dal menu del titolo, modalità Wireframe/Shaded/Ghosted/X-Ray.
- Pannello Proprietà modificabile (nome, livello, colore).
- Novità della 0.5.1: il programma si chiama **Kurva3D** (`kurva3d.exe`). Nuovo menu **?** con *Informazioni su Kurva3D* (versione e librerie usate); nello zip ci sono `THIRD_PARTY_NOTICES.txt` e la cartella `licenses` con le licenze di openNURBS, OpenCASCADE, Qt e delle altre librerie.
- Novità della 0.5: **Make2D** (disegno 2D con linee nascoste), **Join** anche per superfici (diventano polisuperfici o solidi) e mesh, MoveFace, FlowAlongSrf, OrientOnSrf, RailRevolve, Match, SimplifyCrv, FitCrv, InterpCrvOnSrf, CircleTangent, ArcTangent, PolylineOnMesh; mesh: ExtractMeshPart, Unweld, UnweldEdge; ToSubD; blocchi: ReplaceBlock, RenameBlock e la finestra **Gestione blocchi**; **filtro di selezione** (pulsante Filtro nella barra di stato); analisi **Zebra**, angolo di sformo, grafico di curvatura, spigoli nudi (menu Analizza); solidi ombreggiati con spigoli netti come in Rhino.
- Novità della 0.4: deformazioni **Twist, Taper, Bend, Flow** (anche su solidi, mesh e SubD); **Sweep2**, **NetworkSrf**, ExtrudeCrvTapered, ExtendSrf, Slab; Pull, LineSrfNormal, Tangent, Perpendicular; ArrayCrv, ArraySrf; ConvertToPolyline, FilletCorners, BlendCrv; mesh: booleane (MeshBooleanUnion/Difference/Intersection), Reduce, UnifyMeshNormals, FillMeshHoles; SubDCone, Crease/Uncrease, DimOrdinate. Corretto MeshBox (nella 0.3 creava facce intrecciate).
- Novità della 0.3: **punti di controllo** (F10 per mostrarli, F11 o Esc per nasconderli, si trascinano con il mouse); SubD visualizzati lisci (superficie limite e spigoli curvi come in Rhino); **Gumball** come in Rhino (frecce per spostare, archi per ruotare, quadratini per scalare; si attiva o spegne dalla barra di stato); superfici SrfPt, EdgeSrf, Patch, Plane3Pt, PlaneVertical, ExtrudeCrvToPoint, ExtrudeCrvAlongCrv, UntrimAll, ShrinkTrimmedSrf; solidi Pyramid, Wedge, Box3Pt, Sphere3Pt; SubD: sfera, cilindro, toro, piano, da mesh; curve Lines, RoundedRectangle, Parabola, Hyperbola, Bisector, AngleLine, CurveThroughPt, PolylineThroughPoints, Smooth, InsertKnot, CreateUVCrv; mesh Weld, MeshPolyline, MeshToNURB; Shear, SetPt, Align; SelColor, SelSmall, MatchProperties, SetUserText/GetUserText, Purge; Import, Export, IncrementalSave, SelPrev, MergeAllFaces, ExtractWireframe, ExtractIsocurve.
- Novità della 0.2: wireframe con isocurve e contorno apparente come Rhino; Circle3Pt, Arc3Pt, Rectangle3Pt, PolygonEdge, PolygonStar, EllipseDiameter; TruncatedCone, Ellipsoid, Tube; MeshBox/Sphere/Cylinder/Cone/Torus/Plane; Hatch e Dot; Isolate/Unisolate, LayerOn/Off/Lock/Unlock, CurrentLayer; Rotate3D, Scale1D, ScaleNU, Orient, Orient3Pt; SelClosedCrv, SelOpenCrv, SelDup, SelVisible; DupFaceBorder, ExtractControlPoints, PointGrid, Reverse, CloseCrv, Check.

## Limiti noti

È una versione di prova: nulla è ancora stato confrontato con Rhino 8 reale. Le differenze note sono in `docs/DIFFERENZE.md`, lo stato dei file `.3dm` in `docs/COMPATIBILITA.md`. Compilazione, test automatici e prova di avvio della finestra passati su Windows (GitHub Actions).
