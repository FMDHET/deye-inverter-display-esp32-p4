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

# One binary per suite. A suite #includes the .c file under test, so two suites
# must never be linked together.
SUITES = test_modbus_rtu test_webauth test_deye_ctrl

OBJ_COMMON = $(BUILD)/harness.o $(BUILD)/fakes/fakes.o
OBJ_HTTP   = $(BUILD)/fakes/fakes_http.o
OBJ_RTU    = $(BUILD)/fakes/fakes_rtu.o

BINS = $(addprefix $(BUILD)/,$(SUITES))

.PHONY: all run clean
all: run

# -MMD -MP writes a .d file listing every header AND every .c pulled in with
# #include. Without it make never sees that ../main/modbus_rtu.c changed --
# the suite would happily re-run a stale binary and report all green over a
# broken firmware. That happened once during development; hence this comment.
$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(WARN) $(INC) -MMD -MP -c -o $@ $<

$(BUILD)/test_modbus_rtu: $(BUILD)/test_modbus_rtu.o $(OBJ_COMMON)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/test_webauth: $(BUILD)/test_webauth.o $(OBJ_COMMON) $(OBJ_HTTP)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/test_deye_ctrl: $(BUILD)/test_deye_ctrl.o $(OBJ_COMMON) $(OBJ_RTU)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

run: $(BINS)
	@echo "Host-Tests:"
	@fail=0; for b in $(BINS); do ./$$b || fail=1; done; \
	 if [ $$fail -eq 0 ]; then echo "  alle Suiten bestanden"; else \
	 echo "  FEHLGESCHLAGEN"; exit 1; fi

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
