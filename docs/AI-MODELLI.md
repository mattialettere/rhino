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
5. **Risultati del banco (CI Windows, 19/11/2026, solo processore, grammatica attiva)**:

   | Modello | Script validi | Forma plausibile | Note |
   |---|---|---|---|
   | Qwen3 4B | 6/9 | 4/9 | sbaglia gli oggetti nuovi (sgabello, tavolino, vaso) |
   | Qwen2.5-Coder 7B | 6/9 | 2/9 | sul "tavolo con gamba centrale" mette la gamba nell'angolo del piano |
   | Qwen3 8B | 7/9 | 6/9 | il migliore: tavolino e vaso giusti, sbaglia sgabello e mensola |

   Sugli oggetti simili agli esempi (tavoli, sedia, lampada) tutti i modelli funzionano ma
   copiano l'esempio. Sugli oggetti nuovi l'errore tipico e' leggere il secondo angolo di
   _Box come misura invece che come punto. Con i modelli piccoli (1.7B, 1.5B) la grammatica
   non cambia molto: gli script erano gia' quasi sempre scritti bene, l'errore e' nella forma.
   Conclusione: **Qwen3 resta la famiglia consigliata**; Qwen2.5-Coder resta nel catalogo
   come scelta manuale. L'ingombro in pianta del primo banco era misurato con il box largo
   (corretto: ora si usa il box stretto).
6. **Prossimo passo possibile**: addestramento (LoRA) di un modello Apache su esempi di comandi
   Kurva3D, per avere un modello dedicato e nostro.

## Addestramento di un modello per Kurva3D

Il passo successivo alla grammatica e agli esempi: insegnare i comandi al modello con
un addestramento LoRA leggero, cosi' non deve piu' copiare gli esempi delle istruzioni.

1. **Esempi** (`tools/ai-train/generate_dataset.py`, solo Python standard). Genera richieste
   in italiano (tavoli, tavolo con gamba centrale, tavolini, sgabelli, sedie, mensole, librerie,
   vasi, tazze, piastre forate, flange, lampade, panche, scale, solidi semplici) con misure a caso
   e lo script giusto, calcolato dalle misure. Seme fisso: lo stesso comando da' lo stesso file.
2. **Verifica** (`kurva3d-ai-test --validate-jsonl`). In CI ogni esempio viene eseguito da Kurva3D:
   comandi e argomenti della tabella, script senza errori, altezza e ingombro uguali a quelli
   attesi (tolleranza 1%). Il file verificato si scarica dagli artifact della CI (`kurva3d-train-jsonl`).
3. **Addestramento** (`tools/ai-train/train_lora.py` o il notebook `kurva3d_train_colab.ipynb`).
   Qwen3 4B (Apache 2.0), LoRA rank 16, 2 epoche, solo le risposte. Serve una GPU NVIDIA:
   Colab gratuito (T4, circa 1-2 ore) oppure una GPU a noleggio (A100, circa 30 minuti, pochi euro).
4. **GGUF** q4_k_m (circa 2,5 GB), esportato da unsloth con llama.cpp.
5. **Prova** sui 9 oggetti del banco: `kurva3d-ai-test --bench --gguf modello.gguf --compact`.
   Un modello addestrato riceve le istruzioni brevi (`CompactSystemPrompt()`, senza esempi):
   sono le stesse di `tools/ai-train/system_prompt.txt`, e un test controlla che restino uguali.

Licenze: Qwen3, unsloth, PEFT, TRL e transformers sono Apache 2.0, llama.cpp e' MIT; gli esempi
sono generati da noi. Il modello addestrato si puo' quindi distribuire con Kurva3D.
Nel catalogo un modello addestrato avra' `finetuned = true` (istruzioni brevi).

### Primo modello addestrato (19/11/2026)

Qwen3 4B + LoRA, 1200 esempi per 1 giro su Colab gratuito (T4, circa 1 ora), GGUF q4_k_m da 2,5 GB.
Banco dei 9 oggetti in CI (CPU Windows, grammatica attiva, una richiesta per processo):

| | script validi | misure plausibili | secondi medi |
|---|---|---|---|
| Qwen3 4B di base (istruzioni complete con esempi) | 8/9 | 6/9 | 74 |
| Qwen3 4B addestrato (istruzioni brevi) | 7/9 | 6/9 | 52 |

- Il modello addestrato fa bene gli oggetti diversi dagli esempi (sgabello, tavolino 60x60,
  mensola: 3/3 contro 0/3) ed e' piu' veloce perche' le istruzioni sono piu' corte.
- Sbaglia sedia e lampada, che il modello di base copia dagli esempi delle istruzioni:
  nel primo addestramento erano pochi esempi di quel tipo (1200 in tutto, 1 giro).
- Prossimo passo: tutti i 3000 esempi per 2 giri (GPU a noleggio) con piu' sedie e lampade.

Prova: Actions -> Build -> Run workflow, campo "gguf_url" con il link al file (es. Google Drive).

## Oggetti complessi: lavoro per parti (24/11/2026)

Prima di questo cambiamento l'assistente scriveva tutto lo script in una volta, con 6 esempi
fissi (tavoli, sedia, lampada, tazza) e solo forme semplici (box, cilindri, tubi): un violino
richiedeva decine di coordinate coerenti e nessuno dei modelli locali ci riusciva.

Ora, con i modelli non addestrati (Qwen3, Qwen2.5-Coder), l'assistente lavora per parti:

1. **Piano** (`ai::PlanSystemPrompt`, grammatica `ai::PlanGrammar`): il modello scrive un blocco
   ```plan con una riga `# ...` (misure e orientamento) e una riga per parte:
   `PART Nome | cosa e' e con quali comandi | x0,y0,z0 x1,y1,z1` (ingombro minimo e massimo).
2. **Parti**: per ogni parte una richiesta separata ("Part 3 of 7: Fingerboard ... box ..."),
   risposta in un blocco ```kurva con la grammatica dei comandi. Kurva3D la esegue subito su una
   copia del documento insieme alle parti gia' fatte (`ai::CheckPart`): se un comando fallisce o se
   la parte finisce fuori dal suo ingombro (centro spostato o misura molto piu' grande) il modello
   riceve l'errore con le misure e corregge solo quella parte (al massimo 2 volte). Una parte che
   non riesce viene lasciata fuori e segnalata; nella conversazione restano solo le risposte giuste.
   Se nemmeno il piano riesce, si torna allo script in una volta.
3. **Forme composte** (estensioni per l'AI, diventano comandi normali): `_SymCurve` contorno
   simmetrico (chitarre, violini, scafi, carrozzerie), `_SectionLoft` sezioni ellittiche (manici,
   fusoliere, beccucci, pinne), `_Lathe` profili di rivoluzione (vasi, bottiglie, tazze, paralumi),
   `_Plate` lastre in qualsiasi piano (ali, vele, ponticelli), `_PipeThrough` tubi e corde, e
   `@Nome` alla fine dei comandi che agiscono sugli oggetti (niente piu' `_SelNone`/`_SelName`).
4. **Esempi scelti in base alla richiesta** (`ai::Examples`, `ai::SelectExamples`): 11 oggetti
   completi (piano + parti), nelle istruzioni entrano i 2 piu' vicini alla richiesta (parole
   italiane e inglesi, senza accenti), altrimenti chitarra e tavolo. Tutti verificati dai test,
   parte per parte, compreso l'ingombro. `kurva3d-ai-test --examples cartella` li salva in .3dm.
5. Contesto di llama-server portato a 16384 token con una sola conversazione (`-np 1`): istruzioni
   e 2 esempi sono circa 16000 caratteri, ma i modelli Qwen contano ogni cifra come un token.

Il modello addestrato su Kurva3D (`finetuned`) continua a scrivere lo script in una volta con
le istruzioni brevi (`tools/ai-train/system_prompt.txt` invariato): non conosce il formato a parti.

### Prove

- Senza modello: `python3 tools/ai-test/fake_llama_server.py 8099 --break` risponde con gli
  esempi della libreria e sbaglia apposta la prima risposta di due parti (comando inesistente,
  parte spostata di 5 metri); `kurva3d-ai-test --bench-complex --port 8099` verifica piano,
  parti e correzioni. In CI Windows a ogni push.
- Con i modelli veri: Actions -> Build -> Run workflow con "ai_complessi" (o `[ai-complessi]` nel
  messaggio del commit): Qwen3 4B e 8B su violino (anche senza l'esempio del violino), violoncello,
  chitarra elettrica, bicicletta, automobile sportiva, elicottero, teiera; i modelli ottenuti e i
  loro screenshot sono nell'artifact `kurva3d-ai-complessi`.
