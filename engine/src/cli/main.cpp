#include "ghostfs/engine.hpp"

#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>

int main(int argc, char* argv[]) {
  std::string input;
  if (argc >= 3 && std::string(argv[1]) == "--request") {
    input = argv[2];
  } else {
    input.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
  }

  const std::string response = ghostfs::process_request_json(input);
  std::cout << response << '\n';

  try {
    const auto parsed = nlohmann::json::parse(response);
    return parsed.value("ok", false) ? 0 : 1;
  } catch (...) {
    return 1;
  }
}
