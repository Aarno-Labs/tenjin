CC = cc
CFLAGS = -Wall -Wextra

TARGET = program
OBJS = main.o module_one.o module_two.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET)

main.o: source.c
	$(CC) $(CFLAGS) -DMAIN_MODULE -c source.c -o main.o

module_one.o: source.c
	$(CC) $(CFLAGS) -DMODULE_ONE -c source.c -o module_one.o

module_two.o: source.c
	$(CC) $(CFLAGS) -DMODULE_TWO -c source.c -o module_two.o

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
