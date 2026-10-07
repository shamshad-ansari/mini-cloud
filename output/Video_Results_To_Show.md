# Mini Cloud — preliminary policy comparison
Four simulated workers: **2,000 millicores and 2,048 MiB each**. Ten trials per scenario and policy. Both policies receive identical requests within each trial. The benchmark directly calls the implemented C++ scheduler.

| Scenario | Policy | Placement success | CPU balance SD (pp) | Memory balance SD (pp) | Mean scheduling time (microseconds) |
|---|---|---:|---:|---:|---:|
| 8 light requests, empty | First-fit | 100.00% | 50.00 | 12.50 | 0.0198 |
| 8 light requests, empty | Resource-aware | 100.00% | 0.00 | 0.00 | 0.0662 |
| 48 mixed requests, empty | First-fit | 33.33% | 9.84 | 10.92 | 0.0289 |
| 48 mixed requests, empty | Resource-aware | 33.75% | 7.12 | 6.52 | 0.0527 |
| 48 mixed requests, 50% pre-reserved | First-fit | 16.67% | 2.20 | 1.37 | 0.0307 |
| 48 mixed requests, 50% pre-reserved | Resource-aware | 16.67% | 2.27 | 1.42 | 0.0285 |

**Success:** placed/submitted requests. **Balance:** standard deviation of reserved/capacity across workers, in percentage points; lower is more even. **Time:** scheduler-call duration only, excluding launch and networking.

Resource-aware scheduling improves balance in the light and mixed-empty cases, but not consistently in the preloaded case. Short timing samples do not establish a reliable speed ranking.

These are synthetic-capacity results, separate from actual local controller/worker process tests. Raw observations: `output/evidence/policy_trials.csv`. Snapshot: 6 October 2026.
