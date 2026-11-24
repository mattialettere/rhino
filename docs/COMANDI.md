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
| Curve: forme libere | 20 / 20 |
| Curve da oggetti | 15 / 15 |
| Superfici | 32 / 32 |
| Solidi | 31 / 31 |
| SubD | 25 / 25 |
| Mesh | 29 / 29 |
| Trasformazioni | 35 / 35 |
| Modifica | 35 / 35 |
| Selezione | 20 / 20 |
| Blocchi | 6 / 6 |
| Livelli e proprieta' | 26 / 26 |
| Annotazioni | 17 / 17 |
| Analisi | 20 / 20 |
| File | 9 / 9 |
| Rendering (texture) | 5 / 5 |
| Rendering (luci e immagini) | 13 / 13 |
| Viste e sezioni | 9 / 9 |
| Tavole e stampa | 5 / 5 |
| Script | 3 / 3 |
| Storia | 7 / 7 |
| **Totale** | **401 / 401** |

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
| Fillet | ✔ (linee, archi e curve NURBS complanari) |
| FilletCorners | ✔ |
| Chamfer | ✔ (linee, archi e curve NURBS complanari) |
| Offset | ✔ |
| OffsetCrvOnSrf | ✔ |
| Rebuild | ✔ |
| RebuildCrvNonUniform | ✔ |
| Fair | ✔ |
| Smooth | ✔ |
| Simplify | ✔ |
| InsertKink | ✔ (dopo il confronto con Rhino 8) |
| SoftEditCrv | ✔ (Length=, FixEnds; attenuazione lungo la curva) |

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
| IntersectTwoSets | ✔ (secondo gruppo scelto con un clic per oggetto, D-076) |

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
| SoftEditSrf | ✔ (UDistance=, VDistance= in frazione del dominio, FixEdges) |
| VariableFilletSrf | ✔ (raggio lineare da Radius= a EndRadius=, D-076) |

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
| CreateSolid | ✔ (OpenCASCADE: cucitura o celle chiuse; Cells=Yes) |
| SplitEdge | ✔ |
| JoinEdge | ✔ (cucitura con tolleranza pari alla distanza, D-076) |
| UnjoinEdge | ✔ |
| RebuildEdges | ✔ |

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
| ToNURBS | ✔ (anche SubD: superfici approssimate entro la tolleranza, D-051) |
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
| Inset | ✔ (una faccia alla volta, D-076) |
| Slide | ✔ (spigolo singolo, Amount=) |

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
| MeshSplit | ✔ (fino a 20000 facce, D-076) |
| MeshTrim | ✔ |
| MeshFromLines | ✔ |

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
| SoftMove | ✔ (Radius=) |
| Stretch | ✔ |
| Maelstrom | ✔ |
| MoveUVN | ✔ (punto di controllo scelto con un clic, U=, V=, N=) |
| Cage | ✔ |

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
| NamedSelections | ✔ (riga di comando e pannello Selezioni con nome) |

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
| Units | ✔ (D-074) |
| MatchProperties | ✔ |
| SetUserText | ✔ |
| SetObjectColor | ✔ |
| Linetypes | ✔ (pannello Tipi di linea; riga di comando: New, Edit, Rename, Delete, List) |
| SetObjectLinetype | ✔ (estensione: in Rhino si usa il pannello Proprietà) |
| SetObjectPrintWidth | ✔ (estensione: in Rhino si usa il pannello Proprietà) |
| SetObjectPrintColor | ✔ (estensione: in Rhino si usa il pannello Proprietà) |
| SetDocumentUserText | ✔ |
| GetDocumentUserText | ✔ |
| LayerStateManager | ✔ (riga di comando e pannello Stati dei livelli, salvato nel testo del documento, D-076) |
| DocumentProperties | ✔ (finestra Opzioni; -DocumentProperties con le tolleranze) |
| Options | ✔ (unità, tolleranze, griglia, osnap, alias, aspetto, D-080) |
| Alias | ✔ (D-080) |
| CommandHistory | ✔ (F2) |
| PrintDisplay | ✔ (colore e spessore di stampa nelle viste) |
| SmartTrack | ✔ (D-080) |

## Annotazioni

| Comando | Stato |
|---|---|
| Text | ✔ |
| TextProperties | ✔ (D-074: in Rhino 8 si usa il pannello Proprieta', qui anche come comando) |
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
| EdgeContinuity | ✔ (0.12; grafico a pettine nelle viste) |
| PointDeviation | ✔ (0.12; punti colorati e linee nelle viste) |
| Check | ✔ |
| SelBadObjects | ✔ |

## File

| Comando | Stato |
|---|---|
| New | ✔ |
| Open | ✔ |
| Save | ✔ |
| SaveAs | ✔ |
| Import | ✔ (3dm, STEP, IGES, STL, OBJ, DXF, glTF/GLB, 3MF, PLY, FBX, Collada, 3DS, DirectX e altri con Assimp) |
| Export | ✔ (3dm, STEP, IGES, STL, OBJ con .mtl, DXF, glTF/GLB, 3MF, PLY, FBX, Collada, 3DS, DirectX) |
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
| Displacement | ✔ (mesh di rendering, D-078) |
| EdgeSoftening | ✔ (normali ammorbidite, smusso con Chamfer=Yes, D-078) |
| Shutlining | ✔ (profilo tondo o quadrato, D-078) |
| Decal | ✔ (solo proiezione planare, D-078) |

Comandi in più di Kurva3D (non sono di Rhino 8): **SetMaterial** (materiale sugli oggetti selezionati: colore, lucentezza, riflessione, trasparenza, IOR, oppure PBR=Yes con i parametri Physically Based; in Rhino si fa dal pannello Materiali, qui anche da Render > Materiale PBR), **ApplyTexture** (materiale con un'immagine sugli oggetti, usato dal pannello Texture), **RemoveTexture** e **Collaborate** (lavoro in piu' persone sullo stesso progetto in rete locale, con chat e compiti per l'AI di ogni computer: D-067).

## Viste e sezioni

| Comando | Stato |
|---|---|
| CPlane | ✔ (World, Origin, Elevation, 3Point, Reset) |
| NamedView | ✔ (riga di comando e pannello Viste con nome) |
| NamedCPlane | ✔ (riga di comando e pannello CPlane con nome) |
| ClippingPlane | ✔ |
| EnableClippingPlane | ✔ |
| DisableClippingPlane | ✔ |
| Snapshots | ✔ (formato non compatibile con Rhino, D-061) |
| SectionStyle | ✔ (Fill, Pattern, Color: usati da ClippingDrawings, D-079) |
| ClippingDrawings | ✔ (D-079) |

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
