#include "config.h"
#include "wifi_manager.h"
#include "camera_handler.h"
#include "web_server.h"
#include "blynk_handler.h"
#include "audio_handler.h"
#include "sensors_handler.h"
#include "security_system.h"

bool sdAudioInitialized = false;
bool welcomeAudioPlayed = false;
bool wifiConnectionStarted = false;
bool wifiResultProcessed = false;
bool pirInitialized = false;
bool servoInitialized = false;
bool securitySystemInitialized = false;

bool needPlaySuccessAudio = false;
unsigned long wifiStartTime = 0;
const unsigned long WIFI_TIMEOUT = 30000;

void setup() 
{
    Serial.begin(115200);
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    if (!psramFound()) 
    {
        Serial.println("ERROR: PSRAM NOT FOUND!");
        while(1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    EEPROM.begin(512);
    loadCredentials();
    initializeBuffers();
    initializeCamera();
}

void loop() 
{
     //STATE 1 – Audio Init
    if (!sdAudioInitialized) 
    {
        initializeSDCard();
        initializeAudio();
        vTaskDelay(pdMS_TO_TICKS(200));
        playAudio(AUDIO_HELLO);
        
                sdAudioInitialized = true;
        welcomeAudioPlayed = true;
        return;
    }
    handleAudioLoop();

    //STATE 2 – WiFi Start
    if (welcomeAudioPlayed && !wifiConnectionStarted && !isAudioPlaying()) 
    {
        initializeWiFi();
        wifiConnectionStarted = true;
        wifiStartTime = millis();
        return;
    }

    //STATE 3 – WiFi Processing
    if (wifiConnectionStarted && !wifiResultProcessed) {
        handleWiFiLoop();

        if (wifiState == WIFI_STA_OK && !isAudioPlaying()) {
            playAudio(AUDIO_WIFI_SUCCESS);
            vTaskDelay(pdMS_TO_TICKS(100));
            while (isAudioPlaying()) { 
                handleAudioLoop(); 
                yield(); 
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            
            wifiResultProcessed = true;

            if (!servoInitialized) {
                initializeServos();
                servoInitialized = true;
            }
            return;
        }

        if ((millis() - wifiStartTime > WIFI_TIMEOUT) && wifiState != WIFI_STA_OK && !isAudioPlaying()) {
            playAudio(AUDIO_WIFI_FAILED);
            vTaskDelay(pdMS_TO_TICKS(100));
            while (isAudioPlaying()) { 
                handleAudioLoop(); 
                yield(); 
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            
            wifiResultProcessed = true;
            return;
        }
    }

    //STATE 4 – Sensors Init
    if (wifiResultProcessed && !pirInitialized) 
    {
        if (wifiState == WIFI_STA_OK) 
        {
            initializeSensors();
            systemReady = true; 
        }
        
        pirInitialized = true;
        return;
    }
    
    //STATE 5 – Security Init
    if (pirInitialized && !securitySystemInitialized) {
        initSecuritySystem();
        securitySystemInitialized = true;
        return;
    }

    //STATE 6 – Reconnect edge case
    if (needPlaySuccessAudio && wifiState == WIFI_STA_OK && !isAudioPlaying()) 
    {
        playAudio(AUDIO_WIFI_SUCCESS);
        vTaskDelay(pdMS_TO_TICKS(100));
        while (isAudioPlaying()) 
        { 
            handleAudioLoop(); 
            yield(); 
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        needPlaySuccessAudio = false;

        if (!servoInitialized) 
        {
            initializeServos();
            servoInitialized = true;
        }

        if (!systemReady) 
        {
            initializeSensors();
            systemReady = true;
        }
        return;
    }

    //MAIN LOOP
    if (pirInitialized || systemReady) 
    { 
        handleWebServerLoop();
        handleWiFiLoop();
        
        if (systemReady && wifiState == WIFI_STA_OK) 
        {
            handleMotionLoop();
            handleLDRLoop();
            handleBlynkLoop();
            
            if (securitySystemInitialized) {
                handleSecuritySystem();
            }
        }
    }
    
    vTaskDelay(pdMS_TO_TICKS(10));
}