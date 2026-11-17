# Differenze rispetto a Rhino 8

Elenco aggiornato di tutto ciò in cui il programma si comporta diversamente da
Rhino 8, o non è ancora stato verificato. Ogni voce ha un codice stabile.

| Codice | Area | Differenza | Impatto | Stato |
|---|---|---|---|---|
| D-001 | Generale | L'interfaccia `rhino-ui` riproduce solo l'essenziale di Rhino 8: menu, riga di comando in alto, 4 viste, barra laterale, pannelli Livelli e Proprietà, barra di stato. Mancano: barre strumenti a schede con icone, Gumball, pannelli Display/Materiali/Blocchi, menu contestuali, finestre Opzioni, layout di viste salvati | Alto per l'uso quotidiano | Aperta (prima versione 17/11/2026) |
| D-002 | File .3dm | I file prodotti non sono ancora stati aperti in Rhino 8 reale: la compatibilità è verificata solo tra noi e openNURBS | Da misurare | Aperta: servono file in `tests/rhino8_files/` |
| D-003 | File antichi (Rhino 1-2) | Le bounding box in cache dei B-rep di Rhino 1-2 sono larghe; dopo il salvataggio vengono sostituite da quelle esatte. La geometria non cambia | Nessuno (miglioramento) | Accettata: il confronto usa la box ricalcolata |
| D-004 | File Rhino 1 | In `v1_Soccer.3dm` 4 policurve su 1369 oggetti risultano diverse dopo il salvataggio, e ogni esecuzione ne segnala di diverse: probabile valore non inizializzato nella lettura dei file Rhino 1 dentro openNURBS | Basso (solo Rhino 1) | Da indagare |
| D-005 | File Rhino 3 | In alcuni B-rep di Rhino 3 (`v3_Smooth`, `v3_Speeder`, `v3_SphericalSpiral`) 16 byte non nulli diventano zero dopo il salvataggio (sembra un identificatore) | Da capire | Da indagare |
| D-006 | File Rhino 4-5 | Le quote (lineari, radiali, ordinate, angolari) di Rhino 4/5 vengono convertite al formato annotazioni attuale; dopo il salvataggio la loro serializzazione non coincide con quella dell'originale | Da capire: potrebbe essere solo la conversione | Da indagare, confrontando in Rhino 8 |
| D-008 | Comandi | Il mouse dà i punti sul piano di costruzione della vista cliccata, ma i comandi che ricevono coordinate scritte lavorano ancora sempre sul piano Top (XY del mondo). Osnap solo Fine, Punto medio, Centro, Vertice/Punto (Rhino ne ha molti di più: Vicino, Perpendicolare, Tangente, Quadrante, Intersezione…), nessun Gumball, nessuno SmartTrack | Alto | Aperta |
| D-009 | Comandi | Risolta in gran parte (17/11/2026): se non c'è selezione, i comandi chiedono di selezionare gli oggetti e ripartono con Invio, come Rhino. Resta diverso: i comandi che usano più gruppi di oggetti (BooleanDifference, Split, BooleanSplit, Sweep1) li prendono da un'unica selezione, in ordine (D-015) | Basso | Parziale |
| D-010 | Comandi | Box e Cylinder creano oggetti estrusione (come Rhino 8 con l'impostazione UseExtrusions attiva, che credo sia il predefinito); Sphere, Cone e Torus creano superfici/polisuperfici B-rep | Da verificare in Rhino 8 con `What` | Da verificare |
| D-011 | Comandi | Valori predefiniti presunti, da confrontare con Rhino 8: Polygon NumSides=5; SubDBox 1 faccia per lato; Curve grado 3 con nodi uniformi; InterpCrv grado 3, nodi a lunghezza di corda ed estremi "naturali" (n+2 punti di controllo per n punti); Revolve 360°; ExtrudeCrv Solid=No; Rectangle parte dal primo angolo e gira in senso antiorario se il secondo angolo sta in alto a destra | Possibili differenze nei punti di controllo | Da verificare |
| D-012 | Comandi | Join unisce solo curve (non ancora polisuperfici). Layer gestisce solo le opzioni New e Current. Array chiede numeri e passi invece dei punti della cella | Medio | Aperta |
| D-013 | Comandi | Sono riconosciuti solo i nomi inglesi dei comandi (con o senza `_`), non i nomi italiani di Rhino in italiano | Basso | Aperta |
| D-014 | Annulla | Undo/Redo salvano una copia di tutto il modello a ogni comando: semplice e sicuro, ma con modelli grandi usa molta memoria | Basso per ora | Da ottimizzare |
| D-015 | Comandi kernel | BooleanDifference, Split e Sweep1 usano l'ordine di selezione (il primo oggetto selezionato è quello da cui sottrarre, da dividere o il binario). In Rhino i ruoli si scelgono a richieste separate | Medio | Aperta |
| D-016 | Comandi kernel | FilletEdge e ChamferEdge agiscono sugli spigoli cliccati (senza clic: tutti gli spigoli) con un solo raggio/distanza; in Rhino si possono dare raggi diversi per spigolo e ci sono le maniglie di modifica | Medio | Aperta |
| D-017 | Comandi kernel | Booleane, raccordi, offset, loft, sweep, split e mesh sono calcolati con OpenCASCADE, non con l'algoritmo di Rhino: la forma è corretta (volumi verificati) ma il numero di facce, i nodi e la parametrizzazione delle superfici risultanti possono essere diversi da quelli che darebbe Rhino 8. Loft supporta solo gli stili Normal e Straight | Medio | Da verificare in Rhino 8 |
| D-018 | Comandi interattivi | Nella riga di comando le opzioni (es. `Solid=Yes`) vanno scritte; non sono ancora cliccabili come in Rhino. Alcune richieste facoltative (es. angolo di Revolve) non vengono chieste e usano il valore predefinito | Medio | Aperta |
| D-019 | Viste | Colori dello sfondo, della griglia e della selezione, estensione della griglia (±100 unità, linee principali ogni 10) e modalità di visualizzazione predefinite (Perspective ombreggiata, le altre wireframe) sono presunti: da confrontare con Rhino 8 | Basso | Da verificare |
| D-020 | Viste | Le superfici in wireframe mostrano solo gli spigoli, non le isocurve né i contorni apparenti; la sfera in vista Front appare come un semicerchio (la cucitura) invece che come un cerchio | Medio | Aperta |
| D-021 | Viste | Navigazione: tasto destro ruota (Perspective) o sposta (viste piane), Shift+destro sposta, Ctrl+destro zoom, rotella zoom verso il cursore, doppio clic sul titolo ingrandisce. Il verso e la velocità della rotazione vanno confrontati con Rhino 8 | Basso | Da verificare |
| D-022 | Interfaccia | La barra laterale usa pulsanti con testo invece delle icone di Rhino (le icone di Rhino non si copiano); i menu sono in italiano ma i nomi dei comandi restano inglesi | Basso | Accettata |
| D-023 | Piattaforma | L'interfaccia è stata provata solo su Linux (schermo virtuale, OpenGL software): la build Windows con Qt 6 e OpenCASCADE non è ancora stata compilata | Da misurare | Aperta: primo test sul PC di mattia |
| D-024 | Fillet / Chamfer (curve) | Funzionano solo tra due segmenti di linea complanari (anche se non si toccano: vengono allungati o accorciati). Rhino li fa tra curve qualsiasi (archi, NURBS), ha le opzioni Join, Trim, ExtendArcsBy e ricorda il raggio nel documento: qui il raggio/le distanze sono ricordati solo finché il programma resta aperto | Medio | Aperta |
| D-025 | Trim / Extend | Nella finestra ogni clic conclude il comando (Invio lo ripete con gli stessi oggetti di taglio); in Rhino il comando continua a chiedere altre parti. Mancano le opzioni ApparentIntersections di Trim e Type (Natural/Line/Arc/Smooth) di Extend: le curve NURBS vengono estese con l'estensione di openNURBS, le linee restano linee e gli archi restano archi | Medio | Aperta |
| D-026 | Shell / Cap | Shell rimuove le facce cliccate e svuota verso l'interno (come Rhino) ma non accetta solidi senza facce rimosse. Cap chiude solo i fori piani (come Rhino); il risultato è calcolato con OpenCASCADE (cucitura) | Basso | Da verificare in Rhino 8 |
| D-027 | Clic sugli oggetti | Quando un comando chiede di cliccare un oggetto, il punto è preso sulla curva più vicina entro 8 pixel o sulla superficie colpita (mesh di visualizzazione): con superfici molto curve il punto può distare dalla superficie vera quanto la mesh | Basso | Accettata |
| D-028 | Annotazioni | Testi e quote sono oggetti openNURBS (si salvano e rileggono come in Rhino), ma il disegno nelle viste è nostro: carattere Arial di Qt al posto dei caratteri di Rhino, testo della quota sempre centrato sopra la linea, nessun adattamento del testo fuori dalle linee di estensione. Le annotazioni stanno sempre in un piano parallelo al piano XY del mondo; l'unica sostituzione di stile è Height= | Medio | Aperta |
| D-029 | Annotazioni | Dim decide orizzontale/verticale dalla posizione della linea di quota (sopra o sotto i punti: orizzontale; a lato: verticale), come pare fare Rhino; DimRadius e DimDiameter usano l'arco o cerchio selezionato (in Rhino si clicca la curva). Mancano DimOrdinate, DimCurveLength, testo modificabile (TextEdit) e stili di quota | Medio | Da verificare in Rhino 8 |
| D-007 | Confronto | Il confronto non controlla ancora: materiali render content (PBR), testo utente, cronologia, file incorporati, contenuto delle viste | Possibili perdite non rilevate | Aperta |

## Note sul metodo di confronto

- `ON_Object::DataCRC` di openNURBS non è affidabile per il confronto: per
  `ON_PolyCurve` e `ON_ClippingPlaneSurface` include indirizzi di memoria.
  Usiamo l'SHA-1 della serializzazione Rhino 8 dell'oggetto, senza dati utente.
- La serializzazione di `ON_SubD` contiene un contatore che cambia a ogni
  lettura: per SubD si usano `GeometryHash()` e `TopologyHash()`.
- Gli id delle voci di tabella predefinite (es. stili di quota di sistema) e
  degli oggetti dei file antichi senza id vengono generati a ogni lettura: le
  tabelle si confrontano per nome e l'istantanea dell'originale si prende
  prima del salvataggio.
