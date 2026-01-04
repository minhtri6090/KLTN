#!/usr/bin/env python3
import os
import uuid
import logging
import numpy as np
import face_recognition
from datetime import datetime
from typing import List, Dict, Optional

import firebase_admin
from firebase_admin import credentials, db, storage


class FirebaseManager:
    
    def __init__(self, service_account_path: str = "serviceAccountKey.json"):
        self.logger = logging.getLogger(__name__)
        
        try:
            if not firebase_admin._apps:
                cred = credentials.Certificate(service_account_path)
                firebase_admin.initialize_app(cred, {
                    "databaseURL": "https://camera-monitor-328ea-default-rtdb.asia-southeast1.firebasedatabase.app/",
                    "storageBucket": "camera-monitor-328ea.firebasestorage.app"
                })
        except ValueError:  
            pass
        
        self.db = db
        self.bucket = storage.bucket()
        
        self.known_face_encodings:  List[np.ndarray] = []
        self.known_face_ids: List[str] = []
        self.face_id_to_user_map: Dict[str, str] = {}
        
        self.logger.info("Firebase initialized successfully")

    def add_user(self, name:  str, email: str) -> str:
        """Add new user to Firebase"""
        user_id = str(uuid.uuid4())
        
        self.db.reference(f"users/{user_id}").set({
            "name": name,
            "email":  email,
            "created_at": datetime.now().isoformat(),
            "is_active":  True,
            "total_images": 0
        })
        
        self.logger.info(f"User added: {name} (ID: {user_id})")
        return user_id

    def delete_user(self, user_id: str, *, delete_images=True):
        """Delete user and optionally their images"""
        if not user_id:
            raise ValueError("user_id is required")
        
        if not self.db.reference(f"users/{user_id}").get():
            self.logger.warning(f"User not found: {user_id}")
            return
        
        # Delete images from Storage
        if delete_images: 
            blobs = self.bucket.list_blobs(prefix=f"faces/{user_id}/")
            for blob in blobs:
                blob.delete()
                self.logger.debug(f"Deleted blob: {blob.name}")
        
        # Delete user from Database
        self.db.reference(f"users/{user_id}").delete()
        self.logger.info(f"User deleted: {user_id}")
        
        # Reload face data
        self.load_all_faces()

    def get_user(self, user_id: str) -> Optional[Dict]:
        """Get single user by ID"""
        data = self.db.reference(f"users/{user_id}").get()
        
        if data and data.get("is_active", True):
            data["id"] = user_id
            return data
        
        return None

    def get_all_users(self) -> List[Dict]:
        """Get all active users"""
        data = self.db.reference("users").get()
        
        if not data:
            return []
        
        users = [
            dict(val, id=uid) 
            for uid, val in data.items() 
            if val.get("is_active", True)
        ]
        
        return users

    def upload_face_images(self, user_id: str, image_paths: List[str]) -> bool:
        """Upload face images and extract encodings"""
        if not self.db.reference(f"users/{user_id}").get():
            raise ValueError("User not found")
        
        encodings = []
        success_count = 0
        
        for i, path in enumerate(image_paths):
            if not os.path.exists(path):
                self.logger.warning(f"Image not found:  {path}")
                continue
            
            # Load image
            img = face_recognition.load_image_file(path)
            
            # Detect faces (try fast first, then boost)
            locs = face_recognition.face_locations(img, number_of_times_to_upsample=1, model="hog")
            
            if not locs: 
                locs = face_recognition.face_locations(img, number_of_times_to_upsample=2, model="hog")
            
            if not locs:  
                self.logger.warning(f"No faces detected in:  {path}")
                continue
            
            # Select largest face
            areas = [(bottom - top) * (right - left) for (top, right, bottom, left) in locs]
            largest_face_loc = locs[int(np.argmax(areas))]
            
            # Extract encoding
            enc = face_recognition.face_encodings(img, [largest_face_loc], num_jitters=1)
            
            if not enc:
                self.logger.warning(f"Failed to encode face in: {path}")
                continue
            
            # Upload image to Storage
            blob_path = f"faces/{user_id}/face_{i+1}.jpg"
            self.bucket.blob(blob_path).upload_from_filename(path)
            
            encodings.append(enc[0].tolist())
            success_count += 1
            
            self.logger.info(f"Uploaded face image {i+1}:  {path}")
        
        if success_count == 0:
            raise ValueError("No valid face images found")
        
        # Save encodings to Database
        self.db.reference(f"users/{user_id}/face_data").set({
            "face_encodings": encodings,
            "total_images": success_count,
            "updated_at": datetime.now().isoformat()
        })
        
        # Update user total_images
        self.db.reference(f"users/{user_id}/total_images").set(success_count)
        
        self.logger.info(f"Successfully uploaded {success_count} face images for user {user_id}")
        return True

    def load_all_faces(self):
        """Load all face encodings into memory for fast recognition"""
        self.known_face_encodings = []
        self.known_face_ids = []
        self.face_id_to_user_map = {}
        
        for user in self.get_all_users():
            user_id = user["id"]
            
            data = self.db.reference(f"users/{user_id}/face_data").get()
            
            if not data or not data.get("face_encodings"):
                continue
            
            for i, enc in enumerate(data["face_encodings"]):
                encoding_array = np.array(enc, dtype=np.float32)
                
                if encoding_array.shape != (128,):
                    self.logger.warning(f"Invalid encoding shape for user {user_id}, face {i+1}")
                    continue
                
                self.known_face_encodings.append(encoding_array)
                
                face_id = f"{user_id}_{i+1}"
                self.known_face_ids.append(face_id)
                
                self.face_id_to_user_map[face_id] = user_id
        
        self.logger.info(f"Loaded {len(self.known_face_encodings)} face encodings from {len(set(self.face_id_to_user_map.values()))} users")
