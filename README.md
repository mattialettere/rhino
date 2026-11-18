# Kurva3D

Applicazione CAD indipendente, per uso personale, che prende come riferimento
**Rhinoceros 8 per Windows 64 bit** e punta alla piena compatibilità con i
file `.3dm` di Rhino 8. Nessun codice proprietario di McNeel viene copiato:
il comportamento dei comandi si riproduce studiando Rhino 8, e il formato
`.3dm` si legge e scrive con **openNURBS**, la libreria open source che McNeel
pubblica proprio a questo scopo.

## Regole del progetto

1. Riferimento unico: Rhino 8 per Windows (interfaccia, comandi, scorciatoie,
   console comandi, viewport).
2. Riprodurre il comportamento, mai copiare codice proprietario.
3. Compatibilità massima con i `.3dm` di Rhino 8, verificata con test incrociati.
4. Matrice di compatibilità per categoria: [docs/COMPATIBILITA.md](docs/COMPATIBILITA.md).
5. Niente è "compatibile" finché non è verificato in Rhino 8.
6. Elenco delle differenze: [docs/DIFFERENZE.md](docs/DIFFERENZE.md).
   Inventario dei comandi di Rhino 8 e loro stato: [docs/COMANDI.md](docs/COMANDI.md).
7. Niente funzioni di Rhino 9 prima di aver completato Rhino 8 (per questo
   openNURBS è fissato a un tag **8.x**: il ramo principale scrive già file v9).

## Stack tecnico

| Parte | Scelta | Perché |
|---|---|---|
| Linguaggio | C++20 | Lo stesso di openNURBS: nessuno strato di conversione, prestazioni da CAD |
| Build | CMake ≥ 3.24 | Funziona con Visual Studio 2022 su Windows e con GCC/Clang |
| Formato .3dm | openNURBS `v8.35.26251.13001` | Libreria ufficiale e open source di McNeel; scrive file Rhino 8 |
| Interfaccia | Qt 6 (`kurva3d`) | Finestre agganciabili, barre strumenti e riga di comando come Rhino |
| Viewport | OpenGL 2.1 (pipeline fissa) | Quattro viste (Top, Perspective, Front, Right) come Rhino 8; funziona su ogni scheda video |
| Geometria | openNURBS + OpenCASCADE 7.x | openNURBS valuta NURBS/B-rep/SubD; OpenCASCADE fa booleane, raccordi, offset, loft, sweep, split, mesh |

Dettagli e alternative: [docs/STACK.md](docs/STACK.md).

## Struttura

```
src/core/            nucleo: documento .3dm, riepilogo per categoria, confronto modelli
src/commands/        motore dei comandi (sintassi delle macro di Rhino) e comandi
src/kernel/          ponte openNURBS <-> OpenCASCADE e comandi che ne hanno bisogno
src/ui/              finestra in stile Rhino 8 (Qt): riga di comando, 4 viste, pannelli
tools/cad-console    riga di comando in stile Rhino: crea e modifica modelli
tools/3dm-info       stampa il contenuto di un .3dm per categoria
tools/3dm-roundtrip  legge → salva come Rhino 8 → rilegge → confronta
tests/rhino8_files/  file salvati da Rhino 8 per i test incrociati
docs/                matrice di compatibilità, differenze, stack
```

## Compilare su Windows

Servono Visual Studio 2022 (carico di lavoro "Sviluppo di applicazioni desktop
con C++", che include CMake) e Git. Da "Developer PowerShell for VS 2022":

```powershell
cd G:\github\rhinoceros
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release
```

La prima configurazione scarica openNURBS da GitHub (circa 200 MB).
Così si compilano il nucleo, i comandi base e i test. Per la finestra grafica
e per i comandi che usano il motore geometrico servono anche **Qt 6** e
**OpenCASCADE**; il modo più semplice è vcpkg (una volta sola, richiede
1-2 ore di compilazione):

```powershell
git clone https://github.com/microsoft/vcpkg C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
C:\vcpkg\vcpkg install qtbase:x64-windows opencascade:x64-windows
cd G:\github\rhinoceros
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
build\Release\kurva3d.exe
```

In alternativa: Qt 6 dall'installer ufficiale (componente MSVC 2022 64-bit)
e OpenCASCADE 7.x dall'installer del sito, passando a CMake
`-DCMAKE_PREFIX_PATH="C:\Qt\6.x\msvc2022_64;C:\OpenCASCADE-7.x\occt"`.
Se CMake non li trova lo dice nell'output e compila il resto.
**Nota:** la build Windows di `kurva3d` e del kernel non è ancora stata
provata (vedi D-023); la versione Linux è compilata e testata.

## La finestra (kurva3d)

![kurva3d](docs/screenshot-kurva3d.png)

Disposizione come Rhino 8: menu in alto, cronologia e riga `Comando:`, barra
dei comandi a sinistra, quattro viste (Top, Perspective, Front, Right),
pannelli Livelli e Proprietà a destra, barra di stato con coordinate e
interruttori Snap griglia, Orto, Osnap.

- Scrivi il nome del comando (anche senza cliccare sulla riga), poi Invio o
  Spazio; Invio a vuoto ripete l'ultimo comando.
- Clicca i punti nelle viste, oppure scrivi le coordinate; Invio termina,
  Esc annulla.
- Se un comando ha bisogno di oggetti e non ce ne sono di selezionati, li
  chiede: cliccali e premi Invio (es. Trim: clicca gli oggetti di taglio,
  Invio, poi clicca la parte da eliminare).
- Clic su un oggetto per selezionarlo; Shift aggiunge, Ctrl toglie; trascinando
  da sinistra a destra selezioni a finestra, da destra a sinistra a intersezione.
- Tasto destro trascinato: ruota (Perspective) o sposta la vista; Shift+destro
  sposta; Ctrl+destro o rotella: zoom. Doppio clic sul titolo ingrandisce la vista.
- `kurva3d file.3dm` apre un file; `--script comandi.txt` esegue uno script.

## Modellare dalla console

`cad-console` accetta i comandi con la stessa sintassi delle macro di Rhino 8.
Scrivi `?` per l'elenco dei comandi, `esci` per uscire.

```text
Comando: _Box 0,0,0 10,20 30
Comando: _Circle 50,0 8
Comando: _SelLast _ExtrudeCrv _Solid=_Yes 25
Comando: _SelAll _ArrayPolar 0,0 6
Comando: _SaveAs "C:\modelli\prova.3dm"
```

Oppure da uno script: `cad-console script.txt -o risultato.3dm`.
Punti: `x,y` o `x,y,z`; relativi `r5,0` o `@5,0`; polari `10<45`.

## Usare gli strumenti

```powershell
build\Release\3dm-info.exe C:\percorso\modello.3dm
build\Release\3dm-roundtrip.exe C:\percorso\modello.3dm C:\percorso\copia.3dm
```

`3dm-roundtrip` esce con codice 0 solo se la copia, riletta, è uguale
all'originale per tutto ciò che il confronto controlla (vedi
`src/core/ModelCompare.h`).

## Licenze di terze parti

Kurva3D usa openNURBS (licenza permissiva di McNeel), Open CASCADE Technology (LGPL 2.1 con eccezione) e Qt 6 (LGPL 3). Elenco e testi completi in [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) e nella cartella `licenses`, copiati anche nello zip di ogni versione. Rhinoceros e Rhino sono marchi di Robert McNeel & Associates; Kurva3D non è affiliato a McNeel.
