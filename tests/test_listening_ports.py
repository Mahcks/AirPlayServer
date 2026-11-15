"""Exercise real socket conflicts against the built Windows adapter."""
import argparse
import json
import os
from pathlib import Path
import queue
import socket
import subprocess
import tempfile
import threading
import time


def exercise(executable, dynamic):
    with tempfile.TemporaryDirectory(prefix="mirrorsim-ports-") as appdata:
        env = dict(os.environ, APPDATA=appdata, MIRRORSIM_EXTERNAL_DNSSD="1")
        env.pop("MIRRORSIM_DYNAMIC_PORTS", None)
        if dynamic:
            env["MIRRORSIM_DYNAMIC_PORTS"] = "1"
        process = subprocess.Popen(
            [str(executable)], cwd=executable.parent, env=env,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            text=True, creationflags=subprocess.CREATE_NO_WINDOW,
        )
        events = queue.Queue()

        def read_events():
            for line in process.stdout:
                events.put(json.loads(line))

        reader = threading.Thread(target=read_events, daemon=True)
        reader.start()

        def wait_for(name):
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                event = events.get(timeout=max(0.01, deadline - time.monotonic()))
                if event["name"] == name:
                    return event
                if event["name"] == "receiver_error" and not event["recoverable"]:
                    raise AssertionError(event)
            raise AssertionError(f"Timed out waiting for {name}")

        def send(command):
            process.stdin.write(json.dumps(command) + "\n")
            process.stdin.flush()

        try:
            ready = wait_for("receiver_ready")
            assert ("dynamic-listening-ports" in ready["capabilities"]) == dynamic
            for attempt in range(2 if dynamic else 1):
                session = f"ports-test-{attempt}"
                send({"name": "start_session", "session_id": session,
                      "expected_stream_id": session, "receiver_name": "Port regression test"})
                if not dynamic:
                    error = wait_for("receiver_error")
                    assert error["code"] == "start_failed" and not error["recoverable"]
                    break
                listening = wait_for("receiver_listening")
                assert listening["session_id"] == session
                ports = (listening["raop_port"], listening["airplay_port"])
                assert ports[0] != ports[1]
                assert all(port not in (5001, 7001, 7100) for port in ports)
                for port in ports:
                    with socket.create_connection(("127.0.0.1", port), timeout=2):
                        pass
                send({"name": "stop_session", "session_id": session})
                wait_for("stream_discontinuity")
            send({"name": "shutdown"})
            assert process.wait(timeout=10) == 0
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            reader.join(timeout=2)
            process.stdin.close()
            process.stdout.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("adapter", type=Path)
    args = parser.parse_args()
    listeners = []
    try:
        for port in (5001, 7001, 7100):
            listener = socket.socket()
            listeners.append(listener)
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            listener.bind(("0.0.0.0", port))
            listener.listen()
        exercise(args.adapter.resolve(), False)
        exercise(args.adapter.resolve(), True)
        print("PASS: legacy conflict fails; dynamic listeners recover, accept connections, and restart.")
    finally:
        for listener in listeners:
            listener.close()


if __name__ == "__main__":
    main()
