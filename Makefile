CC := gcc
PKG_CONFIG := pkg-config
CFLAGS = -Wall -g `$(PKG_CONFIG) --cflags glib-2.0`
LIBS = `$(PKG_CONFIG) --libs glib-2.0` -lcinet

PREFIX := /usr

all: libciclient.so.1.0

libciclient.so.1.0: ci-client.h ci-client.c
	$(CC) -I. $(CFLAGS) -fPIC -c -o ciclient.o ci-client.c
	$(CC) -shared -Wl,-soname,libciclient.so.1 -o libciclient.so.1.0 ciclient.o $(LIBS)

install: libciclient.so.1.0
	install libciclient.so.1.0 $(PREFIX)/lib/
	ln -sf $(PREFIX)/lib/libciclient.so.1.0 $(PREFIX)/lib/libciclient.so.1
	ln -sf $(PREFIX)/lib/libciclient.so.1 $(PREFIX)/lib/libciclient.so
	cp ci-client.h $(PREFIX)/include

clean:
	$(RM) libciclient.so.1.0 ciclient.o
	
.PHONY: all clean install
