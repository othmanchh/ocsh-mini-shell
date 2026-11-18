CC = gcc
CFLAGS = -Wall -Wextra -std=c99
TARGET = ocsh

all: $(TARGET)

$(TARGET): code/ocsh.c
	$(CC) $(CFLAGS) -o $(TARGET) code/ocsh.c

clean:
	rm -f $(TARGET)

.PHONY: all clean
