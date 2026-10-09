# Native VRPN dependency profile

`dependency/sources.lock.json` fixes upstream VRPN `v07.36` commit
`79deb000cc0b47ae49a80c92c78167c02d8a04d8`, the exact existing local hardening
patch, and c-ares 1.34.8. The builder verifies the patch hash before use. The
application keeps native VRPN report serialization and official wire framing;
management RPC never carries tracker data.

Unpatched upstream can block its sole native loop on partial cookies, message
headers/bodies, slow-reader output, connection setup and destructor flush.
Worker isolation alone leaves all healthy trackers stalled. The retained native
profile adds bounded per-peer progress, finite connection/output behavior and
strict malformed metadata rejection inside that original library. Configured
hostname resolution uses c-ares under the existing finite startup deadline and
cancellation flag, without a DNS thread or hidden refresh path.

The original `tests/process_test.py` exposes the relevant behavior through real
native peers: partial cookie/header/body, a slow reader while healthy consumers
progress, malformed type/sender metadata, no remote-selected log file,
configuration deadlines, owner conflicts, crash fencing and ordered partial-peer
shutdown. These remain isolated checks, without claims of hard realtime,
physical-device acknowledgement or deployed Focal ABI. Optional original
pristine-official-peer interoperability remains available through its existing
CMake input; this iteration does not expand that matrix.

The application retains the HTTP host, pending business reply and endpoint
lease until the native command or owner actually finishes. On completion it
clears the held reply before admitting reuse. Shutdown joins the sole native
worker, completes the last native outcome, then drains HTTP. Failure to achieve
native quiescence within the SDK shutdown budget exits 2; it is never reported
as graceful Stop and does not destruct live worker state.
