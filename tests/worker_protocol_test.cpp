#include "mini_cloud/worker_protocol.hpp"

#include <cstdlib>
#include <iostream>

namespace {

int failures = 0;

void expect(const bool condition, const char* description) {
  if (!condition) {
    std::cerr << "FAILED: " << description << '\n';
    ++failures;
  }
}

}  // namespace

int main() {
  const auto registration = mini_cloud::parse_worker_message(
      "{\"version\":1,\"type\":\"register\",\"worker_id\":\"worker-1\",\"cpu_millicores\":2000,\"memory_mib\":4096}");
  expect(registration && registration->type == mini_cloud::WorkerMessageType::registration &&
             registration->worker_id == "worker-1" && registration->cpu_millicores == 2000 &&
             registration->memory_mib == 4096,
         "valid registration parses");
  const auto heartbeat = mini_cloud::parse_worker_message(
      "{\"version\":1,\"type\":\"heartbeat\",\"worker_id\":\"worker-1\"}");
  expect(heartbeat && heartbeat->type == mini_cloud::WorkerMessageType::heartbeat,
         "valid heartbeat parses");
  const auto generated_registration =
      mini_cloud::parse_worker_message(mini_cloud::registration_message("worker-1", 2000, 4096));
  expect(generated_registration && generated_registration->cpu_millicores == 2000 &&
             generated_registration->memory_mib == 4096,
         "generated registration is valid JSON protocol input");
  const auto launch = mini_cloud::parse_controller_message(
      mini_cloud::launch_message("workload-1", "workload-1-replica-1", "/bin/sleep", {"30"}, 250, 64));
  expect(launch && launch->workload_id == "workload-1" && launch->replica_id == "workload-1-replica-1" &&
             launch->executable == "/bin/sleep" && launch->arguments == std::vector<std::string>{"30"} &&
             launch->cpu_millicores == 250 && launch->memory_mib == 64,
         "generated launch is valid controller protocol input");
  const auto accepted = mini_cloud::parse_worker_message(
      mini_cloud::launch_accepted_message("worker-1", "workload-1", "workload-1-replica-1"));
  expect(accepted && accepted->type == mini_cloud::WorkerMessageType::launch_accepted &&
             accepted->replica_id == "workload-1-replica-1",
         "worker launch acceptance parses");
  expect(!mini_cloud::parse_worker_message(
             "{\"version\":2,\"type\":\"register\",\"worker_id\":\"worker-1\",\"cpu_millicores\":2000,\"memory_mib\":4096}"),
         "unsupported version is rejected");
  expect(!mini_cloud::parse_worker_message(
             "{\"version\":1,\"type\":\"register\",\"worker_id\":\"bad id\",\"cpu_millicores\":2000,\"memory_mib\":4096}"),
         "invalid worker ID is rejected");
  expect(!mini_cloud::parse_worker_message(
             "{\"version\":1,\"type\":\"register\",\"worker_id\":\"worker-1\",\"cpu_millicores\":0,\"memory_mib\":4096}"),
         "zero capacity is rejected");
  expect(!mini_cloud::parse_worker_message("not json"), "malformed line is rejected");
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
