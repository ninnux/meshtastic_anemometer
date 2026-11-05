"""
Ricevitore CoAP per anemometro_sim7080_coap: POST su /wind ogni 5 minuti con
payload JSON {"id": nome_stazione, "mean": kn, "max": kn, "dir": gradi}.

Senza INFLUX_URL nell'ambiente stampa soltanto i dati ricevuti.
"""
import asyncio
import json
import math
import os
import re

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

NAME_PATTERN = re.compile(r"^[A-Za-z0-9_-]{1,32}$")


def parse_report(payload):
    """Ritorna il report validato, oppure None se il messaggio non è valido."""
    try:
        data = json.loads(payload)
        name = data["id"]
        mean, max_, direction = float(data["mean"]), float(data["max"]), int(data["dir"])
    except (ValueError, KeyError, TypeError):
        return None
    if not isinstance(name, str) or not NAME_PATTERN.match(name):
        return None
    if not (math.isfinite(mean) and math.isfinite(max_)) or mean < 0 or max_ < 0:
        return None
    if not 0 <= direction <= 360:
        return None
    return {"name": name, "mean": mean, "max": max_, "dir_deg": direction}


class WindResource(resource.Resource):
    def __init__(self, write_api):
        super().__init__()
        self.write_api = write_api

    async def render_post(self, request):
        confirmable = request.mtype == aiocoap.CON
        report = parse_report(request.payload)
        if report is None:
            print(f"Messaggio non valido da {request.remote.hostinfo}: {request.payload!r}", flush=True)
            return aiocoap.Message(code=aiocoap.BAD_REQUEST) if confirmable else aiocoap.message.NoResponse

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

    site = resource.Site()
    site.add_resource(["wind"], WindResource(write_api))
    await aiocoap.Context.create_server_context(site, bind=(COAP_BIND, COAP_PORT))
    print(f"Server CoAP in ascolto sulla porta {COAP_PORT}", flush=True)
    await asyncio.get_running_loop().create_future()


if __name__ == "__main__":
    asyncio.run(main())
