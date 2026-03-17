# ADR-0008: Epoch Duration Tuning

- **Status**: Revised (keep default 40ms)
- **Date**: 2026-03-17
- **Deciders**: tsurugi-graph development team
- **Related**: Shirakami concurrency control configuration

## Context

Shirakami uses epoch-based concurrency control. The `epoch_duration` parameter controls
how frequently the global epoch advances. Transaction commits must wait for the current
epoch to advance before becoming durable.

The default `epoch_duration` is 40,000 microseconds (40ms). For graph workloads with
many small transactions (e.g., 10K-node chunks during bulk insert), each commit incurs
up to 40ms of epoch-wait latency.

At 5M nodes with CHUNK=10K, there are 500 commits. With 40ms epoch:
- Worst case: 500 × 40ms = 20 seconds of pure wait time
- With 3ms epoch: 500 × 3ms = 1.5 seconds

## Decision

Set `epoch_duration` to 3,000 microseconds (3ms) for graph workloads via
`DatabaseOptions::attribute("epoch_duration", "3000")`.

This reduces per-commit epoch wait from up to 40ms to up to 3ms, a 13x reduction
in maximum commit latency.

### Configuration

For standalone benchmarks (real_bench.cpp):
```cpp
DatabaseOptions db_opts;
db_opts.attribute("epoch_duration", "3000");
```

For production server (tsurugi.ini):
```ini
[cc]
    epoch_duration=3000
```

## Consequences

### Positive

- **Lower commit latency**: Up to 13x reduction in per-commit wait time
- **Better throughput for small transactions**: Graph operations benefit from frequent
  small commits rather than large batch transactions
- **No code changes required**: Configuration-only optimization

### Negative

- **Higher CPU overhead**: Epoch advancement thread runs more frequently (3ms vs 40ms)
- **More frequent WAL flushes**: May increase I/O pressure on storage subsystem

### Risks

- Extremely low epoch_duration (< 1ms) could cause CPU contention. 3ms is a safe
  middle ground between latency and overhead.

## Revision (2026-03-17)

Benchmark comparison of epoch=3000μs vs epoch=40000μs (default) at 100K nodes:

| Metric | epoch=3000 | epoch=40000 | Result |
|:---|---:|---:|:---|
| create_node | 44,874 | **47,761** | Default +6% |
| Total throughput | 62,693 | **66,250** | Default +6% |
| Concurrent write 8T | **17,101** | 15,604 | Short epoch +10% |
| Concurrent mixed 8T | 109,629 | **122,026** | Default +11% |

**Conclusion**: The default epoch_duration=40000μs is **6% faster for single-threaded
workloads** due to lower epoch transition CPU overhead. Short epochs (3ms) only provide
marginal benefit for write-heavy multi-client scenarios (8-thread write-only: +10%).

**Revised recommendation**: Keep the default epoch_duration=40000μs. Only consider
epoch=3000μs for workloads with many concurrent write-heavy clients where commit
latency is critical.
