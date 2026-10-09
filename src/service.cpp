#include "service.hpp"
#include <xgc2/xrpc/bounded_output.hpp>

#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>

namespace router {
namespace {
using namespace xgc2::xrpc;
struct DomainError : std::runtime_error {
    unsigned status;
    std::string code;
    DomainError(unsigned status, std::string code, const char* message)
        : std::runtime_error(message), status(status), code(std::move(code)) {}
};
Json::UInt64 millis(Clock::time_point start, Clock::time_point end) {
    return static_cast<Json::UInt64>(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
}
Json::Value sample(const SampleStats& stats, Clock::time_point now) {
    Json::Value value(Json::objectValue);
    value["received"] = Json::UInt64(stats.received);
    value["forwarded"] = Json::UInt64(stats.forwarded);
    value["send_failed"] = Json::UInt64(stats.send_failed);
    value["sample_age_ms"] = stats.received ? Json::Value(millis(stats.last_received, now)) : Json::Value();
    return value;
}
HttpResponse response(unsigned status, const Json::Value& value) {
    HttpResponse out;
    out.status = status;
    out.headers.emplace_back("Content-Type", "application/json");
    BoundedOutput buffer(256 * 1024);
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    std::unique_ptr<Json::StreamWriter> serializer(writer.newStreamWriter());
    serializer->write(value, &buffer.stream());
    if (!buffer.good())
        throw std::length_error("domain response exceeded bounded output");
    out.body = buffer.value();
    return out;
}
Json::Value policy_fields(std::span<const EffectiveRuntimeField> fields) {
    Json::Value value(Json::objectValue);
    for (const auto& field : fields) {
        Json::Value entry(Json::objectValue);
        if (const auto number = std::get_if<std::int64_t>(&field.value))
            entry["value"] = Json::Int64(*number);
        else
            entry["value"] = std::get<std::string>(field.value);
        entry["source"] = std::string(field.source);
        entry["source_detail"] = field.source_detail;
        entry["dynamic"] = field.dynamic;
        entry["unit"] = std::string(field.unit);
        entry["ceiling"] = field.ceiling ? Json::Value(Json::Int64(*field.ceiling)) : Json::Value();
        value[std::string(field.name)] = std::move(entry);
    }
    return value;
}
} // namespace

Service::Service(Runtime& router, Json::Value service_ref, const RuntimePolicy& policy, Diagnostics& diagnostics)
    : router_(router), service_ref_(std::move(service_ref)), policy_(policy), diagnostics_(diagnostics) {}

Json::Value Service::describe() const {
    Json::Value value(Json::objectValue);
    value["service_ref"] = service_ref_;
    const auto snapshot = router_.snapshot();
    value["lifecycle"] = draining_                                 ? "draining"
                         : snapshot.state == NativeState::Starting ? "starting"
                         : snapshot.state == NativeState::Ready    ? "ready"
                                                                   : "failed";
    value["capabilities"] = parse_json(R"({"data_plane":"vrpn.tracker", "online_configuration":true,
        "persistent_configuration":false,"online_mappings":false,"online_listener":false,
        "runtime_policy":true,"local_private":true,"remote_tls":false})");
    value["limits"]["mappings"] = Json::UInt64(max_mappings);
    value["limits"]["sensors_per_mapping"] = 256;
    value["limits"]["configuration_bytes"] = Json::UInt64(max_config_bytes);
    value["limits"]["native_endpoints_per_connection"] = vrpn_MAX_ENDPOINTS;
    value["limits"]["native_messages_per_channel_per_pump"] = native_messages_per_tick;
    value["limits"]["owner_threads"] = 2;
    value["limits"]["native_workers"] = 1;
    value["limits"]["pending_native_commands"] = 1;
    value["limits"]["pose_freshness_ms"] = 1000;
    return value;
}

Json::Value Service::mappings() const {
    const auto snapshot = router_.snapshot();
    const auto now = Clock::now();
    const auto& config = router_.initial_config();
    Json::Value value(Json::objectValue);
    value["source_age_ms"] = millis(snapshot.source_time, now);
    value["source_stale"] = millis(snapshot.source_time, now) > 1000;
    value["mappings"] = Json::Value(Json::arrayValue);
    for (std::size_t i = 0; i < config.mappings.size(); ++i) {
        const auto& mapping = config.mappings[i];
        const auto& stats = snapshot.routes[i];
        Json::Value entry(Json::objectValue);
        entry["upstream"] = mapping.upstream;
        entry["downstream"] = mapping.downstream;
        entry["sensors"] = mapping.sensors;
        entry["pose"] = sample(stats.pose, now);
        entry["velocity"] = sample(stats.velocity, now);
        entry["acceleration"] = sample(stats.acceleration, now);
        entry["invalid_sensor"] = Json::UInt64(stats.invalid_sensor);
        value["mappings"].append(std::move(entry));
    }
    return value;
}

Json::Value Service::health() const {
    const auto snapshot = router_.snapshot();
    const auto now = Clock::now();
    const auto& config = router_.initial_config();
    const bool stale = millis(snapshot.source_time, now) > 1000;
    Json::Value value(Json::objectValue);
    value["lifecycle"] = draining_                                 ? "draining"
                         : snapshot.state == NativeState::Starting ? "starting"
                         : snapshot.state == NativeState::Ready    ? "ready"
                                                                   : "failed";
    value["uptime_ms"] = millis(started_, now);
    value["source_age_ms"] = millis(snapshot.source_time, now);
    value["source_stale"] = stale;
    value["downstream"]["listening"] = stale ? Json::Value() : Json::Value(snapshot.listening);
    value["downstream"]["last_observed_listening"] = snapshot.listening;
    value["downstream"]["bind_address"] = config.bind_address;
    value["downstream"]["listen_port"] = config.listen_port;
    value["downstream"]["native_state_source"] = "vrpn_Connection::doing_okay";
    value["downstream"]["last_observed_client_connected"] = snapshot.downstream_connected;
    value["upstream"]["last_observed_connection_okay"] = snapshot.upstream_okay;
    value["upstream"]["last_observed_connected"] = snapshot.upstream_connected;
    value["upstream"]["update_rate_status"] =
        config.upstream_update_rate_hz > 0 ? "requested_unconfirmed" : "not_requested";
    bool all_received = true, all_fresh = true;
    for (std::size_t i = 0; i < config.mappings.size(); ++i) {
        const auto& pose = snapshot.routes[i].pose;
        all_received &= pose.received != 0;
        all_fresh &= pose.received && millis(pose.last_received, now) <= 1000;
    }
    const char* status = stale                                     ? "native_stalled"
                         : snapshot.state == NativeState::Starting ? "starting"
                         : snapshot.state != NativeState::Ready    ? "native_failed"
                         : !snapshot.listening                     ? "listener_failed"
                         : !snapshot.forwarding_enabled            ? "paused"
                         : !snapshot.upstream_connected            ? "upstream_disconnected"
                         : !all_received                           ? "waiting_for_samples"
                         : !all_fresh                              ? "stale"
                                                                   : "streaming";
    value["domain_status"] = status;
    value["forwarding_enabled"] = snapshot.forwarding_enabled;
    if (host_) {
        const auto stats = host_->stats();
        value["transport"]["connections"] = Json::UInt64(stats.active_connections);
        value["transport"]["inflight"] = Json::UInt64(stats.inflight_calls);
        value["transport"]["admitted_calls"] = Json::UInt64(stats.admitted_calls);
        value["transport"]["rejected_calls"] = Json::UInt64(stats.rejected_calls);
        value["transport"]["stopping"] = stats.stopping;
    }
    value["diagnostics"]["dropped"] = Json::UInt64(diagnostics_.stats().dropped());
    return value;
}

Json::Value Service::configuration() const {
    const auto snapshot = router_.snapshot();
    Json::Value value(Json::objectValue);
    value["desired_revision"] = Json::UInt64(std::max<std::uint64_t>(1, snapshot.applied_revision));
    value["applied_revision"] = Json::UInt64(snapshot.applied_revision);
    value["persisted_revision"] = Json::Value();
    value["persistence"] = "unsupported";
    value["applied_age_ms"] =
        snapshot.applied_revision ? Json::Value(millis(snapshot.applied_at, Clock::now())) : Json::Value();
    value["applied_request_id"] =
        snapshot.applied_request_id[0] ? Json::Value(snapshot.applied_request_id.data()) : Json::Value();
    value["desired"] =
        config_json(snapshot.applied_revision ? router_.applied_config(snapshot) : router_.initial_config());
    value["applied"] = snapshot.applied_revision ? value["desired"] : Json::Value();
    value["state"] = snapshot.applied_revision ? "applied" : "starting";
    return value;
}

void Service::apply(const Json::Value& request, const HttpRequest& metadata, HttpReply reply) {
    only_fields(request, {"expected_revision", "changes", "persist"});
    if (!request["expected_revision"].isUInt64() || !request["expected_revision"].asUInt64())
        throw std::invalid_argument("expected_revision must be a positive integer");
    if (request.isMember("persist") && !request["persist"].isBool())
        throw std::invalid_argument("persist must be boolean");
    if (request.get("persist", false).asBool())
        throw DomainError(409, "persistence_unsupported", "this provider has no granted durable configuration writer");
    const auto snapshot = router_.snapshot();
    if (snapshot.state != NativeState::Ready)
        throw DomainError(503, "unavailable", "native domain is not ready");
    if (request["expected_revision"].asUInt64() != snapshot.applied_revision)
        throw DomainError(409, "conflict", "configuration revision changed");
    const auto& changes = request["changes"];
    only_fields(changes, {"schema_version", "upstream_host", "upstream_port", "bind_address", "listen_port",
                          "mainloop_rate_hz", "upstream_update_rate_hz", "forwarding_enabled", "mappings"});
    if (changes.empty())
        throw std::invalid_argument("changes must not be empty");
    auto next = config_json(router_.applied_config(snapshot));
    for (const auto& name : changes.getMemberNames())
        next[name] = changes[name];
    const auto validated = parse_config(next);
    const auto current = config_json(router_.applied_config(snapshot));
    for (const auto& name : changes.getMemberNames()) {
        if (name == "schema_version")
            throw std::invalid_argument("schema_version is read-only");
        if (name != "mainloop_rate_hz" && name != "forwarding_enabled" && changes[name] != current[name])
            throw DomainError(409, "restart_required", "the requested change requires a new native instance");
    }
    if (snapshot.applied_revision == std::numeric_limits<std::uint64_t>::max())
        throw DomainError(409, "revision_exhausted", "configuration revision exhausted");
    if (pending_)
        throw DomainError(429, "resource_exhausted", "native command slot is occupied");
    LocalUpdate update;
    update.expected_revision = snapshot.applied_revision;
    update.rate = validated.mainloop_rate_hz;
    update.enabled = validated.forwarding_enabled;
    update.deadline = metadata.deadline;
    std::copy(metadata.request_id.begin(), metadata.request_id.end(), update.request_id.begin());
    const auto admitted = router_.submit(update);
    if (admitted == ApplyState::Conflict)
        throw DomainError(409, "conflict", "configuration revision changed");
    if (admitted == ApplyState::Stopped)
        throw DomainError(503, "unavailable", "native domain is stopping");
    if (admitted != ApplyState::Idle)
        throw DomainError(429, "resource_exhausted", "native command slot is occupied");
    pending_.emplace(std::move(reply));
}

void Service::complete_native_update() {
    if (!pending_)
        return;
    const auto outcome = router_.completion();
    if (outcome == ApplyState::Queued || outcome == ApplyState::Claimed)
        return;
    if (outcome == ApplyState::Applied)
        pending_->complete(response(200, configuration()));
    else if (outcome == ApplyState::Conflict)
        pending_->complete(http_error(409, "conflict", "native revision changed"));
    else if (outcome == ApplyState::Expired)
        pending_->complete(http_error(504, "deadline_exceeded", "native command expired before application"));
    else
        pending_->complete(http_error(503, "unavailable", "native domain stopped before application"));
    pending_.reset();
    router_.release_completion();
}

Json::Value Service::runtime_policy() const {
    Json::Value value(Json::objectValue);
    auto snapshot = diagnostics_.effective_policy();
    value["revision"] = Json::UInt64(snapshot.revision);
    value["fields"] = policy_fields(policy_.fields());
    auto dynamic = policy_fields(snapshot.entries());
    for (const auto& name : dynamic.getMemberNames())
        value["fields"][name] = dynamic[name];
    return value;
}

Json::Value Service::update_policy(const Json::Value& request) {
    only_fields(request, {"expected_revision", "changes"});
    if (!request["expected_revision"].isUInt64() || !request["expected_revision"].asUInt64())
        throw std::invalid_argument("expected_revision must be a positive integer");
    const auto& changes = request["changes"];
    only_fields(changes, {"LOG_LEVEL", "LOG_FORMAT"});
    if (changes.empty())
        throw std::invalid_argument("changes must not be empty");
    DiagnosticPolicyUpdate update;
    if (changes.isMember("LOG_LEVEL")) {
        if (!changes["LOG_LEVEL"].isString())
            throw std::invalid_argument("invalid LOG_LEVEL");
        const auto level = changes["LOG_LEVEL"].asString();
        if (level == "error")
            update.level = LogSeverity::Error;
        else if (level == "warn")
            update.level = LogSeverity::Warn;
        else if (level == "info")
            update.level = LogSeverity::Info;
        else if (level == "debug")
            update.level = LogSeverity::Debug;
        else if (level == "trace")
            update.level = LogSeverity::Trace;
        else
            throw std::invalid_argument("invalid LOG_LEVEL");
    }
    if (changes.isMember("LOG_FORMAT")) {
        if (!changes["LOG_FORMAT"].isString())
            throw std::invalid_argument("invalid LOG_FORMAT");
        const auto format = changes["LOG_FORMAT"].asString();
        if (format == "json")
            update.format = LogFormat::Json;
        else if (format == "text")
            update.format = LogFormat::Text;
        else
            throw std::invalid_argument("invalid LOG_FORMAT");
    }
    switch (diagnostics_.update(request["expected_revision"].asUInt64(), update)) {
    case DiagnosticUpdateResult::Applied:
        return runtime_policy();
    case DiagnosticUpdateResult::RestartRequired:
        throw DomainError(409, "restart_required", "LOG_FORMAT requires restart");
    case DiagnosticUpdateResult::RevisionConflict:
        throw DomainError(409, "conflict", "runtime policy revision changed");
    default:
        throw std::invalid_argument("runtime policy update rejected");
    }
}

void Service::dispatch(HttpRequest request, HttpReply reply) {
    try {
        if (draining_)
            throw DomainError(503, "unavailable", "native application is draining");
        if (reply.cancelled() || Clock::now() >= request.deadline)
            return;
        Json::Value value;
        const bool read = request.method == "GET" || request.method == "HEAD";
        if (read && !request.body.empty())
            throw std::invalid_argument("read methods do not accept a body");
        if (read && request.target == "/v1/describe")
            value = describe();
        else if (read && request.target == "/v1/health")
            value = health();
        else if (read && request.target == "/v1/mappings")
            value = mappings();
        else if (read && request.target == "/v1/config/schema")
            value = config_schema();
        else if (read && request.target == "/v1/config")
            value = configuration();
        else if (read && request.target == "/v1/runtime-policy")
            value = runtime_policy();
        else if (request.method == "POST" &&
                 (request.target == "/v1/config/apply" || request.target == "/v1/runtime-policy/apply")) {
            if (request.body.size() > max_config_bytes)
                throw DomainError(413, "resource_exhausted", "configuration body too large");
            unsigned content_types = 0;
            for (const auto& [name, value] : request.headers) {
                auto key = name;
                for (auto& c : key)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (key != "content-type")
                    continue;
                ++content_types;
                if (value != "application/json" && value != "application/json; charset=utf-8")
                    throw DomainError(415, "invalid_argument", "domain mutations require application/json");
            }
            if (content_types != 1)
                throw std::invalid_argument("exactly one JSON Content-Type is required");
            const auto document = parse_json(request.body);
            if (request.target == "/v1/config/apply") {
                apply(document, request, reply);
                return;
            }
            value = update_policy(document);
        } else
            throw DomainError(404, "not_found", "no such domain method");
        reply.complete(response(200, value));
    } catch (const std::invalid_argument& error) {
        Json::Value value;
        value["error"]["code"] = "invalid_argument";
        value["error"]["message"] = error.what();
        value["error"]["failure_stage"] = "validation";
        reply.complete(response(400, value));
    } catch (const DomainError& error) {
        Json::Value value;
        value["error"]["code"] = error.code;
        value["error"]["message"] = error.what();
        value["error"]["failure_stage"] = "validation";
        const auto revision = router_.snapshot().applied_revision;
        value["desired_revision"] = Json::UInt64(std::max<std::uint64_t>(1, revision));
        value["applied_revision"] = Json::UInt64(revision);
        reply.complete(response(error.status, value));
    }
}
} // namespace router
