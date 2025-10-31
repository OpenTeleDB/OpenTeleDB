# **开发规范**

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

    当用户有规则表达式查询，或者文本近似度查询的需求时，建议对字段使用trgm的gin索引，提升近似度匹配或规则表达式匹配的查询效率，同时覆盖了前后模糊的查询需求。如果没有创建trgm gin索引，则不推荐使用前后模糊查询例如like %xxxx%。

