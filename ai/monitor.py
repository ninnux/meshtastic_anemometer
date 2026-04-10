import time
import requests
from lib import client, BUCKET, predict_now

# Configurazione Telegram
TELEGRAM_TOKEN = "IL_TUO_TELEGRAM_BOT_TOKEN"
CHAT_ID = "IL_TUO_CHAT_ID"

def send_telegram_msg(text):
    url = f"https://api.telegram.org/bot{TELEGRAM_TOKEN}/sendMessage"
    payload = {"chat_id": CHAT_ID, "text": text}
    requests.post(url, json=payload)

def run_monitoring():
    print("Sistema di monitoraggio avviato...")
    last_alert_sent = 0 # Per evitare di inviare 100 messaggi al minuto

    while True:
        try:
            # 1. Recuperiamo gli ultimi dati mediati dalla boa (ultimi 5 minuti)
            query = f'''
            from(bucket: "{BUCKET}")
              |> range(start: -5m)
              |> filter(fn: (r) => r["_measurement"] == "weather_station")
              |> mean()
              |> pivot(rowKey:["_start"], columnKey: ["_field"], valueColumn: "_value")
            '''
            result = client.query_api().query_data_frame(query)

            if not result.empty:
                # Prepariamo i dati per il modello
                current_data = {
                    'wind_smooth': result['wind_speed'].iloc[0],
                    'gust_smooth': result['wind_gust'].iloc[0],
                    'solar_smooth': result['solar_current'].iloc[0], # O l'indice calcolato
                    'pressure': result['pressure'].iloc[0],
                    'temp_air': result['temp_air'].iloc[0]
                }

                # 2. Chiediamo al cervello cosa succederà tra un'ora
                status, prob = predict_now(current_data)

                # 3. Se la probabilità è > 75% e non abbiamo mandato alert di recente
                if prob > 0.75 and (time.time() - last_alert_sent > 3600):
                    msg = f"🏄‍♂️ WING ALERT!\n{status}\nProbabilità: {prob*100:.0f}%\nVento attuale: {current_data['wind_smooth']:.1f} kn"
                    send_telegram_msg(msg)
                    last_alert_sent = time.time()
                    print("Alert inviato!")

        except Exception as e:
            print(f"Errore durante il monitoraggio: {e}")

        # Aspetta 5 minuti prima del prossimo controllo
        time.sleep(300)

if __name__ == "__main__":
    run_monitoring()
