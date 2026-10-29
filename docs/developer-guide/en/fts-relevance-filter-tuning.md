# FTS query merge and relevance filtering (T12 v2)

The DaaT merge loop evaluates the MATCH score before applying its pushed-down
predicate. A direct `WHERE MATCH(...)` then passes that double through the generic
expression evaluator for each candidate document, although only its truth value
is needed at this point.

The new path recognizes a vectorized `T_OP_BOOL` whose only child is exactly the
double relevance projection expression. It tests `0.0 != relevance`, matching
`calc_bool_expr_for_double_type`, and leaves other predicates on the generic
path. All posting scans, merging, BM25 estimation, relevance calculation and
output projection remain intact. It does not assume every posting match has a
nonzero score.

Before each batch the iterator invalidates the BOOL expression's evaluated bits,
using the evaluation context's capacity. A shared consumer can therefore evaluate
it against the newly projected scores rather than reuse a previous batch. Scalar
execution and score comparisons retain their existing behavior.

The merge comparator also selects a direct bytewise function for binary-collated
CHAR/VARCHAR document IDs. It preserves NULL-first ordering, prefixes, and all
bytes including trailing spaces and NULs, and normalizes the result as before.
Other collations and types retain the existing comparator. Shared sorting code
is unchanged.

## Correctness and build

`fts_index.relevance_bool_filter` contains 1,540 input rows and checks every ID
and exact relevance against a general `!= 0` predicate. It also exercises shared
Boolean expressions, repeat execution, LIMIT/OFFSET, score sorting, empty and
unmatched queries, NULL text, Boolean mode, numeric thresholds, updates and
deletes. Expected output was recorded from the preserved v1 executable.

An additional source-extracted differential harness checks the actual new
comparator against the old nullable wrapper and binary collation bodies over
665,726 cases, plus identical stable sorting of 4,096 IDs. Release and
AddressSanitizer/UndefinedBehaviorSanitizer runs pass. The harness models the
charset dispatcher only for valid binary inputs; database integration tests
remain separate.

The final candidate and original competition baseline each pass six mysqltests:
the new case, `ik_char_classification`, `cn_word`, `tokenize_function`,
`simple_query`, and `domain_id_merge_with_compare_null`. Both executables are
full RelWithDebInfo/Unity builds using the same configuration. The original
baseline is `5bce9e06d1281347ad61344e4b01fab9158c58a2`; v2 includes the previous
tokenizer changes in parent `fb2893cb57f30a2ed256df1f556416ee1b7fb41f`.

## Measurements and limits

The two final local report pairs show build -6.97%, tokenization -17.33%, and
query +10.61% improvement, for a three-category mean of **-4.56%**. Thus these
runs do **not** establish an overall speedup. Concurrent host compilation was
observed and even SELECT 1 varied substantially; the LIMIT result includes a
large baseline outlier. These observations must not be presented as a stable
query gain or a predicted platform score. All raw reports, including regressions,
are retained.

Final measurements are recorded in `fts-relevance-filter-tuning-evidence/`.
`comparison.txt` and `comparison.json` average two raw reports per version and
then apply the official three-category weighting. The four full runs use
candidate/baseline/baseline/candidate order, a fresh dedicated tmpfs instance
each time, memory_budget=4G, cpu_count=2, and the same CPU affinity (logical CPUs
0 and 2, on different physical cores) for server and client. The host is WSL2
Ubuntu 24.04. These are local measurements, with remaining host scheduling noise.

All official benchmark files, data generation, SQL, timing, scoring and baseline
remain byte-identical to the competition base; `source-manifest.json` records
their hashes and the final source hashes. Benchmark parameters are the unchanged
20,000 rows, batch 500, tokenization rounds 3,000, query rounds 200, samples 3,
warmup 30, SKIP_LOAD=0. Query hit counts are 8001/11000/7332/20; full correctness
is checked separately by mysqltest.

The raw reports were generated before the v2 commit, so their Git field records
the parent and a dirty working tree. The binary hashes in the validation manifest
and source hashes tie these reports to the tested implementation. Intermediate
v2 reports used a different invalidation length and are excluded.

The public fixed scorer saturates at 100/100 on both local versions. That value
does not establish a code speedup or predict a platform score. Previous user
platform reports used an unavailable scoring reference; v1's first 10.038/50
result and subsequent 0/50 results remain historical evidence. v2 has not been
submitted to the platform by this task.
