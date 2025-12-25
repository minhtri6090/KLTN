#include "security_system.h"
#include "wifi_manager.h"
#include "audio_handler.h"
#include "sensors_handler.h"

SecurityState currentSecurityState = SECURITY_IDLE;
unsigned long motionDetectedTime = 0;
unsigned long lastMotionSeenTime = 0;
bool ownerSmsAlreadySent = false;
bool neighborSmsAlreadySent = false; 
bool familyMemberDetected = false;

bool mqttConnected = false;
HardwareSerial simSerial(1);
WiFiClient espClient;
PubSubClient mqttClient(espClient);

static TaskHandle_t smsTaskHandle = NULL;

void initSecuritySystem() {
    resetSecurityState();

    initSIM();
    vTaskDelay(pdMS_TO_TICKS(500));

    initMQTT();
    vTaskDelay(pdMS_TO_TICKS(500));

}

void initSIM() 
{
    simSerial.begin(115200, SERIAL_8N1, SIM_RX_PIN, SIM_TX_PIN);
    
    pinMode(SIM_POWER_PIN, OUTPUT);
    digitalWrite(SIM_POWER_PIN, HIGH);
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    while(simSerial.available()) simSerial.read();//Clear Receive Buffer
    
    if (sendCommand("AT", "OK", 2000)) 
    {
        sendCommand("AT+CPIN?", "+CPIN: READY", 2000);
        sendCommand("AT+CMGF=1", "OK", 2000);
        sendCommand("AT+CSCS=\"GSM\"", "OK", 2000);
        Serial.println("[SIM] Initialized");
    } 
    else 
    {
        Serial.println("[SIM] Init failed");
    }
}

bool sendCommand(const char* command, const char* expectedResponse, unsigned long timeout) {
    simSerial.println(command);
    
    unsigned long startTime = millis();
    String response = "";

    if (simSerial.available()) {
        char c = simSerial.read();
        response += c;
        
        if (response.indexOf(expectedResponse) >= 0) {
            return true;
        }
    }
    
    return false;
}

bool sendSMS(const char* phoneNumber, const char* message) {
    Serial.printf("[SMS] Sending to %s\n", phoneNumber);
    
    simSerial.print("AT+CMGS=\"");
    simSerial.print(phoneNumber);
    simSerial.println("\"");
    
    vTaskDelay(pdMS_TO_TICKS(500));
    
    unsigned long startTime = millis();
    bool gotPrompt = false;
    
    while (millis() - startTime < 5000 && !gotPrompt) {
        if (simSerial.available()) {
            char c = simSerial.read();
            if (c == '>') {
                gotPrompt = true;
                break;
            }
        }
    }
    
    if (!gotPrompt) {
        Serial.println("[SMS] No prompt");
        return false;
    }
    
    simSerial.print(message);
    vTaskDelay(pdMS_TO_TICKS(500));
    simSerial.write(26);
    
    startTime = millis();
    String response = "";
    bool success = false;
    
    while (millis() - startTime < 20000) {
        if (simSerial.available()) {
            char c = simSerial.read();
            response += c;
            
            if (response.indexOf("+CMGS:") >= 0 && response.indexOf("OK") >= 0) {
                success = true;
                break;
            }
            
            if (response.indexOf("ERROR") >= 0) {
                break;
            }
        }
    }
    
    Serial.printf("[SMS] %s\n", success ? "OK" : "FAILED");
    return success;
}

void sendSMSTask(void* parameter) {
    SMSData* data = (SMSData*)parameter;
    
    Serial.printf("[SMS_TASK] Started on Core %d\n", xPortGetCoreID());
    unsigned long startTime = millis();
    
    bool result = sendSMS(data->phoneNumber, data->message);
    
    Serial.printf("[SMS_TASK] Completed in %lu ms (Result: %s)\n", 
                  millis() - startTime, result ? "SUCCESS" : "FAILED");
    
    delete data;
    smsTaskHandle = NULL;
    vTaskDelete(NULL);
}

void sendSMSAsync(const char* phone, const char* msg) {
    if (smsTaskHandle != NULL) {
        Serial.println("[SMS] Warning: Previous SMS task still running, skipping");
        return;
    }
    
    SMSData* data = new SMSData;
    strncpy(data->phoneNumber, phone, sizeof(data->phoneNumber) - 1);
    strncpy(data->message, msg, sizeof(data->message) - 1);
    data->phoneNumber[sizeof(data->phoneNumber) - 1] = '\0';
    data->message[sizeof(data->message) - 1] = '\0';
    
    BaseType_t result = xTaskCreatePinnedToCore(
        sendSMSTask,
        "SMSTask",
        4096,
        data,
        1,
        &smsTaskHandle,
        0
    );
    
    if (result != pdPASS) {
        Serial.println("[SMS] Failed to create SMS task");
        delete data;
    } else {
        Serial.println("[SMS] SMS task created (async)");
    }
}

void initMQTT() 
{
    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
    mqttClient.setCallback(mqttCallback);
    connectMQTT();
}

void connectMQTT() 
{
    if (mqttConnected || wifiState != WIFI_STA_OK) return;
    
    Serial.print("[MQTT] Connecting...");
    
    if (mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)) 
    {
        mqttConnected = true;
        
        mqttClient.subscribe(MQTT_TOPIC_COMMAND);
        mqttClient.subscribe(MQTT_TOPIC_FAMILY_DETECT);
        
        publishMQTTStatus("ESP32S3 online");
        Serial.println("MQTT OK");
    } 
    else 
    {
        mqttConnected = false;
        Serial.printf(" FAIL RC=%d\n", mqttClient.state());
    }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) 
{
    if (length == 0) return;
    
    char message[length + 1];
    for (unsigned int i = 0; i < length; i++) 
    {
        message[i] = (char)payload[i];
    }
    message[length] = '\0';
    
    Serial.printf("\n[MQTT] <- %s: %s\n", topic, message);
    
    if (strcmp(topic, MQTT_TOPIC_FAMILY_DETECT) == 0) {
        StaticJsonDocument<300> doc;
        DeserializationError error = deserializeJson(doc, message);
        
        if (!error) {
            const char* user_name = doc["user_name"] | "Unknown";
            float confidence = doc["confidence"] | 0.0;
            
            Serial.printf("[SECURITY] Family: %s (%.2f)\n", user_name, confidence);
            onFamilyMemberDetected();
        }
    }
}

void publishMQTTStatus(const char* message) {
    if (!mqttConnected) return;
    
    StaticJsonDocument<256> doc;
    doc["device"] = MQTT_CLIENT_ID;
    doc["status"] = message;
    doc["timestamp"] = millis();
    doc["security_state"] = currentSecurityState;
    
    char buffer[384];
    serializeJson(doc, buffer);
    
    mqttClient.publish(MQTT_TOPIC_STATUS, buffer);
}

void sendNodeCommand(const char* device, const char* action) 
{
    if (!mqttConnected) 
    {
        Serial.println("[MQTT] Not connected");
        return;
    }
    
    StaticJsonDocument<128> doc;
    doc["action"] = action;
    doc["timestamp"] = millis();
    
    char buffer[192];
    serializeJson(doc, buffer);
    
    String topic = "security/node/";
    topic += device;
    
    bool ok = mqttClient.publish(topic.c_str(), buffer, false);
    
    Serial.printf("[MQTT] -> %s: %s (%s)\n", device, action, ok ? "OK" : "FAIL");
}

void handleSecuritySystem() 
{
    static unsigned long lastMQTTReconnectAttempt = 0;
    const unsigned long MQTT_RECONNECT_INTERVAL = 10000;
    
    if (!mqttConnected && wifiState == WIFI_STA_OK) 
    {
        unsigned long now = millis();
        
        if (now - lastMQTTReconnectAttempt > MQTT_RECONNECT_INTERVAL) 
        {
            lastMQTTReconnectAttempt = now;
            Serial.println("[MQTT] Attempting reconnect...");
            connectMQTT();
        }
    }
    
    if (mqttConnected) 
    {
        if (!mqttClient.connected()) 
        {
            Serial.println("[MQTT] Lost connection");
            mqttConnected = false;
        } 
        else 
        {
            mqttClient.loop();
        }
    }
    
    checkSecurityTimers();
}

void onMotionDetected() {
    lastMotionSeenTime = millis();
    
    if (currentSecurityState == SECURITY_IDLE) 
    {
        Serial.println("\n[SECURITY] Motion detected - Starting countdown");
        
        if (isAudioPlaying()) {
            stopAudio();
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        
        playAudio(AUDIO_MOTION_DETECTED);
        
        currentSecurityState = SECURITY_WAITING_OWNER_SMS;
        motionDetectedTime = millis();
        ownerSmsAlreadySent = false;
        neighborSmsAlreadySent = false;
        familyMemberDetected = false;
        
        if (mqttConnected) 
        {
            StaticJsonDocument<256> doc;
            doc["event"] = "motion_detected";
            doc["timestamp"] = millis();
            doc["security_state"] = currentSecurityState;
            
            char buffer[300];
            serializeJson(doc, buffer);
            
            mqttClient.publish(MQTT_TOPIC_ALERT, buffer, true);
        }
        
        publishMQTTStatus("Motion detected");
    }
}

void updateMotionTimestamp() 
{
    lastMotionSeenTime = millis();
}

//Xử lý khi motion kết thúc
void onMotionEnded() {
    Serial.println("[SECURITY] Motion ended - Turning OFF buzzer");
    
    //Tắt buzzer ngay lập tức
    sendNodeCommand("buzzer", "off");
    
    //Nếu đang trong countdown -> Reset về IDLE
    if (currentSecurityState != SECURITY_IDLE && currentSecurityState != SECURITY_ALARM_ACTIVE) {
        Serial.println("[SECURITY] Resetting to IDLE (motion stopped before alarm)");
        resetSecurityState();
    }
    // Nếu đã ở ALARM_ACTIVE (sau 40s) → Giữ trạng thái lock
    else if (currentSecurityState == SECURITY_ALARM_ACTIVE) {
        Serial.println("[SECURITY] Alarm active - Door remains LOCKED");
        publishMQTTStatus("Motion ended - Door locked");
    }
}

void onFamilyMemberDetected() {
    Serial.println("\n[SECURITY] Family member detected - Disarming");
    
    familyMemberDetected = true;
    
    if (isAudioPlaying()) {
        stopAudio();
    }
    
    sendNodeCommand("buzzer", "off");
    sendNodeCommand("lock", "unlock");
    
    resetSecurityState();
    
    publishMQTTStatus("Family confirmed - system disarmed");
}

void resetSecurityState() {
    Serial.println("[SECURITY] Reset to IDLE");
    
    currentSecurityState = SECURITY_IDLE;
    motionDetectedTime = 0;
    lastMotionSeenTime = 0;
    ownerSmsAlreadySent = false;
    neighborSmsAlreadySent = false;
    familyMemberDetected = false;
    
    publishMQTTStatus("IDLE");
}

void checkSecurityTimers() 
{
    if (currentSecurityState == SECURITY_IDLE || familyMemberDetected) 
    {
        return;
    }
    
    unsigned long currentTime = millis();
    unsigned long elapsedTime = currentTime - motionDetectedTime;
    unsigned long timeSinceLastMotion = currentTime - lastMotionSeenTime;
    
    // ✅ Auto reset nếu không motion > 20s (chỉ còn backup, vì đã tắt ở onMotionEnded)
    if (timeSinceLastMotion >= AUTO_RESET_NO_MOTION) {
        Serial.println("\n[SECURITY] No motion > 20s - Auto reset");
        
        sendNodeCommand("buzzer", "off");
        resetSecurityState();
        return;
    }
    
    switch (currentSecurityState) 
    {
        case SECURITY_WAITING_OWNER_SMS:
            if (elapsedTime >= OWNER_SMS_BUZZER_DELAY && !ownerSmsAlreadySent) 
            {
                Serial.println("\n[SECURITY] 20s - Owner SMS + Buzzer ON");
                
                sendSMSAsync(PHONE_NUMBER_OWNER, "CANH BAO: Phat hien chuyen dong tai nha ban!");
                ownerSmsAlreadySent = true;
                
                sendNodeCommand("buzzer", "on");
                
                currentSecurityState = SECURITY_WAITING_NEIGHBOR_SMS;
                publishMQTTStatus("Owner SMS sent");
            }
            break;
            
        case SECURITY_WAITING_NEIGHBOR_SMS:
            if (elapsedTime >= NEIGHBOR_SMS_LOCK_DELAY && !neighborSmsAlreadySent) {
                Serial.println("\n[SECURITY] 40s - Neighbor SMS + Door LOCK");
                
                sendSMSAsync(PHONE_NUMBER_NEIGHBOR, "CANH BAO KHAN CAP: Co the co ke dot nhap tai nha hang xong! Vui long kiem tra giup");
                neighborSmsAlreadySent = true;
                
                sendNodeCommand("lock", "lock");
                
                currentSecurityState = SECURITY_ALARM_ACTIVE;
                publishMQTTStatus("Neighbor SMS sent");
            }
            break;
                    
        default:
            break;
    }
}