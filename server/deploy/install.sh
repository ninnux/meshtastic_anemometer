#!/bin/sh
# Installa o aggiorna anemometer-collector. Da eseguire come root sul server, dalla
# cartella che contiene il binario e i file di configurazione.
set -eu

# SSH inoltra la lingua del client: se manca sul server, update-rc.d riempie l'output di avvisi
LC_ALL=C
export LC_ALL

NAME=anemometer-collector
USER_NAME=anemometer

if [ "$(id -u)" -ne 0 ]; then
	echo "install.sh va eseguito come root" >&2
	exit 1
fi

if ! id "$USER_NAME" >/dev/null 2>&1; then
	useradd --system --user-group --no-create-home --home-dir /nonexistent \
		--shell /usr/sbin/nologin "$USER_NAME"
fi

# Sostituzione atomica: il binario può essere in esecuzione
install -m 0755 -o root -g root "$NAME" "/usr/local/bin/.$NAME.new"
mv -f "/usr/local/bin/.$NAME.new" "/usr/local/bin/$NAME"

install -m 0755 -o root -g root "$NAME.init" "/etc/init.d/$NAME"
install -m 0644 -o root -g root "$NAME.logrotate" "/etc/logrotate.d/$NAME"
install -d -m 0750 -o "$USER_NAME" -g adm "/var/log/$NAME"
# L'ultimo valore visto impedisce di riusare un messaggio recente: va conservato tra un deploy e l'altro
install -d -m 0750 -o "$USER_NAME" -g "$USER_NAME" "/var/lib/$NAME"

# La configurazione esistente non viene mai sovrascritta: contiene il token
if [ ! -e "/etc/default/$NAME" ]; then
	install -m 0640 -o root -g "$USER_NAME" "$NAME.default" "/etc/default/$NAME"
	echo "Creato /etc/default/$NAME: inserisci i dati di InfluxDB."
fi

if ! grep -q '^STATION_KEYS="[^"]' "/etc/default/$NAME"; then
	echo "ATTENZIONE: STATION_KEYS è vuoto in /etc/default/$NAME: ogni messaggio verrà rifiutato."
fi

update-rc.d "$NAME" defaults >/dev/null
service "$NAME" restart
service "$NAME" status
