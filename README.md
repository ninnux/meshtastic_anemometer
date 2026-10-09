# meshtastic_anemometer

Stazione anemometrica low-power per il wing foil, basata su **Seeed XIAO nRF52840** e un anemometro a coppe con banderuola (tipo Davis). Le misure di vento vengono inviate:

- via **LoRa/Meshtastic** (messaggio di testo diretto a un nodo ricevente, ogni ~15 s);
- via **Bluetooth LE** a uno smartphone, letto da una semplice web app (Web Bluetooth).

Lato server, uno script Python riceve i messaggi Meshtastic e li salva su **InfluxDB**. A corredo ci sono degli esperimenti di **machine learning** (Random Forest) per stimare la probabilità di avere vento "navigabile" nelle ore successive, sia a partire dai dati della stazione sia dai dati di **OpenWeather**.

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

## Struttura del repository

| Percorso | Descrizione |
|---|---|
| `anemometro_bluetooth_stringaunica/` | Firmware base: velocità + direzione dalla banderuola, invio Meshtastic + BLE. |
| `meshtastic_davis_anemometer_compensated/` | Firmware per XIAO nRF52840 **Sense**: aggiunge bussola QMC5883L + IMU LSM6DS3 per compensare la direzione con l'orientamento del dispositivo (tilt-compensated heading). Sperimentale. |
| `webapp.html` | Pagina web che si collega via BLE al sensore e mostra velocità istantanea, media, massima e direzione. |
| `collector.py` | Ricevitore: legge i messaggi dal nodo Meshtastic collegato via USB e li scrive su InfluxDB. |
| `ai/` | Prototipo di previsione basato sui dati storici in InfluxDB (pensato per una futura "boa" con pannello solare) e alert Telegram. |
| `openweather/` | Modelli di previsione basati sui dati storici di OpenWeather One Call 3.0, uno per spot (Trevignano, Anzio). |

Ogni sketch Arduino sta nella propria cartella con lo stesso nome del file `.ino`, come richiesto dall'Arduino IDE.

## Firmware

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

- Nel firmware base `Rotations / 3.0f` e poi `* 3.0f` si annullano: la formula della velocità resta `rotazioni × 0.9 × 0.868976` (0.868976 = conversione mph → nodi). Da verificare la costante 0.9 rispetto all'anemometro usato e al periodo di campionamento reale.
- I settori cardinali non sono simmetrici (S fino a 212° invece di 202°).
- `ai/monitor.py` fa `status, prob = predict_now(...)`, ma `predict_now` restituisce solo una stringa.
- `analizza_modello.py` ha il nome del modello cablato nel codice; gli script OpenWeather sono duplicati e potrebbero diventare un unico script con lo spot come parametro.
- Credenziali (InfluxDB, Telegram) sono segnaposto nel codice: meglio spostarle in `.env`. Il `.gitignore` esclude già `.env` e `venv/`.
