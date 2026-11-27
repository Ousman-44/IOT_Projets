
#include <Wire.h>

#define I2C_DEV_ADDR  0x55
#define PIN_SDA       21
#define PIN_SCL       22
#define PERIODE_MS    1000

// Codes d'erreur
#define ERR_WIFI_LOST        1
#define ERR_HR_DISCONNECT    2
#define ERR_DHT_DISCONNECT   3
#define ERR_NO_FINGER        4
#define ERR_TEMP_LIMIT       5
#define ERR_HR_ABNORMAL      6
#define ERR_LOW_MEMORY       7
#define ERR_MQTT_LOST        8
#define NB_ERR               9

const char* getErrorMessage(uint8_t code) {
  switch (code) {
    case ERR_WIFI_LOST:
      return "Impossible de se connecter au Wifi";
    case ERR_HR_DISCONNECT:
      return "Probleme liaison MAX30102";
    case ERR_DHT_DISCONNECT:
      return "Probleme liaison DHT11";
    case ERR_NO_FINGER:
      return "Doigt non detecte";
    case ERR_TEMP_LIMIT:
      return "Temperature hors limites";
    case ERR_HR_ABNORMAL:
      return "Rythme cardiaque anormal";
    case ERR_LOW_MEMORY:
      return "Memoire insuffisante";
    default:
      return "Code erreur non repertorie";
  }
}

// Structure identique sur les deux ESP32
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

static_assert(sizeof(TrameDiag) == 20, "Taille trame incorrecte");

uint8_t echecs = 0;

void setup() {
  Serial.begin(9600);
  delay(1000);

  Serial.println("=== DIAGNOSTIC I2C - NOUVELLE VERSION ===");

  if (!Wire.begin(PIN_SDA, PIN_SCL, 100000)) {
    Serial.println("[ERREUR] Initialisation I2C impossible");
    while (true) {
      delay(1000);
    }
  }

  Wire.setTimeOut(50);

  Serial.println("Bus I2C initialise");
  Serial.println("SDA : GPIO 21");
  Serial.println("SCL : GPIO 22");
  Serial.println("Adresse esclave : 0x55");
  Serial.printf("Taille trame : %u octets\n",
                (unsigned int)sizeof(TrameDiag));
}

void loop() {
  delay(PERIODE_MS);

  Serial.println("--------------------------------");

  // 1. Verifier la presence de l'esclave
  Wire.beginTransmission(I2C_DEV_ADDR);
  uint8_t erreur = Wire.endTransmission();

  Serial.printf("Test adresse I2C 0x55 : %d\n", erreur);

  if (erreur != 0) {
    if (echecs < 255) echecs++;

    switch (erreur) {
      case 2:
        Serial.println("[ERREUR] Adresse 0x55 non reconnue");
        break;

      case 3:
        Serial.println("[ERREUR] Donnees non acquittees");
        break;

      case 4:
        Serial.println("[ERREUR] Communication I2C");
        break;

      case 5:
        Serial.println("[ERREUR] Timeout I2C");
        break;

      default:
        Serial.println("[ERREUR] Esclave inaccessible");
        break;
    }

    Serial.printf("Echecs consecutifs : %d\n", echecs);
    return;
  }

  Serial.println("[OK] Noeud principal detecte");

  // 2. Demander la trame
  TrameDiag t = {};

  size_t n = Wire.requestFrom(
    (uint16_t)I2C_DEV_ADDR,
    sizeof(TrameDiag)
  );

  Serial.printf("Octets recus : %u / %u\n",
                (unsigned int)n,
                (unsigned int)sizeof(TrameDiag));

  if (n != sizeof(TrameDiag)) {
    Serial.println("[ERREUR] Taille de trame incorrecte");

    while (Wire.available()) {
      Wire.read();
    }

    return;
  }

  size_t lus = Wire.readBytes((uint8_t*)&t, n);

  if (lus != sizeof(TrameDiag)) {
    Serial.println("[ERREUR] Lecture incomplete");

    while (Wire.available()) {
      Wire.read();
    }

    return;
  }

  // 3. Calculer le checksum
  uint8_t* p = (uint8_t*)&t;
  uint8_t c = 0;

  for (size_t i = 0; i < sizeof(TrameDiag) - 1; i++) {
    c ^= p[i];
  }

  Serial.printf("Magic recu : 0x%02X\n", t.magic);
  Serial.printf("Checksum recu : 0x%02X\n", t.checksum);
  Serial.printf("Checksum calcule : 0x%02X\n", c);

  // 4. Verifier la validite de la trame
  if (t.magic != 0xAA) {
    Serial.println("[ERREUR] Identifiant magic incorrect");
    return;
  }

  if (c != t.checksum) {
    Serial.println("[ERREUR] Checksum incorrect");
    return;
  }

  echecs = 0;

  Serial.println("[OK] Trame I2C valide");

  // 5. Afficher l'etat du noeud principal
  Serial.printf("Temps actif : %lu s\n",
                (unsigned long)t.uptime);

  Serial.printf("Memoire libre : %lu octets\n",
                (unsigned long)t.memLibre);

  // 6. Afficher les codes d'erreur
  if (t.erreurs == 0) {
    Serial.println("Fonctionnement nominal");
  } else {
    for (uint8_t code = 1; code < NB_ERR; code++) {
      if (t.erreurs & (1 << code)) {
        Serial.printf("[ERREUR %d] %s\n",
                      code, getErrorMessage(code));
      }
    }
  }

  // 7. Afficher les mesures
  if (!(t.erreurs & (1 << ERR_DHT_DISCONNECT))) {
    Serial.printf("Temperature : %.1f C\n", t.temp10 / 10.0);
    Serial.printf("Humidite : %.1f %%\n", t.hum10 / 10.0);
  }

  if (!(t.erreurs & ((1 << ERR_HR_DISCONNECT) |
                     (1 << ERR_NO_FINGER)))) {
    Serial.printf("Rythme cardiaque : %d bpm\n", t.bpm);
    Serial.printf("SpO2 : %d %%\n", t.spo2);
  }
}
