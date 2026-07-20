# Host unit-test build for libhelp parsers (task #267).
# Compiles the parser sources with the NATIVE host gcc (not freestanding) so we
# can prove the parsing logic without a VM. help_ui.c is excluded because it
# depends on the MayteraOS GUI syscalls.

CC = gcc
CFLAGS = -g -O0 -Wall -Wextra -I..

SRCS = test_parsers.c \
       ../help_doc.c ../mhlp.c ../hlp.c ../help_open.c ../help_search.c

TARGET = run_tests

all: $(TARGET)

$(TARGET): $(SRCS) ../help.h ../help_internal.h
	$(CC) $(CFLAGS) -o $@ $(SRCS)

# Build the .HLP test fixture, run all parsers, dump output.
test: $(TARGET) ../../../HELP/SYSTEM.MHLP fixture.hlp
	./$(TARGET) ../../../HELP/SYSTEM.MHLP fixture.hlp

# Generate a tiny synthetic WinHelp file to exercise the .HLP reader.
fixture.hlp: make_fixture
	./make_fixture fixture.hlp

make_fixture: make_fixture.c
	$(CC) $(CFLAGS) -o $@ make_fixture.c

clean:
	rm -f $(TARGET) make_fixture fixture.hlp

.PHONY: all test clean
