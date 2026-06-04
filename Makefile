
all: build

build: encodeit

encodeit:
	gcc -g -o encodeit encodeit.c -I include

run: build
	./encodeit

gdb: build
	gdb -q ./encodeit -ex 'br encodeit.c:100' -ex run -ex 'x/40ai mptr'

.PHONY: clean
.PHONY: all build run gdb

clean:
	rm -f encodeit
