CC := clang
CFLAGS := -Wall -Wextra -Werror -pedantic -std=c23 -O2

.PHONY: all
all: arena.so arena.a

arena.o: arena.h
	$(CC) $(CFLAGS) -x c -c arena.h -o arena.o -DARENA_IMPLEMENTATION

arena.so: arena.o
	$(CC) -shared -o arena.so arena.o

arena.a: arena.o
	ar rcs arena.a arena.o

example: example.c arena.h
	$(CC) $(CFLAGS) example.c -o example

.PHONY: test
test: example
	./example

.PHONY: clean
clean:
	rm -f arena.o arena.so arena.a example
