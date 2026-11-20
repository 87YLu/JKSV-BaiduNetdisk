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


class Fixture(http.server.BaseHTTPRequestHandler):
    scenario = ""
    lists = 0
    slices = 0
    precreates = 0

    def log_message(self, *_):
        pass

    def respond(self, body, status=200):
        data = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        query = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        method = query["method"][0]
        form = urllib.parse.parse_qs(body.decode(errors="replace"))
        if method == "precreate":
            Fixture.precreates += 1
            rapid = self.scenario.startswith("rapid") and not (
                self.scenario == "rapid_recovery" and Fixture.precreates > 1)
            if Fixture.precreates > 1:
                assert "content-md5" not in form and "slice-md5" not in form
            return self.respond({"errno": 0, "return_type": 2 if rapid else 1,
                                 "uploadid": "test-upload", "block_list": [0, 1]})
        if method == "upload":
            Fixture.slices += 1
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
        Fixture.lists += 1
        scenario = self.scenario
        query = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)
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


with tempfile.TemporaryDirectory(prefix="jksv-upload-test-") as temp:
    work = Path(temp)
    (work / "json-c").symlink_to(args.json_build.resolve(), target_is_directory=True)
    compile_flags = ["c++", "-std=c++20", "-I" + str(repo / "tests/host"),
                     "-I" + str(repo / "sysmodule/include"), "-I" + str(work),
                     "-I" + str(args.json_build.resolve()), "-I" + str(args.json_source.resolve())]
    subprocess.run(compile_flags + ["-Dcurl_easy_perform=jksv_test_curl_easy_perform", "-c",
                    str(repo / "sysmodule/source/baidu.cpp"), "-o", str(work / "baidu.o")], check=True)
    binary = work / "test-upload"
    subprocess.run(compile_flags + [str(repo / "tests/host/baidu_upload_main.cpp"),
                    str(repo / "tests/host/local_http.cpp"), str(work / "baidu.o"),
                    str(args.json_build.resolve() / "libjson-c.a"), "-lcurl", "-o", str(binary)], check=True)
    config = work / "sdmc:/config/JKSV/baidu.json"
    config.parent.mkdir(parents=True)
    config.write_text(json.dumps({"app_key": "test", "secret_key": "test", "access_token": "test",
                                 "refresh_token": "test", "expires_at": int(time.time()) + 3600}))
    (work / "backup.zip").write_bytes(payload)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Fixture)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        cases = {"normal_success": True, "rapid_success": True, "normal_missing": False,
                 "rapid_missing": False, "wrong_size": False, "wrong_path": False,
                 "missing_size": False, "directory": False, "no_id": False,
                 "malformed": False, "missing_list": False, "http_error": False,
                 "create_error": False, "eventual": True, "pagination": True, "rapid_recovery": True}
        for scenario, expected in cases.items():
            Fixture.scenario, Fixture.lists, Fixture.slices = scenario, 0, 0
            Fixture.precreates = 0
            result = subprocess.run([str(binary)], cwd=work, capture_output=True, text=True,
                                    env=dict(os.environ, JKSV_TEST_PORT=str(server.server_port)), timeout=15)
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
            print("PASS", scenario)
    finally:
        server.shutdown()
        server.server_close()
        worker.join()
