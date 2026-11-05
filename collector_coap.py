"""
Ricevitore CoAP per anemometro_sim7080_coap: POST su /wind ogni 5 minuti con
payload JSON {"id": nome_stazione, "mean": kn, "max": kn, "dir": gradi,
"n": contatore, "sig": firma}.

Accetta solo i messaggi firmati con le chiavi di STATION_KEYS. Senza INFLUX_URL nell'ambiente stampa soltanto i dati ricevuti.
"""
import asyncio
import hashlib
import hmac
import json
import math
import os
import re
import tempfile

import aiocoap
import aiocoap.resource as resource
from influxdb_client import InfluxDBClient, Point, WriteOptions

# --- CONFIGURAZIONE ---
INFLUX_URL = os.environ.get("INFLUX_URL")
INFLUX_TOKEN = os.environ.get("INFLUX_TOKEN")
INFLUX_ORG = os.environ.get("INFLUX_ORG", "meteo_org")
INFLUX_BUCKET = os.environ.get("INFLUX_BUCKET", "anemometri")
COAP_BIND = os.environ.get("COAP_BIND", "::")
COAP_PORT = int(os.environ.get("COAP_PORT", "5683"))
STATION_KEYS = os.environ.get("STATION_KEYS", "")  # nome:chiave,nome:chiave
STATE_FILE = os.environ.get("STATE_FILE", "")      # contatori dei messaggi già visti

NAME_PATTERN = re.compile(r"^[A-Za-z0-9_-]{1,32}$")
SIGNED_PATTERN = re.compile(rb'^(.*),"sig":"([0-9a-f]{32})"\}$', re.DOTALL)
SIGNATURE_HEX_CHARS = 32
MIN_KEY_LENGTH = 32


class Unauthorized(Exception):
    pass


class Malformed(Exception):
    pass


def parse_station_keys(spec):
    """Una chiave corta si indovina: meglio non partire che credersi protetti."""
    keys = {}
    for entry in filter(None, (e.strip() for e in spec.split(","))):
        name, _, key = entry.partition(":")
        if not NAME_PATTERN.match(name) or name in keys:
            raise ValueError(f"voce di STATION_KEYS non valida per la stazione {name!r}")
        if len(key) < MIN_KEY_LENGTH:
            raise ValueError(f"chiave della stazione {name!r} troppo corta")
        keys[name] = key.encode()
    return keys


def parse_report(payload):
    """Ritorna il report e il contatore. La firma viene controllata da Auth."""
    try:
        data = json.loads(payload)
        name, counter = data["id"], data["n"]
        mean, max_, direction = float(data["mean"]), float(data["max"]), int(data["dir"])
    except (ValueError, KeyError, TypeError):
        raise Malformed("campi non validi") from None
    if not isinstance(name, str) or not NAME_PATTERN.match(name):
        raise Malformed("nome stazione non valido")
    if not isinstance(counter, int) or isinstance(counter, bool) or not 0 <= counter <= 0xFFFFFFFF:
        raise Malformed("contatore non valido")
    if not (math.isfinite(mean) and math.isfinite(max_)) or mean < 0 or max_ < 0:
        raise Malformed("velocità non valida")
    if not 0 <= direction <= 360:
        raise Malformed("direzione fuori intervallo")
    return {"name": name, "mean": mean, "max": max_, "dir_deg": direction}, counter


class Auth:
    """Verifica la firma HMAC e impedisce di riusare un messaggio già visto."""

    def __init__(self, keys, state_path):
        self.keys = keys
        self.state_path = state_path
        self.counters = {}
        if state_path and os.path.exists(state_path):
            # Un file illeggibile blocca l'avvio: azzerare i contatori riaprirebbe ai vecchi messaggi
            with open(state_path, encoding="utf-8") as f:
                self.counters = json.load(f)

    def verify(self, payload):
        # La firma copre i byte esatti ricevuti prima del campo sig
        match = SIGNED_PATTERN.match(payload)
        if not match:
            raise Malformed("firma mancante o malformata")
        signed, signature = match.group(1), match.group(2).decode()

        report, counter = parse_report(payload)
        key = self.keys.get(report["name"])
        if key is None:
            raise Unauthorized(f"stazione {report['name']!r} sconosciuta")
        expected = hmac.new(key, signed, hashlib.sha256).hexdigest()[:SIGNATURE_HEX_CHARS]
        if not hmac.compare_digest(expected, signature):
            raise Unauthorized(f"firma errata per {report['name']!r}")

        last = self.counters.get(report["name"], 0)
        if counter <= last:
            raise Unauthorized(f"contatore {counter} già visto per {report['name']!r} (ultimo {last})")
        self.counters[report["name"]] = counter
        try:
            self.save()
        except OSError as e:
            # Il dato è autentico e vale 5 minuti di misure: si tiene e si segnala
            print(f"Contatori non salvati: {e}", flush=True)
        return report

    def save(self):
        if not self.state_path:
            return
        # File temporaneo e poi rinomina: un guasto a metà non corrompe i contatori
        directory = os.path.dirname(os.path.abspath(self.state_path))
        with tempfile.NamedTemporaryFile("w", dir=directory, delete=False, encoding="utf-8") as f:
            json.dump(self.counters, f)
            f.flush()
            os.fsync(f.fileno())
        os.replace(f.name, self.state_path)


class WindResource(resource.Resource):
    def __init__(self, auth, write_api):
        super().__init__()
        self.auth = auth
        self.write_api = write_api

    async def render_post(self, request):
        confirmable = request.mtype == aiocoap.CON
        try:
            report = self.auth.verify(request.payload)
        except (Malformed, Unauthorized) as e:
            print(f"Scartato da {request.remote.hostinfo}: {e}: {request.payload[:200]!r}", flush=True)
            code = aiocoap.UNAUTHORIZED if isinstance(e, Unauthorized) else aiocoap.BAD_REQUEST
            return aiocoap.Message(code=code) if confirmable else aiocoap.message.NoResponse

        if self.write_api:
            point = Point("meteo") \
                .tag("sensor_name", report["name"]) \
                .field("wind_mean", report["mean"]) \
                .field("wind_max", report["max"]) \
                .field("direction_deg", report["dir_deg"])
            self.write_api.write(bucket=INFLUX_BUCKET, record=point)
        print(f"Ricevuto {report['name']}: media {report['mean']}kn max {report['max']}kn dir {report['dir_deg']}", flush=True)

        # Ai messaggi NON non si risponde: consumerebbe solo traffico dati
        return aiocoap.Message(code=aiocoap.CHANGED) if confirmable else aiocoap.message.NoResponse


async def main():
    write_api = None
    if INFLUX_URL:
        client = InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)
        write_api = client.write_api(write_options=WriteOptions(batch_size=1))

    keys = parse_station_keys(STATION_KEYS)
    if not keys:
        print("STATION_KEYS non impostato: tutti i messaggi verranno rifiutati", flush=True)
    if not STATE_FILE:
        print("STATE_FILE non impostato: dopo un riavvio i vecchi messaggi tornano validi", flush=True)

    site = resource.Site()
    site.add_resource(["wind"], WindResource(Auth(keys, STATE_FILE), write_api))
    await aiocoap.Context.create_server_context(site, bind=(COAP_BIND, COAP_PORT))
    print(f"Server CoAP in ascolto sulla porta {COAP_PORT}", flush=True)
    await asyncio.get_running_loop().create_future()


if __name__ == "__main__":
    asyncio.run(main())
