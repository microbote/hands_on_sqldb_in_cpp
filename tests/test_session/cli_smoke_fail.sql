-- CLI 冒烟脚本：最后一条是错的，退出码应为 1
CREATE DATABASE smoke2;
USE smoke2;
CREATE TABLE t (id INT PRIMARY KEY, v INT);
INSERT INTO t (id, v) VALUES (1, 10);
SELECT * FROM missing_table;
