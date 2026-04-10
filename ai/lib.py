import pandas as pd
import numpy as np
from influxdb_client import InfluxDBClient
from sklearn.ensemble import RandomForestClassifier
import joblib

# --- CONFIGURAZIONE ---
INFLUX_URL = "http://localhost:8086"
TOKEN = "TUO_TOKEN"
ORG = "TUA_ORG"
BUCKET = "boa_wing"

client = InfluxDBClient(url=INFLUX_URL, token=TOKEN, org=ORG)

def clean_and_feature_engineering(df):
    df = df.sort_values('_time')
    
    # 1. GESTIONE PANNELLO 5V
    # Soglia V_OPEN_CIRCUIT impostata a 5.8V per pannelli da 5V nominali
    df['solar_index'] = np.where(
        df['solar_current'] > 0.02, # Soglia minima corrente
        df['solar_current'], 
        np.where(df['solar_voltage'] >= 5.8, df['solar_current'].max(), 0)
    )
    
    # 2. SMOOTHING AGGRESSIVO (Dati ogni 10 sec sono molto nervosi)
    # Media mobile su 30 campioni (5 minuti di dati) per stabilizzare
    df['solar_smooth'] = df['solar_index'].rolling(window=30).mean()
    df['wind_smooth'] = df['wind_speed'].rolling(window=30).mean()
    df['gust_smooth'] = df['wind_gust'].rolling(window=30).mean()
    
    # 3. AUTO-LABELING (Sessione OK)
    # Usiamo la media mobile per l'etichetta: si vola se la media degli ultimi 5 min è > 11 nodi
    df['is_currently_good'] = (df['wind_smooth'] >= 11).astype(int)
    
    # 4. PREVISIONE A 60 MINUTI (Shift di 360 posizioni)
    df['target_future'] = df['is_currently_good'].shift(-360)
    
    df.dropna(subset=['solar_smooth', 'target_future'], inplace=True)
    return df

def train_wing_model():
    # Recupero dati (ultimi 60-90 giorni)
    query = f'from(bucket:"{BUCKET}") |> range(start: -90d) |> pivot(rowKey:["_time"], columnKey: ["_field"], valueColumn: "_value")'
    df_raw = client.query_api().query_data_frame(query)
    
    df = clean_and_feature_engineering(df_raw)
    
    # Caratteristiche per il modello
    features = ['wind_smooth', 'gust_smooth', 'solar_smooth', 'pressure', 'temp_air']
    X = df[features]
    y = df['target_future']
    
    # Addestramento
    model = RandomForestClassifier(n_estimators=100, n_jobs=-1) # n_jobs=-1 usa tutti i processori
    model.fit(X, y)
    
    joblib.dump(model, 'wing_forecaster_v2.pkl')
    print("Modello High-Frequency addestrato e salvato.")

def predict_now(current_raw_data):
    """
    Riceve i dati istantanei e applica lo smoothing prima di predire.
    In un caso reale, dovresti passare la MEDIA degli ultimi minuti.
    """
    model = joblib.load('wing_forecaster_v2.pkl')
    
    # Il modello si aspetta i nomi delle feature usate nel training
    input_data = pd.DataFrame([current_raw_data])
    
    prob = model.predict_proba(input_data)[0][1]
    
    if prob > 0.7:
        return f"ALZA LE VELE! Probabilità vento tra 1 ora: {prob*100:.0f}%"
    else:
        return f"ATTENDI. Probabilità vento tra 1 ora: {prob*100:.0f}%"
