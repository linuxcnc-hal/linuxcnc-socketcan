HALCOMPILE ?= halcompile
RAW_COMPONENT := lsc_socketcan
CANOPEN_COMPONENT := lsc_canopen
COMPILE_ARGS := -std=gnu11 -Wall -Wextra
CANOPEN_LINK_ARGS := -lexpat -lm

.PHONY: all build check install clean

all: build

build:
	cd src && $(HALCOMPILE) --compile --userspace \
		--extra-compile-args="$(COMPILE_ARGS)" $(RAW_COMPONENT).c
	cd src && $(HALCOMPILE) --compile --userspace \
		--extra-compile-args="$(COMPILE_ARGS)" \
		--extra-link-args="$(CANOPEN_LINK_ARGS)" $(CANOPEN_COMPONENT).c

check: build

install:
	cd src && $(HALCOMPILE) --install --userspace \
		--extra-compile-args="$(COMPILE_ARGS)" $(RAW_COMPONENT).c
	cd src && $(HALCOMPILE) --install --userspace \
		--extra-compile-args="$(COMPILE_ARGS)" \
		--extra-link-args="$(CANOPEN_LINK_ARGS)" $(CANOPEN_COMPONENT).c

clean:
	rm -f src/$(RAW_COMPONENT) src/$(CANOPEN_COMPONENT) src/*.o src/*.d src/*.tmp
