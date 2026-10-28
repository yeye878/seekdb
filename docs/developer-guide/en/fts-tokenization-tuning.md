# FTS tokenization tuning experiment

This experiment reduces repeated work in IK and BENG tokenization while retaining the existing character classification, token boundaries, and error handling. It is based on `2026-competition` at `5bce9e06d1281347ad61344e4b01fab9158c58a2`.

## Hot paths and changes

`ObFTCharUtil::do_classify()` previously decoded the same character separately for each category check, up to six times. It now decodes once and applies the original predicates in their original order, including the UTF16 surrogate cases. The IK parser already reads the current character, length, and type before dispatching segmenters; passing those values to `do_process()` removes repeated context reads. Segmenters consume the same cursor position and retain their original order and first-error propagation.

`ObTextWhitespaceTokenizer::get_next()` previously called the general character-length routine for every ASCII byte. The tokenizer now caches whether its opened charset is UTF8MB4 and uses length one for ASCII bytes in that charset. Other bytes and charsets still use the original routine. `found_delimiter()` combines the space, control, and punctuation flags from the same charset table in a single lookup, preserving the unsigned-byte index, sentinel offset, and null-table behavior. Reset and reopen refresh the cached charset state.

These are source-level repeated-work observations, supported by isolated differential tests and tokenizer measurements. No runtime CPU profile was collected. They do not establish that tokenization dominates total database time. Index construction and MATCH also perform work outside these paths.

## Correctness validation

Both baseline and candidate completed full RelWithDebInfo server builds and passed these mysqltest cases against separate fresh instances:

- `fts_index/ik_char_classification`: new TOKENIZE, MATCH, long-document, UPDATE, and DELETE regression.
- Existing `fts_index/cn_word`, `fts_index/tokenize_function`, and `fts_index/simple_query`.

Additional isolated checks covered 6,172,601 IK classification inputs, 140 IK dispatch cases, and 219,193 BENG tokenizer cases. Classification and tokenizer checks passed ASan/UBSan. The BENG checks include all one-byte and two-byte inputs, seeded random data, invalid UTF8, embedded NULs, reset/reopen, and charset fallbacks. The synthetic wide-charset adapter verifies fallback behavior; it is not a real UTF16 SQL integration test. Full local logs and extraction provenance are retained in the task submission evidence.

## Final implementation: one local benchmark pair

The unchanged `tools/benchmark/fts_large_bench.sh` was run serially for baseline and candidate with the official parameters:

```bash
LABEL=baseline ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 \
  SAMPLES=3 WARMUP=30 SKIP_LOAD=0 OUTPUT="$REPORT_FILE" \
  bash tools/benchmark/fts_large_bench.sh
```

Set `MYSQL` and `MYSQL_VERBOSE` to the dedicated instance and use a new report file for every run. Repeat with `LABEL=candidate` against the candidate server. The benchmark deletes and recreates its test database.

Both builds used the same task-local toolchain, RelWithDebInfo, Unity batch size 10, and four build jobs. Runtime was Ubuntu 24.04 under WSL on this computer, `memory_budget=4G`, `cpu_count=2`, with data on a fresh 8 GiB tmpfs. Direct startup avoided incompatible parameters injected by the installed OBD and block-file limitations on the Windows filesystem. This differs from the official evaluation environment.

| Metric | Baseline | Candidate | Time reduction |
| --- | ---: | ---: | ---: |
| IK title + content build (s) | 6.812 | 6.837 | -0.37% |
| IK content build (s) | 5.331 | 5.385 | -1.01% |
| BENG build (s) | 2.869 | 2.902 | -1.15% |
| IK TOKENIZE (ms) | 0.3334 | 0.2209 | +33.74% |
| BENG TOKENIZE (ms) | 0.1375 | 0.1722 | -25.24% |
| Chinese MATCH (ms) | 2.6616 | 2.2575 | +15.18% |
| English MATCH (ms) | 3.7345 | 3.6142 | +3.22% |
| Mixed MATCH (ms) | 2.1347 | 2.3543 | -10.29% |
| MATCH LIMIT (ms) | 3.3704 | 3.0407 | +9.78% |

The category means are build **-0.84%**, tokenize **+4.25%**, and query **+4.47%**, giving an equal-category mean of **+2.63%**. Positive values mean lower time; regressions retain their negative sign. MATCH counts were 8001, 11000, 7332, and 20 for both versions. Raw final reports and their comparison are in [fts-tokenization-tuning-evidence](fts-tokenization-tuning-evidence/).

This is one local pair, with visible sampling variance and a BENG TOKENIZE regression. It does not establish a stable overall speedup or justify dismissing individual regressions as noise. Three earlier pairs tested a candidate without the final delimiter change; they are excluded from the final-version result. A reverted retrieval experiment is also excluded.

The unchanged official scoring script outputs 100/100 for both the unmodified baseline and candidate because this local runtime is much faster than the fixed CI baseline. Those saturated scores are not an optimization gain or an official result. The final implementation still needs evaluation in the official uniform environment. The five `fts_large_bench*` files, input scale, SQL, timing, and scoring rules were not changed.

The reports retain `git_head: unknown` because the task wrapper invoked the benchmark from the task directory outside the source checkout. Published report copies use LF line endings; published BENG log excerpts omit unrelated garbled WSL launcher stderr. The original task logs are preserved unchanged, and the accompanying validation manifest records both original and published hashes. The baseline SHA above and the commit containing this document identify the compared source versions.
