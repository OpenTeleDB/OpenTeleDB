# **应用开发指南**

以下内容为开发设计规范，包括命名规范、COLUMN设计、Constraints设计、Index设计、关于NULL和开发相关规范。

## **命名规范**

1. DB object：database， schema， table， column， view， index， sequence， function， trigger 等名称。

- 建议使用小写字母、数字、下划线的组合。

- 建议不使用双引号即"包围，除非必须包含大写字母或空格等特殊字符。

- 长度不能超过63个字符。

- 不建议以 pg_ 开头（避免与系统 DB object 混淆），不建议以数字开头。

- 禁止使用 SQL 关键字，如 type， order 等。

2. table 能包含的 column 数目，根据字段类型的不同，数目在250到1600之间。

3. 临时或备份的DB object：table、view 等，建议加上日期， 如 dba_ops.b2c_product_summay_2014_07_12 (其中 dba_ops 为 DBA 专用 schema)。

4. index命名规则为：普通索引为 表名_列名_idx，唯一索引为 表名_列名_uidx，如 student_name_idx，student_id_uidx。

## **COLUMN设计**

1. 建议能用数值类型的，就不用字符类型。

2. 建议能用varchar(N)就不用char(N)，以利于节省存储空间。

3. 建议能用varchar(N)就不用text，varchar。

4. 建议使用default NULL，而不用 default ''，以节省存储空间。

5. 建议如有国际货业务的话，使用timestamp with time zone(timestamptz)，而不用 timestamp without time zone，避免时间函数在对于不同时区的时间点返回值不同，也为业务国际化扫清障碍。

6. 建议使用NUMERIC(precision，scale)来存储货币金额和其它要求精确计算的数值， 而不建议使用real，double precision。

## **Constraints 设计**

1. 建议建表时一步到位把主键或者唯一索引也一起建立。

## **Index 设计**

1. OpenTeleDB提供的index类型：B-tree，Hash，GiST (Generalized Search Tree)，SP-GiST (space-partitioned GiST)，GIN (Generalized Inverted Index)，BRIN (Block Range Index)，目前不建议使用Hash，通常情况下使用B-tree，Xstore使用xbtree。

2. 建议用unique index代替unique constraints，便于后续维护。

3. 建议对where中带多个字段and条件的高频query，参考数据分布情况，建多个字段的联合index。

4. 建议对固定条件的（一般有特定业务含义）且选择时数据占比低的query，建议带 where的Partial Indexes。

   ```
   select * from test where status=1 and col=?; -- 其中status=1为固定的条件
   
   create index on test (col) where status=1;
   ```

5. 建议对经常使用表达式作为查询条件的query，可以使用表达式或函数索引加速 query。

   ```
   select * from test where exp(xxx);
   
   create index on test ( exp(xxx) );  
   ```

6. 建议不要建过多index，一般不要超过6个，核心table（产品，订单）可适当增加 index个数。

## **关于 NULL**

1. NULL的判断：IS NULL，IS NOT NULL。  

   注意
   boolean 类型取值 true，false，NULL。
   NOT IN 集合中带有 NULL 元素。

   ```
   teledb=# select * from t_oids;
   
    id | name |     birth     | city 
   
   ----+------+---------------------+------
   
    1 | 张三 | 2000-12-01 00:00:00 | 北京
   
    2 | 李四 | 1997-03-24 00:00:00 | 上海
   
    3 | 王五 | 2004-09-01 00:00:00 | 广州
   
   (3 rows)
   
   teledb=# select * from t_oids where id not in (null);
   
    id | name | birth | city 
   
   ----+------+-------+------
   
   (0 rows)
   ```

2. 建议对字符串型 NULL 值处理后，再进行 || 操作。

   ```
   teledb=# select id,name from t_oids limit 1;
   
    id | name 
   
   ----+------
   
    1 | 张三
   
   (1 row)
   
   
   teledb=# select id,name||null from t_oids limit 1;
   
    id | ?column? 
   
   ----+----------
   
    1 | 
   
   (1 row)
   
    
   
   teledb=# select id,name|| coalesce(null,'') from t_oids limit 1;
   
    id | ?column? 
   
   ----+----------
   
    1 | 张三
   
   (1 row)
   ```

 

3. 建议使用 count(1) 或 count(*) 来统计行数，而不建议使用 count(col) 来统计行数，因为 NULL 值不会计入。

   **说明**

   count(多列列名)时，多列列名必须使用括号，例如count( (col1，col2，col3) )，注意多列的count，即使所有列都为NULL，该行也被计数，所以效果与count(*)一致。

   ```
   teledb=# select * from t_oids ;
   
    id | name |     birth     | city 
   
   ----+------+---------------------+------
   
    1 | 张三 | 2000-12-01 00:00:00 | 北京
   
    2 | 李四 | 1997-03-24 00:00:00 | 上海
   
    3 | 王五 | 2004-09-01 00:00:00 | 广州
   
    4 | 陈六 | 2022-01-01 00:00:00 | 
   
   (4 rows)
   
   
   teledb=# select count(city) from t_oids;
   
    count 
   
   \-------
   
      3
   
   (1 row)
   
    
   
   teledb=# select count(1) from t_oids;
   
    count 
   
   \-------
   
      4
   
   (1 row)
   
   
   teledb=# select count(*) from t_oids;
   
    count 
   
   \-------
   
      4
   
   (1 row)
   
   
   teledb=# select count(id) from t_oids;
   
    count 
   
   \-------
   
      4
   
   (1 row)
   
   teledb=# select count((id,city)) from t_oids;
   
    count 
   
   \-------
   
      4
   
   (1 row)
   ```

   

4. count(distinct col) 计算某列的非 NULL 不重复数量，NULL 不被计数。

   count(distinct (col1，col2，...) ) 计算多列的唯一值时，NULL 会被计数，同时 NULL 与 NULL 会被认为是相同的。

   ```
   teledb=# select count(distinct city) from t_oids;
   
    count 
   
   \-------
   
      3
   
   (1 row)
   
    
   
   teledb=# select count(distinct (id, city)) from t_oids;
   
    count 
   
   \-------
   
      4
   
   (1 row)
   ```

   

5. 两个 NULL 的对比方法。

   ```
   teledb=# select null is not  distinct from null;
   
    ?column? 
   
   \----------
   
    t
   
   (1 row)
   ```

   

## **开发相关规范**

1. 建议对DB object尤其是COLUMN加COMMENT，便于后续了解业务及维护

   注释前后的数据表可读性对比，有注释的一看就明白。

   ```
   teledb=# \d+ t_oids;
   
                      Table "public.t_oids"
   
    Column |        Type        | Collation | Nullable | Default | Storage  | Stats target | Descripti
   
   on 
   
   --------+--------------------------------+-----------+----------+---------+----------+--------------+----------
   
   \---
   
    id   | integer             |      | not null |     | plain   |        | 
   
    name  | character varying        |      |      |     | extended |        | 
   
    birth  | timestamp(0) without time zone |      |      |     | plain   |        | 
   
    city  | character varying        |      |      |     | extended |        | 
   
   Indexes:
   
     "t_oids_pkey" PRIMARY KEY, btree (id)
   
   Has OIDs: yes
   
   Distribute By: SHARD(id)
   
   Location Nodes: ALL DATANODES
   
                  ^
   
   teledb=# comment on column t_oids.name is '姓名';
   
   COMMENT
   
   teledb=# comment on column t_oids.city is '居住城市';
   
   COMMENT
   
   teledb=# \d+ t_oids;
   
                      Table "public.t_oids"
   
    Column |        Type        | Collation | Nullable | Default | Storage  | Stats target | Descripti
   
   on 
   
   --------+--------------------------------+-----------+----------+---------+----------+--------------+----------
   
   \---
   
    id   | integer             |      | not null |     | plain   |        | 
   
    name  | character varying        |      |      |     | extended |        | 姓名
   
    birth  | timestamp(0) without time zone |      |      |     | plain   |        | 
   
    city  | character varying        |      |      |     | extended |        | 居住城市
   
   Indexes:
   
     "t_oids_pkey" PRIMARY KEY, btree (id)
   
   Has OIDs: yes
   
   Distribute By: SHARD(id)
   
   Location Nodes: ALL DATANODES
   ```

   

2. 建议非必须时避免select *，只取所需字段，以减少包括不限于网络带宽消耗。

   ```
   teledb=# explain select * from t_oids;
   
             QUERY PLAN              
   
   \----------------------------------------------------------------
   
    Remote Fast Query Execution  (cost=0.00..0.00 rows=0 width=0)
   
     Node/s: dn01, dn02
   
     ->  Seq Scan on t_oids  (cost=0.00..16.30 rows=630 width=76)
   
   (3 rows)
   
    
   
   teledb=# explain select id from t_oids;
   
            QUERY PLAN              
   
   \---------------------------------------------------------------
   
    Remote Fast Query Execution  (cost=0.00..0.00 rows=0 width=0)
   
     Node/s: dn01, dn02
   
     ->  Seq Scan on t_oids  (cost=0.00..16.30 rows=630 width=4)
   
   (3 rows)
   ```

   

3. 建议update时尽量做<>判断，如update table_a set column_b = c where column_b <> c；

   ```
   teledb=# update t_oids set city = '测试';
   
   UPDATE 4
   
   teledb=# select xmin,* from t_oids;
   
    xmin | id | name |     birth     | city 
   
   ------+----+------+---------------------+------
   
    1181 |  1 | 张三 | 2000-12-01 00:00:00 | 测试
   
    1147 |  3 | 王五 | 2004-09-01 00:00:00 | 测试
   
    1147 |  4 | 陈六 | 2022-01-01 00:00:00 | 测试
   
    1181 |  2 | 李四 | 1997-03-24 00:00:00 | 测试
   
   (4 rows)
   
   
   teledb=# update t_oids set city = '测试';
   
   UPDATE 4
   
   teledb=# select xmin,* from t_oids;
   
    xmin | id | name |     birth     | city 
   
   ------+----+------+---------------------+------
   
    1182 |  1 | 张三 | 2000-12-01 00:00:00 | 测试
   
    1182 |  2 | 李四 | 1997-03-24 00:00:00 | 测试
   
    1148 |  3 | 王五 | 2004-09-01 00:00:00 | 测试
   
    1148 |  4 | 陈六 | 2022-01-01 00:00:00 | 测试
   
   (4 rows)
   
   teledb=# update t_oids set city = '测试' where city != '测试';
   
   UPDATE 0
   
   teledb=# select xmin,* from t_oids;
   
    xmin | id | name |     birth     | city 
   
   ------+----+------+---------------------+------
   
    1182 |  1 | 张三 | 2000-12-01 00:00:00 | 测试
   
    1182 |  2 | 李四 | 1997-03-24 00:00:00 | 测试
   
    1148 |  3 | 王五 | 2004-09-01 00:00:00 | 测试
   
    1148 |  4 | 陈六 | 2022-01-01 00:00:00 | 测试
   
   ```

   

   上面的效果是一样的，但带条件的更新不会产生一个新的版本记录，不需要系统执行 vacuum 回收垃圾数据。

4. 建议将单个事务的多条SQL操作，分解、拆分，或者不放在一个事务里，让每个事务的粒度尽可能小，尽量lock少的资源，避免lock 、dead lock的产生。

   ```
   会话1 把所有数据都更新但不提交，锁住了所有数据
   
   teledb=# begin;
   
   BEGIN
   
   teledb=# update t_oids set city = 'city';
   
   UPDATE 4
   
   会话2 等待
   
   teledb=# update t_oids set city = 'session2';
   
   会话3 等待
   
   teledb=# update t_oids set city = 'session3';
   ```

   如果会话1分批更新的话，则会话2和会话3中就能部分提前完成，这样可以避免大量的锁等待和出现大量的session占用系统资源，在做全表更新时请使用这种方法来执行。

5. 建议大批量的数据入库时，使用copy，不建议使用insert，以提高写入速度。

6. 建议复杂的统计查询可以尝试窗口函数。

7. 对于频繁更新的表，建议建表时指定表的fillfactor=85，每页预留15%的空间给HOT更新使用。 

   ```
   create table test123(id int, info text) with(fillfactor=85);  
   ```

   

8. 使用外键时，一定要设置fk的action，例如cascade，set null，set default。

   ```
   create table tbl2(id int references tbl(id) on delete cascade on update cascade, info text); 
   ```

   

9. 建议有定期历史数据删除需求的业务，表按时间分区，删除时不要使用DELETE操作，而是DROP或者TRUNCATE对应的表。

10. 设计时应尽可能选择合适的数据类型，能用数字的坚决不用字符串，能用树类型的，坚决不用字符串。 使用好的数据类型，可以使用数据库的索引，操作符，函数，提高数据的查询效率。

11. 对于固定条件的查询，可以使用部分索引，减少索引的大小，同时提升查询效率。

    ```
    select * from test where id=1 and col=?; -- 其中id=1为固定的条件  
    
    create index idx on tbl (col) where id=1;  
    ```

12. 对于经常使用表达式作为查询条件的语句，可以使用表达式或函数索引加速查询。

    ```
    select * from test where exp(xxx);  
    
    create index idx on tbl ( exp );
    ```

13. 当用户有规则表达式查询，或者文本近似度查询的需求时，建议对字段使用trgm的gin索引，提升近似度匹配或规则表达式匹配的查询效率，同时覆盖了前后模糊的查询需求。如果没有创建trgm gin索引，则不推荐使用前后模糊查询例如like %xxxx%。