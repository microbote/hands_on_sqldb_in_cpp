-- CLI 冒烟脚本：全部语句都应成功（退出码 0）
CREATE DATABASE smoke;
USE smoke;
CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(16) NOT NULL, v INT);
INSERT INTO t (id, name, v) VALUES (1, 'a', 10);
INSERT INTO t (id, name, v) VALUES (2, 'b', 20);
UPDATE t SET v = 11 WHERE id = 1;
DELETE FROM t WHERE id = 2;
EXPLAIN SELECT id, name FROM t WHERE v >= 10 ORDER BY id DESC LIMIT 5;
EXPLAIN ANALYZE SELECT id FROM t WHERE v >= 10;
\l
\dt
\d t
\dt smoke
SELECT id, name, v FROM t ORDER BY id;
