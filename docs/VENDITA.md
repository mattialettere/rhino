# Vendere Kurva3D: cosa è pronto e cosa resta da fare

Questa pagina raccoglie gli aspetti pratici e legali per vendere Kurva3D.
Non è una consulenza legale o fiscale: i punti marcati ⚖️ vanno fatti
verificare da un avvocato o da un commercialista prima di vendere.

## Già fatto nel programma (dalla 0.6)

| Tema | Stato |
|---|---|
| Nome e marchi | Il programma si chiama Kurva3D ovunque (exe, finestre, installer, zip, file .3dm salvati). "Rhino" compare solo come riferimento descrittivo di compatibilità, con la nota che Kurva3D non è affiliato a McNeel. |
| Icona | Icona originale (curva con poligono di controllo), nell'exe, nella finestra e nell'installer. |
| Dati della versione | `kurva3d.exe` → Proprietà → Dettagli mostra nome, versione, titolare e copyright. Il titolare si imposta con la variabile CMake `KURVA3D_PUBLISHER`. |
| Licenza d'uso | `EULA.txt` (bozza in italiano): mostrata e da accettare nell'installer, apribile dal menu **? → Licenza d'uso**. |
| Licenza del codice | `LICENSE.txt`: codice sorgente "tutti i diritti riservati". |
| Componenti di terze parti | `THIRD_PARTY_NOTICES.txt` e cartella `licenses` nello zip e nell'installer, con i testi completi delle licenze e le note di copyright di tutte le librerie vcpkg; menu **? → Componenti di terze parti** e **Informazioni su Kurva3D**. |
| Installer | `Kurva3D-Setup-<versione>.exe` (Inno Setup): installazione per utente o per tutti, collegamenti, disinstallazione, associazione dei file .3dm facoltativa (disattivata di default per non togliere i .3dm a Rhino). |

## Licenze delle librerie: cosa comportano

| Libreria | Licenza | Obblighi per chi vende |
|---|---|---|
| openNURBS (McNeel) | Licenza openNURBS, permissiva | Mantenere l'avviso di copyright. Uso commerciale permesso. |
| Open CASCADE Technology | LGPL 2.1 + eccezione Open CASCADE | DLL separate e sostituibili (già così); avvisi e testo della licenza (già inclusi); citare l'uso di OCCT nella documentazione (già fatto). Se si modifica il codice di OCCT, le modifiche vanno pubblicate. Chi riceve il programma deve poter avere il sorgente di OCCT: basta indicare dove scaricarlo (già nei notice) ⚖️ |
| Qt 6 | LGPL 3 | Come sopra: DLL sostituibili, testi LGPL e GPL 3, indicazione del sorgente. L'EULA non deve vietare la sostituzione delle DLL né il reverse engineering per il debug di quelle modifiche (già escluso nel punto 5). Attenzione: con LGPL 3 non si possono usare blocchi hardware che impediscano di far girare una Qt modificata. |
| zlib, FreeType, libpng e altre | Permissive | Avvisi di copyright (già inclusi in `licenses/vcpkg`). |

Non serve comprare licenze commerciali di Qt o OCCT finché si collegano
dinamicamente come ora.

## Da fare a cura tua prima di vendere

1. **Repository privato.** Ora `mattialettere/rhino` è pubblico: chiunque può
   scaricare il codice e compilarlo gratis. Su GitHub: Settings → General →
   Danger Zone → Change visibility → Private. Nello stesso posto puoi
   rinominarlo (per esempio `kurva3d`). Le release già pubblicate restano
   scaricabili solo da chi ha accesso al repository privato.
2. **Titolare.** Compila i campi tra parentesi quadre in `EULA.txt` e
   `LICENSE.txt` (nome o ragione sociale, indirizzo, P.IVA, e-mail, foro) e
   imposta `KURVA3D_PUBLISHER` (lo posso fare io quando mi dai i dati).
3. **Partita IVA / attività** per vendere software ⚖️.
4. **Verifica del nome Kurva3D** nelle banche dati dei marchi (UIBM,
   EUIPO/TMview, WIPO) e del dominio; valuta la registrazione del marchio ⚖️.
5. **Revisione dell'EULA** da parte di un legale ⚖️ (garanzia legale e
   recesso per i consumatori, limitazioni di responsabilità).
6. **Canale di vendita.** Un "merchant of record" (per esempio Paddle,
   Lemon Squeezy, FastSpring) incassa, emette fattura e gestisce l'IVA
   europea (OSS) al posto tuo; in alternativa Stripe con gestione IVA tua ⚖️.
7. **Firma del codice.** Senza certificato di firma (Authenticode) Windows
   SmartScreen mostra "Editore sconosciuto". Un certificato OV/EV costa
   circa 200-500 € l'anno; la firma si aggiunge poi alla CI.
8. **Privacy.** Il programma non invia dati. Se aggiungi attivazione online
   o aggiornamenti automatici servirà un'informativa privacy (GDPR) ⚖️.
9. **Compatibilità dichiarata.** Puoi scrivere "compatibile con i file .3dm
   di Rhino 8" (uso descrittivo), ma senza loghi di Rhino/McNeel e senza far
   pensare a un prodotto ufficiale. Prima di dichiararla, va verificata su
   file veri di Rhino 8 (vedi `COMPATIBILITA.md`).

## Da decidere (posso implementarlo io)

- **Protezione della licenza:** nessuna (vendita "sulla fiducia"), chiave
  di licenza verificata offline (firma digitale della chiave, nessun server),
  oppure attivazione online. La chiave offline è il compromesso più comune
  per un programma da desktop.
- **Versione di prova:** per esempio 30 giorni completi, oppure gratuita
  senza salvataggio.
- **Modello di prezzo:** licenza perpetua con un anno di aggiornamenti,
  oppure abbonamento.
