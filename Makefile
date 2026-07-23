all: clean build run

build: encodeit

encodeit:
	gcc -g -o encodeit encodeit.c -I include

# Runtime argument defaults — override any of these on the command line, e.g.:
#   make run SEED=42 NINSTRS=50 NTHREADS=4 LOGFILE=/tmp/run.log
SEED     ?= 0
NINSTRS  ?= 25
NTHREADS ?= 1
LOGFILE  ?=

# Build the argument list dynamically so LOGFILE is only appended when set
RUN_ARGS := $(SEED) $(NINSTRS) $(NTHREADS)
ifneq ($(LOGFILE),)
    RUN_ARGS += $(LOGFILE)
endif

run: build
	./encodeit $(RUN_ARGS)

gdb: build
	gdb -q ./encodeit -ex 'set follow-fork-mode child' -ex 'br executeit' -ex 'run $(RUN_ARGS)' -ex 'x/60ai start_addr'

.PHONY: all build run gdb clean

clean:
	rm -f encodeit
