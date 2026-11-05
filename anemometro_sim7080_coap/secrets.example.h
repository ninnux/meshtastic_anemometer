// Copia questo file in secrets.h, che git ignora, e metti la chiave di questa stazione.
// Genera la chiave con:  openssl rand -hex 32
// Sul server la stessa chiave va in STATION_KEYS, insieme a NODE_NAME.
#pragma once

#error "Inserisci la chiave generata qui sotto, poi cancella questa riga"
#define STATION_KEY "inserisci-qui-la-chiave"
