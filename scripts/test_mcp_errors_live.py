"""Read-only MCP error regression against an already running bridge."""

from datetime import timedelta
from hashlib import sha256
import json
from pathlib import Path
import sys

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


async def run_suite(call, artifacts, process_id):
    assert call("ping")["pid"] == process_id
    parameters = StdioServerParameters(
        command=sys.executable,
        args=[str(Path(__file__).with_name("lmms_mcp.py")), "--include-tool", "get_project_xml"],
    )
    report = []
    async with stdio_client(parameters) as (reader, writer):
        async with ClientSession(reader, writer, read_timeout_seconds=timedelta(seconds=40)) as session:
            await session.initialize()
            catalog = await session.list_tools()
            assert len(catalog.tools) == 61
            before = (await session.call_tool("get_project_xml", {})).structuredContent
            before_info = (await session.call_tool("get_project_info", {})).structuredContent
            assert before["ok"] and before_info["ok"]
            cases = [
                ("update_project", {"bpm": 9}),
                ("get_track", {"track_index": 9999}),
                ("unknown_tool", {}),
            ] * 3 + [
                ("load_instrument_file", {"track_index": 0, "path": "private-path-" * 3000}),
                ("create_track", {"type": "instrument", "name": "large-name-" * 1000}),
                ("update_project", {"numerator": 3}),
            ]
            for name, arguments in cases:
                result = await session.call_tool(name, arguments)
                payload = result.structuredContent
                assert result.isError and payload["ok"] is False, result
                assert payload["changed"] is False, payload
                assert json.loads(result.content[0].text) == payload, result
                serialized = result.model_dump_json(exclude_none=True)
                assert len(serialized) < 1800, len(serialized)
                assert "tools" not in payload and "inputSchema" not in serialized
                assert "private-path-" not in serialized and "large-name-" not in serialized
                report.append({"tool": name, "error": payload["error"], "response_chars": len(serialized),
                               "is_error": result.isError, "changed": payload["changed"]})
            after = (await session.call_tool("get_project_xml", {})).structuredContent
            after_info = (await session.call_tool("get_project_info", {})).structuredContent
            assert after["xml"] == before["xml"]
            assert after_info == before_info
    summary = {
        "checks": len(report), "responses": report,
        "catalog_attached": False, "project_xml_unchanged": True,
        "project_xml_sha256": sha256(before["xml"].encode("utf-8")).hexdigest(),
        "max_response_chars": max(item["response_chars"] for item in report),
    }
    (artifacts / "mcp-error-report.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(f"PASS: {len(report)} MCP stdio error responses, no catalog or argument echoes, "
          f"maximum {summary['max_response_chars']} characters; project XML and dirty state unchanged")
