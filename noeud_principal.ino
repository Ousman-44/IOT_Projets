/*
 * =====================================================================
 *  NOEUD PRINCIPAL  (ESCLAVE I2C, adresse 0x55)
 * =====================================================================
 *  - Capteur cardiaque DFRobot SEN0518 (MAX30102), mode I2C, sur Wire1
 *        D/T -> IO25 (D2)      C/R -> IO26 (D3)     interrupteur sur "I2C"
 *  - Capteur DHT11 sur IO4 (broche "DO" a DROITE - PAS "D0" a gauche !)
 *  - Liaison avec le noeud de diagnostic sur Wire
 *        SDA -> IO21           SCL -> IO22          + GND commun
 *  - Wi-Fi (non bloquant) pour pouvoir signaler une perte du Wifi
 *
 *  Le noeud principal tient a jour une "fiche d'etat" (TrameDiag) et
 *  l'envoie quand le noeud de diagnostic la demande.
 * =====================================================================
 */

#include <Wire.h>
#include <WiFi.h>
#include <DHT.h>
#include "DFRobot_BloodOxygen_S.h"

// ---------------------------------------------------------------------
//  PARAMETRES
// ---------------------------------------------------------------------
#define I2C_DEV_ADDR   0x55      // adresse de ce noeud sur le bus diagnostic
#define PIN_SDA_DIAG   21
#define PIN_SCL_DIAG   22

#define PIN_SDA_HR     25        // D2  -> D/T du capteur cardiaque
#define PIN_SCL_HR     26        // D3  -> C/R du capteur cardiaque
#define ADR_MAX30102   0x57

#define PIN_DHT        4         // "DO" / IO4  (connecteur de droite)
#define TYPE_DHT       DHT11

// Mettre 0 pour tester sans Wi-Fi (l'erreur Wifi ne sera alors jamais levee)
#define WIFI_ACTIF     0
// const char* WIFI_SSID = "VOTRE_SSID";
// const char* WIFI_MDP  = "VOTRE_MOT_DE_PASSE";

// Seuils
#define TEMP_MIN       10.0      // en dessous : temperature hors limites
#define TEMP_MAX       40.0      // au dessus  : temperature hors limites
#define BPM_MIN        40
#define BPM_MAX        180
#define MEMOIRE_MIN    20000     // octets

// ---------------------------------------------------------------------
//  TABLE DES CODES ERREUR  (IDENTIQUE dans le noeud de diagnostic)
//  Chaque code correspond a un bit : plusieurs erreurs peuvent etre
//  actives en meme temps.
// ---------------------------------------------------------------------
#define ERR_WIFI_LOST        1
#define ERR_HR_DISCONNECT    2
#define ERR_DHT_DISCONNECT   3
#define ERR_NO_FINGER        4
#define ERR_TEMP_LIMIT       5
#define ERR_HR_ABNORMAL      6
#define ERR_LOW_MEMORY       7
#define NB_ERR               8

// ---------------------------------------------------------------------
//  FICHE D'ETAT envoyee au diagnostic  (IDENTIQUE des deux cotes)
// ---------------------------------------------------------------------
struct __attribute__((packed)) TrameDiag {
  uint8_t  magic;      // 0xAA = trame valide
  uint16_t erreurs;    // bit n = code erreur n actif
  int16_t  temp10;     // temperature x10
  int16_t  hum10;      // humidite x10
  int16_t  bpm;
  int16_t  spo2;
  uint32_t uptime;     // secondes depuis le demarrage
  uint32_t memLibre;   // memoire libre (octets)
  uint8_t  checksum;   // XOR de tous les octets precedents
};

// ---------------------------------------------------------------------
//  OBJETS ET VARIABLES
// ---------------------------------------------------------------------
DHT dht(PIN_DHT, TYPE_DHT);
DFRobot_BloodOxygen_S_I2C MAX30102(&Wire1, ADR_MAX30102);

uint16_t erreurs = 0;
int16_t  temp10 = 0, hum10 = 0, bpm = 0, spo2 = 0;
bool     maxDemarre = false;

uint8_t      trameEnvoi[sizeof(TrameDiag)];          // fiche prete a partir
portMUX_TYPE verrou = portMUX_INITIALIZER_UNLOCKED;  // protege trameEnvoi

// ---------------------------------------------------------------------
//  OUTILS
// ---------------------------------------------------------------------
void setErreur(uint8_t code, bool actif) {
  if (actif) erreurs |=  (1 << code);
  else       erreurs &= ~(1 << code);
}

bool capteurCardiaqueRepond() {
  Wire1.beginTransmission(ADR_MAX30102);
  return Wire1.endTransmission() == 0;
}

// Construit la fiche d'etat et la copie dans le tampon d'envoi
void preparerTrame() {
  TrameDiag t;
  t.magic    = 0xAA;
  t.erreurs  = erreurs;
  t.temp10   = temp10;
  t.hum10    = hum10;
  t.bpm      = bpm;
  t.spo2     = spo2;
  t.uptime   = millis() / 1000;
  t.memLibre = ESP.getFreeHeap();

  uint8_t* p = (uint8_t*)&t;
  uint8_t  c = 0;
  for (size_t i = 0; i < sizeof(TrameDiag) - 1; i++) c ^= p[i];
  t.checksum = c;

  portENTER_CRITICAL(&verrou);
  memcpy(trameEnvoi, &t, sizeof(TrameDiag));
  portEXIT_CRITICAL(&verrou);
}

// ---------------------------------------------------------------------
//  CALLBACK I2C : le diagnostic demande la fiche d'etat
//  (pas de Serial.print ici : la reponse doit partir tout de suite)
// ---------------------------------------------------------------------
void onRequest() {
  uint8_t copie[sizeof(TrameDiag)];
  portENTER_CRITICAL(&verrou);
  memcpy(copie, trameEnvoi, sizeof(copie));
  portEXIT_CRITICAL(&verrou);
  Wire.write(copie, sizeof(copie));
}

// ---------------------------------------------------------------------
//  INITIALISATION
// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== NOEUD PRINCIPAL (esclave 0x55) ===");

  // Capteur de temperature
  dht.begin();

  // Bus du capteur cardiaque (Wire1, maitre)
  Wire1.begin(PIN_SDA_HR, PIN_SCL_HR, 100000);
  Wire1.setTimeOut(50);

  // Wi-Fi non bloquant : on lance la connexion et on continue
#if WIFI_ACTIF
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_MDP);
  Serial.println("Connexion Wi-Fi lancee (non bloquante)");
#endif

  // Bus du diagnostic (Wire, esclave)
  preparerTrame();                     // une fiche valide existe des le depart
  Wire.onRequest(onRequest);
  Wire.begin((uint8_t)I2C_DEV_ADDR, PIN_SDA_DIAG, PIN_SCL_DIAG, 100000);
  Serial.println("Esclave I2C pret");
}

// ---------------------------------------------------------------------
//  BOUCLE PRINCIPALE  (aucun delay : tout est cadence avec millis())
// ---------------------------------------------------------------------
void loop() {
  unsigned long maintenant = millis();
  static unsigned long tPresence = 0, tMax = 0, tDht = 0, tWifi = 0, tAffiche = 0;

  // 1. Capteur cardiaque branche ?  (toutes les 1 s)
  if (maintenant - tPresence >= 1000) {
    tPresence = maintenant;
    if (!capteurCardiaqueRepond()) {
      if (maxDemarre) Serial.println("MAX30102 : DEBRANCHE");
      maxDemarre = false;
      bpm = 0;
      spo2 = 0;
    } else if (!maxDemarre) {
      // (re)demarrage du capteur apres branchement
      if (MAX30102.begin()) {
        MAX30102.sensorStartCollect();
        maxDemarre = true;
        tMax = maintenant;
        Serial.println("MAX30102 : demarre, posez le doigt");
      }
    }
    setErreur(ERR_HR_DISCONNECT, !maxDemarre);
  }

      // 2. Lecture rythme cardiaque : on interroge le module toutes les 1 s
  //    pour recuperer sa nouvelle valeur des qu'elle est prete
  if (maxDemarre && maintenant - tMax >= 1000) {
    tMax = maintenant;
    MAX30102.getHeartbeatSPO2();
    int b = MAX30102._sHeartbeatSPO2.Heartbeat;
    int s = MAX30102._sHeartbeatSPO2.SPO2;

    bool doigt = (b > 0 && s > 0);
    bpm  = doigt ? b : 0;            // retombe a 0 des que le doigt est retire
    spo2 = doigt ? s : 0;
    setErreur(ERR_NO_FINGER, !doigt);
    setErreur(ERR_HR_ABNORMAL, doigt && (bpm < BPM_MIN || bpm > BPM_MAX));
  }

  // 3. Lecture DHT11  (toutes les 2 s)
  if (maintenant - tDht >= 2000) {
    tDht = maintenant;
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    bool ok = !isnan(t) && !isnan(h);
    setErreur(ERR_DHT_DISCONNECT, !ok);
    if (ok) {
      temp10 = (int16_t)(t * 10);
      hum10  = (int16_t)(h * 10);
      setErreur(ERR_TEMP_LIMIT, t < TEMP_MIN || t > TEMP_MAX);
    } else {
      setErreur(ERR_TEMP_LIMIT, false);
    }
  }

  // 4. Wi-Fi  (verification + nouvel essai toutes les 10 s si coupe)
#if WIFI_ACTIF
  bool wifiOK = (WiFi.status() == WL_CONNECTED);
  setErreur(ERR_WIFI_LOST, !wifiOK);
  if (!wifiOK && maintenant - tWifi >= 10000) {
    tWifi = maintenant;
    WiFi.reconnect();
  }
#endif

  // 5. Memoire
  setErreur(ERR_LOW_MEMORY, ESP.getFreeHeap() < MEMOIRE_MIN);

  // 6. Mise a jour de la fiche envoyee au diagnostic
  preparerTrame();

  // 7. Affichage local (moniteur serie du noeud principal), toutes les 4 s
  if (maintenant - tAffiche >= 1000) {
    tAffiche = maintenant;
    Serial.printf("T=%.1f C  H=%.1f %%  BPM=%d  SpO2=%d %%  erreurs=0x%04X\n",
                  temp10 / 10.0, hum10 / 10.0, bpm, spo2, erreurs);
  }
}
