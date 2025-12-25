#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

const char* WIFI_SSID = "Toof";
const char* WIFI_PASSWORD = "123456789";

const char* MQTT_SERVER = "camera-monitor.local";
const int MQTT_PORT = 1883;
const char* MQTT_USER = "minhtri6090";
const char* MQTT_PASSWORD = "123";
const char* MQTT_CLIENT_ID = "ESP32_SecurityNode";

#define TOPIC_BUZZER            "security/node/buzzer"
#define TOPIC_LOCK              "security/node/lock"
#define TOPIC_CONFIRMATION      "security/camera/confirmation"

#define BUZZER_PIN              14
#define LOCK_PIN                12

//TIMING
const unsigned long AUTO_LOCK_DELAY = 30000; 

WiFiClient espClient;
PubSubClient mqtt(espClient);

unsigned long unlockTime = 0;
bool autoLockScheduled = false;

void connectWiFi();
void connectMQTT();
void onMessage(char* topic, byte* payload, unsigned int length);
void controlBuzzer(bool turnOn);
void controlLock(bool lock);
void sendConfirmation(const char* device, const char* action, bool success);
void checkAutoLock(); 

void setup() 
{
    Serial.begin(115200);
    delay(1000);
    
    pinMode(BUZZER_PIN, OUTPUT);
    pinMode(LOCK_PIN, OUTPUT);

    
    digitalWrite(BUZZER_PIN, LOW);
    digitalWrite(LOCK_PIN, LOW);

    
    Serial.println("\n ESP32 Security Node ");
    
    connectWiFi();
    
    mqtt.setServer(MQTT_SERVER, MQTT_PORT);
    mqtt.setCallback(onMessage);
    mqtt.setBufferSize(512);
    
    connectMQTT();
    
    Serial.println("=== READY ===\n");
}

void loop() 
{
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("[WIFI] Reconnecting...");
        connectWiFi();
    }
    
    if (!mqtt.connected()) 
        connectMQTT();

    mqtt.loop();

    checkAutoLock();
    
    delay(10);
}

void connectWiFi() 
{
    Serial.print("[WIFI] Connecting to: ");
    Serial.println(WIFI_SSID);
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    int attempts = 0;
    while ((WiFi.status() != WL_CONNECTED) && (attempts < 3)) 
    {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) 
    {
        Serial.println("\n[WIFI] Connected");
        Serial.print("[WIFI] IP: ");
        Serial.println(WiFi.localIP());
    } 
    else
        Serial.println("\n[WIFI] Failed");

}

void connectMQTT() 
{
    if (WiFi.status() != WL_CONNECTED) return;
    
    Serial.print("[MQTT] Connecting...");
    
    if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)) 
    {
        Serial.println(" OK");
        
        mqtt.subscribe(TOPIC_BUZZER);
        mqtt.subscribe(TOPIC_LOCK);
        
    } 
    else 
    {
        Serial.print(" FAILED CONNECT TO MQTT: ");
        Serial.print(mqtt.state());
        delay(5000);
    }
}

// =================== NHẬN MESSAGE ===================
void onMessage(char* topic, byte* payload, unsigned int length) 
{
    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, payload, length);
    
    if (error) 
    {
        Serial.print("[MQTT] JSON error: ");
        Serial.println(error.c_str());
        return;
    }
    
    const char* action = doc["action"];
    if (!action) 
    {
        Serial.println("[MQTT] Missing 'action'");
        return;
    }
    
    Serial.printf("\n[MQTT] Topic: %s | Action: %s\n", topic, action);
    
    bool success = false;

    if (strcmp(topic, TOPIC_BUZZER) == 0) 
    {
        if (strcmp(action, "on") == 0) 
        {
            controlBuzzer(true);
            success = true;
        } 
        else if (strcmp(action, "off") == 0)
        {
            controlBuzzer(false);
            success = true;
        }
        sendConfirmation("buzzer", action, success);
    }
    else if (strcmp(topic, TOPIC_LOCK) == 0) 
    {
        if (strcmp(action, "lock") == 0) 
        {
            controlLock(true);
            success = true;
        } 
        else if (strcmp(action, "unlock") == 0) 
        {
            controlLock(false);
            success = true;
        }
        sendConfirmation("lock", action, success);
    }
}

void sendConfirmation(const char* device, const char* action, bool success) 
{
    if (!mqtt.connected()) return;
    
    StaticJsonDocument<256> doc;
    doc["device"] = device;
    doc["action"] = action;
    doc["success"] = success;
    doc["timestamp"] = millis();
    doc["node"] = MQTT_CLIENT_ID;
    
    char buffer[300];
    serializeJson(doc, buffer);
    
    bool sent = mqtt.publish(TOPIC_CONFIRMATION, buffer);
    Serial.printf("[MQTT] Confirmation: %s\n", sent ? "Sent" : "Failed");
}

void controlBuzzer(bool turnOn) 
{
    digitalWrite(BUZZER_PIN, turnOn ? HIGH : LOW);
    Serial.printf("[BUZZER] %s\n", turnOn ? "ON" : "OFF");
}

void controlLock(bool lock) 
{
    digitalWrite(LOCK_PIN, lock ? LOW : HIGH);
    
    Serial.printf("[LOCK] %s\n", lock ? "LOCKED" : "UNLOCKED");

    if (!lock) 
    {
        unlockTime = millis();
        autoLockScheduled = true;
        Serial.printf("[AUTO-LOCK] Scheduled in 30 seconds (at %lu ms)\n", unlockTime + AUTO_LOCK_DELAY);
    }
    else 
    {
        autoLockScheduled = false;
        Serial.println("[AUTO-LOCK] Cancelled");
    }
}

void checkAutoLock() 
{
    if (!autoLockScheduled) return;
    
    unsigned long now = millis();
    unsigned long elapsed = now - unlockTime;

    if (elapsed >= AUTO_LOCK_DELAY) 
    {
        Serial.println("\n[AUTO-LOCK] 30s passed, locking door...");
        
        controlLock(true);
        autoLockScheduled = false;

        sendConfirmation("lock", "auto-lock", true);
    }
    else
    {
        static unsigned long lastLog = 0;
        if (now - lastLog > 5000) 
        {
            lastLog = now;
            unsigned long remaining = (AUTO_LOCK_DELAY - elapsed) / 1000;
            Serial.printf("[AUTO-LOCK] %lu seconds remaining...\n", remaining);
        }
    }
}