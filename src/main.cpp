#include "config/ClientConfig.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/component/event.hpp"
#include "ui/chat/ChatUI.hpp"
#include <sodium.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

void PrintUsage(std::ostream& output, const char* program) {
  output << "Usage: " << program << " [--ca-cert <path>]\n"
         << "\n"
         << "Options:\n"
         << "  --ca-cert <path>  Trust this CA PEM for the current run\n"
         << "  -h, --help         Show this help\n";
}

} // namespace

int main(int argc, char** argv) {
  std::string ca_certificate_path;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "-h" || argument == "--help") {
      PrintUsage(std::cout, argv[0]);
      return 0;
    }
    if (argument == "--ca-cert") {
      if (!ca_certificate_path.empty()) {
        std::cerr << "--ca-cert may only be specified once\n";
        return 2;
      }
      if (++index >= argc || argv[index][0] == '\0') {
        std::cerr << "--ca-cert requires a certificate file path\n";
        return 2;
      }
      ca_certificate_path = argv[index];
      continue;
    }
    constexpr std::string_view prefix = "--ca-cert=";
    if (argument.starts_with(prefix)) {
      if (!ca_certificate_path.empty()) {
        std::cerr << "--ca-cert may only be specified once\n";
        return 2;
      }
      ca_certificate_path = argument.substr(prefix.size());
      if (ca_certificate_path.empty()) {
        std::cerr << "--ca-cert requires a certificate file path\n";
        return 2;
      }
      continue;
    }
    std::cerr << "Unknown option: " << argument << '\n';
    PrintUsage(std::cerr, argv[0]);
    return 2;
  }

  std::string trusted_ca_override;
  if (!ca_certificate_path.empty()) {
    std::error_code path_error;
    const auto path = std::filesystem::absolute(ca_certificate_path, path_error);
    if (path_error || !std::filesystem::is_regular_file(path, path_error) || path_error) {
      std::cerr << "Unable to read CA certificate: " << ca_certificate_path << '\n';
      return 2;
    }
    trusted_ca_override = cim::ClientConfig::LoadCaCertificate(path);
    if (trusted_ca_override.empty()) {
      std::cerr << "Invalid CA certificate PEM: " << ca_certificate_path << '\n';
      return 2;
    }
  }

  if (sodium_init() < 0) {
    return 1;
  }
  auto screen = ftxui::ScreenInteractive::Fullscreen();

  cim::ChatUI chat_ui(
      [&screen] { screen.Post(ftxui::Event::Custom); },
      std::move(trusted_ca_override));
  screen.Loop(chat_ui.GetComponent());

  return 0;
}
