import os
from task import Task
from task_store import load_tasks, add_task, delete_task, toggle_task, STORAGE_FILE

def test_task_manager():
    # Clean up previous tests
    if os.path.exists(STORAGE_FILE):
        os.remove(STORAGE_FILE)

    print("Testing Add Task...")
    t1 = add_task("Test Task 1", "Description 1")
    t2 = add_task("Test Task 2", "Description 2")
    tasks = load_tasks()
    assert len(tasks) == 2
    assert tasks[0].title == "Test Task 1"
    assert tasks[1].id == 2
    print("Add Task: PASSED")

    print("Testing Toggle Task...")
    toggle_task(1)
    tasks = load_tasks()
    assert tasks[0].completed is True
    assert tasks[1].completed is False
    print("Toggle Task: PASSED")

    print("Testing Delete Task...")
    delete_task(1)
    tasks = load_tasks()
    assert len(tasks) == 1
    assert tasks[0].id == 2
    print("Delete Task: PASSED")

    print("Testing Non-existent Task...")
    assert toggle_task(99) is False
    assert delete_task(99) is False
    print("Non-existent Task: PASSED")

    print("\nAll tests passed successfully!")

if __name__ == "__main__":
    test_task_manager()
