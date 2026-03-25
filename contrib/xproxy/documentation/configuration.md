# 配置项说明

| **参数名**                           | **类型** | **说明**                                                     | **热重载** |
| ------------------------------------ | -------- | ------------------------------------------------------------ | ---------- |
| daemonize                            | bool     | 设置是否后台运行进程，默认no                                 | NO         |
| unix_socket_dir                      | string   | unix 套接字目录。和OpenTeleDB混合部署时，建议和OpenTeleDB使用同一个目录。 | NO         |
| unix_socket_mode                     | string   | unix套接字的文件权限。如果设置了unix_socket_dir，则不能为空  | NO         |
| use_unix_socket_if_possible          | bool     | 如为YES, 在可用时自动通过UNIX SOCKET与OpenTeleDB通信         | NO         |
| locks_dir                            | string   | 用于存放文件锁的目录                                         | NO         |
| graceful_die_on_errors               | bool     | 用于设置优雅退出。当开启时，如果进程接收到SIGUSER2信号不会直接退出，而是停止接收新客户端连接，并继续处理等待旧客户端连接上的事务结束。默认no | NO         |
| enable_online_restart                | bool     | 用于设置在线重启。当开启时，可以在不停止旧实例的情况下，直接启动新的实例，旧实例会自动进入优雅退出状态。开启时，需要设置bindwith_reuseport为YES | NO         |
| bindwith_reuseport                   | bool     | 用于设置是否重用监听端口。当启用在线重启功能时需要开启。     | NO         |
| priority                             | integer  | 进程调度的优先级，范围为-20到19，-20优先级最高，19为最低优先级。默认值为0. | NO         |
| pid_file                             | string   | pid文件的路径。默认为空.                                     | NO         |
| include                              | string   | 其他配置文件的路径                                           | NO         |
| sequential_routing                   | string   | 是否按照配置文件定义的顺序匹配规则                           | NO         |
|                                      |          |                                                              |            |
| **日志相关配置**                     |          |                                                              |            |
| **参数名**                           | **类型** | **说明**                                                     | **热重载** |
| log_file                             | string   | 日志文件                                                     | NO         |
| log_format                           | string   | 日志格式，不能为空                                           | NO         |
| log_to_stdout                        | bool     | 是否将日志额外输出到标准输出。默认开启                       | NO         |
| log_syslog                           | bool     | 是否将日志写入到系统日志中                                   | NO         |
| log_syslog_ident                     | string   | 写系统日志时的标识符                                         | NO         |
| log_syslog_facility                  | string   | 写系统日志时指定当前进程的类别                               | NO         |
| log_debug                            | bool     | 是否输出debug日志                                            | NO         |
| log_session                          | bool     | 是否将客户端建立/断开连接的事件打印到日志中。默认开启。      | NO         |
| log_stats                            | bool     | 是否周期性地将工作进程的统计信息打印到日志中。默认开启       | NO         |
| stats_interval                       | integer  | 用于设置内部统计信息更新和打印日志的周期，单位为秒。默认60s  | NO         |
| **性能相关配置**                     |          |                                                              |            |
| **参数名**                           | **类型** | **说明**                                                     | **热加载** |
| workers                              | integer  | 工作线程的数量。默认值为1.                                   | NO         |
| resolvers                            | integer  | DNS解析线程的数量。默认值为1                                 | NO         |
| readahead                            | integer  | 每个连接的读缓冲区的长度，单位字节。默认值8192               | NO         |
| cache_coroutine                      | integer  | 协程池的大小，默认为0.                                       | NO         |
| coroutine_stack_size                 | integer  | 协程栈的大小，单位为页。默认为4（即16KB）。                  | NO         |
| nodelay                              | bool     | 是否启用连接的nodelay，默认开启。                            | NO         |
| keepalive                            | integer  | TCP keepalive的时间，单位秒，默认15s                         | NO         |
| keepalive_keep_interval              | integer  | TCP keepalive的周期，单位秒，默认5s                          | NO         |
| keepalive_probes                     | integer  | TCP keepalive探测的次数。默认为3                             | NO         |
| keepalive_usr_timeout                | integer  | TCP 超时时间。默认0                                          | NO         |
|                                      |          |                                                              |            |
| **全局限制相关配置**                 |          |                                                              |            |
| **参数名**                           | **类型** | **说明**                                                     | **热加载** |
| client_max                           | integer  | 客户端连接的上限。默认值为0，表示无限制。                    | YES        |
| client_max_routing                   | integer  | 客户端连接并行“路由”的上限，“路由”是指建立TLS、登录认证的阶段。默认为工作线程数的16倍。 | YES        |
| server_login_retry                   | integer  | 当OpenTeleDB报错“Too many clients”的时候，等待重试的时间，单位ms。默认为1 | YES        |
|                                      |          |                                                              |            |
| **hba****配置**                      |          |                                                              |            |
| **参数名**                           | **类型** | **说明**                                                     | **热加载** |
| hba_file                             | string   | hba文件的路径，可以是相对路径，也可以是绝对路径              | YES        |
|                                      |          |                                                              |            |
|                                      |          |                                                              |            |
| **监听****端口****的配置**           |          |                                                              |            |
| **参数名**                           | **类型** | **说明**                                                     | **热加载** |
| host                                 | string   | 监听的IP地址。"*"表示所有地址，空则表示监听unix socket       | NO         |
| port                                 | integer  | 监听的端口                                                   | NO         |
| backlog                              | integer  | TCP listen backlog的长度。默认128，当瞬时建立的连接数很多时可以增加该值。 | NO         |
| compression                          | bool     | 是否开启OpenTeleDB的数据压缩协议。默认值为no                 | NO         |
| client_login_timeout                 | integer  | 客户端登录认证的超时时间，单位毫秒。默认值为15000            | NO         |
| port_attrs                           | string   | 用于指定端口的读写属性。可以是： （1）“read-write”，根据用户规则进行自动读写分离 （2）“read-only”，只读端口，总是访问备机 （3）“write-only”，只写端口1，总是访问主机。 | NO         |
|                                      |          |                                                              |            |
| **Storage配置**                      |          |                                                              |            |
| **参数名**                           | **类型** | **说明**                                                     | **热加载** |
| type                                 | string   | storage的类型。可以是： (1) “remote”。表示OpenTeleDB服务器 (2) “local”，表示XProxy本身（用于管理员执行运维操作）。 | YES        |
| port                                 | integer  | OpenTeleDB服务器的默认端口。                                 | YES        |
| target_session_attrs                 | string   | 默认访问OpenTeleDB服务器的主/从节点。可以是“read-write”、“read-only”、“any" | YES        |
| server_max_routing                   | integer  | 限制与OpenTeleDB服务器“建立连接”的并发数（注意，不是与OpenTeleDB服务器的连接数）。默认值为工作线程的数量 | YES        |
|                                      |          |                                                              |            |
| **Storage的endpoints**               |          |                                                              |            |
| **参数名**                           | **类型** | **说明**                                                     | **热加载** |
| hostname                             | string   | hostname                                                     | YES        |
| port                                 | integer  | OpenTeleDB后端端口，例如5432                                 | YES        |
| weight                               | float    | 负载均衡时，该节点的权重，例如0.5                            | YES        |
| application_name                     | string   |                                                              | YES        |
|                                      |          |                                                              |            |
| **Storage的watchdog**                |          |                                                              |            |
| **参数名**                           | **类型** | **说明**                                                     | **热加载** |
| storage                              | string   | storage的名称                                                | YES        |
| storage_db                           | string   | watchdog连接的目标OpenTeleDB数据库                           | YES        |
| storage_user                         | string   | watchdog登录目标OpenTeleDB使用的用户名                       | YES        |
| storage_password                     | string   | watchdog登录目标OpenTeleDB使用的密码。使用md5认证时既可以是明文密码，也可以是密码的md5值；使用scram-sha-256认证时，则只能指定为明文密码。 | YES        |
| pool_routing                         | string   | watchdog使用的连接池类型。必须明确指定为“internal”，即对客户端不可见的连接池。 | YES        |
| pool                                 | string   | watchdog使用的连接池模式。可以为“transaction”、“session”     | YES        |
| pool_size                            | integer  | watchdog使用的连接池的大小限制和。默认是0，表示无限制。      | YES        |
| pool_timeout                         | integer  | watchdog使用的连接池的超时时间。                             | YES        |
| log_debug                            | bool     | watchdog是否打印debug级别的日志                              | YES        |
| replication_delay_threshold          | integer  | 如果主备复制延迟小于这个值，则认为是可以转发读请求的备节点。单位us | YES        |
| catchup_timeout                      | interger | 如果节点心跳延迟小于这个值，则认为是有效的节点。单位s        | YES        |
|                                      |          |                                                              |            |
| **“数据库-用户”规则相关配置**        |          |                                                              |            |
| **参数**                             | **类型** | **说明**                                                     | **热加载** |
| authentication                       | string   | 认证类型。可以是“none”、“block”、“clear_text”、“md5”、“scram-she-256”、“cert”，需与后端OpenTeleDB的认证方式保持一致 | YES        |
| allow_clear_text_frontend_auth       | bool     | 用于设置客户端以明文方式进行登录，从而获取客户端的明文密码。 | YES        |
| auth_common_name                     | string   | 用于cert认证                                                 | YES        |
| target_server_attrs                  | string   | 指定读写分离类型。可以是： （1） “read-write”，访问主 （2） “read-only”，访问备 （3） “auto”，自动读写分离 | YES        |
| client_max                           | integer  | 连接池级别的最大客户端连接上限，默认无限制。 注意：设置为0时表示不允许任意客户端登录。 | YES        |
| pool                                 | string   | 连接池模式。可以是“transaction”、“session”，分别表示事务级连接池，和会话级连接池。 | YES        |
| pool_size                            | integer  | 连接池的大小限制。默认0，表示无限制。                        | YES        |
| pool_timeout                         | integer  | 等待连接池空闲连接的超时时间，超时时会端口客户端的连接，单位ms。默认为0，表示无限等待。 | YES        |
| pool_ttl                             | integer  | 连接池中后端连接的空闲时间，当空闲时间超过该值会被cron线程关闭。默认为0。 | YES        |
| pool_cancel                          | bool     | 客户端退出时，如果后端连接仍在执行查询，发送cancel中止其执行，关闭时会直接断开后端连接。默认开启。 | YES        |
| pool_rollback                        | bool     | 客户端退出时，如果后端连接仍在事务中，发送rollback回滚事务，关闭时会直接断开后端连接。默认开启。 | YES        |
| pool_client_idle_timeout             | integer  | 客户端连接不在事务中的最大空闲时间，超过该空闲时间则会断开客户端连接。单位秒。默认为0，不超时。 | YES        |
| pool_idle_in_transaction_timeout     | integer  | 客户端连接在事务中的最大空闲时间，超过该空闲时间则会断开客户端连接。单位秒。默认为0，不超时。 | YES        |
| pool_idle_timeout_whitelist          | string   | 客户端空闲连接查杀白名单，白名单内的用户连接在事务内或不在事务内，超过空闲时间都不会断开 | YES        |
| pool_reserve_prepared_statement      | bool     | 后端连接是否保留客户端使用PBE协议Prepare的语句，如果需要对PBE协议进行自动读写分离，则需要开启该功能。默认关闭。 | YES        |
| pool_prepared_statement_limit        | integer  | 后端连接保留Prepare的语句的最大数量，超过该值，则最近最不常使用的语句会被close掉。默认为0，表示不限制保留的Prepare语句的数量。 | YES        |
| pool_prepared_statement_expired_time | integer  | 后端连接保留Prepare的语句的最大空闲时间，空闲时间超过该值的语句会被close掉。默认为0，表示不限制Prepare的语句的空闲时间。 | YES        |
| client_fwd_error                     | bool     | 是否将和OpenTeleDB建立连接时的错误传递给客户端。             | YES        |
| server_lifetime                      | integer  | 后端连接的存活时间上限，为了避免长期允许的OpenTeleDB进程cache占用太大内存，单位秒，默认值3600。 | YES        |
| log_debug                            | bool     | 是否打印debug日志                                            | YES        |
| enable_quantles_state                | bool     | 是否使用百分位统计                                           | YES        |
| role                                 | string   | 登录XProxy本地库时使用的角色。可以是“admin”、“stat”、“notallow”，其中“admin”可以执行加载配置的功能，“stat”则只能查看XProxy的监控信息。 | YES        |
| quantiles                            | string   | 登录XProxy本地库时查看事务、查询耗时，使用的百分位列表。可以是多个值，用逗号分隔。例如: "0.95,0.5" | YES        |
| replication_delay_threshold          | integer  | 如果主备复制延迟小于这个值，则认为是可以转发读请求的备节点。单位us | YES        |
| catchup_timeout                      | interger | 如果节点心跳延迟小于这个值，则认为是有效的节点。单位s        | YES        |
| enable_read_only_blacklist           | bool     | 误判只读语句并收到插件报错时，把该语句记录在黑名单中         | YES        |

