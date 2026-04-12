import requests
import pandas as pd
import numpy as np
import time
import joblib
from sklearn.ensemble import RandomForestClassifier
from datetime import datetime, timedelta
import sys
import os
from dotenv import load_dotenv


# --- CARICAMENTO CONFIGURAZIONE ESTERNA ---
load_dotenv() # Carica le variabili dal file .env
API_KEY = os.getenv("OPENWEATHER_API_KEY")
# --- CONFIGURAZIONE ---
LAT = "42.1556597"  # Le coordinate del tuo spot
LON = "12.2461345"
MODEL_FILE = "wing_owm_model.pkl"

# --- 1. DOWNLOAD DATI STORICI (Ricalibrato per 24 ore/giorno) ---
def get_historical_data(days=1):
    print(f"Scaricamento dati storici per gli ultimi {days} giorni (24 letture al giorno)...")
    all_data = []
    
    # Calcoliamo la fine del periodo (ieri a mezzanotte)
    base_date = datetime.now().replace(hour=0, minute=0, second=0, microsecond=0)
    
    for d in range(1, days + 1):
        target_day = base_date - timedelta(days=d)
        print(f"Recupero dati del: {target_day.strftime('%d-%m-%Y')}")
        
        # Facciamo una richiesta per ogni ora del giorno
        for h in range(0, 24):
            # Timestamp preciso per ogni ora
            ts = int((target_day + timedelta(hours=h)).timestamp())
            
            url = f"https://api.openweathermap.org/data/3.0/onecall/timemachine?lat={LAT}&lon={LON}&dt={ts}&appid={API_KEY}&units=metric"
            
            try:
                response = requests.get(url)
                # Se superi i limiti free (1000 chiamate/giorno), qui riceverai errore
                if response.status_code == 429:
                    print("Limite chiamate raggiunto! Salvo quello che ho preso finora.")
                    return pd.DataFrame(all_data)
                
                data = response.json()
                if "data" in data and len(data["data"]) > 0:
                    hour_data = data["data"][0] # Prende la singola ora restituita
                    all_data.append({
                        'time': hour_data['dt'],
                        'temp': hour_data['temp'],
                        'pressure': hour_data['pressure'],
                        'humidity': hour_data['humidity'],
                        'wind_speed': hour_data['wind_speed'] * 1.94384,
                        'wind_deg': hour_data['wind_deg'],
                        'clouds': hour_data['clouds']
                    })
            except Exception as e:
                print(f"Errore al timestamp {ts}: {e}")
                continue
                
    return pd.DataFrame(all_data)



# --- 2. PREPARAZIONE E TRAINING ---
def train_owm_model():
    df = get_historical_data(days=30) # OWM Free ha limiti sui giorni storici
    df = df.sort_values('time')

    # Definiamo il "Volo OK" (Target attuale)
    # Usiamo velocità vento > 12 nodi (più alto perché OWM è meno preciso della boa)
    df['is_good'] = (df['wind_speed'] >= 10).astype(int)

    # SHIFT: Prevediamo tra 3 ore (OpenWeather fornisce dati orari nello storico)
    df['target_future'] = df['is_good'].shift(-3)
    df.dropna(inplace=True)

    features = ['temp', 'pressure', 'humidity', 'wind_speed', 'wind_deg', 'clouds']
    X = df[features]
    y = df['target_future']
    print("Distribuzione classi nel training set:")
    print(df['is_good'].value_counts())
    
    if len(df['is_good'].unique()) < 2:
        print("ATTENZIONE: Il dataset contiene solo una classe. La previsione non sarà affidabile.")

    model = RandomForestClassifier(n_estimators=100)
    model.fit(X, y)
    joblib.dump(model, MODEL_FILE)
    print("Modello OWM addestrato!")

# --- 3. MONITORAGGIO REAL-TIME ---
def monitor_and_predict():
    model = joblib.load(MODEL_FILE)
    print("Monitoraggio avviato tramite OpenWeather...")

    while True:
        # Recupera meteo attuale
        url = f"https://api.openweathermap.org/data/3.0/onecall?lat={LAT}&lon={LON}&exclude=minutely,hourly,daily,alerts&appid={API_KEY}&units=metric"
        data = requests.get(url).json()['current']

        current_features = pd.DataFrame([{
            'temp': data['temp'],
            'pressure': data['pressure'],
            'humidity': data['humidity'],
            'wind_speed': data['wind_speed'] * 1.94384,
            'wind_deg': data['wind_deg'],
            'clouds': data['clouds']
        }])

        prob = model.predict_proba(current_features)[0][1]
        print(f"[{datetime.now().strftime('%H:%M')}] Probabilità vento tra 3 ore: {prob*100:.1f}%")

        if prob > 0.75:
            print("!!! ALERT: Condizioni in arrivo !!!")
            # Qui inseriresti l'invio Telegram

        time.sleep(600) # Controlla ogni 10 minuti

if __name__ == "__main__":
    #train_owm_model() # Scommenta la prima volta per addestrare
    monitor_and_predict()
