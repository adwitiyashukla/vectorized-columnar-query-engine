# vectorized-columnar-query-engine

A columnar query engine written from scratch in C++20, built as vcqe on the command line.
It loads delimited text into its own columnar file format and answers SQL over it a batch
of values at a time instead of a row at a time.

I wrote it to find out where the speed of an analytical database actually comes from, so
the engine can run the same query plan in two modes. One mode pushes batches of 2048
values through the operators. The other walks one tuple at a time through virtual calls,
the way a textbook Volcano engine does. Both modes read the same files and build the same
plan, so the time difference is the execution model and nothing else.

## What it does

The storage layer writes row groups of 65536 rows. Each column chunk inside a row group is
encoded on its own: dictionary plus bit packed codes for repeated strings, bit packing
against the chunk minimum for integers and decimals, plain values when encoding would not
help. Every chunk carries its minimum, its maximum, a null count, a distinct count and a
CRC32C checksum. The footer holds the schema and all chunk metadata and has its own
checksum.

The query layer parses a subset of SQL: filters with and, or, not, between, in and like,
arithmetic on integers, exact decimals and doubles, date literals with interval math, inner
joins, group by with sum, count, min, max and avg, having, order by and limit. The planner
pushes single table predicates into the scan, reads only the columns the query needs, and
orders the joins using the distinct counts stored in the file footer, so a join on a key
column is preferred over one on a low cardinality column. Execution is parallel over row
groups, with a hash table per thread for aggregation and a merge at the end.

Filters compare with AVX2 kernels when the processor has them. The choice is made at
runtime with a CPUID check, and the scalar version runs otherwise.

## Numbers

Measured on my laptop: Intel Core 5 120U, 10 cores and 12 threads, 7.7 GB of RAM, Windows
11, GCC 15.2, release build. The data is scale factor 1 from the generator in this repo,
8,661,590 rows across the eight TPC-H tables.

Loading all eight tables turned 1080.55 MB of text into 515.08 MB of columnar files in
149.24 seconds. The lineitem table alone went from 771.98 MB to 304.77 MB, a ratio of 2.53.

Query times in seconds, best of three runs after one warmup:

| query | 12 threads | 1 thread | one tuple at a time | gain from batching |
| ----- | ---------: | -------: | ------------------: | -----------------: |
| q1    |      0.267 |    1.382 |               2.737 |               2.0x |
| q3    |      0.333 |    0.642 |               3.013 |               4.7x |
| q5    |      0.245 |    1.115 |               5.698 |               5.1x |
| q6    |      0.057 |    0.286 |               0.673 |               2.4x |
| q10   |      0.418 |    1.147 |               2.685 |               2.3x |

The last column divides the one tuple at a time time by the single threaded vectorized
time. Both run the same plan over the same files.

Batch size matters on its own. Q1 on one thread, with only the vector size changed:

| values per batch | seconds |
| ---------------: | ------: |
|                1 |   4.960 |
|                8 |   1.843 |
|               64 |   1.449 |
|              512 |   1.451 |
|             2048 |   1.326 |
|             8192 |   1.365 |

A batch of one is the same code doing the same work one value at a time, and it is 3.7
times slower than a batch of 2048. The curve flattens by 64 values, which is where a batch
and its intermediate results sit in L1 cache.

Minimum and maximum values per chunk let the scan skip whole row groups. A range filter on
the clustered key column reads 131,072 rows instead of 6,001,560:

```
vcqe query --dir data --sql "select count(*), sum(l_extendedprice) from lineitem where l_orderkey between 4000000 and 4100000"
1 rows in 0.015 seconds  [scanned 131072 rows, skipped 90 of 92 row groups]

vcqe query --dir data --sql "select count(*), sum(l_extendedprice) from lineitem where l_orderkey between 4000000 and 4100000" --no-zone-maps
1 rows in 0.070 seconds  [scanned 6001560 rows, skipped 0 of 92 row groups]
```

## Checking the results

The five queries in queries/ are TPC-H Q1, Q3, Q5, Q6 and Q10. I ran them on data from the
official dbgen tool at scale factor 1 and the results match the answer set that ships with
it, down to the cent. The generator in this repo writes TPC-H style data with the same
schema and the same row counts, and that is what the numbers above use, so the benchmark
runs anywhere without building dbgen first.

tools/difftest.py loads the same text files into SQLite, generates random queries over the
tables and compares both engines row by row. The last local run was 85 queries with no
differences. The six test binaries hold 44 cases over the file format, the encodings, the
kernels, the parser, the planner, the operators and deliberately corrupted files. Every
test passes under the address and undefined behavior sanitizers and under the thread
sanitizer.

## Build and run

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

```
build/vcqe gen --dir data --scale 1
build/vcqe load --dir data --all
build/vcqe query --dir data --file queries/q1.sql
build/vcqe bench --dir data --queries queries --runs 5 --sweep
```

`query` takes `--explain` to print the plan, `--mode row` to run the one tuple at a time
executor, `--threads n`, `--vector-size n`, `--no-zone-maps` and `--no-simd`. `tables`
lists what is loaded.

## Configuration

config.toml holds every tunable and is validated on load, so a bad value or an unknown key
is an error and not a surprise later. Storage settings cover the row group size, the
dictionary limits, bit packing and checksums. Execution settings cover the mode, the vector
size, the thread count, the hash table load factor and switches for SIMD, zone maps, column
pruning, predicate pushdown and join reordering.

## License

MIT
