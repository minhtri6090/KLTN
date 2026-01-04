#!/usr/bin/env python3
import os, cv2, json, time, uuid, queue, threading, logging
import numpy as np
import face_recognition
import paho.mqtt.client as mqtt
from datetime import datetime
from firebase_manager import FirebaseManager
from typing import List, Dict, Optional, Tuple
from pathlib import Path

import warnings

cv2.setUseOptimized(True)
cv2.setNumThreads(2)

os.environ['OPENCV_FFMPEG_LOGLEVEL'] = '-8' 
warnings.filterwarnings('ignore')

class MQTTManager:
    def __init__(self, host="camera-monitor.local", port=1883, user="minhtri6090", password="123"):
        self.host, self.port, self.connected = host, port, False
        self.logger = logging.getLogger(__name__)
        
        self.on_motion_callback = None
        
        self.client = mqtt.Client(client_id="rpi-face-recognition", protocol=mqtt.MQTTv311)
        self.client.username_pw_set(user, password)
        self.client.on_connect = self._on_connect
        self.client.on_disconnect = self._on_disconnect
        self.client.on_message = self._on_message
        
        try:
            self.client.max_queued_messages_set(10)
        except Exception: 
            pass

    def _on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            self.connected = True
            self.logger.info("MQTT Connected")
            
            client.subscribe("security/camera/alert", qos=1)
            client.subscribe("security/camera/status", qos=1)
            self.logger.info("Subscribed to ESP32 topics")
        else:
            self.connected = False
            self.logger.error(f"MQTT Connection failed: RC={rc}")

    def _on_disconnect(self, client, userdata, rc):
        self.connected = False
        self.logger.warning(f"MQTT Disconnected (RC={rc})")

    def _on_message(self, client, userdata, msg):
        try:
            topic = msg.topic
            payload = json.loads(msg.payload.decode())
            
            self.logger.info(f"MQTT <- {topic}")
            self.logger.debug(f"   Payload: {payload}")
            
            if topic == "security/camera/alert":
                if payload.get("event") == "motion_detected":
                    self.logger.info("Motion detected signal from ESP32")
                    if self.on_motion_callback:
                        self.on_motion_callback()
                        
        except json.JSONDecodeError:
            self.logger.error(f"Failed to parse MQTT message: {msg.payload}")
        except Exception as e: 
            self.logger.error(f"Error handling MQTT message: {e}")

    def connect(self):
        try:
            self.logger.info(f"Connecting to MQTT broker:  {self.host}:{self.port}")
            self.client.connect(self.host, self.port, 60)
            self.client.loop_start()
        except Exception as e:
            self.logger.error(f"MQTT connection failed: {e}")

    def publish(self, topic, payload):
        if self.connected:
            result = self.client.publish(topic, json.dumps(payload), qos=1)
            if result.rc == mqtt.MQTT_ERR_SUCCESS:
                self.logger.debug(f"MQTT -> {topic}")
            else:
                self.logger.warning(f"Failed to publish to {topic}")

    def close(self):
        try:
            self.client.loop_stop()
            self.client.disconnect()
            self.logger.info("MQTT closed")
        except Exception: 
            pass

class FrameReader(threading.Thread):
    def __init__(self, url, q, reconnect_delay=3.0, max_retries=None):
        super().__init__(daemon=True)
        self.url = url
        self.q = q
        self.running = True
        self.reconnect_delay = reconnect_delay
        self.max_retries = max_retries
        self.logger = logging.getLogger(__name__)
        self.cap = None
        self.retry_count = 0

    def _open_stream(self):
        try:
            if self.cap:
                self.cap.release()
            
            self.logger.info(f"Connecting to stream: {self.url}")
            cap = cv2.VideoCapture(self.url)
            
            if not cap.isOpened():
                self.logger.warning("Failed to open stream")
                return None
            
            try:
                cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*"MJPG"))
            except Exception:
                pass
            cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)
            
            self.logger.info("Stream connected successfully")
            self.retry_count = 0
            return cap
            
        except Exception as e: 
            self.logger.error(f"Error opening stream: {e}")
            return None

    def run(self):
        consecutive_fails = 0
        max_consecutive_fails = 30
        
        while self.running:
            if self.cap is None or not self.cap.isOpened():
                self.retry_count += 1
                
                if self.max_retries and self.retry_count > self.max_retries:
                    self.logger.error("Max retries reached. Stopping.")
                    break
                
                self.logger.warning(f"Attempting reconnect #{self.retry_count}...")
                self.cap = self._open_stream()
                
                if self.cap is None:
                    self.logger.warning(f"Reconnect failed. Retry in {self.reconnect_delay}s")
                    time.sleep(self.reconnect_delay)
                    continue
                
                consecutive_fails = 0
            
            try:
                ok, frame = self.cap.read()
                
                if not ok:
                    consecutive_fails += 1
                    
                    if consecutive_fails >= max_consecutive_fails: 
                        self.logger.warning("Too many consecutive failures.Reconnecting...")
                        if self.cap:
                            self.cap.release()
                        self.cap = None
                        consecutive_fails = 0
                    
                    time.sleep(0.02)
                    continue
                
                consecutive_fails = 0
                
                if not self.q.empty():
                    try:
                        self.q.get_nowait()
                    except queue.Empty:
                        pass
                
                try:
                    self.q.put(frame, block=False)
                except queue.Full:
                    pass
                    
            except Exception as e: 
                self.logger.error(f"Error in frame read loop: {e}")
                consecutive_fails += 1
                time.sleep(0.1)
        
        if self.cap:
            self.cap.release()
        self.logger.info("FrameReader stopped")

    def stop(self):
        self.running = False

class MotionRecorder(threading.Thread):
    def __init__(self, frame_queue, motion_signal, output_dir="/home/camera/recordingss",
                 pre_motion_buffer=10, post_motion_timeout=10, max_storage_gb=50, fb_manager=None):
        super().__init__(daemon=True)
        self.frame_queue = frame_queue
        self.motion_signal = motion_signal
        self.output_dir = Path(output_dir)
        self.pre_motion_buffer_size = pre_motion_buffer * 10  # 10 FPS ? frames
        self.post_motion_timeout = post_motion_timeout
        self.max_storage_bytes = max_storage_gb * 1024 * 1024 * 1024
        self.fb_manager = fb_manager
        self.running = True
        self.logger = logging.getLogger(__name__)
        
        try:
            self.output_dir.mkdir(parents=True, exist_ok=True)
            self.logger.info(f"Recording directory: {self.output_dir}")
        except Exception as e:  
            self.logger.error(f"Failed to create recording directory: {e}")
            raise

    def _cleanup_old_recordings(self):
        try:
            files = sorted(self.output_dir.glob("*.mp4"), key=lambda x: x.stat().st_mtime)
            total_size = sum(f.stat().st_size for f in files)
            
            while total_size > self.max_storage_bytes and files:
                oldest = files.pop(0)
                size = oldest.stat().st_size
                self.logger.info(f"Deleting old recording: {oldest. name} ({size/1024/1024:. 1f} MB)")
                oldest.unlink()
                total_size -= size
                
        except Exception as e:  
            self.logger.error(f"Error cleaning up recordings: {e}")

    def run(self):
        self.logger.info("Motion-triggered recording thread started")
        
        circular_buffer = []
        is_recording = False
        out = None
        filepath = None
        start_time = None
        frame_count = 0
        motion_event_count = 0
        
        while self.running:
            try:
                frame = self.frame_queue.get(timeout=1)
                
                circular_buffer.append(frame.copy())
                if len(circular_buffer) > self.pre_motion_buffer_size:
                    circular_buffer.pop(0)
                
                motion_detected = self.motion_signal.get('detected', False)
                last_motion_time = self.motion_signal.get('last_time', 0)
                time_since_motion = time.time() - last_motion_time
                
                if motion_detected and not is_recording:
                    self.logger.info("Motion detected!  Starting recording...")
                    
                    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
                    filename = f"motion_{timestamp}.mp4"
                    filepath = self.output_dir / filename
                    
                    height, width = frame.shape[:2]
                    fps = 10.0
                    fourcc = cv2.VideoWriter_fourcc(*'mp4v')
                    out = cv2.VideoWriter(str(filepath), fourcc, fps, (width, height))
                    
                    if not out.isOpened():
                        self.logger.error(f"Failed to create video writer:  {filepath}")
                        continue
                    
                    for buffered_frame in circular_buffer:  
                        out.write(buffered_frame)
                        frame_count += 1
                    
                    is_recording = True
                    start_time = time.time()
                    motion_event_count = 1
                    
                    self.logger. info(f"Recording to: {filename} (with {len(circular_buffer)} pre-buffered frames)")
                
                elif motion_detected and is_recording:  
                    motion_event_count += 1
                
                if is_recording:
                    out.write(frame)
                    frame_count += 1

                    if time_since_motion > self.post_motion_timeout:
                        duration = time.time() - start_time
                        out.release()
                        
                        size_mb = filepath. stat().st_size / 1024 / 1024
                        
                        self.logger.info(f"Recording completed: {filepath. name}")
                        self.logger.info(f"   Duration: {duration:.1f}s, Frames: {frame_count}, Size: {size_mb:.1f}MB, Motion events: {motion_event_count}")

                        self._cleanup_old_recordings()
                        
                        is_recording = False
                        out = None
                        frame_count = 0
                        motion_event_count = 0
                        
            except queue.Empty:
                continue
            except Exception as e:  
                self.logger.error(f"Error in motion recorder: {e}")
                if out: 
                    out.release()
                is_recording = False
        
        if out:
            out.release()
        self.logger.info("MotionRecorder stopped")

    def stop(self):
        self.running = False

class StrictMatcher:
    def __init__(self, fb:  FirebaseManager, tol=0.42, margin=0.10):
        self.fb, self.tol, self.margin = fb, tol, margin
        encs = fb.known_face_encodings or []
        self.encs = np.asarray(encs, dtype=np.float32) if encs else np.empty((0,128), np.float32)
        self.enc_users = [fb.face_id_to_user_map[fid] for fid in fb.known_face_ids]
        self.cache = {uid: (fb.get_user(uid) or {}).get("name", uid) for uid in set(self.enc_users)}
        self.enc_norm2 = np.sum(self.encs**2, axis=1) if self.encs.size else np.empty((0,), np.float32)

    def match(self, enc: np.ndarray) -> Tuple[Optional[str], str, float]:
        if self.encs.size == 0:  return None, "Unknown", 0.0
        enc = enc.astype(np.float32)
        dot = self.encs @ enc
        d2 = self.enc_norm2 + float(np.sum(enc**2)) - 2.0*dot
        d = np.sqrt(np.maximum(d2, 0.0))
        per_user:  Dict[str, float] = {}
        for dist, uid in zip(d, self.enc_users):
            v = float(dist)
            if uid not in per_user or v < per_user[uid]: 
                per_user[uid] = v
        items = sorted(per_user.items(), key=lambda x: x[1])
        best_uid, best_d = items[0]
        second_d = items[1][1] if len(items) > 1 else 1.0
        conf = float(1.0 / (1.0 + np.exp(6.0 * (best_d - self.tol))))
        if best_d >= self.tol:  return None, f"Unknown {best_d:.2f}", conf
        if (second_d - best_d) < self.margin: return None, "Ambiguous", conf
        return best_uid, self.cache.get(best_uid, best_uid), conf

class FaceApp:
    def __init__(self, enable_recording=True):
        self.stream = "http://cameraiuh.local/stream"
        self.fb = FirebaseManager()
        self.fb.load_all_faces()
        self.matcher = StrictMatcher(self.fb)
        
        self.motion_signal = {'detected': False, 'last_time': 0.0}
        
        self.mqtt = MQTTManager()
        self.mqtt.on_motion_callback = self._on_motion_detected
        self.mqtt.connect()

        self.q = queue.Queue(maxsize=4)
        
        self.reader = FrameReader(
            self.stream, 
            self.q,
            reconnect_delay=3.0,
            max_retries=None
        )
        self.reader.start()

        self.recorder = None
        if enable_recording:
            self.recorder = MotionRecorder(
                frame_queue=self.q,
                motion_signal=self.motion_signal,
                output_dir="/home/camera/recordingss",
                pre_motion_buffer=10,
                post_motion_timeout=10,
                max_storage_gb=50,
                fb_manager=self.fb
            )
            self.recorder.start()
            logging.info("Motion-triggered recording enabled")

        self.min_face = 48
        self.upsample_fast, self.upsample_boost = 0, 1
        self.boost_interval, self.last_boost = 2.0, 0.0

        self.pub_cooldown, self.last_pub = 1.0, 0.0
        self.last_seen:  Dict[str, float] = {}
        self.min_gap = 5.0

        self.ui_fps, self.t_last = 0.0, time.time()

    def _on_motion_detected(self):
        self.motion_signal['detected'] = True
        self.motion_signal['last_time'] = time.time()
        logging.info("Motion signal received from ESP32 via MQTT")

    def _detect(self, rgb):
        loc = face_recognition.face_locations(rgb, self.upsample_fast, "hog")
        boxes = [(t,r,b,l) for (t,r,b,l) in loc if (r-l)>=self.min_face and (b-t)>=self.min_face]
        if boxes:  return boxes
        if time.time() - self.last_boost >= self.boost_interval:
            loc = face_recognition.face_locations(rgb, self.upsample_boost, "hog")
            boxes = [(t,r,b,l) for (t,r,b,l) in loc if (r-l)>=self.min_face and (b-t)>=self.min_face]
            self.last_boost = time.time()
        return boxes

    def start(self):
        logging.info("Starting face recognition loop")
        no_frame_count = 0
        max_no_frame = 10
        
        try:
            while True:
                try:
                    frame = self.q.get(timeout=2)
                    no_frame_count = 0
                    
                except queue.Empty:
                    no_frame_count += 1
                    
                    if no_frame_count >= max_no_frame: 
                        logging.warning("No frames received. Stream may be disconnected.")
                        no_frame_count = 0
                    
                    blank = np.zeros((480, 640, 3), dtype=np.uint8)
                    cv2.putText(blank, "Waiting for stream...", (150, 240),
                               cv2.FONT_HERSHEY_SIMPLEX, 1.0, (0, 255, 255), 2)
                    cv2.imshow("RPi4 Face Recognition", blank)
                    if cv2.waitKey(100) & 0xFF == ord("q"):
                        break
                    continue

                now = time.time()
                if now - self.t_last > 0: 
                    self.ui_fps = 0.9*self.ui_fps + 0.1*(1.0/(now - self.t_last))
                self.t_last = now

                rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
                boxes = self._detect(rgb)

                if boxes:
                    self.motion_signal['detected'] = True
                    self.motion_signal['last_time'] = time.time()
                else:
                    self.motion_signal['detected'] = False

                if boxes:
                    encs = face_recognition.face_encodings(rgb, boxes, num_jitters=1)
                    for enc, (t,r,b,l) in zip(encs, boxes):
                        uid, label, conf = self.matcher.match(enc)
                        color = (0,255,0) if uid else (0,0,255)
                        cv2.rectangle(frame, (l,t), (r,b), color, 2)
                        cv2.putText(frame, f"{label} ({conf:.2f})", (l+5, b+20),
                                    cv2.FONT_HERSHEY_DUPLEX, 0.6, (255,255,255), 1)
                        if uid: 
                            ts = time.time()
                            if (ts - self.last_seen.get(uid, 0.0) >= self.min_gap) and (ts - self.last_pub >= self.pub_cooldown):
                                self.mqtt.publish("security/camera/family_detected",
                                                  {"user_name": label, "user_id": uid, "confidence": conf})
                                self.last_seen[uid] = ts
                                self.last_pub = ts

                status_color = (0, 255, 0) if self.reader.cap and self.reader.cap.isOpened() else (0, 0, 255)
                status_text = "CONNECTED" if status_color == (0, 255, 0) else "DISCONNECTED"
                
                cv2.putText(frame, f"FPS:{self.ui_fps:.1f}", (10, 30),
                           cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 0), 2)
                cv2.putText(frame, status_text, (10, 60),
                           cv2.FONT_HERSHEY_SIMPLEX, 0.6, status_color, 2)
                
                if self.recorder:
                    if self.motion_signal['detected']:
                        rec_status = "REC"
                        rec_color = (0, 0, 255)
                    else:
                        time_since = time.time() - self.motion_signal['last_time']
                        if time_since < self.recorder.post_motion_timeout:
                            rec_status = f"REC ({int(self.recorder.post_motion_timeout - time_since)}s)"
                            rec_color = (0, 255, 255)
                        else:
                            rec_status = "IDLE"
                            rec_color = (128, 128, 128)
                    
                    cv2.putText(frame, rec_status, (10, 90),
                               cv2.FONT_HERSHEY_SIMPLEX, 0.6, rec_color, 2)
                
                mqtt_color = (0, 255, 0) if self.mqtt.connected else (0, 0, 255)
                mqtt_status = "MQTT:  OK" if self.mqtt.connected else "MQTT: OFF"
                cv2.putText(frame, mqtt_status, (10, 120),
                           cv2.FONT_HERSHEY_SIMPLEX, 0.5, mqtt_color, 1)
                
                cv2.imshow("RPi4 Face Recognition", frame)
                if cv2.waitKey(1) & 0xFF == ord("q"):
                    break

        except KeyboardInterrupt:
            logging.info("Interrupted by user")
        finally:
            self.stop()

    def stop(self):
        logging.info("Stopping all threads...")
        self.reader.stop()
        if self.recorder:
            self.recorder.stop()
        self.mqtt.close()
        cv2.destroyAllWindows()
        logging.info("All threads stopped")

if __name__ == "__main__": 
    logging.basicConfig(
        level=logging.INFO, 
        format="%(asctime)s [%(levelname)s] %(name)s: %(message)s"
    )
    
    app = FaceApp(enable_recording=True)
    app.start()