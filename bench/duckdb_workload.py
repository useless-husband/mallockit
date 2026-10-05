"""DuckDB queries used as a realistic workload (tools/realprogs.py).

Generates a 4-million-row table inside DuckDB and runs grouping, a self
join, string functions and a window function. Prints one JSON line with a
digest of every result, so runs under different allocators can be
compared for identical answers, and the elapsed time."""
import hashlib
import json
import sys
import time

import duckdb

QUERIES = [
    "CREATE TABLE t AS SELECT range AS i, range % 1000 AS g, md5(range::VARCHAR) AS s FROM range(4000000)",
    "SELECT g, count(*), sum(i), max(s), min(length(s)) FROM t GROUP BY g ORDER BY g",
    "SELECT count(*), sum(a.i) FROM t a JOIN t b ON a.i = b.i + 7 WHERE a.g < 300",
    "SELECT count(DISTINCT substr(s, 1, 5)), string_agg(DISTINCT substr(s, 1, 1), '' ORDER BY substr(s, 1, 1)) FROM t",
    "SELECT sum(d) FROM (SELECT i - lag(i) OVER (PARTITION BY g ORDER BY i) AS d FROM t)",
    "SELECT list_sort(list(DISTINCT g % 17))[1:5], count(*) FROM t WHERE s LIKE '%ab%'",
    "SELECT g, quantile_cont(i, 0.5) FROM t GROUP BY g ORDER BY g LIMIT 5",
]


def main():
    threads = int(sys.argv[1]) if len(sys.argv) > 1 else 4
    con = duckdb.connect(":memory:", config={"threads": threads})
    digest = hashlib.sha256()
    t0 = time.perf_counter()
    for q in QUERIES:
        rows = con.execute(q).fetchall()
        digest.update(repr(rows).encode())
    print(json.dumps({"seconds": time.perf_counter() - t0, "digest": digest.hexdigest()[:16],
                      "duckdb": duckdb.__version__, "threads": threads}))


if __name__ == "__main__":
    main()
