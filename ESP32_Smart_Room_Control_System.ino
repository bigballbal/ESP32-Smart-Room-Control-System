#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>

#define AC_ON_TEMP 30
#define AC_OFF_TEMP 25

#define DHTPIN 4
#define DHTTYPE DHT11

#define SENSOR_FAIL_LIMIT 5

// Offline Recovery Settings
#define WIFI_RETRY_INTERVAL 5000
#define WIFI_CONNECT_TIMEOUT 8000

// Data Logging Settings
#define LOG_BUFFER_SIZE 60
#define LOG_INTERVAL_MS 5000

// MQTT Settings
#define MQTT_BROKER "broker.emqx.io"
#define MQTT_PORT 1883
#define MQTT_RETRY_INTERVAL 5000
#define MQTT_PUBLISH_INTERVAL 5000

#define DEVICE_ID "esp32-smart-room-01"

#define MQTT_TOPIC_STATUS       "linballball/smartroom/esp32-smart-room-01/status"
#define MQTT_TOPIC_HEALTH       "linballball/smartroom/esp32-smart-room-01/health"
#define MQTT_TOPIC_LOG          "linballball/smartroom/esp32-smart-room-01/log"
#define MQTT_TOPIC_AVAILABILITY "linballball/smartroom/esp32-smart-room-01/availability"

#define MQTT_TOPIC_CMD_MODE     "linballball/smartroom/esp32-smart-room-01/cmd/mode"
#define MQTT_TOPIC_CMD_LIGHT    "linballball/smartroom/esp32-smart-room-01/cmd/light"
#define MQTT_TOPIC_CMD_AC       "linballball/smartroom/esp32-smart-room-01/cmd/ac"
#define MQTT_TOPIC_CMD_CONTROL  "linballball/smartroom/esp32-smart-room-01/cmd/control"

typedef enum { MANUAL_MODE, AUTO_MODE } Mode;
typedef enum { LOG_SENSOR, LOG_CONTROL, LOG_SYSTEM, LOG_ERROR, LOG_MQTT } LogType;

typedef struct {
    Mode mode;
    int light;
    int temperature;
    int humidity;
    int ac;
} RoomSetting;

typedef struct {
    unsigned long timestampMs;
    LogType type;
    Mode mode;
    int light;
    int ac;
    int temperature;
    int humidity;
    bool sensorHealthy;
    bool wifiConnected;
    bool mqttConnected;
    int sensorFailCount;
    char message[64];
} DataLogEntry;

// Replace these with your own WiFi credentials before uploading to ESP32
const char* ssid = "YOUR_WIFI_NAME";
const char* password = "YOUR_WIFI_PASSWORD";

LiquidCrystal_I2C lcd(0x27, 16, 2);
DHT dht(DHTPIN, DHTTYPE);
WebServer server(80);

WiFiClient espClient;
PubSubClient mqttClient(espClient);

const int lightPin = 25;
const int acPin = 26;

// Shared room state
RoomSetting myRoom = {MANUAL_MODE, 0, 25, 0, 0};

// Sensor health shared data
int sensorFailCount = 0;
bool sensorHealthy = true;
unsigned long lastSensorSuccessTime = 0;

// WiFi health shared data
bool wifiConnected = false;
unsigned long lastWiFiRetryTime = 0;

// MQTT health shared data
bool mqttConnected = false;
unsigned long lastMqttRetryTime = 0;
unsigned long lastMqttPublishTime = 0;

// Data logging buffer
DataLogEntry logBuffer[LOG_BUFFER_SIZE];
int logHead = 0;
int logCount = 0;
unsigned long totalLogCount = 0;

// Mutex
SemaphoreHandle_t roomMutex;
SemaphoreHandle_t logMutex;

// Task handles
TaskHandle_t sensorTaskHandle = NULL;
TaskHandle_t controlTaskHandle = NULL;
TaskHandle_t hardwareTaskHandle = NULL;
TaskHandle_t webTaskHandle = NULL;
TaskHandle_t monitorTaskHandle = NULL;
TaskHandle_t wifiTaskHandle = NULL;
TaskHandle_t dataLogTaskHandle = NULL;
TaskHandle_t mqttTaskHandle = NULL;

// ========================
// REST API Design
// ========================
// GET  /api/status
// GET  /api/health
// GET  /api/logs
// POST /api/logs/clear
// POST /api/mode?value=AUTO
// POST /api/mode?value=MANUAL
// POST /api/light?value=1
// POST /api/light?value=0
// POST /api/light?value=toggle
// POST /api/ac?value=1
// POST /api/ac?value=0
// POST /api/ac?value=toggle
// POST /api/control?light=1&ac=0

// ========================
// MQTT Topic Design
// ========================
// Publish:
// linballball/smartroom/esp32-smart-room-01/status
// linballball/smartroom/esp32-smart-room-01/health
// linballball/smartroom/esp32-smart-room-01/log
// linballball/smartroom/esp32-smart-room-01/availability
//
// Subscribe:
// linballball/smartroom/esp32-smart-room-01/cmd/mode
// payload: AUTO / MANUAL / {"mode":"AUTO"}
//
// linballball/smartroom/esp32-smart-room-01/cmd/light
// payload: 1 / 0 / toggle / {"light":1}
//
// linballball/smartroom/esp32-smart-room-01/cmd/ac
// payload: 1 / 0 / toggle / {"ac":1}
//
// linballball/smartroom/esp32-smart-room-01/cmd/control
// payload: {"light":1,"ac":0}

// ========================
// Utility Functions
// ========================
const char* modeToString(Mode mode) {
    return mode == AUTO_MODE ? "AUTO" : "MANUAL";
}

const char* logTypeToString(LogType type) {
    switch (type) {
        case LOG_SENSOR:  return "SENSOR";
        case LOG_CONTROL: return "CONTROL";
        case LOG_SYSTEM:  return "SYSTEM";
        case LOG_ERROR:   return "ERROR";
        case LOG_MQTT:    return "MQTT";
        default:          return "UNKNOWN";
    }
}

bool parseBoolValue(const String& raw, bool& out) {
    String value = raw;
    value.trim();
    value.toLowerCase();

    if (value == "1" || value == "on" || value == "true") {
        out = true;
        return true;
    }

    if (value == "0" || value == "off" || value == "false") {
        out = false;
        return true;
    }

    return false;
}

bool isToggleValue(const String& raw) {
    String value = raw;
    value.trim();
    value.toLowerCase();
    return value == "toggle";
}

bool parseModeValue(const String& raw, Mode& out) {
    String value = raw;
    value.trim();
    value.toUpperCase();

    if (value == "AUTO") {
        out = AUTO_MODE;
        return true;
    }

    if (value == "MANUAL") {
        out = MANUAL_MODE;
        return true;
    }

    return false;
}

void addCorsHeaders() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void sendJsonDocument(int statusCode, JsonDocument& doc) {
    String response;
    serializeJson(doc, response);
    addCorsHeaders();
    server.send(statusCode, "application/json", response);
}

void sendJsonOk(const char* message) {
    StaticJsonDocument<128> doc;
    doc["ok"] = true;
    doc["message"] = message;
    sendJsonDocument(200, doc);
}

void sendJsonError(int statusCode, const char* errorCode, const char* message) {
    StaticJsonDocument<256> doc;
    doc["ok"] = false;
    doc["error"] = errorCode;
    doc["message"] = message;
    sendJsonDocument(statusCode, doc);
}

bool getJsonBody(StaticJsonDocument<256>& doc) {
    if (!server.hasArg("plain")) {
        return false;
    }

    String body = server.arg("plain");
    body.trim();

    if (body.length() == 0) {
        return false;
    }

    DeserializationError error = deserializeJson(doc, body);

    if (error) {
        return false;
    }

    return true;
}

bool getRequestValue(const char* key, String& out) {
    if (server.hasArg(key)) {
        out = server.arg(key);
        return true;
    }

    StaticJsonDocument<256> doc;
    if (!getJsonBody(doc)) {
        return false;
    }

    if (!doc.containsKey(key)) {
        return false;
    }

    if (doc[key].is<const char*>()) {
        out = String(doc[key].as<const char*>());
    } else if (doc[key].is<int>()) {
        out = String(doc[key].as<int>());
    } else if (doc[key].is<bool>()) {
        out = doc[key].as<bool>() ? "true" : "false";
    } else {
        return false;
    }

    return true;
}

bool getSnapshot(RoomSetting& snapshot,
                 bool& sensorOk,
                 bool& wifiOk,
                 bool& mqttOk,
                 int& failCount,
                 unsigned long& lastSuccess) {
    if (xSemaphoreTake(roomMutex, portMAX_DELAY)) {
        snapshot = myRoom;
        sensorOk = sensorHealthy;
        wifiOk = wifiConnected;
        mqttOk = mqttConnected;
        failCount = sensorFailCount;
        lastSuccess = lastSensorSuccessTime;
        xSemaphoreGive(roomMutex);
        return true;
    }

    return false;
}

void setMqttConnectedState(bool value) {
    if (xSemaphoreTake(roomMutex, portMAX_DELAY)) {
        mqttConnected = value;
        xSemaphoreGive(roomMutex);
    }
}

// ========================
// Data Logging Layer
// ========================
void addDataLog(LogType type, const char* message) {
    RoomSetting snapshot;
    bool sensorOk;
    bool wifiOk;
    bool mqttOk;
    int failCount;
    unsigned long lastSuccess;

    if (!getSnapshot(snapshot, sensorOk, wifiOk, mqttOk, failCount, lastSuccess)) {
        return;
    }

    if (xSemaphoreTake(logMutex, portMAX_DELAY)) {
        DataLogEntry& entry = logBuffer[logHead];

        entry.timestampMs = millis();
        entry.type = type;
        entry.mode = snapshot.mode;
        entry.light = snapshot.light;
        entry.ac = snapshot.ac;
        entry.temperature = snapshot.temperature;
        entry.humidity = snapshot.humidity;
        entry.sensorHealthy = sensorOk;
        entry.wifiConnected = wifiOk;
        entry.mqttConnected = mqttOk;
        entry.sensorFailCount = failCount;

        strncpy(entry.message, message, sizeof(entry.message) - 1);
        entry.message[sizeof(entry.message) - 1] = '\0';

        logHead = (logHead + 1) % LOG_BUFFER_SIZE;

        if (logCount < LOG_BUFFER_SIZE) {
            logCount++;
        }

        totalLogCount++;

        xSemaphoreGive(logMutex);
    }
}

void clearDataLogs() {
    if (xSemaphoreTake(logMutex, portMAX_DELAY)) {
        logHead = 0;
        logCount = 0;
        totalLogCount = 0;
        xSemaphoreGive(logMutex);
    }
}

// ========================
// Control Layer
// ========================
bool setModeValue(Mode newMode) {
    bool changed = false;

    if (xSemaphoreTake(roomMutex, portMAX_DELAY)) {
        changed = myRoom.mode != newMode;
        myRoom.mode = newMode;
        xSemaphoreGive(roomMutex);
    } else {
        return false;
    }

    if (changed) {
        addDataLog(LOG_CONTROL, newMode == AUTO_MODE ? "Mode changed to AUTO" : "Mode changed to MANUAL");
    }

    return true;
}

bool setLightValue(bool value, bool toggle) {
    int newLightValue = 0;

    if (xSemaphoreTake(roomMutex, portMAX_DELAY)) {
        if (myRoom.mode != MANUAL_MODE) {
            xSemaphoreGive(roomMutex);
            return false;
        }

        if (toggle) {
            myRoom.light = !myRoom.light;
        } else {
            myRoom.light = value ? 1 : 0;
        }

        newLightValue = myRoom.light;
        xSemaphoreGive(roomMutex);
    } else {
        return false;
    }

    addDataLog(LOG_CONTROL, newLightValue ? "Light turned ON" : "Light turned OFF");
    return true;
}

bool setACValue(bool value, bool toggle) {
    int newACValue = 0;

    if (xSemaphoreTake(roomMutex, portMAX_DELAY)) {
        if (myRoom.mode != MANUAL_MODE) {
            xSemaphoreGive(roomMutex);
            return false;
        }

        if (toggle) {
            myRoom.ac = !myRoom.ac;
        } else {
            myRoom.ac = value ? 1 : 0;
        }

        newACValue = myRoom.ac;
        xSemaphoreGive(roomMutex);
    } else {
        return false;
    }

    addDataLog(LOG_CONTROL, newACValue ? "AC turned ON" : "AC turned OFF");
    return true;
}

// ========================
// Auto Control Logic
// This function assumes the caller already holds the mutex
// ========================
bool runAutoControlUnsafe() {
    int oldAC = myRoom.ac;

    if (myRoom.temperature > AC_ON_TEMP) {
        myRoom.ac = 1;
    } else if (myRoom.temperature < AC_OFF_TEMP) {
        myRoom.ac = 0;
    }

    return oldAC != myRoom.ac;
}

// ========================
// WiFi Status Update
// non-blocking WiFi recovery
// ========================
void updateWiFiStatus() {
    wl_status_t status = WiFi.status();

    if (status == WL_CONNECTED) {
        bool justReconnected = false;

        if (xSemaphoreTake(roomMutex, portMAX_DELAY)) {
            if (!wifiConnected) {
                wifiConnected = true;
                justReconnected = true;

                Serial.println("[WiFi] Reconnected");
                Serial.print("[WiFi] IP: ");
                Serial.println(WiFi.localIP());
            }
            xSemaphoreGive(roomMutex);
        }

        if (justReconnected) {
            addDataLog(LOG_SYSTEM, "WiFi reconnected");
        }

        return;
    }

    bool justDisconnected = false;

    if (xSemaphoreTake(roomMutex, portMAX_DELAY)) {
        if (wifiConnected) {
            wifiConnected = false;
            mqttConnected = false;
            justDisconnected = true;
            Serial.println("[WiFi] Disconnected");
        }
        xSemaphoreGive(roomMutex);
    }

    if (justDisconnected) {
        addDataLog(LOG_ERROR, "WiFi disconnected");
    }

    unsigned long now = millis();

    if (now - lastWiFiRetryTime >= WIFI_RETRY_INTERVAL) {
        lastWiFiRetryTime = now;

        Serial.println("[WiFi] Trying to reconnect...");
        WiFi.disconnect();
        WiFi.begin(ssid, password);
    }
}

// ========================
// MQTT Layer
// ========================
void publishMqttStatus() {
    RoomSetting snapshot;
    bool sensorOk;
    bool wifiOk;
    bool mqttOk;
    int failCount;
    unsigned long lastSuccess;

    if (!getSnapshot(snapshot, sensorOk, wifiOk, mqttOk, failCount, lastSuccess)) {
        return;
    }

    StaticJsonDocument<512> doc;

    doc["deviceId"] = DEVICE_ID;
    doc["uptimeMs"] = millis();

    JsonObject room = doc.createNestedObject("room");
    room["mode"] = modeToString(snapshot.mode);
    room["light"] = snapshot.light;
    room["ac"] = snapshot.ac;
    room["temperature"] = snapshot.temperature;
    room["humidity"] = snapshot.humidity;

    JsonObject health = doc.createNestedObject("health");
    health["sensorHealthy"] = sensorOk;
    health["wifiConnected"] = wifiOk;
    health["mqttConnected"] = mqttOk;
    health["sensorFailCount"] = failCount;
    health["lastSensorSuccessMs"] = lastSuccess;

    if (WiFi.status() == WL_CONNECTED) {
        health["ip"] = WiFi.localIP().toString();
        health["rssi"] = WiFi.RSSI();
    } else {
        health["ip"] = "";
        health["rssi"] = 0;
    }

    char payload[512];
    size_t length = serializeJson(doc, payload, sizeof(payload));

    mqttClient.publish(MQTT_TOPIC_STATUS, payload, length);
}

void publishMqttHealth() {
    StaticJsonDocument<384> doc;

    RoomSetting snapshot;
    bool sensorOk;
    bool wifiOk;
    bool mqttOk;
    int failCount;
    unsigned long lastSuccess;

    if (!getSnapshot(snapshot, sensorOk, wifiOk, mqttOk, failCount, lastSuccess)) {
        return;
    }

    doc["deviceId"] = DEVICE_ID;
    doc["uptimeMs"] = millis();
    doc["freeHeap"] = ESP.getFreeHeap();
    doc["sensorHealthy"] = sensorOk;
    doc["wifiConnected"] = wifiOk;
    doc["mqttConnected"] = mqttOk;
    doc["sensorFailCount"] = failCount;

    if (WiFi.status() == WL_CONNECTED) {
        doc["ip"] = WiFi.localIP().toString();
        doc["rssi"] = WiFi.RSSI();
    } else {
        doc["ip"] = "";
        doc["rssi"] = 0;
    }

    char payload[384];
    size_t length = serializeJson(doc, payload, sizeof(payload));

    mqttClient.publish(MQTT_TOPIC_HEALTH, payload, length);
}

void publishMqttLog(const char* message) {
    StaticJsonDocument<256> doc;

    doc["deviceId"] = DEVICE_ID;
    doc["timestampMs"] = millis();
    doc["message"] = message;

    char payload[256];
    size_t length = serializeJson(doc, payload, sizeof(payload));

    mqttClient.publish(MQTT_TOPIC_LOG, payload, length);
}

String extractMqttValue(const String& payload, const char* key) {
    StaticJsonDocument<256> doc;

    DeserializationError error = deserializeJson(doc, payload);

    if (!error && doc.containsKey(key)) {
        if (doc[key].is<const char*>()) {
            return String(doc[key].as<const char*>());
        }

        if (doc[key].is<int>()) {
            return String(doc[key].as<int>());
        }

        if (doc[key].is<bool>()) {
            return doc[key].as<bool>() ? "true" : "false";
        }
    }

    return payload;
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    char message[256];

    if (length >= sizeof(message)) {
        length = sizeof(message) - 1;
    }

    memcpy(message, payload, length);
    message[length] = '\0';

    String topicStr = String(topic);
    String payloadStr = String(message);
    payloadStr.trim();

    Serial.print("[MQTT] Message arrived: ");
    Serial.print(topicStr);
    Serial.print(" => ");
    Serial.println(payloadStr);

    if (topicStr == MQTT_TOPIC_CMD_MODE) {
        String raw = extractMqttValue(payloadStr, "mode");

        Mode newMode;
        if (parseModeValue(raw, newMode)) {
            setModeValue(newMode);
            addDataLog(LOG_MQTT, "MQTT mode command received");
            publishMqttLog("MQTT mode command received");
        } else {
            addDataLog(LOG_ERROR, "Invalid MQTT mode command");
            publishMqttLog("Invalid MQTT mode command");
        }
    }

    else if (topicStr == MQTT_TOPIC_CMD_LIGHT) {
        String raw = extractMqttValue(payloadStr, "light");

        bool toggle = isToggleValue(raw);
        bool value = false;

        if (toggle || parseBoolValue(raw, value)) {
            if (setLightValue(value, toggle)) {
                addDataLog(LOG_MQTT, "MQTT light command received");
                publishMqttLog("MQTT light command received");
            } else {
                addDataLog(LOG_ERROR, "MQTT light rejected: not MANUAL mode");
                publishMqttLog("MQTT light rejected: not MANUAL mode");
            }
        } else {
            addDataLog(LOG_ERROR, "Invalid MQTT light command");
            publishMqttLog("Invalid MQTT light command");
        }
    }

    else if (topicStr == MQTT_TOPIC_CMD_AC) {
        String raw = extractMqttValue(payloadStr, "ac");

        bool toggle = isToggleValue(raw);
        bool value = false;

        if (toggle || parseBoolValue(raw, value)) {
            if (setACValue(value, toggle)) {
                addDataLog(LOG_MQTT, "MQTT AC command received");
                publishMqttLog("MQTT AC command received");
            } else {
                addDataLog(LOG_ERROR, "MQTT AC rejected: not MANUAL mode");
                publishMqttLog("MQTT AC rejected: not MANUAL mode");
            }
        } else {
            addDataLog(LOG_ERROR, "Invalid MQTT AC command");
            publishMqttLog("Invalid MQTT AC command");
        }
    }

    else if (topicStr == MQTT_TOPIC_CMD_CONTROL) {
        StaticJsonDocument<256> doc;
        DeserializationError error = deserializeJson(doc, payloadStr);

        if (error) {
            addDataLog(LOG_ERROR, "Invalid MQTT control JSON");
            publishMqttLog("Invalid MQTT control JSON");
            return;
        }

        if (doc.containsKey("light")) {
            String rawLight;

            if (doc["light"].is<const char*>()) {
                rawLight = String(doc["light"].as<const char*>());
            } else if (doc["light"].is<int>()) {
                rawLight = String(doc["light"].as<int>());
            } else if (doc["light"].is<bool>()) {
                rawLight = doc["light"].as<bool>() ? "true" : "false";
            }

            bool toggle = isToggleValue(rawLight);
            bool value = false;

            if (toggle || parseBoolValue(rawLight, value)) {
                setLightValue(value, toggle);
            }
        }

        if (doc.containsKey("ac")) {
            String rawAC;

            if (doc["ac"].is<const char*>()) {
                rawAC = String(doc["ac"].as<const char*>());
            } else if (doc["ac"].is<int>()) {
                rawAC = String(doc["ac"].as<int>());
            } else if (doc["ac"].is<bool>()) {
                rawAC = doc["ac"].as<bool>() ? "true" : "false";
            }

            bool toggle = isToggleValue(rawAC);
            bool value = false;

            if (toggle || parseBoolValue(rawAC, value)) {
                setACValue(value, toggle);
            }
        }

        addDataLog(LOG_MQTT, "MQTT control command received");
        publishMqttLog("MQTT control command received");
    }

    publishMqttStatus();
}

void subscribeMqttTopics() {
    mqttClient.subscribe(MQTT_TOPIC_CMD_MODE);
    mqttClient.subscribe(MQTT_TOPIC_CMD_LIGHT);
    mqttClient.subscribe(MQTT_TOPIC_CMD_AC);
    mqttClient.subscribe(MQTT_TOPIC_CMD_CONTROL);

    Serial.println("[MQTT] Subscribed command topics");
}

void updateMqttConnection() {
    bool wifiOk = false;

    if (xSemaphoreTake(roomMutex, portMAX_DELAY)) {
        wifiOk = wifiConnected;
        xSemaphoreGive(roomMutex);
    }

    if (!wifiOk) {
        if (mqttConnected) {
            setMqttConnectedState(false);
            addDataLog(LOG_MQTT, "MQTT offline because WiFi is offline");
        }
        return;
    }

    if (mqttClient.connected()) {
        if (!mqttConnected) {
            setMqttConnectedState(true);
            addDataLog(LOG_MQTT, "MQTT connected");
        }

        mqttClient.loop();
        return;
    }

    if (mqttConnected) {
        setMqttConnectedState(false);
        addDataLog(LOG_MQTT, "MQTT disconnected");
    }

    unsigned long now = millis();

    if (now - lastMqttRetryTime < MQTT_RETRY_INTERVAL) {
        return;
    }

    lastMqttRetryTime = now;

    Serial.println("[MQTT] Trying to connect...");

    String clientId = String(DEVICE_ID) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);

    bool connected = mqttClient.connect(
        clientId.c_str(),
        MQTT_TOPIC_AVAILABILITY,
        1,
        true,
        "offline"
    );

    if (connected) {
        Serial.println("[MQTT] Connected");

        setMqttConnectedState(true);

        mqttClient.publish(MQTT_TOPIC_AVAILABILITY, "online", true);
        subscribeMqttTopics();

        addDataLog(LOG_MQTT, "MQTT connected");
        publishMqttLog("MQTT connected");
        publishMqttStatus();
        publishMqttHealth();
    } else {
        Serial.print("[MQTT] Connection failed, rc=");
        Serial.println(mqttClient.state());

        setMqttConnectedState(false);
        addDataLog(LOG_ERROR, "MQTT connection failed");
    }
}

void publishMqttPeriodically() {
    if (!mqttClient.connected()) {
        return;
    }

    unsigned long now = millis();

    if (now - lastMqttPublishTime >= MQTT_PUBLISH_INTERVAL) {
        lastMqttPublishTime = now;
        publishMqttStatus();
        publishMqttHealth();
    }
}

// ========================
// Hardware Rendering
// ========================
void renderHardware(const RoomSetting& snapshot, bool sensorOk, bool wifiOk, bool mqttOk) {
    digitalWrite(lightPin, snapshot.light ? HIGH : LOW);

    // AC is currently simulated with an LED on GPIO26.
    // If this output is later connected to an active-low relay module,
    // use LOW for ON and HIGH for OFF.
    digitalWrite(acPin, snapshot.ac ? HIGH : LOW);

    lcd.setCursor(0, 0);
    lcd.print("                ");
    lcd.setCursor(0, 0);

    lcd.print("T:");
    lcd.print(snapshot.temperature);
    lcd.print(" H:");
    lcd.print(snapshot.humidity);
    lcd.print(" A:");
    lcd.print(snapshot.ac ? "ON" : "OF");

    lcd.setCursor(15, 0);
    lcd.print(snapshot.mode == AUTO_MODE ? "A" : "M");

    lcd.setCursor(0, 1);
    lcd.print("                ");
    lcd.setCursor(0, 1);

    lcd.print("L:");
    lcd.print(snapshot.light ? "ON " : "OF ");
    lcd.print("W:");
    lcd.print(wifiOk ? "OK " : "ER ");
    lcd.print("M:");
    lcd.print(mqttOk ? "OK" : "ER");
}

// ========================
// Optional HTML Dashboard
// ========================
String htmlPage() {
    String page = "";
    page += "<!DOCTYPE html><html><head>";
    page += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    page += "<style>";
    page += "body{font-family:Arial;text-align:center;background:#f4f4f4;padding:20px;}";
    page += ".card{background:white;padding:20px;border-radius:12px;max-width:520px;margin:auto;box-shadow:0 2px 8px #aaa;}";
    page += "button{font-size:16px;padding:12px 16px;margin:6px;border-radius:8px;border:0;background:#222;color:white;}";
    page += "pre{text-align:left;background:#111;color:#0f0;padding:12px;border-radius:8px;overflow:auto;max-height:360px;}";
    page += "</style></head><body>";
    page += "<div class='card'>";
    page += "<h2>ESP32 Smart Room API + MQTT</h2>";
    page += "<p>This page uses REST API endpoints. MQTT runs in the background.</p>";
    page += "<button onclick=\"post('/api/mode?value=AUTO')\">AUTO</button>";
    page += "<button onclick=\"post('/api/mode?value=MANUAL')\">MANUAL</button><br>";
    page += "<button onclick=\"post('/api/light?value=toggle')\">Toggle Light</button>";
    page += "<button onclick=\"post('/api/ac?value=toggle')\">Toggle AC</button>";
    page += "<button onclick=\"loadLogs()\">Load Logs</button>";
    page += "<button onclick=\"post('/api/logs/clear')\">Clear Logs</button>";
    page += "<h3>Status</h3>";
    page += "<pre id='status'>Loading...</pre>";
    page += "<h3>Logs</h3>";
    page += "<pre id='logs'>Click Load Logs</pre>";
    page += "</div>";
    page += "<script>";
    page += "async function load(){let r=await fetch('/api/status');let j=await r.json();document.getElementById('status').textContent=JSON.stringify(j,null,2);}";
    page += "async function loadLogs(){let r=await fetch('/api/logs');let j=await r.json();document.getElementById('logs').textContent=JSON.stringify(j,null,2);}";
    page += "async function post(url){let r=await fetch(url,{method:'POST'});let j=await r.json();console.log(j);load();loadLogs();}";
    page += "setInterval(load,3000);load();";
    page += "</script></body></html>";
    return page;
}

// ========================
// REST API Handlers
// ========================
void handleRoot() {
    addCorsHeaders();
    server.send(200, "text/html", htmlPage());
}

void handleOptions() {
    addCorsHeaders();
    server.send(204);
}

void handleApiStatus() {
    RoomSetting snapshot;
    bool sensorOk;
    bool wifiOk;
    bool mqttOk;
    int failCount;
    unsigned long lastSuccess;

    if (!getSnapshot(snapshot, sensorOk, wifiOk, mqttOk, failCount, lastSuccess)) {
        sendJsonError(500, "MUTEX_ERROR", "Failed to read shared room state");
        return;
    }

    StaticJsonDocument<768> doc;
    doc["ok"] = true;

    JsonObject room = doc.createNestedObject("room");
    room["mode"] = modeToString(snapshot.mode);
    room["light"] = snapshot.light;
    room["ac"] = snapshot.ac;
    room["temperature"] = snapshot.temperature;
    room["humidity"] = snapshot.humidity;

    JsonObject health = doc.createNestedObject("health");
    health["sensorHealthy"] = sensorOk;
    health["wifiConnected"] = wifiOk;
    health["mqttConnected"] = mqttOk;
    health["sensorFailCount"] = failCount;
    health["lastSensorSuccessMs"] = lastSuccess;
    health["uptimeMs"] = millis();

    if (WiFi.status() == WL_CONNECTED) {
        health["ip"] = WiFi.localIP().toString();
        health["rssi"] = WiFi.RSSI();
    } else {
        health["ip"] = "";
        health["rssi"] = 0;
    }

    JsonObject mqtt = doc.createNestedObject("mqtt");
    mqtt["broker"] = MQTT_BROKER;
    mqtt["port"] = MQTT_PORT;
    mqtt["deviceId"] = DEVICE_ID;
    mqtt["statusTopic"] = MQTT_TOPIC_STATUS;
    mqtt["healthTopic"] = MQTT_TOPIC_HEALTH;
    mqtt["availabilityTopic"] = MQTT_TOPIC_AVAILABILITY;

    JsonObject logging = doc.createNestedObject("logging");
    logging["bufferSize"] = LOG_BUFFER_SIZE;
    logging["intervalMs"] = LOG_INTERVAL_MS;

    if (xSemaphoreTake(logMutex, portMAX_DELAY)) {
        logging["storedLogs"] = logCount;
        logging["totalLogs"] = totalLogCount;
        xSemaphoreGive(logMutex);
    }

    sendJsonDocument(200, doc);
}

void handleApiHealth() {
    RoomSetting snapshot;
    bool sensorOk;
    bool wifiOk;
    bool mqttOk;
    int failCount;
    unsigned long lastSuccess;

    if (!getSnapshot(snapshot, sensorOk, wifiOk, mqttOk, failCount, lastSuccess)) {
        sendJsonError(500, "MUTEX_ERROR", "Failed to read system health");
        return;
    }

    StaticJsonDocument<1280> doc;
    doc["ok"] = true;
    doc["sensorHealthy"] = sensorOk;
    doc["wifiConnected"] = wifiOk;
    doc["mqttConnected"] = mqttOk;
    doc["sensorFailCount"] = failCount;
    doc["lastSensorSuccessMs"] = lastSuccess;
    doc["uptimeMs"] = millis();
    doc["freeHeap"] = ESP.getFreeHeap();

    if (WiFi.status() == WL_CONNECTED) {
        doc["ip"] = WiFi.localIP().toString();
        doc["rssi"] = WiFi.RSSI();
    } else {
        doc["ip"] = "";
        doc["rssi"] = 0;
    }

    JsonObject logging = doc.createNestedObject("logging");
    logging["bufferSize"] = LOG_BUFFER_SIZE;
    logging["intervalMs"] = LOG_INTERVAL_MS;

    if (xSemaphoreTake(logMutex, portMAX_DELAY)) {
        logging["storedLogs"] = logCount;
        logging["totalLogs"] = totalLogCount;
        xSemaphoreGive(logMutex);
    }

    JsonObject mqtt = doc.createNestedObject("mqtt");
    mqtt["broker"] = MQTT_BROKER;
    mqtt["port"] = MQTT_PORT;
    mqtt["deviceId"] = DEVICE_ID;
    mqtt["publishIntervalMs"] = MQTT_PUBLISH_INTERVAL;
    mqtt["statusTopic"] = MQTT_TOPIC_STATUS;
    mqtt["cmdModeTopic"] = MQTT_TOPIC_CMD_MODE;
    mqtt["cmdLightTopic"] = MQTT_TOPIC_CMD_LIGHT;
    mqtt["cmdACTopic"] = MQTT_TOPIC_CMD_AC;
    mqtt["cmdControlTopic"] = MQTT_TOPIC_CMD_CONTROL;

    JsonObject stack = doc.createNestedObject("stackHighWaterMark");
    stack["sensorTask"] = sensorTaskHandle ? uxTaskGetStackHighWaterMark(sensorTaskHandle) : 0;
    stack["controlTask"] = controlTaskHandle ? uxTaskGetStackHighWaterMark(controlTaskHandle) : 0;
    stack["hardwareTask"] = hardwareTaskHandle ? uxTaskGetStackHighWaterMark(hardwareTaskHandle) : 0;
    stack["webTask"] = webTaskHandle ? uxTaskGetStackHighWaterMark(webTaskHandle) : 0;
    stack["monitorTask"] = monitorTaskHandle ? uxTaskGetStackHighWaterMark(monitorTaskHandle) : 0;
    stack["wifiTask"] = wifiTaskHandle ? uxTaskGetStackHighWaterMark(wifiTaskHandle) : 0;
    stack["dataLogTask"] = dataLogTaskHandle ? uxTaskGetStackHighWaterMark(dataLogTaskHandle) : 0;
    stack["mqttTask"] = mqttTaskHandle ? uxTaskGetStackHighWaterMark(mqttTaskHandle) : 0;

    sendJsonDocument(200, doc);
}

void handleApiLogs() {
    DynamicJsonDocument doc(8192);

    doc["ok"] = true;
    doc["bufferSize"] = LOG_BUFFER_SIZE;
    doc["intervalMs"] = LOG_INTERVAL_MS;

    JsonArray logs = doc.createNestedArray("logs");

    if (xSemaphoreTake(logMutex, portMAX_DELAY)) {
        doc["storedLogs"] = logCount;
        doc["totalLogs"] = totalLogCount;

        int startIndex = (logHead - logCount + LOG_BUFFER_SIZE) % LOG_BUFFER_SIZE;

        for (int i = 0; i < logCount; i++) {
            int index = (startIndex + i) % LOG_BUFFER_SIZE;
            DataLogEntry& entry = logBuffer[index];

            JsonObject item = logs.createNestedObject();
            item["timestampMs"] = entry.timestampMs;
            item["type"] = logTypeToString(entry.type);
            item["message"] = entry.message;
            item["mode"] = modeToString(entry.mode);
            item["light"] = entry.light;
            item["ac"] = entry.ac;
            item["temperature"] = entry.temperature;
            item["humidity"] = entry.humidity;
            item["sensorHealthy"] = entry.sensorHealthy;
            item["wifiConnected"] = entry.wifiConnected;
            item["mqttConnected"] = entry.mqttConnected;
            item["sensorFailCount"] = entry.sensorFailCount;
        }

        xSemaphoreGive(logMutex);
    } else {
        sendJsonError(500, "MUTEX_ERROR", "Failed to read logs");
        return;
    }

    sendJsonDocument(200, doc);
}

void handleApiClearLogs() {
    clearDataLogs();
    addDataLog(LOG_SYSTEM, "Logs cleared");
    sendJsonOk("Logs cleared");
}

void handleApiMode() {
    String raw;

    if (!getRequestValue("value", raw) && !getRequestValue("mode", raw)) {
        sendJsonError(400, "MISSING_VALUE", "Use /api/mode?value=AUTO or JSON {\"mode\":\"AUTO\"}");
        return;
    }

    Mode newMode;
    if (!parseModeValue(raw, newMode)) {
        sendJsonError(400, "INVALID_MODE", "Mode must be AUTO or MANUAL");
        return;
    }

    if (!setModeValue(newMode)) {
        sendJsonError(500, "MUTEX_ERROR", "Failed to update mode");
        return;
    }

    StaticJsonDocument<192> doc;
    doc["ok"] = true;
    doc["message"] = "Mode updated";
    doc["mode"] = modeToString(newMode);
    sendJsonDocument(200, doc);
}

void handleApiLight() {
    String raw;

    if (!getRequestValue("value", raw) && !getRequestValue("light", raw)) {
        sendJsonError(400, "MISSING_VALUE", "Use /api/light?value=1, 0, or toggle");
        return;
    }

    bool toggle = isToggleValue(raw);
    bool value = false;

    if (!toggle && !parseBoolValue(raw, value)) {
        sendJsonError(400, "INVALID_VALUE", "Light value must be 1, 0, true, false, on, off, or toggle");
        return;
    }

    if (!setLightValue(value, toggle)) {
        sendJsonError(409, "MODE_CONFLICT", "Light can only be controlled in MANUAL mode");
        return;
    }

    sendJsonOk("Light updated");
}

void handleApiAC() {
    String raw;

    if (!getRequestValue("value", raw) && !getRequestValue("ac", raw)) {
        sendJsonError(400, "MISSING_VALUE", "Use /api/ac?value=1, 0, or toggle");
        return;
    }

    bool toggle = isToggleValue(raw);
    bool value = false;

    if (!toggle && !parseBoolValue(raw, value)) {
        sendJsonError(400, "INVALID_VALUE", "AC value must be 1, 0, true, false, on, off, or toggle");
        return;
    }

    if (!setACValue(value, toggle)) {
        sendJsonError(409, "MODE_CONFLICT", "AC can only be controlled in MANUAL mode");
        return;
    }

    sendJsonOk("AC updated");
}

void handleApiControl() {
    bool hasLight = false;
    bool hasAC = false;
    String rawLight;
    String rawAC;

    hasLight = getRequestValue("light", rawLight);
    hasAC = getRequestValue("ac", rawAC);

    if (!hasLight && !hasAC) {
        sendJsonError(400, "MISSING_VALUE", "Use /api/control?light=1&ac=0 or JSON {\"light\":1,\"ac\":0}");
        return;
    }

    if (hasLight) {
        bool toggle = isToggleValue(rawLight);
        bool value = false;

        if (!toggle && !parseBoolValue(rawLight, value)) {
            sendJsonError(400, "INVALID_LIGHT", "Light must be 1, 0, true, false, on, off, or toggle");
            return;
        }

        if (!setLightValue(value, toggle)) {
            sendJsonError(409, "MODE_CONFLICT", "Light can only be controlled in MANUAL mode");
            return;
        }
    }

    if (hasAC) {
        bool toggle = isToggleValue(rawAC);
        bool value = false;

        if (!toggle && !parseBoolValue(rawAC, value)) {
            sendJsonError(400, "INVALID_AC", "AC must be 1, 0, true, false, on, off, or toggle");
            return;
        }

        if (!setACValue(value, toggle)) {
            sendJsonError(409, "MODE_CONFLICT", "AC can only be controlled in MANUAL mode");
            return;
        }
    }

    sendJsonOk("Control updated");
}

void handleNotFound() {
    sendJsonError(404, "NOT_FOUND", "Endpoint not found");
}

void registerRoutes() {
    server.on("/", HTTP_GET, handleRoot);

    server.on("/api/status", HTTP_GET, handleApiStatus);
    server.on("/api/health", HTTP_GET, handleApiHealth);
    server.on("/api/logs", HTTP_GET, handleApiLogs);
    server.on("/api/logs/clear", HTTP_POST, handleApiClearLogs);

    server.on("/api/mode", HTTP_POST, handleApiMode);
    server.on("/api/light", HTTP_POST, handleApiLight);
    server.on("/api/ac", HTTP_POST, handleApiAC);
    server.on("/api/control", HTTP_POST, handleApiControl);

    server.on("/api/status", HTTP_OPTIONS, handleOptions);
    server.on("/api/health", HTTP_OPTIONS, handleOptions);
    server.on("/api/logs", HTTP_OPTIONS, handleOptions);
    server.on("/api/logs/clear", HTTP_OPTIONS, handleOptions);
    server.on("/api/mode", HTTP_OPTIONS, handleOptions);
    server.on("/api/light", HTTP_OPTIONS, handleOptions);
    server.on("/api/ac", HTTP_OPTIONS, handleOptions);
    server.on("/api/control", HTTP_OPTIONS, handleOptions);

    server.onNotFound(handleNotFound);
}

// ========================
// Task 1: Sensor Task
// ========================
void sensorTask(void *parameter) {
    while (true) {
        float temp = dht.readTemperature();
        float hum = dht.readHumidity();

        if (!isnan(temp) && !isnan(hum)) {
            if (xSemaphoreTake(roomMutex, portMAX_DELAY)) {
                myRoom.temperature = (int)temp;
                myRoom.humidity = (int)hum;

                sensorFailCount = 0;
                sensorHealthy = true;
                lastSensorSuccessTime = millis();

                xSemaphoreGive(roomMutex);
            }

            Serial.print("[Sensor] Temp: ");
            Serial.print(temp);
            Serial.print(" C, Hum: ");
            Serial.println(hum);
        } else {
            bool sensorJustFailed = false;

            if (xSemaphoreTake(roomMutex, portMAX_DELAY)) {
                sensorFailCount++;

                if (sensorFailCount >= SENSOR_FAIL_LIMIT && sensorHealthy) {
                    sensorHealthy = false;
                    sensorJustFailed = true;
                }

                xSemaphoreGive(roomMutex);
            }

            Serial.println("[Sensor] Failed to read from DHT11");

            if (sensorJustFailed) {
                addDataLog(LOG_ERROR, "Sensor marked unhealthy");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

// ========================
// Task 2: Control Task
// ========================
void controlTask(void *parameter) {
    while (true) {
        bool acChanged = false;
        int newACValue = 0;

        if (xSemaphoreTake(roomMutex, portMAX_DELAY)) {
            if (myRoom.mode == AUTO_MODE && sensorHealthy) {
                acChanged = runAutoControlUnsafe();
                newACValue = myRoom.ac;
            }

            xSemaphoreGive(roomMutex);
        }

        if (acChanged) {
            addDataLog(LOG_CONTROL, newACValue ? "AUTO control turned AC ON" : "AUTO control turned AC OFF");
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// ========================
// Task 3: Hardware Task
// ========================
void hardwareTask(void *parameter) {
    RoomSetting snapshot;
    bool sensorOk;
    bool wifiOk;
    bool mqttOk;

    while (true) {
        if (xSemaphoreTake(roomMutex, portMAX_DELAY)) {
            snapshot = myRoom;
            sensorOk = sensorHealthy;
            wifiOk = wifiConnected;
            mqttOk = mqttConnected;
            xSemaphoreGive(roomMutex);
        }

        renderHardware(snapshot, sensorOk, wifiOk, mqttOk);

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// ========================
// Task 4: Web Server Task
// ========================
void webTask(void *parameter) {
    bool wifiOk;

    while (true) {
        if (xSemaphoreTake(roomMutex, portMAX_DELAY)) {
            wifiOk = wifiConnected;
            xSemaphoreGive(roomMutex);
        }

        if (wifiOk) {
            server.handleClient();
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ========================
// Task 5: Monitor Task
// ========================
void monitorTask(void *parameter) {
    while (true) {
        Serial.println("========== Task Monitor ==========");

        if (sensorTaskHandle != NULL) {
            Serial.print("[Stack] Sensor Task: ");
            Serial.println(uxTaskGetStackHighWaterMark(sensorTaskHandle));
        }

        if (controlTaskHandle != NULL) {
            Serial.print("[Stack] Control Task: ");
            Serial.println(uxTaskGetStackHighWaterMark(controlTaskHandle));
        }

        if (hardwareTaskHandle != NULL) {
            Serial.print("[Stack] Hardware Task: ");
            Serial.println(uxTaskGetStackHighWaterMark(hardwareTaskHandle));
        }

        if (webTaskHandle != NULL) {
            Serial.print("[Stack] Web Task: ");
            Serial.println(uxTaskGetStackHighWaterMark(webTaskHandle));
        }

        if (monitorTaskHandle != NULL) {
            Serial.print("[Stack] Monitor Task: ");
            Serial.println(uxTaskGetStackHighWaterMark(monitorTaskHandle));
        }

        if (wifiTaskHandle != NULL) {
            Serial.print("[Stack] WiFi Task: ");
            Serial.println(uxTaskGetStackHighWaterMark(wifiTaskHandle));
        }

        if (dataLogTaskHandle != NULL) {
            Serial.print("[Stack] Data Log Task: ");
            Serial.println(uxTaskGetStackHighWaterMark(dataLogTaskHandle));
        }

        if (mqttTaskHandle != NULL) {
            Serial.print("[Stack] MQTT Task: ");
            Serial.println(uxTaskGetStackHighWaterMark(mqttTaskHandle));
        }

        if (xSemaphoreTake(roomMutex, portMAX_DELAY)) {
            Serial.print("[Health] Sensor Healthy: ");
            Serial.println(sensorHealthy ? "YES" : "NO");

            Serial.print("[Health] Sensor Fail Count: ");
            Serial.println(sensorFailCount);

            Serial.print("[Health] Last Sensor Success: ");
            Serial.print(lastSensorSuccessTime);
            Serial.println(" ms");

            Serial.print("[WiFi] Status: ");
            Serial.println(wifiConnected ? "ONLINE" : "OFFLINE");

            if (wifiConnected) {
                Serial.print("[WiFi] IP: ");
                Serial.println(WiFi.localIP());
            }

            Serial.print("[MQTT] Status: ");
            Serial.println(mqttConnected ? "CONNECTED" : "DISCONNECTED");

            xSemaphoreGive(roomMutex);
        }

        if (xSemaphoreTake(logMutex, portMAX_DELAY)) {
            Serial.print("[Logging] Stored Logs: ");
            Serial.println(logCount);

            Serial.print("[Logging] Total Logs: ");
            Serial.println(totalLogCount);

            xSemaphoreGive(logMutex);
        }

        Serial.println("==================================");

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

// ========================
// Task 6: WiFi Task
// ========================
void wifiTask(void *parameter) {
    while (true) {
        updateWiFiStatus();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ========================
// Task 7: Data Logging Task
// Records system status every 5 seconds
// ========================
void dataLogTask(void *parameter) {
    while (true) {
        addDataLog(LOG_SENSOR, "Periodic room data log");
        vTaskDelay(pdMS_TO_TICKS(LOG_INTERVAL_MS));
    }
}

// ========================
// Task 8: MQTT Task
// Handles MQTT broker connection, command topic subscription, and periodic status publishing
// ========================
void mqttTask(void *parameter) {
    while (true) {
        updateMqttConnection();
        publishMqttPeriodically();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ========================
// Setup
// ========================
void setup() {
    Serial.begin(115200);

    Wire.begin(21, 22);
    lcd.init();
    lcd.backlight();

    dht.begin();

    pinMode(lightPin, OUTPUT);
    pinMode(acPin, OUTPUT);

    roomMutex = xSemaphoreCreateMutex();
    logMutex = xSemaphoreCreateMutex();

    if (roomMutex == NULL || logMutex == NULL) {
        Serial.println("[Error] Failed to create mutex");
        while (true);
    }

    addDataLog(LOG_SYSTEM, "System booting");

    lcd.setCursor(0, 0);
    lcd.print("Connecting WiFi");

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    unsigned long startAttemptTime = millis();

    while (WiFi.status() != WL_CONNECTED &&
           millis() - startAttemptTime < WIFI_CONNECT_TIMEOUT) {
        delay(500);
        Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
        if (xSemaphoreTake(roomMutex, portMAX_DELAY)) {
            wifiConnected = true;
            xSemaphoreGive(roomMutex);
        }

        Serial.println();
        Serial.print("[WiFi] Connected. IP: ");
        Serial.println(WiFi.localIP());

        addDataLog(LOG_SYSTEM, "Initial WiFi connected");

        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("WiFi Connected");
        lcd.setCursor(0, 1);
        lcd.print(WiFi.localIP());
    } else {
        if (xSemaphoreTake(roomMutex, portMAX_DELAY)) {
            wifiConnected = false;
            mqttConnected = false;
            xSemaphoreGive(roomMutex);
        }

        Serial.println();
        Serial.println("[WiFi] Initial connection failed. Offline mode.");

        addDataLog(LOG_ERROR, "Initial WiFi connection failed");

        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("WiFi Offline");
        lcd.setCursor(0, 1);
        lcd.print("Local mode only");
    }

    registerRoutes();
    server.begin();

    mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setBufferSize(768);

    addDataLog(LOG_SYSTEM, "REST API server started");
    addDataLog(LOG_MQTT, "MQTT client initialized");

    delay(3000);
    lcd.clear();

    // ========================
    // Create FreeRTOS Tasks
    // ========================

    xTaskCreatePinnedToCore(
        sensorTask,
        "Sensor Task",
        4096,
        NULL,
        1,
        &sensorTaskHandle,
        1
    );

    xTaskCreatePinnedToCore(
        controlTask,
        "Control Task",
        2048,
        NULL,
        2,
        &controlTaskHandle,
        1
    );

    xTaskCreatePinnedToCore(
        hardwareTask,
        "Hardware Task",
        4096,
        NULL,
        1,
        &hardwareTaskHandle,
        1
    );

    xTaskCreatePinnedToCore(
        webTask,
        "Web Server Task",
        8192,
        NULL,
        1,
        &webTaskHandle,
        0
    );

    xTaskCreatePinnedToCore(
        monitorTask,
        "Monitor Task",
        4096,
        NULL,
        1,
        &monitorTaskHandle,
        1
    );

    xTaskCreatePinnedToCore(
        wifiTask,
        "WiFi Task",
        4096,
        NULL,
        1,
        &wifiTaskHandle,
        0
    );

    xTaskCreatePinnedToCore(
        dataLogTask,
        "Data Log Task",
        4096,
        NULL,
        1,
        &dataLogTaskHandle,
        1
    );

    xTaskCreatePinnedToCore(
        mqttTask,
        "MQTT Task",
        6144,
        NULL,
        1,
        &mqttTaskHandle,
        0
    );

    addDataLog(LOG_SYSTEM, "FreeRTOS tasks started");

    Serial.println("[System] FreeRTOS tasks started");
    Serial.println("[System] REST API routes registered");
    Serial.println("[System] Data logging enabled");
    Serial.println("[System] MQTT enabled");

    Serial.println("[MQTT] Publish topics:");
    Serial.println(MQTT_TOPIC_STATUS);
    Serial.println(MQTT_TOPIC_HEALTH);
    Serial.println(MQTT_TOPIC_LOG);
    Serial.println(MQTT_TOPIC_AVAILABILITY);

    Serial.println("[MQTT] Subscribe topics:");
    Serial.println(MQTT_TOPIC_CMD_MODE);
    Serial.println(MQTT_TOPIC_CMD_LIGHT);
    Serial.println(MQTT_TOPIC_CMD_AC);
    Serial.println(MQTT_TOPIC_CMD_CONTROL);
}

// ========================
// Loop
// ========================
void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}
