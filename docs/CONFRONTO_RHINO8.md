# Kurva3D a confronto con Rhino 8 per Windows

Stato al 23/11/2026, aggiornato dopo le PR #21, #22 e #23 (funzioni mancanti aggiunte; versione pubblicata v0.14.2, nessuna nuova release).
Fonti: codice in `src/`, [COMANDI.md](COMANDI.md), [COMPATIBILITA.md](COMPATIBILITA.md),
[DIFFERENZE.md](DIFFERENZE.md), [VENDITA.md](VENDITA.md), risultati della CI di GitHub.

## In breve

**Kurva3D non è ancora un sostituto di Rhino 8 né un prodotto pronto da vendere.**

- Copre i comandi di modellazione più usati: 401 comandi censiti in COMANDI.md, contro
  gli oltre mille di Rhino 8. Molti funzionano in modo semplificato (vedi DIFFERENZE.md).
- Le funzioni segnate come mancanti nella prima versione di questo confronto sono state
  aggiunte (PR #21, #22, #23), tranne DWG, RhinoCommon/C#, i file di Grasshopper, il
  render su GPU e un sistema di plug-in. Sono tutte provate solo dai test di Kurva3D.
- **Niente è ancora verificato con Rhino 8 reale.** Tutti i test confrontano Kurva3D con
  se stesso e con openNURBS. Nessun file salvato da Rhino 8 è mai entrato nei test
  (`tests/rhino8_files/` è vuota) e nessun file di Kurva3D è stato riaperto in Rhino 8.
  I 14 file di prova in `rhino8-prova/` aspettano ancora il controllo in Rhino 8.
- L'unico uso reale provato è l'apertura, la modifica e il salvataggio dei DXF di ingombro
  (LP403232, LPR443535), verificato il 23/11/2026, ma solo tra Kurva3D ed ezdxf, non in
  AutoCAD né in Rhino 8.
- Per uso personale può affiancare Rhino 8 su lavori semplici. Per sostituirlo mancano
  le verifiche incrociate e diverse funzioni importanti (elenco sotto).

## Legenda

| Simbolo | Significato |
|---|---|
| ✅ | Presente **e verificato con Rhino 8 reale** (oggi: nessuna voce) |
| 🟢 | Presente e provato su file o uso reale, ma **non** con Rhino 8 |
| 🟡 | Presente, provato solo dai test automatici di Kurva3D (non verificato con Rhino 8) |
| 🟠 | Presente in forma parziale o semplificata (differenze note) |
| 🔴 | Mancante |

## Confronto per categoria

| Categoria | Kurva3D | Stato | Cosa manca o è diverso rispetto a Rhino 8 |
|---|---|---|---|
| **Curve** | 71 comandi (punti, linee, forme, forme libere, curve da oggetti), osnap Fine, Vicino, Punto, Medio, Centro, Intersezione, Perpendicolare, Quadrante, Nodo, Vertice | 🟡 / 🟠 | Fillet e Chamfer tra curve qualsiasi complanari che si toccano, senza le opzioni Join/Trim (D-024); Trim/Extend senza ApparentIntersections e Type (D-025); osnap Tangente sui wire disegnati e SmartTrack solo lungo gli assi del CPlane (D-080); valori predefiniti presunti (D-011) |
| **Superfici** | 29 comandi (Loft, Sweep1/2, NetworkSrf, Patch, EdgeSrf, BlendSrf, MatchSrf, FilletSrf, MergeSrf, ExtendSrf, ...) | 🟡 / 🟠 | Calcolate con OpenCASCADE: forma corretta ma punti di controllo, nodi e parametrizzazione diversi da Rhino (D-017, D-033, D-041, D-042, D-044, D-049); VariableFilletSrf solo con raggio lineare (D-076) |
| **Solidi** | 24 comandi (primitive, booleane, FilletEdge, ChamferEdge, Shell, Cap, ShrinkWrap, ...) | 🟡 / 🟠 | Booleane con ruoli dati dall'ordine di selezione (D-015); raggio variabile solo lineare (D-016, D-071); CreateSolid, IntersectTwoSets, SplitEdge, JoinEdge, UnjoinEdge con i limiti di D-076 |
| **SubD** | 23 comandi | 🟠 | Molti comandi ricostruiscono la gabbia e perdono le pieghe (D-052, D-056); superficie limite calcolata da Kurva3D (D-035); ToNURBS converte i SubD con superfici approssimate entro la tolleranza, non esatte (D-051) |
| **Mesh** | 19 comandi (primitive, Reduce, QuadRemesh, booleane mesh, MeshRepair, FillMeshHoles, OffsetMesh, ...) | 🟡 / 🟠 | QuadRemesh non adattivo e senza mesh in ingresso (D-054); densità predefinite presunte (D-030); MeshSplit/MeshTrim fino a 20000 facce (D-076) |
| **Trasformazioni** | 30 comandi (Move, Rotate, Scale, Array*, Flow, Twist, Bend, Taper, Splop, Smash, Squish, Orient, Gumball) | 🟡 / 🟠 | Deformazioni con raffinamento fisso (D-040); Gumball senza alcune funzioni (D-032, D-073); SoftMove, Maelstrom, Cage con i limiti di D-076 |
| **Modifica e selezione** | 34 comandi di modifica e 19 di selezione | 🟡 / 🟠 | Comandi a più gruppi di oggetti diversi da Rhino (D-009); Selezioni con nome salvate nel testo del documento, non lette da Rhino (D-076) |
| **Analisi** | 15 comandi (Zebra, EMap, CurvatureAnalysis, DraftAngleAnalysis, CurvatureGraph, GCon, EdgeContinuity, PointDeviation, Length, Area, Volume, ...) | 🟡 / 🟠 | EdgeContinuity disegna il grafico a pettine e PointDeviation i punti colorati, ma i segni restano fino al comando successivo (D-070); impostazioni diverse (D-045) |
| **Annotazioni** | 14 comandi (Text, Dim*, DimArc, DimCurveLength, Leader, Hatch, ...) | 🟡 / 🟠 | Gli oggetti sono quelli di openNURBS, ma il disegno nelle viste è di Kurva3D: caratteri e posizioni del testo diversi (D-028, D-029, D-039); retini mostrati solo pieni (D-030) |
| **Blocchi** | 6 comandi (Block, Insert, BlockManager, BlockEdit, ...) | 🟡 / 🟠 | BlockEdit senza finestra separata (D-055); blocchi collegati non testati |
| **Livelli e proprietà** | 12 comandi, pannelli Livelli e Proprietà, tipi di linea | 🟡 / 🟠 | -Layer senza tutte le opzioni (D-069); stati dei livelli in formato proprio, non letti da Rhino (D-076); colori per viewport non gestiti |
| **Materiali e render** | Materiali, texture, luci, sole, Rendered, Raytraced e Render con path tracer proprio | 🟠 | Materiali PBR con un editor semplice, non l'editor dei materiali di Rhino 8 (D-059, D-068); path tracer su CPU, non Cycles: niente GPU né denoiser (D-063); Displacement, EdgeSoftening, Shutlining e Decal salvati in formato proprio, non letti da Rhino (D-078); manca l'editor degli ambienti |
| **Tavole e stampa** | Layout, Detail, stampa, PDF | 🟡 / 🟠 | Non aperte in Rhino 8; manca la navigazione dentro un dettaglio attivo, il blocco dei dettagli, i livelli per dettaglio (D-062) |
| **Viste** | 4 viste, CPlane, viste e CPlane con nome, ClippingPlane, Snapshots, ViewCaptureToFile, Make2D | 🟠 | Snapshots in formato proprio, illeggibili da Rhino (D-061); Make2D senza vista prospettica (D-047); le viste non riempiono le parti tagliate dai piani di sezione, il riempimento c'è solo nei disegni di ClippingDrawings (D-079) |
| **File .3dm** | Lettura e scrittura Rhino 8 con openNURBS 8.35 | 🟡 | 153 file d'esempio openNURBS (Rhino 1-8) riletti identici tranne 6 antichi (D-004...D-006). **Nessuna verifica con Rhino 8 reale** (D-002) |
| **Import/export** | STEP, IGES, STL, OBJ+MTL, glTF/GLB, 3MF, PLY, DXF | 🟢 DXF / 🟡 gli altri | DXF provato sui file reali di ingombro (solo con ezdxf). Gli altri formati, compresi FBX, Collada, 3DS e DirectX (Assimp, solo mesh: D-077), solo andata e ritorno in Kurva3D. Manca DWG (solo librerie a pagamento o GPL), mancano SketchUp e altri formati che Rhino 8 legge |
| **Scripting** | Python 3.12 con circa 140 funzioni di rhinoscriptsyntax, ScriptEditor | 🟠 | Rhino 8 ha RhinoCommon completo, C#, l'editor RhinoCode con debugger; gli script di Rhino che usano altre funzioni o RhinoCommon non girano (D-064) |
| **Cronologia (History)** | Meccanismo proprio su circa 40 comandi | 🟠 | Formato diverso da Rhino: Rhino 8 non usa la cronologia di Kurva3D e Kurva3D conserva quella di Rhino senza usarla (D-065) |
| **Grasshopper** | NodeEditor di Kurva3D, circa 70 componenti | 🟠 | Non è Grasshopper: non apre file .gh/.ghx, nessun plug-in di Grasshopper (D-066) |
| **Plug-in** | Nessun sistema di plug-in | 🔴 | Rhino 8 ha SDK, Package Manager e migliaia di plug-in (V-Ray, Enscape, Ladybug, ...); in Kurva3D si possono solo eseguire script Python |
| **Interfaccia e scorciatoie** | Menu in italiano, barre a schede, temi chiaro/scuro, riga di comando, opzioni cliccabili, nomi italiani dei comandi | 🟠 | Icone e aspetto volutamente diversi (D-022, D-057); nomi italiani scelti da Kurva3D (D-072); poche scorciatoie; finestra Opzioni ridotta, alias e cronologia comandi presenti (D-080); manca la personalizzazione delle barre; navigazione nelle viste da confrontare (D-021) |
| **Piattaforme** | Windows e Mac (dmg firmato solo ad hoc) | 🟢 Windows | Interfaccia provata sul PC di mattia; Mac senza certificato Apple |

### Cose in più rispetto a Rhino 8

- **Assistente AI locale** (D-058): descrizione a parole trasformata in comandi, senza rete.
- **Collaborazione in rete locale** (D-067): più computer sullo stesso modello.

## Stabilità

- Test automatici: 169 test CTest, tra cui `commands` (3100 controlli sul branch della PR #23), 14 test dei file
  di prova Rhino 8, test di collaborazione, AI, interfaccia e Python sulla CI Windows.
- Build su `main`: dopo la PR #21 la CI Windows si era fermata perché il pacchetto vcpkg di
  Assimp non si installava; la PR #22 compila Assimp dal sorgente. Il runner Windows ora
  ricompila anche OpenCASCADE 8.0.1: l'esito della build va controllato prima di fidarsi
  di `main`.
  La CI verde dice che i test passano, non che il programma sia stabile nell'uso reale.
- Nessun problema aperto su GitHub, ma questo vuol dire solo che nessuno li ha segnalati:
  il programma è stato usato da una sola persona e per pochi giorni.
- Le geometrie sono calcolate con OpenCASCADE (D-017): su modelli complessi i risultati di
  booleane, raccordi e offset possono riuscire dove Rhino fallisce o viceversa. Non c'è
  ancora una prova su progetti reali di grandi dimensioni.
- Undo copia tutto il modello a ogni comando (D-014): con file grandi usa molta memoria.

## Prima di poterlo vendere

In ordine di importanza:

1. **CI verde su `main` per alcune settimane di uso**, senza regressioni.
2. **Verifica con Rhino 8 reale** (serve mattia o chi ha Rhino 8): aprire in Rhino 8 i 14
   file di `rhino8-prova/` seguendo il LEGGIMI, e mettere in `tests/rhino8_files/` file
   salvati da Rhino 8 per ogni categoria. Solo dopo si possono segnare ✅ nella matrice
   e scrivere "compatibile con i file di Rhino 8".
3. **Prova su progetti veri**: alcuni progetti reali fatti per intero in Kurva3D, annotando
   blocchi, crash e risultati sbagliati.
4. **Verifica dei formati di scambio** con altri programmi (AutoCAD per DXF, un altro CAD
   per STEP/IGES, slicer per 3MF/STL, Blender per glTF/OBJ).
5. **Punti legali e commerciali** di [VENDITA.md](VENDITA.md): repository privato, dati del
   titolare in EULA e LICENSE, partita IVA, ricerca del marchio "Kurva3D", revisione
   dell'EULA, canale di vendita, certificato di firma del codice, firma Apple per il Mac.
6. **Scelte di prodotto**: protezione della licenza, versione di prova, prezzo.
7. **Descrizione onesta**: presentarlo come CAD NURBS indipendente che legge e scrive i
   .3dm, non come sostituto di Rhino 8, finché le funzioni marcate 🔴 e 🟠 più importanti
   (plug-in, Grasshopper, render, DWG) non ci sono.
