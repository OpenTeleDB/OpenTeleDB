-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION xstore" to load this file. \quit

--
-- create table AM
--
CREATE FUNCTION xheap_tableam_handler(internal)
  RETURNS table_am_handler
  AS 'MODULE_PATHNAME', 'xheap_tableam_handler'
  LANGUAGE C IMMUTABLE STRICT;

CREATE ACCESS METHOD xstore TYPE TABLE HANDLER xheap_tableam_handler;

--
-- create xbtree AM
--
CREATE FUNCTION xbthandler(internal)
  RETURNS index_am_handler
  AS 'MODULE_PATHNAME', 'xbthandler'
  LANGUAGE C IMMUTABLE STRICT;

CREATE ACCESS METHOD xbtree TYPE INDEX HANDLER xbthandler;

--
-- xbtree opfamily
--
CREATE OPERATOR FAMILY array_ops USING xbtree;
CREATE OPERATOR FAMILY bit_ops USING xbtree;
CREATE OPERATOR FAMILY bool_ops USING xbtree;
CREATE OPERATOR FAMILY bpchar_ops USING xbtree;
CREATE OPERATOR FAMILY bytea_ops USING xbtree;
CREATE OPERATOR FAMILY char_ops USING xbtree;
CREATE OPERATOR FAMILY datetime_ops USING xbtree;
CREATE OPERATOR FAMILY float_ops USING xbtree;
CREATE OPERATOR FAMILY network_ops USING xbtree;
CREATE OPERATOR FAMILY integer_ops USING xbtree;
CREATE OPERATOR FAMILY interval_ops USING xbtree;
CREATE OPERATOR FAMILY macaddr_ops USING xbtree;
CREATE OPERATOR FAMILY macaddr8_ops USING xbtree;
CREATE OPERATOR FAMILY numeric_ops USING xbtree;
CREATE OPERATOR FAMILY oid_ops USING xbtree;
CREATE OPERATOR FAMILY oidvector_ops USING xbtree;
CREATE OPERATOR FAMILY record_ops USING xbtree;
CREATE OPERATOR FAMILY record_image_ops USING xbtree;
CREATE OPERATOR FAMILY text_ops USING xbtree;
CREATE OPERATOR FAMILY time_ops USING xbtree;
CREATE OPERATOR FAMILY timetz_ops USING xbtree;
CREATE OPERATOR FAMILY varbit_ops USING xbtree;
CREATE OPERATOR FAMILY text_pattern_ops USING xbtree;
CREATE OPERATOR FAMILY bpchar_pattern_ops USING xbtree;
CREATE OPERATOR FAMILY money_ops USING xbtree;
CREATE OPERATOR FAMILY tid_ops USING xbtree;
CREATE OPERATOR FAMILY xid8_ops USING xbtree;
CREATE OPERATOR FAMILY uuid_ops USING xbtree;
CREATE OPERATOR FAMILY pg_lsn_ops USING xbtree;
CREATE OPERATOR FAMILY enum_ops USING xbtree;
CREATE OPERATOR FAMILY tsvector_ops USING xbtree;
CREATE OPERATOR FAMILY tsquery_ops USING xbtree;
CREATE OPERATOR FAMILY range_ops USING xbtree;
CREATE OPERATOR FAMILY jsonb_ops USING xbtree;
CREATE OPERATOR FAMILY multirange_ops USING xbtree;

--
-- xbtree opclass and amproc
--
CREATE OPERATOR CLASS array_ops
DEFAULT FOR TYPE anyarray USING xbtree
FAMILY array_ops
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    FUNCTION        1       btarraycmp(anyarray,anyarray),
STORAGE         anyarray;

CREATE OPERATOR CLASS bit_ops
DEFAULT FOR TYPE bit USING xbtree
FAMILY bit_ops
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    FUNCTION        1       bitcmp(bit,bit),
    FUNCTION        4       btequalimage(oid),
STORAGE         bit;

CREATE OPERATOR CLASS bool_ops
DEFAULT FOR TYPE bool USING xbtree
FAMILY bool_ops
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    FUNCTION        1       btboolcmp(bool,bool),
    FUNCTION        4       btequalimage(oid),
STORAGE         bool;

CREATE OPERATOR CLASS bpchar_ops
DEFAULT FOR TYPE bpchar USING xbtree
FAMILY bpchar_ops
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    FUNCTION        1       bpcharcmp(bpchar,bpchar),
    FUNCTION        2       bpchar_sortsupport(internal),
    FUNCTION        4       btvarstrequalimage(oid),
STORAGE         bpchar;

CREATE OPERATOR CLASS bytea_ops
DEFAULT FOR TYPE bytea USING xbtree
FAMILY bytea_ops
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    FUNCTION        1       byteacmp(bytea,bytea),
    FUNCTION        2       bytea_sortsupport(internal),
    FUNCTION        4       btequalimage(oid),
STORAGE         bytea;

CREATE OPERATOR CLASS char_ops
DEFAULT FOR TYPE "char" USING xbtree
FAMILY char_ops
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    FUNCTION        1       btcharcmp("char","char"),
    FUNCTION        4       btequalimage(oid),
STORAGE         "char";

CREATE OPERATOR CLASS cidr_ops
FOR TYPE inet USING xbtree
FAMILY network_ops
AS
--    OPERATOR        1       <,
--    FUNCTION        1       network_cmp(inet,inet),
STORAGE         inet;

CREATE OPERATOR CLASS date_ops
DEFAULT FOR TYPE date USING xbtree
FAMILY datetime_ops
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    OPERATOR        1       <(date,timestamp),
    OPERATOR        2       <=(date,timestamp),
    OPERATOR        3       =(date,timestamp),
    OPERATOR        4       >=(date,timestamp),
    OPERATOR        5       >(date,timestamp),
    OPERATOR        1       <(date,timestamptz),
    OPERATOR        2       <=(date,timestamptz),
    OPERATOR        3       =(date,timestamptz),
    OPERATOR        4       >=(date,timestamptz),
    OPERATOR        5       >(date,timestamptz),
    FUNCTION        1       date_cmp(date,date),
    FUNCTION        2       date_sortsupport(internal),
    FUNCTION        4       btequalimage(oid),
    FUNCTION        1       date_cmp_timestamp(date,timestamp),
    FUNCTION        1       date_cmp_timestamptz(date,timestamptz),
    FUNCTION        3       pg_catalog.in_range(date,date,interval,bool,bool),
STORAGE         date;

CREATE OPERATOR CLASS float4_ops
DEFAULT FOR TYPE float4 USING xbtree
FAMILY float_ops
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    OPERATOR        1       <(float4,float8),
    OPERATOR        2       <=(float4,float8),
    OPERATOR        3       =(float4,float8),
    OPERATOR        4       >=(float4,float8),
    OPERATOR        5       >(float4,float8),
    FUNCTION        1       btfloat4cmp(float4,float4),
    FUNCTION        1       btfloat48cmp(float4,float8),
    FUNCTION        2       btfloat4sortsupport(internal),
    FUNCTION        3       pg_catalog.in_range(float4,float4,float8,bool,bool),
STORAGE         float4;

CREATE OPERATOR CLASS float8_ops
DEFAULT FOR TYPE float8 USING xbtree
FAMILY float_ops
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    OPERATOR        1       <(float8,float4),
    OPERATOR        2       <=(float8,float4),
    OPERATOR        3       =(float8,float4),
    OPERATOR        4       >=(float8,float4),
    OPERATOR        5       >(float8,float4),
    FUNCTION        1       btfloat8cmp(float8,float8),
    FUNCTION        1       btfloat84cmp(float8,float4),
    FUNCTION        2       btfloat8sortsupport(internal),
    FUNCTION        3       pg_catalog.in_range(float8,float8,float8,bool,bool),
STORAGE         float8;

CREATE OPERATOR CLASS inet_ops
DEFAULT FOR TYPE inet USING xbtree
FAMILY network_ops
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    FUNCTION        1       network_cmp(inet,inet),
    FUNCTION        2       network_sortsupport(internal),
    FUNCTION        4       btequalimage(oid),
STORAGE         inet;

CREATE OPERATOR CLASS int2_ops
DEFAULT FOR TYPE int2 USING xbtree
FAMILY integer_ops
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    OPERATOR        1       <(int2,int8),
    OPERATOR        2       <=(int2,int8),
    OPERATOR        3       =(int2,int8),
    OPERATOR        4       >=(int2,int8),
    OPERATOR        5       >(int2,int8),
    OPERATOR        1       <(int2,int4),
    OPERATOR        2       <=(int2,int4),
    OPERATOR        3       =(int2,int4),
    OPERATOR        4       >=(int2,int4),
    OPERATOR        5       >(int2,int4),
    FUNCTION        1       btint28cmp(int2,int8),
    FUNCTION        3       pg_catalog.in_range(int2,int2,int8,bool,bool),
    FUNCTION        1       btint2cmp(int2,int2),
    FUNCTION        2       btint2sortsupport(internal),
    FUNCTION        3       pg_catalog.in_range(int2,int2,int2,bool,bool),
    FUNCTION        4       btequalimage(oid),
    FUNCTION        1       btint24cmp(int2,int4),
    FUNCTION        3       pg_catalog.in_range(int2,int2,int4,bool,bool),
STORAGE         int2;

CREATE OPERATOR CLASS int4_ops
DEFAULT FOR TYPE int4 USING xbtree
FAMILY integer_ops
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    OPERATOR        1       <(int4,int8),
    OPERATOR        2       <=(int4,int8),
    OPERATOR        3       =(int4,int8),
    OPERATOR        4       >=(int4,int8),
    OPERATOR        5       >(int4,int8),
    OPERATOR        1       <(int4,int2),
    OPERATOR        2       <=(int4,int2),
    OPERATOR        3       =(int4,int2),
    OPERATOR        4       >=(int4,int2),
    OPERATOR        5       >(int4,int2),
    FUNCTION        1       btint48cmp(int4,int8),
    FUNCTION        3       pg_catalog.in_range(int4,int4,int8,bool,bool),
    FUNCTION        1       btint42cmp(int4,int2),
    FUNCTION        3       pg_catalog.in_range(int4,int4,int2,bool,bool),
    FUNCTION        1       btint4cmp(int4,int4),
    FUNCTION        2       btint4sortsupport(internal),
    FUNCTION        3       pg_catalog.in_range(int4,int4,int4,bool,bool),
    FUNCTION        4       btequalimage(oid),
STORAGE         int4;

CREATE OPERATOR CLASS int8_ops
DEFAULT FOR TYPE int8 USING xbtree
FAMILY integer_ops
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    OPERATOR        1       <(int8,int2),
    OPERATOR        2       <=(int8,int2),
    OPERATOR        3       =(int8,int2),
    OPERATOR        4       >=(int8,int2),
    OPERATOR        5       >(int8,int2),
    OPERATOR        1       <(int8,int4),
    OPERATOR        2       <=(int8,int4),
    OPERATOR        3       =(int8,int4),
    OPERATOR        4       >=(int8,int4),
    OPERATOR        5       >(int8,int4),
    FUNCTION        1       btint8cmp(int8,int8),
    FUNCTION        2       btint8sortsupport(internal),
    FUNCTION        3       pg_catalog.in_range(int8,int8,int8,bool,bool),
    FUNCTION        4       btequalimage(oid),
    FUNCTION        1       btint82cmp(int8,int2),
    FUNCTION        1       btint84cmp(int8,int4),
STORAGE         int8;

CREATE OPERATOR CLASS interval_ops
DEFAULT FOR TYPE interval USING xbtree
FAMILY interval_ops
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    FUNCTION        1       interval_cmp(interval,interval),
    FUNCTION        3       pg_catalog.in_range(interval,interval,interval,bool,bool),
    FUNCTION        4       btequalimage(oid),
STORAGE         interval;

CREATE OPERATOR CLASS macaddr_ops
DEFAULT FOR TYPE macaddr USING xbtree
FAMILY macaddr_ops
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    FUNCTION        1       macaddr_cmp(macaddr,macaddr),
    FUNCTION        2       macaddr_sortsupport(internal),
    FUNCTION        4       btequalimage(oid),
STORAGE         macaddr;

CREATE OPERATOR CLASS macaddr8_ops
DEFAULT FOR TYPE macaddr8 USING xbtree
FAMILY macaddr8_ops
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    FUNCTION        1       macaddr8_cmp(macaddr8,macaddr8),
    FUNCTION        4       btequalimage(oid),
STORAGE         macaddr8;

CREATE OPERATOR CLASS name_ops
DEFAULT FOR TYPE name USING xbtree
FAMILY text_ops
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    OPERATOR        1       <(name,text),
    OPERATOR        2       <=(name,text),
    OPERATOR        3       =(name,text),
    OPERATOR        4       >=(name,text),
    OPERATOR        5       >(name,text),
    FUNCTION        1       btnamecmp(name,name),
    FUNCTION        2       btnamesortsupport(internal),
    FUNCTION        4       btvarstrequalimage(oid),
    FUNCTION        1       btnametextcmp(name,text),
STORAGE         cstring;

CREATE OPERATOR CLASS numeric_ops
DEFAULT FOR TYPE numeric USING xbtree
FAMILY numeric_ops
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    FUNCTION        1       numeric_cmp(numeric,numeric),
    FUNCTION        2       numeric_sortsupport(internal),
    FUNCTION        3       pg_catalog.in_range(numeric,numeric,numeric,bool,bool),
STORAGE         numeric;

CREATE OPERATOR CLASS oid_ops
DEFAULT FOR TYPE oid USING xbtree
FAMILY oid_ops
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    FUNCTION        1       btoidcmp(oid,oid),
    FUNCTION        2       btoidsortsupport(internal),
    FUNCTION        4       btequalimage(oid),
STORAGE         oid;

CREATE OPERATOR CLASS oidvector_ops
DEFAULT FOR TYPE oidvector USING xbtree
FAMILY oidvector_ops
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    FUNCTION        1       btoidvectorcmp(oidvector,oidvector),
    FUNCTION        4       btequalimage(oid),
STORAGE         oidvector;

CREATE OPERATOR CLASS record_ops
DEFAULT FOR TYPE record USING xbtree
FAMILY record_ops
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    FUNCTION        1       btrecordcmp(record,record),
STORAGE         record;

CREATE OPERATOR CLASS record_image_ops
FOR TYPE record USING xbtree
FAMILY record_image_ops
AS
    OPERATOR        1       *<,
    OPERATOR        2       *<=,
    OPERATOR        3       *=,
    OPERATOR        4       *>=,
    OPERATOR        5       *>,
    FUNCTION        1       btrecordimagecmp(record,record),
STORAGE         record;

CREATE OPERATOR CLASS text_ops
DEFAULT FOR TYPE text USING xbtree
FAMILY text_ops
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    OPERATOR        1       <(text,name),
    OPERATOR        2       <=(text,name),
    OPERATOR        3       =(text,name),
    OPERATOR        4       >=(text,name),
    OPERATOR        5       >(text,name),
    FUNCTION        1       bttextnamecmp(text,name),
    FUNCTION        1       bttextcmp(text,text),
    FUNCTION        2       bttextsortsupport(internal),
    FUNCTION        4       btvarstrequalimage(oid),
STORAGE         text;

CREATE OPERATOR CLASS time_ops
DEFAULT FOR TYPE time USING xbtree
FAMILY time_ops
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    FUNCTION        1       time_cmp(time,time),
    FUNCTION        3       pg_catalog.in_range(time,time,interval,bool,bool),
    FUNCTION        4       btequalimage(oid),
STORAGE         time;

CREATE OPERATOR CLASS timestamptz_ops
DEFAULT FOR TYPE timestamptz USING xbtree
FAMILY datetime_ops
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    OPERATOR        1       <(timestamptz,date),
    OPERATOR        2       <=(timestamptz,date),
    OPERATOR        3       =(timestamptz,date),
    OPERATOR        4       >=(timestamptz,date),
    OPERATOR        5       >(timestamptz,date),
    OPERATOR        1       <(timestamptz,timestamp),
    OPERATOR        2       <=(timestamptz,timestamp),
    OPERATOR        3       =(timestamptz,timestamp),
    OPERATOR        4       >=(timestamptz,timestamp),
    OPERATOR        5       >(timestamptz,timestamp),
    FUNCTION        1       timestamptz_cmp_date(timestamptz,date),
    FUNCTION        1       timestamptz_cmp_timestamp(timestamptz,timestamp),
    FUNCTION        1       timestamptz_cmp(timestamptz,timestamptz),
    FUNCTION        2       timestamp_sortsupport(internal),
    FUNCTION        4       btequalimage(oid),
    FUNCTION        3       pg_catalog.in_range(timestamptz,timestamptz,interval,bool,bool),
STORAGE         timestamptz;

CREATE OPERATOR CLASS timetz_ops
DEFAULT FOR TYPE timetz USING xbtree
FAMILY timetz_ops
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    FUNCTION        1       timetz_cmp(timetz,timetz),
    FUNCTION        3       pg_catalog.in_range(timetz,timetz,interval,bool,bool),
    FUNCTION        4       btequalimage(oid),
STORAGE         timetz;

CREATE OPERATOR CLASS varbit_ops
DEFAULT FOR TYPE varbit USING xbtree
FAMILY varbit_ops
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    FUNCTION        1       varbitcmp(varbit,varbit),
    FUNCTION        4       btequalimage(oid),
STORAGE         varbit;

CREATE OPERATOR CLASS varchar_ops
FOR TYPE text USING xbtree
FAMILY text_ops
AS
--    OPERATOR        1       <,
--    FUNCTION        1       bttextcmp(text,text),
STORAGE         text;

CREATE OPERATOR CLASS timestamp_ops
DEFAULT FOR TYPE timestamp USING xbtree
FAMILY datetime_ops
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    OPERATOR        1       <(timestamp,date),
    OPERATOR        2       <=(timestamp,date),
    OPERATOR        3       =(timestamp,date),
    OPERATOR        4       >=(timestamp,date),
    OPERATOR        5       >(timestamp,date),
    OPERATOR        1       <(timestamp,timestamptz),
    OPERATOR        2       <=(timestamp,timestamptz),
    OPERATOR        3       =(timestamp,timestamptz),
    OPERATOR        4       >=(timestamp,timestamptz),
    OPERATOR        5       >(timestamp,timestamptz),
    FUNCTION        1       timestamp_cmp_date(timestamp,date),
    FUNCTION        1       timestamp_cmp(timestamp,timestamp),
    FUNCTION        2       timestamp_sortsupport(internal),
    FUNCTION        4       btequalimage(oid),
    FUNCTION        1       timestamp_cmp_timestamptz(timestamp,timestamptz),
    FUNCTION        3       pg_catalog.in_range(timestamp,timestamp,interval,bool,bool),
STORAGE         timestamp;

CREATE OPERATOR CLASS text_pattern_ops
FOR TYPE text USING xbtree
FAMILY text_pattern_ops
AS
    OPERATOR        1       ~<~,
    OPERATOR        2       ~<=~,
    OPERATOR        3       =,
    OPERATOR        4       ~>=~,
    OPERATOR        5       ~>~,
    FUNCTION        1       bttext_pattern_cmp(text,text),
    FUNCTION        2       bttext_pattern_sortsupport(internal),
    FUNCTION        4       btequalimage(oid),
STORAGE         text;

CREATE OPERATOR CLASS varchar_pattern_ops
FOR TYPE text USING xbtree
FAMILY text_pattern_ops
AS
--    OPERATOR        1       <,
--    FUNCTION        1       bttext_pattern_cmp(text,text),
STORAGE         text;

CREATE OPERATOR CLASS bpchar_pattern_ops
FOR TYPE bpchar USING xbtree
FAMILY bpchar_pattern_ops
AS
    OPERATOR        1       ~<~,
    OPERATOR        2       ~<=~,
    OPERATOR        3       =,
    OPERATOR        4       ~>=~,
    OPERATOR        5       ~>~,
    FUNCTION        1       btbpchar_pattern_cmp(bpchar,bpchar),
    FUNCTION        2       btbpchar_pattern_sortsupport(internal),
    FUNCTION        4       btequalimage(oid),
STORAGE         bpchar;

CREATE OPERATOR CLASS money_ops
DEFAULT FOR TYPE money USING xbtree
FAMILY money_ops
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    FUNCTION        1       cash_cmp(money,money),
    FUNCTION        4       btequalimage(oid),
STORAGE         money;

CREATE OPERATOR CLASS tid_ops
DEFAULT FOR TYPE tid USING xbtree
FAMILY tid_ops
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    FUNCTION        1       bttidcmp(tid,tid),
    FUNCTION        4       btequalimage(oid),
STORAGE         tid;

CREATE OPERATOR CLASS xid8_ops
DEFAULT FOR TYPE xid8 USING xbtree
FAMILY xid8_ops
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    FUNCTION        1       xid8cmp(xid8,xid8),
    FUNCTION        4       btequalimage(oid),
STORAGE         xid8;

CREATE OPERATOR CLASS uuid_ops
DEFAULT FOR TYPE uuid USING xbtree
FAMILY uuid_ops
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    FUNCTION        1       uuid_cmp(uuid,uuid),
    FUNCTION        2       uuid_sortsupport(internal),
    FUNCTION        4       btequalimage(oid),
STORAGE         uuid;

CREATE OPERATOR CLASS pg_lsn_ops
DEFAULT FOR TYPE pg_lsn USING xbtree
FAMILY pg_lsn_ops
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    FUNCTION        1       pg_lsn_cmp(pg_lsn,pg_lsn),
    FUNCTION        4       btequalimage(oid),
STORAGE         pg_lsn;

CREATE OPERATOR CLASS enum_ops
DEFAULT FOR TYPE anyenum USING xbtree
FAMILY enum_ops
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    FUNCTION        1       enum_cmp(anyenum,anyenum),
    FUNCTION        4       btequalimage(oid),
STORAGE         anyenum;

CREATE OPERATOR CLASS tsvector_ops
DEFAULT FOR TYPE tsvector USING xbtree
FAMILY tsvector_ops
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    FUNCTION        1       tsvector_cmp(tsvector,tsvector),
STORAGE         tsvector;

CREATE OPERATOR CLASS tsquery_ops
DEFAULT FOR TYPE tsquery USING xbtree
FAMILY tsquery_ops
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    FUNCTION        1       tsquery_cmp(tsquery,tsquery),
STORAGE         tsquery;

CREATE OPERATOR CLASS range_ops
DEFAULT FOR TYPE anyrange USING xbtree
FAMILY range_ops
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    FUNCTION        1       range_cmp(anyrange,anyrange),
STORAGE         anyrange;

CREATE OPERATOR CLASS multirange_ops
DEFAULT FOR TYPE anymultirange USING xbtree
FAMILY multirange_ops
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    FUNCTION        1       multirange_cmp(anymultirange,anymultirange),
STORAGE         anymultirange;

CREATE OPERATOR CLASS jsonb_ops
DEFAULT FOR TYPE jsonb USING xbtree
FAMILY jsonb_ops
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    FUNCTION        1       jsonb_cmp(jsonb,jsonb),
STORAGE         jsonb;

--
-- xbtree funcs
--
CREATE OR REPLACE FUNCTION get_xbtree_oid()
RETURNS oid AS $$
DECLARE
    val oid;
BEGIN
    SELECT oid INTO val FROM pg_am WHERE amname='xbtree';
    IF val IS NULL THEN
        RETURN 0;
    END IF;
    RETURN val;
END;
$$ LANGUAGE plpgsql;

DROP SCHEMA IF EXISTS xstore;
CREATE SCHEMA xstore;

--
-- xbt_metap()
--
CREATE FUNCTION xstore.xbt_metap(IN relname text,
    OUT magic int4,
    OUT version int4,
    OUT root int4,
    OUT "level" int4,
    OUT fastroot int4,
    OUT fastlevel int4)
AS 'MODULE_PATHNAME', 'xbt_metap'
LANGUAGE C STRICT PARALLEL SAFE;

--
-- xbt_page_stats()
--
CREATE FUNCTION xstore.xbt_page_stats(IN relname text, IN blkno int4,
    OUT blkno integer,
    OUT type "char",
    OUT live_items integer,
    OUT dead_items integer,
    OUT avg_item_size integer,
    OUT page_size integer,
    OUT free_size integer,
    OUT xbtpo_prev integer,
    OUT xbtpo_next integer,
    OUT xbtpo integer,
    OUT xbtpo_flags integer,
    OUT btpo_cycleid integer,
    OUT pd_prune_xid bigint,
    OUT last_delete_xid bigint,
    OUT activeTupleCount integer,
    OUT xact bigint)
AS 'MODULE_PATHNAME', 'xbt_page_stats'
LANGUAGE C STRICT PARALLEL SAFE;

--
-- xbt_page_items()
--
CREATE FUNCTION xstore.xbt_page_items(IN relname text, IN blkno int4,
    OUT lp_off smallint,
    OUT lp_flags smallint,
    OUT lp_len smallint,
    OUT itemoffset smallint,
    OUT ctid tid,
    OUT tableoid oid,
    OUT itemlen int,
    OUT nulls bool,
    OUT vars bool,
    OUT modified_xid bigint,
    OUT urec bigint,
    OUT data text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'xbt_page_items'
LANGUAGE C STRICT PARALLEL SAFE;

CREATE FUNCTION xstore.xbt_page_items(IN page bytea,
    OUT lp_off smallint,
    OUT lp_flags smallint,
    OUT lp_len smallint,
    OUT itemoffset smallint,
    OUT ctid tid,
    OUT tableoid oid,
    OUT itemlen int,
    OUT nulls bool,
    OUT vars bool,
    OUT modified_xid bigint,
    OUT urec bigint,
    OUT data text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'xbt_page_items_bytea'
LANGUAGE C STRICT PARALLEL SAFE;


