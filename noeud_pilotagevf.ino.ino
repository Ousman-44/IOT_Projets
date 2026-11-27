/*
 * =====================================================================
 *  NOEUD DE PILOTAGE  (Wi-Fi + MQTT)  - LED + bouton
 * =====================================================================
 *  Cablage (connecteur de GAUCHE, en comptant depuis le haut / antenne) :
 *    - 4e trou  = D3 / IO26 -> fil BLEU  -> a5 -> resistance 220 ohms (b5-b7)
 *                              -> LED (patte longue e7, patte courte f7) -> j7 -> ligne -
 *    - 5e trou  = D4 / IO27 -> fil ROUGE -> a2 -> bouton (e2 f2 e4 f4) -> j4 -> ligne -
 *    - 15e trou = GND       -> fil VIOLET -> ligne -
 *  Moniteur serie : 115200 baud.
 *
 *  Recoit (subscribe) :
 *     pilotage/led     "ON" / "OFF" / "BLINK"
 *  Envoie (publish) :
 *     pilotage/bouton  "appuye" / "relache"   (a chaque changement)
 *     pilotage/toggle  "ON" / "OFF"           (change a chaque appui, retenu)
 *     pilotage/etat    "online" / "offline"   (Last Will)
 *
 *  Bibliotheque : PubSubClient (Nick O'Leary)
 * =====================================================================
 */

#include <WiFi.h>
#include <PubSubClient.h>

// ---------------------------------------------------------------------
//  PARAMETRES
// ---------------------------------------------------------------------
const char* WIFI_SSID    = "iot";
const char* WIFI_MDP     = "iotisis;";
const char* MQTT_SERVEUR = "172.18.32.45";
const int   MQTT_PORT    = 1883;

#define PIN_LED      26      // D3 / IO26 (4e trou)  -> fil bleu  -> resistance -> LED
#define PIN_BOUTON   27      // D4 / IO27 (5e trou)  -> fil rouge -> bouton

#define T_LED     "pilotage/led"
#define T_BOUTON  "pilotage/bouton"
#define T_TOGGLE  "pilotage/toggle"
#define T_ETAT    "pilotage/etat"

// ---------------------------------------------------------------------
//  VARIABLES
// ---------------------------------------------------------------------
WiFiClient   clientWifi;
PubSubClient mqtt(clientWifi);
String       idClient;

enum ModeLed { LED_ETEINTE, LED_ALLUMEE, LED_CLIGNOTE };
ModeLed modeLed = LED_ETEINTE;
bool    etatToggle = false;

// anti-rebond du bouton
bool          dernierLu   = HIGH;
bool          etatStable  = HIGH;
unsigned long tChangement = 0;

// ---------------------------------------------------------------------
//  RECEPTION MQTT : commande de la LED
// ---------------------------------------------------------------------
void callback(char* topic, byte* payload, unsigned int len) {
  String msg;
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  msg.trim();
  msg.toUpperCase();
  Serial.printf("Recu [%s] : %s\n", topic, msg.c_str());

  if (String(topic) == T_LED) {
    if (msg == "BLINK")                                   modeLed = LED_CLIGNOTE;
    else if (msg == "ON" || msg == "1" || msg == "TRUE")  modeLed = LED_ALLUMEE;
    else                                                  modeLed = LED_ETEINTE;
  }
}

// ---------------------------------------------------------------------
//  CONNEXION MQTT  (appelee au plus toutes les 5 s)
// ---------------------------------------------------------------------
void connecterMQTT() {
  Serial.printf("Connexion MQTT a %s ... ", MQTT_SERVEUR);
  if (mqtt.connect(idClient.c_str(), nullptr, nullptr, T_ETAT, 1, true, "offline")) {
    Serial.println("OK");
    mqtt.publish(T_ETAT, "online", true);
    mqtt.subscribe(T_LED);
    mqtt.publish(T_TOGGLE, etatToggle ? "ON" : "OFF", true);
  } else {
    Serial.printf("echec (code %d)\n", mqtt.state());
  }
}

// ---------------------------------------------------------------------
//  BOUTON  (anti-rebond 30 ms, envoi a chaque changement)
// ---------------------------------------------------------------------
void gererBouton() {
  bool lu = digitalRead(PIN_BOUTON);
  if (lu != dernierLu) {
    dernierLu = lu;
    tChangement = millis();
  }
  if (millis() - tChangement > 30 && lu != etatStable) {
    etatStable = lu;
    bool appuye = (etatStable == LOW);
    Serial.println(appuye ? "Bouton appuye" : "Bouton relache");
    if (mqtt.connected()) mqtt.publish(T_BOUTON, appuye ? "appuye" : "relache");

    if (appuye) {                         // chaque appui inverse le toggle
      etatToggle = !etatToggle;
      Serial.printf("Toggle -> %s\n", etatToggle ? "ON" : "OFF");
      if (mqtt.connected()) mqtt.publish(T_TOGGLE, etatToggle ? "ON" : "OFF", true);
    }
  }
}

// ---------------------------------------------------------------------
//  LED  (clignotement sans delay)
// ---------------------------------------------------------------------
void gererLed() {
  bool ledOn = false;
  if (modeLed == LED_ALLUMEE)       ledOn = true;
  else if (modeLed == LED_CLIGNOTE) ledOn = (millis() / 250) % 2;
  digitalWrite(PIN_LED, ledOn);
}

// ---------------------------------------------------------------------
//  INITIALISATION
// ---------------------------------------------------------------------
void setup() {
  Serial.begin(9600);
  delay(1000);
  Serial.println("=== NOEUD DE PILOTAGE ===");

  // Adresse MAC (a donner au prof si le reseau filtre les appareils)
  Serial.print("Adresse MAC de l'ESP : ");
  Serial.println(WiFi.macAddress());

  // Raison precise en cas de refus du Wi-Fi
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      int r = info.wifi_sta_disconnected.reason;
      if (r == 8) return;                 // 8 = deconnexion volontaire (nouvel essai)
      Serial.printf("Wi-Fi refuse, raison %d : ", r);
      if      (r == 15 || r == 204 || r == 202 || r == 2) Serial.println("mot de passe incorrect");
      else if (r == 201)                                  Serial.println("reseau introuvable");
      else if (r == 203)                                  Serial.println("association refusee (filtrage MAC ?)");
      else                                                Serial.println("autre raison");
    }
  });

  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BOUTON, INPUT_PULLUP);

  // Signe de vie : 3 clignotements rapides au demarrage
  for (int i = 0; i < 3; i++) {
    digitalWrite(PIN_LED, HIGH); delay(150);
    digitalWrite(PIN_LED, LOW);  delay(150);
  }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_MDP);
  Serial.printf("Connexion au Wi-Fi \"%s\" ...\n", WIFI_SSID);

  idClient = "noeud_pilotage_" + WiFi.macAddress();
  idClient.replace(":", "");
  mqtt.setServer(MQTT_SERVEUR, MQTT_PORT);
  mqtt.setCallback(callback);
  mqtt.setSocketTimeout(2);
  mqtt.setKeepAlive(10);
}

// ---------------------------------------------------------------------
//  BOUCLE PRINCIPALE  (aucun delay)
// ---------------------------------------------------------------------
void loop() {
  static unsigned long tWifi = 0, tMqtt = 0;
  static bool wifiAvant = false;
  unsigned long maintenant = millis();

  // --- Wi-Fi ---
  bool wifiOK = (WiFi.status() == WL_CONNECTED);
  if (wifiOK != wifiAvant) {
    wifiAvant = wifiOK;
    if (wifiOK) { Serial.print("Wi-Fi OK, IP : "); Serial.println(WiFi.localIP()); }
    else        Serial.println("Wi-Fi perdu");
  }
  if (!wifiOK && maintenant - tWifi >= 15000) {     // diagnostic + nouvel essai
    tWifi = maintenant;
    int s = WiFi.status();
    Serial.printf("Wi-Fi non connecte (statut %d) : ", s);
    if      (s == WL_NO_SSID_AVAIL)  Serial.println("reseau introuvable (nom faux ou 5 GHz)");
    else if (s == WL_CONNECT_FAILED) Serial.println("echec (mot de passe faux ?)");
    else                             Serial.println("nouvel essai...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_MDP);
  }

  // --- MQTT ---
  if (wifiOK) {
    if (!mqtt.connected()) {
      if (maintenant - tMqtt >= 5000) {
        tMqtt = maintenant;
        connecterMQTT();
      }
    } else {
      mqtt.loop();
    }
  }

  gererBouton();
  gererLed();
}
