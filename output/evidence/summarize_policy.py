"""Print a compact summary of benchmark CSV supplied on standard input."""
import collections
import csv
import statistics
import sys

groups = collections.defaultdict(list)
for row in csv.DictReader(sys.stdin):
    if row['scenario'] != 'mixed-repeat':
        groups[row['scenario'], row['policy']].append(row)
print('Scenario          Policy  Trials Success% CPU-SD Memory-SD Time(us)')
for (scenario, policy), rows in groups.items():
    mean = lambda key: statistics.mean(float(row[key]) for row in rows)
    label = 'FF' if policy == 'first-fit' else 'LD'
    print(f'{scenario:17} {label:6} {len(rows):6} {mean("success_pct"):8.2f} '
          f'{mean("cpu_sd_pp"):6.2f} {mean("memory_sd_pp"):9.2f} '
          f'{mean("mean_schedule_us"):8.4f}')
print('FF=first-fit; LD=resource-aware. SD is reservation balance in percentage points.')
print('Lower SD is more even. Timing is preliminary; capacities are simulated.')
