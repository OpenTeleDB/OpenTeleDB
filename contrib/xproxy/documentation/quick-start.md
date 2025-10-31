# Quick Start

## 环境

CPU架构：x86_64

OS：Ubuntu 24.04.1 LTS

编译器: gcc version 13.3.0

## 编译与构建

```SQL
sh ./ctg_build.sh
cd xproxy
// 目录结构为 bin:二进制文件以及启动脚本目录 etc:配置目录 lib：动态库依赖目录
```

## 配置

以一主一备的OpenTeleDB实例为例

在配置文件模板xproxy.conf中storage endpoints模块中配置数据库主机的信息（主、备节点的IP、端口）：

```SQL
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

```SQL
    watchdog {
        storage "postgres_server"
        storage_db "postgres"
        storage_user "<your_OpenTeleDB_user_name>"
        storage_password "<your_OpenTeleDB_password>"
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

```SQL
database default
{
    user "user_aq_internal_pooling" {
            authentication "none"
            storage "postgres_server"
            pool "session"
            storage_db "postgres"
            storage_user "<your_OpenTeleDB_user_name>"
            storage_password "<your_OpenTeleDB_password>"
            log_debug no
            log_query no
            pool_size 10
            pool_timeout 10000
            pool_routing "internal"
            enable_quantiles_state yes
            catchup_timeout 4
    }
```

## 运行

```SQL
cd xproxy/bin
# 启动
sh xproxy-start.sh ../etc/xproxy.conf
# 启动后，查看进程
ps -ef | grep xproxy
# 信息如下
user    44346   104  0 10:18 pts/3    00:00:00 xproxy: version 1.0.0_P1 listening and accepting new connections, configuration file: xproxy.conf

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

## 使用

## case1

本例子展示基础的连接与使用

使用psql客户端连接代理：

```SQL
psql -h 127.0.0.1 -p 6001 postrges
# 执行SQL查询，可以观察到连接在不同的OpenTeleDB backend连接上调度
postgres=# select pg_backend_pid();
 pg_backend_pid
----------------
          49218
(1 row)

postgres=# begin;select pg_backend_pid();end;
BEGIN
 pg_backend_pid
----------------
          49392
(1 row)

COMMIT
```

使用psql客户端连接内部控制台console，并用`SHOW CLIENTS`命令观察连接到代理的客户端信息：

```SQL
psql -h 127.0.0.1 -p 6001 console
psql (16.6, server 1.0.0_P1-1.0.0_P1-9-g98b4ea3-release)
Type "help" for help.

console=> show clients;
 type | user | database |  state  | storage_user |   addr    | port  | local_addr | local_port | query_time | transaction_time | from_last_query | wait | wait_us |      id       |      ptr       | coro | remote_pid | tls
------+------+----------+---------+--------------+-----------+-------+------------+------------+------------+------------------+-----------------+------+---------+---------------+----------------+------+------------+-----
 C    | user| postgres | pending |              | 127.0.0.1 | 34044 | 127.0.0.1  |       6001 |            |                  | 37972863        |    0 |       0 | c5e53da072c8d | 0x74ca2c19b060 |    1 |          0 |
 C    | user| console  | pending |              | 127.0.0.1 | 44022 | 127.0.0.1  |       6001 |            |                  |                 |    0 |       0 | c3f361506aedf | 0x74ca2c1aeaf0 |    1 |          0 |
(2 rows)
```

## case2

本例子展示连接池将多数前端连接映射到少数后端数据库连接的能力

设定数据库连接上限为50：

```SQL
postgres=# show max_connections ;
 max_connections
-----------------
 50
(1 row)

postgres=#
```

使用sysbench进行压测，设定连接数为500：

```SQL
sysbench --db-driver=pgsql --pgsql-host=127.0.0.1 --pgsql-port=6001 --pgsql-user=your_user --pgsql-db=your_database --table-size=1000 --tables=10 --time=600  --report-interval=5 --threads=500   oltp_read_write.lua run
```

使用psql连接内部控制台console，并用`SHOW POOLS`命令观察连接到代理的连接池信息：

```SQL
console=> show pools;
 database | user | cl_active | cl_idle | cl_waiting | sv_active | sv_idle | sv_used | sv_tested | sv_login |      sv_detail      | maxwait | maxwait_us |  pool_mode
----------+------+-----------+---------+------------+-----------+---------+---------+-----------+----------+---------------------+---------+------------+-------------
 postgres | user1 |         0 |       3 |          0 |         0 |       2 |       0 |         0 |        0 | {0/1/0, 0/1/0}      |       0 |          0 | transaction
 sbtest   | user1 |        47 |       1 |        452 |        47 |     113 |       0 |         0 |        0 | {47/1/447, 0/112/0} |       0 |          0 | transaction
 postgres | user1 |         0 |       0 |          0 |         0 |       1 |       0 |         0 |        0 | {0/1/0, 0/0/0}      |       0 |          0 | session
 console  | user1 |         0 |       1 |          0 |         0 |       0 |       0 |         0 |        0 | {}                  |       0 |          0 | session
```

可以观察到接入了500个客户端，超出后端设置的50个连接的上限