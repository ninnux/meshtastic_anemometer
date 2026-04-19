import joblib
import pandas as pd

# Nome del file che hai già creato
MODEL_FILE = "wing_owm_model_anzio.pkl"

def analizza_importanza():
    try:
        # 1. Carichiamo il modello dal file
        model = joblib.load(MODEL_FILE)
        
        # 2. Definiamo le feature nello stesso ordine in cui le abbiamo date al modello
        # (L'ordine deve essere identico a quello usato durante model.fit)
        features = ['wind_speed', 'wind_gust', 'wind_deg', 'pressure', 'temp', 'uvi']
        
        # 3. Estraiamo le importanze
        importanze = model.feature_importances_
        
        # 4. Creiamo un DataFrame per visualizzarle meglio
        df_importanza = pd.DataFrame({
            'Grandezza': features,
            'Peso_Percentuale': importanze * 100
        })
        
        # 5. Ordiniamo dal più importante
        df_importanza = df_importanza.sort_values(by='Peso_Percentuale', ascending=False)
        
        print("\n--- ANALISI A POSTERIORI DEL MODELLO ---")
        print(f"Modello analizzato: {MODEL_FILE}")
        print("-" * 40)
        
        for index, row in df_importanza.iterrows():
            # Creiamo una piccola barra visiva per rendere l'idea
            bar_length = int(row['Peso_Percentuale'] / 2)
            bar = "█" * bar_length
            print(f"{row['Grandezza'].upper():<12} | {row['Peso_Percentuale']:>6.2f}% {bar}")
            
        print("-" * 40)
        
        # Un piccolo commento basato sui risultati
        top_feature = df_importanza.iloc[0]['Grandezza']
        print(f"La variabile che domina la tua previsione è: {top_feature.upper()}")
        
    except FileNotFoundError:
        print(f"Errore: Il file {MODEL_FILE} non esiste. Verifica il nome!")
    except Exception as e:
        print(f"Errore durante l'analisi: {e}")

if __name__ == "__main__":
    analizza_importanza()
