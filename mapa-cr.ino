#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <FS.h>
#include "SPIFFS.h"
#include <Preferences.h>

// see https://github.com/adafruit/Adafruit_NeoPixel/issues/375#issuecomment-2268197924
// should be resolved in 1.12.4
//SET_LOOP_TASK_STACK_SIZE(16*1024);

#include "config.h"

#define LEDS_COUNT  72
#define LEDS_PIN	25

Preferences myPrefs;

// Objekt pro ovladani adresovatelnych RGB LED
// Je jich 72 a jsou v serii pripojene na GPIO pin 25
Adafruit_NeoPixel pixely(LEDS_COUNT, LEDS_PIN, NEO_GRB + NEO_KHZ800);
// HTTP server bezici na standardnim TCP portu 80
WebServer server(80);
// Pamet pro JSON s povely
// Alokujeme pro nej 10 000 B, co je hodne,
// ale melo by to stacit i pro jSON,
// ktery bude obsahovat instrukce pro vsech 72 RGB LED
StaticJsonDocument<10000> doc;

/* Allow easy and lossless re-rendering when brightness or gamma changes */
uint32_t color_cache[LEDS_COUNT];

struct map_mode {
  String url;
  void (*process_json)(void);
  int refresh_minutes;
  bool no_cycle;
};

struct mode_cache {
  uint32_t colors[LEDS_COUNT];
  uint32_t last_refresh;
};

uint32_t delay10 = 600000;  // Prodleva mezi aktualizaci dat, 10 minut
uint8_t jas = 5;            // Vychozi jas
int current_mode = 0;
bool cycle_modes = true;
int cycle_seconds = 30;
bool gamma_cor = true;
bool ignore_night = false;
bool night_active = false;
// this is in minutes since midnight
int time_off = 20*60;
int time_on = 8*60;

// Initialize array with number of districts readed from TMEP, 
// which we will later populate with values from JSON
float TMEPDistrictTemperatures[77];

// We need to map our LED order with district order from TMEP,
// so here we have array with positions of districts on TMEP that we get from JSON
// starting from zero, so: index 0 (first LED) is district with ID 9 and so on
// until we have district for every from our 72 LEDs
int TMEPDistrictPosition[LEDS_COUNT] = {
 9, 11, 12, 8, 10, 13, 6, 15, 7, 5, 3, 14, 16, 67, 66, 4, 2, 24, 17, 1, 68, 18, 65, 64, 0, 
 25, 76, 20, 69, 19, 27, 23, 73, 70, 21, 29, 28, 59, 22, 71, 61, 63, 30, 72, 31, 26, 48, 
 46, 33, 39, 58, 49, 51, 47, 57, 40, 32, 35, 56, 38, 55, 34, 45, 41, 50, 36, 54, 52, 37, 
 44, 53, 43
};

#define NUM_MODES 3
struct mode_cache mode_caches[NUM_MODES];

/* cache and set color */
static void set_color(int id, uint32_t col) {
  color_cache[id] = col;
  mode_caches[current_mode].colors[id] = col;
}

static void clear_colors() {
  for (int i = 0; i < LEDS_COUNT; i++)
    set_color(i, 0);

  pixely.clear();
}

static void render_cached_colors() {
  for (int i = 0; i < LEDS_COUNT; i++) {
    uint32_t col = color_cache[i];

    if (gamma_cor)
      col = pixely.gamma32(col);

    pixely.setPixelColor(i, col);
  }
  pixely.show();
}

static void switch_mode(int mode) {
  current_mode = mode;
  for (int i = 0; i < LEDS_COUNT; i++) {
    color_cache[i] = mode_caches[mode].colors[i];
  }
}

static void process_radar() {
  clear_colors();
  JsonArray mesta = doc["seznam"].as<JsonArray>();
  for (JsonObject mesto : mesta) {
    int id = mesto["id"];
    int r = mesto["r"];
    int g = mesto["g"];
    int b = mesto["b"];

    set_color(id, pixely.Color(r, g, b));
  }
  render_cached_colors();
}

static void process_temp() {
  String tmp;
  float maxTemp = -99;
  float minTemp =  99;
  // Read all TMEP districts with their indexes
  for (JsonObject item : doc.as<JsonArray>()) {
      int TMEPdistrictIndex = item["id"];
      // Substract 1, so index will start from 0
      TMEPdistrictIndex -= 1;
      double h = item["h"];
      TMEPDistrictTemperatures[TMEPdistrictIndex] = h;

      // find the min and max temperature for adjusting of color layout
      tmp = TMEPDistrictTemperatures[TMEPdistrictIndex];
      if (tmp.toFloat() < minTemp) minTemp = tmp.toFloat();
      if (tmp.toFloat() > maxTemp) maxTemp = tmp.toFloat();
  }

  clear_colors();
  // Now go through our LEDs and we will set their colors
  for (int LED = 0; LED <= LEDS_COUNT - 1; LED++) {
    int color;
    // Get color for correct district and recalculate based on the min and max of temperature in Czechia - variable color layout
    //color = map(TMEPDistrictTemperatures[TMEPDistrictPosition[LED]], minTemp, maxTemp, 170, 0);
    // Get color for correct district LED - fixed color layout (min -15; max 40 °C)
    color = map(TMEPDistrictTemperatures[TMEPDistrictPosition[LED]], -15, 40, 170, 0) << 8;
    set_color(LED, pixely.ColorHSV(color)); // Assuming Wheel function is generating HSV colors
  }

  render_cached_colors();
}

const struct map_mode modes[NUM_MODES] = {
  {
    .url = String("http://kloboukuv.cloud/radarmapa/?chcu=posledni_v2.json"),
    .process_json = process_radar,
    .refresh_minutes = 10,
  },
  {
    .url = String("http://cdn.tmep.cz/app/export/okresy-cr-teplota.json"),
    .process_json = process_temp,
    .refresh_minutes = 2,
  },
  {
    .url = String(""),
    .no_cycle = true,
  },
};

static void cycle_mode() {
  int mode = current_mode;
  while (true) {
    mode++;
    if (mode == NUM_MODES) {
      mode = 0;
    }
    if (!modes[mode].no_cycle) {
      switch_mode(mode);
      break;
    }
  }
}

// Stazeni radarovych dat z webu
void stahniData() {
  const struct map_mode *mode = &modes[current_mode];

  if (mode->url == "")
    return;

  HTTPClient http;
  http.begin(mode->url);
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    DeserializationError e = deserializeJson(doc, http.getString());
    http.end();
    if (e) {
        Serial.print("deserializeJson() failed: ");
        Serial.println(e.f_str());
        return;
    }
    mode->process_json();
    Serial.println("Downloaded new data");
  } else {
    Serial.print("HTTP Error code: ");
    Serial.println(httpCode);
    http.end();
  }
}

void handle_json(void) {
  // Pokud HTTP data obsahuji parametr mesta
  // predame jeho obsah JSON dekoderu
  if (server.hasArg("mesta")) {
    DeserializationError e = deserializeJson(doc, server.arg("mesta"));
    if (e) {
        Serial.print("deserializeJson() failed: ");
        Serial.println(e.f_str());
        server.send(200, "text/plain", String("CHYBA\nJSON problem: ") + e.c_str());
        return;
    }
    process_radar();
    server.send(200, "text/plain", "OK");
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
  if (id >= LEDS_COUNT) {
    server.send(200, "text/plain", "CHYBA\nSpatne id diody");
    return;
  }

  if (server.hasArg("r"))
    r = server.arg("r").toInt();
  if (server.hasArg("g"))
    g = server.arg("g").toInt();
  if (server.hasArg("b"))
    b = server.arg("b").toInt();

  uint32_t color = pixely.Color(r, g, b);

  if (id < 0) {
    for (int i = 0; i < LEDS_COUNT; i++) {
      set_color(i, color);
    }
  } else {
    set_color(id, color);
  }
  render_cached_colors();
  server.send(200, "text/plain", "OK");
}

void handle_cfg_set() {
  bool render = false;
  if (server.hasArg("jas")) {
    jas = server.arg("jas").toInt();
    pixely.setBrightness(jas);
    render = true;
  }
  if (server.hasArg("rezim")) {
    int want_mode = server.arg("rezim").toInt();
    if (want_mode >= NUM_MODES) {
      server.send(200, "text/plain", "CHYBA, neznámý režim");
      return;
    }
    switch_mode(want_mode);
    render = true;
  }
  if (server.hasArg("gamma")) {
    gamma_cor = server.arg("gamma") == "true";
    render = true;
  }
  if (server.hasArg("ignoruj_noc")) {
    ignore_night = server.arg("ignoruj_noc") == "true";
    if (night_active) {
      night_active == false;
      render = true;
    }
  }
  if (server.hasArg("cyklovat")) {
    cycle_modes = server.arg("cyklovat") == "true";
  }
  if (server.hasArg("cykl_sec")) {
    cycle_seconds = server.arg("cykl_sec").toInt();
  }
  if (server.hasArg("noc_od")) {
    time_off = server.arg("noc_od").toInt();
  }
  if (server.hasArg("noc_do")) {
    time_on = server.arg("noc_do").toInt();
  }
  if (server.hasArg("ulozit")) {
    myPrefs.begin("mapa-cr", false);

    myPrefs.putUChar("jas", jas);
    myPrefs.putInt("current_mode", current_mode);
    myPrefs.putBool("cycle_modes", cycle_modes);
    myPrefs.putInt("cycle_seconds", cycle_seconds);
    myPrefs.putBool("gamma_cor", gamma_cor);
    myPrefs.putInt("time_off", time_off);
    myPrefs.putInt("time_on", time_on);

    myPrefs.end();
  }
  if (render)
    render_cached_colors();
  server.send(200, "text/plain", "OK");
}

void handle_cfg_get() {
  String json;

  doc.clear();

  doc["rezim"] = current_mode;
  doc["cyklovat"] = cycle_modes;
  doc["cykl_sec"] = cycle_seconds;
  doc["jas"] = jas;
  doc["gamma"] = gamma_cor;
  doc["ignoruj_noc"] = ignore_night;
  doc["noc_od"] = time_off;
  doc["noc_do"] = time_on;

  serializeJson(doc, json);
  server.send(200, "application/json", json);
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

  myPrefs.begin("mapa-cr", true);

  jas = myPrefs.getUChar("jas", jas);
  current_mode = myPrefs.getInt("current_mode", current_mode);
  cycle_modes = myPrefs.getBool("cycle_modes", cycle_modes);
  cycle_seconds =  myPrefs.getInt("cycle_seconds", cycle_seconds);
  gamma_cor = myPrefs.getBool("gamma_cor", gamma_cor);
  time_off = myPrefs.getInt("time_off", time_off);
  time_on = myPrefs.getInt("time_on", time_on);

  myPrefs.end();

  SPIFFS.begin();

  server.serveStatic("/", SPIFFS, "/index.html");

  // Pro HTTP pozadavku / zavolame funkci httpDotaz
  server.on("/json", handle_json);
  server.on("/off", handle_off);
  server.on("/cfg_set", handle_cfg_set);
  server.on("/cfg_get", handle_cfg_get);
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

static long last_cycle = 0;
static long lastSec = 0;

// Smycka loop se opakuje stale dokola
// a nastartuje se po zpracovani funkce setup
void loop() {
  // Vyridime pripadne TCP spojeni klientu se serverem
  server.handleClient();

  /* only bother once a second */
  if (!(millis() - lastSec) > 1000)
    return;

  lastSec = millis();

  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)) {
    pixely.clear();
    pixely.show();

    return;
  }

  int minutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;

  if (!ignore_night &&
      ((time_on <  time_off && !(minutes >= time_on && minutes < time_off)) ||
       (time_on >= time_off && (time_off <= minutes && minutes < time_on)))) {
    if (!night_active) {
      pixely.clear();
      pixely.show();
      night_active = true;
    }

    return;
  }

  if (night_active) {
    render_cached_colors();
    night_active = false;
  }

  if (cycle_modes && millis() - last_cycle >= cycle_seconds * 1000) {
    last_cycle = millis();
    cycle_mode();
    render_cached_colors();
  }

  minutes += 24 * 60;
  uint32_t last = mode_caches[current_mode].last_refresh;

  // midnight
  if (last > minutes) {
    last -= 24 * 60;
    mode_caches[current_mode].last_refresh = last;
  }

  if (last == 0 || minutes - last >= modes[current_mode].refresh_minutes) {
    stahniData();
    mode_caches[current_mode].last_refresh = minutes;
  }
}
