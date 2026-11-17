# Matrice di compatibilità con Rhino 8

Ultimo aggiornamento: 17/11/2026 · openNURBS v8.35.26251.13001

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
| **Materiali** | 🟡 | 🟡 | 🟡 | ⬜ | nome, diffuso, speculare, lucentezza, trasparenza, numero texture | I materiali "render content" di Rhino 8 (PBR ecc.) non sono confrontati: ⬜ |
| **Annotazioni** (testo, quote, leader, punti di testo) | 🟡 | 🟡 | 🟡 | ⬜ | bytes serializzati | Quote di Rhino 4/5: 🟠 D-006. Creazione con Text, Dim*, Leader, Centermark: testata (salva e rilegge identico), disegno nelle viste semplificato D-028 |
| Stili di quota | 🟡 | 🟡 | 🟡 | ⬜ | nome, altezza testo, frecce, scala | Solo alcuni campi |
| Retini e motivi retino | 🟡 | 🟡 | 🟡 | ⬜ | bytes / nome | |
| Tipi di linea | 🟡 | 🟡 | 🟡 | ⬜ | nome, segmenti, lunghezza motivo | |
| Gruppi | 🟡 | 🟡 | 🟡 | ⬜ | nome | |
| Piani di sezione (clipping) | 🟡 | 🟡 | 🟡 | ⬜ | bytes serializzati | Da file Rhino 8 |
| Luci | 🟡 | 🟡 | 🟡 | ⬜ | bytes serializzati | |
| Viste con nome, unità, tolleranza | 🟡 | 🟡 | 🟡 | ⬜ | numero viste, unità, tolleranza assoluta | Contenuto delle viste non confrontato |
| Testo utente (attributi), cronologia, file incorporati | ⬜ | ⬜ | ⬜ | ⬜ | non confrontati | Da aggiungere al confronto |

## Interfaccia e comandi

| Area | Stato |
|---|---|
| Finestra principale, viewport | 🟠 Prima versione `rhino-ui` (4 viste, riga di comando, Livelli, Proprietà): provata solo su Linux, non ancora confrontata con Rhino 8. Differenze D-001, D-019…D-023 |
| Riga comandi (sintassi delle macro) | 🟡 `rhino-ui` e `cad-console`: richiesta punti passo-passo, Invio/Spazio/Esc come Rhino |
| Input col mouse | 🟠 Clic per i punti, elastico, osnap Fine/Medio/Centro/Vertice, snap griglia, orto, selezione a clic e finestra, selezione chiesta dal comando, clic sugli oggetti per Trim/Extend/Fillet/Shell (D-008, D-009, D-027) |
| Comandi di disegno e modifica | 🟡 81 comandi su 301 censiti (22 con OpenCASCADE), testati in automatico ma non ancora confrontati con Rhino 8: vedi [COMANDI.md](COMANDI.md) |
| Scorciatoie e alias | 🟠 Solo Ctrl+N/O/S/Z/Y/A, Canc, Esc, Invio/Spazio ripete l'ultimo comando |

I file prodotti dai comandi vengono salvati in formato Rhino 8, riletti e
confrontati in ogni test (nessuna differenza); i B-rep prodotti da OpenCASCADE
passano anche il controllo `IsValid` di openNURBS. Resta da aprirli in Rhino 8.
