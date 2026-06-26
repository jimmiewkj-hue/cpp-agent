class Task:
    def __init__(self, task_id, title, description="", completed=False):
        self.id = task_id
        self.title = title
        self.description = description
        self.completed = completed

    def to_dict(self):
        return {
            "id": self.id,
            "title": self.title,
            "description": self.description,
            "completed": self.completed
        }

    @classmethod
    def from_dict(cls, data):
        return cls(
            task_id=data["id"],
            title=data["title"],
            description=data.get("description", ""),
            completed=data.get("completed", False)
        )

    def __str__(self):
        status = "[X]" if self.completed else "[ ]"
        return f"{self.id}. {status} {self.title} - {self.description}"
