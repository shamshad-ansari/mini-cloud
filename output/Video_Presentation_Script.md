# Midterm video script — approximately 3 minutes 40 seconds

Read the narration aloud. **Show** and **Run** are instructions, not narration. This covers cumulative progress through Week 7, live software, results, and the second-half plan.

## Prepare before recording

Open `README.md`, `src/resources.cpp`, and `src/scheduler.cpp` in your editor. Open `output/Midterm_Project_Report.pdf` at page 9 for the schedule.

Build the project and benchmark before recording, from the project directory:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 2
c++ -std=c++23 -O2 -Iinclude output/evidence/policy_benchmark.cpp \
  src/scheduler.cpp src/resources.cpp -o /tmp/mini-cloud-policy-benchmark
```

Open four terminals in the project directory. Start these before recording:

**Terminal 1 — controller:**

```bash
./build/mini_cloud_controller 7000 2000
```

**Terminal 2 — worker A:**

```bash
./build/mini_cloud_worker 127.0.0.1 7000 worker-a 1000 128 100 --no-cgroups &
demo_worker_a_pid=$!
```

**Terminal 3 — worker B:**

```bash
./build/mini_cloud_worker 127.0.0.1 7000 worker-b 1000 128 100 --no-cgroups
```

**Terminal 4 — ordinary shell:** admission examples, tests, and benchmark.

Wait for both worker registrations. Start a fresh controller for the recording so the first workload is `workload-1`. Rehearse once; allow terminal pauses while keeping the recording between 3 and 5 minutes.

## [0:00–0:25 — Introduction: show README.md]

Hi, my project is Mini Cloud, a miniature private compute cloud written in C++23 for Linux.

Its goal is to schedule workloads using CPU and memory requests, supervise their processes, detect worker failures, and restore missing replicas.

This presentation summarizes cumulative progress through Week 7 and demonstrates the working prototype.

## [0:25–0:55 — Completed work: show resources.cpp, then scheduler.cpp]

The project progressed from resource admission and automated tests to two scheduling policies, process supervision, worker registration, heartbeats, and remote launches. I then added replicated placement, explicit stop, failure recovery, and cgroup-limit support.

CPU uses integer millicores and memory uses MiB, with overflow protection. First-fit chooses the first feasible worker. The resource-aware policy minimizes the larger projected CPU or memory reservation ratio, breaking ties by worker ID.

**Show:** `checked_add` in `src/resources.cpp`, then `choose_worker` in `src/scheduler.cpp`. Explain the milestones cumulatively; do not invent specific week assignments.

## [0:55–1:15 — Live admission: show Terminal 4]

**Run:**

```bash
./build/resource_admission_tracer first-fit 1000 2048 worker-a 2000 4096 500 1024
./build/resource_admission_tracer first-fit 1600 2048 worker-a 2000 4096 500 1024
```

The first request fits and selects worker A. The second exceeds available CPU by 100 millicores, so placement is rejected. Both CPU and memory must fit before a workload is admitted.

**Show:** `selected worker=worker-a`, then `no placement` and `cpu=100m`.

## [1:15–1:50 — Live remote launch: show Terminal 1]

**Type into the controller, not a shell:**

```text
submit least-loaded-dominant-resource 500 64 2 -- /bin/sleep 120
status
```

Here, two workers have registered and are sending heartbeats. I am submitting two replicas, each requesting 500 millicores and 64 MiB.

The policy places one on each worker. The agents launch actual processes and acknowledge that they are running.

These workers are processes on one host. Cgroup enforcement is disabled for this demo; enforced limits require a delegated Linux environment.

**Show:** registration events, `replica_running`, and `desired=2 pending=0 running=2`. If still launching, wait briefly and type `status` again.

## [1:50–2:25 — Live failure recovery and stop]

**Run in Terminal 2:**

```bash
kill -KILL "$demo_worker_a_pid"
```

**Wait for `replacement_running`. Type into Terminal 1:**

```text
status
stop workload-1
```

**After stop acknowledgements, type:**

```text
status
```

Killing worker A causes the controller to detect connection loss and launch a replacement on worker B, which has spare capacity.

Stopping the workload releases reservations after termination is confirmed.

An old process can survive agent failure, so this recovery does not guarantee exactly-once execution.

**Show:** `worker_dead`, `replica_lost`, `replacement_running`, restored running count, then `desired=0 pending=0 running=0`.

## [2:25–2:50 — Live tests: show Terminal 4]

**Run:**

```bash
ctest --test-dir build --output-on-failure
```

Now I am running the automated tests live. They cover admission, scheduling, supervision, communication, replicated placement, recovery, and accounting.

Our earlier verification passed nine executed suites. Two privileged cgroup suites were skipped because delegation was unavailable. I am checking the current summary rather than counting skipped tests as verified.

**Show:** the final summary. The previous run took about nine seconds. Describe today's actual result if it differs; do not claim success before it completes.

## [2:50–3:15 — Live measured comparison: show Terminal 4]

**Run:**

```bash
/tmp/mini-cloud-policy-benchmark | python3 output/evidence/summarize_policy.py
```

This benchmark runs the actual scheduler with simulated capacities and identical request streams for both policies.

For the light workload, both place every request, while resource-aware scheduling balances reservations evenly. The mixed-empty case shows a small placement-success improvement.

The output compares success, CPU and memory balance, and scheduling time across ten trials per scenario. Timing is preliminary, and benefits depend on the workload.

**Show:** the six summary rows. `FF` means first-fit; `LD` means resource-aware. Lower balance standard deviation is more even. Fresh times can differ from the report. These are synthetic scheduling results, distinct from the actual process demo.

## [3:15–3:40 — Second-half plan: show report page 9]

Week 8 targets enforced-limit validation and the Release-build warning. Weeks 9 and 10 target a reproducible recovery runner and 60 trials. Week 11 targets a larger policy comparison.

The remaining weeks cover cross-node validation where available, final analysis, and delivery.

The midpoint outcome is a working, tested prototype, with broader performance evaluation still in progress. My repository link is in the report. Thank you.

## After recording

Type `quit` into the controller. Stop worker B with Ctrl+C in Terminal 3. A sleep process left by the killed agent may continue until its 120-second duration ends.

Confirm the video is 3–5 minutes and the commands, results, and narration are legible and audible.
