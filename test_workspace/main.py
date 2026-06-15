from calculator import Calculator
from shapes import Circle, Rectangle

def main():
    calc = Calculator()
    print('Add:', calc.add(10, 5))
    print('Sub:', calc.subtract(10, 5))

    c = Circle(5)
    print('Circle area:', c.area())

    r = Rectangle(4, 6)
    print('Rect area:', r.area())

if __name__ == '__main__':
    main()
