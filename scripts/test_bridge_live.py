import argparse
import asyncio
import json
from pathlib import Path
import sys

from bridge_client import BridgeClient
from test_windows import verify_close_prompt
from test_protocol_live import run_suite as protocol_suite


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("baseline", "features"), default="features")
    parser.add_argument("--artifacts", type=Path, required=True)
    options = parser.parse_args()
    options.artifacts.mkdir(parents=True, exist_ok=True)
    with (options.artifacts / "responses.jsonl").open("w", encoding="utf-8") as log:
        def call(command, arguments=None, expect_ok=True):
            try:
                with BridgeClient(timeout=35) as client:
                    result = client.call(command, arguments)
            except (RuntimeError, OSError, ValueError) as error:
                log.write(json.dumps({"request": command, "args": arguments, "transport_error": str(error)}, ensure_ascii=False) + "\n")
                log.flush()
                raise
            log.write(json.dumps({"request": command, "args": arguments, "response": result}, ensure_ascii=False) + "\n")
            log.flush()
            assert result["ok"] is expect_ok, result
            return result

        ping = call("ping")
        report = call("selftest")
        assert report["failed"] == 0, report
        before = call("song")
        assert not before["modified"], before
        protocol_suite(call)
        call(f"tempo {128 if before['tempo'] != 128 else 129}")
        after = call("song")
        assert after["modified"], "REGRESSION: changing tempo through the bridge did not mark the LMMS project modified"
        assert "*" in after["windowTitle"], after
        assert call("selftest")["failed"] == 0
        call("project.new", expect_ok=False)
        assert call("song")["file"] == before["file"], "Refused project.new must preserve the project"
        call("tempo bogus", expect_ok=False)
        print("PASS: bridge tempo edits mark the native LMMS song modified; unsafe new-project requests are refused")
        verify_close_prompt(call, options.artifacts, ping["pid"])
        if options.mode == "features":
            from test_composition_live import run_suite
            from test_render_live import run_suite as render_suite
            from test_mcp_live import run_suite as mcp_suite
            from test_mcp_errors_live import run_suite as mcp_error_suite
            from test_review_fixes_live import run_suite as review_suite
            run_suite(call, options.artifacts, ping["pid"])
            render_suite(call, options.artifacts)
            asyncio.run(mcp_suite(call, options.artifacts, ping["pid"]))
            asyncio.run(mcp_error_suite(call, options.artifacts, ping["pid"]))
            asyncio.run(review_suite(call, options.artifacts))
        else:
            saved = call("save_project", {"path": str(options.artifacts / "baseline-saved.mmp")})
            assert not saved["modified"] and "*" not in saved["windowTitle"], saved
            assert call("selftest")["failed"] == 0
            print("PASS: native LMMS save clears the modified flag and window marker")
        print("PASS: live bridge regression tests")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, RuntimeError, OSError, ValueError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
