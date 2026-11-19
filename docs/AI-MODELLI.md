# Modelli per l'assistente AI di Kurva3D

Kurva3D sara' venduto: si possono scaricare solo modelli con licenza **Apache 2.0** o **MIT**
(controllata anche al momento del download). Verifica fatta il 19/11/2026 sulle schede Hugging Face.

## Modelli dedicati al CAD (text-to-CAD)

| Modello | Licenza dei pesi | Base | Cosa produce | Si puo' includere? |
|---|---|---|---|---|
| CAD-Recode (filapro/cad-recode) | CC BY-NC 4.0 (non commerciale) | Qwen2-1.5B | codice CadQuery da una nuvola di punti (non da testo) | No: licenza e tipo di input |
| Text2CAD (SadilKhan) | dataset CC BY-NC-SA 4.0 | - | sequenze CAD (schizzo + estrusione) | No: licenza non commerciale |
| CADFusion (microsoft/CADFusion) | scheda "MIT", ma e' un fine-tuning di Llama-3-8B | Llama-3-8B | sequenze CAD in un formato proprio | No: i pesi restano soggetti alla licenza Llama 3 |
| DreamCAD (SadilKhan) | Stability AI Community License | Stable Diffusion 3.5 | immagini in stile CAD | No: licenza e non genera geometria |
| C3D-v0 (numinousmuses) | scheda Apache 2.0 ma "eredita la licenza Gemma" | Gemma 3n | codice CadQuery | No: licenza Gemma |
| CAD-Coder (gudo7208) | Apache 2.0 | Qwen2.5-7B-Instruct | codice CadQuery (Python) | Licenza ok, ma produce CadQuery, non comandi Kurva3D: servirebbe un traduttore CadQuery -> Kurva3D. Candidato per il futuro |

Conclusione: oggi nessun modello CAD dedicato si puo' mettere dentro cosi' com'e'.
L'unico con licenza compatibile (CAD-Coder) parla CadQuery; lo si potra' valutare
quando Kurva3D sapra' eseguire script CadQuery o tradurli nei suoi comandi.

## Modelli generali nel catalogo

- **Qwen3** 1.7B, 4B, 8B, 14B, 32B: Apache 2.0 (famiglia consigliata finora).
- **Qwen2.5-Coder** 1.5B, 7B, 14B, 32B: Apache 2.0, addestrati sul codice. Il **3B e' escluso**
  (licenza Qwen Research). Alcuni file GGUF ufficiali sono divisi in parti (es. 7B Q4_K_M):
  Kurva3D scarica e verifica tutte le parti.

## Come si rende preciso un modello generale

1. **Grammatica (GBNF di llama.cpp)**: il modello puo' scrivere solo un blocco ```kurva con
   righe di commento "# ..." e i comandi della tabella `CommandSpecs()` con gli argomenti
   nella forma giusta (punti, numeri, opzioni). Niente comandi inventati o argomenti mancanti.
2. **Esempi completi nelle istruzioni**: tavolo, tavolo a rombo, tavolo con gamba centrale,
   sedia, lampada ad arco, tazza (verificati dai test).
3. **Verifica e nuovi tentativi**: lo script viene eseguito su una copia del documento; se un
   comando fallisce, ha argomenti sbagliati, crea pezzi uno dentro l'altro o lascia il modello
   vuoto, il modello riceve l'errore e riprova (fino a 3 volte).
4. **Banco di prova**: `kurva3d-ai-test --bench --models a,b [--both]` confronta i modelli sulle
   stesse richieste (anche "un tavolo con gamba centrale") e controlla l'altezza del risultato.
   In CI parte se il messaggio del commit contiene `[ai-bench]`.
5. **Prossimo passo possibile**: addestramento (LoRA) di un modello Apache su esempi di comandi
   Kurva3D, per avere un modello dedicato e nostro.
