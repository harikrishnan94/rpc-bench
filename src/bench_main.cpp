// Entry point for the benchmark runner executable.

#include "rpcbench/bench.hpp"
#include "rpcbench/config.hpp"

#include <cstdio>
#include <fstream>
#include <span>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
  try {
    std::vector<std::string_view> args;
    args.reserve(static_cast<std::size_t>(argc > 1 ? argc - 1 : 0));
    for (int index = 1; index < argc; ++index) {
      args.emplace_back(argv[index]);
    }

    for (const auto arg : args) {
      if (arg == "--help") {
        const auto help = rpcbench::bench_usage(argv[0]);
        std::fputs(help.c_str(), stdout);
        return 0;
      }
    }

    auto config = rpcbench::parse_bench_config(args, argv[0]);
    if (!config) {
      std::fprintf(stderr, "error: %s\n", config.error().c_str());
      const auto help = rpcbench::bench_usage(argv[0]);
      std::fputs(help.c_str(), stderr);
      return 1;
    }

    const rpcbench::BenchmarkRunner runner(*config);
    auto report = runner.run();
    if (!report) {
      std::fprintf(stderr, "error: %s\n", report.error().c_str());
      return 1;
    }

    const auto text = report->to_text();
    std::fputs(text.c_str(), stdout);

    if (config->json_output) {
      std::ofstream stream(*config->json_output, std::ios::binary);
      if (!stream) {
        std::fputs("error: could not open JSON output path\n", stderr);
        return 1;
      }
      stream << report->to_json();
    }

    return 0;
  } catch (const std::exception& exception) {
    std::fprintf(stderr, "error: %s\n", exception.what());
    return 1;
  } catch (...) {
    std::fputs("error: benchmark terminated with an unknown failure\n", stderr);
    return 1;
  }
}
