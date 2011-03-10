SOURCES := $(wildcard *.c *.h)
TARGETS := evecho
PUPPET_TARGETS := evecho-32 evecho-64

CFLAGS += -ggdb -Wall -Wextra -pedantic
LDFLAGS += -levent

all: $(TARGETS)

puppet: $(PUPPET_TARGETS)

.PHONY : force
force: clean all

.PHONY : clean 
clean:
	rm -f $(TARGETS) $(PUPPET_TARGETS) $(TEST_TARGETS) *.o
	rm -rf build

evecho-32: $(SOURCES)
	$(CC) -o $@ $(CFLAGS) -m32 $(filter %.c,$^)

evecho-64: $(SOURCES)
	$(CC) -o $@ $(CFLAGS) -m64 $(filter %.c,$^)

dep:
	sh ./automake.sh

#### AUTO ####
#### END AUTO ####
