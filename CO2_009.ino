/*
  ESP32 Air Quality Monitor
  SCD41 + ENS160 + AHT21
  Web interface + JSON API + MQTT + Home Assistant Discovery
  All sensors grouped under 1 device, discovery republished on reconnect
  and when Home Assistant publishes homeassistant/status = online
*/

#include <WiFi.h>
#include <Wire.h>
#include <PubSubClient.h>
#include <SensirionI2cScd4x.h>
#include "SparkFun_ENS160.h"
#include <Adafruit_AHTX0.h>
#include <ArduinoJson.h>
#include <WebServer.h>

#include "secrets.h"

// -------- WiFi Settings --------
const char* ssid     = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// -------- MQTT Settings --------
const char* mqtt_server = MQTT_SERVER;
const int mqtt_port     = MQTT_PORT;
const char* mqtt_user   = MQTT_USER;
const char* mqtt_pass   = MQTT_PASSWORD;


const char* mqtt_topic       = "home/airquality";
const char* ha_status_topic  = "homeassistant/status"; 
const char* ha_avail_topic   = "homeassistant/sensor/esp32_airquality/availability"; 

WiFiClient espClient;
PubSubClient mqtt(espClient);

// -------- I2C Pins --------
#define SDA_PIN 21
#define SCL_PIN 22
#define SCD41_ADDR 0x62

// -------- Sensor Objects --------
SensirionI2cScd4x scd4x;
SparkFun_ENS160 ens;
Adafruit_AHTX0 aht;
WebServer server(80);

// -------- Sensor Variables --------
uint16_t co2_ppm=0, tvoc_ppb=0, eco2_ppm=0;
float tC_scd=NAN, rh_scd=NAN, tC_aht=NAN, rh_aht=NAN;
uint8_t aqi=0;

// -------- Helper Functions --------
const char* aqiText(uint8_t v){
  switch(v){
    case 1: return "Excellent"; case 2: return "Good";
    case 3: return "Moderate"; case 4: return "Poor"; case 5: return "Unhealthy";
  }
  return "?";
}

const char* ventHint(uint16_t co2){
  if(co2>=1500) return "VENTILATE NOW";
  if(co2>=1000) return "Add fresh air";
  if(co2>=800)  return "Okay";
  return "Good";
}

// -------- MQTT Discovery (single sensor) --------
void publishOneDiscovery(const char* sensor_id, const char* name, const char* unit, const char* json_path){
  StaticJsonDocument<512> doc;
  String topic = String("homeassistant/sensor/") + sensor_id + "/config";

  doc["name"] = name;
  doc["state_topic"] = mqtt_topic;
  if (strlen(unit) > 0) doc["unit_of_measurement"] = unit;
  doc["value_template"] = String("{{ value_json.") + json_path + " }}";
  doc["unique_id"] = sensor_id;

  // Device object (all sensors must use same identifiers)
  JsonObject device = doc.createNestedObject("device");
  JsonArray ids = device.createNestedArray("identifiers");
  ids.add("esp32_airquality_001");    // SAME for all sensors!
  device["name"] = "ESP32 Air Quality Monitor";
  device["model"] = "ESP32 SCD41 + ENS160 + AHT21";
  device["manufacturer"] = "DroneBot Workshop";

  // Availability (optional but recommended)
  doc["availability_topic"] = ha_status_topic;
  doc["payload_available"] = "online";
  doc["payload_not_available"] = "offline";

  char buffer[512];
  size_t n = serializeJson(doc, buffer);
  bool ok = mqtt.publish(topic.c_str(), (const uint8_t*)buffer, n, true); // retain=true
  if(ok) Serial.println("[MQTT] Discovery published: " + String(sensor_id) + " -> " + topic);
  else Serial.println("[MQTT] Discovery FAILED: " + String(sensor_id) + " -> " + topic);
}

// -------- Publish all discovery topics --------
void publishAllDiscovery(){
  publishOneDiscovery("esp32_co2_001",      "CO2",     "ppm", "co2");
  publishOneDiscovery("esp32_tvoc_001",     "TVOC",    "ppb", "tvoc");
  publishOneDiscovery("esp32_eco2_001",     "eCO2",    "ppm", "eco2");
  publishOneDiscovery("esp32_aqi_001",      "AQI",     "",    "aqi");
  publishOneDiscovery("esp32_temp_aht_001", "Temp AHT","°C",  "t_aht");
  publishOneDiscovery("esp32_rh_aht_001",   "RH AHT",  "%",   "rh_aht");
  publishOneDiscovery("esp32_temp_scd_001", "Temp SCD","°C",  "t_scd");
  publishOneDiscovery("esp32_rh_scd_001",   "RH SCD",  "%",   "rh_scd");
}

// -------- MQTT message callback (listens for HA online) --------
void mqttCallback(char* topic, byte* payload, unsigned int length){
  String t = String(topic);
  String msg;
  for(unsigned int i=0;i<length;i++) msg += (char)payload[i];
  Serial.println("[MQTT] Message arrived on topic: " + t + " payload: " + msg);

  if(t.equals(ha_status_topic) && msg == "online"){
    Serial.println("[MQTT] Home Assistant ONLINE -> re-publishing discovery");
    publishAllDiscovery();
  }
}

// -------- MQTT Reconnect --------
void reconnectMQTT(){
  while(!mqtt.connected()){
    Serial.print("[MQTT] Connecting...");
    mqtt.setCallback(mqttCallback);
    if(mqtt.connect("ESP32_AirMonitor", mqtt_user, mqtt_pass)){
      Serial.println("connected!");
      mqtt.subscribe(ha_status_topic);
      publishAllDiscovery();
      mqtt.publish(ha_avail_topic, "online", true);
    } else {
      Serial.print("failed, rc=");
      Serial.print(mqtt.state());
      Serial.println(", retrying in 2s");
      delay(2000);
    }
  }
}

// -------- Web Interface --------
void handleRoot(){
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Air Quality Monitor</title>";
  html += "<meta http-equiv='refresh' content='2'>";
  html += "<style>body{font-family:sans-serif;text-align:center;} table{margin:auto;border-collapse:collapse;} td,th{padding:8px;border:1px solid #333;}</style>";
  html += "</head><body><h2>ESP32 Air Quality Monitor</h2><table>";
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
  html += "</table></body></html>";
  server.send(200,"text/html",html);
}

void handleJson(){
  StaticJsonDocument<256> doc;
  doc["co2"] = co2_ppm;
  if(!isnan(tC_scd)) doc["t_scd"] = tC_scd;
  if(!isnan(rh_scd)) doc["rh_scd"] = rh_scd;
  doc["t_aht"] = tC_aht;
  doc["rh_aht"] = rh_aht;
  doc["tvoc"] = tvoc_ppb;
  doc["eco2"] = eco2_ppm;
  doc["aqi"] = aqi;
  String output;
  serializeJson(doc, output);
  server.send(200,"application/json",output);
}

// -------- Setup --------
void setup(){
  Serial.begin(115200);
  delay(200);

  WiFi.begin(ssid,password);
  while(WiFi.status()!=WL_CONNECTED){ delay(500); Serial.print("."); }
  Serial.println("\n[WiFi] Connected, IP: " + WiFi.localIP().toString());

  Wire.begin(SDA_PIN,SCL_PIN);
  Wire.setClock(400000);

  ens.begin(); ens.setOperatingMode(SFE_ENS160_STANDARD);
  aht.begin();
  scd4x.begin(Wire,SCD41_ADDR);
  scd4x.stopPeriodicMeasurement(); delay(500); scd4x.startPeriodicMeasurement();

  server.on("/", handleRoot);
  server.on("/data.json", handleJson);
  server.begin();
  Serial.println("[Web] Server started");

  mqtt.setServer(mqtt_server,mqtt_port);
  reconnectMQTT();
}

// -------- Loop --------
void loop(){
  if(!mqtt.connected()) reconnectMQTT();
  mqtt.loop();

  // SCD41
  bool ready=false;
  if(scd4x.getDataReadyStatus(ready)==0 && ready){
    uint16_t co2; float t,rh;
    if(scd4x.readMeasurement(co2,t,rh)==0 && co2!=0){
      co2_ppm=co2; tC_scd=t; rh_scd=rh;
    }
  }

  // AHT21
  sensors_event_t hum,temp;
  if(aht.getEvent(&hum,&temp)){
    tC_aht=temp.temperature; rh_aht=hum.relative_humidity;
  }

  // ENS160
  if(ens.checkDataStatus()){
    tvoc_ppb=ens.getTVOC(); eco2_ppm=ens.getECO2(); aqi=ens.getAQI();
  }

  static uint32_t lastMQTT=0;
  if(millis()-lastMQTT>5000){
    lastMQTT=millis();
    StaticJsonDocument<256> doc;
    doc["co2"] = co2_ppm;
    if(!isnan(tC_scd)) doc["t_scd"] = tC_scd;
    if(!isnan(rh_scd)) doc["rh_scd"] = rh_scd;
    doc["t_aht"] = tC_aht;
    doc["rh_aht"] = rh_aht;
    doc["tvoc"] = tvoc_ppb;
    doc["eco2"] = eco2_ppm;
    doc["aqi"] = aqi;
    char buffer[256]; size_t n = serializeJson(doc, buffer);
    mqtt.publish(mqtt_topic,(const uint8_t*)buffer,n,true);
  }

  server.handleClient();
}
