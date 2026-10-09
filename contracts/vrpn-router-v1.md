# VRPN router management v1

`xgc2-vrpn-router` owns one native application, a single shared VRPN upstream
connection, a shared downstream VRPN server, and one XRPC `http.v1` host. The
control host shares native application startup and reports `starting` until
native listener construction and its first tick succeed. It drains only after
native objects are released. The process owner
starts/stops/restarts the application; a domain call never launches a process.

## Startup and reference

The production command is `xgc2-vrpn-router --bootstrap-input INPUT.json`.
The shared C++ loader validates the private BootstrapInput. Its `application`
is the original versioned domain JSON, with no independent path/port/tracker
CLI overrides. Service identity is `xgc2.vrpn-router`, API version `1`, profile
`http.v1`. The application generates its actual unpredictable instance ID on
each start. Only `GET /v1/describe` permits discovery without an instance; other
calls carry the actual reference, finite deadline and request ID through XRPC.

Configuration is bounded to 64 KiB, schema version 1. `config/router.json` is a
complete example. Legacy INI and individual tracker/port CLI overrides are
retired; there is one versioned domain input. The configured source document is
read-only. This provider has no configuration file/database writer, implicit
HOME/cwd/state directory or durable online save capability. The process/managed
file owner supplies the explicit input and runtime-directory grant. The binding
requires a Unix endpoint and `local_private` authentication, without storage
grants. The SDK retains the existing owned directory through host quiescence;
the application does not create it or reopen its pathname for the transport.
Optional owner authorization is enforced by the common loader. The original
systemd unit names the owner's explicit `/etc/xgc2/vrpn-router/bootstrap.json`;
it neither manufactures credentials nor prepares an input at startup.

## Methods

| Method | Result / effect |
| --- | --- |
| `GET /v1/describe` | Actual ServiceRef, native lifecycle, capabilities, fixed product limits |
| `GET /v1/health` | Maintained native connection state, sample-derived domain status, uptime/source age, transport admission and diagnostic drop counts |
| `GET /v1/mappings` | Immutable mapping roster and per-type received/forwarded/send-failed counters, monotonic receive age; never a network probe |
| `GET /v1/config/schema` | Schema and live/restart/read-only/persistence declarations |
| `GET /v1/config` | Desired and applied documents/revisions, applied age/request ID; persisted revision is null |
| `POST /v1/config/apply` | Atomic revision-checked update of supported local state |
| `GET /v1/runtime-policy` | SDK policy revision, effective field values, sources and ceilings; no bootstrap or unrelated environment |
| `POST /v1/runtime-policy/apply` | SDK diagnostics CAS for `LOG_LEVEL`; `LOG_FORMAT` changes require restart |

Read methods also support fenced HEAD. No tracker is exposed as a separate
listener. Native pose, velocity, acceleration, sensor indexes, quaternion data
and source timestamps remain native VRPN reports, not RPC payloads.

Configuration apply accepts
`{"expected_revision":1,"changes":{"forwarding_enabled":false,"mainloop_rate_hz":500},"persist":false}`.
Only forwarding enable and local cadence change online. All values and the
whole candidate document validate before effects. One fixed command slot hands
the two local values to the sole native owner. The held HTTP reply completes
only after the native owner rechecks deadline/revision and calls the domain
function. No queued acceptance is reported as completed application. The response is `state: applied` with
equal desired/applied revisions and the effective age. Calls that require a
different listener, upstream, roster or upstream update-rate request return
409 `restart_required` before effects. Unsupported persistence, stale revision,
read-only/unknown/invalid fields likewise leave all applied state unchanged.
The provider does not retain an unapplied desired draft. Request IDs correlate
the latest applied revision; they are not durable operation receipts. There is
no automatic mutation replay or exactly-once promise across restart.

The configured upstream update rate is a native VRPN request, not confirmed
physical rate. Health reports `requested_unconfirmed`. Required pose freshness
uses local steady-clock receipt age, with a declared 1000 ms threshold. Missing
velocity/acceleration reports remain null ages; they do not invent activity or
make an otherwise functioning pose source unhealthy. `forwarded` means accepted
by native VRPN report serialization, not acknowledged delivery to a client.

## Scheduling and bounds

One HTTP owner and one fixed native worker serve the whole application. The
worker owns all VRPN construction, native calls and destruction. The management
owner reads a fixed snapshot and never touches VRPN objects across threads.
This product is a best-effort network forwarder, not a hard realtime controller.
Native tracker mainloops, including their existing ping/pong semantics, are
retained. The router owns a fixed vector of at most 128 mappings,
at most 256 sensors per mapping, and no per-tracker threads/listeners/SDK pools.
VRPN's native endpoint ceiling is 256 per connection; the native input-pump
trigger is 256 messages per channel per pump, with VRPN's documented final UDP
packet overshoot. This is not a strict global messages-per-tick guarantee.

The pinned native VRPN profile retains the original decoder and forwarding
semantics while bounding peer-local progress. Configuration DNS uses c-ares
under the startup deadline. See [native-dependency.md](native-dependency.md) for
the source identity, original defect and finite profile limits.

On shutdown the application retains the management host and endpoint lease
while native work/destruction finishes. Only then may XRPC drain release the
lease. Failure to finish within the shared shutdown budget terminates the whole
process with exit code 2 without C++ teardown; this is failed quiescence, not a
graceful stop. OS process termination releases native resources together, and
an explicitly authorized XRPC reclaim policy is needed for the stale socket
inode. There is no unsafe detached worker or fake long-running RPC lease.

XRPC owns parser buffers, connection/inflight limits, finite deadlines,
keepalive, error framing, inode/lock lifecycle and cancelled replies. Product
JSON uses JsonCpp with duplicate keys/comments/trailing data rejected, bounded
depth and bounded output. Runtime environment is resolved once by the native
composition root using the common `XGC2_XRPC_` registry.

## Local evidence

`tests/process_test.py` uses temporary private endpoints and native VRPN peers.
It verifies exact pose/velocity/acceleration payload and timestamp preservation,
atomic apply and rejected partial updates, stale revisions, unknown/duplicate
fields, restart-only changes, persistence rejection, instance/metadata errors,
both listener-owner conflicts, concurrent control calls while data forwards,
upstream exit freshness, normal exit cleanup, and crash/restart stale fencing.
It also verifies healthy tracker progress during partial native cookies,
headers/bodies and slow-reader backpressure, malformed metadata rejection,
subsequent command reuse, and graceful partial-peer shutdown.
The tests start the production executable with actual private common
BootstrapInput files and owner-allocated directories. No systemd instance or
existing station participates.
