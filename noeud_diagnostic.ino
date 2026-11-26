/*
 * =====================================================================
 *  NOEUD DE DIAGNOSTIC  (MAITRE I2C) - "valise du garagiste"
 * =====================================================================
 *  - Interroge le noeud principal (adresse 0x55) toutes les secondes
 *        SDA -> IO21           SCL -> IO22          + GND commun
 *  - Affiche son etat de fonctionnement sur le moniteur serie
 *  - Aucun capteur, aucun Wi-Fi
 * =====================================================================
 */

#include <Wire.h>

#define I2C_DEV_ADDR   0x55
#define PIN_SDA        21
#define PIN_SCL        22
#define PERIODE_MS     1000

// ---------------------------------------------------------------------
//  TABLE DES CODES ERREUR  (IDENTIQUE dans le noeud principal)
// ---------------------------------------------------------------------
#define ERR_WIFI_LOST        1
#define ERR_HR_DISCONNECT    2
#define ERR_DHT_DISCONNECT   3
#define ERR_NO_FINGER        4
#define ERR_TEMP_LIMIT       5
#define ERR_HR_ABNORMAL      6
#define ERR_LOW_MEMORY       7
#define NB_ERR               8

const char* getErrorMessage(uint8_t code) {
  switch (code) {
    case ERR_WIFI_LOST:      return "Impossible de se connecter au Wifi";
    case ERR_HR_DISCONNECT:  return "Probleme liaison capteur cardiaque (MAX30102)";
    case ERR_DHT_DISCONNECT: return "Probleme liaison capteur temperature (DHT11)";
    case ERR_NO_FINGER:      return "Capteur cardiaque : doigt non detecte";
    case ERR_TEMP_LIMIT:     return "Temperature hors limites";
    case ERR_HR_ABNORMAL:    return "Rythme cardiaque anormal";
    case ERR_LOW_MEMORY:     return "Memoire insuffisante";
    default:                 return "Code erreur non repertorie";
  }
}

// ---------------------------------------------------------------------
//  FICHE D'ETAT  (IDENTIQUE des deux cotes)
// ---------------------------------------------------------------------
struct __attribute__((packed)) TrameDiag {
  uint8_t  magic;
  uint16_t erreurs;
  int16_t  temp10;
  int16_t  hum10;
  int16_t  bpm;
  int16_t  spo2;
  uint32_t uptime;
  uint32_t memLibre;
  uint8_t  checksum;
};

uint8_t echecs = 0;   // nombre de demandes ratees d'affilee

// ---------------------------------------------------------------------
//  INITIALISATION
// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1000);
  Wire.begin(PIN_SDA, PIN_SCL, 100000);
  Wire.setTimeOut(50);
  Serial.println("=== NOEUD DE DIAGNOSTIC (maitre) ===");
}

// ---------------------------------------------------------------------
//  BOUCLE PRINCIPALE
// ---------------------------------------------------------------------
void loop() {
  delay(PERIODE_MS);

  TrameDiag t;
  bool ok = false;

  size_t n = Wire.requestFrom((uint16_t)I2C_DEV_ADDR, sizeof(TrameDiag));
  if (n == sizeof(TrameDiag)) {
    Wire.readBytes((uint8_t*)&t, n);
    uint8_t* p = (uint8_t*)&t;
    uint8_t  c = 0;
    for (size_t i = 0; i < sizeof(TrameDiag) - 1; i++) c ^= p[i];
    ok = (t.magic == 0xAA && c == t.checksum);
  }
  while (Wire.available()) Wire.read();   // vide ce qui reste

  Serial.println("-----------------------------");

  // 1. Le noeud principal ne repond pas
  if (!ok) {
    echecs++;
    if (echecs >= 2)
      Serial.println("[ERREUR] Noeud principal ne repond pas (liaison I2C coupee ou noeud eteint)");
    else
      Serial.println("[ATTENTION] Reponse invalide, nouvel essai...");
    return;
  }
  echecs = 0;

  Serial.printf("Noeud principal actif depuis %lu s | memoire libre : %lu octets\n",
                (unsigned long)t.uptime, (unsigned long)t.memLibre);

  // 2. Etat general
  if (t.erreurs == 0) {
    Serial.println("Fonctionnement nominal : acquisition capteur en cours");
  } else {
    for (uint8_t code = 1; code < NB_ERR; code++) {
      if (t.erreurs & (1 << code)) {
        Serial.printf("[ERREUR %d] %s\n", code, getErrorMessage(code));
      }
    }
  }

  // 3. Dernieres mesures valides
  if (!(t.erreurs & (1 << ERR_DHT_DISCONNECT))) {
    Serial.printf("Temperature : %.1f C | Humidite : %.1f %%\n",
                  t.temp10 / 10.0, t.hum10 / 10.0);
  }
  if (!(t.erreurs & ((1 << ERR_HR_DISCONNECT) | (1 << ERR_NO_FINGER)))) {
    Serial.printf("Rythme cardiaque : %d bpm | SpO2 : %d %%\n", t.bpm, t.spo2);
  }
}
