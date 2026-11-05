.POSIX:
include config.mk

all: linkbrarian lbr tests/httpd

linkbrarian: linkbrarian.o
	$(CC) $(LDFLAGS) -o $@ linkbrarian.o $(LIBS)

linkbrarian.o: linkbrarian.c config.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ linkbrarian.c

lbr: linkbrarian
	cp linkbrarian lbr

tests/httpd: tests/httpd.o
	$(CC) $(LDFLAGS) -o $@ tests/httpd.o

tests/httpd.o: tests/httpd.c
	$(CC) $(CFLAGS) -c -o $@ tests/httpd.c

check: all
	sh tests/run.sh

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp linkbrarian $(DESTDIR)$(PREFIX)/bin/linkbrarian
	cp lbr $(DESTDIR)$(PREFIX)/bin/lbr

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/linkbrarian
	rm -f $(DESTDIR)$(PREFIX)/bin/lbr

clean:
	rm -f linkbrarian lbr linkbrarian.o tests/httpd tests/httpd.o

.PHONY: all check install uninstall clean
