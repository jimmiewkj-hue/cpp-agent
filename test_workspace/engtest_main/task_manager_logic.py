import json
import os

class TaskManager:
    def __init__(self, storage_file='tasks.json'):
        self.storage_file = storage_file
        self.tasks = self.load_tasks()

    def load_tasks(self):
        if os.path.exists(self.storage_file):
            try:
                with open(self.storage_file, 'r') as f:
                    return json.load(f)
            except (json.JSONDecodeError, IOError):
                return []
        return []

    def save_tasks(self):
        try:
            with open(self.storage_file, 'w') as f:
                json.dump(self.tasks, f, indent=4)
        except IOError as e:
            print(f"Error saving tasks: {e}")

    def add_task(self, description):
        task = {
            'id': len(self.tasks) + 1,
            'description': description,
            'completed': False
        }
        self.tasks.append(task)
        self.save_tasks()
        return task

    def list_tasks(self):
        return self.tasks

    def complete_task(self, task_id):
        for task in self.tasks:
            if task['id'] == task_id:
                task['completed'] = True
                self.save_tasks()
                return True
        return False

    def delete_task(self, task_id):
        for i, task in enumerate(self.tasks):
            if task['id'] == task_id:
                del self.tasks[i]
                # Re-index tasks to keep IDs sequential (optional but cleaner for this simple app)
                for j, t in enumerate(self.tasks):
                    t['id'] = j + 1
                self.save_tasks()
                return True
        return False
