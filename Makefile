CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -O2 -g
PKG_CONFIG ?= pkg-config
GST_FLAGS := $(shell $(PKG_CONFIG) --cflags gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 gobject-2.0 glib-2.0)
GST_LIBS := $(shell $(PKG_CONFIG) --libs gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 gobject-2.0 glib-2.0)
GTK_FLAGS := $(shell $(PKG_CONFIG) --cflags gtk4)
GTK_LIBS := $(shell $(PKG_CONFIG) --libs gtk4)

PREFIX ?= /usr/local
DESTDIR ?=
INSTALL ?= install
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share
APPLICATIONSDIR ?= $(DATADIR)/applications
DESKTOP_FILE := data/udp-h265-viewer.desktop

INCLUDES := -Iinclude
SRCS := \
	src/main.c \
	src/viewer_core.c \
	src/relay_controller.c \
	src/pipeline_builder.c \
	src/stats.c \
	src/logging.c \
	src/gui_shell.c

OBJS := $(SRCS:.c=.o)
TARGET := udp-h265-viewer

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(GST_LIBS) $(GTK_LIBS) -lm

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) $(GST_FLAGS) $(GTK_FLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

install: $(TARGET) $(DESKTOP_FILE)
	$(INSTALL) -d "$(DESTDIR)$(BINDIR)"
	$(INSTALL) -m 0755 $(TARGET) "$(DESTDIR)$(BINDIR)/$(TARGET)"
	$(INSTALL) -d "$(DESTDIR)$(APPLICATIONSDIR)"
	$(INSTALL) -m 0644 $(DESKTOP_FILE) "$(DESTDIR)$(APPLICATIONSDIR)/udp-h265-viewer.desktop"

.PHONY: all clean install
