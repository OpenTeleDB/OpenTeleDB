# query_check

## Name
query_check

## Description
check the SQL query for notification of the read-only statement and statements that require memory caching

## Installation
```
cd query_check
make USE_PGXS=1
make USE_PGXS=1 install
```

## Configuration
The following GUCs can be configured, in postgresql.conf:
- **query_check.enabled** (boolean, default true): whether or not query_check should be enabled

## Usage
1. add extension to your postgresql.conf
```
shared_preload_libraries = 'query_check'
```

2. restart postgresql

3. use the extension
```
CREATE EXTENSION query_check;
\dx
```

## Details

### read-only statement

We consider the following statements as read-only:
- SELECT/WITH without FOR UPDATE/SHARE
- COPY TO STDOUT
- EXPLAIN
- EXPLAIN ANALYZE and query is SELECT not including UDF
- SELECT without UDF
- SELECT without SYSTEM CATALOG
- ALL QUERY that uses hint /*+ read-only */

If we check the query and it's not read-only, we will send such **error** message to the client.

```bash
ERROR: xproxy not_readonly query
```

### memory caching statements

We consider the following statements as memory caching:
- CREATE/DROP TEMORARY TABLE
- PREPARE/DEALLOCATE statement
- SET GUC_params that flag isn't GUC_REPORT or doesn't set in LOCAL mode
- DISCARD
- session level advisory lock/unlock
- LISTEN/UNLISTEN

If we check the query and it's memory caching, we will send such **notice** message to the client:

1. CREATE/DROP TEMPORARY TABLE
```bash
NOTICE: xproxy create temp table <table_name>
NOTICE: xproxy drop temp table <table_name>
```

2. PREPARE/DEALLOCATE statement
```bash
NOTICE: xproxy prepare stmt <stmt_name>
NOTICE: xproxy deallocate stmt <stmt_name>
NOITCE: xproxy deallocate all # DEALLOCATE_ALL
```

3. SET GUC_params
```bash
NOTICE: xproxy set variable <param_name> to <value> # VAR_SET_VALUE
NOITCE: xproxy reset all # VAR_RESET_ALL
NOTICE: xproxy set variable <param_name> from current # VAR_SET_CURRENT
NOTICE: xproxy reset variable <param_name> # VAR_SET_DEFAULT/VAR_RESET
```

4. session level advisory lock/unlock
```bash
NOTICE: xproxy no session level advisory locks held by the current session
NOTICE: xproxy session level advisory locks held by the current session
```

5. LISTEN/UNLISTEN
```bash
NOTICE: xproxy listen on <channel_name>
NOTICE: xproxy unlisten on <channel_name>
NOTICE: xproxy unlisten all # UNLISTEN *
```
