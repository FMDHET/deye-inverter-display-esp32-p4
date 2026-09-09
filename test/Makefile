# Host tests: `make -C test`
#
# No PlatformIO, no framework, no download -- one compiler and a Makefile, so
# the tests run before anything has been fetched. `pio test` with the native
# platform would be the idiomatic route, but it wants its own platform package
# and would build the ESP env's pre-scripts (which bump the build number).

CC      ?= cc
CFLAGS  ?= -std=gnu11 -g -O1
WARN     = -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
INC      = -I. -Ifakes -I../main
LDLIBS   = -lm
BUILD    = build

# One binary per suite. A suite includes the .c file under test, so two suites
# must never be linked together.
SUITES   = test_modbus_rtu test_webauth
COMMON   = harness.c fakes/fakes.c
HTTP     = fakes/fakes_http.c

BINS = $(addprefix $(BUILD)/,$(SUITES))

.PHONY: all run clean
all: run

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/test_modbus_rtu: test_modbus_rtu.c $(COMMON) | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(INC) -o $@ $^ $(LDLIBS)

$(BUILD)/test_webauth: test_webauth.c $(COMMON) $(HTTP) | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(INC) -o $@ $^ $(LDLIBS)

run: $(BINS)
	@echo "Host-Tests:"
	@fail=0; for b in $(BINS); do ./$$b || fail=1; done; \
	 if [ $$fail -eq 0 ]; then echo "  alle Suiten bestanden"; else \
	 echo "  FEHLGESCHLAGEN"; exit 1; fi

clean:
	rm -rf $(BUILD)
