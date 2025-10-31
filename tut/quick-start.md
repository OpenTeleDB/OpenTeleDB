# **快速入门**

## **安装**OpenTeleDB

### **准备软硬件安装环境**

本章节描述安装前需要进行的环境准备。建议部署OpenTeleDB的各服务器具有等价的服务器配置。

### **硬件环境要求**

表1硬件环境要求列出了OpenTeleDB服务器应具备的最低硬件要求，在实际产品中，硬件配置的规划需考虑数据规模及所期望的数据库响应速度。请根据实际情况进行规划。

表1：硬件环境要求

| **项目** | **配置描述**                                                 |
| -------- | ------------------------------------------------------------ |
| 内存     | 功能调试建议8GB以上。性能调试或商业部署建议16GB以上。复杂的查询对内存的需求量比较高，在高并发场景下，可能出现内存不足。此时建议使用大内存的机器，或使用负载管理限制系统的并发。 |
| CPU      | 功能调试最小1*8核2.0GHz。性能调试和商业部署建议1*16GHz。 说明：个人开发者最低配置2核4G, 推荐配置4核8G。 |
| 硬盘     | 用于安装TeleDB的硬盘需满足如下要求：建议至少10GB硬盘空间，具体需求取决于数据库的大小和增长预期。 |

 

**软件环境要求**

表2：软件环境要求

| **软件类型**  | **配置描述**                                           |
| ------------- | ------------------------------------------------------ |
| Linux操作系统 | Linux 各主流发行版(CentOS, RedHat, Ubuntu, Debian)等。 |

 

**软件依赖要求**

表3：软件依赖要求

| **所属软件**   | **建议版本**  |
| -------------- | ------------- |
| gcc            | 4.8 及以上    |
| gcc-c++        | 4.8 及以上    |
| make           | 3.82 及以上   |
| bison          | 3.0 及以上    |
| flex           | 2.5.31 及以上 |
| readline-devel | 6.0 及以上    |
| zstd-devel     | 1.4.0 及以上  |
| lz4-devel      | 1.8.0 及以上  |
| openssl-devel  | 1.1.1 及以上  |

 

### **安装软件包**

**获取安装包**

您可参考如下步骤获取安装包。

1. 从OpenTeleDB开源社区下载对应平台的安装包。

   1. 登录OpenTeleDB开源社区。
   2. 选择对应平台，单击**下载**，获取最新安装包。

2. 解压安装包。

   执行如下命令解压安装包，检查安装目录及文件是否齐全。

   ```
   tar -zxvf xxxx.tar.gz
   ```

3. 或者使用git clone拉取源代码。进入解压目录，解压安装包。

4. 安装相关依赖

   根据自己需要的增加依赖。以下列举部分依赖。

   ```
   yum install -y curl-devel libicu-devel pam-devel krb5-devel openldap-devel systemd-devel readline readline-devel zlib zlib-devel gettext gettext-devel openssl openssl-devel pam pam-devel libxml2 libxml2-devel libxslt libxslt-devel perl perl-devel tcl-devel uuid-devel gcc gcc-c++ make flex bison perl-ExtUtils* libcurl-devel asciidoc xmlto opensp mariadb-devel libtool libuuid-devel gflags-devel lcov libyaml-devel boost boost-devel libgsasl-devel cmake3 golang
   ```

5. 编译安装

   编译选项与PostgreSQL差别在于OpenTeleDB多了--with-xstore编译参数，加上此编译参数则会编译xstore模块代码。

   编译xraft编译选项需要：--with-zstd  --with-lz4 --with-xraft --with-openssl。

   ```
   export codes_dir= 路径 #源码目录路径
   
   export pg_install_dir= 路径 #OpenTeleDB安装路径
   
   ./configure --prefix=${pg_install_dir} --with-libxml --with-uuid=ossp --with-openssl --with-xstore  #在解压出来的源码目录下执行，配置编译选项
   
   make && make install
   ```

6. 按需安装contrib插件工具

   以xstore工具举例：

   ```
   cd ${codes_dir}/contrib/xstore
   
   make && make install
   ```

### **初始化数据库和启动**

1. 数据库初始化，修改数据库配置文件。

   ```
   ${pg_install_dir}/bin/initdb -D ${pg_data_dir}
   
   echo "shared_preload_libraries = 'xstore.so'" >> ${pg_data_dir}/postgresql.conf #使用xstore功能需要配置此项
   
   echo "shared_preload_libraries = 'xraft.so'" >> ${pg_data_dir}/postgresql.conf #使用xraft功能需要配置此项
   ```

2. 启动数据库

   ```
   export pg_data_dir=${pg_install_dir}/data
   
   ${pg_install_dir}/bin/pg_ctl -D ${pg_data_dir} start
   ```

   显示server started则为启动成功。

## **访问OpenTeleDB**

### **psql连接**

执行如下命令连接到OpenTeleDB数据库实例。

```
psql -h 实例地址 -p 端口 -U dbadmin -d postgres
```



### **应用程序连接**

#### **Java连接**

1. Jdbc包下载地址：https://jdbc.postgresql.org/download.html

2. 链接实例示例代码如下：

   ```
   import java.sql.Connection;
   
   import java.sql.DriverManager;
   
   import java.sql.Statement;
   
    
   
   public class createtable {
   
     public static void main( String args[] )
   
      {
   
       Connection c = null;
   
       Statement stmt = null;
   
       try {
   
        Class.forName("org.postgresql.Driver");
   
        c = 
   
   DriverManager.getConnection("jdbc:postgresql://127.0.0.1:15432/postgres?currentSchema=public&binaryTransfer=false","openteledb", "openteledb");
   
        System.out.println("Opened database successfully");
   
        stmt = c.createStatement();
   
   //输入待执行语句
   
        String sql = "create table test(id int,nickname text)" ;
   
        stmt.executeUpdate(sql);
   
        stmt.close();
   
        c.close();
   
       } catch ( Exception e ) {
   
        System.err.println( e.getClass().getName()+": "+ e.getMessage() );
   
        System.exit(0);
   
       }
   
       System.out.println("Table created successfully");
   
      }
   
   }
   ```

 

####  **C程序连接**

1. 连接数据库示例代码如下：

   ```
   \#include <stdio.h>  
   
   \#include <stdlib.h>  
   
   \#include "libpq-fe.h"   
   
   int
   
   main(int argc, char **argv){
   
     const char *conninfo;
   
     PGconn   *conn;    
   
     if (argc > 1){
   
       conninfo = argv[1];
   
     }else{
   
       conninfo = "dbname = postgres";  
   
     }       
   
     conn = PQconnectdb(conninfo);
   
     if (PQstatus(conn) != CONNECTION_OK){
   
       fprintf(stderr, "连接数据库失败: %s",PQerrorMessage(conn));        
   
     }else{
   
       printf("连接数据库成功！\n");
   
     }
   
     PQfinish(conn);
   
     return 0;
   
   }
   ```

2. 编译

   ```
   gcc -c -I /usr/local/install/teledbx_pgxz/include/ conn.c  
   
   gcc -o conn conn.o -L /usr/local/install/teledbx_pgxz/lib/ -lpq
   ```

3. 运行

   ```
   ./conn "host=xxx.xx.x.x dbname=postgres port=11000"  
   
   连接数据库成功！
   
   ./conn "host=xxx.xx.x.x dbname=postgres port=15432 user=openteledb"  
   
   连接数据库成功！ 
   ```

#### **Python程序连接**

1. 安装psycopg2模块

2. 链接数据库示例代码如下:

   ```
   \#coding=utf-8
   
   \#!/usr/bin/python
   
   import psycopg2
   
   try:
   
     conn = psycopg2.connect(database="postgres", user="openteledb", password="", host="xxx.xx.x.x", port="15432")
   
     print "连接数据库成功"
   
     conn.close()
   
   except psycopg2.Error,msg:
   
     print "连接数据库出错，错误详细信息： %s" %(msg.args[0])
   ```

3.运行

```
[root@VM_0_29_centos python]$ python conn.py 
```

连接数据库成功。  

#### **PHP程序连接**

1. 链接数据库示例代码如下：

```
<?php   

$host="xxx.xx.x.x";

$port="15432";

$dbname="postgres";

$user="openteledb" ;

$password="";  

 

//连接数据库

$conn=@pg_connect("host=$host port=$port dbname=$dbname user=$user password=$password");    

if (!$conn){

  $error_msg=@pg_errormessage($conn); 

  echo "连接数据库出错，详情：".$error_msg."\n<BR>"; ;

  exit;

}else{

  echo "连接数据库成功"."\n<BR>";    

} 

//关闭连接

pg_close($conn);

?>
```

2.运行

```
[root@VM_0_47_centos test]# curl http://127.0.0.1:8080/dbsta/test/conn.php
```

连接数据库成功。

## **使用**OpenTeleDB

本节描述使用数据库的基本操作。通过此节您可以完成创建数据库、创建表及向表中插入数据和查询表中数据等操作。

### **前提条件**

OpenTeleDB数据库正常运行。

### **操作步骤**

1. 连接数据库

   ```
   psql -h 实例地址 -p 端口 -d postgres
   ```

2. 创建数据库

   ```
   Create database test;
   ```

3. 创建表、插入数据、查询

   ```
   \c test
   
   Create table test(a int , b int);
   
   Insert into test values(1,1),(2,2);
   
   Select * from test;
   ```

