# meshtastic_anemometer

Stazione anemometrica low-power per il wing foil, basata su **Seeed XIAO nRF52840** e un anemometro a coppe con banderuola (tipo Davis). Le misure di vento vengono inviate:

- via **LoRa/Meshtastic** (messaggio di testo diretto a un nodo ricevente, ogni ~15 s);
- via **rete cellulare** (modem SIM7080G, Cat-M / NB-IoT) con protocollo **CoAP**, ogni 5 minuti: è il firmware alternativo del branch `sim7080`, che sostituisce il nodo Meshtastic;
- via **Bluetooth LE** a uno smartphone, letto da una semplice web app (Web Bluetooth).

Lato server, due script Python ricevono i messaggi Meshtastic e CoAP e li salvano su **InfluxDB**. A corredo ci sono degli esperimenti di **machine learning** (Random Forest) per stimare la probabilità di avere vento "navigabile" nelle ore successive, sia a partire dai dati della stazione sia dai dati di **OpenWeather**.

```
 Anemometro + banderuola
          │ impulsi (D2) / analogico (A3)
          ▼
 ┌─────────────────────┐   UART 38400   ┌──────────────────┐   LoRa    ┌──────────────────┐  USB   ┌──────────────┐
 │ XIAO nRF52840       │ ─────────────▶ │ nodo Meshtastic  │ ────────▶ │ nodo Meshtastic  │ ─────▶ │ collector.py │──▶ InfluxDB
 │ (firmware .ino)     │                │ (modulo seriale) │           │ ricevente        │        └──────────────┘
 └─────────────────────┘                └──────────────────┘           └──────────────────┘
          │ BLE "WindSensor"
          ▼
     webapp.html (smartphone, Chrome / Web Bluetooth)
```

Con il firmware SIM7080 il percorso via LoRa è sostituito da:

```
 ┌─────────────────────┐   UART 115200  ┌──────────┐  Cat-M / NB-IoT  ┌───────────────────┐
 │ XIAO nRF52840       │ ─────────────▶ │ SIM7080G │ ───────────────▶ │ collector_coap.py │──▶ InfluxDB
 │ (firmware .ino)     │ ─ PWRKEY (D1)▶ │          │  CoAP / UDP 5683 └───────────────────┘
 └─────────────────────┘                └──────────┘
```

## Installazione

Questa guida installa il firmware SIM7080 e il ricevitore in Go su un server Devuan. I dettagli sono nelle sezioni [Versione SIM7080 + CoAP](#versione-sim7080--coap) e [Ricevitore in Go e deploy su Devuan](#ricevitore-in-go-e-deploy-su-devuan).

> Il firmware non è ancora stato provato su un modem e un anemometro veri: al primo avvio segui anche [Da verificare sull'hardware](#da-verificare-sullhardware).

**Cosa serve:**

- sul computer: [PlatformIO](https://platformio.org/) (oppure l'Arduino IDE), Go 1.22 o più recente, `openssl` e un client SSH;
- sul server: Devuan con accesso SSH come root o con `sudo`, l'orologio sincronizzato (ad esempio con il pacchetto `ntp`), un'istanza InfluxDB v2 con il suo token e la porta UDP 5683 raggiungibile da Internet;
- per la stazione: XIAO nRF52840, modem SIM7080G con SIM Cat-M o NB-IoT, anemometro Davis.

### 1. Scarica il codice

```bash
git clone -b sim7080 git@github.com:ninnux/meshtastic_anemometer.git
cd meshtastic_anemometer
```

### 2. Genera la chiave della stazione

**Ogni stazione ha una chiave propria.** Generane una nuova per ognuna e conservala: serve sia alla stazione sia al server.

```bash
openssl rand -hex 32
cp anemometro_sim7080_coap/secrets.example.h anemometro_sim7080_coap/secrets.h
```

In `anemometro_sim7080_coap/secrets.h` cancella la riga `#error` e incolla la chiave:

```cpp
#define STATION_KEY "la_chiave_generata"
```

Il file è escluso da git: non committarlo e non condividerlo.

### 3. Configura lo sketch

In testa ad `anemometro_sim7080_coap/anemometro_sim7080_coap.ino` imposta almeno:

| Define | Cosa inserire |
|---|---|
| `NODE_NAME` | Nome **univoco** della stazione: lettere, numeri, `_` e `-`, massimo 32 caratteri |
| `COAP_SERVER_IP` | Indirizzo del server, se diverso da `anemometro.ninux.org` |
| `MODEM_APN` | APN della SIM |
| `MODEM_PWRKEY_ACTIVE` | `HIGH` o `LOW`, secondo la scheda del modem |

### 4. Collega l'hardware

Segui la tabella in [Collegamenti](#collegamenti). Prima di dare corrente controlla l'alimentazione separata del modem e il livello logico delle sue linee.

### 5. Compila e carica il firmware

```bash
pio run -e sim7080 -t upload
```

### 6. Installa il ricevitore sul server

```bash
cd server
deploy/deploy.sh utente@server        # aggiungi -a arm64 o -a armv7 se il server non è amd64
```

### 7. Configura il ricevitore

Sul server, in `/etc/default/anemometer-collector`:

```sh
INFLUX_URL="http://indirizzo-influxdb:8086"
INFLUX_TOKEN="il_token"
INFLUX_ORG="meteo_org"
INFLUX_BUCKET="anemometri"
STATION_KEYS="first_anemometer:la_chiave_generata"
```

Il nome prima dei due punti deve essere **identico** a `NODE_NAME`, e la chiave identica a quella in `secrets.h`. Poi riavvia:

```bash
sudo service anemometer-collector restart
```

Apri la porta UDP 5683 sul firewall del server: lo script di deploy non lo tocca.

### 8. Verifica

Il primo messaggio parte 5 minuti dopo l'avvio della stazione.

```bash
pio device monitor                                                # sulla stazione
sudo tail -f /var/log/anemometer-collector/collector.log          # sul server
```

| Dove | Messaggio | Significato |
|---|---|---|
| stazione | `SIM7080: connesso`, poi `CoAP inviato: {...}` | invio riuscito |
| server | `ricevuto first_anemometer: media ...` | messaggio accettato e passato a InfluxDB |
| server | `firma errata` | la chiave è diversa tra stazione e server |
| server | `stazione "..." sconosciuta` | il nome non è in `STATION_KEYS`, o è diverso da `NODE_NAME` |
| server | `ora ... fuori finestra` | l'orologio del server o della stazione è sbagliato |
| server | `valore n ... già visto` | messaggio ripetuto, o orologio della stazione tornato indietro: si risolve da solo quando l'ora supera l'ultimo valore |
| stazione | `Sincronizzazione NTP fallita` | il modem non ottiene l'ora: controlla che NTP passi dall'APN |

Se sul server non compare nulla, controlla prima `COAP_SERVER_IP` e il firewall.

### Più stazioni e gestione delle chiavi

Ogni stazione ha nome, chiave e ultimo valore `n` propri. Il server sceglie la chiave in base al campo `id` del messaggio. Se una stazione viene compromessa, le altre restano protette: la sua chiave non permette di scrivere a nome di un'altra.

Non usare la stessa chiave o lo stesso nome su due stazioni. Con la stessa chiave ne basterebbe una compromessa per falsificarle tutte. Con lo stesso nome condividerebbero l'ultimo valore `n`, e i messaggi di una verrebbero scartati come già visti.

Per inserire una stazione segui [Aggiungere un nuovo nodo](#aggiungere-un-nuovo-nodo).

**Sostituire una chiave:** per esempio se la stazione è stata rubata. Generane una nuova, aggiornala in `secrets.h` e in `STATION_KEYS`, ricarica il firmware e riavvia il servizio. Fino al caricamento, i messaggi della stazione vengono rifiutati.

**Rimuovere una stazione:** togli la sua coppia da `STATION_KEYS` e riavvia il servizio. Da quel momento i suoi messaggi vengono rifiutati.

### Aggiungere un nuovo nodo

Il server è già installato, quindi non serve rifare il deploy. Le altre stazioni non vanno toccate.

**1. Scegli il nome e genera la chiave**

Il nome deve essere diverso da quello di tutte le altre stazioni: lettere, numeri, `_` e `-`, massimo 32 caratteri.

```bash
openssl rand -hex 32
```

Conserva la chiave in un posto sicuro, come un gestore di password, insieme al nome del nodo.

**2. Configura il firmware**

In `anemometro_sim7080_coap/secrets.h` metti la chiave del nuovo nodo. Se il file non esiste, crealo da `secrets.example.h` come al [passo 2](#2-genera-la-chiave-della-stazione).

```cpp
#define STATION_KEY "chiave_del_nuovo_nodo"
```

In `anemometro_sim7080_coap/anemometro_sim7080_coap.ino` imposta il nome e controlla gli altri parametri:

```cpp
#define NODE_NAME        "nuovo_nodo"
#define COAP_SERVER_IP   "indirizzo_del_server"
#define MODEM_APN        "apn_della_sim"
```

`secrets.h` e `NODE_NAME` valgono per una sola stazione alla volta: aggiornali entrambi prima di compilare per un'altra. Non committare un `NODE_NAME` specifico di una stazione.

**3. Carica il firmware sul nuovo nodo**

Collega la scheda del nuovo nodo, e solo quella, poi:

```bash
pio run -e sim7080 -t upload
```

**4. Aggiungi la chiave sul server**

In `/etc/default/anemometer-collector` aggiungi la coppia in fondo a `STATION_KEYS`, separata da una virgola e senza togliere le altre:

```sh
STATION_KEYS="first_anemometer:chiave_uno,nuovo_nodo:chiave_del_nuovo_nodo"
```

Il nome deve essere identico a `NODE_NAME`, e la chiave identica a quella in `secrets.h`. Poi riavvia il servizio, perché le chiavi vengono lette solo all'avvio:

```bash
sudo service anemometer-collector restart
sudo service anemometer-collector status
```

Se il servizio non parte, la causa è nell'ultima riga di `/var/log/anemometer-collector/collector.log`, ad esempio una chiave più corta di 32 caratteri. Durante il riavvio, che dura pochi secondi, un messaggio in arrivo può andare perso.

**5. Verifica**

Il primo messaggio parte 5 minuti dopo l'avvio del nodo. Sul server deve comparire:

```
ricevuto nuovo_nodo: media ...
```

Su InfluxDB il nodo si distingue dal tag `sensor_name`. Se invece compare un altro messaggio, usa la tabella al [passo 8](#8-verifica).

## Struttura del repository

| Percorso | Descrizione |
|---|---|
| `anemometro_bluetooth_stringaunica/` | Firmware base: velocità + direzione dalla banderuola, invio Meshtastic + BLE. |
| `meshtastic_davis_anemometer_compensated/` | Firmware per XIAO nRF52840 **Sense**: aggiunge bussola QMC5883L + IMU LSM6DS3 per compensare la direzione con l'orientamento del dispositivo (tilt-compensated heading). Sperimentale. |
| `anemometro_sim7080_coap/` | Firmware con modem **SIM7080G** al posto del nodo Meshtastic: invio CoAP ogni 5 minuti su rete cellulare, modem in PSM tra un invio e l'altro, BLE invariato. |
| `sim7080_serial_bridge/` | Convertitore USB-seriale per provare a mano il modem con i comandi AT. |
| `collector_coap.py` | Ricevitore CoAP del firmware SIM7080 in Python: scrive su InfluxDB. |
| `server/` | Lo stesso ricevitore in Go, consigliato per il server, con lo script di deploy per Devuan in `server/deploy/`. |
| `platformio.ini`, `pio_sketch_dir.py` | Compilazione con PlatformIO, un ambiente per ogni sketch. |
| `webapp.html` | Pagina web che si collega via BLE al sensore e mostra velocità istantanea, media, massima e direzione. |
| `collector.py` | Ricevitore: legge i messaggi dal nodo Meshtastic collegato via USB e li scrive su InfluxDB. |
| `ai/` | Prototipo di previsione basato sui dati storici in InfluxDB (pensato per una futura "boa" con pannello solare) e alert Telegram. |
| `openweather/` | Modelli di previsione basati sui dati storici di OpenWeather One Call 3.0, uno per spot (Trevignano, Anzio). |

Ogni sketch Arduino sta nella propria cartella con lo stesso nome del file `.ino`, come richiesto dall'Arduino IDE.

## Firmware

### Compilazione

Con **PlatformIO** (`platformio.ini` nella radice del repository, un ambiente per sketch):

```bash
pio run                        # firmware SIM7080 (ambiente predefinito)
pio run -e bluetooth           # anemometro_bluetooth_stringaunica
pio run -e compensated         # meshtastic_davis_anemometer_compensated, per XIAO Sense
pio run -e bridge              # sim7080_serial_bridge, convertitore per provare il modem
pio run -e sim7080_dummy       # SIM7080 con vento finto, invio ogni minuto e comandi AT sul monitor
pio run -e sim7080 -t upload   # compila e carica
pio device monitor             # monitor seriale a 115200 baud
```

Gli sketch restano compilabili anche con l'Arduino IDE. PlatformIO usa il BSP Seeed 1.1.1, l'Arduino IDE quello installato (a volte più recente).

### Hardware

- **MCU:** Seeed XIAO nRF52840 (BSP "Seeed nRF52 Boards" ≥ 1.1.0)
- **Anemometro (coppe):** contatto reed su `D2`, interrupt su fronte di discesa, con debounce software.
- **Banderuola:** potenziometro su `A3`, mappato linearmente 0–1023 → 0–360°.
- **Meshtastic:** modulo/nodo Meshtastic collegato in seriale su `RX=D7`, `TX=D6` a 38400 baud (libreria Arduino `Meshtastic.h`, funzioni `mt_serial_init` / `mt_send_text`). Il nodo deve avere il Serial Module abilitato in modalità protobuf/API.
- **Solo versione compensata:** bussola QMC5883L (I²C `0x0D`) e IMU LSM6DS3 integrata nella XIAO Sense (I²C `0x6A`).

### Funzionamento

- Un `SoftwareTimer` scatta ogni 500 ms. Ogni 2 tick (~1 s) il loop si sveglia, legge la direzione, conta le rotazioni accumulate dall'interrupt e calcola la velocità in nodi.
- Tra un campione e l'altro la CPU dorme con `__WFE()` per ridurre i consumi.
- Vengono mantenute velocità **istantanea**, **media** e **massima** del periodo; dopo ogni invio Meshtastic media e massima vengono azzerate.
- Ogni 30 tick (~15 s) viene inviato un messaggio di testo **diretto** al nodo `DEST_NODE`.
- Il BLE non resta sempre attivo: l'advertising è acceso 10 s ogni 30 s (`ADV_WINDOW_MS` / `ADV_PERIOD_MS`) finché un telefono non si connette, con potenza TX 0 dBm.

### Parametri da configurare (in testa allo sketch)

| Define | Significato |
|---|---|
| `DEST_NODE` | Node ID Meshtastic del ricevitore (es. `0x236704f7`). |
| `NODE_NAME` | Nome della stazione, finisce nel messaggio e diventa il tag `sensor_name` in InfluxDB. |
| `VaneOffset` | Offset in gradi per allineare la banderuola al nord (solo versione base). |
| `SERIAL_RX_PIN`, `SERIAL_TX_PIN`, `BAUD_RATE` | Collegamento seriale verso il nodo Meshtastic. |

### Formati dei messaggi

**Meshtastic** (testo, max 128 byte):

```
Name:first_anemometer Last:12.3 Mean:10.8 Max:15.1 kn Dir:225 SW
```

**BLE** (max 32 byte), servizio `A0010000-0001-0001-0001-000000000001`, caratteristica `A0010000-0001-0001-0001-000000000002` (read + notify), nome dispositivo `WindSensor`:

```
S:12.3 M:10.8 X:15.1 D:225 SW
```

`S` = istantanea, `M` = media, `X` = massima (nodi), `D` = direzione in gradi + punto cardinale (8 settori).

### Versione compensata (bussola + IMU)

- All'avvio scansiona il bus I²C e carica la calibrazione della bussola da LittleFS (`/compass_calib.bin`). Se non c'è, esegue una calibrazione di 30 s (ruotare il dispositivo su tutti gli assi) e la salva.
- A ogni campione sveglia IMU e bussola, calcola roll/pitch dall'accelerometro, l'heading compensato in inclinazione, e lo somma all'angolo della banderuola, poi rimette i sensori in sleep.
- **Attenzione:** al momento `setupCompass()` contiene `InternalFS.remove(CALIB_FILE)` (marcato "da rimuovere"), quindi la calibrazione viene rifatta a ogni avvio.

### Versione SIM7080 + CoAP

`anemometro_sim7080_coap/` sostituisce il nodo Meshtastic con un modem SIM7080G. Il BLE resta come nel firmware base.

> **Stato:** il firmware completo gira sulla XIAO con il modem collegato ai pin: i messaggi firmati arrivano in NB-IoT al server `anemometro.ninux.org` e finiscono su InfluxDB. La prova è stata fatta con vento finto (`sim7080_dummy`), senza anemometro e banderuola: vedi [Da verificare sull'hardware](#da-verificare-sullhardware).

#### Cosa misura e cosa invia

Velocità e direzione vengono lette ogni secondo. Ogni 5 minuti parte un messaggio con tre valori:

| Campo | Significato | Come viene calcolato |
|---|---|---|
| `mean` | velocità media (nodi) | media di tutte le letture dei 5 minuti, pesata sul tempo misurato |
| `max` | raffica massima (nodi) | valore più alto della media sugli ultimi 3 s, a finestra scorrevole aggiornata ogni secondo |
| `dir` | direzione (gradi) | media vettoriale delle letture dei 5 minuti |

Il messaggio è un CoAP POST su `coap://<server>:5683/wind`, con Content-Format `application/json`, non confermabile (NON) salvo `COAP_CONFIRMABLE`:

```json
{"id":"first_anemometer","mean":10.8,"max":15.1,"dir":225,"n":1793865182,"sig":"684020558e98f5a3ce8e48347f52966a"}
```

`id` è `NODE_NAME` e il ricevitore lo salva come tag `sensor_name`. `n` è l'ora Unix di fine misura e, con `sig`, serve all'autenticazione. Il messaggio è di circa 155 byte con le intestazioni IP/UDP, quindi circa 1,3 MB al mese, escluse la segnalazione di rete e l'overhead dell'operatore.

#### Autenticazione

Ogni messaggio è firmato con **HMAC-SHA256**, troncato a 128 bit, e ogni stazione ha la sua chiave. Il ricevitore accetta solo messaggi con firma valida, con `n` più alto dell'ultimo visto per quella stazione e con un'ora recente: non più vecchia di 15 minuti e non più avanti di 2 minuti rispetto all'ora del server.

- **Cosa protegge:** dati falsi, messaggi alterati e messaggi vecchi reinviati.
- **Cosa non protegge:** i dati viaggiano in chiaro, quindi sono leggibili da chi intercetta il traffico. Per cifrarli servirebbe DTLS o una VPN.
- **La firma:** copre tutti i byte del messaggio che precedono `,"sig":"`.
- **L'ora:** la stazione non salva nulla in flash. Dopo l'avvio chiede l'ora via NTP con `AT+CNTP`, la legge con `AT+CCLK?`, la mantiene con il proprio orologio e la risincronizza ogni 24 ore. Il server di default è `pool.ntp.org` (`NTP_SERVER`). Senza un'ora valida il report non parte. Se la risincronizzazione fallisce, continua a inviare per 7 giorni con l'ora che ha.
- **Dati e riavvii:** su InfluxDB ogni punto ha l'ora di misura, non quella di ricezione. Un riavvio non richiede nessun recupero, e il campo `n` a 32 bit basta fino al 2106.
- **Sul server:** l'ultimo `n` di ogni stazione è salvato in `STATE_FILE`, così anche subito dopo un riavvio un messaggio ancora recente non può essere riusato. Se il file è illeggibile il ricevitore non parte: cancellarlo è sicuro, a costo di accettare per 15 minuti un messaggio già visto.
- **Orologio del server:** deve essere sincronizzato, ad esempio con un client NTP, altrimenti i messaggi validi vengono scartati.
- **NTP non è autenticato:** chi manipola la rete può far sbagliare l'ora alla stazione e bloccarne gli invii, ma non può falsificare i dati. Il server scarta l'ora sbagliata senza aggiornare l'ultimo `n`, quindi gli invii riprendono appena l'ora torna corretta.

Per generare le chiavi e aggiungere stazioni vedi [Installazione](#installazione). Senza `secrets.h` lo sketch non compila. Senza chiavi sul server ogni messaggio viene rifiutato.

Chi ha accesso fisico alla stazione può leggerne la chiave: in quel caso sostituiscila.

Sul BLE `M` e `X` sono la media e la raffica del periodo in corso e si azzerano a ogni invio, mentre `S` è il valore istantaneo su 1 s.

#### Calcolo della velocità

```
V[mph] = P × 2.25 / T        P = impulsi, T = tempo misurato in secondi
V[kn]  = V[mph] × 0.868976
```

- **Formula:** è quella dell'anemometro Davis 6410 (1600 giri/ora = 1 mph), con un impulso a giro. Ogni impulso al secondo vale 1,96 kn.
- **Tempo misurato:** `T` è il tempo reale tra due letture, quindi una lettura in ritardo non crea una falsa raffica.
- **Debounce:** `DebounceMs` è 40 ms e permette di leggere fino a circa 48 kn. 35 kn corrispondono a un impulso ogni 56 ms.
- **Risoluzione:** la raffica su 3 s ha passi di circa 0,65 kn, la media su 5 minuti meno di 0,01 kn, il valore `S` su 1 s circa 2 kn.

#### Lettura della direzione

La banderuola è letta con riferimento VDD (`AR_VDD4`) sull'intera scala 0–360°, quindi **il potenziometro deve essere alimentato dal 3V3 della XIAO**. Con il riferimento predefinito (0–3,6 V) una banderuola a 3,3 V si fermerebbe attorno a 330°. Il firmware base usa ancora quel riferimento.

#### Collegamenti

Lo stesso schema vale per il firmware e per il convertitore `sim7080_serial_bridge`.

```
                         Seeed XIAO nRF52840
                       ┌─────────────────────┐
 Anemometro            │                     │            Modem SIM7080G
   contatto ──────┬────┤ D2                  │          ┌──────────────────┐
                  │    │                  D6 ├── TX ───▶│ RX               │
             pull-up   │                  D7 │◀── RX ───┤ TX               │
              10 kΩ    │                  D1 ├─────────▶│ PWRKEY           │
                  │    │                     │          │                  │
           3V3 ───┴────┤ 3V3                 │          │             VBAT │◀── alimentazione
                       │                     │          │                  │    separata
 Banderuola            │                     │          │                  │    (+)
   alimentazione ──────┤ 3V3                 │          │                  │
   cursore ────────────┤ A3                  │          │                  │
   massa ─────────┬────┤ GND                 │     ┌────┤ GND              │
                  │    └─────────────────────┘     │    └──────────────────┘
 Anemometro       │                                │
   contatto ──────┤                                │
                  └────────────────────────────────┴─── massa comune ───▶ alimentazione (−)
```

| XIAO | Collegato a | Note |
|---|---|---|
| `D2` | contatto dell'anemometro | l'altro capo va a massa. Il pin non ha il pull-up attivo nel firmware, quindi serve una resistenza esterna verso 3V3, ad esempio da 10 kΩ |
| `A3` | cursore della banderuola | il potenziometro va tra `3V3` e `GND` della XIAO, perché è letto con riferimento VDD |
| `D6` (TX) | RX del modem | UART a 115200 baud |
| `D7` (RX) | TX del modem | |
| `D1` | PWRKEY del modem | obbligatorio: serve ad accendere il modem e a risvegliarlo dal PSM |
| `GND` | massa del modem e dell'alimentazione | massa in comune obbligatoria |
| `5V` | 5 V dell'alimentazione esterna | sulla scheda provata è servito alimentare la XIAO dagli stessi 5 V del modem |

**Scheda provata:** il modulo "universale" con serigrafia SIM800/868, NB-SIM7080, CAT1-A7680X ha il connettore `DTR GND VDD PWRK UTX URX GND`. `VDD` è l'ingresso a 5 V, `URX`/`UTX` lavorano a 3,3 V e vanno direttamente a `D6`/`D7`, `PWRK` va a `D1` (`MODEM_PWRKEY_ACTIVE` `HIGH`), `DTR` resta libero. All'accensione il modem resta spento finché non riceve un impulso su PWRKEY. La UART ha funzionato solo con modem e XIAO alimentati dagli stessi 5 V esterni e massa comune; con il modem alimentato in altro modo si accendeva ma non rispondeva ad `AT`.

**Prima di dare corrente:**

- **Alimentazione del modem:** va alimentato a parte, non dal pin 3V3 della XIAO. In trasmissione ha picchi di centinaia di mA, e alcune fonti indicano fino a 2 A, quindi serve un'alimentazione adatta con un condensatore vicino al modulo. Controlla la tensione ammessa sul manuale della tua scheda.
- **Livelli logici:** il modulo SIM7080G lavora a 1,8 V, mentre la XIAO lavora a 3,3 V. La scheda provata ha già il traslatore e accetta 3,3 V: controlla la tua. Se non c'è, va aggiunto su RX, TX e PWRKEY.
- **PWRKEY:** `MODEM_PWRKEY_ACTIVE` è `HIGH` se la scheda ha un transistor sulla linea, `LOW` se il pin è collegato direttamente al modulo.
- **Antenna e SIM:** non trasmettere senza antenna collegata, e inserisci la SIM a modem spento.
- **Colori dei fili:** in un Davis 6410 dovrebbero essere nero per il contatto dell'anemometro, rosso per la massa, verde per il cursore e giallo per l'alimentazione della banderuola. Li cito a memoria: verificali con un multimetro. Il contatto dell'anemometro si chiude tra nero e rosso.

#### Parametri in testa allo sketch

| Define | Valore attuale | Significato |
|---|---|---|
| `MODEM_APN` | `iot.1nce.net` | APN della SIM |
| `MODEM_NETWORK_MODE` | `"2"` | `"2"` NB-IoT, `"3"` Cat-M e NB-IoT, `"1"` solo Cat-M |
| `COAP_SERVER_IP` | `anemometro.ninux.org` | IP o hostname del server con il ricevitore |
| `COAP_SERVER_PORT` | `5683` | Porta UDP del server |
| `NTP_SERVER` | `pool.ntp.org` | Server NTP da cui prendere l'ora |
| `NODE_NAME` | `first_anemometer` | Nome della stazione: lettere, numeri, `_` e `-`, massimo 32 caratteri |
| `STATION_KEY` | in `secrets.h` | Chiave della stazione, fuori dal repository |
| `COAP_CONFIRMABLE` | `false` | `true` usa messaggi CON e rileva anche un server spento, con una conferma per ogni messaggio |
| `REPORT_PERIOD_S` | `300` | Secondi tra due invii |
| `DebounceMs` | `40` | Debounce del contatto reed |
| `MODEM_PWRKEY_PIN` | `1` | Pin collegato a PWRKEY |
| `MODEM_PWRKEY_ACTIVE` | `HIGH` | `HIGH` con transistor sulla scheda del modem, `LOW` se PWRKEY è collegato direttamente |
| `FAILS_TO_RESET` | `3` | Errori consecutivi prima dello spegnimento completo |
| `VaneOffset` | `0` | Offset in gradi per allineare la banderuola al nord |

Nel driver `sim7080.cpp` si trovano i valori PSM richiesti alla rete (`PSM_PERIODIC_TAU`, `PSM_ACTIVE_TIME`) e la durata dell'impulso (`PWRKEY_PULSE_MS`).

#### Ciclo di invio e risparmio energetico (PSM)

Il modem non si spegne tra un invio e l'altro: resta registrato in rete e dorme con il Power Saving Mode (`AT+CPSMS`). Ho scelto il PSM e non l'eDRX perché la stazione non deve ricevere comandi dal server.

A ogni invio il firmware:

1. apre la UART e sveglia il modem con un impulso su PWRKEY, ma solo se non risponde a tre `AT` consecutivi, perché un impulso a modem acceso lo spegnerebbe;
2. controlla SIM, registrazione (`AT+CGATT?`) e PDP (`AT+CNACT`), e riattiva quello che manca;
3. inizializza il client CoAP con `AT+CCOAPINIT` e `AT+CCOAPURL`;
4. compone il pacchetto con `AT+CCOAPPARA`, con il payload in esadecimale perché le virgolette del JSON chiuderebbero la stringa del comando, e lo invia con `AT+CCOAPACTION`;
5. richiude la UART, e il modem entra da solo in PSM.

Altri dettagli:

- **Valori richiesti alla rete:** active time di 2 s e TAU periodico di 2 ore. La rete può assegnarne altri: il log mostra la riga `+CPSMRDP:`, dove il quarto valore è l'active time assegnato e 0 significa PSM non supportato. In quel caso il modem resta sveglio e funziona comunque.
- **Tecnologia e bande:** il firmware imposta NB-IoT (`MODEM_NETWORK_MODE`) e le bande 3, 8 e 20, se diverse da quelle salvate nel modem. Il modem provato aveva NB-IoT solo sulla banda 8, e non trovava rete: Vodafone Italia usa NB-IoT sulla banda 20.
- **Perché NB-IoT:** nelle prove su Vodafone con active time richiesto di 2 s, in NB-IoT la rete ha assegnato 2 s e il modem si è addormentato circa 5 s dopo l'invio. In Cat-M ha assegnato 120 s e il modem si è addormentato dopo circa 130 s. Prima di installare una stazione, controlla sul posto che NB-IoT ci sia, con `AT+CSQ` e `AT+CGATT?`. Altrimenti imposta `"3"`.
- **Segnale dopo il risveglio:** appena sveglio `AT+CSQ` riporta `99,99` anche se la radio funziona: nelle prove NTP ha risposto proprio in quel momento. Non va usato per decidere se inviare.
- **Risveglio:** nelle prove la connessione è pronta circa 10 s dopo l'impulso. Dopo il PSM `AT+CGATT?` può riportare `1` prima che la radio sia pronta, quindi il firmware ripete `AT+CNACT` finché la connessione non è attiva.
- **Quando dorme:** il modem entra in PSM solo dopo che la rete ha rilasciato la connessione e l'active time è scaduto. Prima dell'invio il firmware manda `AT+CRAI` per chiedere un rilascio rapido, che il manuale prevede solo per NB-IoT.
- **Errori:** dopo `FAILS_TO_RESET` errori consecutivi il modem viene spento con `AT+CPOWD=1` e riacceso al report successivo. È l'unico caso di spegnimento completo.
- **Campionamento:** il modem gira in un task FreeRTOS separato, quindi i comandi AT non interrompono le letture.
- **Dati persi:** non c'è un buffer. Se la rete manca o il modem è occupato, in coda resta solo l'ultimo report e gli altri vanno persi; ogni report perso vale 5 minuti di dati.
- **Consegna:** con NON un server spento non viene rilevato. Con `COAP_CONFIRMABLE` sì, ma va prima verificato sul modem come arrivano le conferme.

#### Ricevitore

```bash
pip install aiocoap influxdb-client
INFLUX_URL=http://... INFLUX_TOKEN=... python collector_coap.py
```

| Variabile d'ambiente | Predefinito | Significato |
|---|---|---|
| `STATION_KEYS` | nessuno | Chiavi delle stazioni, `nome:chiave,nome:chiave`, di almeno 32 caratteri; senza, ogni messaggio viene rifiutato |
| `STATE_FILE` | nessuno | File con l'ultimo `n` di ogni stazione; senza, dopo un riavvio un messaggio ancora recente può essere riusato |
| `INFLUX_URL` | nessuno | URL di InfluxDB; senza, stampa soltanto i dati |
| `INFLUX_TOKEN` | nessuno | Token di InfluxDB |
| `INFLUX_ORG` | `meteo_org` | Organizzazione |
| `INFLUX_BUCKET` | `anemometri` | Bucket |
| `COAP_BIND` | `::` | Indirizzo di ascolto |
| `COAP_PORT` | `5683` | Porta UDP di ascolto |

Per ogni messaggio scrive un punto nella measurement `meteo`, con i campi `wind_mean`, `wind_max`, `direction_deg` e il tag `sensor_name`. L'orario è quello di fine misura, inviato dalla stazione nel campo `n`, e i valori si riferiscono ai 5 minuti precedenti. I messaggi scartati vengono registrati nel log. Ai messaggi NON non risponde, per non consumare traffico; a quelli CON risponde `2.04`, `4.00` se il messaggio è malformato o `4.01` se la firma o l'ora non sono valide.

La porta UDP deve essere raggiungibile da Internet. Il traffico non è cifrato: per nasconderlo servirebbe una VPN o un APN privato.

Per il server è consigliata la versione in Go, descritta in [Ricevitore in Go e deploy su Devuan](#ricevitore-in-go-e-deploy-su-devuan).

#### Test sul computer

Provano il driver con un modem simulato, il payload, il calcolo della raffica, la firma e la gestione dell'ora. Non provano lo sketch `.ino` né l'hardware. Dalla cartella `anemometro_sim7080_coap/`:

```bash
g++ -std=c++17 -Itest -I. test/test_sim7080.cpp sim7080.cpp -o /tmp/test_sim7080 && /tmp/test_sim7080
```

Se su macOS non trova gli header C++, aggiungi `-isysroot $(xcrun --show-sdk-path) -I$(xcrun --show-sdk-path)/usr/include/c++/v1`.

#### Da verificare sull'hardware

**Provato con la XIAO:** registrazione in rete NB-IoT, connessione dati con `iot.1nce.net`, NTP, PSM concesso dalla rete, risveglio con PWRKEY e invio CoAP, con ricezione e firma verificate dal server e scrittura su InfluxDB.

Per provare modem e server senza sensori c'è l'ambiente `sim7080_dummy`: genera vento e direzione finti, invia ogni minuto e stampa sul monitor seriale ogni comando AT (`>>`) e ogni risposta (`<<`):

```bash
pio run -e sim7080_dummy -t upload
pio device monitor
```

La traccia si attiva con `-DSIM7080_TRACE` e i dati finti con `-DDUMMY_WIND`, quindi si possono usare anche separatamente. Alla fine ricarica `pio run -e sim7080 -t upload`.

Per provare i comandi a mano usa la XIAO come convertitore. I collegamenti restano gli stessi:

```bash
pio run -e bridge -t upload
pio device monitor
```

Scrivi un comando e premi Invio. `#pwr` dà un impulso su PWRKEY, che accende il modem oppure lo spegne se è già acceso: prova prima `AT`, e usalo solo se non risponde. Il primo `AT` dopo l'accensione va perso, perché il modem lo usa per riconoscere la velocità. `#baud 9600` cambia la velocità verso il modem, `#help` mostra l'aiuto. Con il firmware della stazione la XIAO non fa da convertitore, quindi alla fine ricarica `pio run -e sim7080 -t upload`.

Questa è la sequenza da provare:

```
AT                                         deve rispondere OK
AT+CGATT?                                  deve rispondere +CGATT: 1
AT+CNCFG=0,1,"iot.1nce.net"                con l'APN della tua SIM
AT+CNACT=0,1                               attendi +APP PDP: 0,ACTIVE
AT+CNTP="pool.ntp.org",0,0,0
AT+CNTP                                    attendi +CNTP: 1 (61-65 sono errori)
AT+CCLK?                                   deve riportare l'ora UTC corrente
AT+CCOAPINIT
AT+CCOAPURL="coap://indirizzo_del_server:5683"
AT+CCOAPPARA="CODE",2,"TYPE","NON","MID",1,"URI-PATH",0,"wind","CONTENT-FORMAT",50,"PAYLOAD",1,"7b7d"
AT+CCOAPACTION                             deve rispondere +CCOAPACTION: 0,1
AT+CCOAPTERM
```

Il payload `7b7d` è un JSON vuoto, quindi il ricevitore lo scarta. Se nel suo log compare `scartato da ...: firma mancante`, l'invio funziona. Sul modem provato `AT+CCOAPPARA` risponde `ERROR` con `MID` oltre 9999, quindi il driver lo riporta sotto 10000.

Il riferimento per i comandi è il *SIM7070/SIM7080/SIM7090 Series AT Command Manual V1.05*, capitolo 18 per il CoAP, che non è incluso nel repository. Restano da verificare questi punti:

1. **Errore DNS di NTP:** al risveglio dal PSM `AT+CNTP` a volte risponde `+CNTP: 62`. Il firmware ora ritenta fino a 3 volte, ma i nuovi tentativi non sono ancora stati visti all'opera.
2. **Invio durante il PSM:** su circa 8 cicli uno è andato perso: dopo una nuova registrazione in rete il modem è entrato in PSM durante `AT+CCOAPACTION`, senza risposta.
3. **Segnale:** nella prova `AT+CSQ` dava 0–4, molto debole. Controlla antenna e posizione.
4. **Conferme CON:** il firmware presume che la risposta del server arrivi come `+CCOAPRECV`. Se non è così, con `COAP_CONFIRMABLE` ogni invio risulterebbe fallito.
5. **Impulsi per giro e rimbalzi:** un giro lento delle coppe deve dare un solo impulso, anche con il debounce a 40 ms.
6. **Taratura:** la formula vale per un vero Davis. Un confronto con un anemometro portatile è il controllo più affidabile.
7. **Consumo:** misura la corrente del modem per vedere quanto consuma davvero nelle varie fasi.
8. **PWRKEY collegato direttamente:** con un filo tra `D1` e PWRKEY, a seconda della scheda, il modem può accendersi da solo e riavviarsi ogni 16 s circa. Se succede, controlla `MODEM_PWRKEY_ACTIVE`.

Sul monitor seriale un ciclo riuscito mostra `SIM7080: connesso`, poi `CoAP inviato: {...}` e infine `SIM7080: in standby, entrera' in PSM`. Il primo messaggio parte 5 minuti dopo l'avvio: per una prova veloce abbassa `REPORT_PERIOD_S`, ad esempio a 30, oppure usa l'ambiente `sim7080_dummy`, ma per verificare il PSM serve un periodo più lungo.

## Ricevitore in Go e deploy su Devuan

`server/` è la versione in Go di `collector_coap.py`: stesso messaggio, stesse variabili d'ambiente, stessi dati su InfluxDB. In più:

- **Binario unico:** è statico, quindi sul server non servono Python né librerie.
- **InfluxDB fermo:** i punti restano in memoria con il loro orario e vengono riscritti quando torna raggiungibile. La coda contiene 2048 punti, circa una settimana per una stazione; a coda piena si perdono i più vecchi. Se il server viene fermato mentre InfluxDB è irraggiungibile, i punti in coda vanno persi.
- **Autenticazione:** come la versione Python, verifica firma e ora, con `STATION_KEYS` e `STATE_FILE`.

Serve Go 1.22 o più recente sul computer da cui si fa il deploy. Il token non compare mai sulla riga di comando del processo.

### Deploy

```bash
cd server
deploy/deploy.sh utente@server              # linux/amd64
deploy/deploy.sh -a arm64 utente@server     # arm64 oppure armv7
deploy/deploy.sh -p 2222 utente@server      # altra porta SSH
```

Lo script compila il binario, lo copia via SSH ed esegue `install.sh` sul server, con `sudo` se l'utente non è root. Si può rilanciare per aggiornare: la configurazione esistente non viene mai sovrascritta.

Devuan usa di default sysvinit, quindi il servizio è uno script in `/etc/init.d`. Sul server vengono creati:

| Percorso | Contenuto |
|---|---|
| `/usr/local/bin/anemometer-collector` | Binario |
| `/etc/init.d/anemometer-collector` | Servizio, avviato al boot |
| `/etc/default/anemometer-collector` | Configurazione con token e chiavi, permessi `640` |
| `/var/lib/anemometer-collector/counters.json` | Ultimo valore `n` di ogni stazione, conservato tra un deploy e l'altro |
| `/var/log/anemometer-collector/collector.log` | Log, ruotato ogni settimana da `/etc/logrotate.d/anemometer-collector` |

Il servizio gira con l'utente di sistema `anemometer`, senza privilegi di root.

Dopo il primo deploy:

1. inserisci `INFLUX_URL`, `INFLUX_TOKEN` e `STATION_KEYS` in `/etc/default/anemometer-collector`: finché `STATION_KEYS` è vuoto ogni messaggio viene rifiutato, e finché `INFLUX_URL` è vuoto i dati finiscono solo nel log;
2. riavvia con `service anemometer-collector restart`;
3. apri la porta UDP 5683 sul firewall del server, perché lo script non tocca il firewall.

Per controllare il servizio: `service anemometer-collector status` e `tail -f /var/log/anemometer-collector/collector.log`.

### Test del server

```bash
cd server
go test ./...
```

## Web app BLE

`webapp.html` è una pagina statica senza dipendenze. Va aperta in un browser con supporto **Web Bluetooth** (Chrome/Edge su Android o desktop; non Safari su iOS) e servita da `https://` o `localhost`. Premendo *Connetti* cerca il dispositivo `WindSensor`, legge la caratteristica una volta al secondo e mostra i valori.

## Collector (Meshtastic → InfluxDB)

```bash
pip install meshtastic influxdb-client pypubsub
python collector.py
```

- Si collega al nodo Meshtastic via USB (`SerialInterface()` con autodetect della porta).
- Considera solo i messaggi `TEXT_MESSAGE_APP` **non broadcast** che rispettano il formato sopra.
- Scrive un punto nella measurement `meteo` con tag `sensor_name` e campi `wind_last`, `wind_mean`, `wind_max`, `direction_deg`, `direction_text`.
- Configurare `INFLUX_URL`, `INFLUX_TOKEN`, `INFLUX_ORG`, `INFLUX_BUCKET` in testa al file (default bucket: `anemometri`).

## Previsione con OpenWeather (`openweather/`)

Script quasi identici, uno per spot:

| Script | Spot | Modello |
|---|---|---|
| `main_trevignano.py` | Trevignano (lago di Bracciano) 42.156, 12.246 | `wing_owm_model_trevignano.pkl` |
| `main_anzio.py` | Anzio 41.450, 12.633 | `wing_owm_model_anzio.pkl` |
| `main.py` | prima versione (stesse coordinate di Trevignano) | `wing_owm_model.pkl` |

Configurazione: un file `openweather/.env` con

```
OPENWEATHER_API_KEY=la_tua_chiave
```

(serve un abbonamento One Call API 3.0).

Uso, dalla cartella `openweather/`:

```bash
python -m venv venv && source venv/bin/activate
pip install requests pandas numpy scikit-learn joblib python-dotenv

python main_anzio.py --train   # scarica 30 giorni × 24 ore di storico e addestra il modello
python main_anzio.py           # monitoraggio: ogni 10 minuti stampa la probabilità tra 3 ore
python analizza_modello.py     # importanza delle feature del modello (file impostato in MODEL_FILE)
```

Logica del modello:

- **Feature:** `wind_speed`, `wind_gust` (nodi), `wind_deg`, `pressure`, `temp`, `uvi`.
- **Etichetta "si vola":** vento medio ≥ 10 kn **oppure** raffica ≥ 12 kn.
- **Target:** l'etichetta 3 ore dopo (`shift(-3)` su dati orari).
- **Classificatore:** `RandomForestClassifier(n_estimators=100)`.
- Il download si interrompe da solo se l'API risponde 429 (limite chiamate); il training usa i dati scaricati fino a quel punto.

## Prototipo AI su dati della stazione (`ai/`)

Bozza pensata per una boa con pannello solare da 5 V che invia dati ogni 10 s su InfluxDB (bucket `boa_wing`, measurement `weather_station`, campi `wind_speed`, `wind_gust`, `solar_current`, `solar_voltage`, `pressure`, `temp_air`).

- `lib.py`: costruisce un indice di irraggiamento dal pannello solare, applica medie mobili su 5 minuti, etichetta "si vola" con media ≥ 11 kn e addestra una Random Forest per prevedere la condizione **a 60 minuti**.
- `train.py`: addestra sugli ultimi 90 giorni e salva `wing_forecaster_v2.pkl`.
- `monitor.py`: ogni 5 minuti calcola la previsione e, se la probabilità supera il 75%, invia un alert **Telegram** (al massimo uno all'ora).

Questi campi e questo bucket **non** coincidono con quelli scritti da `collector.py`: il codice è un prototipo non ancora collegato al resto.

## Problemi noti / TODO

- I firmware Meshtastic calcolano ancora `rotazioni × 0.9 × 0.868976` con campioni da 1 s, mentre la formula Davis darebbe 2.25 al posto di 0.9. Leggono quindi 2,5 volte meno del firmware SIM7080.
- Il commit `73c061b` (9 ottobre 2026) ha portato il campione da circa 0,5 s a 1 s senza cambiare la formula. A parità di vento i dati su InfluxDB hanno quindi tre scale diverse, da non mescolare senza correggerle:

  | Dati | kn per impulso al secondo |
  |---|---|
  | Firmware Meshtastic fino al 9 ottobre 2026 | circa 0,39 |
  | Firmware Meshtastic dopo il 9 ottobre 2026 | 0,78 |
  | Firmware SIM7080 | 1,96 |

  Le soglie e i modelli basati sui dati vecchi, come gli 11 kn in `ai/lib.py`, vanno rivisti.
- Anche `max` cambia significato: nel firmware SIM7080 è una raffica su 3 s, nei firmware Meshtastic il picco del singolo campione.
- Il firmware base legge la banderuola con il riferimento 0–3,6 V: se è alimentata a 3,3 V, la direzione si ferma attorno a 330°.
- I settori cardinali non sono simmetrici (S fino a 212° invece di 202°).
- `ai/monitor.py` fa `status, prob = predict_now(...)`, ma `predict_now` restituisce solo una stringa.
- `analizza_modello.py` ha il nome del modello cablato nel codice; gli script OpenWeather sono duplicati e potrebbero diventare un unico script con lo spot come parametro.
- Credenziali (InfluxDB, Telegram) sono segnaposto nel codice: meglio spostarle in `.env`. Il `.gitignore` esclude già `.env` e `venv/`.
