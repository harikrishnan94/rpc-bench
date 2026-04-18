// Text and JSON formatting for benchmark results. Keeping rendering centralized
// makes it easier for tests and future tooling to consume a stable report shape
// while the execution engine evolves.

#include "rpcbench/metrics.hpp"

#include <algorithm>
#include <format>
#include <string_view>

namespace rpcbench {
namespace {

std::string_view bench_mode_name(BenchMode mode) {
  switch (mode) {
  case BenchMode::connect:
    return "connect";
  case BenchMode::spawn_local:
    return "spawn-local";
  }
  return "unknown";
}

std::string format_latency(std::uint64_t nanoseconds) {
  if (nanoseconds >= 1'000'000'000ULL) {
    return std::format("{:.2f}s", static_cast<double>(nanoseconds) / 1'000'000'000.0);
  }
  if (nanoseconds >= 1'000'000ULL) {
    return std::format("{:.2f}ms", static_cast<double>(nanoseconds) / 1'000'000.0);
  }
  if (nanoseconds >= 1'000ULL) {
    return std::format("{:.2f}us", static_cast<double>(nanoseconds) / 1'000.0);
  }
  return std::format("{}ns", nanoseconds);
}

std::string join_strings(const std::vector<std::string>& values, std::string_view delimiter) {
  std::string joined;
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) {
      joined += delimiter;
    }
    joined += values[index];
  }
  return joined;
}

std::string json_escape(std::string_view text) {
  std::string escaped;
  escaped.reserve(text.size() + 8);

  for (const char ch : text) {
    switch (ch) {
    case '\\':
      escaped += "\\\\";
      break;
    case '"':
      escaped += "\\\"";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      escaped.push_back(ch);
      break;
    }
  }

  return escaped;
}

std::string format_result_text(const BenchmarkResult& result, std::size_t index) {
  return std::format("Run {}: workers={} clients={} queue={} key={} value={} mix={} iteration={}\n"
                     "  Endpoints: {}\n"
                     "  Throughput: {:.2f} ops/s, {:.2f} MiB/s\n"
                     "  Latency: min={} p50={} p90={} p99={} max={}\n"
                     "  Ops: total={} get={} put={} delete={} errors={}\n"
                     "  Outcomes: found-get={} missing-get={} removed-delete={} missing-delete={}\n"
                     "  Bytes: request={} response={}\n",
                     index + 1,
                     result.server_workers,
                     result.client_threads,
                     result.queue_depth,
                     result.key_size,
                     result.value_size,
                     result.mix.label(),
                     result.iteration,
                     join_strings(result.endpoints, ", "),
                     result.ops_per_second,
                     result.mib_per_second,
                     format_latency(result.latency.min_ns),
                     format_latency(result.latency.p50_ns),
                     format_latency(result.latency.p90_ns),
                     format_latency(result.latency.p99_ns),
                     format_latency(result.latency.max_ns),
                     result.counts.total_ops,
                     result.counts.get_ops,
                     result.counts.put_ops,
                     result.counts.delete_ops,
                     result.counts.errors,
                     result.counts.found_gets,
                     result.counts.missing_gets,
                     result.counts.removed_deletes,
                     result.counts.missing_deletes,
                     result.counts.request_bytes,
                     result.counts.response_bytes);
}

void append_json_string(std::string& out, std::string_view text) {
  out += '"';
  out += json_escape(text);
  out += '"';
}

} // namespace

std::string BenchmarkReport::to_text() const {
  std::string text = std::format("rpc-bench report\n"
                                 "mode: {}\n"
                                 "runs: {}\n\n",
                                 bench_mode_name(mode),
                                 results.size());

  for (std::size_t index = 0; index < results.size(); ++index) {
    text += format_result_text(results[index], index);
    if (index + 1 != results.size()) {
      text += '\n';
    }
  }

  return text;
}

std::string BenchmarkReport::to_json() const {
  std::string json;
  json += "{\n";
  json += "  \"mode\": ";
  append_json_string(json, bench_mode_name(mode));
  json += ",\n";
  json += "  \"results\": [\n";

  for (std::size_t index = 0; index < results.size(); ++index) {
    const auto& result = results[index];
    json += "    {\n";
    json += std::format("      \"serverWorkers\": {},\n", result.server_workers);
    json += std::format("      \"clientThreads\": {},\n", result.client_threads);
    json += std::format("      \"queueDepth\": {},\n", result.queue_depth);
    json += std::format("      \"keySize\": {},\n", result.key_size);
    json += std::format("      \"valueSize\": {},\n", result.value_size);
    json += "      \"mix\": ";
    append_json_string(json, result.mix.label());
    json += ",\n";
    json += std::format("      \"iteration\": {},\n", result.iteration);
    json += std::format("      \"measureSeconds\": {:.6f},\n", result.measure_seconds);
    json += "      \"endpoints\": [";
    for (std::size_t endpoint = 0; endpoint < result.endpoints.size(); ++endpoint) {
      if (endpoint != 0) {
        json += ", ";
      }
      append_json_string(json, result.endpoints[endpoint]);
    }
    json += "],\n";
    json += "      \"counts\": {\n";
    json += std::format("        \"totalOps\": {},\n", result.counts.total_ops);
    json += std::format("        \"getOps\": {},\n", result.counts.get_ops);
    json += std::format("        \"putOps\": {},\n", result.counts.put_ops);
    json += std::format("        \"deleteOps\": {},\n", result.counts.delete_ops);
    json += std::format("        \"foundGets\": {},\n", result.counts.found_gets);
    json += std::format("        \"missingGets\": {},\n", result.counts.missing_gets);
    json += std::format("        \"removedDeletes\": {},\n", result.counts.removed_deletes);
    json += std::format("        \"missingDeletes\": {},\n", result.counts.missing_deletes);
    json += std::format("        \"errors\": {},\n", result.counts.errors);
    json += std::format("        \"requestBytes\": {},\n", result.counts.request_bytes);
    json += std::format("        \"responseBytes\": {}\n", result.counts.response_bytes);
    json += "      },\n";
    json += "      \"latencyNs\": {\n";
    json += std::format("        \"min\": {},\n", result.latency.min_ns);
    json += std::format("        \"p50\": {},\n", result.latency.p50_ns);
    json += std::format("        \"p90\": {},\n", result.latency.p90_ns);
    json += std::format("        \"p99\": {},\n", result.latency.p99_ns);
    json += std::format("        \"max\": {}\n", result.latency.max_ns);
    json += "      },\n";
    json += std::format("      \"opsPerSecond\": {:.6f},\n", result.ops_per_second);
    json += std::format("      \"mibPerSecond\": {:.6f}\n", result.mib_per_second);
    json += "    }";
    if (index + 1 != results.size()) {
      json += ",";
    }
    json += "\n";
  }

  json += "  ]\n";
  json += "}\n";
  return json;
}

} // namespace rpcbench
