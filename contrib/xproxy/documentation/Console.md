## Console 管控命令说明

xproxy 使用本地数据库 console 保存服务的统计信息，可以通过 psql 登录该数据库查看统计信息或者执行管控命令，详见文档：[Console.md](/document/xproxy.conf)

```SQL
Bash
$ psql -d console -h 127.0.0.1 -p 6432
console=> show help;
NOTICE:  
Console usage
        SHOW STATS|HELP|POOLS|POOLS_EXTENDED|DATABASES|SERVER_PREP_STMTS|SERVERS|CLIENTS
        SHOW LISTS|ERRORS|ERRORS_PER_ROUTE|VERSION|LISTEN|STORAGES
        KILL_CLIENT <client_id>
        RELOAD
        SET key=arg
        CREATE <module_path>
        DROP SERVERS|MODULE <servers>|<module>
SHOW
```

使用 show help 命令可以看到 xproxy 支持的控制台命令如下

```SQL
01 SHOW 命令
    1.1 SHOW STATS
    1.2 SHOW HELP
    1.3 SHOW POOLS
    1.4 SHOW POOLS_EXTENDED
    1.5 SHOW DATABASES
    1.6 SHOW SERVER_PREP_STMTS
    1.7 SHOW SERVERS
    1.8 SHOW CLIENTS
    1.9 SHOW LISTS
    1.10 SHOW ERRORS
    1.11 SHOW ERRORS_PER_ROUTE
    1.12 SHOW VERSION
    1.13 SHOW LISTEN
    1.14 SHOW STORAGES
02 KILL 命令
03 RELOAD 命令
04 SET 命令
05 CREATE 命令(模块管理命令，未涉及)
06 DROP 命令(模块管理命令，未涉及)
```

**01 SHOW 命令**

这些命令用于查看当前系统的状态和统计信息。

**1.1 SHOW** **STATS**

显示当前系统的整体统计信息，包括连接数、请求数、错误数等。

```SQL
SQL
console=> show stats;
-[ RECORD 1 ]---------------------+---------
database                          | postgres
total_xact_count                  | 26
total_query_count                 | 26
total_received                    | 676
total_sent                        | 5166
total_xact_time                   | 22734
total_query_time                  | 22734
total_pool_wait_time              | 1001912
avg_xact_count                    | 0
avg_query_count                   | 0
avg_recv                          | 0
avg_sent                          | 0
avg_xact_time                     | 0
avg_query_time                    | 0
avg_pool_wait_time                | 0
total_parse_count                 | 0
total_parse_count_reuse           | 0
total_rwsplit_count               | 26
total_rwsplit_wrong_count         | 0
total_rwsplit_blacklist_hit_count | 0
```

- **database**: 当前统计信息所属的数据库名称
- **total_xact_count**: 总事务数（包括读写事务）
- **total_query_count**: 总查询数（包括读写查询）
- **total_received**: 从客户端接收到的总数据量（以字节为单位）
- **total_sent**: 发送到客户端的总数据量（以字节为单位）
- **total_xact_time**: 所有事务的总执行时间（以毫秒为单位）
- **total_query_time**: 所有查询的总执行时间（以毫秒为单位）
- **total_pool_wait_time**: 所有连接池等待时间的总和（以毫秒为单位）
- **avg_xact_count**: 平均每秒事务数（每秒事务处理量）
- **avg_query_count**: 平均每秒查询数（每秒查询处理量）
- **avg_recv**: 平均每秒接收的数据量（以字节为单位）
- **avg_sent**: 平均每秒发送的数据量（以字节为单位）
- **avg_xact_time**: 平均每个事务的执行时间（以毫秒为单位）
- **avg_query_time**: 平均每个查询的执行时间（以毫秒为单位）
- **avg_pool_wait_time**: 平均每个连接池等待时间（以毫秒为单位）
- **total_parse_count**: 总解析次数（SQL 语句解析次数）
- **total_parse_count_reuse**: 总重用解析次数（SQL 语句重用次数）
- **total_rwsplit_count**: 总读写分离次数（读写分离操作次数）
- **total_rwsplit_wrong_count**: 总读写分离错误次数（读写分离失败次数）
- **total_rwsplit_blacklist_hit_count**: 总读写分离黑名单命中次数（读写分离黑名单命中次数）

**1.2 SHOW HELP**

显示所有可用的控制台命令

```SQL
Bash
console=> show help;
NOTICE:  
Console usage
        SHOW STATS|HELP|POOLS|POOLS_EXTENDED|DATABASES|SERVER_PREP_STMTS|SERVERS|CLIENTS
        SHOW LISTS|ERRORS|ERRORS_PER_ROUTE|VERSION|LISTEN|STORAGES
        KILL_CLIENT <client_id>
        RELOAD
        SET key=arg
        CREATE <module_path>
        DROP SERVERS|MODULE <servers>|<module>
```

**1.3 SHOW POOLS**

以 (database, user) 即 route 为单位展示当前所有连接池的状态和统计信息

```SQL
console=> show pools;
 database |    user    | cl_active | cl_waiting | sv_active | sv_idle | sv_used | sv_tested | sv_login | maxwait | maxwait_us |  pool_mode  
----------+------------+-----------+------------+-----------+---------+---------+-----------+----------+---------+------------+-------------
 postgres | postgres   |         0 |          4 |         0 |       6 |       0 |         0 |        0 |       0 |          0 | transaction
 postgres | user1 |         0 |          1 |         0 |       0 |       0 |         0 |        0 |       0 |          0 | transaction
 postgres | postgres   |         0 |          0 |         0 |       3 |       0 |         0 |        0 |       0 |          0 | session
 console  | user1 |         0 |          1 |         0 |       0 |       0 |         0 |        0 |       0 |          0 | session
(4 rows)
```

- **database**: 数据库名称，表示连接池对应的数据库
- **user**: 用户名，表示连接池对应的数据库用户
- **cl_active**: 当前活跃的客户端连接数
- **cl_waiting**: 当前等待的客户端连接数
- **sv_active**: 当前活跃的服务器连接数
- **sv_idle**: 当前空闲的服务器连接数
- **sv_used**: 当前正在使用的服务器连接数
- **sv_tested**: 当前经过测试的服务器连接数
- **sv_login**: 当前正在登录的服务器连接数
- **maxwait**: 最大等待时间（秒）
- **maxwait_us**: 最大等待时间（微秒）
- **pool_mode**: 连接池模式，例如 transaction 或 session

**1.4 SHOW POOLS_EXTENDED**

展示连接池的详细状态和统计信息

```SQL
console=> show pools_extended;
  database  |    user     | cl_active | cl_waiting | sv_active | sv_idle | sv_used | sv_tested | sv_login | maxwait | maxwait_us |  pool_mode  | bytes_recieved | bytes_sent | tcp_conn_count | query_0.95 | transaction_0.95 | pool_wait_0.95 | query_0.5 | transaction_0.5 | pool_wait_0.5
------------+-------------+-----------+------------+-----------+---------+---------+-----------+----------+---------+------------+-------------+----------------+------------+----------------+------------+------------------+----------------+-----------+-----------------+---------------
 postgres   | audit_admin |         0 |          2 |         0 |       2 |       0 |         0 |        0 |       0 |          0 | transaction |              0 |          0 |              2 |          0 |                0 |              0 |         0 |               0 |             0
 console    | th          |         0 |          1 |         0 |       0 |       0 |         0 |        0 |       0 |          0 | session     |              0 |          0 |              1 |          0 |                0 |              0 |         0 |               0 |             0
 postgres   | audit_admin |         0 |          0 |         0 |       1 |       0 |         0 |        0 |       0 |          0 | session     |              0 |          0 |              1 |          0 |                0 |         829163 |         0 |               0 |        829163
 aggregated | aggregated  |           |            |           |         |         |           |          |         |            |             |                |            |                |          0 |                0 |         829163 |         0 |               0 |        829163
(4 rows)
```

包含所有 show pools 包含的字段，并新增如下几个统计字段：

- **bytes_recieved**: 从客户端接收到的总数据量（以字节为单位）
- **bytes_sent**: 发送到客户端的总数据量（以字节为单位）
- **tcp_conn_count**: 当前的 TCP 连接数
- **query_${quantile}**: 请求的百分位延迟。
- **transaction_${quantile}**：事务的百分位延迟。
- **pool_wait_${quantile}**：连接池的百分位延迟。

其中百分位延迟由配置文件的enable_quantles_state和quantiles控制。enable_quantles_state表示是否使用统计相应连接池的的延迟信息，quantiles表示当前以哪些百分位来查看统计信息。

此外还会多出一行database=“aggregated”数据，它用来展示所有连接池聚合之后的百分位延迟信息。

**1.5 SHOW DATABASES**

显示当前配置的数据库相关属性和连接统计信息

```SQL
console=> show databases;
   name   |                     host                     | port |   database   |        force_user        | pool_size | reserve_pool |  pool_mode  | max_connections | current_connections | paused | disabled 
----------+----------------------------------------------+------+--------------+--------------------------+-----------+--------------+-------------+-----------------+---------------------+--------+----------
 postgres | 127.0.0.1:8888,127.0.0.1:8887,127.0.0.1:8886 | 8888 | watchdog_int | watchdog_int             |        10 |            0 | transaction |               0 |                   4 |      0 |        0
 postgres | 127.0.0.1:8888,127.0.0.1:8887,127.0.0.1:8886 | 8888 | default_db   | default_user             |         0 |            0 | transaction |               0 |                   1 |      0 |        0
 postgres | 127.0.0.1:8888,127.0.0.1:8887,127.0.0.1:8886 | 8888 | default_db   | user_aq_internal_pooling |         0 |            0 | session     |             107 |                   0 |      0 |        0
 console  |                                              |    0 | console      | default_user             |         0 |            0 | session     |               0 |                   1 |      0 |        0
(4 rows)
```

- **name**: 数据库的名称
- **host**: 数据库服务器的主机地址，可以是单个地址或多个地址（用逗号分隔）
- **port**: 数据库服务器的**默认**端口号，不指定端口时启用
- **database**: 配置文件中 rule 指定的数据库名称
- **force_user**: 配置文件中 rule 指定的数据库连接用户
- **pool_size**: 每个数据库的连接池大小
- **reserve_pool**: 预留的连接池大小，用于处理突发连接请求
- **pool_mode**: 连接池模式，例如 transaction（事务模式）或 session（会话模式）
- **max_connections**: 数据库允许的最大连接数
- **current_connections**: 当前活动的连接数
- **paused**: 是否暂停连接池（1 表示暂停，0 表示未暂停）
- **disabled**: 是否禁用连接池（1 表示禁用，0 表示未禁用）

**1.6 SHOW SERVER_PREP_STMTS**

显示服务器端预编译语句（prepared statements）的状态和统计信息

```SQL
console=> show server_prep_stmts;
 type | user | database | sid | definition | time 
------+------+----------+-----+------------+------
(0 rows)
```

- **type**: 记录的类型（通常为 S 表示后端服务器）
- **user**: 执行预编译语句的数据库用户
- **database**: 预编译语句所属的数据库名称
- **sid**: 服务器端的唯一标识（sever.id）
- **definition**: 预编译语句的定义（SQL 语句的文本内容）
- **time**: 预编译语句距离上次被执行的时间间隔

**1.7 SHOW SERVERS**

显示当前与 xproxy 建立连接的后端服务器（server）的状态和详细信息

```XML
console=> show servers;
 type |   user   | database | state |     addr      |     port      |  local_addr   |  local_port   | connect_time | request_time | wait | wait_us |      ptr      | link | remote_pid |  tls   | offline 
------+----------+----------+-------+---------------+---------------+---------------+---------------+--------------+--------------+------+---------+---------------+------+------------+--------+---------
 S    | postgres | postgres | idle  | <unix socket> | <unix socket> | <unix socket> | <unix socket> |              |              |    0 |       0 | sdb5d7e058ac2 |      |          0 | (null) | 0
 S    | postgres | postgres | idle  | <unix socket> | <unix socket> | <unix socket> | <unix socket> |              |              |    0 |       0 | s061a7a5d88bb |      |          0 | (null) | 0
 S    | postgres | postgres | idle  | <unix socket> | <unix socket> | <unix socket> | <unix socket> |              |              |    0 |       0 | s8a7f4e63c8b8 |      |          0 | (null) | 0
 S    | postgres | postgres | idle  | <unix socket> | <unix socket> | <unix socket> | <unix socket> |              |              |    0 |       0 | sec70002ec645 |      |          0 | (null) | 0
(4 rows)
```

- **type**: 记录的类型（通常为 S 表示服务端）
- **user**: 当前连接到服务器的数据库用户
- **database**: 当前连接的数据库名称
- **state**: 服务器的当前状态（例如，idle 表示空闲）
- **addr**: 服务器的地址（例如，<unix socket> 表示使用 Unix 套接字连接）
- **port**: 服务器的端口号
- **local_addr**: 本地地址
- **local_port**: 本地端口号
- **connect_time**: 连接到服务器的时间
- **request_time**: 最后一次请求的时间
- **wait**: 服务器等待的时间（以毫秒为单位）
- **wait_us**: 服务器等待的时间（以微秒为单位）
- **ptr**: 服务器的内部指针或标识符
- **link**: 服务器的连接状态
- **remote_pid**: 服务器的远程进程 ID
- **tls**: 服务器的 TLS（传输层安全）状态（如果为 (null)，表示没有使用 TLS）
- **offline**: 服务器是否离线（1 表示离线，0 表示在线）

**1.8 SHOW CLIENTS**

显示当前连接到 xproxy 的客户端（client）连接的状态和详细信息

```SQL
console=> show clients;
 type |    user    | database |  state  | storage_user |   addr    | port  | local_addr | local_port | query_time | transaction_time | from_last_query | wait | wait_us |      id       |      ptr       | coro | remote_pid | tls 
------+------------+----------+---------+--------------+-----------+-------+------------+------------+------------+------------------+-----------------+------+---------+---------------+----------------+------+------------+-----
 C    | user1 | postgres | pending |              | 127.0.0.1 | 41502 | 127.0.0.1  |       6432 |            |                  | 252230157270    |    0 |       0 | cf3e4f978f9a0 | 0x7fee6416ee30 |    1 |          0 | 
 C    | user1 | console  | pending |              | 127.0.0.1 | 42390 | 127.0.0.1  |       6432 |            |                  |                 |    0 |       0 | c35b0585ab994 | 0x7fee64181d30 |    4 |          0 | 
(2 rows)
```

- **type**: 记录的类型（C 表示客户端）
- **user**: 当前连接的客户端的数据库用户
- **database**: 当前连接的数据库名称
- **state**: 客户端的当前状态（例如，pending 表示等待中）
- **storage_user**: rule 中配置的用于访问指定 storage 的用户
- **addr**: 客户端的地址（例如，127.0.0.1 表示本地客户端）
- **port**: 客户端的端口号（例如，41502 表示客户端使用的端口号）
- **local_addr**: 本地地址（例如，127.0.0.1 表示 xproxy 监听的本地地址）
- **local_port**: 本地端口号（例如，6432 表示 xproxy 监听的本地端口号）
- **query_time**: 客户端查询的总时间
- **transaction_time**: 客户端事务的总时间
- **from_last_query**: 从上次查询到现在的时间间隔
- **wait**: 客户端等待的时间（以毫秒为单位）
- **wait_us**: 客户端等待的时间（以微秒为单位）
- **id**: 客户端的唯一标识符
- **ptr**: 客户端的内部指针或标识符
- **coro**: 客户端使用的协程标识符
- **remote_pid**: 客户端的远程进程 ID（字段未启用，默认为 0）
- **tls**: 客户端的 TLS（传输层安全）状态（如果为空，表示没有使用 TLS）

**1.9 SHOW LISTS**

展示当前系统中各种资源列表及其数量

```SQL
console=> show lists;
     list      | items 
---------------+-------
 databases     |     0
 users         |     0
 pools         |     4
 free_clients  |     0
 used_clients  |     2
 login_clients |     0
 free_servers  |     6
 used_servers  |     0
 dns_names     |     0
 dns_zones     |     0
 dns_queries   |     0
 dns_pending   |     0
(12 rows)
```

字段的具体含义：

- **list**: 资源列表的名称
- **items**: 该资源列表中的项目数量

list 字段内容具体解释：

- **databases**: 数据库列表中的项目数量
- **users**: 用户列表中的项目数量
- **pools**: 连接池列表中的项目数量
- **free_clients**: 空闲客户端列表中的项目数量
- **used_clients**: 已使用客户端列表中的项目数量
- **login_clients**: 登录客户端列表中的项目数量
- **free_servers**: 空闲服务器列表中的项目数量
- **used_servers**: 已使用服务器列表中的项目数量
- **dns_names**: DNS 名称列表中的项目数量
- **dns_zones**: DNS 区域列表中的项目数量
- **dns_queries**: DNS 查询列表中的项目数量
- **dns_pending**: DNS 待处理查询列表中的项目数量

**1.10 SHOW ERRORS**

展示系统中记录的错误类型及其对应的错误计数

```SQL
console=> show errors;
           error_type            | count 
---------------------------------+-------
 OD_ROUTER_ERROR                 |     0
 OD_ROUTER_ERROR_NOT_FOUND       |     0
 OD_ROUTER_ERROR_LIMIT           |     0
 OD_ROUTER_ERROR_LIMIT_ROUTE     |     0
 OD_ROUTER_ERROR_TIMEDOUT        |     0
 OD_ROUTER_ERROR_REPLICATION     |     0
 OD_EOOM                         |     0
 OD_EATTACH                      |     0
 OD_EATTACH_TOO_MANY_CONNECTIONS |     0
 OD_ESERVER_CONNECT              |     0
 OD_ESERVER_READ                 |     0
 OD_ESERVER_WRITE                |     0
 OD_ECLIENT_WRITE                |     0
 OD_ECLIENT_READ                 |     0
 OD_ESYNC_BROKEN                 |     0
 OD_ECATCHUP_TIMEOUT             |     0
(16 rows)
```

字段的具体含义：

- **error_type**: 错误类型，表示系统中定义的特定错误类别
- **count**: 该错误类型的计数，表示该错误类型发生的次数

error_type 具体类型解释如下：

- **OD_ROUTER_ERROR**: 路由器错误的计数
- **OD_ROUTER_ERROR_NOT_FOUND**: 路由器未找到错误的计数
- **OD_ROUTER_ERROR_LIMIT**: 路由器限制错误的计数
- **OD_ROUTER_ERROR_LIMIT_ROUTE**: 路由器限制路由错误的计数
- **OD_ROUTER_ERROR_TIMEDOUT**: 路由器超时错误的计数
- **OD_ROUTER_ERROR_REPLICATION**: 路由器复制错误的计数
- **OD_EOOM**: 内存不足错误的计数
- **OD_EATTACH**: 获取连接错误的计数
- **OD_EATTACH_TOO_MANY_CONNECTIONS**: 连接过多错误的计数
- **OD_ESERVER_CONNECT**: 服务器连接错误的计数
- **OD_ESERVER_READ**: 服务器读取错误的计数
- **OD_ESERVER_WRITE**: 服务器写入错误的计数
- **OD_ECLIENT_WRITE**: 客户端写入错误的计数
- **OD_ECLIENT_READ**: 客户端读取错误的计数
- **OD_ESYNC_BROKEN**: 同步错误的计数
- **OD_ECATCHUP_TIMEOUT**: 同步超时错误的计数

**1.11 SHOW ERRORS_PER_ROUTE**

按（user，database）表示的 route 显示错误信息和计数，字段类型与 show errors 一致

**1.12 SHOW VERSION**

显示 xproxy 的版本信息

```SQL
console=> show version;
      version      
-------------------
 136-b676edb-debug
(1 row)
```

**1.13 SHOW LISTEN**

显示 xproxy 监听的端口和相关配置

```SQL
console=> show listen;
 host | port |   tls   | tls_cert_file | tls_key_file | tls_ca_file | tls_protocols 
------+------+---------+---------------+--------------+-------------+---------------
 *    | 6432 | disable | (None)        | (None)       | (None)      | (None)
(1 row)
```

- **host**: 显示 xproxy 监听的主机地址，* 表示监听所有地址
- **port**: 显示 xproxy 监听的端口号
- **tls**: 显示是否启用了 TLS，disable 表示未启用
- **tls_cert_file**: 显示 TLS 证书文件的路径，(None) 表示未配置
- **tls_key_file**: 显示 TLS 密钥文件的路径
- **tls_ca_file**: 显示 TLS CA 文件的路径
- **tls_protocols**: 显示支持的 TLS 协议版本

**1.14 SHOW STORAGES**

显示 rule 中后端节点相关配置，以及对应存储配置中各节点运行状态信息

```SQL
console=> show storages;
      name       |  type  | endpoint_host | endpoint_port | endpoint_role | endpoint_weight | endpoint_select_cnt |   tls   | tls_cert_file | tls_key_file | tls_ca_file | tls_protocols 
-----------------+--------+---------------+---------------+---------------+-----------------+---------------------+---------+---------------+--------------+-------------+---------------
 postgres_server | remote | 127.0.0.1     |          8888 | primary       |        0.000000 |                   0 | disable | (None)        | (None)       | (None)      | (None)
 postgres_server | remote | 127.0.0.1     |          8887 | standby       |        0.500000 |                  13 | disable | (None)        | (None)       | (None)      | (None)
 postgres_server | remote | 127.0.0.1     |          8886 | standby       |        0.500000 |                  13 | disable | (None)        | (None)       | (None)      | (None)
 local           | local  | (None)        |             0 | (None)        |        0.000000 |                   0 | disable | (None)        | (None)       | (None)      | (None)
(4 rows)
```

- **name**: 存储的名称，用于标识存储资源
- **type**: 存储的类型，例如 remote 表示远程存储，local 表示本地存储
- **endpoint_host**: 存储配置中后端指定后端节点（endpoint）的主机地址
- **endpoint_port**: 后端节点的端口号
- **endpoint_role**: 后端节点的角色，例如 primary 表示主节点，standby 表示备节点
- **endpoint_weight**: 后端节点的权重，用于负载均衡
- **endpoint_select_cnt**: 后端节点在负载均衡过程中被选中执行 select 语句的次数 (数据可能随着route的回收而清空)
- **tls**: 是否启用 TLS（传输层安全）
- **tls_cert_file**: TLS 证书文件的路径
- **tls_key_file**: TLS 密钥文件的路径
- **tls_ca_file**: TLS CA 文件的路径
- **tls_protocols**: 支持的 TLS 协议版本

**1.15 SHOW RULES**

**1.16 SHOW GLOBAL_CONFIG**



**02 KILL 命令**

**KILL_CLIENT <client_id>**

终止指定的客户端连接，<client_id> 是客户端的唯一标识符

**03 RELOAD 命令**

重新加载 xproxy 的配置文件，无需重启服务
