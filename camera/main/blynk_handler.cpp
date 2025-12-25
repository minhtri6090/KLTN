#define BLYNK_TEMPLATE_ID "TMPL6Ulz28slZ"
#define BLYNK_TEMPLATE_NAME "ESP32S3"
#define BLYNK_AUTH_TOKEN "Vqm7rUR3VZoz_tZBlEXJ8w2cbQ4YnDUt"

#define BLYNK_PRINT Serial

#include "blynk_handler.h"
#include "wifi_manager.h"
#include "security_system.h"
#include <BlynkSimpleEsp32.h>

Servo servo1, servo2;

int servo1Angle = 90;
int servo2Angle = 134;

volatile bool holdServo1Left = false;
volatile bool holdServo1Right = false;
volatile bool holdServo2Down = false;
volatile bool holdServo2Up = false;

const int MIN_ANGLE = 0;
const int MAX_ANGLE = 180;
const int SERVO_STEP = 4;
const int SERVO_SPEED_MS = 60;

BlynkTimer servoTimer;

static bool blynkInitialized = false;
static unsigned long lastBlynkReconnectAttempt = 0;
static const unsigned long BLYNK_RECONNECT_INTERVAL = 30000;

void initializeBlynk() {
    if(savedSSID.length() > 0 && savedPassword.length() > 0) {
        Serial.println("[BLYNK] Initializing...");
        
        Blynk.config(BLYNK_AUTH_TOKEN, "blynk.cloud", 80);//
        
        servoTimer.setInterval(SERVO_SPEED_MS, updateServoPositions);
        blynkInitialized = true;
        
        reconnectBlynk();
    }
}

void initializeServos() {
    servo1.setPeriodHertz(50); 
    servo2.setPeriodHertz(50);

    servo1.attach(SERVO1_PIN);
    servo2.attach(SERVO2_PIN);

    servo1.write(servo1Angle);
    servo2.write(servo2Angle);
    
    vTaskDelay(pdMS_TO_TICKS(500));
}

void updateServoPositions() {
    bool moved = false;

    if (holdServo1Left && servo1Angle > MIN_ANGLE) {
        servo1Angle = max(MIN_ANGLE, servo1Angle - SERVO_STEP);
        moved = true;
    }
    if (holdServo1Right && servo1Angle < MAX_ANGLE) {
        servo1Angle = min(MAX_ANGLE, servo1Angle + SERVO_STEP);
        moved = true;
    }

    if (holdServo2Down && servo2Angle > MIN_ANGLE) {
        servo2Angle = max(MIN_ANGLE, servo2Angle - SERVO_STEP);
        moved = true;
    }
    if (holdServo2Up && servo2Angle < MAX_ANGLE) {
        servo2Angle = min(MAX_ANGLE, servo2Angle + SERVO_STEP);
        moved = true;
    }

    if (moved) {
        servo1.write(servo1Angle);
        servo2.write(servo2Angle);
    }
}

void moveServoToCenter() {
    servo1Angle = 90;
    servo2Angle = 134;
    
    servo1.write(servo1Angle);
    servo2.write(servo2Angle);
}

void handleServoLoop() {
    if (Blynk.connected()) {
        servoTimer.run();
    }
}

void reconnectBlynk() 
{
    if (!blynkInitialized) 
    {
        return;
    }
    
    if (wifiState != WIFI_STA_OK || WiFi.status() != WL_CONNECTED) 
    {
        return;
    }
    
    unsigned long now = millis();
    
    if (now - lastBlynkReconnectAttempt < BLYNK_RECONNECT_INTERVAL) 
    {
        return;
    }
    
    if (!Blynk.connected()) 
    {
        Serial.println("[BLYNK] Attempting reconnect...");
        lastBlynkReconnectAttempt = now;
        
        if (Blynk.connect(5000)) 
        {
            Serial.println("[BLYNK] Reconnected");
            Blynk.syncAll();
        } 
        else 
        {
            Serial.println("[BLYNK] Reconnect failed");
        }
    }
}

bool isBlynkConnected() {
    return blynkInitialized && Blynk.connected();
}

void handleBlynkLoop() 
{
    if(wifiState == WIFI_STA_OK && WiFi.status() == WL_CONNECTED) 
    {
        if (!Blynk.connected()) 
        {
            reconnectBlynk();
        }
        
        Blynk.run();
        handleServoLoop();
    }
}

BLYNK_CONNECTED() {
    Serial.println("[BLYNK] Connected to Blynk Cloud");
    Blynk.syncAll();
}

BLYNK_DISCONNECTED() {
    Serial.println("[BLYNK] Disconnected from Blynk Cloud");
}

BLYNK_WRITE(V_SERVO1_LEFT) {
    holdServo1Left = (param.asInt() == 1);
}

BLYNK_WRITE(V_SERVO1_RIGHT) {
    holdServo1Right = (param.asInt() == 1);
}

BLYNK_WRITE(V_SERVO2_DOWN) {
    holdServo2Down = (param.asInt() == 1);
}

BLYNK_WRITE(V_SERVO2_UP) {
    holdServo2Up = (param.asInt() == 1);
}

BLYNK_WRITE(V_SERVO_CENTER) {
    if (param.asInt() == 1) { 
        moveServoToCenter();
        Serial.println("[BLYNK] Center executed");
    }
}

BLYNK_WRITE(V_EMERGENCY_UNLOCK) {
    if (param.asInt() == 1) {
        Serial.println("[BLYNK] Emergency unlock pressed");
        handleEmergencyUnlock();
    }
}

void handleEmergencyUnlock() {
    Serial.println("[EMERGENCY] Executing unlock sequence");
    
    extern bool isAudioPlaying();
    extern void stopAudio();
    
    if (isAudioPlaying()) {
        stopAudio();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    sendNodeCommand("buzzer", "off");
    vTaskDelay(pdMS_TO_TICKS(100));
    
    sendNodeCommand("lock", "unlock");
    vTaskDelay(pdMS_TO_TICKS(100));
    
    if (currentSecurityState != SECURITY_IDLE) {
        resetSecurityState();
    }
    
    Serial.println("[EMERGENCY] Unlock completed");
    
    if (Blynk.connected()) {
        Blynk.logEvent("emergency_unlock", "Door unlocked. Auto-lock in 30s");
    }
}