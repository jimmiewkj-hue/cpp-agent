from task_manager_logic import TaskManager

class TaskCLI:
    def __init__(self):
        self.manager = TaskManager()

    def run(self):
        print("Welcome to the Simple Task Manager!")
        print("Commands: add <desc>, list, complete <id>, delete <id>, quit")
        
        while True:
            try:
                user_input = input("\n> ").strip()
                if not user_input:
                    continue
                
                parts = user_input.split(maxsplit=1)
                command = parts[0].lower()
                args = parts[1] if len(parts) > 1 else None

                if command == 'quit':
                    print("Goodbye!")
                    break
                elif command == 'add':
                    if args:
                        self.manager.add_task(args)
                        print(f"Task added: {args}")
                    else:
                        print("Error: Please provide a task description.")
                elif command == 'list':
                    tasks = self.manager.list_tasks()
                    if not tasks:
                        print("No tasks found.")
                    else:
                        for task in tasks:
                            status = "[x]" if task['completed'] else "[ ]"
                            print(f"{task['id']}. {status} {task['description']}")
                elif command == 'complete':
                    if args and args.isdigit():
                        if self.manager.complete_task(int(args)):
                            print(f"Task {args} marked as completed.")
                        else:
                            print(f"Error: Task {args} not found.")
                    else:
                        print("Error: Please provide a valid task ID.")
                elif command == 'delete':
                    if args and args.isdigit():
                        if self.manager.delete_task(int(args)):
                            print(f"Task {args} deleted.")
                        else:
                            print(f"Error: Task {args} not found.")
                    else:
                        print("Error: Please provide a valid task ID.")
                else:
                    print("Unknown command. Available commands: add, list, complete, delete, quit")
            except EOFError:
                break
            except Exception as e:
                print(f"An unexpected error occurred: {e}")
