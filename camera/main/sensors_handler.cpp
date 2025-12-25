
#include "sensors_handler.h"
#include "audio_handler.h"
#include "security_system.h"
#include "driver/gpio.h"

bool systemReady = false;
unsigned long lastMotionTime = 0;
const unsigned long motionCooldown = 5000;

int radarState = LOW; 
int radarVal = 0; 
unsigned long motionStartTime = 0;
bool motionInProgress = false;

// ✅ LDR & LED State
int ldrValue = 0;
bool isDark = false;
bool irLedState = false;
bool flashLedState = false;
unsigned long lastLDRRead = 0;

// ✅ Debounce cho motion update
unsigned long lastMotionUpdateTime = 0;
const unsigned long motionUpdateInterval = 500;

// ✅ Debounce cho motion END (tránh spam khi PIR nhiễu)
static unsigned long motionEndCandidateTime = 0;
const unsigned long MOTION_END_DEBOUNCE_MS = 200; // 200ms ổn với hầu hết cảm biến PIR

// ✅ Khi flash được bật do motion, ignore đọc LDR trong khoảng thời gian này
static unsigned long flashIgnoreUntil = 0;
const unsigned long FLASH_LDR_IGNORE_MS = 10000; // 10s, có thể điều chỉnh

void initializeSensors() 
{
    pinMode(PIR_PIN, INPUT);
    
    pinMode(FLASH_LED_PIN, OUTPUT);
    digitalWrite(FLASH_LED_PIN, LOW);
    flashLedState = false;
    
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    irLedState = false;
    
    pinMode(LDR_PIN, INPUT);
    readLDRSensor();
    
    radarVal = digitalRead(PIR_PIN);
    radarState = LOW;
    motionInProgress = false;
    lastMotionTime = 0;
    lastMotionUpdateTime = 0;
    motionEndCandidateTime = 0;
    flashIgnoreUntil = 0;
    
    Serial.println("[PIR] Initialized (polling mode)");
    
    updateLEDsBasedOnConditions();
}

void readLDRSensor() 
{
    // Nếu đang trong khoảng ignore do flash bật, bỏ qua đọc LDR (không cập nhật isDark)
    unsigned long now = millis();
    if (flashIgnoreUntil != 0 && now < flashIgnoreUntil) 
    {
        // Vẫn cập nhật ldrValue để debug/hiển thị nếu cần, nhưng không thay đổi state isDark
        ldrValue = analogRead(LDR_PIN);
        // Log 1 lần khi bắt đầu ignore (giảm spam)
        static unsigned long lastIgnoreLog = 0;
        if (now - lastIgnoreLog > 2000) 
        {
            Serial.printf("[LDR] Ignoring LDR for %lu ms (value=%d)\n", flashIgnoreUntil - now, ldrValue);
            lastIgnoreLog = now;
        }
        return;
    }

    ldrValue = analogRead(LDR_PIN);

    bool wasDark = isDark;
    isDark = (ldrValue < LDR_DARK_THRESHOLD);
    
    if (wasDark != isDark) 
    {
        Serial.printf("[LDR] Light changed: %s (value=%d)\n", isDark ? "DARK" : "BRIGHT", ldrValue);
        updateLEDsBasedOnConditions();
    }
}

void controlIRLED(bool turnOn) 
{
    if (turnOn != irLedState) 
    {
        irLedState = turnOn;
        digitalWrite(LED_PIN, turnOn ? HIGH : LOW);
        Serial.printf("[IR_LED] %s\n", turnOn ? "ON" : "OFF");
    }
}

void controlFlashLED(bool turnOn) 
{
    if (turnOn != flashLedState) 
    {
        flashLedState = turnOn;
        digitalWrite(FLASH_LED_PIN, turnOn ? HIGH : LOW);
        Serial.printf("[FLASH_LED] %s\n", turnOn ? "ON" : "OFF");

        if (turnOn) {
            // Khi bật flash do motion -> ignore LDR trong thời gian định trước
            flashIgnoreUntil = millis() + FLASH_LDR_IGNORE_MS;
            Serial.printf("[LDR] Flash ON -> ignoring LDR for %lu ms\n", FLASH_LDR_IGNORE_MS);
        } else {
            // Khi tắt flash -> clear override
            flashIgnoreUntil = 0;
            Serial.println("[LDR] Flash OFF -> resume LDR readings");
        }
    }
}

void updateLEDsBasedOnConditions() {
    if (isDark) 
    {
        if (motionInProgress) 
        {
            controlIRLED(false);
            controlFlashLED(true);
        } 
        else 
        {
            controlIRLED(true);
            controlFlashLED(false);
        }
    } 
    else 
    {
        controlIRLED(false);
        controlFlashLED(false);
    }
}

void handleLDRLoop() 
{
    if (systemReady && (millis() - lastLDRRead >= LDR_READ_INTERVAL)) 
    {
        lastLDRRead = millis();
        readLDRSensor();
    }
}

void resetMotionCooldown() 
{
    lastMotionTime = 0;
    Serial.println("[MOTION] Cooldown reset");
}

void handleMotionLoop() 
{
    if (!systemReady) return;
    
    radarVal = digitalRead(PIR_PIN);
    unsigned long currentTime = millis();
    
    // Nếu phát hiện chuyển sang HIGH => reset candidate end
    if (radarVal == HIGH && radarState == LOW) 
    {
        if (currentTime - lastMotionTime > motionCooldown) 
        {
            Serial.println("\n[MOTION] Motion started");
            
            lastMotionTime = currentTime;
            motionStartTime = currentTime;
            radarState = HIGH;
            motionInProgress = true;
            lastMotionUpdateTime = currentTime;
            motionEndCandidateTime = 0; // huỷ candidate end nếu có
            
            updateLEDsBasedOnConditions();
            
            // Trigger security system (chỉ lần đầu)
            onMotionDetected();
            
        } 
        else 
        {
            radarState = HIGH;
            motionEndCandidateTime = 0;
        }
    }
    // Motion CONTINUE (throttle update)
    else if (radarVal == HIGH && radarState == HIGH) 
    {
        // Chỉ cập nhật mỗi 500ms
        if (currentTime - lastMotionUpdateTime >= motionUpdateInterval) 
        {
            lastMotionUpdateTime = currentTime;
            
            extern SecurityState currentSecurityState;
            if (currentSecurityState != SECURITY_IDLE) 
            {
                updateMotionTimestamp();
            }
        }
        motionEndCandidateTime = 0; // vẫn HIGH nên clear candidate end
    }
    // Motion END → debounce trước khi xác nhận
    else if (radarVal == LOW && radarState == HIGH) 
    {
        // Nếu chưa có candidate thì bắt đầu đếm
        if (motionEndCandidateTime == 0) 
        {
            motionEndCandidateTime = currentTime;
            // không log to nhiều để tránh spam
        } 
        else 
        {
            // nếu đã giữ LOW đủ thời gian => xác nhận motion ended
            if (currentTime - motionEndCandidateTime >= MOTION_END_DEBOUNCE_MS) 
            {
                Serial.println("\n[MOTION] Motion ended");
                radarState = LOW;
                motionInProgress = false;
                motionEndCandidateTime = 0;
                
                updateLEDsBasedOnConditions();
                
                //  GỌI HÀM TẮT BUZZER KHI MOTION END
                onMotionEnded();
            }
        }
    } 
    else 
    {
        // trạng thái khác: reset candidate để an toàn
        motionEndCandidateTime = 0;
    }
}