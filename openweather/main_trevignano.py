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
LAT = "42.1556597"
LON = "12.2461345"
MODEL_FILE = "wing_owm_model_trevignano.pkl"

# --- 1. DOWNLOAD DATI STORICI ---
def get_historical_data(days=30):
    print(f"Scaricamento dati ({days}gg, 24h/gg) con campi: Vento, Gust, UV, Pressione, Temp...")
    all_data = []
    base_date = datetime.now().replace(hour=0, minute=0, second=0, microsecond=0)
    
    for d in range(1, days + 1):
        target_day = base_date - timedelta(days=d)
        print(f"Recupero: {target_day.strftime('%d-%m-%Y')}...", end="\r")
        
        for h in range(0, 24):
            ts = int((target_day + timedelta(hours=h)).timestamp())
            url = f"https://api.openweathermap.org/data/3.0/onecall/timemachine?lat={LAT}&lon={LON}&dt={ts}&appid={API_KEY}&units=metric"
            
            try:
                response = requests.get(url)
                if response.status_code == 429: return pd.DataFrame(all_data)
                
                data = response.json()
                if "data" in data and len(data["data"]) > 0:
                    h_data = data["data"][0]
                    
                    # Estrazione sicura dei campi (wind_gust può mancare)
                    all_data.append({
                        'time': h_data['dt'],
                        'temp': h_data['temp'],
                        'pressure': h_data['pressure'],
                        'wind_speed': h_data['wind_speed'] * 1.94384,
                        'wind_gust': h_data.get('wind_gust', h_data['wind_speed']) * 1.94384,
                        'wind_deg': h_data['wind_deg'],
                        'uvi': h_data.get('uvi', 0) # L'indice UV
                    })
            except Exception: continue
            
    print("\nDownload completato.")
    return pd.DataFrame(all_data)

# --- 2. TRAINING ---
def train_owm_model():
    df = get_historical_data(days=30)
    if df.empty: return

    df = df.sort_values('time')
    # Target: Volo OK se la raffica o la velocità media superano i 10 nodi
    df['is_good'] = ((df['wind_speed'] >= 10) | (df['wind_gust'] >= 12)).astype(int)
    
    # Prevediamo a 3 ore
    df['target_future'] = df['is_good'].shift(-3)
    df.dropna(inplace=True)

    # Lista aggiornata delle Feature
    features = ['wind_speed', 'wind_gust', 'wind_deg', 'pressure', 'temp', 'uvi']
    
    X = df[features]
    y = df['target_future']

    print(f"Training su {len(df)} righe. Distribuzione classi:\n{y.value_counts()}")

    model = RandomForestClassifier(n_estimators=100, random_state=42)
    model.fit(X, y)
    joblib.dump(model, MODEL_FILE)
    print(f"Modello salvato!")

# --- 3. MONITORAGGIO ---
def monitor_and_predict():
    if not os.path.exists(MODEL_FILE): return
    model = joblib.load(MODEL_FILE)
    features_names = ['wind_speed', 'wind_gust', 'wind_deg', 'pressure', 'temp', 'uvi']
    
    print("Monitoraggio attivo...")

    while True:
        try:
            url = f"https://api.openweathermap.org/data/3.0/onecall?lat={LAT}&lon={LON}&exclude=minutely,hourly,daily,alerts&appid={API_KEY}&units=metric"
            r = requests.get(url).json()
            curr = r['current']

            # Creazione dataframe per la predizione con i nuovi campi
            current_df = pd.DataFrame([{
                'wind_speed': curr['wind_speed'] * 1.94384,
                'wind_gust': curr.get('wind_gust', curr['wind_speed']) * 1.94384,
                'wind_deg': curr['wind_deg'],
                'pressure': curr['pressure'],
                'temp': curr['temp'],
                'uvi': curr.get('uvi', 0)
            }])

            probs = model.predict_proba(current_df)[0]
            prob = probs[1] if len(probs) > 1 else (1.0 if model.classes_[0] == 1 else 0.0)

            print(f"[{datetime.now().strftime('%H:%M')}] Pressure: {current_df['pressure'].iloc[0]:.1f} | Temp: {current_df['temp'].iloc[0]:.1f}gradi | Vento: {current_df['wind_speed'].iloc[0]:.1f}kt | UV: {current_df['uvi'].iloc[0]} | Prob. tra 3h: {prob*100:.1f}%")
            
        except Exception as e:
            print(f"Errore: {e}")

        time.sleep(600)

if __name__ == "__main__":
    import sys
    if len(sys.argv) > 1 and sys.argv[1] == "--train":
        train_owm_model()
    else:
        monitor_and_predict()
