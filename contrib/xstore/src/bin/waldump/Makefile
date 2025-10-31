# src/bin/waldump/Makefile

PGFILEDESC = "pg_store_waldump - decode and display WAL"
PGAPPICON=win32

subdir = contrib/xstore/src/bin/waldump
top_builddir = ../../../../../
include $(top_builddir)/src/Makefile.global
	
OBJS = \
	$(RMGRDESCOBJS) \
	$(WIN32RES) \
	xlogreader.o \
	compat.o \
	rmgrdesc_utils.o \
	xlogstats.o \
	waldump.o \
	rmgrdesc.o \
	$(top_srcdir)/contrib/xstore/src/undo/undodesc.o \
	$(top_srcdir)/contrib/xstore/src/xheap/xheapdesc.o \
	$(top_srcdir)/contrib/xstore/src/xbtree/xbtdesc.o \
	$(top_srcdir)/contrib/xstore/src/xbtree/xbtrecycle_desc.o

override CPPFLAGS := -DFRONTEND $(CPPFLAGS) -fPIC -I$(top_srcdir)/contrib/xstore/include
LDFLAGS_INTERNAL += -L$(top_builddir)/src/fe_utils -lpgfeutils $(libpq_pgport)


RMGRDESCSOURCES = $(sort $(notdir $(wildcard $(top_srcdir)/src/backend/access/rmgrdesc/*desc.c)))
RMGRDESCOBJS = $(patsubst %.c,%.o,$(RMGRDESCSOURCES))


all: pg_xstore_waldump

pg_xstore_waldump: $(OBJS) | submake-libpgport
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

compat.c: % : $(top_srcdir)/src/bin/pg_waldump/%
	rm -f $@ && $(LN_S) $< .

xlogreader.c: % : $(top_srcdir)/src/backend/access/transam/%
	rm -f $@ && $(LN_S) $< .

xlogstats.c: % : $(top_srcdir)/src/backend/access/transam/%
	rm -f $@ && $(LN_S) $< .

$(RMGRDESCSOURCES): % : $(top_srcdir)/src/backend/access/rmgrdesc/%
	rm -f $@ && $(LN_S) $< .

rmgrdesc_utils.c: % : $(top_srcdir)/src/backend/access/rmgrdesc/%
	rm -f $@ && $(LN_S) $< .

install: all installdirs
	$(INSTALL_PROGRAM) pg_xstore_waldump$(X) '$(DESTDIR)$(bindir)/pg_xstore_waldump$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/pg_xstore_waldump$(X)'

clean distclean maintainer-clean:
	rm -f pg_xstore_waldump$(X) $(OBJS) $(RMGRDESCSOURCES) xlogreader.c xlogstats.c \
encryption.c compat.c
	rm -rf tmp_check

