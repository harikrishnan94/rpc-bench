// Entry point for the standalone key-value server process.

#include "rpcbench/config.hpp"
#include "rpcbench/rpc.hpp"
#include "rpcbench/storage.hpp"

#include <cstdio>
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
        const auto help = rpcbench::server_usage(argv[0]);
        std::fputs(help.c_str(), stdout);
        return 0;
      }
    }

    auto config = rpcbench::parse_server_config(args);
    if (!config) {
      std::fprintf(stderr, "error: %s\n", config.error().c_str());
      const auto help = rpcbench::server_usage(argv[0]);
      std::fputs(help.c_str(), stderr);
      return 1;
    }

    const rpcbench::EzRpcServerRunner runner(*config, rpcbench::make_in_memory_store());
    runner.run();
    return 0;
  } catch (const std::exception& exception) {
    std::fprintf(stderr, "error: %s\n", exception.what());
    return 1;
  } catch (...) {
    std::fputs("error: server terminated with an unknown failure\n", stderr);
    return 1;
  }
}
