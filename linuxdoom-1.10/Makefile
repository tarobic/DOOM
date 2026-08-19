################################################################
#
# $Id:$
#
# $Log:$
#
CC = gcc

CPPFLAGS := -DNORMALUNIX -DLINUX -DDEBUG_PRESENTATION_TIMING -DDEBUG
CFLAGS = -std=gnu99 -g -O0 -Wshadow
LDFLAGS = -L/usr/X11R6/lib
LDLIBS = -lXext -lX11 -lnsl -lm -lwayland-client -lxkbcommon
SANITIZE_FLAGS = -fsanitize=address -fno-ipa-icf -fno-omit-frame-pointer # todo

BUILD_DIR := ./linux
$(shell mkdir -p $(BUILD_DIR))

PROTOCOLS_BASE_PATH := $(shell pkg-config wayland-protocols --variable=pkgdatadir)
PROTOCOL_NAMES := xdg-shell presentation-time viewporter fractional-scale-v1 content-type-v1
PROTOCOL_DIR := ./wayland_protocols

get_protocol_abs_path = $(shell find $(PROTOCOLS_BASE_PATH) -name $(1).xml)

extract_file_name = $(notdir $(basename $(1)))

generate = $(shell wayland-scanner client-header < $(1) > $(PROTOCOL_DIR)/$(2).h \
		   && wayland-scanner private-code < $(1) > $(PROTOCOL_DIR)/$(2).c)

# I hate makefiles bro how did we get here
evaluate_protocol = $(call generate,$(1),$(call extract_file_name,$(1),),)

$(PROTOCOL_NAMES): $(shell mkdir -p $(PROTOCOL_DIR))
	@echo generating $@
	$(call evaluate_protocol,$(call get_protocol_abs_path,$@,),)
	$(CC) $(CFLAGS) -isystem $(PROTOCOL_DIR) -c $(PROTOCOL_DIR)/$@.c -o $(BUILD_DIR)/$@.o $(LDLIBS)

.PHONY: protocols
protocols: $(PROTOCOL_NAMES)

OBJECTS := $(patsubst %.c,%.o,$(wildcard *.c))

$(OBJECTS): %.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -isystem $(PROTOCOL_DIR) -c $< -o $(BUILD_DIR)/$@ $(LDLIBS) $(LDFLAGS)

.PHONY: all
all: $(BUILD_DIR)/linuxxdoom

$(BUILD_DIR)/linuxxdoom: protocols $(OBJECTS)
	@echo $(PROTOCOL_OBJECTS)
	$(CC) $(CPPFLAGS) -isystem $(PROTOCOL_DIR) $(BUILD_DIR)/*.o -o $@ $(LDLIBS) $(LDFLAGS)

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)
	rm -rf $(PROTOCOL_DIR)

.PHONY: test
test:

#############################################################
#
#############################################################
