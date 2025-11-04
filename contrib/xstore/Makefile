# contrib/xstore/Makefile

MODULE_big = xstore
OBJS = src/undo/undorecord.o  src/undo/undofetch.o \
	src/undo/undobuffer.o src/undo/undofile.o \
	src/undo/undolog.o  src/undo/undoxlog.o \
	src/undo/undoworker.o src/undo/undorequest.o \
	src/xstore.o src/util/xxact.o src/undo/undodesc.o src/xheap/xheapdesc.o \
	src/xbtree/xbtdesc.o src/xbtree/xbtrecycle_desc.o \
	src/xheap/xheapam.o src/xheap/xpage.o src/xheap/xtuple.o src/xheap/xtuptoaster.o \
	src/xheap/xredo.o src/xheap/prunexheap.o src/xheap/xscan.o src/xheap/xhio.o src/xheap/xvacuumlazy.o \
	src/xheap/xtupleslot.o src/xheap/xheapam_handler.o src/xheap/xdecode.o \
	src/xheap/xheapam_visibility.o src/xheap/xstat.o \
	src/xbtree/xbtreefun.o \
	src/xbtree/xbtrecycle.o src/xbtree/xbtree.o src/xbtree/xbtundo.o  \
	src/xbtree/xbtpage.o  src/xbtree/xbtxlog.o src/xbtree/xbtvisibility.o \
	src/xbtree/xbtsplitloc.o src/xbtree/xbtsort.o  \
	src/xbtree/xbtinsert.o src/xbtree/xbtsearch.o src/xbtree/xbtutils.o \
	src/xheap/xmulti.o src/xheap/xlru.o src/xheap/xheapundo.o  src/xheap/xlock.o \
	src/xheap/xcluster.o src/xheap/xheapfun.o


EXTENSION = xstore
DATA = xstore--1.0.sql


REGRESS = init test_setup boolean char name varchar text int2 int4 int8 oid float4 float8 bit numeric txid money pg_lsn regproc \
	numerology lseg line path circle date time timetz timestamp timestamptz interval \
	horology tstypes regex type_sanity misc_sanity comments unicode copy copyselect \
	create_function_c create_misc create_operator create_procedure create_table create_type create_schema \
	create_view create_aggregate create_function_sql create_cast \
	typed_table drop_if_exists roleattributes hash_func errors infinite_recurse random \
	select_into select_distinct_on select_implicit select_having delete namespace init_privs security_label \
	collate matview lock object_address drop_operator password alter_generic alter_operator async dbsize sysviews \
	tsrf tid tidrangescan create_role psql_crosstab subscription \
	portals_p2 dependency guc bitmapops tsdicts foreign_data xmlmap functional_deps advisory_lock \
	json json_encoding jsonpath jsonpath_encoding jsonb_jsonpath plancache temp rangefuncs prepare conversion truncate \
	sequence polymorphism largeobject xml hash_part partition_aggregate tuplesort explain oidjoins collate.linux.utf8 collate.icu.utf8 \
	enum copydml fast_default create_index create_table_like amutils btree_index case alter_table arrays cluster copy2 domain equivclass \
	event_trigger expressions foreign_key generated groupingsets identity incremental_sort indexing indirect_toast insert_conflict insert \
	index_including join_hash jsonb tidscan transactions tsearch  updatable_views update \
	limit memoize misc_functions misc mvcc partition_info plpgsql portals  privileges psql publication \
	reloptions replica_identity returning rules rowtypes vacuum_parallel vacuum with write_parallel \
	stats stats_ext strings subselect tablespace select select_distinct select_views tablesample \
	rowsecurity partition_join partition_prune inherit aggregates merge window


REGRESS_OPTS = --dlpath=../../src/test/regress --xstorecheck


ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
override PG_CPPFLAGS += -I$(CURDIR)/include
include $(PGXS)
else
subdir = contrib/xstore
top_builddir = ../..
override PG_CPPFLAGS += -I$(top_srcdir)/$(subdir)/include
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk

SUBDIRS += src/bin/waldump

$(recurse)
$(recurse_always)
endif

##
## Regression Clean up
##

clean-xstore :
	rm -rf $(pg_regress_clean_files)

