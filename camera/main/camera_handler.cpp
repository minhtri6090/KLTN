#include "camera_handler.h"
#include "web_server.h"

uint8_t* mjpeg_buf_a = nullptr;
uint8_t* mjpeg_buf_b = nullptr;
volatile size_t frame_len_a = 0;
volatile size_t frame_len_b = 0;
volatile bool frame_ready_a = false;
volatile bool frame_ready_b = false;
volatile bool use_buf_a = true;
portMUX_TYPE frameMux = portMUX_INITIALIZER_UNLOCKED;

uint8_t* payload_buf_a = nullptr;
uint8_t* payload_buf_b = nullptr;
uint8_t* frame_buf = nullptr;
uint32_t frame_cnt_recv = 0;
uint32_t frame_cnt_sent = 0;

USB_STREAM* uvc = nullptr;
bool uvcStarted = false;

static bool streaming_started = false;
static TaskHandle_t clientProcessorHandle = NULL;

void initializeBuffers() 
{
    if(!psramFound()) 
    {
        while(true) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    mjpeg_buf_a = (uint8_t*)heap_caps_malloc(MJPEG_BUF_SIZE, MALLOC_CAP_SPIRAM);
    mjpeg_buf_b = (uint8_t*)heap_caps_malloc(MJPEG_BUF_SIZE, MALLOC_CAP_SPIRAM);
    payload_buf_a = (uint8_t*)heap_caps_malloc(USB_PAYLOAD_BUF_SIZE, MALLOC_CAP_SPIRAM);
    payload_buf_b = (uint8_t*)heap_caps_malloc(USB_PAYLOAD_BUF_SIZE, MALLOC_CAP_SPIRAM);
    frame_buf = (uint8_t*)heap_caps_malloc(USB_FRAME_BUF_SIZE, MALLOC_CAP_SPIRAM);
    
    if(!mjpeg_buf_a || !mjpeg_buf_b || !payload_buf_a || !payload_buf_b || !frame_buf) 
    {
        heap_caps_free(mjpeg_buf_a);
        heap_caps_free(mjpeg_buf_b);
        heap_caps_free(payload_buf_a);
        heap_caps_free(payload_buf_b);
        heap_caps_free(frame_buf);
        while(1) vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void initializeCamera() 
{
    uvc = new USB_STREAM();
    uvc->uvcConfiguration(FRAME_WIDTH, FRAME_HEIGHT, FRAME_INTERVAL_FPS_30,
                         USB_PAYLOAD_BUF_SIZE, payload_buf_a, payload_buf_b,
                         USB_FRAME_BUF_SIZE, frame_buf);
    uvc->uvcCamRegisterCb(frame_cb, nullptr);
}

//hàm nãy sẽ được gọi khi có frame mưới được nhận
void frame_cb(uvc_frame_t* frame, void*) 
{
    if (!frame || !frame->data || frame->data_bytes == 0) return;
    if (frame->data_bytes > MJPEG_BUF_SIZE || frame->data_bytes < 2000) return;
    
    frame_cnt_recv++;
    
    //double buffering
    portENTER_CRITICAL_ISR(&frameMux);
    if (use_buf_a && !frame_ready_a) 
    {
        memcpy(mjpeg_buf_a, frame->data, frame->data_bytes);
        frame_len_a = frame->data_bytes;
        frame_ready_a = true;
        use_buf_a = false;
    } 
    else if (!use_buf_a && !frame_ready_b) 
    {
        memcpy(mjpeg_buf_b, frame->data, frame->data_bytes);
        frame_len_b = frame->data_bytes;
        frame_ready_b = true;
        use_buf_a = true;
    }
    portEXIT_CRITICAL_ISR(&frameMux);
}

void clientProcessorTask(void *pvParameters) 
{
    stream_client_t* streamClient;
    
    while (true) 
    {
        if (xQueueReceive(clientQueue, &streamClient, portMAX_DELAY) == pdTRUE) 
        {
            extern TaskHandle_t streamTaskHandle[MAX_CLIENTS];
            int taskSlot = -1;
            for (int i = 0; i < MAX_CLIENTS; i++) 
            {
                if (streamTaskHandle[i] == NULL) 
                {
                    taskSlot = i;
                    break;
                }
            }
            
            if (taskSlot != -1) 
            {
                String taskName = "StreamTask" + String(taskSlot);
                xTaskCreatePinnedToCore(
                    stream_task,
                    taskName.c_str(),
                    8192,
                    streamClient,
                    3,
                    &streamTaskHandle[taskSlot],
                    1
                );
            } 
            else 
            {
                streamClient->client.stop();
                delete streamClient;
            }
        }
    }
}

void startStream() 
{
    if(!uvcStarted) 
    {
        uvc->start();
        uvcStarted = true;
    }
    
    if (!streaming_started) 
    {
        clientQueue = xQueueCreate(MAX_CLIENTS, sizeof(stream_client_t*));
        for (int i = 0; i < MAX_CLIENTS; i++) streamTaskHandle[i] = NULL;
        
        xTaskCreatePinnedToCore(clientProcessorTask, "ClientProcessor", 4096, NULL, 3, &clientProcessorHandle, 1);
        streaming_started = true;
    }
}

void stopStream() {
    if (!streaming_started) return;
    
    Serial.println("[CAMERA] Stopping stream");
    
    if(uvcStarted && uvc != nullptr) {
        uvc->stop();
        uvcStarted = false; 
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (streamTaskHandle[i] != NULL) {
            vTaskDelete(streamTaskHandle[i]);
            streamTaskHandle[i] = NULL;
        }
    }
    
    if (clientProcessorHandle != NULL) {
        vTaskDelete(clientProcessorHandle);
        clientProcessorHandle = NULL;
    }
    
    if (clientQueue != NULL) {
        stream_client_t* streamClient;
        while(xQueueReceive(clientQueue, &streamClient, pdMS_TO_TICKS(100)) == pdTRUE) 
        {
            if (streamClient != nullptr) {
                streamClient->client.flush();      // Flush trước
                streamClient->client.stop();
                vTaskDelay(pdMS_TO_TICKS(10));    // Đợi close hoàn toàn
                delete streamClient;
                streamClient = nullptr;
            }
        }
        
        vQueueDelete(clientQueue);
        clientQueue = NULL;
    }
    
    portENTER_CRITICAL(&frameMux);
    frame_ready_a = false;
    frame_ready_b = false;
    use_buf_a = true;
    portEXIT_CRITICAL(&frameMux);
    
    streaming_started = false;
    Serial.println("[CAMERA] Stream stopped");
}