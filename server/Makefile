CC = gcc
CFLAGS = -Wall -Wextra -pthread -g
TARGET = server

all: $(TARGET)

$(TARGET): src/server.c
	$(CC) $(CFLAGS) -o $(TARGET) src/server.c

clean:
	rm -f $(TARGET) *.o
