/* Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd */
CREATE EXTENSION query_check;

select load_query_check();
SELECT set_config(name, 'on', null) from pg_settings where name = 'query_check.enabled';

drop table if exists users;
create table users(username text, email text);
insert into users values ('alice', 'alice@example.com');
insert into users values ('bob', 'bob@example.com');
insert into users values ('charlie', 'charlie@example.com');

--
-- session context (needs to be executed in the master node)
--

-- case1: create/drop temp table
CREATE TEMP TABLE temp_test(a int, b text);
CREATE TEMP TABLE temp_users AS SELECT * FROM users;
DROP TABLE temp_test;
DROP TABLE temp_users;

-- case2: prepare staement
PREPARE stmt1 (text, text) AS INSERT INTO users VALUES ($1, $2);
EXECUTE stmt1 ('Alice', 'alice@example.com');
DEALLOCATE PREPARE stmt1;

PREPARE stmt2 (text) AS SELECT * FROM users WHERE username = $1;
EXECUTE stmt2 ('Alice');
DEALLOCATE PREPARE stmt2;

DEALLOCATE PREPARE stmt3;

-- case3: set guc parmeters
SET client_min_messages TO ERROR;

-- case4: utility statement in function/procedure
DROP FUNCTION IF EXISTS test_func;

CREATE OR REPLACE FUNCTION test_func() RETURNS VOID AS $$
BEGIN
    CREATE TEMP TABLE temp_test(a int, b text);
    CREATE TEMP TABLE temp_users AS SELECT * FROM users;

    PREPARE stmt1 (text, text) AS INSERT INTO users VALUES ($1, $2);
    EXECUTE 'EXECUTE stmt1 (''Alice'', ''alice@example.com'')';
    DEALLOCATE PREPARE stmt1;

    PREPARE stmt2 (text) AS SELECT * FROM temp_users WHERE username = $1;
    EXECUTE 'EXECUTE stmt2 (''Alice'')';
    DEALLOCATE PREPARE stmt2;

    SET client_min_messages TO ERROR;
    DROP TABLE temp_test;
    DROP TABLE temp_users;
END;
$$ LANGUAGE plpgsql;

SELECT test_func();

DROP PROCEDURE IF EXISTS test_proc;

CREATE OR REPLACE PROCEDURE test_proc()
AS $$
BEGIN
    CREATE TEMP TABLE temp_test(a int, b text);
    CREATE TEMP TABLE temp_users AS SELECT * FROM users;

    PREPARE stmt1 (text, text) AS INSERT INTO users VALUES ($1, $2);
    EXECUTE 'EXECUTE stmt1 (''Alice'', ''alice@example.com'')';
    DEALLOCATE PREPARE stmt1;

    PREPARE stmt2 (text) AS SELECT * FROM temp_users WHERE username = $1;
    EXECUTE 'EXECUTE stmt2 (''Alice'')';
    DEALLOCATE PREPARE stmt2;

    SET client_min_messages TO ERROR;
    DROP TABLE temp_test;
    DROP TABLE temp_users;
END;
$$ LANGUAGE plpgsql;

CALL test_proc();

-- case5: utility statement in transaction block
BEGIN;
CREATE TEMP TABLE temp_test(a int, b text);
CREATE TEMP TABLE temp_users AS SELECT * FROM users;

PREPARE stmt1 (text, text) AS INSERT INTO users VALUES ($1, $2);
EXECUTE stmt1 ('Alice', 'alice@example.com');
DEALLOCATE PREPARE stmt1;

PREPARE stmt2 (text) AS SELECT * FROM temp_users WHERE username = $1;
EXECUTE stmt2 ('Alice');
DEALLOCATE PREPARE stmt2;

SET client_min_messages TO ERROR;
DROP TABLE temp_test;
DROP TABLE temp_users;
COMMIT;

BEGIN;
CREATE TEMP TABLE temp_test(a int, b text);
CREATE TEMP TABLE temp_users AS SELECT * FROM users;

PREPARE stmt1 (text, text) AS INSERT INTO users VALUES ($1, $2);
EXECUTE stmt1 ('Alice', 'alice@example.com');
DEALLOCATE PREPARE stmt1;

PREPARE stmt2 (text) AS SELECT * FROM temp_users WHERE username = $1;
EXECUTE stmt2 ('Alice');
DEALLOCATE PREPARE stmt2;

SET client_min_messages TO ERROR;
DROP TABLE temp_test;
DROP TABLE temp_users;
ROLLBACK;

-- case6: utility statements in subtransaction block
BEGIN;

SET client_min_messages TO ERROR;

SAVEPOINT sp1;

CREATE TEMP TABLE temp_test(a int, b text);
CREATE TEMP TABLE temp_users AS SELECT * FROM users;

PREPARE stmt1 (text, text) AS INSERT INTO users VALUES ($1, $2);
EXECUTE stmt1 ('Alice', 'alice@example.com');
DEALLOCATE PREPARE stmt1;

PREPARE stmt2 (text) AS SELECT * FROM temp_users WHERE username = $1;
EXECUTE stmt2 ('Alice');
DEALLOCATE PREPARE stmt2;
DROP TABLE temp_test;
DROP TABLE temp_users;

ROLLBACK TO SAVEPOINT sp1;

END;

-- case7: session level advisory lock 
SELECT pg_advisory_lock(1);
SELECT pg_advisory_unlock(1);
SELECT pg_advisory_lock(1, 1);
SELECT pg_advisory_unlock(1, 1);
SELECT pg_advisory_lock_shared(1);
SELECT pg_advisory_unlock_shared(1);
SELECT pg_advisory_lock_shared(1, 1);
SELECT pg_advisory_unlock_shared(1, 1);
SELECT pg_try_advisory_lock(1);
SELECT pg_try_advisory_lock(1, 1);
SELECT pg_try_advisory_lock_shared(1);
SELECT pg_try_advisory_lock_shared(1, 1);
SELECT pg_advisory_unlock_all();

DROP TABLE IF EXISTS test_lock_tb;
CREATE TABLE test_lock_tb(id int, flag bool);
SELECT pg_advisory_lock(1);
SELECT pg_advisory_lock(1, 1);
SELECT * FROM test_lock_tb WHERE flag = (SELECT pg_advisory_unlock(1));
SELECT * FROM test_lock_tb WHERE flag = (SELECT pg_advisory_unlock(1, 1));
DROP TABLE test_lock_tb;

-- case8: LISTEN/NOTIFY
LISTEN test_channel;
--NOTIFY test_channel, 'test message 1';
--BEGIN;
--NOTIFY test_channel, 'test message 2';
--COMMIT;
BEGIN;
NOTIFY test_channel, 'test message 3';
ROLLBACK;
UNLISTEN test_channel;

DROP FUNCTION test_func;
DROP PROCEDURE test_proc;
DROP TABLE users;
DROP EXTENSION query_check;