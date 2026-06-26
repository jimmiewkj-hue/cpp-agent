import sys
from task_store import load_tasks, add_task, delete_task, toggle_task

def print_menu():
    print("\n--- Simple Task Manager ---")
    print("1. List Tasks")
    print("2. Add Task")
    print("3. Toggle Task Completion")
    print("4. Delete Task")
    print("5. Exit")
    print("---------------------------")

def main():
    while True:
        print_menu()
        try:
            choice = input("Choose an option: ").strip()
        except EOFError:
            # Handle non-interactive environments
            print("\nNon-interactive environment detected. Exiting.")
            sys.exit(0)

        if choice == "1":
            tasks = load_tasks()
            if not tasks:
                print("\nNo tasks found.")
            else:
                print("\nYour Tasks:")
                for task in tasks:
                    print(task)
        
        elif choice == "2":
            try:
                title = input("Enter task title: ").strip()
                if not title:
                    print("Title cannot be empty.")
                    continue
                description = input("Enter task description: ").strip()
                add_task(title, description)
                print("Task added successfully.")
            except EOFError:
                break

        elif choice == "3":
            try:
                task_id_str = input("Enter task ID to toggle: ")
                task_id = int(task_id_str)
                if toggle_task(task_id):
                    print("Task updated.")
                else:
                    print("Task not found.")
            except (ValueError, EOFError):
                print("Invalid ID format or input ended.")

        elif choice == "4":
            try:
                task_id_str = input("Enter task ID to delete: ")
                task_id = int(task_id_str)
                if delete_task(task_id):
                    print("Task deleted.")
                else:
                    print("Task not found.")
            except (ValueError, EOFError):
                print("Invalid ID format or input ended.")

        elif choice == "5":
            print("Exiting... Goodbye!")
            sys.exit(0)
        
        else:
            print("Invalid choice, please try again.")

if __name__ == "__main__":
    main()
