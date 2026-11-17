# File di Rhino 8 per i test incrociati

Metti qui file `.3dm` salvati da **Rhino 8 per Windows** (anche piccoli: un
oggetto per categoria è l'ideale). Ogni file diventa automaticamente un test
`rhino8_<nome>` alla prossima configurazione di CMake.

Il test fa: lettura → scrittura in formato Rhino 8 → rilettura → confronto.
La seconda metà della verifica è manuale: aprire in Rhino 8 il file prodotto
in `build/roundtrip_out/rhino8_<nome>.3dm` e controllare che sia identico
all'originale (comando `What`/`Proprietà`, livelli, materiali, blocchi).
Il risultato va registrato in `docs/COMPATIBILITA.md`.

Suggerimento per il primo giro (un file per voce):
- curve NURBS e linee/archi/polilinee
- superficie NURBS non tagliata e superficie tagliata
- polisuperficie chiusa (B-rep) ed estrusione
- oggetto SubD
- mesh con colori per vertice
- blocco con istanze annidate (anche blocco collegato)
- livelli annidati con colori, tipi di linea, stato acceso/spento/bloccato
- materiali (PBR e personalizzati) assegnati per livello e per oggetto
- quote lineari, angolari, raggio, testo, leader, retini
