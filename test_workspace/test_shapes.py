from shapes import Circle, Rectangle

# Create a Circle with radius 3
circle = Circle(3)
print(f"Circle (radius=3):")
print(f"  Area:     {circle.area():.2f}")
print(f"  Perimeter: {circle.perimeter():.2f}")
print()

# Create a Rectangle with width 4 and height 5
rectangle = Rectangle(4, 5)
print(f"Rectangle (width=4, height=5):")
print(f"  Area:     {rectangle.area():.2f}")
print(f"  Perimeter: {rectangle.perimeter():.2f}")
