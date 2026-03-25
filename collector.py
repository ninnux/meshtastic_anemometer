import re
import meshtastic
import meshtastic.serial_interface
from influxdb_client import InfluxDBClient, Point, WriteOptions
from pubsub import pub

# --- CONFIGURAZIONE ---
INFLUX_URL = "http://IP-DEL-TUO-LXC:8086"
INFLUX_TOKEN = "IL_TUO_TOKEN_QUI"
INFLUX_ORG = "meteo_org"
INFLUX_BUCKET = "anemometri"

# Setup Client
client = InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)
write_api = client.write_api(write_options=WriteOptions(batch_size=1))

def parse_anemometer_text(text):
    """
    Estrae i dati dalla stringa: 
    Name:%s Last:%.1f Mean:%.1f Max:%.1f kn Dir:%d %s
    """
    # Pattern regex aggiornato per catturare esattamente la tua struttura
    pattern = r"Name:(\S+) Last:([\d.]+) Mean:([\d.]+) Max:([\d.]+) kn Dir:(\d+) (\S+)"
    match = re.search(pattern, text)
    
    if match:
        return {
            "name": match.group(1),
            "last": float(match.group(2)),
            "mean": float(match.group(3)),
            "max":  float(match.group(4)),
            "dir_deg": int(match.group(5)),
            "dir_str": match.group(6)
        }
    return None

def on_receive(packet, interface):
    try:
        if 'decoded' in packet and packet['decoded']['portnum'] == 'TEXT_MESSAGE_APP':
            # Filtro per messaggi diretti (non broadcast)
            if packet.get('to') != 4294967295:
                raw_text = packet['decoded']['payload'].decode('utf-8')
                data = parse_anemometer_text(raw_text)
                
                if data:
                    # Costruzione del punto InfluxDB
                    point = Point("meteo") \
                        .tag("sensor_name", data["name"]) \
                        .field("wind_last", data["last"]) \
                        .field("wind_mean", data["mean"]) \
                        .field("wind_max", data["max"]) \
                        .field("direction_deg", data["dir_deg"]) \
                        .field("direction_text", data["dir_str"]) # Ora è un FIELD
                    
                    write_api.write(bucket=INFLUX_BUCKET, record=point)
                    print(f"Salvato {data['name']}: {data['last']}kn {data['dir_str']}")
                else:
                    # Log utile per capire se la stringa cambia formato
                    print(f"Formato non riconosciuto: {raw_text}")

    except Exception as e:
        print(f"Errore durante l'elaborazione: {e}")

# Inizializzazione interfaccia (USB/Serial)
try:
    INTERFACE = meshtastic.serial_interface.SerialInterface()
    pub.subscribe(on_receive, "meshtastic.receive")
    print("Sistema di ricezione anemometri avviato. In attesa di messaggi...")
except Exception as e:
    print(f"Impossibile connettersi al nodo Meshtastic: {e}")

while True:
    import time
    time.sleep(1)  
