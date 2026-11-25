"""Exercise the production background client against a local HTTP fixture.

Requires a host json-c static build (never the Switch library):
python3 tests/test_baidu_upload.py --json-source PATH --json-build PATH
"""
import argparse
import hashlib
import http.server
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time
import urllib.parse

parser = argparse.ArgumentParser()
parser.add_argument("--json-source", type=Path, required=True)
parser.add_argument("--json-build", type=Path, required=True)
args = parser.parse_args()
repo = Path(__file__).resolve().parents[1]
target = "/apps/JKSV/Test/backup.zip"
payload = b"a" * (4 * 1024 * 1024) + b"b"
LOCATED_HOST = "c3.pcs.baidu.com"
DEFAULT_UPLOAD_HOST = "d.pcs.baidu.com"


class Fixture(http.server.BaseHTTPRequestHandler):
    scenario = ""
    lists = 0
    slices = 0
    precreates = 0
    locates = 0
    refreshes = 0
    slice_hosts = []
    slice_tokens = []

    def log_message(self, *_):
        pass

    def respond(self, body, status=200):
        data = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def split(self):
        """Returns the scheme and host the client asked for, the path, and the query."""
        parts = urllib.parse.urlsplit(self.path)
        _, _, scheme, host, path = parts.path.split("/", 4)
        assert scheme == "https", self.path
        return host, "/" + path, urllib.parse.parse_qs(parts.query)

    def do_POST(self):
        host, _, query = self.split()
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        method = query["method"][0]
        form = urllib.parse.parse_qs(body.decode(errors="replace"))
        if method == "precreate":
            Fixture.precreates += 1
            rapid = self.scenario.startswith("rapid") and not (
                self.scenario == "rapid_recovery" and Fixture.precreates > 1)
            if self.scenario == "rapid_recovery" and Fixture.precreates > 1:
                assert "content-md5" not in form and "slice-md5" not in form
            return self.respond({"errno": 0, "return_type": 2 if rapid else 1,
                                 "uploadid": "test-upload", "block_list": [0, 1]})
        if method == "upload":
            Fixture.slices += 1
            Fixture.slice_hosts.append(host)
            Fixture.slice_tokens.append(query["access_token"][0])
            if self.scenario == "token_rejected" and query["access_token"] == ["test"]:
                # PCS says a token is bad with error_code, not errno.
                return self.respond({"error_code": 31045, "error_msg": "user not exists"})
            part = body.split(b"\r\n\r\n", 1)[1].rsplit(b"\r\n--", 1)[0]
            return self.respond({"errno": 0, "md5": hashlib.md5(part).hexdigest()})
        if method == "create":
            if form.get("isdir") == ["1"]:
                return self.respond({"errno": 0})
            if self.scenario == "create_error":
                return self.respond({"errno": 2})
            # Deliberately omit file metadata: only an independent lookup counts.
            return self.respond({"errno": 0})
        raise AssertionError(method)

    def do_GET(self):
        host, path, query = self.split()
        if path == "/oauth/2.0/token":
            return self.token(query)
        if path == "/rest/2.0/pcs/file":
            return self.locate(host, query)
        return self.listing(query)

    def token(self, query):
        Fixture.refreshes += 1
        assert query["grant_type"] == ["refresh_token"]
        assert query["refresh_token"] == ["test"], query
        return self.respond({"access_token": "refreshed-access", "refresh_token": "refreshed-refresh",
                             "expires_in": 2592000})

    def locate(self, host, query):
        Fixture.locates += 1
        assert host == DEFAULT_UPLOAD_HOST
        assert query["method"] == ["locateupload"] and query["appid"] == ["250528"]
        assert query["upload_version"] == ["2.0"] and query["uploadid"] == ["test-upload"]
        assert query["path"] == [target]
        if self.scenario == "locate_error":
            return self.respond({"error_code": 31023, "error_msg": "param error"})
        if self.scenario == "locate_insecure":
            # Anything but HTTPS has to be ignored. The token is in every upload URL.
            return self.respond({"error_code": 0, "servers": [{"server": f"http://{LOCATED_HOST}"}],
                                 "bak_servers": []})
        return self.respond({"error_code": 0, "servers": [{"server": f"https://{LOCATED_HOST}"}],
                             "bak_servers": [{"server": "https://c.pcs.baidu.com"}]})

    def listing(self, query):
        Fixture.lists += 1
        scenario = self.scenario
        assert query["method"] == ["list"]
        assert query["dir"] == ["/apps/JKSV/Test"]
        if scenario == "http_error":
            return self.respond({"errno": 0}, 503)
        if scenario == "malformed":
            return self.respond([])
        if scenario == "missing_list":
            return self.respond({"errno": 0})
        if (scenario.endswith("missing") or (scenario == "eventual" and self.lists == 1)
                or (scenario == "rapid_recovery" and Fixture.precreates == 1)):
            return self.respond({"errno": 0, "list": []})
        entry = {"path": target, "fs_id": 123, "size": len(payload), "isdir": 0}
        if scenario == "wrong_size":
            entry["size"] -= 1
        if scenario == "missing_size":
            del entry["size"]
        if scenario == "wrong_path":
            entry["path"] = "/apps/JKSV/Other/backup.zip"
        if scenario == "directory":
            entry["isdir"] = 1
        if scenario == "no_id":
            entry["fs_id"] = 0
        if scenario == "pagination" and query["start"] == ["0"]:
            return self.respond({"errno": 0, "list": [dict(entry, path=f"/other/{i}") for i in range(100)]})
        return self.respond({"errno": 0, "list": [entry]})


def write_config(path, **overrides):
    config = {"app_key": "test", "secret_key": "test", "access_token": "test",
              "refresh_token": "test", "expires_at": int(time.time()) + 3600}
    config.update(overrides)
    path.write_text(json.dumps(config))


def run_client(binary, work, port, **environment):
    return subprocess.run([str(binary)], cwd=work, capture_output=True, text=True,
                          env=dict(os.environ, JKSV_TEST_PORT=str(port), **environment), timeout=60)


def reset(scenario):
    Fixture.scenario, Fixture.lists, Fixture.slices = scenario, 0, 0
    Fixture.precreates = Fixture.locates = Fixture.refreshes = 0
    Fixture.slice_hosts, Fixture.slice_tokens = [], []


def leftovers(config):
    return [p.name for p in config.parent.iterdir() if p.name != config.name]


with tempfile.TemporaryDirectory(prefix="jksv-upload-test-") as temp:
    work = Path(temp)
    (work / "json-c").symlink_to(args.json_build.resolve(), target_is_directory=True)
    compile_flags = ["c++", "-std=c++20", "-I" + str(repo / "tests/host"),
                     "-I" + str(repo / "sysmodule/include"), "-I" + str(work),
                     "-I" + str(args.json_build.resolve()), "-I" + str(args.json_source.resolve()),
                     "-I" + str(repo / "include")]
    subprocess.run(compile_flags + ["-Dcurl_easy_perform=jksv_test_curl_easy_perform", "-c",
                    str(repo / "sysmodule/source/baidu.cpp"), "-o", str(work / "baidu.o")], check=True)
    binary = work / "test-upload"
    subprocess.run(compile_flags + [str(repo / "tests/host/baidu_upload_main.cpp"),
                    str(repo / "tests/host/local_http.cpp"), str(work / "baidu.o"),
                    str(args.json_build.resolve() / "libjson-c.a"), "-lcurl", "-o", str(binary)], check=True)
    config = work / "sdmc:/config/JKSV/baidu.json"
    config.parent.mkdir(parents=True)
    lock = Path(str(config) + ".lock")
    (work / "backup.zip").write_bytes(payload)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Fixture)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        cases = {"normal_success": True, "rapid_success": True, "normal_missing": False,
                 "rapid_missing": False, "wrong_size": False, "wrong_path": False,
                 "missing_size": False, "directory": False, "no_id": False,
                 "malformed": False, "missing_list": False, "http_error": False,
                 "create_error": False, "eventual": True, "pagination": True, "rapid_recovery": True,
                 "locate_error": True, "locate_insecure": True}
        for scenario, expected in cases.items():
            reset(scenario)
            write_config(config)
            result = run_client(binary, work, server.server_port)
            assert result.returncode == 0, (scenario, result.stdout, result.stderr)
            assert result.stdout.startswith("SUCCESS\n") == expected, (scenario, result.stdout)
            assert ("UPLOAD_VERIFIED" in result.stderr) == expected, (scenario, result.stderr)
            assert Fixture.slices == (0 if scenario in ("rapid_missing", "rapid_success") else 2), scenario
            if scenario == "pagination":
                assert Fixture.lists == 2
            if scenario.endswith("missing"):
                assert Fixture.lists == (6 if scenario == "rapid_missing" else 3)
            if scenario == "eventual":
                assert Fixture.lists == 2
            if scenario == "rapid_recovery":
                assert Fixture.precreates == 2 and Fixture.lists == 4
            # Slices go wherever locateupload says, falling back to the old fixed server.
            if Fixture.slices:
                expected_host = DEFAULT_UPLOAD_HOST if scenario.startswith("locate_") else LOCATED_HOST
                assert set(Fixture.slice_hosts) == {expected_host}, (scenario, Fixture.slice_hosts)
                assert Fixture.locates == 1, (scenario, Fixture.locates)
            if scenario.startswith("locate_"):
                assert "UPLOAD_HOST_FALLBACK" in result.stderr, result.stderr
            assert Fixture.refreshes == 0, scenario
            print("PASS", scenario)

        # An expired token is refreshed, and both the rotated tokens are saved without leaving partial files.
        reset("normal_success")
        write_config(config, expires_at=1)
        result = run_client(binary, work, server.server_port)
        assert result.stdout.startswith("SUCCESS\n"), result
        assert Fixture.refreshes == 1
        assert set(Fixture.slice_tokens) == {"refreshed-access"}, Fixture.slice_tokens
        saved = json.loads(config.read_text())
        assert saved["access_token"] == "refreshed-access" and saved["refresh_token"] == "refreshed-refresh"
        assert saved["app_key"] == "test", saved
        assert leftovers(config) == [], leftovers(config)
        print("PASS token_refresh")

        # JKSV holds the lock and refreshes in the meantime. The refresh token on disk when the worker started is used up
        # by then, so the worker has to pick up JKSV's tokens instead of refreshing again.
        reset("normal_success")
        write_config(config, expires_at=1)
        lock.write_text(str(int(time.time()) + 30))

        def other_process_refreshes():
            time.sleep(1.5)
            write_config(config, access_token="jksv-access", refresh_token="jksv-refresh",
                         expires_at=int(time.time()) + 2592000)
            lock.unlink()

        refresher = threading.Thread(target=other_process_refreshes)
        refresher.start()
        result = run_client(binary, work, server.server_port)
        refresher.join()
        assert result.stdout.startswith("SUCCESS\n"), result
        assert Fixture.refreshes == 0, "the used-up refresh token was sent again"
        assert set(Fixture.slice_tokens) == {"jksv-access"}, Fixture.slice_tokens
        assert "TOKEN_RELOADED" in result.stderr, result.stderr
        assert json.loads(config.read_text())["refresh_token"] == "jksv-refresh"
        assert leftovers(config) == [], leftovers(config)
        print("PASS token_reloaded_from_other_process")

        # A crash while refreshing leaves the lock behind. Its expiration lets the next refresh go ahead.
        reset("normal_success")
        write_config(config, expires_at=1)
        lock.write_text(str(int(time.time()) - 5))
        started = time.time()
        result = run_client(binary, work, server.server_port)
        assert result.stdout.startswith("SUCCESS\n"), result
        assert Fixture.refreshes == 1 and time.time() - started < 10
        assert leftovers(config) == [], leftovers(config)
        print("PASS abandoned_lock")

        # A lock that expires far in the future was written before the clock went back. Waiting it out could take days.
        reset("normal_success")
        write_config(config, expires_at=1)
        lock.write_text(f"{int(time.time()) + 10 ** 6} 1")
        started = time.time()
        result = run_client(binary, work, server.server_port)
        assert result.stdout.startswith("SUCCESS\n"), result
        assert Fixture.refreshes == 1 and time.time() - started < 10
        assert not lock.exists()
        print("PASS future_lock")

        # Baidu rejects the access token in the middle of an upload. It's refreshed and the upload runs again.
        reset("token_rejected")
        write_config(config)
        result = run_client(binary, work, server.server_port)
        assert result.stdout.startswith("SUCCESS\n"), result
        assert Fixture.refreshes == 1, Fixture.refreshes
        assert Fixture.slice_tokens[0] == "test" and Fixture.slice_tokens[-1] == "refreshed-access", Fixture.slice_tokens
        assert json.loads(config.read_text())["refresh_token"] == "refreshed-refresh"
        print("PASS token_rejected")

        # A game is running: no slice goes out, and it isn't reported as a failure.
        reset("normal_success")
        write_config(config)
        result = run_client(binary, work, server.server_port, JKSV_TEST_GAME_RUNNING="1")
        lines = result.stdout.split("\n")
        assert lines[0] == "FAILED" and lines[2] == "DEFERRED", result
        assert Fixture.slices == 0, Fixture.slices
        print("PASS deferred_for_game")

        # Power cut after the old file was moved aside but before the new one was renamed into place.
        reset("normal_success")
        write_config(Path(str(config) + ".tmp"), access_token="recovered-access")
        write_config(Path(str(config) + ".bak"), access_token="stale-access")
        config.unlink()
        result = run_client(binary, work, server.server_port)
        assert result.stdout.startswith("SUCCESS\n"), result
        assert set(Fixture.slice_tokens) == {"recovered-access"}, Fixture.slice_tokens
        print("PASS interrupted_write_recovery")

        # Power cut while the new file was being written: the old one has to win.
        reset("normal_success")
        write_config(config)
        Path(str(config) + ".tmp").write_text('{"app_key": "test", "access_tok')
        result = run_client(binary, work, server.server_port)
        assert result.stdout.startswith("SUCCESS\n"), result
        assert set(Fixture.slice_tokens) == {"test"}, Fixture.slice_tokens
        Path(str(config) + ".tmp").unlink()
        print("PASS truncated_write_ignored")
    finally:
        server.shutdown()
        server.server_close()
        worker.join()
