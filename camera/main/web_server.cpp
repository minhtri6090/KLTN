#include "wifi_manager.h"
#include "web_server.h"
#include "config.h"
#include "camera_handler.h"

WebServer server(80);
bool serverRunning = false;
bool apAdminLoggedIn = false;

QueueHandle_t clientQueue = NULL;
TaskHandle_t streamTaskHandle[MAX_CLIENTS] = {NULL, NULL, NULL};

void stream_task(void *pvParameters) 
{
    stream_client_t* streamClient = (stream_client_t*)pvParameters;
    
    WiFiClient& client = streamClient->client;

    unsigned long fpsLastReport = millis();
    int framesSent = 0;

    Serial.printf("[TASK] Streaming client %s\n", client.remoteIP().toString().c_str());

    unsigned long lastFrameTime = 0;
    const unsigned long frameInterval = 10;

    while (streamClient->active && client.connected()) 
    {
        if (millis() - lastFrameTime >= frameInterval) 
        {
            uint8_t* frameBuffer = nullptr;
            size_t frameLen = 0;

            portENTER_CRITICAL(&frameMux);
            if (frame_ready_a && use_buf_a) {
                frameBuffer = mjpeg_buf_a;
                frameLen = frame_len_a;
                frame_ready_a = false;
                use_buf_a = false;
            } else if (frame_ready_b && ! use_buf_a) {
                frameBuffer = mjpeg_buf_b;
                frameLen = frame_len_b;
                frame_ready_b = false;
                use_buf_a = true;
            }
            portEXIT_CRITICAL(&frameMux);

            if (frameBuffer && frameLen > 0) 
            {
                String boundary = "--frame\r\n";
                boundary += "Content-Type: image/jpeg\r\n";
                boundary += "Content-Length: " + String(frameLen) + "\r\n\r\n";
                client.print(boundary);
                client.write(frameBuffer, frameLen);
                client.print("\r\n");
                lastFrameTime = millis();

            //     framesSent++; // Đếm frame đã gửi

            //     // Báo FPS mỗi 1 giây
            //     if (millis() - fpsLastReport >= 1000) 
            //     {
            //         Serial.printf("[STREAM] FPS: %d\n", framesSent);
            //         framesSent = 0;
            //         fpsLastReport = millis();
            //     }
            }
        }

        if (! client.connected()) break;
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    // ✅ CLEANUP AN TOÀN HƠN
    Serial.printf("[TASK] Client %s disconnected, cleaning up\n", client.remoteIP(). toString().c_str());
    client.stop();
    
    // ✅ DÙNG CRITICAL SECTION KHI CẬP NHẬT ARRAY
    portENTER_CRITICAL(&frameMux);
    TaskHandle_t current = xTaskGetCurrentTaskHandle();
    for (int i = 0; i < MAX_CLIENTS; i++) 
    {
        if (streamTaskHandle[i] == current) 
        {
            streamTaskHandle[i] = NULL;
            break;
        }
    }
    portEXIT_CRITICAL(&frameMux);
    
    delete streamClient;
    vTaskDelete(NULL);
}

void handle_stream() {
    Serial.println("[STREAM] Client requesting stream");
    startStream();

    WiFiClient client = server.client();
    if (!client.connected()) {
        Serial.println("[STREAM] Error: Invalid client");
        return;
    }

    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: multipart/x-mixed-replace; boundary=frame");
    client.println("Access-Control-Allow-Origin: *");
    client.println("Cache-Control: no-cache, no-store, must-revalidate");
    client.println("Pragma: no-cache");
    client.println("Expires: 0");
    client.println("Connection: keep-alive");
    client.println();

    stream_client_t* streamClient = new stream_client_t;
    streamClient->client = client;
    streamClient->active = true;

    if (clientQueue != NULL) {
        if (xQueueSend(clientQueue, &streamClient, 0) != pdTRUE) {
            Serial.println("[STREAM] Max clients reached, rejecting");
            client.stop();
            delete streamClient;
        }
    }
}

void startMJPEGStreamingServer() 
{
    if (serverRunning) 
    {
        Serial.println("[SERVER] Server already running");
        return;
    }
    
    clientQueue = xQueueCreate(MAX_CLIENTS, sizeof(stream_client_t*));
    if (clientQueue == NULL) 
    {
        Serial.println("[SERVER] Failed to create client queue");
        return;
    }
    
    server.on("/stream", HTTP_GET, handle_stream);
    server.onNotFound([]() {
        server.send(404, "text/plain", "Not Found");
    });
    
    server.begin();
    serverRunning = true;
    
    Serial.println("[SERVER] MJPEG Streaming Server started");
    Serial.printf("[SERVER] Stream: http://%s/stream\n", WiFi.localIP().toString().c_str());
}

void stopMJPEGStreamingServer() 
{
    if (serverRunning) 
    {
        server.stop();
        Serial.println("[SERVER] Streaming server stopped");
    }
    serverRunning = false;

    if (clientQueue != nullptr) 
    {
        vQueueDelete(clientQueue);
        clientQueue = nullptr;
    }
}
void startAPWebServer() {
    apAdminLoggedIn = false;
    
    server.on("/", HTTP_GET, handleRootAP);
    server.on("/login", HTTP_POST, handleLoginAP);
    server.on("/scan", HTTP_GET, handleScanAP);
    server.on("/scan", HTTP_POST, handleScanAP);
    server.on("/scan-results", HTTP_GET, handleScanResults);
    server.on("/style.css", HTTP_GET, handleStyleCSS);
    
    server.begin();
    serverRunning = true;
    Serial.printf("Camera Configuration Portal: http://%s/\n", WiFi.softAPIP().toString().c_str());
}

void handleStyleCSS() {
    String css = R"=====(
* {
  margin: 0;
  padding: 0;
  box-sizing: border-box;
}

body {
  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
  background: linear-gradient(135deg, rgba(45, 74, 166, 0.9), rgba(30, 58, 138, 0.9));
  min-height: 100vh;
  display: flex;
  align-items: center;
  justify-content: center;
  padding: 20px;
  position: relative;
  overflow-x: hidden;
}

.container {
  background: rgba(255, 255, 255, 0.96);
  border-radius: 12px;
  box-shadow: 0 25px 50px -12px rgba(0,0,0,0.3);
  overflow: hidden;
  width: 100%;
  max-width: 400px;
  backdrop-filter: blur(20px);
  position: relative;
  z-index: 1;
  border: 1px solid rgba(255, 255, 255, 0.3);
}

.header {
  background: linear-gradient(135deg, #2d4aa6, #1e3a8a);
  color: white;
  padding: 30px 25px;
  text-align: center;
  position: relative;
  overflow: hidden;
}

.header h1 {
  font-size: 24px;
  font-weight: 700;
  margin-bottom: 8px;
  text-shadow: 0 2px 4px rgba(0,0,0,0.1);
}

.header p {
  opacity: 0.9;
  font-size: 14px;
  text-shadow: 0 1px 2px rgba(0,0,0,0.1);
}

.content {
  padding: 30px 25px;
  position: relative;
}

.form-group {
  margin-bottom: 20px;
}

.form-label {
  display: block;
  margin-bottom: 8px;
  font-weight: 600;
  color: #1e3a8a;
  font-size: 14px;
}

.form-input {
  width: 100%;
  padding: 12px 16px;
  border: 2px solid #cbd5e1;
  border-radius: 8px;
  font-size: 16px;
  transition: all 0.3s ease;
  background: rgba(255, 255, 255, 0.9);
}

.form-input:focus {
  outline: none;
  border-color: #2d4aa6;
  box-shadow: 0 0 0 3px rgba(45,74,166,0.1);
  background: white;
  transform: translateY(-1px);
}

.password-container {
  position: relative;
}

.password-container .form-input {
  padding-right: 45px;
}

.password-toggle {
  position: absolute;
  right: 12px;
  top: 50%;
  transform: translateY(-50%);
  background: none;
  border: none;
  cursor: pointer;
  padding: 4px;
  border-radius: 4px;
  transition: all 0.2s ease;
  color: #6b7280;
}

.password-toggle:hover {
  background: rgba(45, 74, 166, 0.1);
  color: #2d4aa6;
}

.eye-icon {
  width: 18px;
  height: 18px;
  display: inline-block;
}

.btn {
  display: block;
  width: 100%;
  padding: 12px 20px;
  border: none;
  border-radius: 8px;
  font-size: 16px;
  font-weight: 600;
  cursor: pointer;
  transition: all 0.3s ease;
  text-decoration: none;
  text-align: center;
  position: relative;
  overflow: hidden;
}

.btn-primary {
  background: linear-gradient(135deg, #2d4aa6, #1e3a8a);
  color: white;
  box-shadow: 0 4px 15px rgba(45, 74, 166, 0.3);
}

.btn-primary:hover {
  background: linear-gradient(135deg, #1e3a8a, #1e40af);
  transform: translateY(-2px);
  box-shadow: 0 6px 20px rgba(45, 74, 166, 0.4);
}

.btn-primary:disabled {
  background: #9ca3af;
  cursor: not-allowed;
  transform: none;
  box-shadow: none;
}

.btn-secondary {
  background: #f3f4f6;
  color: #1e3a8a;
  border: 2px solid #cbd5e1;
}

.btn-secondary:hover {
  background: #e5e7eb;
  border-color: #2d4aa6;
  transform: translateY(-1px);
}

.alert {
  padding: 12px 16px;
  border-radius: 8px;
  margin-bottom: 20px;
  position: relative;
  backdrop-filter: blur(5px);
}

.alert-error {
  background: rgba(254, 226, 226, 0.9);
  color: #dc2626;
  border: 1px solid #fca5a5;
}

.alert-info {
  background: rgba(45, 74, 166, 0.1);
  color: #1e3a8a;
  border: 1px solid rgba(45, 74, 166, 0.2);
}

.wifi-item {
  display: flex;
  align-items: center;
  padding: 12px;
  border: 2px solid #cbd5e1;
  border-radius: 8px;
  margin-bottom: 8px;
  cursor: pointer;
  transition: all 0.3s ease;
  background: rgba(249, 250, 251, 0.8);
  backdrop-filter: blur(5px);
}

.wifi-item:hover {
  border-color: #2d4aa6;
  background: rgba(255, 255, 255, 0.9);
  transform: translateY(-1px);
  box-shadow: 0 4px 12px rgba(45, 74, 166, 0.1);
}

.wifi-item.selected {
  border-color: #2d4aa6;
  background: rgba(45,74,166,0.05);
  transform: translateY(-1px);
  box-shadow: 0 4px 12px rgba(45, 74, 166, 0.2);
}

.wifi-name {
  flex: 1;
  font-weight: 500;
  margin-right: 12px;
  color: #1e3a8a;
}

.wifi-security {
  font-size: 12px;
  color: #6b7280;
  background: rgba(107, 114, 128, 0.1);
  padding: 2px 6px;
  border-radius: 4px;
}

.university-header {
  background: linear-gradient(135deg, rgba(45, 74, 166, 0.1), rgba(30, 58, 138, 0.05));
  padding: 12px 16px;
  margin: -30px -25px 20px -25px;
  border-bottom: 1px solid rgba(45, 74, 166, 0.2);
  text-align: center;
  position: relative;
}

.university-header h3 {
  color: #2d4aa6;
  font-size: 14px;
  margin: 0;
  font-weight: 600;
  text-shadow: 0 1px 2px rgba(0,0,0,0.05);
}

.loading {
  text-align: center;
  padding: 40px 20px;
  color: #6b7280;
}

.spinner {
  display: inline-block;
  width: 20px;
  height: 20px;
  border: 3px solid rgba(45, 74, 166, 0.1);
  border-top: 3px solid #2d4aa6;
  border-radius: 50%;
  animation: spin 1s linear infinite;
  margin-right: 10px;
}

@keyframes spin {
  0% { transform: rotate(0deg); }
  100% { transform: rotate(360deg); }
}

@media (max-width: 480px) {
  .container {
    margin: 10px;
    max-width: none;
  }
  
  .header {
    padding: 25px 20px;
  }
  
  .content {
    padding: 25px 20px;
  }
}
)=====";
    server.send(200, "text/css", css);
}

void handleRootAP() {
    String html = R"=====(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>WiFi Config</title>
    <link rel="stylesheet" href="/style.css">
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>WiFi Config</h1>
            <p>Configuration Portal</p>
        </div>
        
        <div class="content">
            <div class="university-header">
                <h3>Industrial University of Ho Chi Minh City</h3>
            </div>
            
            <form method="POST" action="/login">
                <div class="form-group">
                    <label class="form-label">Username</label>
                    <input type="text" name="username" class="form-input" placeholder="Enter username" required>
                </div>
                
                <div class="form-group">
                    <label class="form-label">Password</label>
                    <div class="password-container">
                        <input type="password" name="password" id="passwordInput" class="form-input" placeholder="Enter password" required>
                        <button type="button" class="password-toggle" onclick="togglePassword()">
                            <svg class="eye-icon" id="eyeIcon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                                <path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"></path>
                                <circle cx="12" cy="12" r="3"></circle>
                            </svg>
                        </button>
                    </div>
                </div>
                
                <button type="submit" class="btn btn-primary">Login</button>
            </form>
        </div>
    </div>
    
    <script>
        function togglePassword() {
            const passwordInput = document.getElementById('passwordInput');
            const eyeIcon = document.getElementById('eyeIcon');
            
            if (passwordInput.type == 'password') {
                passwordInput.type = 'text';
                eyeIcon.innerHTML = '<path d="M17.94 17.94A10.07 10.07 0 0 1 12 20c-7 0-11-8-11-8a18.45 18.45 0 0 1 5.06-5.94M9.9 4.24A9.12 9.12 0 0 1 12 4c7 0 11 8 11 8a18.5 18.5 0 0 1-2.16 3.19m-6.72-1.07a3 3 0 1 1-4.24-4.24"></path><line x1="1" y1="1" x2="23" y2="23"></line>';
            } else {
                passwordInput.type = 'password';
                eyeIcon.innerHTML = '<path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"></path><circle cx="12" cy="12" r="3"></circle>';
            }
        }
    </script>
</body>
</html>
)=====";
    server.send(200, "text/html; charset=utf-8", html);
}

void handleLoginAP() {
    bool loginSuccess = (
        server.hasArg("username") &&
        server.hasArg("password") &&
        server.arg("username") == "admin" && 
        server.arg("password") == "admin"
    );
    
    if (loginSuccess) {
        apAdminLoggedIn = true;
        server.sendHeader("Location", "/scan");
        server.send(302, "text/plain", "");
        return;
    }
    
    String html = R"=====(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Login Failed</title>
    <link rel="stylesheet" href="/style.css">
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>Login Failed</h1>
            <p>Invalid credentials</p>
        </div>
        
        <div class="content">
            <div class="university-header">
                <h3>Industrial University of Ho Chi Minh City</h3>
            </div>
            
            <div class="alert alert-error">Wrong username or password!</div>
            
            <form method="POST" action="/login">
                <div class="form-group">
                    <label class="form-label">Username</label>
                    <input type="text" name="username" class="form-input" placeholder="Enter username" required autofocus>
                </div>
                
                <div class="form-group">
                    <label class="form-label">Password</label>
                    <div class="password-container">
                        <input type="password" name="password" id="passwordInput" class="form-input" placeholder="Enter password" required>
                        <button type="button" class="password-toggle" onclick="togglePassword()">
                            <svg class="eye-icon" id="eyeIcon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                                <path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"></path>
                                <circle cx="12" cy="12" r="3"></circle>
                            </svg>
                        </button>
                    </div>
                </div>
                
                <button type="submit" class="btn btn-primary">Try Again</button>
            </form>
        </div>
    </div>
    
    <script>
        function togglePassword() {
            const passwordInput = document.getElementById('passwordInput');
            const eyeIcon = document.getElementById('eyeIcon');
            
            if (passwordInput.type === 'password') {
                passwordInput.type = 'text';
                eyeIcon.innerHTML = '<path d="M17.94 17.94A10.07 10.07 0 0 1 12 20c-7 0-11-8-11-8a18.45 18.45 0 0 1 5.06-5.94M9.9 4.24A9.12 9.12 0 0 1 12 4c7 0 11 8 11 8a18.5 18.5 0 0 1-2.16 3.19m-6.72-1.07a3 3 0 1 1-4.24-4.24"></path><line x1="1" y1="1" x2="23" y2="23"></line>';
            } else {
                passwordInput.type = 'password';
                eyeIcon.innerHTML = '<path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"></path><circle cx="12" cy="12" r="3"></circle>';
            }
        }
    </script>
</body>
</html>
)=====";
    server.send(401, "text/html; charset=utf-8", html);
}

void handleScanAP() {
    if (!apAdminLoggedIn) {
        server.sendHeader("Location", "/");
        server.send(302, "text/plain", "");
        return;
    }

    if (server.method() == HTTP_POST) {
        String ssid = server.arg("ssid");
        String pass = server.arg("password");

        if (ssid.length() == 0) {
            server.send(400, "text/html", getErrorPage("Please select a WiFi network"));
            return;
        }

        Serial.printf("Received connection request: SSID='%s'\n", ssid.c_str());
        saveCredentials(ssid, pass);
        
        String html = R"=====(
        <!DOCTYPE html>
        <html>
        <head>
            <meta charset="UTF-8">
            <meta name="viewport" content="width=device-width, initial-scale=1.0">
            <meta http-equiv='refresh' content='3;url=/'>
            <title>Connecting...</title>
            <style>
                * { margin: 0; padding: 0; box-sizing: border-box; }
                body {
                    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
                    background: linear-gradient(135deg, rgba(45, 74, 166, 0.9), rgba(30, 58, 138, 0.9));
                    min-height: 100vh;
                    display: flex;
                    align-items: center;
                    justify-content: center;
                    padding: 20px;
                }
                .container {
                    background: rgba(255, 255, 255, 0.96);
                    border-radius: 12px;
                    box-shadow: 0 25px 50px -12px rgba(0,0,0,0.3);
                    overflow: hidden;
                    width: 100%;
                    max-width: 400px;
                    backdrop-filter: blur(20px);
                    border: 1px solid rgba(255, 255, 255, 0.3);
                }
                .header {
                    background: linear-gradient(135deg, #2d4aa6, #1e3a8a);
                    color: white;
                    padding: 30px 25px;
                    text-align: center;
                }
                .header h1 {
                    font-size: 24px;
                    font-weight: 700;
                    margin-bottom: 8px;
                    text-shadow: 0 2px 4px rgba(0,0,0,0.1);
                }
                .header p {
                    opacity: 0.9;
                    font-size: 14px;
                }
                .content {
                    padding: 30px 25px;
                    text-align: center;
                }
                .university-header {
                    background: linear-gradient(135deg, rgba(45, 74, 166, 0.1), rgba(30, 58, 138, 0.05));
                    padding: 12px 16px;
                    margin: -30px -25px 20px -25px;
                    border-bottom: 1px solid rgba(45, 74, 166, 0.2);
                }
                .university-header h3 {
                    color: #2d4aa6;
                    font-size: 14px;
                    font-weight: 600;
                }
                .spinner {
                    display: inline-block;
                    width: 24px;
                    height: 24px;
                    border: 3px solid rgba(45, 74, 166, 0.1);
                    border-top: 3px solid #2d4aa6;
                    border-radius: 50%;
                    animation: spin 1s linear infinite;
                    margin-right: 10px;
                }
                @keyframes spin {
                    0% { transform: rotate(0deg); }
                    100% { transform: rotate(360deg); }
                }
                .status-text {
                    color: #1e3a8a;
                    font-size: 16px;
                    margin-top: 15px;
                }
                .redirect-info {
                    color: #6b7280;
                    font-size: 13px;
                    margin-top: 20px;
                    font-style: italic;
                }
            </style>
        </head>
        <body>
            <div class="container">
                <div class="header">
                    <h1>Connecting...</h1>
                    <p>Please wait</p>
                </div>
                <div class="content">
                    <div class="university-header">
                        <h3>Industrial University of Ho Chi Minh City</h3>
                    </div>
                    <div>
                        <div class="spinner"></div>
                        <div class="status-text">Connecting to WiFi network</div>
                    </div>
                    <div class="redirect-info">
                        Auto redirect in 3 seconds...
                    </div>
                </div>
            </div>
        </body>
        </html>
        )=====";
        server.send(200, "text/html", html);

        connecting = true;
        connectingSSID = ssid;
        connectingPassword = pass;
        connectStartTime = millis();
        return;
    }

    String loadingHtml = R"=====(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Scanning WiFi Networks</title>
    <link rel="stylesheet" href="/style.css">
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>Scanning Networks</h1>
            <p>Please wait...</p>
        </div>
        <div class="content">
            <div class="university-header">
                <h3>Industrial University of Ho Chi Minh City</h3>
            </div>
            
            <div class="loading">
                <div class="spinner"></div>
                Scanning for available WiFi networks...
            </div>
            <div style="text-align: center; margin-top: 20px; color: #6b7280;">
                <p>This may take a few seconds</p>
            </div>
        </div>
    </div>
    
    <script>
        setTimeout(() => {
            window.location.href = '/scan-results';
        }, 3000);
    </script>
</body>
</html>
)=====";
    
    server.send(200, "text/html", loadingHtml);
}

void handleScanResults() {
    if (!apAdminLoggedIn) {
        server.sendHeader("Location", "/");
        server.send(302, "text/plain", "");
        return;
    }
    
    WiFi.mode(WIFI_AP_STA);
    vTaskDelay(pdMS_TO_TICKS(200));
    int n = WiFi.scanNetworks(false, true, false, 300);
    
    String html = R"=====(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Select WiFi Network</title>
    <link rel="stylesheet" href="/style.css">
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>Select WiFi Network</h1>
            <p>Found %%COUNT%% networks</p>
        </div>
        
        <div class="content">
            <div class="university-header">
                <h3>Industrial University of Ho Chi Minh City</h3>
            </div>
            
            <form method="POST" action="/scan" id="wifiForm">
                <div class="form-group">
                    <label class="form-label">Available Networks</label>
                    <div style="max-height: 250px; overflow-y: auto; border: 1px solid #cbd5e1; border-radius: 8px; padding: 8px;">
                        %%WIFI_LIST%%
                    </div>
                </div>
                
                <div class="form-group">
                    <label class="form-label">WiFi Password</label>
                    <div class="password-container">
                        <input type="password" name="password" id="passwordInput" class="form-input" 
                               placeholder="Enter password (leave empty for open networks)" maxlength="63">
                        <button type="button" class="password-toggle" onclick="togglePassword()">
                            <svg class="eye-icon" id="eyeIcon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                                <path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"></path>
                                <circle cx="12" cy="12" r="3"></circle>
                            </svg>
                        </button>
                    </div>
                </div>
                
                <button type="submit" class="btn btn-primary" id="connectBtn" disabled>
                    Connect to WiFi
                </button>
                
                <a href="/" class="btn btn-secondary" style="margin-top: 10px;">Back to Login</a>
            </form>
            
            <div style="margin-top: 20px; font-size: 14px; color: #6b7280;">
                <p><strong>Instructions:</strong></p>
                <p>1. Select a network from the list above</p>
                <p>2. Enter the correct WiFi password</p>
                <p>3. Wait for automatic connection and redirection</p>
            </div>
        </div>
    </div>

    <script>
        let selectedSSID = '';
        
        function selectWiFi(ssid, element) {
            selectedSSID = ssid;
            
            document.querySelectorAll('.wifi-item').forEach(item => {
                item.classList.remove('selected');
            });
            
            element.classList.add('selected');
            
            let ssidInput = document.getElementById('ssidInput');
            if (!ssidInput) {
                ssidInput = document.createElement('input');
                ssidInput.type = 'hidden';
                ssidInput.name = 'ssid';
                ssidInput.id = 'ssidInput';
                document.getElementById('wifiForm').appendChild(ssidInput);
            }
            ssidInput.value = ssid;
            
            const connectBtn = document.getElementById('connectBtn');
            connectBtn.disabled = false;
            connectBtn.style.opacity = '1';
            
            document.getElementById('passwordInput').focus();
        }
        
        function togglePassword() {
            const passwordInput = document.getElementById('passwordInput');
            const eyeIcon = document.getElementById('eyeIcon');
            
            if (passwordInput.type === 'password') {
                passwordInput.type = 'text';
                eyeIcon.innerHTML = '<path d="M17.94 17.94A10.07 10.07 0 0 1 12 20c-7 0-11-8-11-8a18.45 18.45 0 0 1 5.06-5.94M9.9 4.24A9.12 9.12 0 0 1 12 4c7 0 11 8 11 8a18.5 18.5 0 0 1-2.16 3.19m-6.72-1.07a3 3 0 1 1-4.24-4.24"></path><line x1="1" y1="1" x2="23" y2="23"></line>';
            } else {
                passwordInput.type = 'password';
                eyeIcon.innerHTML = '<path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"></path><circle cx="12" cy="12" r="3"></circle>';
            }
        }
        
        document.getElementById('wifiForm').addEventListener('submit', function(e) {
            if (!selectedSSID) {
                e.preventDefault();
                alert('Please select a WiFi network first!');
                return false;
            }
            
            const connectBtn = document.getElementById('connectBtn');
            connectBtn.innerHTML = 'Connecting...';
            connectBtn.disabled = true;
        });
    </script>
</body>
</html>
)=====";

    String wifiList = "";
    if (n == 0) {
        wifiList = "<div class='alert alert-error'>No networks found. <button onclick='location.reload()' class='btn btn-secondary'>Scan Again</button></div>";
    } else {
        for (int i = 0; i < n; i++) {
            String ssid = WiFi.SSID(i);
            int rssi = WiFi.RSSI(i);
            wifi_auth_mode_t encType = WiFi.encryptionType(i);
            
            if (ssid.length() == 0) continue;
            
            bool isOpen = (encType == WIFI_AUTH_OPEN);
            String security = isOpen ? "Open" : "Secured";
            
            String escapedSSID = ssid;
            escapedSSID.replace("\"", "&quot;");
            
            wifiList += "<div class='wifi-item' onclick='selectWiFi(\"" + escapedSSID + "\", this);' title='Signal: " + String(rssi) + " dBm'>";
            wifiList += "<span class='wifi-name'>" + ssid + "</span>";
            wifiList += "<span class='wifi-security'>" + security + "</span>";
            wifiList += "</div>";
        }
    }
    
    html.replace("%%COUNT%%", String(n));
    html.replace("%%WIFI_LIST%%", wifiList);
    
    server.send(200, "text/html", html);
    WiFi.mode(WIFI_AP);
}

String getErrorPage(String message) {
    return R"=====(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Error</title>
    <link rel="stylesheet" href="/style.css">
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>Error</h1>
            <p>Something went wrong</p>
        </div>
        <div class="content">
            <div class="university-header">
                <h3>Industrial University of Ho Chi Minh City</h3>
            </div>
            
            <div class="alert alert-error">)====" + message + R"=====(</div>
            <a href="/scan" class="btn btn-primary">Try Again</a>
            <a href="/" class="btn btn-secondary" style="margin-top: 10px;">Back to Home</a>
        </div>
    </div>
</body>
</html>
)=====";
}




void handleWebServerLoop() 
{
    if (serverRunning) 
    {
        server.handleClient();
    }
}

void handleNotFound() 
{
    server.send(404, "text/plain", "Not Found");
}

void stopWebServer() {
    if (serverRunning) {
        server.stop();
        serverRunning = false;
    }
}

void restartWebServer() {
    stopWebServer();
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    if (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) {
        startAPWebServer();
    } else if (WiFi.getMode() == WIFI_STA) {
        startMJPEGStreamingServer();
    }
}