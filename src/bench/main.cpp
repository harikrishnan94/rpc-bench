// Entry point for the CRC32 benchmark executable.

#include "app/application.hpp"

#include <exception>
#include <fstream>
#include <kj/exception.h>
#include <kj/string.h>
#include <print>
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
        std::print("{}", rpcbench::bench_usage(argv[0]));
        return 0;
      }
    }

    auto config = rpcbench::parse_bench_config(args, argv[0]);
    if (!config) {
      std::println(stderr, "error: {}", config.error());
      std::print(stderr, "{}", rpcbench::bench_usage(argv[0]));
      return 1;
    }

    auto result = rpcbench::run_benchmark_app(*config);
    if (!result) {
      std::println(stderr, "error: {}", result.error());
      return 1;
    }

    std::print("{}", result->to_text());
    if (config->json_output) {
      std::ofstream stream(*config->json_output, std::ios::binary);
      if (!stream) {
        std::println(stderr, "error: could not open JSON output path");
        return 1;
      }
      stream << result->to_json();
    }

    return 0;
  } catch (const kj::Exception& exception) {
    std::println(stderr, "error: {}", kj::str(exception).cStr());
    return 1;
  } catch (const std::exception& exception) {
    std::println(stderr, "error: {}", exception.what());
    return 1;
  } catch (...) {
    std::println(stderr, "error: benchmark terminated with an unknown failure");
    return 1;
  }
}
