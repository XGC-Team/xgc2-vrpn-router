#!/usr/bin/env python3
"""Real native VRPN peers and HTTP/UDS management, isolated temporary process tree.

The suite starts the production executable with a private common BootstrapInput.
"""
import argparse
import concurrent.futures
import http.client
import json
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import tempfile
import time


class HTTP(http.client.HTTPConnection):
    def __init__(self, path):
        super().__init__("localhost", timeout=3)
        self.path = str(path)

    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.settimeout(self.timeout)
        self.sock.connect(self.path)


def free_port():
    for _ in range(20):
        with socket.socket() as tcp, socket.socket(type=socket.SOCK_DGRAM) as udp:
            tcp.bind(("127.0.0.1", 0))
            port = tcp.getsockname()[1]
            try:
                udp.bind(("127.0.0.1", port))
                return port
            except OSError:
                pass
    raise AssertionError("could not allocate TCP+UDP test port")


def run(args):
    children, logs = [], []
    outcomes = {}
    with tempfile.TemporaryDirectory(prefix="vrpn-xrpc-") as temp:
        root = Path(temp)
        endpoint = root / "control.sock"
        up_port, down_port = free_port(), free_port()
        config = {
            "schema_version": 1, "upstream_host": "127.0.0.1", "upstream_port": up_port,
            "bind_address": "127.0.0.1", "listen_port": down_port,
            "mainloop_rate_hz": 500, "upstream_update_rate_hz": 0,
            "forwarding_enabled": True,
            "mappings": [{"upstream": "up", "downstream": "down", "sensors": 2}],
        }
        def command(document, socket_path, label):
            bootstrap = root / (label + "-bootstrap.json")
            bootstrap.write_text(json.dumps({
                "schema_version": 1,
                "binding": {
                    "schema_version": 1, "target_id": "fixture",
                    "service": "xgc2.vrpn-router", "api_version": "1", "profile": "http.v1",
                    "endpoint": {"kind": "unix", "address": str(socket_path)},
                    "runtime_grant": "runtime", "authentication": "local_private",
                    "secret_handles": {}, "storage_grants": [],
                },
                "grants": {}, "application": document,
            }))
            bootstrap.chmod(0o600)
            return [args.host, "--bootstrap-input", str(bootstrap)]

        host_command = command(config, endpoint, "host")

        def start(command, label):
            output = open(root / (label + ".log"), "wb")
            logs.append(output)
            process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT)
            children.append(process)
            return process

        sequence = 0
        instance = None

        def call(path, body=None, expected=200, bound=True, connection=None, headers=None):
            nonlocal sequence
            sequence += 1
            connection = connection or HTTP(endpoint)
            metadata = {"X-Request-ID": "test-" + str(sequence), "X-Xrpc-Timeout-Ms": "2000"}
            if bound and instance:
                metadata["X-Xrpc-Instance-ID"] = instance
            if headers:
                metadata.update(headers)
            wire = None if body is None else body if isinstance(body, str) else json.dumps(body)
            if wire is not None:
                metadata["Content-Type"] = "application/json"
            try:
                connection.request("GET" if body is None else "POST", path, wire, metadata)
                response = connection.getresponse()
                payload = response.read()
                assert response.status == expected, (path, response.status, payload)
                if bound and expected == 200:
                    assert response.getheader("X-Xrpc-Instance-ID") == instance
                return json.loads(payload)
            finally:
                connection.close()

        def ready(process):
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                assert process.poll() is None, "host exited before readiness"
                try:
                    description = call("/v1/describe", bound=False)
                    if description["lifecycle"] == "ready":
                        return description["service_ref"]["instance_id"]
                except (OSError, http.client.HTTPException):
                    pass
                time.sleep(0.02)
            raise AssertionError("host readiness timeout")

        try:
            upstream = start([args.peer, "source", f"127.0.0.1:{up_port}"], "upstream")
            host = start(host_command, "host")
            instance = ready(host)
            assert call("/v1/health")["downstream"]["listening"]
            consumer = subprocess.run([args.peer, "sink", f"127.0.0.1:{down_port}"], capture_output=True, timeout=12)
            assert consumer.returncode == 0, consumer.stdout + consumer.stderr
            outcomes["native_exact_pose_velocity_acceleration"] = consumer.stdout.decode().strip()
            mapped = call("/v1/mappings")["mappings"][0]
            for kind in ("pose", "velocity", "acceleration"):
                assert mapped[kind]["forwarded"] >= 10 and mapped[kind]["sample_age_ms"] < 1000
            outcomes["healthy"] = call("/v1/health")["domain_status"]
            assert outcomes["healthy"] == "streaming"
            assert call("/v1/config/schema")["persistence"] is False

            # Atomic live apply, stale revision, restart/persistence boundaries.
            result = call("/v1/config/apply", {"expected_revision": 1, "changes": {"forwarding_enabled": False, "mainloop_rate_hz": 300}})
            assert result["state"] == "applied" and result["desired_revision"] == result["applied_revision"] == 2
            assert result["persisted_revision"] is None
            before = call("/v1/mappings")["mappings"][0]["pose"]
            time.sleep(0.15)
            after = call("/v1/mappings")["mappings"][0]["pose"]
            assert after["received"] > before["received"] and after["forwarded"] == before["forwarded"]
            assert call("/v1/health")["domain_status"] == "paused"
            assert call("/v1/config/apply", {"expected_revision": 1, "changes": {"forwarding_enabled": True}}, 409)["error"]["code"] == "conflict"
            assert call("/v1/config/apply", {"expected_revision": 2, "changes": {"listen_port": free_port()}}, 409)["error"]["code"] == "restart_required"
            call("/v1/config/apply", {"expected_revision": 2, "persist": True, "changes": {"forwarding_enabled": True}}, 409)
            call("/v1/config/apply", {"expected_revision": 2, "changes": {"forwarding_enabled": True, "mainloop_rate_hz": -1}}, 400)
            call("/v1/config/apply", '{"expected_revision":2,"changes":{"forwarding_enabled":true,"forwarding_enabled":false}}', 400)
            call("/v1/config/apply", {"expected_revision": 2, "changes": {"typo": True}}, 400)
            still = call("/v1/config")
            assert still["applied_revision"] == 2 and not still["applied"]["forwarding_enabled"]
            call("/v1/config/apply", {"expected_revision": 2, "changes": {"forwarding_enabled": True}})
            outcomes["config_atomic_revision_negative_cases"] = "passed"

            # Authoritative transport fence and malformed metadata never dispatch.
            call("/v1/config", expected=409, headers={"X-Xrpc-Instance-ID": "stale"})
            call("/v1/config", expected=409, bound=False)
            call("/v1/describe", expected=400, bound=False, headers={"X-Xrpc-Timeout-Ms": "00"})
            policy = call("/v1/runtime-policy")
            updated = call("/v1/runtime-policy/apply", {"expected_revision": policy["revision"], "changes": {"LOG_LEVEL": "debug"}})
            assert updated["fields"]["LOG_LEVEL"]["value"] == "debug"
            call("/v1/runtime-policy/apply", {"expected_revision": updated["revision"], "changes": {"LOG_FORMAT": "text"}}, 409)
            outcomes["fencing_metadata_runtime_policy"] = "passed"

            # Native and control duplicate owners fail without damaging owner.
            duplicate = start(command(config, root / "other.sock", "duplicate-native"), "duplicate-native")
            assert duplicate.wait(timeout=5) != 0 and not (root / "other.sock").exists()
            other = dict(config, listen_port=free_port())
            duplicate = start(command(other, endpoint, "duplicate-control"), "duplicate-control")
            assert duplicate.wait(timeout=5) != 0
            call("/v1/health")
            outcomes["native_and_control_owner_conflicts"] = "passed"

            def native_handshake(peer):
                peer.settimeout(2)
                cookie = b""
                while len(cookie) < 24:
                    cookie += peer.recv(24 - len(cookie))
                assert len(cookie) == 24
                peer.sendall(cookie)

            def native_frame(kind, payload, sender=0):
                return struct.pack("!IIIiiI", 24 + len(payload), 0, 0, sender, kind, 0) + payload + bytes((-len(payload)) % 8)

            # Adversarial raw peers are only test fixtures. The product uses the
            # official native wire decoder, with bounded per-peer progress.
            for case in ("cookie", "header", "body"):
                print("native case", case, flush=True)
                with socket.create_connection(("127.0.0.1", down_port), timeout=2) as stalled_native:
                    if case == "cookie":
                        stalled_native.sendall(b"v")
                    else:
                        native_handshake(stalled_native)
                        frame = native_frame(-4, bytes(64))
                        stalled_native.sendall(frame[:3] if case == "header" else frame[:25])
                    baseline = call("/v1/mappings")["mappings"][0]["pose"]["forwarded"]
                    healthy = start([args.peer, "sink", f"127.0.0.1:{down_port}"], "healthy-during-" + case)
                    assert healthy.wait(timeout=3) == 0, case
                    assert call("/v1/mappings")["mappings"][0]["pose"]["forwarded"] > baseline
                    assert not call("/v1/health")["source_stale"]
                    # Incomplete frames must be evicted without another byte.
                    stalled_native.settimeout(4)
                    while stalled_native.recv(65536):
                        pass
                outcomes["native_slow_" + case] = "healthy tracker progresses; bad peer expires"
            applied = call("/v1/config/apply", {"expected_revision": 3, "changes": {"mainloop_rate_hz": 500}})
            assert applied["applied_revision"] == 4

            # A TCP-only peer stops reading after its cookie. Small receive
            # window plus the profile's bounded kernel send buffer forces real
            # backpressure while another native client continues to receive.
            with socket.socket() as reader:
                reader.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
                reader.settimeout(2)
                reader.connect(("127.0.0.1", down_port))
                native_handshake(reader)
                baseline = call("/v1/mappings")["mappings"][0]["pose"]["forwarded"]
                for repetition in range(7):
                    healthy = start([args.peer, "sink", f"127.0.0.1:{down_port}"], "healthy-reader-" + str(repetition))
                    assert healthy.wait(timeout=2) == 0
                    time.sleep(0.9)
                    assert not call("/v1/health")["source_stale"]
                assert call("/v1/mappings")["mappings"][0]["pose"]["forwarded"] > baseline + 300
                reader.settimeout(2)
                end = time.monotonic() + 2
                while reader.recv(65536):
                    assert time.monotonic() < end, "slow reader was not independently evicted"
            outcomes["native_slow_reader"] = "bounded send backlog; healthy tracker continues; peer evicted"

            # Native malformed metadata and remote persistence never reach a
            # domain handler or open a peer-selected file.
            forbidden = root / "peer-owned.log"
            attacks = [
                native_frame(-2147483648, b""),
                native_frame(-2, b""),
                native_frame(-2, struct.pack("!I", 0xffffffff)),
                native_frame(-1, struct.pack("!I", 2) + b"x\0", sender=-1),
                native_frame(-3, b"no-terminator", sender=4000),
                native_frame(-3, b"localhost\0", sender=4000),
                native_frame(-4, struct.pack("!II", len(str(forbidden)), 0) + str(forbidden).encode() + b"\0\0", sender=1),
            ]
            for attack_index, frame in enumerate(attacks):
                print("native attack", attack_index, flush=True)
                with socket.create_connection(("127.0.0.1", down_port), timeout=2) as malicious:
                    native_handshake(malicious)
                    malicious.sendall(frame)
                    malicious.settimeout(2)
                    try:
                        end = time.monotonic() + 3
                        while malicious.recv(65536):
                            assert time.monotonic() < end, ("bad peer not closed", attack_index)
                    except ConnectionResetError:
                        pass
                assert not forbidden.exists()
                assert call("/v1/health")["domain_status"] == "streaming"
            outcomes["native_malformed_metadata_remote_logging"] = "rejected; no peer-selected file"

            # Fixed HTTP connection admission while headers are deliberately
            # incomplete. This uses raw sockets only as an adversarial peer.
            slow_http = []
            try:
                for _ in range(20):
                    peer = socket.socket(socket.AF_UNIX)
                    peer.settimeout(1)
                    peer.connect(str(endpoint))
                    try:
                        peer.sendall(b"GET /v1/health HTTP/1.1\r\n")
                    except (BrokenPipeError, ConnectionResetError):
                        pass
                    slow_http.append(peer)
                time.sleep(0.05)
            finally:
                for peer in slow_http:
                    peer.close()
            time.sleep(0.05)
            assert call("/v1/health")["transport"]["connections"] <= 16
            outcomes["incomplete_header_admission"] = "bounded and recovers"

            # Concurrent management traffic and data plane continue together.
            start_forwarded = call("/v1/mappings")["mappings"][0]["pose"]["forwarded"]
            def load(_):
                for _ in range(25):
                    call("/v1/health")
            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                list(pool.map(load, range(8)))
            time.sleep(0.05)
            assert call("/v1/mappings")["mappings"][0]["pose"]["forwarded"] > start_forwarded
            outcomes["concurrent_management_and_data"] = "200 calls / 8 callers passed"

            upstream.terminate()
            upstream.wait(timeout=3)
            time.sleep(1.2)
            stale = call("/v1/mappings")["mappings"][0]["pose"]
            assert stale["sample_age_ms"] >= 1000
            assert call("/v1/health")["domain_status"] in ("stale", "upstream_disconnected")
            outcomes["upstream_exit_freshness"] = "passed"

            upstream = start([args.peer, "source", f"127.0.0.1:{up_port}"], "upstream-restarted")
            old = instance
            host.terminate()
            assert host.wait(timeout=7) == 0 and not endpoint.exists()
            host = start(host_command, "restarted")
            instance = ready(host)
            assert instance != old
            call("/v1/config", expected=409, headers={"X-Xrpc-Instance-ID": old})
            assert call("/v1/config")["applied_revision"] == 1
            old = instance
            host.kill()
            host.wait(timeout=3)
            host = start(host_command, "crash-restarted")
            instance = ready(host)
            assert instance != old
            call("/v1/config", expected=409, headers={"X-Xrpc-Instance-ID": old})
            host.terminate()
            assert host.wait(timeout=7) == 0 and not endpoint.exists()
            outcomes["exit_restart_crash_and_stale_fence"] = "passed"

            # Native partial peers no longer postpone quiescence. Exit must
            # destruct the native owner before the SDK releases its lease.
            host = start(host_command, "partial-peer-shutdown")
            instance = ready(host)
            with socket.create_connection(("127.0.0.1", down_port), timeout=2) as slow_shutdown:
                slow_shutdown.sendall(b"v")
                time.sleep(0.05)
                assert not call("/v1/health")["source_stale"]
                begin = time.monotonic()
                host.terminate()
                assert host.wait(timeout=3) == 0
                assert time.monotonic() - begin < 2
            assert not endpoint.exists()
            outcomes["partial_native_shutdown_quiescence"] = "graceful exit; owned inode removed"
        except BaseException:
            for log in logs:
                log.flush()
            for file in root.glob("*.log"):
                print(file.name, file.read_text(errors="replace")[-4000:])
            raise
        finally:
            for process in children:
                if process.poll() is None:
                    process.terminate()
            for process in children:
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3)
            for log in logs:
                log.close()
    print(json.dumps(outcomes, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True)
    parser.add_argument("--peer", required=True)
    run(parser.parse_args())
