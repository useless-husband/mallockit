-- sqlite3 CLI workload (tools/realprogs.py): build, index, group, join.
CREATE TABLE t(i INTEGER PRIMARY KEY, g INTEGER, s TEXT);
WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c WHERE x < 600000)
INSERT INTO t SELECT x, x % 997, printf('%08x%08d', (x * 2654435761) % 4294967296, x) FROM c;
CREATE INDEX t_g ON t(g, s);
SELECT g, count(*), sum(i), min(s) FROM t GROUP BY g ORDER BY g LIMIT 3;
SELECT count(*), sum(a.i) FROM t a JOIN t b ON a.g = b.g AND b.i = a.i + 997 WHERE a.i < 200000;
SELECT count(DISTINCT substr(s, 9)) FROM t;
SELECT group_concat(g, ',') FROM (SELECT DISTINCT g FROM t WHERE g < 20 ORDER BY g);
