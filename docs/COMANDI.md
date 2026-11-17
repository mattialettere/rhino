# Comandi di Rhino 8: inventario e stato

Elenco dei comandi di creazione, modifica e modellazione di Rhino 8 per Windows, raggruppati come nei menu di Rhino. I nomi sono quelli inglesi di Rhino (con prefisso `_` funzionano anche con Rhino in italiano).

| Stato | Significato |
|---|---|
| ✔ | Implementato e coperto dai test automatici. **Non ancora confrontato con Rhino 8 reale** |
| ⬜ | Da fare: realizzabile con openNURBS |
| 🧱 | Da fare: serve il motore geometrico (OpenCASCADE, ora integrato: si possono fare) |

I comandi si usano dalla riga di comando della finestra `rhino-ui` (come in Rhino: si scrive il nome, Invio o Spazio, poi si cliccano i punti) oppure dalla console `cad-console` con la sintassi delle macro di Rhino, per esempio `_Box 0,0,0 10,10 10` oppure `_SelCrv _ExtrudeCrv _Solid=_Yes 10`. Le differenze note rispetto a Rhino 8 sono in [DIFFERENZE.md](DIFFERENZE.md).

## Riepilogo

| Categoria | Fatti / totali |
|---|---|
| Punti | 3 / 9 |
| Curve: linee e polilinee | 2 / 11 |
| Curve: forme | 7 / 19 |
| Curve: forme libere | 6 / 18 |
| Curve da oggetti | 7 / 14 |
| Superfici | 9 / 29 |
| Solidi | 14 / 24 |
| SubD | 1 / 23 |
| Mesh | 1 / 19 |
| Trasformazioni | 9 / 30 |
| Modifica | 16 / 34 |
| Selezione | 11 / 19 |
| Blocchi | 3 / 6 |
| Livelli e proprieta' | 2 / 11 |
| Annotazioni | 9 / 14 |
| Analisi | 8 / 15 |
| File | 4 / 7 |
| **Totale** | **112 / 302** |

## Punti

| Comando | Stato |
|---|---|
| Point | ✔ |
| Points | ✔ |
| PointGrid | ⬜ |
| DivideCrv | 🧱 |
| Divide | ✔ |
| Measure | 🧱 |
| ExtractPt | ⬜ |
| ExtractControlPoints | ⬜ |
| MarkCurvature | 🧱 |

## Curve: linee e polilinee

| Comando | Stato |
|---|---|
| Line | ✔ |
| Lines | ⬜ |
| LineSrfNormal | 🧱 |
| Polyline | ✔ |
| PolylineThroughPoints | ⬜ |
| PolylineOnMesh | 🧱 |
| ConvertToPolyline | 🧱 |
| Tangent | 🧱 |
| Perpendicular | 🧱 |
| Bisector | ⬜ |
| AngleLine | ⬜ |

## Curve: forme

| Comando | Stato |
|---|---|
| Rectangle | ✔ |
| Rectangle3Pt | ⬜ |
| RoundedRectangle | ⬜ |
| Polygon | ✔ |
| PolygonStar | ⬜ |
| PolygonEdge | ⬜ |
| Circle | ✔ |
| Circle3Pt | ⬜ |
| CircleTangent | 🧱 |
| Arc | ✔ |
| Arc3Pt | ⬜ |
| ArcTangent | 🧱 |
| Ellipse | ✔ |
| EllipseDiameter | ⬜ |
| Parabola | ⬜ |
| Hyperbola | ⬜ |
| Spiral | ✔ |
| Helix | ✔ |
| ConicCurve | ⬜ |

## Curve: forme libere

| Comando | Stato |
|---|---|
| Curve | ✔ |
| InterpCrv | ✔ |
| InterpCrvOnSrf | 🧱 |
| HandleCurve | ⬜ |
| Sketch | ⬜ |
| CurveThroughPt | ⬜ |
| Blend | 🧱 |
| BlendCrv | 🧱 |
| Fillet | ✔ |
| FilletCorners | 🧱 |
| Chamfer | ✔ |
| Offset | ✔ |
| OffsetCrvOnSrf | 🧱 |
| Rebuild | ✔ |
| RebuildCrvNonUniform | 🧱 |
| Fair | 🧱 |
| Smooth | ⬜ |
| Simplify | 🧱 |

## Curve da oggetti

| Comando | Stato |
|---|---|
| Project | ✔ |
| Pull | 🧱 |
| DupEdge | ✔ |
| DupBorder | ✔ |
| DupFaceBorder | ⬜ |
| Contour | ✔ |
| Section | ✔ |
| Silhouette | 🧱 |
| Intersect | ✔ |
| CreateUVCrv | ⬜ |
| ExtractIsocurve | ⬜ |
| ExtractWireframe | ⬜ |
| ProjectToCPlane | ✔ |
| Make2D | 🧱 |

## Superfici

| Comando | Stato |
|---|---|
| Plane | ✔ |
| Plane3Pt | ⬜ |
| PlaneVertical | ⬜ |
| PlanarSrf | ✔ |
| SrfPt | ⬜ |
| EdgeSrf | 🧱 |
| Loft | ✔ |
| Sweep1 | ✔ |
| Sweep2 | 🧱 |
| NetworkSrf | 🧱 |
| Patch | 🧱 |
| Revolve | ✔ |
| RailRevolve | 🧱 |
| ExtrudeCrv | ✔ |
| ExtrudeCrvAlongCrv | 🧱 |
| ExtrudeCrvToPoint | ⬜ |
| ExtrudeCrvTapered | 🧱 |
| ExtrudeSrf | ✔ |
| Pipe | ✔ |
| OffsetSrf | ✔ |
| FilletSrf | 🧱 |
| ChamferSrf | 🧱 |
| BlendSrf | 🧱 |
| MatchSrf | 🧱 |
| ExtendSrf | 🧱 |
| ShrinkTrimmedSrf | 🧱 |
| UntrimAll | ⬜ |
| Heightfield | ⬜ |
| DevLoft | 🧱 |

## Solidi

| Comando | Stato |
|---|---|
| Box | ✔ |
| Box3Pt | ⬜ |
| Sphere | ✔ |
| Sphere3Pt | ⬜ |
| Cylinder | ✔ |
| Cone | ✔ |
| TruncatedCone | ⬜ |
| Torus | ✔ |
| Ellipsoid | ⬜ |
| Pyramid | ⬜ |
| Tube | ⬜ |
| Wedge | ⬜ |
| Cap | ✔ |
| BooleanUnion | ✔ |
| BooleanDifference | ✔ |
| BooleanIntersection | ✔ |
| BooleanSplit | ✔ |
| FilletEdge | ✔ |
| ChamferEdge | ✔ |
| Shell | ✔ |
| MergeAllFaces | ⬜ |
| MoveFace | 🧱 |
| ExtractSrf | ✔ |
| Slab | 🧱 |

## SubD

| Comando | Stato |
|---|---|
| SubDBox | ✔ |
| SubDSphere | ⬜ |
| SubDCylinder | ⬜ |
| SubDCone | ⬜ |
| SubDTorus | ⬜ |
| SubDPlane | ⬜ |
| SubDFromMesh | ⬜ |
| ToSubD | 🧱 |
| ToNURBS | 🧱 |
| SubDLoft | 🧱 |
| SubDSweep | 🧱 |
| SubDExtrude | 🧱 |
| Bevel | 🧱 |
| Bridge | 🧱 |
| Crease | ⬜ |
| Uncrease | ⬜ |
| InsertEdge | 🧱 |
| Stitch | 🧱 |
| Append | 🧱 |
| Fill | 🧱 |
| Reflect | ⬜ |
| MultiPipe | 🧱 |
| SubDDisplayToggle | ⬜ |

## Mesh

| Comando | Stato |
|---|---|
| Mesh | ✔ |
| MeshBox | ⬜ |
| MeshSphere | ⬜ |
| MeshCylinder | ⬜ |
| MeshCone | ⬜ |
| MeshTorus | ⬜ |
| MeshPlane | ⬜ |
| MeshPolyline | ⬜ |
| MeshPatch | 🧱 |
| QuadRemesh | 🧱 |
| Reduce | 🧱 |
| MeshBooleanUnion | 🧱 |
| MeshBooleanDifference | 🧱 |
| Weld | ⬜ |
| UnweldEdge | 🧱 |
| FillMeshHoles | 🧱 |
| UnifyMeshNormals | 🧱 |
| MeshToNURB | ⬜ |
| ExtractMeshPart | ⬜ |

## Trasformazioni

| Comando | Stato |
|---|---|
| Move | ✔ |
| Copy | ✔ |
| Rotate | ✔ |
| Rotate3D | ⬜ |
| Scale | ✔ |
| Scale1D | ⬜ |
| Scale2D | ✔ |
| ScaleNU | ⬜ |
| Mirror | ✔ |
| Orient | ⬜ |
| Orient3Pt | ⬜ |
| OrientOnSrf | 🧱 |
| Array | ✔ |
| ArrayLinear | ✔ |
| ArrayPolar | ✔ |
| ArrayCrv | 🧱 |
| ArraySrf | 🧱 |
| Align | ⬜ |
| SetPt | ⬜ |
| Shear | ⬜ |
| Twist | 🧱 |
| Bend | 🧱 |
| Taper | 🧱 |
| Flow | 🧱 |
| FlowAlongSrf | 🧱 |
| Splop | 🧱 |
| CageEdit | 🧱 |
| Smash | 🧱 |
| UnrollSrf | 🧱 |
| Squish | 🧱 |

## Modifica

| Comando | Stato |
|---|---|
| Delete | ✔ |
| Join | ✔ |
| Explode | ✔ |
| Trim | ✔ |
| Split | ✔ |
| Extend | ✔ |
| ExtendCrvOnSrf | 🧱 |
| Flip | ✔ |
| Dir | ⬜ |
| Reverse | ⬜ |
| Group | ✔ |
| Ungroup | ✔ |
| Hide | ✔ |
| Show | ✔ |
| Lock | ✔ |
| Unlock | ✔ |
| Undo | ✔ |
| Redo | ✔ |
| CloseCrv | ⬜ |
| Seam | 🧱 |
| ChangeDegree | ✔ |
| InsertKnot | ⬜ |
| RemoveKnot | ⬜ |
| InsertControlPoint | 🧱 |
| PointsOn | ⬜ |
| PointsOff | ⬜ |
| EditPtOn | ⬜ |
| MakePeriodic | 🧱 |
| Match | 🧱 |
| Fit | 🧱 |
| ConvertToBezier | ⬜ |
| Isolate | ⬜ |
| Unisolate | ⬜ |
| Purge | ⬜ |

## Selezione

| Comando | Stato |
|---|---|
| SelAll | ✔ |
| SelNone | ✔ |
| SelLast | ✔ |
| SelPrev | ⬜ |
| SelCrv | ✔ |
| SelPt | ✔ |
| SelSrf | ✔ |
| SelPolysrf | ✔ |
| SelMesh | ✔ |
| SelSubD | ✔ |
| SelLayer | ✔ |
| SelColor | ⬜ |
| SelClosedCrv | ⬜ |
| SelOpenCrv | ⬜ |
| SelDup | ⬜ |
| SelSmall | ⬜ |
| SelVisible | ⬜ |
| Invert | ✔ |
| SelectionFilter | ⬜ |

## Blocchi

| Comando | Stato |
|---|---|
| Block | ✔ |
| Insert | ✔ |
| ExplodeBlock | ✔ |
| BlockEdit | ⬜ |
| BlockManager | ⬜ |
| ReplaceBlock | ⬜ |

## Livelli e proprieta'

| Comando | Stato |
|---|---|
| Layer | ✔ |
| ChangeLayer | ✔ |
| CurrentLayer | ⬜ |
| LayerOff | ⬜ |
| LayerOn | ⬜ |
| LayerLock | ⬜ |
| LayerUnlock | ⬜ |
| Properties | ⬜ |
| SetObjectName | ⬜ |
| MatchProperties | ⬜ |
| SetUserText | ⬜ |

## Annotazioni

| Comando | Stato |
|---|---|
| Text | ✔ |
| TextObject | ⬜ |
| Dim | ✔ |
| DimAligned | ✔ |
| DimRotated | ✔ |
| DimAngle | ✔ |
| DimRadius | ✔ |
| DimDiameter | ✔ |
| Centermark | ✔ |
| DimOrdinate | ⬜ |
| Leader | ✔ |
| Dot | ⬜ |
| Hatch | 🧱 |
| DimStyle | ⬜ |

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
| EvaluatePt | ⬜ |
| BoundingBox | ✔ |
| CurvatureGraph | 🧱 |
| Zebra | 🧱 |
| Draft | 🧱 |
| ShowEdges | 🧱 |
| Check | ⬜ |
| SelBadObjects | ⬜ |

## File

| Comando | Stato |
|---|---|
| New | ✔ |
| Open | ✔ |
| Save | ✔ |
| SaveAs | ✔ |
| Import | ⬜ |
| Export | ⬜ |
| IncrementalSave | ⬜ |

Questo elenco copre i comandi di modellazione principali, non tutti gli oltre mille comandi di Rhino 8 (rendering, layout, scripting, Grasshopper restano fuori per ora).
