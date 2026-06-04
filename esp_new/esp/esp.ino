#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <esp_system.h>

// ---------- USER CONFIG ------------------------------------------------------
// WiFi
const char* WIFI_SSID     = "CONI";
const char* WIFI_PASSWORD = "Seif2004!";

const char* MQTT_HOST     = "06a2ba7fa5274bd89278d9107b2f4f8b.s1.eu.hivemq.cloud";
const uint16_t MQTT_PORT  = 8883;                  
const char* MQTT_USER     = "Car_Antitheft";
const char* MQTT_PASS     = "Tttt7504";

const char* DEVICE_ID     = "car001";

// UART to STM32
#define RXD2 16
#define TXD2 17

WiFiClientSecure  wifiClient;
PubSubClient      mqtt(wifiClient);

// State mirrored from STM32
String currentStatus  = "ARMED";
String doorStatus     = "CLOSED";
String engineStatus   = "OFF";
String netStatus      = "LOST";
double lastLat        = 0.0;
double lastLon        = 0.0;
bool   alarmTriggered = false;
bool   alarmPublished = false;

// Topics (built once in setup)
String topicState;
String topicAlarm;
String topicOnline;
String topicCmd;

//  WiFi
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi connecting");

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi OK, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi failed — will keep retrying in loop().");
  }
}

//  MQTT
void onMqttMessage(char* topic, byte* payload, unsigned int len) {
  String msg;
  msg.reserve(len);
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];

  Serial.printf("MQTT << %s : %s\n", topic, msg.c_str());

  if (String(topic) == topicCmd) {
    if (msg.equalsIgnoreCase("ARM")) {
      Serial2.println("CMD:ARM");
    } else if (msg.equalsIgnoreCase("DISARM")) {
      Serial2.println("CMD:DISARM");
    }
  }
}

void connectMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;

  // No CA pinning — encrypted but not authenticated. Good enough for hobby.
  wifiClient.setInsecure();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(30);

  String clientId = String("esp32-") + DEVICE_ID + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);

  Serial.printf("MQTT connecting to %s:%u as %s ...\n", MQTT_HOST, MQTT_PORT, clientId.c_str());

  bool ok = mqtt.connect(
      clientId.c_str(),
      MQTT_USER, MQTT_PASS,
      topicOnline.c_str(), 1, true, "false");

  if (ok) {
    Serial.println("MQTT connected");
    mqtt.publish(topicOnline.c_str(), "true", true);
    mqtt.subscribe(topicCmd.c_str(), 1);
    Serial.printf("Subscribed: %s\n", topicCmd.c_str());
  } else {
    Serial.printf("MQTT connect failed, state=%d\n", mqtt.state());
  }
}

void publishState() {
  if (!mqtt.connected()) return;

  StaticJsonDocument<320> doc;
  doc["status"] = currentStatus;
  doc["door"]   = doorStatus;
  doc["engine"] = engineStatus;
  doc["net"]    = netStatus;
  doc["lat"]    = lastLat;
  doc["lon"]    = lastLon;
  doc["heap"]   = ESP.getFreeHeap();
  doc["ts"]     = millis();

  char buf[320];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  mqtt.publish(topicState.c_str(), (const uint8_t*)buf, n, true);
}

//  UART parser (unchanged protocol from STM32)
void parseSTM32Data(String data) {
  if (data.startsWith("STATUS:")) {
    currentStatus = data.substring(7);
  }
  else if (data.startsWith("GPS:")) {
    int comma = data.indexOf(',');
    if (comma > 4) {
      double newLat = data.substring(4, comma).toDouble();
      double newLon = data.substring(comma + 1).toDouble();
      if (newLat != 0.0 && newLon != 0.0 &&
          abs(newLat) <= 90.0 && abs(newLon) <= 180.0) {
        lastLat = newLat;
        lastLon = newLon;
      }
    }
  }
  else if (data.startsWith("DOOR:"))   { doorStatus   = data.substring(5); }
  else if (data.startsWith("ENGINE:")) { engineStatus = data.substring(7); }
  else if (data.startsWith("NET:"))    { netStatus    = data.substring(4); }
  else if (data.startsWith("ALARM:"))  { alarmTriggered = true; }
}

//  setup / loop
void setup() {
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2);
  delay(500);

  Serial.println("\n== Car Security ESP32 MQTT bridge ==");
  Serial.printf("Reset reason: %d\n", esp_reset_reason());

  // Build topics once
  String base  = String("carsec/") + DEVICE_ID;
  topicState   = base + "/state";
  topicAlarm   = base + "/alarm";
  topicOnline  = base + "/online";
  topicCmd     = base + "/cmd";

  connectWiFi();
  connectMQTT();
}

void loop() {
  // Drain UART from STM32
  while (Serial2.available()) {
    String data = Serial2.readStringUntil('\n');
    data.trim();
    if (data.length() > 0) parseSTM32Data(data);
  }

  // Maintain WiFi
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastWifiTry = 0;
    if (millis() - lastWifiTry > 5000) {
      lastWifiTry = millis();
      Serial.println("WiFi lost, reconnecting...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  }

  // Maintain MQTT
  if (WiFi.status() == WL_CONNECTED && !mqtt.connected()) {
    static unsigned long lastMqttTry = 0;
    if (millis() - lastMqttTry > 3000) {
      lastMqttTry = millis();
      connectMQTT();
    }
  }
  mqtt.loop();

  // Periodic state publish (1 Hz)
  static unsigned long lastPub = 0;
  if (millis() - lastPub > 1000) {
    lastPub = millis();
    publishState();
  }

  // Alarm edge: publish once per trigger
  if (alarmTriggered && !alarmPublished && mqtt.connected()) {
    mqtt.publish(topicAlarm.c_str(), "1", true);
    alarmPublished = true;
  }
  // Auto-clear once status leaves ALARM
  if (currentStatus != "ALARM") {
    alarmTriggered = false;
    alarmPublished = false;
  }

  delay(10);
}
