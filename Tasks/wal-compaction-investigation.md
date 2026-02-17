# WAL Compaction Investigation (Performance-First, Evidence-Driven)

## Scope and status
- Scope covered:
  - `WaxCore` WAL and commit/replay path
  - index compaction interaction (`FTS5 VACUUM`) vs WAL behavior
  - investigation-only instrumentation and benchmark harness (no broad refactor)
- Workspace status:
  - Baseline snapshot metrics below were taken from existing logs in a moving workspace.
  - Final implementation go/no-go should use a re-run on a pinned commit.

## Deliverables produced
1. Baseline report: this file (`/Users/chriskarani/CodingProjects/Wax/Tasks/wal-compaction-investigation.md`)
2. Baseline artifact JSON: `/Users/chriskarani/CodingProjects/Wax/Tasks/wal-compaction-baseline.json`
3. Harness + support:
   - `/Users/chriskarani/CodingProjects/Wax/Tests/WaxIntegrationTests/WALCompactionBenchmarks.swift`
   - `/Users/chriskarani/CodingProjects/Wax/Tests/WaxIntegrationTests/WALCompactionWorkloadSupport.swift`
4. WAL instrumentation tests:
   - `/Users/chriskarani/CodingProjects/Wax/Tests/WaxCoreTests/WALCompactionInstrumentationTests.swift`

## Current WAL lifecycle and bottlenecks
### Lifecycle (today)
1. `put`/`putBatch` appends payload bytes then WAL records.
2. WAL capacity is checked before append; if full, write path can trigger synchronous commit.
3. `commit` applies pending WAL mutations into TOC, writes staged indexes, writes TOC/footer, fsyncs, updates header, fsyncs again, checkpoints WAL.
4. `open` restores footer/TOC and replays pending WAL.

### Hot bottlenecks (code anchors)
1. Capacity-triggered synchronous commit in write path:
   - `/Users/chriskarani/CodingProjects/Wax/Sources/WaxCore/Wax.swift:166`
   - `/Users/chriskarani/CodingProjects/Wax/Sources/WaxCore/Wax.swift:186`
2. Commit fsync path (footer fsync + header fsync):
   - `/Users/chriskarani/CodingProjects/Wax/Sources/WaxCore/Wax.swift:1236`
   - `/Users/chriskarani/CodingProjects/Wax/Sources/WaxCore/Wax.swift:1253`
3. WAL append syscall overhead (record + sentinel writes):
   - `/Users/chriskarani/CodingProjects/Wax/Sources/WaxCore/WAL/WALRingWriter.swift:137`
   - `/Users/chriskarani/CodingProjects/Wax/Sources/WaxCore/WAL/WALRingWriter.swift:146`
   - `/Users/chriskarani/CodingProjects/Wax/Sources/WaxCore/WAL/WALRingWriter.swift:370`
4. Open/recovery double WAL scan:
   - `/Users/chriskarani/CodingProjects/Wax/Sources/WaxCore/Wax.swift:448`
   - `/Users/chriskarani/CodingProjects/Wax/Sources/WaxCore/Wax.swift:453`
5. Index compaction and growth trigger:
   - `/Users/chriskarani/CodingProjects/Wax/Sources/Wax/Orchestrator/MemoryOrchestrator+Maintenance.swift:164`
   - `/Users/chriskarani/CodingProjects/Wax/Sources/WaxTextSearch/FTS5SearchEngine.swift:492`
   - `/Users/chriskarani/CodingProjects/Wax/Sources/WaxCore/Wax.swift:1160`
   - `/Users/chriskarani/CodingProjects/Wax/Sources/WaxCore/Wax.swift:1187`

## Baseline metrics (existing evidence)
### Sources
- `/tmp/wax-wal-commit-baseline.log`
- `/tmp/wax_probe_commit_modes.log`
- `/tmp/wax_probe_pressure.log`
- `/tmp/wax_probe_compact_run.log`
- `/tmp/wax_probe_reopen_large.log`

### Commit latency
- `wax_commit_incremental_batch64`: p50 `12.5ms`, p95 `14.8ms`, p99 `15.0ms`
- `stage_for_commit_incremental_batch64`: p50 `19.5ms`, p95 `24.4ms`, p99 `24.6ms`
- `text_only_commit`: p50 `9.0ms`, p95 `10.2ms`, p99 `10.6ms`
- `hybrid_commit`: p50 `11.0ms`, p95 `12.5ms`, p99 `13.5ms`

### WAL pressure behavior
- 3,000 puts under tiny WAL produced 24 auto-commit events (`~125 puts/event`).
- Put latency during pressure: p50 `11.6ms`, p95 `14.4ms`, p99 `14.6ms`.

### File growth under repeated compaction
- After initial flush: `281,634,796` logical bytes.
- After 8 `compactIndexes` runs: `343,403,260` logical bytes.
- Growth: `+61,768,464` bytes (`~7.72MB` per compaction run).

### Reopen/recovery latency at larger file size
- File size: `343,403,260` logical / `89,812,992` allocated.
- Open+close: p50 `333ms`, p95 `533ms`, p99 `601ms`.

## Full standard matrix run (pinned snapshot)
- Run date: `2026-02-17`
- Commit SHA: `91e681fea54fce8143ba7bef69616b8ae830fb35`
- Command:
```bash
WAX_BENCHMARK_WAL_COMPACTION=1 \
WAX_BENCHMARK_SCALE=standard \
WAX_WAL_REOPEN_ITERATIONS=7 \
WAX_WAL_SAMPLE_EVERY_WRITES=250 \
WAX_BENCHMARK_WAL_OUTPUT=/Users/chriskarani/CodingProjects/Wax/Tasks/wal-compaction-baseline.json \
swift test --filter WALCompactionBenchmarks
```
- Artifact:
  - `/Users/chriskarani/CodingProjects/Wax/Tasks/wal-compaction-baseline.json`

### Key results by workload
- `small_text`: commit p50/p95/p99 `10.14/11.50/11.59ms`, auto-commit `0`, checkpoints `10`, reopen p95 `1270.15ms`
- `small_hybrid`: commit p50/p95/p99 `12.97/14.20/14.51ms`, auto-commit `0`, checkpoints `10`, reopen p95 `1259.61ms`
- `medium_text`: commit p50/p95/p99 `21.68/34.53/36.05ms`, auto-commit `0`, checkpoints `50`, reopen p95 `1280.78ms`
- `medium_hybrid`: commit p50/p95/p99 `36.52/69.70/70.81ms`, auto-commit `0`, checkpoints `50`, reopen p95 `1309.52ms`
- `large_text_10k`: commit p50/p95/p99 `36.21/72.62/74.76ms`, auto-commit `0`, checkpoints `50`, reopen p95 `1363.52ms`
- `large_hybrid_10k`: commit p50/p95/p99 `62.08/126.99/129.06ms`, auto-commit `0`, checkpoints `50`, reopen p95 `1303.82ms`
- `sustained_write_text`: commit p50/p95/p99 `241.94/241.94/241.94ms`, auto-commit `25`, wrap `25`, checkpoints `26`, reopen p95 `1359.85ms`
- `sustained_write_hybrid`: commit p50/p95/p99 `64.69/123.76/173.38ms`, auto-commit `0`, wrap `22`, checkpoints `157`, reopen p95 `1335.89ms`

### Observed patterns
1. Commit tail grows materially with workload size and hybrid mode.
2. Sustained tiny-WAL text workload shows explicit capacity pressure (`25` auto-commits, `25` wraps), validating write-path pressure bottleneck.
3. Reopen p95 is consistently high (roughly `1.25s` to `1.36s`) for large WAL-size configurations, reinforcing replay/open optimization priority.

## Failure and recovery implications
1. Tail-latency amplification risk:
   - When WAL fills, put path can block on `commitLocked()`, pushing write p95/p99 up.
2. Recovery cost drift:
   - Larger file footprints and duplicated/rewritten index blobs increase open/replay work and cold-open tails.
3. Storage health concern:
   - Repeated compaction growth without strong idempotency guard indicates live-set inefficiency.
4. Crash safety posture remains correct today:
   - Two-phase durability (footer then header) plus WAL replay keeps correctness, but costs latency.

## Investigation instrumentation added
### Additive WAL runtime stats API
- New `WaxWALStats` and `Wax.walStats()`:
  - `/Users/chriskarani/CodingProjects/Wax/Sources/WaxCore/Wax.swift:15`
  - `/Users/chriskarani/CodingProjects/Wax/Sources/WaxCore/Wax.swift:1789`
- Fields include:
  - `walSize`, `writePos`, `checkpointPos`, `pendingBytes`, `committedSeq`, `lastSeq`, `wrapCount`, `checkpointCount`, `sentinelWriteCount`, `autoCommitCount`

### Writer/commit counters
- `WALRingWriter` counters:
  - `wrapCount`, `checkpointCount`, `sentinelWriteCount`
  - `/Users/chriskarani/CodingProjects/Wax/Sources/WaxCore/WAL/WALRingWriter.swift:21`
- `Wax` auto-commit counter:
  - `/Users/chriskarani/CodingProjects/Wax/Sources/WaxCore/Wax.swift:180`
  - `/Users/chriskarani/CodingProjects/Wax/Sources/WaxCore/Wax.swift:201`

### Validation tests
- `/Users/chriskarani/CodingProjects/Wax/Tests/WaxCoreTests/WALCompactionInstrumentationTests.swift`
- Covers:
  - wrap/checkpoint/sentinel counter increments
  - walStats sequence/checkpoint semantics
  - capacity-triggered auto-commit counter behavior

## Measurement harness (workload matrix)
### Implemented matrix
1. `small_text`: 500 writes, text-only, commit every 50
2. `small_hybrid`: 500 writes, hybrid, commit every 50
3. `medium_text`: 5,000 writes, text-only, commit every 100
4. `medium_hybrid`: 5,000 writes, hybrid, commit every 100
5. `large_text_10k`: 10,000 writes, text-only, commit every 200
6. `large_hybrid_10k`: 10,000 writes, hybrid, commit every 200
7. `sustained_write_text`: 30,000 writes, small WAL pressure, no periodic manual commit
8. `sustained_write_hybrid`: 10,000 writes, small WAL pressure, staged commits

### Metrics captured per workload
- Commit latency p50/p95/p99 (and stage latency)
- Put latency and auto-commit-trigger put latency
- WAL pending-bytes distribution
- Wrap/checkpoint/auto-commit/sentinel counters
- File growth over time (logical + allocated)
- Reopen/open+close latency distribution
- Health checks (`verify`, reopen frame-count match)

### How to run
```bash
WAX_BENCHMARK_WAL_COMPACTION=1 \
WAX_BENCHMARK_SCALE=standard \
swift test --filter WALCompactionBenchmarks
```

Optional:
```bash
WAX_BENCHMARK_WAL_OUTPUT=/tmp/wal-compaction.json
WAX_WAL_SAMPLE_EVERY_WRITES=250
WAX_WAL_REOPEN_ITERATIONS=7
```

### Sanity run executed
- Smoke run completed successfully and emitted:
  - `/tmp/wal-compaction-smoke2.json`
- This validates harness behavior and JSON output shape; go/no-go decisions should still use full standard matrix on pinned commit.

## Ranked compaction candidate designs
1. Skip no-op index staging on identical bytes
- Expected impact: high on file growth and compaction idempotency.
- Complexity/risk: low.
- Compatibility: current format.
- Rollout: metrics gate + compaction-growth regression checks.
- Validation plan: run repeated `compactIndexes` loops and enforce near-zero growth delta on unchanged corpus.

2. Single-pass WAL replay (state + mutations together)
- Expected impact: medium-high on reopen/recovery p95/p99.
- Complexity/risk: low-medium.
- Compatibility: current format.
- Rollout: replay-correctness test suite + crash recovery regression pack.
- Validation plan: compare replay outputs against current two-pass logic across corruption and crash fixtures.

3. Proactive WAL-pressure checkpointing
- Expected impact: high on write tail spikes from capacity-edge commits.
- Complexity/risk: medium.
- Compatibility: additive behavior.
- Rollout: feature flag + p95/p99 guardrails.
- Validation plan: sustained tiny-WAL workload should reduce auto-commit-triggered put p95/p99 spikes.

4. Append-path syscall reduction (sentinel strategy + batched writes)
- Expected impact: medium on sustained ingest throughput and tails.
- Complexity/risk: medium-high.
- Compatibility: possible in current format with strict crash semantics checks.
- Rollout: corruption/fault-injection test gate.
- Validation plan: WAL fault-injection + reopen verification must remain green while ingest throughput improves.

5. Live-set rewrite compactor (deep option, v2-capable)
- Expected impact: very high on long-run growth and storage health.
- Complexity/risk: high.
- Compatibility: best treated as format-evolution track.
- Rollout: offline maintenance command first, then staged migration.
- Validation plan: compare long-run file growth and cold-open recovery against v1 baseline under 10k+ and sustained workloads.

## Ranked implementation plan
### Phase 1: completed in this investigation
- Additive WAL stats instrumentation and tests
- Reproducible workload harness + JSON output
- Baseline report + machine-readable artifact

### Phase 2: quick wins
1. Candidate #1 (no-op index staging skip)
2. Candidate #2 (single-pass replay)
- Re-run full matrix and compare deltas.

### Phase 3: medium changes
1. Candidate #3 (proactive pressure checkpointing)
2. Candidate #4 if p99 still misses target

### Phase 4: deep option
- Prototype candidate #5 as optional maintenance pipeline with explicit migration/rollback plan.

## Go / no-go criteria
- Go on quick wins when at least one candidate achieves:
  - `>=20%` p95 improvement in target metric, or
  - `>=80%` reduction in unnecessary compaction growth,
  - with no correctness regressions.
- No-go on deep format work unless quick wins miss targets and full matrix shows clear ROI.

## Implementation outcomes (items 1-5 executed)
### Item 1: skip no-op index staging + avoid unnecessary FTS `VACUUM`
- Commit: `de6fdd8`
- Outcome:
  - Repeated unchanged `compactIndexes` growth now bounded (test gate `<=4096` bytes) instead of multi-MB drift.
  - Added coverage:
    - `/Users/chriskarani/CodingProjects/Wax/Tests/WaxCoreTests/IndexStagingNoOpTests.swift`
    - `/Users/chriskarani/CodingProjects/Wax/Tests/WaxIntegrationTests/IndexCompactionTests.swift`
- Decision: kept.

### Item 2: single-pass WAL replay on open
- Commit: `7889c99`
- Outcome:
  - Replaced two WAL scans with one combined pending/state scan.
  - Replay/crash correctness remained green (`WALReplayTests`, `CrashRecoveryTests`).
  - Reopen p95 in matrix remained noisy; no correctness regressions.
- Decision: kept.

### Item 3: proactive WAL-pressure checkpointing
- Commit: `ff3235a`
- Outcome:
  - Added `WaxOptions.walProactiveCommitThresholdPercent`.
  - Initial default-on attempt showed sustained-write tail regression signal (`/tmp/wal-item3.json`), so behavior was reverted to opt-in default (`nil`).
  - Validation passed with opt-in controls and no default-path behavior change (`/tmp/wal-item3-defaultoff.json`).
- Decision: kept as opt-in only (regression path removed).

### Item 4: append-path syscall reduction
- Commit: `a647ae8`
- Outcome:
  - Inlined sentinel writes when contiguous.
  - Coalesced contiguous batch WAL operations.
  - Added `writeCallCount` instrumentation (`WaxWALStats` + benchmark pressure summary) and deterministic tests proving reduced write calls:
    - `/Users/chriskarani/CodingProjects/Wax/Tests/WaxCoreTests/WALCompactionInstrumentationTests.swift`
  - Matrix runs showed high run-to-run variance, but no sustained correctness issues; write-call reduction is deterministic evidence.
  - Artifacts: `/tmp/wal-item4.json`, `/tmp/wal-item4-rerun.json`, `/tmp/wal-item4-final.json`.
- Decision: kept.

### Item 5: offline live-set rewrite compactor (deep option)
- Commit: `5906e7c`
- Outcome:
  - Added offline maintenance pipeline:
    - `rewriteLiveSet(to:options:)` in `/Users/chriskarani/CodingProjects/Wax/Sources/Wax/Orchestrator/MemoryOrchestrator+Maintenance.swift`
    - options/report types:
      - `/Users/chriskarani/CodingProjects/Wax/Sources/Wax/Maintenance/LiveSetRewriteOptions.swift`
      - `/Users/chriskarani/CodingProjects/Wax/Sources/Wax/Maintenance/LiveSetRewriteReport.swift`
  - Deep-compaction behavior supports dropping payload bytes for non-live frames while preserving frame graph and index carry-forward.
  - Added integration coverage:
    - `/Users/chriskarani/CodingProjects/Wax/Tests/WaxIntegrationTests/LiveSetRewriteCompactionTests.swift`
  - WAL benchmark matrix showed only noise-level movement versus item 4 (`/tmp/wal-item5.json`).
- Decision: kept as offline maintenance command (non-destructive to source file).

## Final prioritization after implementation
1. **Ship now**: item 1 + item 2 + item 4 (current-format wins with low migration risk).
2. **Ship guarded**: item 3 as opt-in threshold control with workload-specific tuning.
3. **Adopt cautiously**: item 5 as explicit maintenance workflow (operator-invoked, rollback-safe by separate destination file).

## Final go/no-go call
- **Go** for implementation phase completion on quick wins:
  - compaction-growth waste addressed (item 1),
  - replay path simplified without correctness regressions (item 2),
  - WAL append-path syscall reductions landed with deterministic evidence (item 4).
- **Conditional go** for proactive pressure commits:
  - keep opt-in only until dedicated percentile guardrail targets are met on pinned hardware.
- **Go (pilot)** for deep compactor:
  - offline-only usage first, with explicit runbooks and backup/validation checks.
