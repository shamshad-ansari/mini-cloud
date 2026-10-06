# Mini Cloud

The first tracer selects a worker for one replica request. CPU is expressed in
integer millicores and memory in integer MiB.

## Build and test (Linux)

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

## Scheduling tracer

```bash
# policy, request CPU, request memory, then one or more worker records:
# worker ID, capacity CPU, capacity memory, reserved CPU, reserved memory
./build/resource_admission_tracer first-fit 1000 2048 \
  worker-a 2000 4096 500 1024 \
  worker-b 2000 4096 0 0
# selected worker=worker-a policy=first-fit

./build/resource_admission_tracer least-loaded-dominant-resource 1600 2048 \
  worker-a 2000 4096 500 1024 \
  worker-b 2000 4096 500 1024
# no placement policy=least-loaded-dominant-resource
# capacity deficit worker=worker-a cpu=100m memory=0MiB
# capacity deficit worker=worker-b cpu=100m memory=0MiB
```

Allowed policies are `first-fit` and `least-loaded-dominant-resource`. First-fit
uses the supplied worker order. The dominant-resource policy selects the feasible
worker with the smallest projected `max(cpu reservation / CPU capacity, memory
reservation / memory capacity)` and breaks exact ties by worker ID.

The process exits with `0` for a placement, `1` for no placement, and `2` for
unknown policies or malformed/invalid input. Capacity and requested CPU/memory must
be positive; reservations may be zero but cannot exceed capacity. Values that cannot
be represented as unsigned 64-bit integers, and additions that overflow, are rejected.

## Local process-supervision tracer

The local runtime tracer launches one child process, emits JSON Lines lifecycle
events, then intentionally stops and reaps the child after the requested delay.

```bash
./build/process_supervision_tracer demo-replica 1000 -- /bin/sleep 60
```

Example output:

```json
{"event":"replica_started","replica_id":"demo-replica","pid":1234,"state":"RUNNING"}
{"event":"replica_stopping","replica_id":"demo-replica","pid":1234,"state":"STOPPING"}
{"event":"replica_terminated","replica_id":"demo-replica","pid":1234,"state":"STOPPED","intentional":true,"signal":15}
```

If the child exits before the explicit stop, the final event instead has state
`EXITED_UNEXPECTEDLY`, including a normal exit with code `0`. The tracer exits with
`0` after an intentional stop, `1` after an unexpected exit or launch/stop failure,
and `2` for invalid command-line input.

## TCP worker registration and heartbeats

Start the controller in one terminal. Type `status` to inspect its live worker state
and `quit` to stop it.

```bash
./build/mini_cloud_controller 7000
```

Start a worker in another terminal. The final argument sends 20 heartbeats at a
50-millisecond interval; omit it or use `0` to continue sending indefinitely.

```bash
./build/mini_cloud_worker 127.0.0.1 7000 worker-a 2000 4096 50 20
```

The controller emits versioned JSON Lines events such as:

```json
{"timestamp_ms":1735689600000,"event":"worker_registered","worker_id":"worker-a"}
{"timestamp_ms":1735689600050,"event":"worker_heartbeat","worker_id":"worker-a"}
```

While the worker remains connected and sends heartbeats, the controller command
`status` reports, for example:

```text
STATUS worker=worker-a health=HEALTHY cpu=2000m memory=4096MiB
```

## Submit a remotely supervised replica

With the controller and worker running, submit a workload on the controller's
standard input. The command format is policy, CPU millicores, memory MiB,
replica count, then the executable and its whitespace-delimited arguments.

```text
submit first-fit 250 64 1 -- /bin/sleep 30
status
```

The controller emits JSONL `workload_submitted`, `scheduling_decision`,
`launch_accepted`, and `replica_running` events. IDs such as `workload-1` and
`workload-1-replica-1` are controller generated and stable across every event
for that submission. `status` includes `desired`, `pending`, and `running`, as
well as `reserved_cpu` and `reserved_memory` for the registered worker.

If capacity cannot satisfy a request, the replica remains pending and the
`replica_pending` event reports `cpu_deficit_millicores` and
`memory_deficit_mib`; no process launch is sent to the worker.

## Replicated placement and explicit stop

The controller accepts multiple workers with distinct IDs and explicit capacities.
For example, start these workers in separate terminals (omit heartbeat count to
keep each worker connected):

```bash
./build/mini_cloud_worker 127.0.0.1 7000 worker-a 1000 128 100
./build/mini_cloud_worker 127.0.0.1 7000 worker-b 1000 128 100
./build/mini_cloud_worker 127.0.0.1 7000 worker-c 1000 128 100
```

After all three workers register, enter controller commands:

```text
submit least-loaded-dominant-resource 500 64 7 -- /bin/sleep 60
status
stop workload-1
status
stop workload-1
```

Six replicas run on workers `a, b, c, a, b, c`; replica seven remains pending
with a deficit of 500 millicores and 64 MiB on each worker. First-fit places the
same request on `a, a, b, b, c, c`. The controller orders candidates by worker ID,
so placement with identical available workers and workloads is independent of
registration order. Reconciliation reserves each placement before selecting the
next replica and retries pending replicas when workers register or capacity is
released.

`status` lists each worker's health and reservations, workload desired state, and
every replica's worker and state (`PENDING`, `LAUNCHING`, `RUNNING`, `STOPPING`,
`STOPPED`). `stop <workload-id>` sets desired replicas to zero before sending
remote stop commands. Workers terminate and reap their supervised processes,
then acknowledge termination; only then does the controller release reservations.
Pending replicas stop immediately. Repeating a completed stop is harmless, and
stopped workloads are excluded from reconciliation. Unknown workload IDs report
an error.

## Worker failure detection and replica recovery

The controller uses a fixed heartbeat timeout of 2000 milliseconds by default.
An optional second argument sets a positive timeout in milliseconds for the
controller session:

```bash
./build/mini_cloud_controller 7000 2000
```

Use the three workers above and submit a workload with spare recovery capacity:

```text
submit least-loaded-dominant-resource 500 64 3 -- /bin/sleep 60
status
```

The initial placements are `worker-a`, `worker-b`, `worker-c`. Kill the agent
process for `worker-a` (for example, `kill -KILL <agent-pid>`). TCP loss marks it
`DEAD` immediately; when the connection remains open but heartbeats cease, the
controller marks it dead after the timeout. Timeout checks run at least once per
100-millisecond polling cycle under normal load. Each worker transitions to dead
once, and its capacity is excluded from scheduling. Dead IDs remain reserved for
the controller session; a restarted worker must register with a new ID.

The lost replica is replaced on `worker-b` using the same workload and replica
IDs. Reconciliation reserves capacity one replica at a time using the workload's
original policy. Replacement is confirmed only by the new worker's running
acknowledgement. JSONL events distinguish these stages in order:

```text
worker_dead
replica_lost
replacement_scheduled
replacement_launch_issued
replacement_launch_accepted
replacement_running
```

Each event includes `timestamp_ms` and worker identity. Replica events also
include workload and replica IDs; loss and replacement events include an
`attempt` number that increases on each loss. Worker death includes the reason
and configured timeout. The original scheduling and launch events remain
available as well.

Without healthy spare capacity, the replica stays `LOST`, reports per-worker
capacity deficits, and contributes to the `pending` count. Other running
replicas keep their placement. Registering additional capacity allows recovery;
explicitly stopped workloads are excluded, including when stopped while lost.
If termination on a lost worker cannot be confirmed, its replica remains `LOST`
and the stopped workload reports `STOPPING` with desired count zero.

`status` reports dead workers and includes:

```text
RECOVERY heartbeat_timeout_ms=2000 partition_safe=false exactly_once=false duplicate_execution_possible=true
```

This recovery path supports agent crash/kill and TCP loss. It does not provide
partition safety, fencing, or exactly-once execution. Old processes may keep
running after an agent is killed or disconnected, so replacements can cause
duplicate execution. `RUNNING` counts acknowledged controller placements and
does not prove that old copies have terminated. Recovery of a child process exit
on an otherwise healthy agent is outside this failure path.

Run the recovery integration test with:

```bash
ctest --test-dir build -R worker_recovery_integration_tests --output-on-failure
```
