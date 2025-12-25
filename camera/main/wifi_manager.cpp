#include "wifi_manager.h"
#include "web_server.h"
#include "camera_handler.h"
#include "blynk_handler.h"
#include "audio_handler.h"

extern WebServer server;
extern bool serverRunning;

const char* AP_SSID = "Camera Monitor";
const char* AP_PASSWORD = "12345678";
const IPAddress AP_IP(192, 168, 4, 1);
const int AP_CHANNEL = 1;
const int AP_MAX_CONN = 4;
const bool AP_HIDDEN = false;

WiFiState wifiState = WIFI_AP_MODE;
bool connecting = false;
String connectingSSID = "";
String connectingPassword = "";
unsigned long connectStartTime = 0;
const unsigned long connectTimeout = 30000;

String savedSSID = "";
String savedPassword = "";

static int connectionAttempts = 0;
static const int maxConnectionAttempts = 2;
static unsigned long lastLogTime = 0;

static bool mdnsInitialized = false;

extern bool needPlaySuccessAudio;

void loadCredentials() 
{
    savedSSID = readEEPROM(0, 32);
    savedPassword = readEEPROM(32, 64);
    
    //
    if (savedSSID.length() == 0 || savedSSID.length() > 32 || savedPassword.length() > 64) 
    {
        savedSSID = "";
        savedPassword = "";
        for (int i = 0; i < 96; i++) 
        {
            EEPROM.write(i, 0);
        }
        EEPROM.commit();
    }
    
    //
    for (int i = 0; i < savedSSID.length(); i++) 
    {
        if (savedSSID[i] < 32 || savedSSID[i] > 126) 
        {
            savedSSID = "";
            savedPassword = "";
            break;
        }
    }
}

void saveCredentials(String ssid, String password) 
{
    if (ssid.length() > 32 || password.length() > 64) 
        return;

    writeEEPROM(0, 32, ssid);
    writeEEPROM(32, 64, password);
    EEPROM.commit();
    
    savedSSID = ssid;
    savedPassword = password;
}

String readEEPROM(int offset, int maxLen) 
{
    String value = "";
    for (int i = 0; i < maxLen; ++i) 
    {
        char c = EEPROM.read(offset + i);
        if (c == 0) break;
        if (c >= 32 && c <= 126) 
        {
            value += c;
        } 
        else
            break;

    }
    return value;
}

void writeEEPROM(int offset, int maxLen, String value) 
{
    for (int i = 0; i < maxLen; ++i) 
    {
        if (i < value.length()) 
        {
            EEPROM.write(offset + i, value[i]);
        } 
        else 
        {
            EEPROM.write(offset + i, 0);
        }
    }
}

void initializeWiFi() 
{
    if (savedSSID.length() == 0) 
    {
        startAPConfigPortal();
        return;
    }
    
    connectWiFiSTA(savedSSID, savedPassword);
}

void connectWiFiSTA(String ssid, String password) 
{
    if (millis() - lastLogTime > 5000) 
    {
        Serial.printf("[WIFI] Connecting to: %s\n", ssid.c_str());
        lastLogTime = millis();
    }
    
    connectionAttempts++;

    if (wifiState == WIFI_AP_MODE) 
    {
        if (serverRunning) 
        {
            stopMJPEGStreamingServer();
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        WiFi.softAPdisconnect(true);// Disconnect to switch to STA mode
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    WiFi.mode(WIFI_STA);
    vTaskDelay(pdMS_TO_TICKS(500));
    
    WiFi.setAutoReconnect(true); //Automatically reconnect if network drops
    WiFi.persistent(true); //Save WiFi information to Flash memory (NVS)
    WiFi.setSleep(false); //turn off power saving mode
    
    WiFi.disconnect(true);
    vTaskDelay(pdMS_TO_TICKS(500));
    
    WiFi.begin(ssid.c_str(), password.c_str());
    
    connecting = true;
    connectStartTime = millis();
    connectingSSID = ssid;
    connectingPassword = password;
}

void handleSuccessfulConnection() 
{
    Serial.println("[WIFI] Connected");
    Serial.printf("[WIFI] IP: %s\n", WiFi.localIP().toString().c_str());
    
    connecting = false;
    connectionAttempts = 0;
    wifiState = WIFI_STA_OK;

    WiFi.mode(WIFI_STA);
    vTaskDelay(pdMS_TO_TICKS(1000));

    initializeMDNS();

    startMJPEGStreamingServer();
    startStream();
    
    extern bool servoInitialized;
    if (!servoInitialized) 
    {
        initializeBlynk();
    } 
    else 
    {
        reconnectBlynk();
    }

    extern bool wifiConnectionStarted;
    extern bool wifiResultProcessed;

    if (wifiConnectionStarted && wifiResultProcessed) {
        needPlaySuccessAudio = true;
    }
}

void handleFailedConnection() {
    Serial.println("[WIFI] Connection failed");
    connecting = false;
    
    if (connectionAttempts < maxConnectionAttempts) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        connectWiFiSTA(connectingSSID, connectingPassword);
        return;
    }
    
    Serial.println("[WIFI] Starting AP mode");
    connectionAttempts = 0;
    wifiState = WIFI_AP_MODE;
    
    startAPConfigPortal();
}

void startAPConfigPortal() 
{
    if (serverRunning) 
    {
        stopMJPEGStreamingServer();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    
    stopStream();

    if (mdnsInitialized) 
    {
        MDNS.end();
        mdnsInitialized = false;
        Serial.println("[mDNS] Stopped (AP mode)");
    }

    WiFi.disconnect(true);
    vTaskDelay(pdMS_TO_TICKS(1000));

    WiFi.mode(WIFI_AP);
    vTaskDelay(pdMS_TO_TICKS(1000));

    WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, AP_HIDDEN, AP_MAX_CONN);
    
    vTaskDelay(pdMS_TO_TICKS(1000));

    IPAddress apIP = WiFi.softAPIP();
    if (apIP == IPAddress(0, 0, 0, 0)) 
    {
        vTaskDelay(pdMS_TO_TICKS(3000));
        ESP.restart();
    }

    WiFi.setSleep(false);
    startAPWebServer();
    wifiState = WIFI_AP_MODE;
    
    Serial.printf("[AP] IP: %s\n", apIP.toString().c_str());
}



void handleWiFiLoop() 
{
    static String lastProcessedSSID = "";
    if (connecting && connectingSSID.length() > 0 && connectingSSID != lastProcessedSSID)
    {
        lastProcessedSSID = connectingSSID; //avoid continuous loops
        connectWiFiSTA(connectingSSID, connectingPassword);
        return;
    }

    if (connecting) 
    {
        unsigned long elapsed = millis() - connectStartTime;
        wl_status_t status = WiFi.status();
        
        if (status == WL_CONNECTED) 
        {
            handleSuccessfulConnection();
            lastProcessedSSID = "";
            return;
        }
        
        if (status == WL_NO_SSID_AVAIL || status == WL_CONNECT_FAILED || elapsed > connectTimeout) 
        {
            handleFailedConnection();
            lastProcessedSSID = "";
            return;
        }
    }

    if (wifiState == WIFI_STA_OK) 
    {
        static unsigned long lastCheck = 0;
        if (millis() - lastCheck > 30000) 
        {
            lastCheck = millis();
            
            if (WiFi.status() != WL_CONNECTED) 
            {
                Serial.println("[WIFI] Connection lost");
                
                if (mdnsInitialized) 
                {
                    MDNS.end();
                    mdnsInitialized = false;
                    Serial.println("[mDNS] Stopped (connection lost)");
                }
                
                wifiState = WIFI_AP_MODE;
                startAPConfigPortal();
            }
        }
    }
}

void initializeMDNS() 
{
    if (mdnsInitialized) 
        return;

    
    if (wifiState != WIFI_STA_OK || WiFi.status() != WL_CONNECTED)
        return;
    
    if (MDNS.begin(MDNS_HOSTNAME)) 
    {
        mdnsInitialized = true;
        MDNS.addService("http", "tcp", 80);
        
        Serial.println("[mDNS] Started successfully");
        Serial.printf("[mDNS] Stream: http://%s.local/stream\n", MDNS_HOSTNAME);
        
    } 
    else 
    {
        Serial.println("[mDNS] Failed to start");
        mdnsInitialized = false;
    }
}
