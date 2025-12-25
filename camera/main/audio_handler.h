#ifndef AUDIO_HANDLER_H
#define AUDIO_HANDLER_H

#include "config.h"

extern String audioFiles[];

extern Audio *audio;

void initializeAudio();
void initializeSDCard();
void playAudio(int audioIndex);
void handleAudioLoop();
bool isAudioPlaying();
void stopAudio();

#endif