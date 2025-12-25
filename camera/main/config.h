#ifndef CONFIG_H
#define CONFIG_H

// INCLUDES
#include <WiFi.h>
#include <WebServer.h>
#include <EEPROM.h>
#include "USB_STREAM.h"
#include "esp_heap_caps.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <SPI.h>
#include <SD.h>
#include "Audio.h"
#include <ESP32Servo.h>
#include <ESPmDNS.h>

#define MDNS_HOSTNAME "cameraiuh"

#define FRAME_WIDTH 800
#define FRAME_HEIGHT 600
#define MJPEG_BUF_SIZE (FRAME_WIDTH * FRAME_HEIGHT * 2)
#define USB_PAYLOAD_BUF_SIZE (64 * 1024)
#define USB_FRAME_BUF_SIZE (256 * 1024)


#define SD_CS     10
#define SPI_MOSI  12
#define SPI_MISO  13

#define SPI_SCK   11

#define I2S_DOUT  40 
#define I2S_BCLK  41
#define I2S_LRC   42

#define SIM_TX_PIN          16
#define SIM_RX_PIN          17
#define SIM_POWER_PIN       15

#define PIR_PIN             7

#define SERVO1_PIN          47
#define SERVO2_PIN          48

#define LDR_PIN 4              
#define LED_PIN 6              // IR LED
#define FLASH_LED_PIN  5      // Flash LED

// ✅ Cấu hình LDR (đơn giản)
#define LDR_DARK_THRESHOLD 100   // < 400 = tối
#define LDR_READ_INTERVAL 1000   // Đọc mỗi 1s

#define AUDIO_HELLO                0  
#define AUDIO_WIFI_FAILED          1  
#define AUDIO_WIFI_SUCCESS         2  
#define AUDIO_MOTION_DETECTED      3 

#define MAX_CLIENTS 3
#define APP_CPU 1
#define PRO_CPU 0

#define V_SERVO1_LEFT   V10
#define V_SERVO1_RIGHT  V11
#define V_SERVO2_DOWN   V12
#define V_SERVO2_UP     V13
#define V_SERVO_CENTER  V14
#define V_EMERGENCY_UNLOCK V15

enum WiFiState { 
    WIFI_STA_OK,   
    WIFI_AP_MODE    
};

#endif