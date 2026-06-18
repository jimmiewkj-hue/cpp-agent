from calculator import Calculator

def test_calculator():
    calc = Calculator()
    
    # Test addition
    assert calc.add(2, 3) == 5, "Addition failed"
    
    # Test subtraction
    assert calc.subtract(10, 4) == 6, "Subtraction failed"
    
    # Test multiplication
    assert calc.multiply(3, 4) == 12, "Multiplication failed"
    
    print("All tests passed!")

if __name__ == "__main__":
    test_calculator()
