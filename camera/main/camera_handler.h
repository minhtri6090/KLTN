#ifndef CAMERA_HANDLER_H
#define CAMERA_HANDLER_H

#include "config.h"

extern USB_STREAM* uvc;
extern bool uvcStarted;

extern uint8_t* mjpeg_buf_a;
extern uint8_t* mjpeg_buf_b;
extern volatile size_t frame_len_a;
extern volatile size_t frame_len_b;
extern volatile bool frame_ready_a;
extern volatile bool frame_ready_b;
extern volatile bool use_buf_a;
extern portMUX_TYPE frameMux;

extern uint8_t* payload_buf_a;
extern uint8_t* payload_buf_b;
extern uint8_t* frame_buf;
extern uint32_t frame_cnt_recv;
extern uint32_t frame_cnt_sent;

void initializeBuffers();
void initializeCamera();
void frame_cb(uvc_frame_t* frame, void*);
void startStream();
void stopStream();
void clientProcessorTask(void *pvParameters);


#endif