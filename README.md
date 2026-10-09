# XGC2 VRPN Router

One native process relays the original VRPN tracker pose, velocity and
acceleration reports between one upstream connection and one downstream server.
It has one fixed native worker and one XRPC management owner. Mappings, sensor
indexes, quaternion values and source timestamps retain the native VRPN format.
It has no ROS dependency or ROS topics.

Build with the installed shared C++ XRPC HTTP/bootstrap/diagnostics SDK,
JsonCpp, c-ares 1.34.8 and the pinned native VRPN profile supplied by the
versioned XGC2 build images. Images own the third-party source lock, bounded
progress patch and dependency build. The product builder verifies the installed
profile marker at `/opt/xgc2/vrpn-native` and consumes the separate pristine
official peers at `/opt/xgc2/vrpn-official`. Use `VRPN_ROOT` and the normal CMake
prefix path for those installed dependencies. Standard CMake build, CTest and
install entrypoints apply.

The only runtime input is:

```sh
xgc2-vrpn-router --bootstrap-input /absolute/owner/bootstrap.json
```

The shared loader validates the private BootstrapInput and runtime grant. Its
binding is `xgc2.vrpn-router`, API `1`, `http.v1`, Unix, `local_private`.
`application` contains the complete schema-version-1 document illustrated by
`config/router.json`. The owner allocates the private endpoint directory and
supplies optional authorization material. The router creates a fresh actual
ServiceRef incarnation and binds the retained directory; it does not discover
an endpoint, parse a second bootstrap format or create owner directories.
`--check-config` with the same input validates the domain before binding.

The original systemd unit consumes the owner's explicit
`/etc/xgc2/vrpn-router/bootstrap.json`. Native listener/upstream changes require
restart; forwarding enable and mainloop cadence can change through the existing
revision-checked RPC. No granted durable writer exists, so online persistence
remains explicitly unsupported as before.

See [the management contract](contracts/vrpn-router-v1.md) for methods, limits,
scientific report preservation, native completion and ordered Stop. The
existing native-process test now starts the production executable, uses private
owner inputs, and exercises the original transfer/configuration/fencing/shutdown
checks. Installed package/central station acceptance belongs to the release
owner and is separate from these isolated source checks.
