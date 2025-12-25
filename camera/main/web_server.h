#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "config.h"

extern WebServer server;
extern bool serverRunning;

typedef struct {
    WiFiClient client;
    bool active;
} stream_client_t;

extern QueueHandle_t clientQueue;
extern TaskHandle_t streamTaskHandle[MAX_CLIENTS];

extern void stream_task(void *pvParameters);

void startMJPEGStreamingServer();
void stopMJPEGStreamingServer();
void handleWebServerLoop();

void handle_stream();

void startAPWebServer();

void handleRootAP();
void handleLoginAP();
void handleScanAP();
void handleStyleCSS();
void handleScanResults();

String getErrorPage(String message);

#endif