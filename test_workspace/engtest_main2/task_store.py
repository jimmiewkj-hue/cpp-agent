import json
import os
from task import Task

STORAGE_FILE = "tasks.json"

def load_tasks():
    if not os.path.exists(STORAGE_FILE):
        return []
    try:
        with open(STORAGE_FILE, "r") as f:
            data = json.load(f)
            return [Task.from_dict(item) for item in data]
    except (json.JSONDecodeError, IOError):
        return []

def save_tasks(tasks):
    try:
        with open(STORAGE_FILE, "w") as f:
            json.dump([task.to_dict() for task in tasks], f, indent=4)
    except IOError as e:
        print(f"Error saving tasks: {e}")

def add_task(title, description=""):
    tasks = load_tasks()
    task_id = max([t.id for t in tasks], default=0) + 1
    new_task = Task(task_id, title, description)
    tasks.append(new_task)
    save_tasks(tasks)
    return new_task

def delete_task(task_id):
    tasks = load_tasks()
    updated_tasks = [t for t in tasks if t.id != task_id]
    if len(tasks) == len(updated_tasks):
        return False
    save_tasks(updated_tasks)
    return True

def toggle_task(task_id):
    tasks = load_tasks()
    for t in tasks:
        if t.id == task_id:
            t.completed = not t.completed
            save_tasks(tasks)
            return True
    return False
