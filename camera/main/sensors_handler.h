#ifndef SENSORS_HANDLER_H
#define SENSORS_HANDLER_H

#include "config.h"

extern bool systemReady;
extern unsigned long lastMotionTime;
extern const unsigned long motionCooldown;
extern int radarState;
extern int radarVal;
extern unsigned long motionStartTime;
extern bool motionInProgress;

extern int ldrValue;
extern bool isDark;
extern bool irLedState;
extern bool flashLedState;
extern unsigned long lastLDRRead;

void initializeSensors(); 
void handleMotionLoop();
void handleLDRLoop();
void readLDRSensor();

void controlIRLED(bool turnOn);
void controlFlashLED(bool turnOn);
void updateLEDsBasedOnConditions();

void resetMotionCooldown();

#endif