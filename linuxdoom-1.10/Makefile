################################################################
#
# $Id:$
#
# $Log:$
#
CC = gcc  # gcc or g++

CFLAGS = -std=gnu99 -g -O0 -DNORMALUNIX -DLINUX -Wshadow -DDEBUG_PRESENTATION_TIMING
LDFLAGS = -L/usr/X11R6/lib
LIBS = -lXext -lX11 -lnsl -lm -lwayland-client -lxkbcommon
SANITIZE_FLAGS = -fsanitize=address -fno-ipa-icf -fno-omit-frame-pointer

WL_PROTOCOLS_BASE_PATH := $(shell pkg-config wayland-protocols --variable=pkgdatadir)

# subdirectory for objects
O = linux

# not too sophisticated dependency
OBJS = \
		$(O)/doomdef.o \
		$(O)/doomstat.o \
		$(O)/dstrings.o \
		$(O)/i_system.o \
		$(O)/i_sound.o \
		$(O)/i_video.o \
		$(O)/i_net.o \
		$(O)/tables.o \
		$(O)/f_finale.o \
		$(O)/f_wipe.o \
		$(O)/d_main.o \
		$(O)/d_net.o \
		$(O)/d_items.o \
		$(O)/g_game.o \
		$(O)/m_menu.o \
		$(O)/m_misc.o \
		$(O)/m_argv.o \
		$(O)/m_bbox.o \
		$(O)/m_fixed.o \
		$(O)/m_swap.o \
		$(O)/m_cheat.o \
		$(O)/m_random.o \
		$(O)/am_map.o \
		$(O)/p_ceilng.o \
		$(O)/p_doors.o \
		$(O)/p_enemy.o \
		$(O)/p_floor.o \
		$(O)/p_inter.o \
		$(O)/p_lights.o \
		$(O)/p_map.o \
		$(O)/p_maputl.o \
		$(O)/p_plats.o \
		$(O)/p_pspr.o \
		$(O)/p_setup.o \
		$(O)/p_sight.o \
		$(O)/p_spec.o \
		$(O)/p_switch.o \
		$(O)/p_mobj.o \
		$(O)/p_telept.o \
		$(O)/p_tick.o \
		$(O)/p_saveg.o \
		$(O)/p_user.o \
		$(O)/r_bsp.o \
		$(O)/r_data.o \
		$(O)/r_draw.o \
		$(O)/r_main.o \
		$(O)/r_plane.o \
		$(O)/r_segs.o \
		$(O)/r_sky.o \
		$(O)/r_things.o \
		$(O)/w_wad.o \
		$(O)/wi_stuff.o \
		$(O)/v_video.o \
		$(O)/st_lib.o \
		$(O)/st_stuff.o \
		$(O)/hu_stuff.o \
		$(O)/hu_lib.o \
		$(O)/s_sound.o \
		$(O)/z_zone.o \
		$(O)/info.o \
		$(O)/sounds.o \

.PHONY: all
all: $(O)/linuxxdoom

.PHONY: clean
clean:
	rm -f *.o *~ *.flc
	rm -f linux/*

.PHONY: protocols
protocols:
	$(shell mkdir -p ./wayland_protocols)

	$(shell wayland-scanner client-header \
		< $(WL_PROTOCOLS_BASE_PATH)/stable/xdg-shell/xdg-shell.xml \
		> ./wayland_protocols/xdg-shell.h)
	$(shell wayland-scanner private-code \
		< $(WL_PROTOCOLS_BASE_PATH)/stable/xdg-shell/xdg-shell.xml \
		> ./wayland_protocols/xdg-shell.c)

	$(shell wayland-scanner client-header \
		< $(WL_PROTOCOLS_BASE_PATH)/stable/presentation-time/presentation-time.xml \
		> ./wayland_protocols/presentation-time.h)
	$(shell wayland-scanner private-code \
		< $(WL_PROTOCOLS_BASE_PATH)/stable/presentation-time/presentation-time.xml \
		> ./wayland_protocols/presentation-time.c)

	$(shell wayland-scanner client-header \
		< $(WL_PROTOCOLS_BASE_PATH)/stable/viewporter/viewporter.xml \
		> ./wayland_protocols/viewporter.h)
	$(shell wayland-scanner private-code \
		< $(WL_PROTOCOLS_BASE_PATH)/stable/viewporter/viewporter.xml \
		> ./wayland_protocols/viewporter.c)

$(O)/linuxxdoom: protocols $(OBJS) $(O)/i_main.o
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) $(O)/i_main.o \
	-o $(O)/linuxxdoom $(LIBS)

$(O)/%.o: %.c
	$(CC) $(CFLAGS) -isystem ./wayland_protocols -c $< -o $@

.PHONY: test
test:

#############################################################
#
#############################################################
