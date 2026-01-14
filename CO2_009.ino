/*
 * ESP32 Air Quality Monitor
 * SCD41 + ENS160 + AHT21
 * Web interface + JSON API + Home Assistant REST API Client
 */

#include <WiFi.h>
#include <Wire.h>
#include <SensirionI2cScd4x.h>
#include "SparkFun_ENS160.h"
#include <Adafruit_AHTX0.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <HTTPClient.h> // Bibliotheek voor HTTP REST calls
#include "secrets.h" // PASSWORDS ENZO 
// ==========================================================
// ======== 🚨 Configuratie AANPASSEN! 🚨 (Jouw settings) ========
// ==========================================================

// Home Assistant (HA) REST API Instellingen
const char* HA_IP = "homeassistant.local"; // Vul het IP van je HA-server in
const int HA_PORT = 8123;
// VUL HIER JE GEGENEREERDE LONG-LIVED ACCESS TOKEN IN!
const char* HA_TOKEN = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJhYzcxMzU5Nzk0N2M0Mjg2YjJmOWZhYTUzMzY4ODQyMCIsImlhdCI6MTc2NTYyNzkxNywiZXhwIjoyMDgwOTg3OTE3fQ.7NOwefg_wouE4pGJ8VZE0hYCkbLH4Rc8K3qo2bUfS-g"; 

// WiFi Gegevens

const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// ==========================================================

// Unieke ID voor DIT apparaat (Voor groepering in HA als één Apparaat)
const char* DEVICE_ID = "esp32_airquality_001"; 

// -------- Sensor Objecten --------
SparkFun_ENS160 ens;
SensirionI2cScd4x scd4x;
Adafruit_AHTX0 aht;
WebServer server(80);

// -------- Sensor Variabelen --------
uint16_t co2_ppm=0, tvoc_ppb=0, eco2_ppm=0;
float tC_scd=NAN, rh_scd=NAN, tC_aht=NAN, rh_aht=NAN;
uint8_t aqi=0;

// -------- I2C Pins --------
#define SDA_PIN 21
#define SCL_PIN 22
#define SCD41_ADDR 0x62

// -------- Helper Functies --------
const char* aqiText(uint8_t v){
    switch(v){
        case 1: return "Super"; 
        case 2: return "Goed";
        case 3: return "Matig"; 
        case 4: return "Slecht"; 
        case 5: return "Ongezond";
    }
    return "?";
}

const char* ventHint(uint16_t co2){
    if(co2>=1500) return "Ventileer nu !";
    if(co2>=1000) return "Is een goed idee";
    if(co2>=800)  return "Eventueel";
    return "Niet nodig";
}

// -------- REST API FUNCTIES (Kern van de communicatie) --------

/**
 * Verstuurt een enkele sensorstatus naar Home Assistant via een HTTP POST.
 */
void sendToHASensorState(float value, const char* entity_suffix, const char* unit, const char* friendly_name, const char* device_class) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[REST] WiFi niet verbonden.");
        return;
    }

    HTTPClient http;
    String entity_id = "sensor." + String(DEVICE_ID) + "_" + String(entity_suffix);
    String url = "http://" + String(HA_IP) + ":" + String(HA_PORT) + "/api/states/" + entity_id;
    http.begin(url); 
    
    http.addHeader("Authorization", "Bearer " + String(HA_TOKEN));
    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<512> doc;
    doc["state"] = value;
    doc["unique_id"] = String(DEVICE_ID) + "_" + String(entity_suffix);

    JsonObject attributes = doc.createNestedObject("attributes");
    attributes["unit_of_measurement"] = unit;
    attributes["friendly_name"] = friendly_name; 
    attributes["device_class"] = device_class;
    
    JsonObject device = attributes.createNestedObject("device");
    device["identifiers"] = DEVICE_ID;
    device["name"] = "ESP32 Air Quality Monitor";
    device["model"] = "ESP32 SCD41 + ENS160 + AHT21";
    device["manufacturer"] = "DroneBot Workshop";
    
    String payload;
    
    serializeJson(doc, payload);

    int httpResponseCode = http.POST(payload);
    if (httpResponseCode > 0) {
        Serial.printf("[REST] HA POST OK (%s), Code: %d\n", entity_id.c_str(), httpResponseCode);
    } else {
        Serial.printf("[REST] HA POST FAIL (%s), Code: %d, Error: %s\n", 
                        entity_id.c_str(), httpResponseCode, http.errorToString(httpResponseCode).c_str());
    }
    http.end();
}

/**
 * Verstuur AQI als TEKST naar Home Assistant
 */
void sendAQIText(){
    if (WiFi.status() != WL_CONNECTED) return;

    HTTPClient http;
    String entity_id = "sensor." + String(DEVICE_ID) + "_aqi_text_001";
    String url = "http://" + String(HA_IP) + ":" + String(HA_PORT) + "/api/states/" + entity_id;
    
    http.begin(url);
    http.addHeader("Authorization", "Bearer " + String(HA_TOKEN));
    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<256> doc;
    doc["state"] = String(aqiText(aqi));
    doc["unique_id"] = String(DEVICE_ID) + "_aqi_text_001";

    JsonObject attributes = doc.createNestedObject("attributes");
    attributes["friendly_name"] = "AQI Text";
    
    JsonObject device = attributes.createNestedObject("device");
    device["identifiers"] = DEVICE_ID;
    device["name"] = "ESP32 Air Quality Monitor";
    device["model"] = "ESP32 SCD41 + ENS160 + AHT21";
    device["manufacturer"] = "DroneBot Workshop";

    String payload;
    serializeJson(doc, payload);

    int httpResponseCode = http.POST(payload);
    if (httpResponseCode > 0) {
        Serial.printf("[REST] HA POST OK (%s), Code: %d\n", entity_id.c_str(), httpResponseCode);
    } else {
        Serial.printf("[REST] HA POST FAIL (%s), Code: %d, Error: %s\n", 
                      entity_id.c_str(), httpResponseCode, http.errorToString(httpResponseCode).c_str());
    }
    http.end();
}

/**
 * Verstuur VentHint naar Home Assistant
 */
void sendVentHint(){
    if (WiFi.status() != WL_CONNECTED) return;

    HTTPClient http;
    String entity_id = "sensor." + String(DEVICE_ID) + "_vent_001";
    String url = "http://" + String(HA_IP) + ":" + String(HA_PORT) + "/api/states/" + entity_id;

    http.begin(url);
    http.addHeader("Authorization", "Bearer " + String(HA_TOKEN));
    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<256> doc;
    doc["state"] = String(ventHint(co2_ppm));
    doc["unique_id"] = String(DEVICE_ID) + "_vent_001";

    JsonObject attributes = doc.createNestedObject("attributes");
    attributes["friendly_name"] = "Ventilation Hint";

    JsonObject device = attributes.createNestedObject("device");
    device["identifiers"] = DEVICE_ID;
    device["name"] = "ESP32 Air Quality Monitor";
    device["model"] = "ESP32 SCD41 + ENS160 + AHT21";
    device["manufacturer"] = "DroneBot Workshop";

    String payload;
    serializeJson(doc, payload);

    int httpResponseCode = http.POST(payload);
    if (httpResponseCode > 0) {
        Serial.printf("[REST] HA POST OK (%s), Code: %d\n", entity_id.c_str(), httpResponseCode);
    } else {
        Serial.printf("[REST] HA POST FAIL (%s), Code: %d, Error: %s\n", 
                      entity_id.c_str(), httpResponseCode, http.errorToString(httpResponseCode).c_str());
    }
    http.end();
}

/**
 * Roept de sendToHASensorState aan voor alle sensoren.
 */
void sendAllSensorData(){
    // CO2 (PPM)
    sendToHASensorState(co2_ppm, "co2_001", "ppm", "CO2", "carbon_dioxide");
    
    // TVOC (PPB)
    sendToHASensorState(tvoc_ppb, "tvoc_001", "ppb", "TVOC", "volatile_organic_compounds");
    
    // eCO2 (PPM)
    sendToHASensorState(eco2_ppm, "eco2_001", "ppm", "eCO2", "carbon_dioxide");
    
    // AQI (Index)
    sendToHASensorState(aqi, "aqi_001", "", "AQI", "aqi"); 

    // Temp AHT (°C)
    if (!isnan(tC_aht)) sendToHASensorState(tC_aht, "temp_aht_001", "°C", "Temp AHT", "temperature");
    
    // RH AHT (%)
    if (!isnan(rh_aht)) sendToHASensorState(rh_aht, "rh_aht_001", "%", "RH AHT", "humidity");
    
    // Temp SCD (°C)
    if (!isnan(tC_scd)) sendToHASensorState(tC_scd, "temp_scd_001", "°C", "Temp SCD", "temperature");
    
    // RH SCD (%)
    if (!isnan(rh_scd)) sendToHASensorState(rh_scd, "rh_scd_001", "%", "RH SCD", "humidity");
}

// -------- Web Interface (blijft hetzelfde) --------
void handleRoot() {
    String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Air Quality Monitor</title>";
    html += "<meta http-equiv='refresh' content='2'>";
    html += "<style>body{font-family:sans-serif;text-align:center;} table{margin:auto;border-collapse:collapse;} td,th{padding:8px;border:1px solid #333;}</style>";
    html += "</head><body><h2>ESP32 Air Quality Monitor Beneden</h2><table>";

    // Sensoren
    html += "<tr><th>Sensor</th><th>Value</th></tr>";
    html += "<tr><td>CO2 ppm</td><td>" + String(co2_ppm) + "</td></tr>";
    html += "<tr><td>Temp SCD C</td><td>" + String(tC_scd,2) + "</td></tr>";
    html += "<tr><td>RH SCD %</td><td>" + String(rh_scd,1) + "</td></tr>";
    html += "<tr><td>Temp AHT C</td><td>" + String(tC_aht,2) + "</td></tr>";
    html += "<tr><td>RH AHT %</td><td>" + String(rh_aht,1) + "</td></tr>";
    html += "<tr><td>TVOC ppb</td><td>" + String(tvoc_ppb) + "</td></tr>";
    html += "<tr><td>eCO2 ppm</td><td>" + String(eco2_ppm) + "</td></tr>";
    html += "<tr><td>AQI</td><td>" + String(aqi) + " (" + String(aqiText(aqi)) + ")</td></tr>";
    html += "<tr><td>Ventilation</td><td>" + String(ventHint(co2_ppm)) + "</td></tr>";

    // WiFi info
    html += "<tr><th colspan='2'>WiFi Info</th></tr>";
    html += "<tr><td>Connected BSSID</td><td>" + WiFi.BSSIDstr() + "</td></tr>";
    html += "<tr><td>Connected SSID</td><td>" + WiFi.SSID() + "</td></tr>";
    html += "<tr><td>DNS</td><td>" + WiFi.dnsIP(0).toString() + " " + WiFi.dnsIP(1).toString() + "</td></tr>";
    html += "<tr><td>IP Address</td><td>" + WiFi.localIP().toString() + "</td></tr>";
    html += "<tr><td>MAC Address</td><td>" + WiFi.macAddress() + "</td></tr>";
    html += "<tr><td>Signal Strength</td><td>" + String(WiFi.RSSI()) + " dBm / " + String(map(WiFi.RSSI(),-100,-50,0,100)) + " %</td></tr>";

    // Sluit tabel en pagina
    html += "</table></body></html>";

    server.send(200, "text/html", html);
}

// -------- JSON endpoint (blijft hetzelfde) --------
void handleJson(){
    StaticJsonDocument<256> doc;
    doc["co2"]=co2_ppm;
    if(!isnan(tC_scd)) doc["t_scd"]=tC_scd; else doc["t_scd"]=JsonVariant(); 
    if(!isnan(rh_scd)) doc["rh_scd"]=rh_scd; else doc["rh_scd"]=JsonVariant();
    doc["t_aht"]=tC_aht;
    doc["rh_aht"]=rh_aht;
    doc["tvoc"]=tvoc_ppb;
    doc["eco2"]=eco2_ppm;
    doc["aqi"]=aqi;
    String output;
    serializeJson(doc,output);
    server.send(200,"application/json",output);
}

// -------- Setup --------
void setup(){
    Serial.begin(115200);
    delay(200);

    // WiFi connectie
    WiFi.begin(ssid,password);
    while(WiFi.status()!=WL_CONNECTED){ delay(500); Serial.print("."); }
    Serial.println("\n[WiFi] Connected, IP: " + WiFi.localIP().toString());
    
    // I2C en Sensor initialisatie
    Wire.begin(SDA_PIN,SCL_PIN);
    Wire.setClock(400000);

    ens.begin(); ens.setOperatingMode(SFE_ENS160_STANDARD);
    aht.begin();
    scd4x.begin(Wire,SCD41_ADDR); 
    scd4x.stopPeriodicMeasurement(); delay(500); scd4x.startPeriodicMeasurement();

    // Webserver
    server.on("/", handleRoot);
    server.on("/data.json", handleJson);
    server.begin();
    Serial.println("[Web] Server started");
}

// -------- Loop --------
void loop(){
    
    // Sensor metingen
    bool ready=false;
    if(scd4x.getDataReadyStatus(ready)==0 && ready){
        uint16_t co2; float t,rh;
        if(scd4x.readMeasurement(co2,t,rh)==0 && co2!=0){
            co2_ppm=co2; tC_scd=t; rh_scd=rh;
        }
    }

    sensors_event_t hum,temp;
    if(aht.getEvent(&hum,&temp)){ tC_aht=temp.temperature; rh_aht=hum.relative_humidity; }

    if(ens.checkDataStatus()){ tvoc_ppb=ens.getTVOC(); eco2_ppm=ens.getECO2(); aqi=ens.getAQI(); }

    static uint32_t lastREST=0; 
    uint32_t now = millis();

    // Verstuur ALLE sensoren via REST elke 5 seconden
    if(now - lastREST > 5000){ 
        lastREST = now;
        if (WiFi.status() == WL_CONNECTED) {
            sendAllSensorData(); 
            sendAQIText();      // AQI tekst
            sendVentHint();     // Ventilatiehint
        } else {
            Serial.println("[REST] WiFi lost. Trying to reconnect...");
            WiFi.begin(ssid, password);
        }
    }

    server.handleClient();
}
