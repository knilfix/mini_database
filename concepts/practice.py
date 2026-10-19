# practice.py -- a vim practice file (it is full of bugs on purpose)

import os
import sys

def add(a, b):
    return a + b

def greet(name):
    print("Hello, " + name)

def multiply(x, y):
    result = x * y
    return result

numbers = [1,2, 2, 3, 4, 5]
total = 0
for n in numbers:
    total = total + n
print("Total is", total)



items = ["apple", "banana", "cherry"]
for item in items:
    print(item)
    print("done")
    print("done")

name = "Vim"
greet(name)
