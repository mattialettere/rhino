# Comandi di Rhino 8: inventario e stato

Elenco dei comandi di creazione, modifica e modellazione di Rhino 8 per Windows, raggruppati come nei menu di Rhino. I nomi sono quelli inglesi di Rhino (con prefisso `_` funzionano anche con Rhino in italiano).

| Stato | Significato |
|---|---|
| ✔ | Implementato e coperto dai test automatici. **Non ancora confrontato con Rhino 8 reale** |
| ⬜ | Da fare: realizzabile con openNURBS |
| 🧱 | Da fare: serve il motore geometrico (OpenCASCADE, ora integrato: si possono fare) |

I comandi si usano dalla riga di comando della finestra `kurva3d` (come in Rhino: si scrive il nome, Invio o Spazio, poi si cliccano i punti) oppure dalla console `cad-console` con la sintassi delle macro di Rhino, per esempio `_Box 0,0,0 10,10 10` oppure `_SelCrv _ExtrudeCrv _Solid=_Yes 10`. Le differenze note rispetto a Rhino 8 sono in [DIFFERENZE.md](DIFFERENZE.md).

## Riepilogo

| Categoria | Fatti / totali |
|---|---|
| Punti | 9 / 9 |
| Curve: linee e polilinee | 11 / 11 |
| Curve: forme | 19 / 19 |
| Curve: forme libere | 18 / 18 |
| Curve da oggetti | 14 / 14 |
| Superfici | 29 / 29 |
| Solidi | 24 / 24 |
| SubD | 23 / 23 |
| Mesh | 19 / 19 |
| Trasformazioni | 30 / 30 |
| Modifica | 34 / 34 |
| Selezione | 19 / 19 |
| Blocchi | 6 / 6 |
| Livelli e proprieta' | 12 / 12 |
| Annotazioni | 14 / 14 |
| Analisi | 15 / 15 |
| File | 9 / 9 |
| Rendering (texture) | 5 / 5 |
| Rendering (luci e immagini) | 9 / 9 |
| Viste e sezioni | 7 / 7 |
| Tavole e stampa | 5 / 5 |
| Script | 3 / 3 |
| Storia | 5 / 5 |
| **Totale** | **339 / 339** |

## Punti

| Comando | Stato |
|---|---|
| Point | ✔ |
| Points | ✔ |
| PointGrid | ✔ |
| DivideCrv | ✔ |
| Divide | ✔ |
| Measure | ✔ |
| ExtractPt | ✔ |
| ExtractControlPoints | ✔ |
| MarkCurvature | ✔ |

## Curve: linee e polilinee

| Comando | Stato |
|---|---|
| Line | ✔ |
| Lines | ✔ |
| LineSrfNormal | ✔ |
| Polyline | ✔ |
| PolylineThroughPoints | ✔ |
| PolylineOnMesh | ✔ |
| ConvertToPolyline | ✔ |
| Tangent | ✔ |
| Perpendicular | ✔ |
| Bisector | ✔ |
| AngleLine | ✔ |

## Curve: forme

| Comando | Stato |
|---|---|
| Rectangle | ✔ |
| Rectangle3Pt | ✔ |
| RoundedRectangle | ✔ |
| Polygon | ✔ |
| PolygonStar | ✔ |
| PolygonEdge | ✔ |
| Circle | ✔ |
| Circle3Pt | ✔ |
| CircleTangent | ✔ |
| Arc | ✔ |
| Arc3Pt | ✔ |
| ArcTangent | ✔ |
| Ellipse | ✔ |
| EllipseDiameter | ✔ |
| Parabola | ✔ |
| Hyperbola | ✔ |
| Spiral | ✔ |
| Helix | ✔ |
| ConicCurve | ✔ |

## Curve: forme libere

| Comando | Stato |
|---|---|
| Curve | ✔ |
| InterpCrv | ✔ |
| InterpCrvOnSrf | ✔ |
| HandleCurve | ✔ |
| Sketch | ✔ |
| CurveThroughPt | ✔ |
| Blend | ✔ |
| BlendCrv | ✔ |
| Fillet | ✔ |
| FilletCorners | ✔ |
| Chamfer | ✔ |
| Offset | ✔ |
| OffsetCrvOnSrf | ✔ |
| Rebuild | ✔ |
| RebuildCrvNonUniform | ✔ |
| Fair | ✔ |
| Smooth | ✔ |
| Simplify | ✔ |

## Curve da oggetti

| Comando | Stato |
|---|---|
| Project | ✔ |
| Pull | ✔ |
| DupEdge | ✔ |
| DupBorder | ✔ |
| DupFaceBorder | ✔ |
| Contour | ✔ |
| Section | ✔ |
| Silhouette | ✔ |
| Intersect | ✔ |
| CreateUVCrv | ✔ |
| ExtractIsocurve | ✔ |
| ExtractWireframe | ✔ |
| ProjectToCPlane | ✔ |
| Make2D | ✔ |

## Superfici

| Comando | Stato |
|---|---|
| Plane | ✔ |
| Plane3Pt | ✔ |
| PlaneVertical | ✔ |
| PlanarSrf | ✔ |
| SrfPt | ✔ |
| EdgeSrf | ✔ |
| Loft | ✔ |
| Sweep1 | ✔ |
| Sweep2 | ✔ |
| NetworkSrf | ✔ |
| Patch | ✔ |
| Revolve | ✔ |
| RailRevolve | ✔ |
| ExtrudeCrv | ✔ |
| ExtrudeCrvAlongCrv | ✔ |
| ExtrudeCrvToPoint | ✔ |
| ExtrudeCrvTapered | ✔ |
| ExtrudeSrf | ✔ |
| Pipe | ✔ |
| OffsetSrf | ✔ |
| FilletSrf | ✔ |
| ChamferSrf | ✔ |
| BlendSrf | ✔ |
| MatchSrf | ✔ |
| ExtendSrf | ✔ |
| ShrinkTrimmedSrf | ✔ |
| UntrimAll | ✔ |
| Heightfield | ✔ |
| DevLoft | ✔ |
| MergeSrf | ✔ (0.13) |

## Solidi

| Comando | Stato |
|---|---|
| Box | ✔ |
| Box3Pt | ✔ |
| Sphere | ✔ |
| Sphere3Pt | ✔ |
| Cylinder | ✔ |
| Cone | ✔ |
| TruncatedCone | ✔ |
| Torus | ✔ |
| Ellipsoid | ✔ |
| Pyramid | ✔ |
| Tube | ✔ |
| Wedge | ✔ |
| Cap | ✔ |
| BooleanUnion | ✔ |
| BooleanDifference | ✔ |
| BooleanIntersection | ✔ |
| BooleanSplit | ✔ |
| FilletEdge | ✔ |
| FilletEdge raggio variabile (EndRadius=) | ✔ (0.13) |
| SplitFace | ✔ (0.13) |
| ChamferEdge | ✔ |
| Shell | ✔ |
| MergeAllFaces | ✔ |
| MoveFace | ✔ |
| ExtractSrf | ✔ |
| Slab | ✔ |

## SubD

| Comando | Stato |
|---|---|
| SubDBox | ✔ |
| SubDSphere | ✔ |
| SubDCylinder | ✔ |
| SubDCone | ✔ |
| SubDTorus | ✔ |
| SubDPlane | ✔ |
| SubDFromMesh | ✔ |
| ToSubD | ✔ |
| ToNURBS | ✔ |
| SubDLoft | ✔ |
| SubDSweep | ✔ |
| SubDExtrude | ✔ |
| Bevel | ✔ |
| Bridge | ✔ |
| Crease | ✔ |
| Uncrease | ✔ |
| InsertEdge | ✔ |
| Stitch | ✔ |
| Append | ✔ |
| Fill | ✔ |
| Reflect | ✔ |
| MultiPipe | ✔ |
| SubDDisplayToggle | ✔ |

## Mesh

| Comando | Stato |
|---|---|
| Mesh | ✔ |
| MeshBox | ✔ |
| MeshSphere | ✔ |
| MeshCylinder | ✔ |
| MeshCone | ✔ |
| MeshTorus | ✔ |
| MeshPlane | ✔ |
| MeshPolyline | ✔ |
| MeshPatch | ✔ |
| QuadRemesh | ✔ |
| Reduce | ✔ |
| MeshBooleanUnion | ✔ |
| MeshBooleanDifference | ✔ |
| Weld | ✔ |
| UnweldEdge | ✔ |
| FillMeshHoles | ✔ |
| ShrinkWrap | ✔ (0.13) |
| OffsetMesh | ✔ (0.13) |
| MeshRepair | ✔ (0.13) |
| CheckMesh | ✔ (0.13) |
| CullDegenerateMeshFaces | ✔ (0.13) |
| RebuildMeshNormals | ✔ (0.13) |
| ExtractRenderMesh | ✔ (0.13) |
| UnifyMeshNormals | ✔ |
| MeshToNURB | ✔ |
| ExtractMeshPart | ✔ |

## Trasformazioni

| Comando | Stato |
|---|---|
| Move | ✔ |
| Copy | ✔ |
| Rotate | ✔ |
| Rotate3D | ✔ |
| Scale | ✔ |
| Scale1D | ✔ |
| Scale2D | ✔ |
| ScaleNU | ✔ |
| Mirror | ✔ |
| Orient | ✔ |
| Orient3Pt | ✔ |
| OrientOnSrf | ✔ |
| Array | ✔ |
| ArrayLinear | ✔ |
| ArrayPolar | ✔ |
| ArrayCrv | ✔ |
| ArraySrf | ✔ |
| Align | ✔ |
| SetPt | ✔ |
| Shear | ✔ |
| Twist | ✔ |
| Bend | ✔ |
| Taper | ✔ |
| Flow | ✔ |
| FlowAlongSrf | ✔ |
| Splop | ✔ |
| CageEdit | ✔ |
| Smash | ✔ |
| UnrollSrf | ✔ |
| Squish | ✔ |

## Modifica

| Comando | Stato |
|---|---|
| Delete | ✔ |
| Join | ✔ |
| Explode | ✔ |
| Trim | ✔ |
| Split | ✔ |
| Extend | ✔ |
| ExtendCrvOnSrf | ✔ |
| Flip | ✔ |
| Dir | ✔ |
| Reverse | ✔ |
| Group | ✔ |
| Ungroup | ✔ |
| Hide | ✔ |
| Show | ✔ |
| Lock | ✔ |
| Unlock | ✔ |
| Undo | ✔ |
| Redo | ✔ |
| CloseCrv | ✔ |
| Seam | ✔ |
| ChangeDegree | ✔ |
| InsertKnot | ✔ |
| RemoveKnot | ✔ |
| InsertControlPoint | ✔ |
| PointsOn | ✔ |
| PointsOff | ✔ |
| EditPtOn | ✔ |
| MakePeriodic | ✔ |
| Match | ✔ |
| MatchCrv | ✔ (0.13) |
| Fit | ✔ |
| ConvertToBezier | ✔ |
| Isolate | ✔ |
| Unisolate | ✔ |
| Purge | ✔ |

## Selezione

| Comando | Stato |
|---|---|
| SelAll | ✔ |
| SelNone | ✔ |
| SelLast | ✔ |
| SelPrev | ✔ |
| SelCrv | ✔ |
| SelPt | ✔ |
| SelSrf | ✔ |
| SelPolysrf | ✔ |
| SelMesh | ✔ |
| SelSubD | ✔ |
| SelLayer | ✔ |
| SelColor | ✔ |
| SelClosedCrv | ✔ |
| SelOpenCrv | ✔ |
| SelDup | ✔ |
| SelSmall | ✔ |
| SelVisible | ✔ |
| Invert | ✔ |
| SelectionFilter | ✔ |

## Blocchi

| Comando | Stato |
|---|---|
| Block | ✔ |
| Insert | ✔ |
| ExplodeBlock | ✔ |
| BlockEdit | ✔ |
| BlockManager | ✔ |
| ReplaceBlock | ✔ |

## Livelli e proprieta'

| Comando | Stato |
|---|---|
| Layer | ✔ |
| ChangeLayer | ✔ |
| CurrentLayer | ✔ |
| LayerOff | ✔ |
| LayerOn | ✔ |
| LayerLock | ✔ |
| LayerUnlock | ✔ |
| Properties | ✔ |
| SetObjectName | ✔ |
| MatchProperties | ✔ |
| SetUserText | ✔ |
| SetObjectColor | ✔ |
| Linetypes | ✔ (pannello Tipi di linea; riga di comando: New, Edit, Rename, Delete, List) |
| SetObjectLinetype | ✔ (estensione: in Rhino si usa il pannello Proprietà) |
| SetObjectPrintWidth | ✔ (estensione: in Rhino si usa il pannello Proprietà) |
| SetObjectPrintColor | ✔ (estensione: in Rhino si usa il pannello Proprietà) |
| SetDocumentUserText | ✔ |
| GetDocumentUserText | ✔ |

## Annotazioni

| Comando | Stato |
|---|---|
| Text | ✔ |
| TextObject | ✔ |
| Dim | ✔ |
| DimAligned | ✔ |
| DimRotated | ✔ |
| DimAngle | ✔ |
| DimRadius | ✔ |
| DimDiameter | ✔ |
| Centermark | ✔ |
| DimOrdinate | ✔ |
| DimArc | ✔ (0.13) |
| DimCurveLength | ✔ (0.13) |
| Leader | ✔ |
| Dot | ✔ |
| Hatch | ✔ |
| DimStyle | ✔ |

## Analisi

| Comando | Stato |
|---|---|
| What | ✔ |
| Distance | ✔ |
| Length | ✔ |
| Area | ✔ |
| Volume | ✔ |
| Angle | ✔ |
| Radius | ✔ |
| EvaluatePt | ✔ |
| BoundingBox | ✔ |
| CurvatureGraph | ✔ |
| Zebra | ✔ |
| Draft | ✔ |
| ShowEdges | ✔ |
| CurvatureAnalysis | ✔ (0.12) |
| EMap | ✔ (0.12) |
| GCon | ✔ (0.12) |
| EdgeContinuity | ✔ (0.12) |
| PointDeviation | ✔ (0.12) |
| Check | ✔ |
| SelBadObjects | ✔ |

## File

| Comando | Stato |
|---|---|
| New | ✔ |
| Open | ✔ |
| Save | ✔ |
| SaveAs | ✔ |
| Import | ✔ (3dm, STEP, IGES, STL, OBJ, DXF, glTF/GLB, 3MF, PLY) |
| Export | ✔ (3dm, STEP, IGES, STL, OBJ con .mtl, DXF, glTF/GLB, 3MF, PLY) |
| IncrementalSave | ✔ |
| ViewCaptureToFile | ✔ |
| ViewCaptureToClipboard | ✔ |

## Rendering (texture)

| Comando | Stato |
|---|---|
| ApplyBoxMapping | ✔ |
| ApplyPlanarMapping | ✔ |
| ApplyCylindricalMapping | ✔ |
| ApplySphericalMapping | ✔ |
| ApplySurfaceMapping | ✔ |

## Rendering (luci e immagini)

| Comando | Stato |
|---|---|
| Render | ✔ (path tracer di Kurva3D, non Cycles: D-063) |
| PointLight | ✔ |
| DirectionalLight | ✔ |
| SpotLight | ✔ |
| RectangularLight | ✔ |
| Sun | ✔ (azimut e altezza manuali; niente luogo e ora) |
| Skylight | ✔ |
| GroundPlane | ✔ |
| SetDisplayMode | ✔ (Wireframe, Shaded, Ghosted, XRay, Rendered, Raytraced) |

Comandi in più di Kurva3D (non sono di Rhino 8): **SetMaterial** (materiale semplice sugli oggetti selezionati: colore, lucentezza, riflessione, trasparenza, IOR; in Rhino si fa dal pannello Materiali), **ApplyTexture** (materiale con un'immagine sugli oggetti, usato dal pannello Texture), **RemoveTexture** e **Collaborate** (lavoro in piu' persone sullo stesso progetto in rete locale, con chat e compiti per l'AI di ogni computer: D-067).

## Viste e sezioni

| Comando | Stato |
|---|---|
| CPlane | ✔ (World, Origin, Elevation, 3Point, Reset) |
| NamedView | ✔ (riga di comando, senza pannello) |
| NamedCPlane | ✔ (riga di comando, senza pannello) |
| ClippingPlane | ✔ |
| EnableClippingPlane | ✔ |
| DisableClippingPlane | ✔ |
| Snapshots | ✔ (formato non compatibile con Rhino, D-061) |

## Tavole e stampa

| Comando | Stato |
|---|---|
| Layout | ✔ (Size, Orientation, Width, Height, Details=0/1/4) |
| CopyLayout | ✔ |
| Layouts | ✔ (da riga di comando: List, Activate, Rename, Delete; in Rhino e' un pannello) |
| Detail | ✔ (Add, Scale, Projection, ZoomExtents, DisplayMode; D-062) |
| Print | ✔ (stampante o PDF; D-062) |

## Script

| Comando | Stato |
|---|---|
| RunPythonScript | ✔ (Python 3 con un sottoinsieme di rhinoscriptsyntax; anche codice in linea `-RunPythonScript (codice)`; D-064) |
| ScriptEditor | ✔ (editor con evidenziazione, Apri/Salva, Esegui F5; senza debugger, D-064) |
| Grasshopper | ✔ come **NodeEditor**: editor visuale a nodi di Kurva3D, con componenti propri; non legge i file .gh (D-066) |

## Storia

| Comando | Stato |
|---|---|
| History | ✔ (Record, Update, Lock; pulsante "Registra storia" nella barra di stato; D-065) |
| HistoryPurge | ✔ |
| SelChildren | ✔ |
| SelParents | ✔ |
| SelObjectsWithHistory | ✔ |
| Worksession | ✔ (Attach, Detach, DetachAll, List, Refresh; nessun file .rws, vedi D-073) |
| GumballAlignment | ✔ (World, CPlane, Object; anche dal menu del pulsante Gumball) |

Comandi che registrano la storia: Loft, ExtrudeCrv, ExtrudeSrf, ExtrudeCrvToPoint, ExtrudeCrvAlongCrv, ExtrudeCrvTapered, Revolve, RailRevolve, Sweep1, Sweep2, Pipe, Offset, OffsetSrf, PlanarSrf, EdgeSrf, NetworkSrf, Patch, Array, ArrayLinear, ArrayPolar, ArrayCrv, Mirror, Project, Pull, DupEdge, DupBorder, Contour, Intersect, Silhouette, BlendCrv, BlendSrf, FilletSrf, ExtractIsocurve, Smash, Twist, Bend, Taper, Flow (questi ultimi solo quando creano copie).


Questo elenco copre i comandi di modellazione principali, non tutti gli oltre mille comandi di Rhino 8 (Grasshopper resta fuori per ora).
