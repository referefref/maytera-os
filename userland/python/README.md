# Python for MayteraOS

This directory contains the Python 3 interpreter implementation for MayteraOS.

## Overview

MayteraOS includes a minimal but functional Python interpreter that supports:

- Variables, expressions, and basic arithmetic
- Strings with concatenation and indexing
- Lists with append and indexing
- Control structures: if/elif/else, while, for...in
- Function definitions with def
- Built-in functions: print(), len(), range(), str(), int(), input(), open()
- File I/O operations
- Comments with #

## User-Space Python Interpreter

The main Python interpreter is built as a user-space application in:
`/opt/maytera/userland/apps/python/`

### Building

```bash
cd /opt/maytera/userland/apps/python
make
```

This produces the `python` executable.

### Usage

Interactive REPL:
```bash
./python
>>> print("Hello World")
Hello World
>>> x = 5
>>> print(x * 2)
10
>>> exit()
```

Execute a script:
```bash
./python hello.py
```

Execute code directly:
```bash
./python -c "print('Hello')"
```

## MicroPython Port

The `micropython/ports/maytera/` directory contains a port of MicroPython for MayteraOS
with the following custom modules:

### os Module

```python
import os

# File operations
fd = os.open("/path/to/file", os.O_RDONLY)
data = os.read(fd, 1024)
os.write(fd, b"data")
os.close(fd)

# Directory operations
os.mkdir("/new_dir")
os.listdir("/path")

# Process operations
pid = os.getpid()
os.chdir("/path")
cwd = os.getcwd()
```

### socket Module

```python
import socket

# Create TCP client
sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.connect(("10.0.0.1", 80))
sock.send(b"GET / HTTP/1.0\r\n\r\n")
data = sock.recv(1024)
sock.close()
```

### maytera Module

```python
import maytera

# Create a window
win = maytera.Window("My App", 100, 100, 400, 300)

# Drawing
win.draw_rect(10, 10, 100, 50, 0x00FF0000)  # Red rectangle
win.draw_text(20, 20, "Hello", 0x00FFFFFF)  # White text
win.invalidate()

# Event handling
while True:
    event = win.get_event(100)  # 100ms timeout
    if event:
        if event['type'] == maytera.EVENT_WINDOW_CLOSE:
            break

win.close()

# System functions
maytera.sleep(1000)  # Sleep 1 second
pid = maytera.getpid()
ticks = maytera.ticks()
```

## Example Scripts

See the `examples/` directory for sample Python scripts:

- `hello.py` - Simple hello world
- `fibonacci.py` - Calculate Fibonacci numbers
- `primes.py` - Find prime numbers
- `interactive.py` - Interactive input demo

## Limitations

The current minimal interpreter has some limitations:

1. No exception handling (try/except)
2. No classes (class keyword recognized but not fully implemented)
3. No import of external modules
4. Limited standard library
5. Single-threaded only

The MicroPython port (when fully built) removes most of these limitations.
