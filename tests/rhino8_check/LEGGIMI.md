# File di prova Kurva3D → Rhino 8

Questi 14 file sono stati creati da Kurva3D con gli script `.txt` di questa cartella
(un file per categoria). Kurva3D li salva, li rilegge e li confronta in ogni test
automatico, ma **solo Rhino 8 può dire se sono davvero compatibili**.

Tempo stimato: 20–30 minuti.

## Parte 1: aprire i file di Kurva3D in Rhino 8

Per ogni file:

1. Aprilo in Rhino 8 (File > Apri). Se compare un avviso o un errore, annotalo.
2. Lancia il comando `Audit3dmFile` e poi `SelBadObjects`: annota se trova problemi.
3. Controlla i punti della tabella qui sotto e segna ✅ oppure descrivi cosa non va.
4. Salva con **Salva con nome** nella cartella `da_rhino8` con lo stesso nome
   (es. `da_rhino8/07_livelli_tipi_linea.3dm`), senza modificare nulla.

| File | Cosa controllare in Rhino 8 |
|---|---|
| 01_curve | 8 curve: linea, rettangolo chiuso, arco, cerchio, ellisse, curva di grado 3, curva interpolata chiamata "Curva interpolata", elica di 4 giri |
| 02_superfici | superficie piana con foro circolare, piano 20×15, loft da 3 cerchi (livello Profili), superficie di rivoluzione, superficie estrusa da una curva ondulata |
| 03_solidi_brep | box, sfera, cilindro, cono, toro, estrusione chiusa (rettangolo), box con un incavo sferico (differenza booleana), box con spigoli raccordati r=2. `What` deve dire "polisuperficie chiusa" / "estrusione chiusa" |
| 04_subd | box SubD con uno spigolo vivo (piega), sfera SubD, toro SubD, SubD ottenuta da una mesh box |
| 05_mesh | box, sfera, cilindro e piano mesh, più una mesh ottenuta da un box |
| 06_blocchi | blocchi "Vite" e "Gruppo_viti" nel pannello Blocchi; Gruppo_viti contiene istanze di Vite (blocco annidato); istanze ruotate di 45° e 90° e scalate ×2 e ×0,5 |
| 07_livelli_tipi_linea | albero livelli Edificio > Muri > Esterni/Interni, Edificio > Arredi; colori dei livelli; tipi di linea Tratteggio, TrattoPunto, Punti (Interni tratteggiato, Arredi tratto-punto); spessore di stampa Esterni 0,5 e Interni 0,25; livello Nascosto spento, Bloccato bloccato; la linea in alto: viola, tipo di linea Punti, spessore 0,7, colore di stampa rosso |
| 08_materiali_texture | materiali Rosso_lucido, Cromo, Vetro (pannello Materiali e vista Renderizzata); box con texture a scacchi "legno.png" ripetuta 2 volte (immagine incorporata nel file); cilindro sul livello Metallo con materiale del livello Cromo |
| 09_annotazioni | testo, quote lineare, allineata, ruotata 45°, raggio, diametro, angolo, coordinata, leader "Nota con freccia", punto di testo, retino pieno con foro, segno di centro |
| 10_gruppi_testo_utente | box e sfera nello stesso gruppo; box "Pezzo A" con testo utente Codice=K-001 e Peso=12.5 (pannello Proprietà > Attributi); cilindro nascosto (`Show`), cono bloccato (`Unlock`); testo utente del documento Progetto e Revisione (`GetDocumentUserText`) |
| 11_viste_cplane_sezioni | vista con nome "Prova_Kurva3D", CPlane con nome "Sopra_box" (a quota 10), piano di sezione orizzontale a quota 5 che taglia box e sfera |
| 12_luci_sole | luce puntiforme, direzionale, spot, rettangolare (pannello Luci); sole acceso azimut 135° altezza 40°, piano di terra acceso, illuminazione dal cielo accesa (pannelli Sole e Ambiente) |
| 13_tavole | tavola "Tavola_1" A4 con un dettaglio in vista frontale scala 1:5 e un testo nello spazio pagina |
| 14_cronologia | loft tra due cerchi. La cronologia di Kurva3D è salvata in un formato proprio: Rhino 8 non la usa (spostando un cerchio il loft non si aggiorna, è previsto) |

## Parte 2: file creati da Rhino 8

Crea in Rhino 8 questi piccoli file (bastano pochi oggetti) e salvali nella cartella
`rhino8_originali`:

1. `pbr.3dm` — due oggetti con materiali **PBR** (es. "Oro" metallico e "Plastica"
   rugosa) e uno con materiale Personalizzato con texture.
2. `blocchi_collegati.3dm` — un blocco incorporato, un blocco annidato e un blocco
   **collegato** a un altro file.
3. `cronologia.3dm` — con `Record History` attivo: un loft tra due curve e un'estrusione.
4. `testo_utente.3dm` — oggetti con testo utente e testo utente del documento.
5. `subd_mesh.3dm` — una SubD con pieghe e una mesh con colori per vertice.
6. `annotazioni.3dm` — tutte le quote, testi con caratteri diversi, retini a motivo
   (non pieni), stili di quota personalizzati.
7. `tavole.3dm` — una tavola con due dettagli a scale diverse e un cartiglio.
8. `istantanee.3dm` — due istantanee (Snapshots) salvate.

## Cosa mandarmi

Le cartelle `da_rhino8` e `rhino8_originali` (anche zippate) e le note della
Parte 1. I file andranno in `tests/rhino8_files/`: da quel momento ogni test
automatico li legge, li salva e li confronta, e i punti verificati diventano ✅
nella matrice di compatibilità.
