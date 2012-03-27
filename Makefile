SOURCES := $(wildcard *.c)
TARGETS := evecho timesend loopsend

CFLAGS += -ggdb -Wall -Wextra -pedantic --std=c99
LDFLAGS += -levent -lrt

all: $(TARGETS)

evecho: evecho.o connection.o
	$(CC) -o $@ $(CFLAGS) $(LDFLAGS) $^

loopsend: loopsend.o sendlib.o

timesend: timesend.o sendlib.o

.PHONY : force
force: clean all

.PHONY : clean 
clean:
	rm -f $(TARGETS) $(TEST_TARGETS) *.o

dep:
	sh ./automake.sh

#### AUTO ####
connection.o: connection.c connection.h debugs.h
evecho.o: evecho.c debugs.h connection.h
loopsend.o: loopsend.c sendlib.h
sendlib.o: sendlib.c
timesend.o: timesend.c sendlib.h
#### END AUTO ####
