# contrib/query_check/Makefile
MODULE_big = query_check
OBJS = \
	$(WIN32RES) \
	query_check.o

EXTENSION = query_check
DATA = query_check--1.0.sql \
	query_check--1.0--1.1.sql
PGFILEDESC = "query_check - checking the query type"

LDFLAGS_SL += $(filter -lm, $(LIBS))

REGRESS = query_check_test

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/query_check
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
