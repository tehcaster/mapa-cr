#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <FS.h>
#include "SPIFFS.h"

#include "config.h"

// Objekt pro ovladani adresovatelnych RGB LED
// Je jich 72 a jsou v serii pripojene na GPIO pin 25
Adafruit_NeoPixel pixely(72, 25, NEO_GRB + NEO_KHZ800);
// HTTP server bezici na standardnim TCP portu 80
WebServer server(80);
// Pamet pro JSON s povely
// Alokujeme pro nej 10 000 B, co je hodne,
// ale melo by to stacit i pro jSON,
// ktery bude obsahovat instrukce pro vsech 72 RGB LED
StaticJsonDocument<10000> doc;

uint32_t t = 0;             // Refreshovaci timestamp
uint32_t delay10 = 600000;  // Prodleva mezi aktualizaci dat, 10 minut
uint8_t jas = 5;            // Vychozi jas

// Dekoder JSONu a rozsvecovac svetylek
int jsonDecoder(String s, bool log) {
  DeserializationError e = deserializeJson(doc, s);
  if (e) {
    if (e == DeserializationError::InvalidInput) {
      return -1;
    } else if (e == DeserializationError::NoMemory) {
      return -2;
    } else {
      return -3;
    }
  } else {
    pixely.clear();
    JsonArray mesta = doc["seznam"].as<JsonArray>();
    for (JsonObject mesto : mesta) {
      int id = mesto["id"];
      int r = mesto["r"];
      int g = mesto["g"];
      int b = mesto["b"];
      if (log) Serial.printf("Rozsvecuji mesto %d barvou R=%d G=%d B=%d\r\n", id, r, g, b);
      pixely.setPixelColor(id, pixely.Color(r, g, b));
    }
    pixely.show();
    return 0;
  }
}

// Stazeni radarovych dat z webu
void stahniData() {
  HTTPClient http;
  http.begin("http://kloboukuv.cloud/radarmapa/?chcu=posledni_v2.json");
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    int err = jsonDecoder(http.getString(), true);
    switch (err) {
      case 0:
        Serial.println("Hotovo!");
        break;
      case -1:
        Serial.println("Spatny format JSONu");
        break;
      case -2:
        Serial.println("Malo pameti, navys velikost StaticJsonDocument");
        break;
      case -3:
        Serial.println("Chyba pri parsovani JSONu");
        break;
    }
  }
  http.end();
}

void handle_json(void) {
  // Pokud HTTP data obsahuji parametr mesta
  // predame jeho obsah JSON dekoderu
  if (server.hasArg("mesta")) {
    int err = jsonDecoder(server.arg("mesta"), true);
    switch (err) {
      case 0:
        server.send(200, "text/plain", "OK");
        break;
      case -1:
        server.send(200, "text/plain", "CHYBA\nSpatny format JSON");
        break;
      case -2:
        server.send(200, "text/plain", "CHYBA\nMalo pameti RAM pro JSON. Navys ji!");
        break;
      case -3:
        server.send(200, "text/plain", "CHYBA\nNepodarilo se mi dekodovat jSON");
        break;
    }
  }
  // Ve vsech ostatnich pripadech odpovime chybovym hlasenim
  else {
    server.send(200, "text/plain", "CHYBA\nNeznamy prikaz");
  }
}

void handle_single() {
  int id = 0, r = 0, g = 0, b = 0;
  if (server.hasArg("id")) {
    id = server.arg("id").toInt();
  } else {
    server.send(200, "text/plain", "CHYBA\nChybi id diody");
    return;
  }
  if (server.hasArg("r"))
    r = server.arg("r").toInt();
  if (server.hasArg("g"))
    g = server.arg("g").toInt();
  if (server.hasArg("b"))
    b = server.arg("b").toInt();
  pixely.setPixelColor(id, pixely.Color(r, g, b));
  pixely.show();
  server.send(200, "text/plain", "OK");
}

void handle_cfg() {
  if (server.hasArg("jas")) {
    jas = server.arg("jas").toInt();
    pixely.setBrightness(jas);
    pixely.show();
  }
  if (server.hasArg("redirect") && server.arg("redirect").toInt() == 1) {
    server.sendHeader("Location", "/");
    server.send(303, "text/plain", "");
  } else {
    server.send(200, "text/plain", "OK");
  }
}

void handle_off() {
    server.send(200, "text/plain", "OK");
    pixely.clear();
    pixely.show();  
}

// Hlavni funkce setup se zpracuje hned po startu cipu ESP32
void setup() {
  // Nastartujeme serivou linku rychlosti 115200 b/s
  Serial.begin(115200);
  delay(3000);
  // Pripojime se k Wi-Fi a pote vypiseme do seriove linky IP adresu
  WiFi.disconnect(); // Vynucene odpojeni; obcas pomuze, kdyz se cip po startu nechce prihlasit
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, heslo);
  Serial.printf("Pripojuji se k %s ", ssid);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  // Automaticke pripojeni pri ztrate Wi-Fi
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  // Vypiseme do seriove linky pro kontrolu LAN IP adresu mapy
  Serial.printf(" OK\nIP: %s\r\n", WiFi.localIP().toString());

  configTzTime(MY_TZ, MY_NTP_SERVER, "", "");

  SPIFFS.begin();

  server.serveStatic("/", SPIFFS, "/index.html");

  // Pro HTTP pozadavku / zavolame funkci httpDotaz
  server.on("/json", handle_json);
  server.on("/off", handle_off);
  server.on("/cfg", handle_cfg);
  server.on("/single", handle_single);

  // Aktivujeme server
  server.begin();
  // Nakonfigurujeme adresovatelene LED do vychozi zhasnute pozice
  // Nastavime 8bit jas na hodnotu 5
  // Nebude svitit zbytecne moc a vyniknou mene kontrastni barvy
  pixely.begin();
  pixely.setBrightness(jas);
  pixely.clear();
  pixely.show();
}

// Smycka loop se opakuje stale dokola
// a nastartuje se po zpracovani funkce setup
void loop() {
  // Vyridime pripadne TCP spojeni klientu se serverem
  server.handleClient();
  // Kazdych deset minut stahnu nova data
  if (t == 0 || millis() - t > delay10) {
    t = millis();
    stahniData();
  }
  // Pockame 2 ms (prenechame CPU pro ostatni ulohy na pozadi) a opakujeme
  delay(2);
}
