/* Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd */
CREATE EXTENSION query_check;

drop table if exists users;
create table users(username text, email text);
insert into users values ('alice', 'alice@example.com');
insert into users values ('bob', 'bob@example.com');
insert into users values ('charlie', 'charlie@example.com');

--
-- read only (needs to be executed in a read-only node)
--

-- case1: select for update/share
SELECT username FROM users;
SELECT username FROM users FOR UPDATE;
SELECT username FROM users FOR SHARE;
SELECT * FROM users WHERE username = '/*+ read-only*/' FOR UPDATE;

-- case2: select into 
SELECT username, email
INTO active_users
FROM users
WHERE email LIKE '%@example.com';

-- case3: WITH query
WITH inserted_users AS (
    INSERT INTO users (username, email)
    VALUES
    ('Alice', 'alice@example.com'),
    ('Bob', 'bob@example.com'),
    ('Charlie', 'charlie@example.com')
    RETURNING *
)
SELECT * FROM inserted_users;

WITH user_details AS (
    SELECT username, email
    FROM users
    WHERE username LIKE 'j%'
)
SELECT * FROM user_details;

-- case4: select has udf call

-- system function call
SELECT pg_is_in_recovery();

-- user-defined read_only function call
CREATE OR REPLACE FUNCTION get_user_count() RETURNS INTEGER AS $$
BEGIN
    RETURN (SELECT COUNT(*) FROM users);
END;
$$ LANGUAGE plpgsql;

SELECT * from get_user_count();

-- user-defined writing function call
CREATE OR REPLACE FUNCTION insert_new_user(username TEXT, email TEXT)
RETURNS VOID AS $$
BEGIN
    INSERT INTO users (username, email) VALUES (username, email);
END;
$$ LANGUAGE plpgsql;

SELECT insert_new_user('john_doe', 'john@example.com');

-- case5: select has system catalog
SELECT * FROM pg_stat_replication;

-- case6: copy to/from
-- copy to stdout
COPY users TO STDOUT DELIMITER ',' CSV HEADER;
-- copy to/from file
COPY users TO '/tmp/users.csv' DELIMITER ',' CSV HEADER;
COPY users FROM '/tmp/users.csv' DELIMITER ',' CSV HEADER;

-- case7: explain and explain analyze with udf
EXPLAIN SELECT * FROM users;
EXPLAIN ANALYZE SELECT * FROM users;
EXPLAIN SELECT get_user_count();
EXPLAIN ANALYZE SELECT get_user_count();

-- case8: use hint like /*+ read-only */
SELECT * FROM pg_stat_replication;
/*+ read-only */ SELECT * FROM pg_stat_replication;

SELECT * from get_user_count();
/*+ read-only */ SELECT * from get_user_count();

-- case9: recursive function
CREATE FUNCTION factorial(int)
RETURNS bigint AS $$
BEGIN
    IF $1 <= 1 THEN
        RETURN 1;
    ELSE
        RETURN $1 * factorial($1 - 1);
    END IF;
END;
$$ LANGUAGE plpgsql;

SELECT * FROM factorial(10);
/*+ read-only */ SELECT * FROM factorial(10);

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
NOTIFY test_channel, 'test message 1';
BEGIN;
NOTIFY test_channel, 'test message 2';
COMMIT;
BEGIN;
NOTIFY test_channel, 'test message 3';
ROLLBACK;
UNLISTEN test_channel;

DROP FUNCTION get_user_count;
DROP FUNCTION insert_new_user;
DROP FUNCTION test_func;
DROP PROCEDURE test_proc;
DROP TABLE users;
DROP EXTENSION query_check;