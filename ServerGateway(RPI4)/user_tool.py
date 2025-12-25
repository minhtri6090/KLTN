#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
import sys
from typing import List, Optional

from firebase_manager import FirebaseManager


class UserTool:
    def __init__(self) -> None:
        self.fm = FirebaseManager()

    @staticmethod
    def _prompt_nonempty(label: str) -> str:
        while True: 
            s = input(label).strip()
            if s:
                return s
            print("Value cannot be empty.")

    @staticmethod
    def _confirm(prompt: str) -> bool:
        ans = input(f"{prompt} [y/N]:  ").strip().lower()
        return ans == "y"

    def _choose_user(self, users: List[dict]) -> Optional[dict]:
        if not users:
            print("No users found.")
            return None
        print("\nUsers:")
        for i, u in enumerate(users, 1):
            print(f"{i: 2d}. {u. get('name','<no-name>')}  | id: {u.get('id')}  | email: {u.get('email','')}")
        sel = input("Select number (or empty to cancel): ").strip()
        if not sel or not sel.isdigit():
            return None
        idx = int(sel) - 1
        if 0 <= idx < len(users):
            return users[idx]
        print("Invalid selection.")
        return None

    def add_user(self) -> None:
        print("\n=== Add New User ===")
        name = self._prompt_nonempty("Name:  ")
        email = self._prompt_nonempty("Email: ")
        try:
            user_id = self.fm.add_user(name, email)
            print(f"Created:  {name}  (id: {user_id})")
        except Exception as e:
            print(f"Error creating user: {e}")
            return

        if self._confirm("Add face images now?"):
            self.add_face_images(user_id, name)

    def add_face_images(self, user_id: str, user_name: str) -> None:
        print(f"\n=== Add Face Images for {user_name} ===")
        print("Enter image paths, one per line.  Empty line to finish.")
        paths:  List[str] = []
        while True:
            p = input(f"Image {len(paths)+1}: ").strip()
            if not p:
                break
            if os.path.exists(p):
                paths.append(p)
                print(f"  + {p}")
            else:
                print("  ! File not found.")

        if not paths:
            print("No images added.")
            return

        try:
            self.fm.upload_face_images(user_id, paths)
            self.fm.load_all_faces()
            print(f"Uploaded {len(paths)} images and reloaded face data.")
        except Exception as e: 
            print(f"Error uploading images: {e}")

    def list_users(self) -> None:
        print("\n=== All Users ===")
        try:
            users = self.fm.get_all_users()
            if not users:
                print("No users found.")
                return
            for u in users:
                print(f"- {u.get('name','<no-name>')} | id: {u.get('id')} | email: {u.get('email','')}"
                      f" | images: {u.get('total_images',0)} | created: {u.get('created_at','N/A')}")
        except Exception as e:
            print(f"Error listing users: {e}")

    def delete_user(self, user_id: Optional[str] = None) -> None:
        try:
            if not user_id:
                users = self.fm.get_all_users()
                u = self._choose_user(users)
                if not u: 
                    print("Canceled.")
                    return
                user_id = u. get("id")
                name = u.get("name", "<no-name>")
            else:
                match = next((u for u in self.fm.get_all_users() if u.get("id") == user_id), None)
                name = match.get("name", "<no-name>") if match else "<no-name>"

            if not user_id:
                print("Invalid user id.")
                return

            if not self._confirm(f"Delete user '{name}' (id: {user_id})?  This cannot be undone."):
                print("Canceled.")
                return

            self. fm.delete_user(user_id)
            print(f"Deleted user '{name}' (id: {user_id}).")
            try:
                self.fm.load_all_faces()
            except Exception: 
                pass

        except AttributeError:
            print("FirebaseManager. delete_user(user_id) is not implemented.  Please add it.")
        except Exception as e:
            print(f"Error deleting user: {e}")

    def interactive_menu(self) -> None:
        while True:
            print("\n=== FIREBASE USER TOOL ===")
            print("1) Add new user")
            print("2) List users")
            print("3) Delete user")
            print("0) Exit")
            try:
                choice = input("Select:  ").strip()
                if choice == "1":
                    self.add_user()
                elif choice == "2": 
                    self.list_users()
                elif choice == "3": 
                    uid = input("Enter user id (leave empty to choose from list): ").strip() or None
                    self.delete_user(uid)
                elif choice == "0": 
                    print("Bye.")
                    break
                else: 
                    print("Invalid choice.")
            except KeyboardInterrupt:
                print("\nBye.")
                break


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Firebase User Management Tool")
    sub = p.add_subparsers(dest="cmd")

    sub.add_parser("add", help="Add new user interactively")
    sub.add_parser("list", help="List all users")

    p_del = sub.add_parser("delete", help="Delete a user by id (or interactive if omitted)")
    p_del.add_argument("--id", type=str, default=None)

    return p


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()
    tool = UserTool()

    if args.cmd == "add": 
        tool.add_user()
    elif args.cmd == "list":
        tool.list_users()
    elif args.cmd == "delete":
        tool.delete_user(user_id=args.id)
    else:
        tool.interactive_menu()


if __name__ == "__main__": 
    main()