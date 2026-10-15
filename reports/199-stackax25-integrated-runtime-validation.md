# APP-032 — Integrated runtime validation

Data: 2026-10-15

## Baseline e integrazione

- `libax25`: branch `stackax25/integration`, HEAD `5ad32c656d7cca13bc8b22d4bd76bd756e6a2001`.
- `ax25-tools`: branch `stackax25/upstream-fixes`, commit locale `30ede9a` (`ax25d: harden signal handling and configuration reload`).
- `ax25-apps`: branch `stackax25/upstream-fixes`, HEAD `340e63673a75b33f037175e461242671262d71f4`, nessuna modifica sorgente.
- Modifica applicativa: solo `ax25/ax25d.c` in `ax25-tools`.
- `git diff --check`: PASS prima del commit.

Il commit APP-031O è stato integrato nella copia operativa di `ax25d` e completato con una correzione minima: una configurazione malformata o una fase di costruzione fallita non pubblica la lista temporanea di listener. Il listener precedente resta pubblicato; gli effetti esterni di socket eventualmente già creati da `ReadConfig()` restano un rischio storico separato.

## Build privata ARMv7 su IR3VIG

Workspace remoto: `/tmp/stackax25-app032/`. Nessun repository operativo remoto è stato modificato.

- Build/install privata `libax25` in `/tmp/stackax25-app032/libax25-prefix`: **PASS**.
- Build/install privata `ax25-tools`: **PASS**.
- Link `ax25d`: **PASS**; ELF ARM 32-bit EABI5, dinamico.
- Dipendenze: `libax25.so.0`, `libc.so.6`; libreria privata usata tramite `LD_LIBRARY_PATH`.
- SHA-256 eseguibile temporaneo: `51c5b0f78a2068e46ed148401143040bb54175118c143d9b8ec657ef66c19c2e`.
- Build privata `ax25-apps`: **PASS**, dopo `autoreconf -fi` nella sola copia temporanea; HEAD della copia `340e63673a75b33f037175e461242671262d71f4`.

Il sorgente locale APP-031O di riferimento ha SHA-256 `0d326bfbfd74cd14c16cd9f8380811cc04ae6803f6bd3d57eddfc2acf5424769`. L’eseguibile remoto è stato prodotto dalla copia APP-032 con la correzione di reload.

## Runtime reale

Helper installato e verificato:

`/usr/local/libexec/stackax25-kiss-test` — SHA-256 `6c7693926a02127b5ac84d43d95862752cc515c378263b74f22ae4c07e5ff933`.

Risultati:

- Avvio helper e creazione `ax0`: **PASS**.
- Porta `STACKQ`, callsign `N0CALL-1`, MTU 256: **PASS**.
- `/proc/net/ax25` ha mostrato il listener `ax0` durante il test: **PASS**.
- Avvio del vero `ax25d` APP-032 e daemon loop persistente: **PASS**. PID osservato `28377` nel primo ciclo e `30169` nel secondo.
- SIGHUP con configurazione valida: **PASS**; processo e listener sono rimasti attivi.
- SIGHUP con `BROKEN CONFIG`: **PASS** per il criterio APP-032; il processo è rimasto attivo e la riga `/proc/net/ax25` è rimasta invariata rispetto al listener precedente.
- Ripristino configurazione valida e SIGHUP: **PASS**.
- SIGALRM manuale: **PASS**; il processo è rimasto attivo e il listener è rimasto presente.
- SIGCHLD manuale senza figlio terminato: **PASS** limitato al percorso di gestione; reaping di un figlio reale: **NOT_RUN**.
- SIGTERM e uscita del daemon: **PASS**.
- `stop` helper e rimozione di `ax0`: **PASS**; nessun `ax25d`/`kissattach` residuo osservato.

## Connessione AX.25

La connessione richiesta tra due endpoint distinti non è stata dimostrata.

- Un tentativo con il binario privato `ax25_call` è fallito prima dell’apertura della sessione con `ERROR: problem with axports file`: **FAIL** del tentativo locale, non del daemon.
- Il test non ha prodotto evidenza di SABM/SABME, UA, frame I o disconnessione.
- Non è stata predisposta una seconda porta KISS con callsign distinto tramite l’helper autorizzato.
- Fork/exec di `ax25d` e figlio reale non sono quindi stati esercitati: **NOT_RUN**.
- Nessun traffico radio esterno, AXIP o AXUDP è stato generato.

Questa limitazione impedisce di dichiarare completato il collaudo applicativo end-to-end. Non è evidenza di una regressione del codice di reload.

## Stato Git e push

- Commit locale creato in `ax25-tools`: `30ede9ac3dddd85bff5842942d4afcf6ed768114`.
- `libax25` e `ax25-apps`: nessun commit necessario, perché non sono state introdotte modifiche sorgente.
- Push: **NOT_RUN**. La verifica del remote GitHub ha fallito per risoluzione DNS (`Could not resolve host: github.com`); nessun riferimento remoto è stato alterato.
- Deployment permanente: **NO**.
- Repository operativi remoti IR3VIG: invariati; sono state usate solo copie sotto `/tmp` e il helper/test KISS già autorizzato.

## Decisione

La variante è tecnicamente idonea alla revisione d’integrazione per il lifecycle del daemon e per il reload transazionale osservato. La raccomandazione finale è **CONDITIONAL** per l’integrazione complessiva dello stack: resta non verificato il percorso fork/exec con una connessione AX.25 reale e il test a due endpoint.

La raccomandazione per deployment permanente è **NO**. Il successivo collegamento AXIP verso IR3VIB resta fuori perimetro e non è stato testato.

```text
APP032_LIBAX25_BUILD = PASS
APP032_AX25_TOOLS_BUILD = PASS
APP032_AX25_APPS_BUILD = PASS
APP032_KISS_PORT = PASS
APP032_DAEMON_LOOP = PASS
APP032_VALID_RELOAD = PASS
APP032_MALFORMED_RELOAD_PRESERVES_LISTENER = PASS
APP032_SIGALRM = PASS
APP032_SIGCHLD_REAL_REAP = NOT_RUN
APP032_FORK_EXEC = NOT_RUN
APP032_AX25_TWO_ENDPOINTS = NOT_RUN
APP032_AX25_FRAME_EXCHANGE = NOT_RUN
APP032_SHUTDOWN_CLEANUP = PASS
APP032_SOURCE_COMMIT = 30ede9ac3dddd85bff5842942d4afcf6ed768114
APP032_PUSH = NOT_RUN_DNS_FAILURE
APP032_DEPLOYMENT = NO
```
