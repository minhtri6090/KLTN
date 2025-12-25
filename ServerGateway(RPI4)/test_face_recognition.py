#!/usr/bin/env python3
import cv2, json, time, queue, threading, logging
import numpy as np
import face_recognition
import paho.mqtt.client as mqtt
from datetime import datetime
from firebase_manager import FirebaseManager
from typing import Dict, Optional, Tuple

cv2.setUseOptimized(True)
cv2.setNumThreads(2)

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
            
            # ? Subscribe to ESP32 motion detection
            client.subscribe("security/esp32/motion", qos=1)
            self.logger.info("Subscribed to ESP32 motion topic")
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
            
            # ? Handle ESP32 motion detection
            if topic == "security/esp32/motion":
                if payload.get("state") == "detected":
                    self.logger.info("?? Motion detected from ESP32 LD2410C")
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
                    self.logger.error("Max retries reached.Stopping.")
                    break
                
                self.logger.warning(f"Attempting reconnect #{self.retry_count}...")
                self.cap = self._open_stream()
                
                if self.cap is None:
                    self.logger.warning(f"Reconnect failed.Retry in {self.reconnect_delay}s")
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
    def __init__(self):
        self.stream = "http://cameraiuh.local/stream"
        self.fb = FirebaseManager()
        self.fb.load_all_faces()
        self.matcher = StrictMatcher(self.fb)
        
        self.motion_signal = {
            'detected': False, 
            'last_time': 0.0,
            'source': None  # 'esp32' or 'face_detected'
        }
        
        self.mqtt = MQTTManager()
        self.mqtt.on_motion_callback = self._on_motion_detected
        self.mqtt.connect()

        self.q = queue.Queue(maxsize=2)
        
        self.reader = FrameReader(
            self.stream, 
            self.q,
            reconnect_delay=3.0,
            max_retries=None
        )
        self.reader.start()

        self.min_face = 48
        self.upsample_fast, self.upsample_boost = 0, 1
        self.boost_interval, self.last_boost = 2.0, 0.0

        self.pub_cooldown, self.last_pub = 1.0, 0.0
        self.last_seen:  Dict[str, float] = {}
        self.min_gap = 5.0

        self.ui_fps, self.t_last = 0.0, time.time()

    def _on_motion_detected(self):
        """Called when ESP32 LD2410C detects motion"""
        self.motion_signal['detected'] = True
        self.motion_signal['last_time'] = time.time()
        self.motion_signal['source'] = 'esp32'
        logging.info("?? Motion from ESP32 LD2410C")

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

                # ? Face detection triggers motion signal (low priority)
                if boxes:
                    if self.motion_signal['source'] != 'esp32' or \
                       (time.time() - self.motion_signal['last_time']) > 5.0:
                        self.motion_signal['detected'] = True
                        self.motion_signal['last_time'] = time.time()
                        self.motion_signal['source'] = 'face_detected'
                
                # Motion expires after 10s
                if (time.time() - self.motion_signal['last_time']) > 10:
                    self.motion_signal['detected'] = False
                    self.motion_signal['source'] = None

                if boxes:
                    encs = face_recognition.face_encodings(rgb, boxes, num_jitters=1)
                    for enc, (t,r,b,l) in zip(encs, boxes):
                        uid, label, conf = self.matcher.match(enc)
                        color = (0,255,0) if uid else (0,0,255)
                        cv2.rectangle(frame, (l,t), (r,b), color, 2)
                        cv2.putText(frame, f"{label} ({conf:.2f})", (l+5, b+20),
                                    cv2.FONT_HERSHEY_DUPLEX, 0.6, (255,255,255), 1)
                        
                        # ? Publish family detection to ESP32
                        if uid:  
                            ts = time.time()
                            if (ts - self.last_seen.get(uid, 0.0) >= self.min_gap) and \
                               (ts - self.last_pub >= self.pub_cooldown):
                                
                                self.mqtt.publish("security/camera/family_detected", {
                                    "user_name": label, 
                                    "user_id": uid, 
                                    "confidence":  conf,
                                    "timestamp": datetime.now().isoformat()
                                })
                                
                                self.last_seen[uid] = ts
                                self.last_pub = ts
                                logging.info(f"?? Family detected: {label} (conf:  {conf:.2f})")

                # ? UI Display
                status_color = (0, 255, 0) if self.reader.cap and self.reader.cap.isOpened() else (0, 0, 255)
                status_text = "CONNECTED" if status_color == (0, 255, 0) else "DISCONNECTED"
                
                cv2.putText(frame, f"FPS:{self.ui_fps:.1f}", (10, 30),
                           cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 0), 2)
                cv2.putText(frame, status_text, (10, 60),
                           cv2.FONT_HERSHEY_SIMPLEX, 0.6, status_color, 2)
                
                # Motion status
                if self.motion_signal['detected']:
                    source = self.motion_signal['source'] or ''
                    motion_text = f"MOTION ({source})"
                    motion_color = (0, 0, 255)
                else:
                    motion_text = "IDLE"
                    motion_color = (128, 128, 128)
                
                cv2.putText(frame, motion_text, (10, 90),
                           cv2.FONT_HERSHEY_SIMPLEX, 0.6, motion_color, 2)
                
                # MQTT status
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
        self.mqtt.close()
        cv2.destroyAllWindows()
        logging.info("All threads stopped")

if __name__ == "__main__":  
    logging.basicConfig(
        level=logging.INFO, 
        format="%(asctime)s [%(levelname)s] %(name)s: %(message)s"
    )
    
    app = FaceApp()
    app.start()