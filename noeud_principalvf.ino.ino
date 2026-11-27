/*
 * =====================================================================
 *  NOEUD PRINCIPAL  (ESCLAVE I2C, adresse 0x55)  +  MQTT
 * =====================================================================
 *  - Capteur cardiaque DFRobot SEN0518 (MAX30102), mode I2C, sur Wire1
 *        D/T -> IO25 (D2)      C/R -> IO26 (D3)     interrupteur sur "I2C"
 *  - Capteur DHT11 sur IO4 (broche "DO" a DROITE - PAS "D0" a gauche !)
 *  - Liaison avec le noeud de diagnostic sur Wire
 *        SDA -> IO21           SCL -> IO22          + GND commun
 *  - Wi-Fi + MQTT (non bloquants) : envoi des mesures au Raspberry Pi
 *
 *  Le noeud principal tient a jour une "fiche d'etat" (TrameDiag) et
 *  l'envoie quand le noeud de diagnostic la demande.
 *
 *  Bibliotheques : DFRobot_BloodOxygen_S, DHT sensor library (Adafruit),
 *                  Adafruit Unified Sensor, PubSubClient (Nick O'Leary)
 * =====================================================================
 */

#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>
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

// Wi-Fi : OBLIGATOIRE pour MQTT -> laisser a 1
#define WIFI_ACTIF     1
const char* WIFI_SSID = "iot";
const char* WIFI_MDP  = "iotisis;";

// MQTT (broker Mosquitto sur le Raspberry Pi)
const char* MQTT_SERVEUR = "172.18.32.45";
const int   MQTT_PORT    = 1883;
#define PREFIXE        "sante/"  // debut des topics (mettre ex. "groupe3/sante/" si broker partage)
#define PERIODE_PUB    2000      // envoi des mesures toutes les 2 s

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
#define ERR_MQTT_LOST        8
#define NB_ERR               9

const char* getErrorMessage(uint8_t code) {
  switch (code) {
    case ERR_WIFI_LOST:      return "Impossible de se connecter au Wifi";
    case ERR_HR_DISCONNECT:  return "Probleme liaison capteur cardiaque (MAX30102)";
    case ERR_DHT_DISCONNECT: return "Probleme liaison capteur temperature (DHT11)";
    case ERR_NO_FINGER:      return "Capteur cardiaque : doigt non detecte";
    case ERR_TEMP_LIMIT:     return "Temperature hors limites";
    case ERR_HR_ABNORMAL:    return "Rythme cardiaque anormal";
    case ERR_LOW_MEMORY:     return "Memoire insuffisante";
    case ERR_MQTT_LOST:      return "Wifi OK mais broker MQTT injoignable";
    default:                 return "Code erreur non repertorie";
  }
}

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

WiFiClient   clientWifi;
PubSubClient mqtt(clientWifi);
String       idClient;            // identifiant MQTT unique (base sur l'adresse MAC)

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
//  MQTT
// ---------------------------------------------------------------------
// Tentative de connexion au broker (appelee au plus toutes les 5 s)
void connecterMQTT() {
  Serial.print("Connexion MQTT a ");
  Serial.print(MQTT_SERVEUR);
  Serial.print(" ... ");

  // "Last Will" : si le noeud disparait, le broker publie "offline" a sa place
  if (mqtt.connect(idClient.c_str(), nullptr, nullptr,
                   PREFIXE "etat", 1, true, "offline")) {
    Serial.println("OK");
    mqtt.publish(PREFIXE "etat", "online", true);
  } else {
    Serial.printf("echec (code %d)\n", mqtt.state());
  }
}

// Envoi des mesures (toutes les PERIODE_PUB ms)
void publierMesures() {
  char buf[200];

  // 1. Tout en un seul message JSON (pratique dans Node-RED : noeud "json")
  snprintf(buf, sizeof(buf),
           "{\"temperature\":%.1f,\"humidite\":%.1f,\"bpm\":%d,\"spo2\":%d,\"erreurs\":%u}",
           temp10 / 10.0, hum10 / 10.0, bpm, spo2, erreurs);
  mqtt.publish(PREFIXE "mesures", buf);

  // 2. Une valeur par topic (pratique pour les jauges / graphiques)
  if (!(erreurs & (1 << ERR_DHT_DISCONNECT))) {
    snprintf(buf, sizeof(buf), "%.1f", temp10 / 10.0);
    mqtt.publish(PREFIXE "temperature", buf);
    snprintf(buf, sizeof(buf), "%.1f", hum10 / 10.0);
    mqtt.publish(PREFIXE "humidite", buf);
  }
  snprintf(buf, sizeof(buf), "%d", bpm);
  mqtt.publish(PREFIXE "bpm", buf);
  snprintf(buf, sizeof(buf), "%d", spo2);
  mqtt.publish(PREFIXE "spo2", buf);
}

// Envoi du texte des erreurs, seulement quand elles changent
void publierErreursSiChangement() {
  static uint16_t dernieres = 0xFFFF;
  if (erreurs == dernieres) return;
  dernieres = erreurs;

  String texte;
  if (erreurs == 0) {
    texte = "Fonctionnement nominal";
  } else {
    for (uint8_t code = 1; code < NB_ERR; code++) {
      if (erreurs & (1 << code)) {
        if (texte.length()) texte += " | ";
        texte += getErrorMessage(code);
      }
    }
  }
  mqtt.publish(PREFIXE "alerte", texte.c_str(), true);
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
  Serial.begin(9600);
  delay(1000);
  Serial.println("=== NOEUD PRINCIPAL (esclave 0x55) + MQTT ===");

  // Capteur de temperature
  dht.begin();

  // Bus du capteur cardiaque (Wire1, maitre)
  Wire1.begin(PIN_SDA_HR, PIN_SCL_HR, 100000);
  Wire1.setTimeOut(50);

  // Wi-Fi + MQTT non bloquants : on lance la connexion et on continue
#if WIFI_ACTIF
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_MDP);
  Serial.println("Connexion Wi-Fi lancee (non bloquante)");

  idClient = "noeud_principal_" + WiFi.macAddress();
  idClient.replace(":", "");
  mqtt.setServer(MQTT_SERVEUR, MQTT_PORT);
  mqtt.setSocketTimeout(2);      // ne pas bloquer longtemps si le broker est absent
  mqtt.setKeepAlive(10);
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
  static unsigned long tPresence = 0, tMax = 0, tDht = 0, tWifi = 0,
                       tMqtt = 0, tPub = 0, tAffiche = 0;

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
  if (!maxDemarre) {               // pas de capteur : ces erreurs n'ont pas de sens
    setErreur(ERR_NO_FINGER, false);
    setErreur(ERR_HR_ABNORMAL, false);
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

  // 4. Wi-Fi + MQTT
#if WIFI_ACTIF
  bool wifiOK = (WiFi.status() == WL_CONNECTED);
  setErreur(ERR_WIFI_LOST, !wifiOK);
  if (!wifiOK && maintenant - tWifi >= 10000) {
    tWifi = maintenant;
    WiFi.reconnect();
  }

  if (wifiOK) {
    if (!mqtt.connected()) {
      if (maintenant - tMqtt >= 5000) {     // nouvel essai toutes les 5 s
        tMqtt = maintenant;
        connecterMQTT();
      }
    } else {
      mqtt.loop();                          // garde la connexion en vie
    }
  }
  // Erreur MQTT seulement si le Wi-Fi marche (sinon c'est l'erreur Wifi)
  setErreur(ERR_MQTT_LOST, wifiOK && !mqtt.connected());
#endif

  // 5. Memoire
  setErreur(ERR_LOW_MEMORY, ESP.getFreeHeap() < MEMOIRE_MIN);

  // 6. Mise a jour de la fiche envoyee au diagnostic
  preparerTrame();

  // 7. Envoi MQTT des mesures et des erreurs
#if WIFI_ACTIF
  if (mqtt.connected()) {
    publierErreursSiChangement();
    if (maintenant - tPub >= PERIODE_PUB) {
      tPub = maintenant;
      publierMesures();
    }
  }
#endif

  // 8. Affichage local (moniteur serie du noeud principal), toutes les 1 s
  if (maintenant - tAffiche >= 1000) {
    tAffiche = maintenant;
    Serial.printf("T=%.1f C  H=%.1f %%  BPM=%d  SpO2=%d %%  erreurs=0x%04X  MQTT=%s\n",
                  temp10 / 10.0, hum10 / 10.0, bpm, spo2, erreurs,
                  mqtt.connected() ? "OK" : "non");
  }
}
