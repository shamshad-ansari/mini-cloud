# Mini Cloud

The first tracer validates resource admission for one worker. CPU is expressed in
integer millicores and memory in integer MiB.

## Build and test (Linux)

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

## Resource-admission tracer

```bash
# capacity CPU, capacity memory, reserved CPU, reserved memory, request CPU, request memory
./build/resource_admission_tracer 2000 4096 500 1024 1000 2048
# accepted

./build/resource_admission_tracer 2000 4096 500 1024 1600 2048
# rejected: insufficient CPU
```

The process exits with `0` for admission, `1` for a capacity rejection, and `2`
for malformed or invalid resource input. Capacity and requested CPU/memory must be
positive; reservations may be zero but cannot exceed capacity. Values that cannot
be represented as unsigned 64-bit integers, and additions that overflow, are
rejected.
