# File di prova per i formati di scambio

I DXF di questa cartella sono scritti da **ezdxf** (libreria Python indipendente,
compatibile con AutoCAD), non da Kurva3D, e servono al test "Scambio_file" di
`commands-test`:

| File | Contenuto |
|---|---|
| `completo_r2018.dxf` | DXF R2018 di testo: livelli (colore, RGB, spento, bloccato, tipo di linea, spessore), colori/tipi di linea/spessori per oggetto, blocchi annidati con attributo, testi con accenti, MTEXT, quote (lineare, allineata, raggio, diametro, angolare, testo sostituito), retini (pieno e ANSI31 con isole), direttrice, gruppo, 3DFACE, polyface, spazio carta |
| `completo_r2018_binario.dxf` | lo stesso in DXF binario |
| `completo_r12.dxf` | versione R12 (codepage Windows-1252) |
| `metri.dxf` | una linea di 1 m in un DXF in metri |

Per rigenerarli: `pip install ezdxf` e `python3 genera_dxf.py .`
