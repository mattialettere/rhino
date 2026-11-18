# Stack tecnico: scelte e alternative

## Formato .3dm: openNURBS (scelto)

- Libreria C++ pubblicata da McNeel con licenza permissiva, pensata per
  permettere a programmi terzi di leggere e scrivere `.3dm`.
- È la stessa base di `rhino3dm` (Python/JS/.NET).
- Il tag è fissato a **v8.35.26251.13001** (ultima release 8.x al 17/11/2026).
  Il ramo `main` è già alla versione 9 e scrive archivi v90: non va usato
  finché gli obiettivi Rhino 8 non sono completi.
- Limite: openNURBS legge, scrive e valuta la geometria, ma **non** contiene
  gli algoritmi di modellazione di Rhino (booleane, raccordi, offset,
  intersezioni, meshing di rendering, ecc.). Quelli vanno scritti o presi da
  librerie open source.

Alternative scartate:
- *rhino3dm in Python*: comodo per script, ma un CAD interattivo in Python
  sarebbe lento; resta utile per test rapidi.
- *Scrivere un lettore .3dm da zero*: mesi di lavoro per ottenere quello che
  openNURBS già fa, con rischio di incompatibilità.

## Interfaccia: Qt 6 (in uso dal 17/11/2026, eseguibile `kurva3d`)

Permette di riprodurre la disposizione di Rhino 8: barra dei menu, riga
comandi in alto con cronologia, barre strumenti a schede, pannelli agganciabili
(Proprietà, Livelli, Display), barra di stato con snap e Gumball, quattro
viewport con titoli cliccabili. Le viste usano OpenGL 2.1 a pipeline fissa
(`QOpenGLWidget`), che funziona su qualsiasi scheda video Windows.

## Geometria avanzata: OpenCASCADE (in uso, opzionale)

Usato per booleane, raccordi, smussi, offset, loft, sweep, tubi, intersezioni,
split, mesh, area e volume (`src/kernel/`). La geometria resta in openNURBS:
prima del calcolo viene convertita in OpenCASCADE e il risultato viene
riconvertito in B-rep openNURBS, NURBS esatti (nessuna approssimazione a mesh).
La conversione è verificata dai test con volumi e aree esatti.
Se OpenCASCADE non è installato il progetto compila lo stesso, senza questi
comandi. Licenza LGPL 2.1 con eccezione: va bene per un'applicazione
personale; per distribuirla bisogna collegarlo dinamicamente.

## Piattaforma

Obiettivo principale Windows 64 bit (come il riferimento). Il nucleo compila
anche su Linux: la suite di test gira lì in integrazione continua.
