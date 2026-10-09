#pragma once
#include "router.hpp"
#include <json/json.h>
#include <string_view>

namespace router {
Json::Value parse_json(std::string_view bytes);
std::string read_document(const std::string& path, std::size_t limit);
void only_fields(const Json::Value& value, std::initializer_list<const char*> names);
Config parse_config(const Json::Value& value);
Json::Value config_json(const Config& config);
Json::Value config_schema();
} // namespace router
