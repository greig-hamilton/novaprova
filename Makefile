
prefix=		/usr/local
includedir=	$(prefix)/include
libdir=		$(prefix)/lib

CC=		gcc
CDEBUGFLAGS=	-g
COPTFLAGS=	-O0
CDEFINES=	-D_GNU_SOURCE
CWARNFLAGS=	-Wall -Wextra
CFLAGS=		$(CDEBUGFLAGS) $(COPTFLAGS) $(CWARNFLAGS) $(CDEFINES)
INSTALL=	/usr/bin/install -c
RANLIB=		ranlib

all: libu4c.a

libu4c_SOURCE=	\
		u4c.c common.c run.c textl.c proxyl.c discover.c \
		isyslog.c iassert.c icunit.c iexit.c
libu4c_PRIVHEADERS= \
		common.h u4c_priv.h
libu4c_HEADERS=	u4c.h
libu4c_OBJS=	$(libu4c_SOURCE:.c=.o)

libu4c.a: $(libu4c_OBJS)
	$(AR) $(ARFLAGS) libu4c.a $(libu4c_OBJS)

install: all
	$(INSTALL) -d $(DESTDIR)$(includedir)
	for hdr in $(libu4c_HEADERS) ; do \
	    $(INSTALL) -m 644 $$hdr $(DESTDIR)$(includedir)/$$hdr ;\
	done
	$(INSTALL) -d $(DESTDIR)$(libdir)
	$(INSTALL) -m 644 libu4c.a $(DESTDIR)$(libdir)/libu4c.a
	$(RANLIB) $(DESTDIR)$(libdir)/libu4c.a

clean:
	$(RM) libu4c.a $(libu4c_OBJS)

check: all
	cd tests ; $(MAKE) $@

