# primes.py - Find prime numbers

def is_prime(n):
    if n < 2:
        return False
    if n == 2:
        return True
    if n % 2 == 0:
        return False
    i = 3
    while i * i <= n:
        if n % i == 0:
            return False
        i += 2
    return True

print("Prime numbers up to 100:")
primes = []
for n in range(2, 101):
    if is_prime(n):
        primes = primes + [n]

print(primes)
print("Found", len(primes), "prime numbers")
