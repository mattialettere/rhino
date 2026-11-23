# Matrice di compatibilità con Rhino 8

Ultimo aggiornamento: 19/11/2026 · openNURBS v8.35.26251.13001

## Legenda

| Stato | Significato |
|---|---|
| ⬜ Non verificato | Nessun test ancora |
| 🟡 Andata/ritorno OK | Letto, salvato come Rhino 8, riletto e confrontato senza differenze dal nostro strumento, **sui file d'esempio di openNURBS**. Non è ancora compatibilità |
| 🟠 Differenze note | Il confronto trova differenze: vedi [DIFFERENZE.md](DIFFERENZE.md) |
| ✅ Verificato con Rhino 8 | Test incrociato: file di Rhino 8 → nostro programma → riaperto in Rhino 8 senza differenze, **e** file creato da noi aperto correttamente in Rhino 8 |

**Nessuna categoria è ancora ✅.** Per arrivarci servono file salvati da Rhino 8
reale (cartella `tests/rhino8_files/`) e il controllo in Rhino 8 dei file che
produciamo.

**File di prova per Rhino 8 (0.10)**: `tests/rhino8_check/` contiene 14 script, uno
per categoria (curve, superfici, B-rep, SubD, mesh, blocchi annidati, livelli e tipi
di linea, materiali e texture, annotazioni, gruppi e testo utente, viste/CPlane/sezioni,
luci e sole, tavole, cronologia). Ogni test crea il `.3dm` in `build/rhino8_check/`,
lo rilegge e lo confronta con il modello in memoria (test `rhino8check_*`). Le
istruzioni per il controllo in Rhino 8 sono in `tests/rhino8_check/LEGGIMI.md`.

## File .3dm (lettura e scrittura)

Test automatici: 153 file d'esempio (versioni Rhino 1→8), andata e ritorno in
formato Rhino 8. 147 identici, 6 con differenze note (tutti di Rhino 1, 3, 4, 5).
Tutti i file di Rhino 6, 7 e 8 risultano identici.

| Categoria | Lettura | Scrittura v8 | Andata/ritorno | Rhino 8 reale | Cosa confronta il test | Note |
|---|---|---|---|---|---|---|
| **NURBS**: curve (NURBS, linee, archi, polilinee, policurve) | 🟡 | 🟡 | 🟡 | ⬜ | bytes serializzati, bounding box, attributi | Policurve di Rhino 1: 🟠 D-004 |
| **NURBS**: superfici (NURBS, rivoluzione, piane) | 🟡 | 🟡 | 🟡 | ⬜ | come sopra | |
| **B-rep** (polisuperfici, superfici tagliate) | 🟡 | 🟡 | 🟡 | ⬜ | come sopra | B-rep di Rhino 3: 🟠 D-005 |
| Estrusioni (ON_Extrusion) | 🟡 | 🟡 | 🟡 | ⬜ | come sopra | |
| **SubD** | 🟡 | 🟡 | 🟡 | ⬜ | hash geometrico e topologico SubD | 15 oggetti da file Rhino 7 e 8 |
| **Mesh** | 🟡 | 🟡 | 🟡 | ⬜ | bytes serializzati, attributi | Colori per vertice e normali non verificati a parte |
| Punti e nuvole di punti | 🟡 | 🟡 | 🟡 | ⬜ | come curve | |
| **Blocchi** (definizioni e istanze) | 🟡 | 🟡 | 🟡 | ⬜ | definizione: nome, tipo, numero oggetti; istanza: bytes | Un solo file d'esempio (Rhino 4). Blocchi collegati e annidati non testati |
| **Livelli** | 🟡 | 🟡 | 🟡 | ⬜ | percorso completo, colore, colore di stampa, spessore di stampa, visibile, bloccato, tipo di linea | Colori per viewport e sezioni non confrontati |
| **Materiali** | 🟡 | 🟡 | 🟡 | ⬜ | nome, diffuso, speculare, lucentezza, trasparenza, riflettivita', IOR, emissione, valori PBR, texture, collegamento al render content | Render content di Rhino 8 (0.10): conservati e confrontati (tipo, nome, XML); i materiali modificati in Kurva3D aggiornano il render content collegato (test "Render content", D-068). Nessun file d'esempio con materiali PBR: servono file di Rhino 8 reale. Texture di Kurva3D (0.8): materiale ON_Material con texture bitmap, immagine incorporata (ON_EmbeddedFile), mappatura (ON_TextureMapping) sul canale 1 degli attributi: salvataggio e rilettura testati tra Kurva3D e openNURBS (test "Texture"); **non ancora aperti in Rhino 8 reale** (da verificare: Rhino crea un materiale personalizzato dal materiale semplice e legge la mappatura associata all'id ON_UniversalRenderEngineId) |
| **Annotazioni** (testo, quote, leader, punti di testo) | 🟡 | 🟡 | 🟡 | ⬜ | bytes serializzati | Quote di Rhino 4/5: 🟠 D-006. Creazione con Text, Dim*, Leader, Centermark: testata (salva e rilegge identico), disegno nelle viste semplificato D-028 |
| Stili di quota | 🟡 | 🟡 | 🟡 | ⬜ | nome, altezza testo, frecce, scala | Solo alcuni campi |
| Retini e motivi retino | 🟡 | 🟡 | 🟡 | ⬜ | bytes / nome | |
| Tipi di linea | 🟡 | 🟡 | 🟡 | ⬜ | nome, segmenti, lunghezza motivo | |
| Gruppi | 🟡 | 🟡 | 🟡 | ⬜ | nome | |
| Piani di sezione (clipping) | 🟡 | 🟡 | 🟡 | ⬜ | bytes serializzati | Da file Rhino 8. Creati da ClippingPlane (0.10): piano, estensione, viste in cui sono attivi; salvataggio e rilettura testati; **direzione del taglio non ancora confrontata con Rhino 8 reale** (D-061) |
| Luci | 🟡 | 🟡 | 🟡 | ⬜ | bytes serializzati | Create da PointLight, DirectionalLight, SpotLight, RectangularLight (0.12): ON_Light del .3dm. Sole, cielo e piano di terra nelle impostazioni di rendering (ON_3dmRenderSettings): salvataggio e rilettura testati (test "Rendering"); **da aprire in Rhino 8 reale** |
| Viste con nome, unità, tolleranza | 🟡 | 🟡 | 🟡 | ⬜ | nome, posizione e direzione della camera, proiezione; CPlane con nome (origine, normale); unità, tolleranza assoluta |  NamedView e NamedCPlane (0.10) scrivono nelle stesse tabelle di Rhino (m_named_views, m_named_cplanes): nome, camera e piano testati in salvataggio e rilettura. Le viste delle finestre si salvano nel file e si rileggono all'apertura (viste con lo stesso nome) |
| Tavole e dettagli (layout) | 🟡 | 🟡 | 🟡 | ⬜ | nome e formato delle tavole, oggetti nello spazio pagina | Create da Layout/Detail (0.11): vista di tipo pagina con dimensioni in mm, ON_DetailView con vista, scala e bordo, oggetti con spazio pagina e id della tavola; salvataggio e rilettura testati (test "Tavole_e_PDF"); **da aprire in Rhino 8 reale** |
| Istantanee (Snapshots) | 🔴 | 🔴 | 🔴 | ⬜ | — | Formato proprio di Kurva3D in una stringa del documento: Rhino non le vede e Kurva3D non legge quelle di Rhino (D-061) |
| Mappature delle texture | 🟡 | 🟡 | 🟡 | ⬜ | tipo di mappatura, canale dell'oggetto | Create da ApplyTexture e Apply*Mapping (0.8) |
| Testo utente (oggetti, livelli, documento) | 🟡 | 🟡 | 🟡 | ⬜ | chiavi e valori | SetUserText, SetDocumentUserText (0.10) |
| Cronologia (record di Rhino) | 🟡 | 🟡 | 🟡 | ⬜ | contenuto serializzato dei record | Conservata cosi' com'e'; Kurva3D non la usa e salva la propria (D-065) |
| File incorporati | 🟡 | 🟡 | 🟡 | ⬜ | nome e contenuto (SHA-1) | Immagini delle texture (ApplyTexture), percorso assoluto come Rhino |
| Render content (materiali, ambienti, texture di Rhino 8) | 🟡 | 🟡 | 🟡 | ⬜ | tipo, nome, XML (attributi in ordine) | 3 file d'esempio di Rhino 4, 5, 7 (D-068) |
| Gruppi, colore e spessore di stampa degli oggetti, spazio pagina | 🟡 | 🟡 | 🟡 | ⬜ | per oggetto | |
| Sole, piano di terra, cielo | 🟡 | 🟡 | 🟡 | ⬜ | acceso, azimut, altezza, quota | |

## Interfaccia e comandi

| Area | Stato |
|---|---|
| Finestra principale, viewport | 🟠 Prima versione `kurva3d` (4 viste, riga di comando, Livelli, Proprietà): provata solo su Linux, non ancora confrontata con Rhino 8. Differenze D-001, D-019…D-023 |
| Riga comandi (sintassi delle macro) | 🟡 `kurva3d` e `cad-console`: richiesta punti passo-passo, Invio/Spazio/Esc come Rhino |
| Input col mouse | 🟠 Clic per i punti, elastico, osnap Fine/Medio/Centro/Vertice, snap griglia, orto, selezione a clic e finestra, selezione chiesta dal comando, clic sugli oggetti per Trim/Extend/Fillet/Shell (D-008, D-009, D-027) |
| Comandi di disegno e modifica | 🟡 81 comandi su 301 censiti (22 con OpenCASCADE), testati in automatico ma non ancora confrontati con Rhino 8: vedi [COMANDI.md](COMANDI.md) |
| Scorciatoie e alias | 🟠 Solo Ctrl+N/O/S/Z/Y/A, Canc, Esc, Invio/Spazio ripete l'ultimo comando |

I file prodotti dai comandi vengono salvati in formato Rhino 8, riletti e
confrontati in ogni test (nessuna differenza); i B-rep prodotti da OpenCASCADE
passano anche il controllo `IsValid` di openNURBS. Resta da aprirli in Rhino 8.

## Formati di scambio (0.9, 0.11, 0.15)

Dalla 0.15 il doppio clic (o Apri) su DXF, STEP e IGES crea un documento con le unita'
del file e con tutto quello che il formato contiene (vedi D-074). Test automatici
(test "Scambio_file"): DXF R2018 testo, R2018 binario e R12 cp1252 scritti da
[ezdxf](https://ezdxf.mozman.de) (`tests/exchange_files/`, script di generazione nel
LEGGIMI della cartella), letti e controllati voce per voce, salvati come .3dm e riletti.

| Formato | Import | Export | Verificato con | Note |
|---|---|---|---|---|
| STEP (.stp/.step) | ✔ nomi, colori, livelli, unita' | ✔ nomi, colori, livelli | andata e ritorno in Kurva3D (solidi chiusi, ingombro identico; nome, colore e livello; documento in pollici riaperto in pollici) | ⬜ da provare con file di altri CAD e con Rhino 8. Assiemi appiattiti |
| IGES (.igs/.iges) | ✔ nomi, colori, unita' | ✔ nomi, colori | andata e ritorno in Kurva3D (nome, colore, pollici) | ⬜ come sopra. Livelli IGES numerici: il nome non torna |
| STL | ✔ | ✔ (binario e ASCII) | andata e ritorno, mesh chiusa | ⬜ |
| OBJ (+ .mtl) | ✔ | ✔ | andata e ritorno con materiali e texture (test "Formati mesh") | ⬜ da provare con Rhino 8, Blender |
| glTF 2.0 (.gltf, .glb) | ✔ | ✔ | andata e ritorno con materiali PBR, trasparenza, texture incorporata, Y in alto e metri; struttura controllata (accessori, buffer, indici) | ⬜ da provare con il validatore Khronos, Rhino 8, Blender |
| 3MF | ✔ | ✔ | andata e ritorno con colori e unita' | ⬜ da provare con un programma di stampa 3D (Cura, PrusaSlicer, Bambu Studio) e Rhino 8 |
| PLY | ✔ | ✔ (binario e ASCII) | andata e ritorno con colori per vertice, nuvola di punti | ⬜ da provare con MeshLab, Rhino 8 |
| DXF (lettura R12...R2018, testo e binario; scrittura R12) | ✔ livelli, tipi di linea, colori/spessori per oggetto, blocchi veri e attributi, testi, quote vere e stili di quota, retini, direttrici, gruppi, spazio carta, unita' | ✔ livelli, tipi di linea, colori, blocchi, testi; quote e retini come geometria | file di ezdxf (R2018, R2018 binario, R12 cp1252) controllati voce per voce; DXF esportato da Kurva3D riletto (blocchi, testi, livelli); DXF batterie dell'utente: quote "32", "4" e "4,4" come in AutoCAD; stessi DXF (LP403232, LPR443535) aperti, modificati (testo, spostamento, nuova linea), salvati in DXF e 3DM e riletti: 3DM identico, DXF con testi/livelli/geometria uguali e quote come geometria; DXF scritto controllato con l'audit di ezdxf (0 errori) | ⬜ da provare con file di AutoCAD e con Rhino 8. Non letti: solidi ACIS, immagini, xref (D-074) |
| DWG, FBX | ⬜ | ⬜ | | richiedono librerie a pagamento (ODA, Autodesk) |
| SketchUp (.skp), Collada (.dae) | ⬜ | ⬜ | | SketchUp: SDK con licenza propria, da valutare; Collada: possibile in futuro |

