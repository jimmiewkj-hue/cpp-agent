import unittest
import os
import json
from task_manager_logic import TaskManager

class TestTaskManager(unittest.TestCase):
    def setUp(self):
        self.test_file = 'test_tasks.json'
        self.manager = TaskManager(storage_file=self.test_file)

    def tearDown(self):
        if os.path.exists(self.test_file):
            os.remove(self.test_file)

    def test_add_task(self):
        task = self.manager.add_task("Test Task")
        self.assertEqual(task['description'], "Test Task")
        self.assertEqual(len(self.manager.list_tasks()), 1)

    def test_list_tasks(self):
        self.manager.add_task("Task 1")
        self.manager.add_task("Task 2")
        tasks = self.manager.list_tasks()
        self.assertEqual(len(tasks), 2)
        self.assertEqual(tasks[0]['description'], "Task 1")
        self.assertEqual(tasks[1]['description'], "Task 2")

    def test_complete_task(self):
        task = self.manager.add_task("Complete me")
        task_id = task['id']
        success = self.manager.complete_task(task_id)
        self.assertTrue(success)
        self.assertTrue(self.manager.list_tasks()[0]['completed'])

    def test_delete_task(self):
        self.manager.add_task("Task 1")
        self.manager.add_task("Task 2")
        task_id = 1
        success = self.manager.delete_task(task_id)
        self.assertTrue(success)
        self.assertEqual(len(self.manager.list_tasks()), 1)
        self.assertEqual(self.manager.list_tasks()[0]['description'], "Task 2")

    def test_persistence(self):
        self.manager.add_task("Persistent Task")
        # Create a new manager instance with the same file
        new_manager = TaskManager(storage_file=self.test_file)
        tasks = new_manager.list_tasks()
        self.assertEqual(len(tasks), 1)
        self.assertEqual(tasks[0]['description'], "Persistent Task")

if __name__ == '__main__':
    unittest.main()
