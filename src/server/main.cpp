// Entry point for the standalone CRC32 server executable.

#include "app/application.hpp"

#include <exception>
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
        std::print("{}", rpcbench::server_usage(argv[0]));
        return 0;
      }
    }

    auto config = rpcbench::parse_server_config(args);
    if (!config) {
      std::println(stderr, "error: {}", config.error());
      std::print(stderr, "{}", rpcbench::server_usage(argv[0]));
      return 1;
    }

    return rpcbench::run_server_app(*config);
  } catch (const kj::Exception& exception) {
    std::println(stderr, "error: {}", kj::str(exception).cStr());
    return 1;
  } catch (const std::exception& exception) {
    std::println(stderr, "error: {}", exception.what());
    return 1;
  } catch (...) {
    std::println(stderr, "error: server terminated with an unknown failure");
    return 1;
  }
}
