CC = cc
CFLAGS = -Wall -Wextra

all: foo

foo: main.o libdistinct.so
	$(CC) main.o libdistinct.so -o foo -L. -Wl,-rpath,.

main.o: main.c
	$(CC) $(CFLAGS) -c main.c -o main.o

libdistinct.so: lib.c
	$(CC) $(CFLAGS) -shared -fPIC -o libdistinct.so lib.c

clean:
	rm -f main.o libdistinct.so foo

.PHONY: all clean
