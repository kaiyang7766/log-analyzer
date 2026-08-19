#pragma once

#include <filesystem>
#include <string>

#include "logscope/model.h"

namespace logscope {

FieldConfig load_config(const std::filesystem::path& path);
std::string default_config_yaml();

}  // namespace logscope
