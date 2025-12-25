#include "audio_handler.h"
#include "wifi_manager.h"

#define AUDIO_FILES_COUNT (sizeof(audioFiles)/sizeof(audioFiles[0]))

String audioFiles[] = {
  "/amthanh/xin_chao.mp3",                                  
  "/amthanh/ket_noi_wifi_khong_thanh_cong.mp3",            
  "/amthanh/ket_noi_wifi_thanh_cong.mp3",
  "/amthanh/phat_hien_chuyen_dong.mp3",    
};

Audio *audio = nullptr;
bool audioInitialized = false;

void initializeSDCard() 
{
    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, SD_CS);
    
    if (!SD.begin(SD_CS)) {
        Serial.println("[SD] Card Mount Failed");
        return;
    }
    
    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
        Serial.println("[SD] No SD card attached");
        return;
    }
}

void initializeAudio() 
{
    if (audio == nullptr) 
    {
        audio = new Audio();
        if (audio) 
        {
            audio->setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
            audio->setVolume(21); 
            audioInitialized = true;
        }
    }
}

void playAudio(int audioIndex) 
{
    if (!audioInitialized || !audio) 
    {
        Serial.println("[AUDIO] Not initialized");
        return;
    }
    
    if (audio->isRunning()) 
    {
        Serial.println("[AUDIO] Stopping current audio");
        audio->stopSong();
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    String filePath = audioFiles[audioIndex];
    
    if (SD.exists(filePath)) //check if file exists
    {
        Serial.printf("[AUDIO] Playing: %s\n", filePath.c_str());
        audio->connecttoFS(SD, filePath.c_str());
    } 
    else 
    {
        Serial.printf("[AUDIO] File not found: %s\n", filePath.c_str());
    }
}

void stopAudio() {
    if (audioInitialized && audio && audio->isRunning()) {
        Serial.println("[AUDIO] Stopped");
        audio->stopSong();
    }
}

void handleAudioLoop() {
    if (audioInitialized && audio) 
    {
        audio->loop();
    }
}

bool isAudioPlaying() {
    return (audioInitialized && audio && audio->isRunning());
}