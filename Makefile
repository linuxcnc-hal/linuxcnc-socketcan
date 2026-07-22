HALCOMPILE ?= halcompile
COMPONENT := lsc_socketcan
SOURCE := $(COMPONENT).c
COMPILE_ARGS := -std=gnu11 -Wall -Wextra

.PHONY: all build check install clean

all: build

build:
	cd src && $(HALCOMPILE) --compile --userspace \
		--extra-compile-args=$(COMPILE_ARGS) $(SOURCE)

check: build

install:
	cd src && $(HALCOMPILE) --install --userspace \
		--extra-compile-args=$(COMPILE_ARGS) $(SOURCE)

clean:
	rm -f src/$(COMPONENT) src/*.o src/*.d src/*.tmp

