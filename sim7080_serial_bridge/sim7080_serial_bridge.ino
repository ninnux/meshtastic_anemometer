// ============================================================
// sim7080_serial_bridge.ino
// Board: Seeed XIAO nRF52840
// BSP:   Seeed nRF52 Boards >= 1.1.0
//
// Convertitore USB-seriale per provare a mano il modem SIM7080G.
// Usa gli stessi collegamenti del firmware anemometro_sim7080_coap.
// ============================================================

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>  // la seriale USB; senza Bluefruit va inclusa qui
#include "bridge_line.h"

// ── Modem (Serial1: RX=D7, TX=D6) ────────────────────────────
#define MODEM_BAUD_RATE     115200
#define MODEM_PWRKEY_PIN    1
#define MODEM_PWRKEY_ACTIVE HIGH  // HIGH con transistor sulla scheda, LOW se diretto
#define PWRKEY_PULSE_MS     1200

LineBuffer line;

void printHelp() {
  Serial.println();
  Serial.println("Convertitore seriale per SIM7080G. Scrivi un comando AT e premi Invio.");
  Serial.println("  #pwr   impulso su PWRKEY: accende il modem, oppure lo spegne se e' acceso");
  Serial.println("  #baud N  velocita' verso il modem (es. #baud 9600), di base 115200");
  Serial.println("  #help  mostra questo aiuto");
  Serial.println("Se ogni comando appare due volte, invia ATE0 per togliere l'eco del modem.");
  Serial.println();
}

void pulsePwrkey() {
  Serial.println("Impulso su PWRKEY...");
  pinMode(MODEM_PWRKEY_PIN, OUTPUT);
  digitalWrite(MODEM_PWRKEY_PIN, MODEM_PWRKEY_ACTIVE);
  delay(PWRKEY_PULSE_MS);
  // Attivo basso: a riposo la linea resta flottante, mai forzata a 3.3 V
  if (MODEM_PWRKEY_ACTIVE == LOW) pinMode(MODEM_PWRKEY_PIN, INPUT);
  else                            digitalWrite(MODEM_PWRKEY_PIN, LOW);
  Serial.println("Fatto.");
}

// Cambia la velocita' verso il modem: in autobaud il modem si adatta
// al primo "AT", quindi a velocita' basse si vede se il segnale e' debole
void setBaud(const char* arg) {
  long baud = atol(arg);
  if (baud < 1200 || baud > 921600) {
    Serial.println("Uso: #baud 9600 (da 1200 a 921600)");
    return;
  }
  Serial1.end();
  Serial1.begin(baud);
  Serial.print("Seriale modem a ");
  Serial.println(baud);
}

void runLocalCommand(const char* command) {
  if (strcmp(command, "#pwr") == 0)              pulsePwrkey();
  else if (strncmp(command, "#baud ", 6) == 0)   setBaud(command + 6);
  else                                           printHelp();
}

void setup() {
  // Con il transistor la linea flottante potrebbe premere PWRKEY
  if (MODEM_PWRKEY_ACTIVE != LOW) {
    pinMode(MODEM_PWRKEY_PIN, OUTPUT);
    digitalWrite(MODEM_PWRKEY_PIN, LOW);
  }

  Serial.begin(115200);
  while (!Serial) delay(10);  // altrimenti l'aiuto andrebbe perso

  Serial1.begin(MODEM_BAUD_RATE);
  printHelp();
}

void loop() {
  while (Serial1.available()) Serial.write(Serial1.read());

  while (Serial.available()) {
    char c = Serial.read();
    switch (line.feed(c)) {
      case LineEvent::Stored:
        Serial.write(c);
        break;
      case LineEvent::Erased:
        Serial.print("\b \b");
        break;
      case LineEvent::Complete:
        Serial.println();
        if (line.text()[0] == '#') {
          runLocalCommand(line.text());
        } else {
          Serial1.print(line.text());
          Serial1.print("\r");
        }
        line.clear();
        break;
      case LineEvent::TooLong:
        Serial.println();
        Serial.println("Riga troppo lunga per il modem: non inviata.");
        break;
      case LineEvent::None:
        break;
    }
  }

  delay(1);  // cede la CPU
}
