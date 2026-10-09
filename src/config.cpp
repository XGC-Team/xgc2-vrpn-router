#include "config.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace router {
namespace {
bool contains_comment(const Json::Value& value) {
    for (int placement = Json::commentBefore; placement < Json::numberOfCommentPlacement; ++placement)
        if (value.hasComment(static_cast<Json::CommentPlacement>(placement)))
            return true;
    if (value.isArray() || value.isObject())
        for (const auto& member : value)
            if (contains_comment(member))
                return true;
    return false;
}
} // namespace
Json::Value parse_json(std::string_view bytes) {
    Json::CharReaderBuilder builder;
    // JsonCpp 1.9.5 accepts comments between object members even when
    // allowComments=false. Retain its parsed comment metadata and reject it
    // explicitly, rather than implementing a second JSON lexer.
    builder["collectComments"] = true;
    builder["allowComments"] = true;
    builder["allowTrailingCommas"] = false;
    builder["strictRoot"] = true;
    builder["failIfExtra"] = true;
    builder["rejectDupKeys"] = true;
    builder["stackLimit"] = 16;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value value;
    std::string error;
    if (!reader->parse(bytes.data(), bytes.data() + bytes.size(), &value, &error) || contains_comment(value))
        throw std::invalid_argument("invalid JSON document");
    return value;
}

std::string read_document(const std::string& path, std::size_t limit) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open explicit input document");
    std::string bytes(limit + 1, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (input.bad())
        throw std::runtime_error("cannot read explicit input document");
    bytes.resize(static_cast<std::size_t>(input.gcount()));
    if (bytes.size() > limit)
        throw std::invalid_argument("input document exceeds size limit");
    return bytes;
}

void only_fields(const Json::Value& value, std::initializer_list<const char*> names) {
    if (!value.isObject())
        throw std::invalid_argument("expected a JSON object");
    for (const auto& name : value.getMemberNames()) {
        if (std::find(names.begin(), names.end(), name) == names.end())
            throw std::invalid_argument("unknown field: " + name);
    }
}

namespace {
std::uint16_t port(const Json::Value& value) {
    if (!value.isUInt() || !value.asUInt() || value.asUInt() > 65535)
        throw std::invalid_argument("port must be an integer in 1..65535");
    return static_cast<std::uint16_t>(value.asUInt());
}
std::string string(const Json::Value& value) {
    if (!value.isString())
        throw std::invalid_argument("expected a string");
    return value.asString();
}
double number(const Json::Value& value) {
    if (!value.isNumeric())
        throw std::invalid_argument("expected a number");
    return value.asDouble();
}
} // namespace

Config parse_config(const Json::Value& value) {
    only_fields(value, {"schema_version", "upstream_host", "upstream_port", "bind_address", "listen_port",
                        "mainloop_rate_hz", "upstream_update_rate_hz", "forwarding_enabled", "mappings"});
    if (!value["schema_version"].isUInt() || value["schema_version"].asUInt() != 1)
        throw std::invalid_argument("schema_version must be 1");
    Config config;
    config.upstream_host = string(value["upstream_host"]);
    config.upstream_port = port(value["upstream_port"]);
    config.bind_address = string(value["bind_address"]);
    config.listen_port = port(value["listen_port"]);
    config.mainloop_rate_hz = number(value["mainloop_rate_hz"]);
    config.upstream_update_rate_hz = number(value["upstream_update_rate_hz"]);
    if (!value["forwarding_enabled"].isBool())
        throw std::invalid_argument("forwarding_enabled must be boolean");
    config.forwarding_enabled = value["forwarding_enabled"].asBool();
    const auto& mappings = value["mappings"];
    if (!mappings.isArray() || mappings.empty() || mappings.size() > max_mappings)
        throw std::invalid_argument("mappings count must be in 1..128");
    config.mappings.reserve(mappings.size());
    for (const auto& mapping : mappings) {
        only_fields(mapping, {"upstream", "downstream", "sensors"});
        if (!mapping["sensors"].isInt())
            throw std::invalid_argument("sensors must be an integer");
        config.mappings.push_back(
            {string(mapping["upstream"]), string(mapping["downstream"]), mapping["sensors"].asInt()});
    }
    validate(config);
    return config;
}

Json::Value config_json(const Config& config) {
    Json::Value out(Json::objectValue);
    out["schema_version"] = 1;
    out["upstream_host"] = config.upstream_host;
    out["upstream_port"] = config.upstream_port;
    out["bind_address"] = config.bind_address;
    out["listen_port"] = config.listen_port;
    out["mainloop_rate_hz"] = config.mainloop_rate_hz;
    out["upstream_update_rate_hz"] = config.upstream_update_rate_hz;
    out["forwarding_enabled"] = config.forwarding_enabled;
    out["mappings"] = Json::Value(Json::arrayValue);
    for (const auto& mapping : config.mappings) {
        Json::Value entry(Json::objectValue);
        entry["upstream"] = mapping.upstream;
        entry["downstream"] = mapping.downstream;
        entry["sensors"] = mapping.sensors;
        out["mappings"].append(std::move(entry));
    }
    return out;
}

Json::Value config_schema() {
    return parse_json(R"({
      "schema_version":1,"atomic":true,"persistence":false,
      "live_fields":["mainloop_rate_hz","forwarding_enabled"],
      "restart_required_fields":["upstream_host","upstream_port","bind_address","listen_port","upstream_update_rate_hz","mappings"],
      "read_only_fields":["schema_version"],
      "schema":{"type":"object","additionalProperties":false,
        "required":["schema_version","upstream_host","upstream_port","bind_address","listen_port","mainloop_rate_hz","upstream_update_rate_hz","forwarding_enabled","mappings"],
        "properties":{
          "schema_version":{"const":1},
          "upstream_host":{"type":"string","minLength":1,"maxLength":253,"pattern":"^[A-Za-z0-9.-]+$"},
          "upstream_port":{"type":"integer","minimum":1,"maximum":65535},
          "bind_address":{"type":"string","maxLength":253,"pattern":"^[A-Za-z0-9.-]*$"},
          "listen_port":{"type":"integer","minimum":1,"maximum":65535},
          "mainloop_rate_hz":{"type":"number","minimum":1,"maximum":2000},
          "upstream_update_rate_hz":{"type":"number","minimum":0,"maximum":2000},
          "forwarding_enabled":{"type":"boolean"},
          "mappings":{"type":"array","minItems":1,"maxItems":128,"items":{
            "type":"object","additionalProperties":false,"required":["upstream","downstream","sensors"],
            "properties":{
              "upstream":{"type":"string","minLength":1,"maxLength":63,"pattern":"^[A-Za-z0-9_.-]+$"},
              "downstream":{"type":"string","minLength":1,"maxLength":63,"pattern":"^[A-Za-z0-9_.-]+$"},
              "sensors":{"type":"integer","minimum":1,"maximum":256}
            }}}
        }}})");
}
} // namespace router
