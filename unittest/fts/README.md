# Focused FTS optimization regressions

These checks exercise the production implementations and the SQL behavior affected
by the FTS optimizations. The official benchmark and its fixed baseline are unchanged.

## Native regression tests

First finish a normal seekdb build with its pinned dependencies installed. The helper
uses that build's actual compiler flags and production link command, substitutes the
three test translation units for the server entry point, and links the installed
Google Test libraries. It does not rebuild seekdb or substitute mock implementations.

From the repository root:

```bash
python3 unittest/fts/build_regressions.py \
  --build build_release \
  --out /tmp/seekdb-fts-regressions \
  --execute

/tmp/seekdb-fts-regressions/fts_regression_tests
```

Omit `--execute` to validate the build metadata and show the intended outputs without
compiling. The output directory must be outside the source and build trees. The
executed compiler and linker commands are saved in `build-plan.json` there.

The 12 tests cover:

- Word aggregation: first-occurrence frequency, repeated words, collation equality,
  first spelling, forced bucket collisions, error propagation, and parser-helper
  reuse across populated and empty documents.
- IK classification: all 1,112,064 valid UTF-8 Unicode scalar values, every UTF-16
  code unit in both byte orders, and malformed, truncated, and null inputs. The
  unchanged production predicates in the old order provide the comparison oracle.
- String comparison: every supported collation, both trailing-space modes, empty
  strings, embedded NUL, Unicode and malformed byte sequences, and invalid inputs.
  Results are compared with the original production collation handler.

## SQL regression tests

The two new cases are in the existing `fts_index` mysqltest suite:

- `no_score_projection`: independent expected counts and IDs; observable score
  materialization as an oracle for threshold and top-k behavior; prepared statements;
  joins, residual predicates, grouping, distinct and multiple aggregates; limit and
  offset; heap document IDs; DML and rollback.
- `count_only_boundaries`: 255/256/257/512/513 matching documents, duplicate and
  overlapping postings, batch-boundary limits, prepared-statement reuse, bulk-built
  and incrementally maintained indexes, updates/deletes, transaction visibility,
  rollback, and an independently materialized scored result.

They run through the repository's existing mysqltest discovery. To execute only these
cases against a dedicated local test instance, adjust the client connection as needed:

```bash
export LD_LIBRARY_PATH="$PWD/deps/3rd/u01/obclient/lib:$PWD/deps/3rd/u01/obclient/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
FTS_SUITE=tools/deploy/mysql_test/test_suite/fts_index
FTS_LOG_DIR=$(mktemp -d /tmp/seekdb-fts-mysqltest.XXXXXX)

for FTS_CASE in no_score_projection count_only_boundaries; do
  deps/3rd/u01/obclient/bin/mysqltest \
    --host=127.0.0.1 --port=2881 --user=root \
    --tmpdir="$FTS_LOG_DIR" --logdir="$FTS_LOG_DIR" --silent \
    --test-file="$FTS_SUITE/t/$FTS_CASE.test" \
    --result-file="$FTS_SUITE/r/mysql/$FTS_CASE.result" || exit
done
```

These fixtures create and drop their own `fts_no_score_projection` and
`fts_count_boundaries` databases. They do not change the benchmark database.

## Exact TOKENIZE differential

Capture a reference using the original server binary, then run the same command with
`--reference` against the candidate server:

```bash
python3 unittest/storage/fts_word_aggregation_diff.py \
  --mysql 'mysql -h127.0.0.1 -P2881 -uroot' \
  --save-reference /tmp/fts-tokenize-reference.json

python3 unittest/storage/fts_word_aggregation_diff.py \
  --mysql 'mysql -h127.0.0.1 -P2881 -uroot' \
  --reference /tmp/fts-tokenize-reference.json
```

This compares 533 complete JSON results byte for byte, including token order and
first spelling. It covers five parsers, the three supported UTF-8 collations, both
output modes, a binary-collation BENG fallback, empty/NULL input, Unicode, embedded
NUL, repeated words, long words, and alternating input rows. It also checks independent
frequency and empty-input invariants. All statements in this differential are reads.
