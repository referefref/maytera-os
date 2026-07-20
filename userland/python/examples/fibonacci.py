# fibonacci.py - Calculate Fibonacci numbers

def fib(n):
    if n <= 1:
        return n
    a = 0
    b = 1
    i = 2
    while i <= n:
        c = a + b
        a = b
        b = c
        i += 1
    return b

print("Fibonacci sequence:")
for i in range(10):
    print("fib(", i, ") =", fib(i))
