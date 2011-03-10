SOURCES := $(wildcard *.c)
OBJECTS := $(SOURCES:.c=.o)
TARGETS := evecho
PUPPET_TARGETS := evecho-32 evecho-64

CFLAGS += -ggdb -Wall -Wextra -pedantic --std=c99
LDFLAGS += -levent

all: $(TARGETS)

evecho: $(OBJECTS)
	$(CC) -o $@ $(CFLAGS) $(LDFLAGS) $^

puppet: $(PUPPET_TARGETS)

.PHONY : force
force: clean all

.PHONY : clean 
clean:
	rm -f $(TARGETS) $(PUPPET_TARGETS) $(TEST_TARGETS) *.o

dep:
	sh ./automake.sh

#### AUTO ####
connection.o: connection.c connection.h debugs.h
evecho.o: evecho.c debugs.h connection.h
#### END AUTO ####
