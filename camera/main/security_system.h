#ifndef SECURITY_SYSTEM_H
#define SECURITY_SYSTEM_H

#include "config.h"
#include <PubSubClient.h>
#include <ArduinoJson.h>

#define MQTT_SERVER         "camera-monitor.local"
#define MQTT_PORT           1883
#define MQTT_USER           "minhtri6090"
#define MQTT_PASSWORD       "123"
#define MQTT_CLIENT_ID      "ESP32S3_SecurityCam"

#define MQTT_TOPIC_COMMAND       "security/camera/command"
#define MQTT_TOPIC_STATUS        "security/camera/status"
#define MQTT_TOPIC_ALERT         "security/camera/alert"
#define MQTT_TOPIC_FAMILY_DETECT "security/camera/family_detected"
#define MQTT_TOPIC_CONFIRMATION  "security/camera/confirmation"

#define PHONE_NUMBER_OWNER    "0976168240"
#define PHONE_NUMBER_NEIGHBOR "0976168240"

#define OWNER_SMS_BUZZER_DELAY   20000
#define NEIGHBOR_SMS_LOCK_DELAY  40000
#define AUTO_RESET_NO_MOTION     5000

enum SecurityState {
    SECURITY_IDLE = 0,
    SECURITY_MOTION_DETECTED,
    SECURITY_WAITING_OWNER_SMS,
    SECURITY_WAITING_NEIGHBOR_SMS,
    SECURITY_ALARM_ACTIVE
};

struct SMSData {
    char phoneNumber[16];
    char message[160];
};

extern SecurityState currentSecurityState;
extern unsigned long motionDetectedTime;
extern unsigned long lastMotionSeenTime;
extern bool ownerSmsAlreadySent;
extern bool neighborSmsAlreadySent;
extern bool familyMemberDetected;

extern bool mqttConnected;
extern PubSubClient mqttClient;
extern HardwareSerial simSerial;

void initSecuritySystem();
void initSIM();
void initMQTT();

void connectMQTT();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void publishMQTTStatus(const char* message);
void sendNodeCommand(const char* device, const char* action);

void handleSecuritySystem();
void onMotionDetected();
void updateMotionTimestamp();
void onMotionEnded();  
void onFamilyMemberDetected();
void resetSecurityState();
void checkSecurityTimers();

bool sendCommand(const char* command, const char* expectedResponse, unsigned long timeout);
bool sendSMS(const char* phoneNumber, const char* message);

void sendSMSTask(void* parameter);
void sendSMSAsync(const char* phone, const char* msg);

#endif