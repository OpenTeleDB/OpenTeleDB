# **简易教程**

## **基本概念**

OpenTeleDB是基于一个开源的对象关系型数据库系统，以健壮性、完整性、功能强大著称。它支持复杂的查询，外键，事务完整性，MVCC。其架构由后端（处理数据存储，查询和事务）和前端（客户端应用与数据库交互）组成。



**数据库**

数据库（DataBase, DB）是长期存储在计算设备内、有组织的、统一管理的相关数据的集合。数据库管理系统（DBMS）是位于用户与系统之间的一层数据管理软件，为用户或应用程序提供访问数据库的方法。

 

**表**

表是由行与列组合成的，是数据库中用来存储数据的对象，是整个数据库系统的基础。

每张表只能属于一个数据库，也只能对应到一个表空间。每张表对应的数据文件必须在同一个表空间中。

 

**索引**

是一种为了提高数据查询效率而设计的、分布于多个节点之上的数据结构。它就像一本超大型书籍的分布式目录，能帮助数据库系统快速定位到数据所在的具体物理位置（在哪台机器的哪个分片上），而无需进行全库扫描。

 

**视图**

是一个虚拟的表，其内容由查询定义。它本身不存储数据，而是从一个或多个基表（这些基表可能分布在不同的节点上）中动态计算或拼接出来的结果集。

 

**函数/存储过程**

函数（Function） 和 存储过程（Procedure） 是用于封装可重用 SQL 逻辑的重要数据库对象。它们可以接受参数、执行复杂操作、返回结果，极大提升开发效率和数据一致性。

 

## **语法**

基础语法与开源的PostgreSQL语法保持一致。

## **数据类型**

表一：数字类型

| **名字**         | **存储尺寸** | **描述**           | **范围**                                               |
| ---------------- | ------------ | ------------------ | ------------------------------------------------------ |
| smallint         | 2字节        | 小范围整数         | 小范围整数-32768 到 +32767                             |
| integer          | 4字节        | 整数的典型选择     | -2147483648 到 +2147483647                             |
| bigint           | 8字节        | 大范围整数         | 大范围整数-9223372036854775808 到 +9223372036854775807 |
| decimal          | 可变         | 用户指定精度，精确 | 最高小数点前131072位，以及小数点后16383位              |
| numeric          | 可变         | 用户指定精度，精确 | 最高小数点前131072位，以及小数点后16383位              |
| real             | 4字节        | 可变精度，不精确   | 6位十进制精度                                          |
| double precision | 8字节        | 可变精度，不精确   | 15位十进制精度                                         |
| smallserial      | 2字节        | 自动增加的小整数   | 1到32767                                               |
| serial           | 4字节        | 自动增加的整数     | 1到2147483647                                          |
| bigserial        | 8字节        | 自动增长的大整数   | 1到9223372036854775807                                 |

 

表二：字符类型

| **名字**                         | **描述**                   |
| -------------------------------- | -------------------------- |
| character varying(n), varchar(n) | 可变长度，有长度限制       |
| character(n), char(n), bpchar(n) | 固定长度，空格填充         |
| bpchar                           | 可变长度，无限制，空格修剪 |
| text                             | 可变长度，无限制           |

 

表三：二进制数据类型

| **名字** | **存储尺寸**               | **描述**     |
| -------- | -------------------------- | ------------ |
| bytea    | 1或4字节外加真正的二进制串 | 变长二进制串 |

 

表四：日期类型

| **名字**                                  | **存储尺寸** | **描述**                   | **最小值**    | **最大值**    | **解析度** |
| ----------------------------------------- | ------------ | -------------------------- | ------------- | ------------- | ---------- |
| timestamp [ (*p*) ] [ without time zone ] | 8字节        | 包括日期和时间（无时区）   | 4713 BC       | 294276 AD     | 1微秒/14位 |
| timestamp [ (*p*) ] [ with time zone ]    | 8字节        | 包括日期和时间（有时区）   | 4713 BC       | 294276 AD     | 1微秒/14位 |
| date                                      | 4字节        | 日期（没有一天中的时间）   | 4713 BC       | 5874897 AD    | 1日        |
| time [ (*p*) ] [ without time zone ]      | 8字节        | 一天中的时间（无时区）     | 0:00:00       | 24:00:00      | 1微秒/14位 |
| time [ (*p*) ] [ with time zone ]         | 12字节       | 仅是一天中的时间，带有时区 | 00:00:00+1459 | 24:00:00-1459 | 1微秒/14位 |
| interval [ *fields* ] [ (*p*) ]           | 16字节       | 时间间隔                   | -178000000年  | 178000000年   | 1微秒/14位 |

 

表五：布尔类型

| **名字** | **存储尺寸** | **描述**     |
| -------- | ------------ | ------------ |
| boolean  | 1字节        | 状态为真或假 |

 

## **数据库管理**

### **创建数据库**

**说明**

只有超级用户或具有特殊的CREATEDB特权才可以创建数据库。

默认情况下，可通过克隆标准系统数据库template1创建新数据库。

可通过写TEMPLATE name指定不同的模板。

可通过写TEMPLATE template0您可以创建一个干净的数据库，只包含Teledb所预定义的标准对象。

以下为创建数据库相关操作。

- 默认参数创建数据库

```
teledb=# create database teledb_db;

CREATE DATABASE
```

- 指定克隆库

```
teledb=# create database teledb_db_template TEMPLATE  template0;

CREATE DATABASE
```

- 指定所有者

```
teledb=# create role teledb_user with login;

CREATE ROLE

teledb=# create database teledb_db_owner owner teledb_user;

CREATE DATABASE

teledb=# \l+ teledb_db_owner 

                            List of databases

    Name    |   Owner   | Encoding |  Collate  |   Ctype   | Access privileges | Size  | Tablespace | Description 

------------------+--------------+----------+-------------+-------------+-------------------+-------+------------+-------------

 teledb_db_owner | teledb_user | UTF8   | en_US.UTF-8 | en_US.UTF-8 |          | 7393 kB | pg_default | 

(1 row)
```

- 指定编码

```
teledb=# create database teledb_db_encoding ENCODING UTF8;  

CREATE DATABASE

teledb=# \l+ teledb_db_encoding 

                           List of databases

    Name     |  Owner  | Encoding |  Collate  |   Ctype   | Access privileges | Size  | Tablespace | Description 

---------------------+---------+----------+-------------+-------------+-------------------+-------+------------+-------------

 teledb_db_encoding | teledb | UTF8   | en_US.UTF-8 | en_US.UTF-8 |          | 7393 kB | pg_default | 

(1 row)
```

- 指定排序规则

```
teledb=# create database teledb_db_lc_collate lc_collate 'C' template template0;

CREATE DATABASE
```

- 指定分组规则

```
teledb=# create database teledb_lc_ctype LC_CTYPE 'C' template template0;

CREATE DATABASE
```

- 配置数据可连接

```
teledb=# create database teledb__allow_connections ALLOW_CONNECTIONS true;

CREATE DATABASE

teledb=# select datallowconn from pg_database where datname = 'teledb__allow_connections';

 datallowconn 

\--------------

 t

(1 row)

teledb=# \c teledb__allow_connections

You are now connected to database "teledb__allow_connections" as user "teledb".
```

- 配置连接数

```
teledb=# create database teledb_connlimit CONNECTION LIMIT 100;

CREATE DATABASE

teledb=# select datconnlimit  from pg_database where datname='teledb_connlimit';  

 datconnlimit 

--------------

     100

(1 row)
```

- 配置数据库可以被复制（是否模板数据库）

```
teledb=#  create database teledb_istemplate is_template true;

CREATE DATABASE

teledb=# select datconnlimit  from pg_database where datname='teledb_connlimit';  

 datconnlimit 

--------------

     100

(1 row)
```

- 同时配置多个参数

```
teledb=# create database teledb_mul owner teledb_user  CONNECTION LIMIT 50 template template0 encoding 'utf8'  lc_collate 'C';

CREATE DATABASE
```

### **修改数据库配置**

您可参考如下操作修改数据库配置。

- 修改数据库名称

```
teledb=# alter database teledb_db rename to teledb_db_new;

ALTER DATABASE
```

- 修改连接数

```
teledb=# alter database teledb_db_new connection limit 50;

ALTER DATABASE
```

- 修改数据库所有者

```
teledb=# alter database teledb_db_new owner to teledb;

ALTER DATABASE
```

- 设置search_path为默认数据

```
teledb=# alter database teledb_db_new set search_path to public, pg_catalog;

ALTER DATABASE
```

- alter database不支持修改的项目

| **项目**   | **备注** |
| ---------- | -------- |
| encoding   | 编码     |
| lc_collate | 排序规则 |
| lc_ctype   | 分组规则 |

### **删除数据库**

您可参考如下操作删除数据库。

- 删除数据库teledb_db_new

```
teledb=# drop database teledb_db_new;
```

仍有会话连接数据库时，会报错

```
ERROR:  database "teledb_db_new" is being accessed by other users

DETAIL:  There is 1 other session using the database.
```



- 停止该数据库的所有连接后重新删除数据库

```
teledb=# select pg_terminate_backend(pid) from pg_stat_activity where datname='teledb_db_new';

 pg_terminate_backend 

----------------------

 t

(1 row)

teledb=# drop database teledb_db_new;

DROP DATABASE
```

## **高级特性示例**

### **XProxy**

1. XProxy构建

XProxy进入xproxy源码目录进行构建

```
sh ./ctg_build.sh // 构建完成后，进入根目录下的xproxy目录

cd xproxy  // 目录结构为 bin:二进制文件以及启动脚本目录 etc:配置目录 lib：动态库依赖目录
```



2. XProxy配置

以一主一备的PostgreSQL实例为例

在配置文件模板xproxy.conf中storage endpoints模块中配置数据库主机的信息（主、备节点的IP、端口）：

```
 endpoints {

    endpoint

    {

      hostname "<your_primary_databse_host_ip>"

      port <your_primary_databse_host_port>

      weight 0.0

      application_name ""

    }

    endpoint

    {

      hostname "<your_standby_databse_host_ip>"

      port <your_standby_databse_host_port>

      weight 10.0

      application_name "<your_standby_databse_host_ip>:<your_standby_databse_host_port>"

    } 

}
```

 

在storage watchdog模块中，配置数据库用户信息(用户名、密码)：

```
 watchdog {

    storage "postgres_server"

    storage_db "postgres"

    storage_user "<your_postgresql_user_name>"

    storage_password "<your_postgresql_password>"

    pool_routing "internal"

    pool "transaction"

    pool_size 10

    pool_timeout 0

    pool_ttl 0

    log_debug no

    replication_delay_threshold 0

    catchup_timeout 15

}
```

 

在 database default user "user_aq_internal_pooling"模块中，配置数据库用户信息(用户名、密码)，作为内部认证账号：

```
database default

{

  user "user_aq_internal_pooling" {

      authentication "none"

      storage "postgres_server"

      pool "session"

      storage_db "postgres"

      storage_user "<your_postgresql_user_name>"

      storage_password "<your_postgresql_password>"

      log_debug no

      log_query no

      pool_size 10

      pool_timeout 10000

      pool_routing "internal"

      enable_quantiles_state yes

      catchup_timeout 4

}

｝
```

 

3. 启动xproxy

```
cd xproxy/bin

sh xproxy-start.sh ../etc/template.conf  # 启动

# 启动后，查看进程

ps -ef | grep xproxy

# 信息如下

user   44346  104  0 10:18 pts/3   00:00:00 xproxy: version 1.0.0_P1 listening and accepting new connections, configuration file: template.conf

 

# 查看日志

tail -f xproxy.log

# 信息如下 （watchdog每秒探测各数据库节点心跳、延迟等状态）

user@d8d2edbbecb1:~/xproxy$ tail -f xproxy.log

44346 2025-09-24T10:19:23Z info [add2a91651311 s6345332caaa1] (watchdog) dog [83] received heartbeat information from node [127.0.0.1:5432] with value 1758680363

44346 2025-09-24T10:19:23Z info [aa921094cfb99 s0d1698345b53] (watchdog) dog [86] received heartbeat information from node [127.0.0.1:5433] with value 1758680363

44346 2025-09-24T10:19:23Z info [add2a91651311 s6345332caaa1] (watchdog) dog [83] received role information from node [127.0.0.1:5432] with value 1

44346 2025-09-24T10:19:23Z info [aa921094cfb99 s0d1698345b53] (watchdog) dog [86] received role information from node [127.0.0.1:5433] with value 2

44346 2025-09-24T10:19:23Z info [af6c381650ad4 none] (watchdog) replag received from node [127.0.0.1:5433] with value 0

44346 2025-09-24T10:19:24Z info [add2a91651311 s2a4b1e2b1b37] (watchdog) dog [83] received heartbeat information from node [127.0.0.1:5432] with value 1758680364

44346 2025-09-24T10:19:24Z info [aa921094cfb99 s0d1698345b53] (watchdog) dog [86] received heartbeat information from node [127.0.0.1:5433] with value 1758680364

44346 2025-09-24T10:19:24Z info [add2a91651311 s2a4b1e2b1b37] (watchdog) dog [83] received role information from node [127.0.0.1:5432] with value 1

44346 2025-09-24T10:19:24Z info [aa921094cfb99 s0d1698345b53] (watchdog) dog [86] received role information from node [127.0.0.1:5433] with value 2

44346 2025-09-24T10:19:24Z info [af6c381650ad4 none] (watchdog) replag received from node [127.0.0.1:5433] with value 0
```

 

4. 案例1 基础的连接与使用

使用psql连接代理：

```
psql -h 127.0.0.1 -p 6001 postrges
```

\# 执行SQL查询，可以观察到连接在不同的PostgreSQL backend连接上调度

```
postgres=# select pg_backend_pid();

 pg_backend_pid

\----------------

     49218

(1 row)

 

postgres=# begin;select pg_backend_pid();end;

BEGIN

 pg_backend_pid

\----------------

     49392

(1 row)

 

COMMIT
```

 

使用psql连接内部控制台console，并用SHOW CLIENTS命令观察连接到代理的客户端信息：

```
psql -h 127.0.0.1 -p 6001 console

psql (16.6, server 1.0.0_P1-1.0.0_P1-9-g98b4ea3-release)

Type "help" for help.

 

console=> show clients;

 type | user | database |  state  | storage_user |  addr   | port  | local_addr | local_port | query_time | transaction_time | from_last_query | wait | wait_us |    id    |    ptr    | coro | remote_pid | tls

------+------+----------+---------+--------------+-----------+-------+------------+------------+------------+------------------+-----------------+------+---------+---------------+----------------+------+------------+-----

 C   | user| postgres | pending |        | 127.0.0.1 | 34044 | 127.0.0.1  |    6001 |       |          | 37972863     |   0 |    0 | c5e53da072c8d | 0x74ca2c19b060 |   1 |      0 |

 C   | user| console  | pending |        | 127.0.0.1 | 44022 | 127.0.0.1  |    6001 |       |          |         |   0 |    0 | c3f361506aedf | 0x74ca2c1aeaf0 |   1 |      0 |

(2 rows)
```

 

5. 案例2 连接池将多数前端连接映射到少数后端数据库连接的能力

设定数据库连接上限为50 （max_connections）

使用sysbench进行压测，设定连接数为500：

```
sysbench --db-driver=pgsql --pgsql-host=127.0.0.1 --pgsql-port=6001 --pgsql-user=maki --pgsql-db=sbtest --table-size=1000 --tables=10 --time=600  --report-interval=5 --threads=500  oltp_read_write.lua run
```

使用psql连接内部控制台console，并用SHOW POOLS命令观察连接到代理的连接池信息：

```
console=> show pools;

 database | user | cl_active | cl_idle | cl_waiting | sv_active | sv_idle | sv_used | sv_tested | sv_login |    sv_detail    | maxwait | maxwait_us |  pool_mode

----------+------+-----------+---------+------------+-----------+---------+---------+-----------+----------+---------------------+---------+------------+-------------

 postgres | maki |     0 |    3 |      0 |     0 |    2 |    0 |     0 |     0 | {0/1/0, 0/1/0}    |    0 |      0 | transaction

 sbtest  | maki |     47 |    1 |     452 |     47 |   113 |    0 |     0 |     0 | {47/1/447, 0/112/0} |    0 |      0 | transaction

 postgres | maki |     0 |    0 |      0 |     0 |    1 |    0 |     0 |     0 | {0/1/0, 0/0/0}    |    0 |      0 | session

 console  | maki |     0 |    1 |      0 |     0 |    0 |    0 |     0 |     0 | {}          |    0 |      0 | session
```

可以观察到接入了500个客户端，超出后端设置的50个连接的上限。

以下给出一份template.conf文件示例

```
# 服务相关配置

daemonize no

unix_socket_dir "/tmp"

unix_socket_mode "0644"

locks_dir "/tmp/odyssey"

use_unix_socket_if_possible yes

graceful_die_on_errors no

enable_online_restart no

bindwith_reuseport no

pid_file "/tmp/odyssey.pid"

# 日志相关的配置

log_file "./xproxy.log"

log_format "%p %t %l [%i %s] (%c) %m\n"

log_to_stdout no

log_syslog no

log_syslog_ident "xproxy"

log_syslog_facility "daemon"

log_debug no

log_config yes

log_session yes

log_query no

log_stats no

log_general_stats_prom no

log_route_stats_prom no

stats_interval 60

# 性能相关配置

workers 4

resolvers 1

readahead 8192

cache_coroutine 100112

coroutine_stack_size 4

nodelay yes

keepalive 15

keepalive_keep_interval 5

keepalive_probes 3

keepalive_usr_timeout 0

# 全局限制

client_max 200000

server_login_retry 1

# 监听端口配置

# 可以同时监听多个端口

listen {

  host "*"

  port 6001

  backlog 4096

  compression no

  port_attrs "Read-write"

}

 

listen {

  host "*"

  port 6002

  backlog 4096

  compression no

  port_attrs "Write-only"

}

 

listen {

  host "*"

  port 6003

  backlog 4096

  compression no

  port_attrs "Read-only"

}

 

# 存储节点配置

storage "postgres_server" {

  type "remote"

  # 配置PG服务器的地址以及权重

  endpoints {

    endpoint

    {

      hostname "127.0.0.1"

      port 5432

      weight 0.0

      application_name ""

    }

   endpoint

    {

      hostname "127.0.0.1"

      port 5433

      weight 10.0

     application_name "127.0.0.1:5433"

    } 

  }

  target_session_attrs "read-write"

  watchdog {

    storage "postgres_server"

    storage_db "postgres"

    storage_user "maki"

    storage_password "M$eDqyi_n5%lryd1"

    pool_routing "internal"

    pool "transaction"

    pool_size 10

    pool_timeout 0

    pool_ttl 0

    log_debug no

    replication_delay_threshold 0

    catchup_timeout 15

  }

}

# 数据库和用户配置

database default {

  # 专门用于登录认证的“内部”用户，并不存在于PG数据库

  user "user_aq_internal_pooling" {

      authentication "none"

      storage "postgres_server"

      pool "session"

      storage_db "postgres"

      storage_user "maki"

      storage_password "M$eDqyi_n5%lryd1"

      log_debug no

      log_query no

      pool_size 10

      pool_timeout 10000

      pool_routing "internal"

      enable_quantiles_state yes

     catchup_timeout 4

  }

  user default {

      authentication "scram-sha-256"

      allow_clear_text_frontend_auth no

      auth_query "SELECT usename, passwd FROM pg_shadow WHERE usename=$1"

      auth_query_db "postgres"

      auth_query_user "user_aq_internal_pooling"

      target_server_attrs "auto"

      client_max 100000

      storage "postgres_server"

      pool "transaction"

      pool_size 112

      pool_timeout 0

      pool_ttl 1800

      pool_cancel yes

      pool_rollback yes

      pool_idle_in_transaction_timeout 0

      pool_reserve_prepared_statement yes

      pool_prepared_statement_limit 500

      pool_prepared_statement_expired_time 120

      client_fwd_error yes

      server_lifetime 3600

      application_name_add_host yes

      reserve_session_server_connection no

      log_debug no

      enable_quantiles_state yes

      replication_delay_threshold 30000000

      catchup_timeout 5

      enable_read_only_blacklist yes 

  }

}

 

# 管控数据库和用户名配置

storage "local" {

    type "local"

}

database "console" {

    user default {

        authentication "scram-sha-256"

        auth_query "SELECT usename, passwd FROM pg_shadow WHERE usename=$1"

        auth_query_db "postgres"

        auth_query_user "user_aq_internal_pooling"

        role "admin"

        pool "session"

        storage "local"

        quantiles "0.95,0.5"

    }

}
```

 

### **XStore**

**前提条件**

1. 已成功编译安装带有xstore功能的数据库

2. 配置文件有shared_preload_libraries = 'xstore.so'

3. 需要create extension xstore;

 

**操作步骤**

**创建XStore存储引擎表**

1. 创建表时指定存储引擎类型

```
Create table xt1(a int primary key, b int) using xstore;
```

2. GUC参数配置指定xstore存储引擎

数据库配置文件postgresql.conf添加如下配置项

```
default_table_access_method = 'xstore'

Create table xt1(a int primary key, b int);
```

**XStore索引使用xbtree**

1. 不指定创建索引类型，XStore表默认创建xbtree索引

```
create index xbt_idx1 on xt1(a);
```

2. 使用using xbtree关键字

```
create index xbt_idx2 on xt1 using xbtree(a);
```

其余语法与heap使用基本保持一致，索引使用区别如上。

### **XRaft**

**前提条件**

1.已成功编带有xraft功能的数据库(configure带上--with-zstd  --with-lz4 --with-xraft --with-openssl)

2.配置文件增加以下内容：

```
shared_preload_libraries = 'xraft.so'

xraft_node_id = 1  

xraft_data_path = '/xxx/cluster/data1/dn1/dcf_data'  

xraft_config = '[{"stream_id":1,"node_id":1,"ip":"x.x.x.21","port":xx,"role":"LEADER"},{"stream_id":1,"node_id":2,"ip":"x.x.x.22","port":xx,"role":"FOLLOWER"},{"stream_id":1,"node_id":3,"ip":"x.x.x.23","port":xx,"role":"FOLLOWER"}]'
```

**操作步骤**

1.使用initdb初始化数据库

2.拷贝data目录到其他节点

3.在每个节点的data目录创建standby.signal文件

4.使用pg_ctl以standby的方式启动每个节点，后续会通过xraft完成选主和升主的过程。