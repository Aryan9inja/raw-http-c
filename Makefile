CC = gcc

TARGET = server

SRCS = server.c httpParser.c handlers.c
OBJS = $(SRCS:.c=.o)

all: dev

dev: CFLAGS = -Wall -Wextra -g
dev: $(TARGET)

prod: CFLAGS =
prod: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

run: $(TARGET)
	./$(TARGET)