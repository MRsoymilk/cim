#pragma once

#include <filesystem>
#include <string>

namespace cim {

std::string LoadAdminKey(const std::filesystem::path& path,
                         bool create_if_missing,
                         std::string& error);
bool IsAdminCommand(int argc, char** argv);
int RunAdminCommand(int argc, char** argv);

} // namespace cim
