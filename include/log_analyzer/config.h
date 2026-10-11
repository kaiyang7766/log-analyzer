#pragma once

#include <filesystem>
#include <string>

#include "log_analyzer/model.h"

namespace log_analyzer {

FieldConfig load_config(const std::filesystem::path& path);
std::string default_config_yaml();

}  // namespace log_analyzer
