#ifndef BLYNK_HANDLER_H
#define BLYNK_HANDLER_H

#include "config.h"

void initializeBlynk();
void handleBlynkLoop();
void initializeServos();
void handleServoLoop();
void updateServoPositions();
void moveServoToCenter();

void reconnectBlynk();
bool isBlynkConnected();
void handleEmergencyUnlock();

#endif