CC = cc
CFLAGS = -Wall -DNDEBUG

program: main.o
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o program

.PHONY: clean
