# XStore

XStore是OpenTeleDB针对高并发的OLTP场景、要求性能稳定的业务所设计的原位更新存储引擎。  
XStore通过重新设计堆表xheap、重新设计索引xbtree，每次数据更新时，直接在原位更新数据，旧数据写入Undo，从而解决了数据空间膨胀的问题。同时通过原位更新和回滚管理机制，自动vacuum不再需要扫盘回收数据页和索引页，彻底解决垃圾回收带来的性能波动。

## 快速入门

### 编译安装

```bash
export pg_install_dir= /xx/xx   #安装目录
export pg_data_dir=${pg_install_dir}/data  

cd xx/postgres/ #代码目录
./configure --prefix=${pg_install_dir} --with-xstore  
make && make install 
cd contrib/xstore  
make && make install
```
    
### 初始化数据库和启动
1. 数据库初始化，修改数据库配置文件。  
    ```bash
    ${pg_install_dir}/bin/initdb -D ${pg_data_dir}
    echo "shared_preload_libraries = 'xstore.so'" >> ${pg_data_dir}/postgresql.conf #使用xstore功能需要配置此项
    ```

2. 启动数据库  
    ```bash
    export pg_data_dir=${pg_install_dir}/data  
    ${pg_install_dir}/bin/pg_ctl -D ${pg_data_dir} start
    显示server started则为启动成功。
    ```

### XStore使用

XStore的SQL语法与PostgreSQL的heap表使用基本一致，差别在于建表与建索引上存在区别。

- #### XStore使用前提

1. 编译参数带上--with-xstore 编译安装成功
2. postgresql.conf配置文件添加配置 shared_preload_libraries = 'xstore.so'
3. 创建插件 
    ```SQL
    create extension xstore;
    ```

- #### 创建XStore存储引擎表
1. 创建XStore存储引擎表  
    ```SQL
    create table xt1(a int primary key, b int) using xstore;
    ```

2. GUC参数配置指定XStore存储引擎  
数据库配置文件配置 default_table_access_method = 'xstore'，需重启生效配置  
    ```SQL
    create table xt1(a int primary key, b int);
    ```

- #### XStore索引使用xbtree
1. 不指定创建索引类型，XStore表默认创建xbtree索引  
    ```SQL
    create index xbt_idx1 on xt1(a);
    ```

2. 使用using xbtree关键字
    ```SQL
    create index xbt_idx2 on xt1 using xbtree(a);
    ```



