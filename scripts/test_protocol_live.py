import json


def run_suite(call):
    before = call("get_project_xml")["xml"]
    for request_id in (37, "réponse", None):
        response = call(json.dumps({"cmd": "ping", "args": {}, "id": request_id}, ensure_ascii=False))
        assert response["id"] == request_id and response["pong"]
    assert call('{"cmd":"ping"}')["pong"]
    assert call("ping {}")["pong"]
    invalid = [
        '{"cmd":',
        '{"cmd":1}',
        '{"cmd":"ping","args":[]}',
        '{"cmd":"ping","args":{},"unsupported":true}',
        '{"cmd":"set_tempo","args":{"bpm":NaN}}',
        '{"cmd":"set_tempo","args":{"bpm":120.5}}',
        '{"cmd":"set_tempo","args":{"bpm":true}}',
        '{"cmd":"set_tempo","args":{"bpm":120,"unsupported":1}}',
        "set_tempo 120",
        "tempo 10.5",
        "play unexpected",
        "not-a-command",
    ]
    for request in invalid:
        response = call(request, expect_ok=False)
        assert not response["modified"] and call("get_project_xml")["xml"] == before, response
    for command, arguments in (
        ("load_project", {"path": "relative.mmp"}),
        ("load_project", {"path": "\\\\server\\share\\song.mmp"}),
        ("save_project", {"path": "C:\\unused\\song.mmp:alternate"}),
        ("render_project", {"path": "relative.wav"}),
        ("set_parameter", {"model_id": 4294967295, "value": 100}),
    ):
        response = call(command, arguments, expect_ok=False)
        assert not response["modified"] and call("get_project_xml")["xml"] == before, response
    print("PASS: live JSON/legacy framing, request ID echo, malformed/unknown arguments, local-path guards and stale parameter rejection without project edits")
