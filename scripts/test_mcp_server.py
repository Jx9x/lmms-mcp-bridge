import asyncio
from copy import deepcopy
import json
import threading
import unittest
from unittest.mock import Mock

from bridge_client import BridgeError
from lmms_mcp import BridgeGateway, create_server


def tool(name, properties=None, required=(), read_only=False, destructive=False):
    return {
        "name": name,
        "description": f"Native {name}.",
        "inputSchema": {
            "type": "object", "properties": properties or {}, "required": list(required), "additionalProperties": False,
        },
        "annotations": {"readOnlyHint": read_only, "destructiveHint": destructive, "openWorldHint": False},
    }


RETIRED = {
    "song", "tempo", "get_arrangement", "list_available_plugins", "list_effect_plugins", "add_note", "add_note_by_name",
    "set_parameter", "project.new", "list_zyn_presets", "tracks", "get_tracks", "get_notes", "load_preset",
    "add_instrument_track", "add_sample_track", "add_automation_track", "add_pattern_track",
    "rename_track", "set_track_volume", "set_track_panning", "mute_track", "solo_track", "assign_track_to_mixer",
    "rename_clip", "move_clip", "resize_clip", "set_tempo", "set_time_signature", "set_master_volume", "set_master_pitch",
    "set_mixer_channel_name", "set_mixer_channel_volume", "mute_mixer_channel", "solo_mixer_channel",
    "toggle_effect", "set_effect_wet", "play", "pause", "stop", "set_playhead", "set_automation_point",
}
OPTIONAL = {
    "help", "ping", "selftest", "show", "get_project_xml", "note_name_to_key", "key_to_note_name",
    "bars_to_ticks_converter", "ticks_to_bars_converter", "generate_scale", "set_zyn_params", "load_zyn_preset",
}
CONSOLIDATED = {
    "create_track", "update_track", "update_clip", "update_project", "update_mixer_channel", "update_effect", "transport",
}
INDEX = {"type": "integer", "minimum": 0, "maximum": 9999}
TEXT = {"type": "string", "maxLength": 32767}
SCOPE = {"container": {**TEXT, "enum": ["song", "bb"]}}
NOTE = {
    "type": "object", "additionalProperties": False, "required": [],
    "properties": {
        "key": {"oneOf": [{"type": "integer", "minimum": 12, "maximum": 119}, {"type": "string", "maxLength": 12}]},
        "pos": {"type": "integer", "minimum": 0}, "length": {"type": "integer", "minimum": 1},
        "volume": {"type": "integer", "minimum": 0, "maximum": 200},
    },
}
CATALOG = [
    tool("update_project", {"bpm": {"type": "integer", "minimum": 10, "maximum": 999}}),
    tool("create_track", {**SCOPE, "type": {**TEXT, "enum": ["instrument", "sample", "automation", "bb"]},
                          "name": {"type": "string", "maxLength": 256}}, ("type",)),
    tool("update_track", {**SCOPE, "track_index": INDEX, "name": TEXT}, ("track_index",)),
    tool("update_clip", {**SCOPE, "track_index": INDEX, "clip_index": INDEX}, ("track_index",)),
    tool("update_mixer_channel", {"channel_num": INDEX}, ("channel_num",)),
    tool("update_effect", {"target_index": INDEX, "effect_index": INDEX}, ("target_index", "effect_index")),
    tool("transport", {"action": {**TEXT, "enum": ["play", "pause", "stop", "seek"]}}, ("action",)),
    tool("get_pattern", {**SCOPE, "track_index": INDEX, "include_disabled": {"type": "boolean"}}, ("track_index",), True),
    tool("list_tracks", SCOPE, read_only=True),
    tool("add_notes_batch", {**SCOPE, "track_index": INDEX, "include_pattern": {"type": "boolean"},
                             "notes": {"type": "array", "items": NOTE, "minItems": 1, "maxItems": 512}},
         ("track_index", "notes")),
    tool("load_instrument_file", {**SCOPE, "track_index": INDEX, "path": TEXT, "preserve_name": {"type": "boolean"}},
         ("track_index", "path")),
    tool("scan_vst_directory", {"directory": TEXT}, ("directory",), True),
    tool("add_vst_track", {"plugin_path": TEXT}, ("plugin_path",)),
    tool("save_project", {"path": TEXT}, destructive=True),
] + [tool(name) for name in sorted(RETIRED | OPTIONAL)]


def native_note(index=0, volume=100):
    return {"index": index, "key": 48, "midi_key": 60, "lmms_key": 48, "note_name": "C4", "pos": index * 48,
            "length": 48, "len": 48, "volume": volume, "vol": volume, "panning": 0, "pan": 0}


class ServerTests(unittest.TestCase):
    def test_initialization_describes_default_playback_cap(self):
        server = create_server(BridgeGateway(request=Mock()))
        instructions = server.create_initialization_options().instructions
        self.assertIn("Playback volume is capped at 40 percent by default", instructions)


class GatewayTests(unittest.IsolatedAsyncioTestCase):
    def gateway(self, include_tools=()):
        def request(command, arguments):
            return {"ok": True, "tools": CATALOG} if command == "help" else {"ok": True, "cmd": command, "modified": True}

        mock = Mock(side_effect=request)
        return BridgeGateway(request=mock, include_tools=include_tools), mock

    async def test_catalog_filters_native_aliases_and_optional_tools(self):
        gateway, request = self.gateway()
        tools = await gateway.catalog()
        names = {item.name for item in tools}
        self.assertEqual(names, {item["name"] for item in CATALOG} - RETIRED - OPTIONAL)
        self.assertTrue(CONSOLIDATED <= names)
        self.assertTrue({"scan_vst_directory", "add_vst_track"} <= names)
        request.assert_called_once_with("help", None)

    async def test_compact_schema_removes_only_default_length_caps(self):
        gateway, _ = self.gateway()
        before = deepcopy(CATALOG)
        tools = {item.name: item for item in await gateway.catalog()}
        self.assertNotIn('"maxLength": 32767', json.dumps([item.model_dump(exclude_none=True) for item in tools.values()]))
        self.assertEqual(tools["create_track"].inputSchema["properties"]["name"]["maxLength"], 256)
        pitch = tools["add_notes_batch"].inputSchema["properties"]["notes"]["items"]["properties"]["key"]
        self.assertEqual(pitch["oneOf"][1]["maxLength"], 12)
        self.assertEqual(CATALOG, before)
        self.assertTrue(tools["get_pattern"].annotations.readOnlyHint)
        self.assertTrue(tools["save_project"].annotations.destructiveHint)
        self.assertFalse(tools["create_track"].inputSchema["additionalProperties"])
        self.assertEqual(tools["create_track"].inputSchema["required"], ["type"])
        self.assertIn("include_pattern", tools["add_notes_batch"].inputSchema["properties"])
        self.assertIn("full_detail", tools["add_notes_batch"].inputSchema["properties"])

    async def test_mcp_insertion_can_explicitly_request_full_pattern(self):
        gateway, request = self.gateway()
        await gateway.catalog()
        request.reset_mock()
        result = await gateway.call("add_notes_batch", {"track_index": 0, "notes": [{"key": 60}], "include_pattern": True})
        self.assertFalse(result.isError)
        request.assert_called_once_with("add_notes_batch", {"track_index": 0, "notes": [{"key": 60}], "include_pattern": True})

    async def test_full_detail_is_not_sent_to_native_and_retains_pattern(self):
        gateway, request = self.gateway()
        await gateway.catalog()
        request.reset_mock()
        request.side_effect = None
        request.return_value = {"ok": True, "notes": [native_note()], "pattern": {"notes": [native_note()]}}
        result = await gateway.call("add_notes_batch", {"track_index": 0, "notes": [{"key": 60}], "full_detail": True})
        self.assertIn("pattern", result.structuredContent)
        request.assert_called_once_with("add_notes_batch", {"track_index": 0, "notes": [{"key": 60}], "include_pattern": True})

    async def test_compact_defaults_and_explicit_detail_override(self):
        boolean = {"type": "boolean"}
        native = deepcopy(CATALOG)
        for item in native:
            if item["name"] == "update_track":
                item["inputSchema"]["properties"].update({key: boolean for key in ("include_notes", "include_parameters", "include_clips")})
        request = Mock(return_value={"ok": True, "tools": native})
        gateway = BridgeGateway(request=request)
        await gateway.catalog()
        request.reset_mock()
        request.return_value = {"ok": True}
        await gateway.call("update_track", {"track_index": 0, "name": "Lead"})
        request.assert_called_with("update_track", {"track_index": 0, "name": "Lead", "include_notes": False,
                                                   "include_parameters": False, "include_clips": False})
        await gateway.call("update_track", {"track_index": 0, "name": "Lead", "full_detail": True, "include_notes": False})
        request.assert_called_with("update_track", {"track_index": 0, "name": "Lead", "include_notes": False,
                                                   "include_parameters": True, "include_clips": True})

    async def test_validation_errors_are_compact_and_report_unchanged(self):
        gateway, _ = self.gateway()
        result = await gateway.call("update_project", {"bpm": 9})
        self.assertTrue(result.isError)
        self.assertFalse(result.structuredContent["changed"])
        self.assertNotIn("tools", result.structuredContent)
        self.assertLess(len(result.content[0].text), 300)

    async def test_large_invalid_arguments_are_not_echoed_into_errors(self):
        gateway, request = self.gateway()
        await gateway.catalog()
        request.reset_mock()
        cases = [
            ("load_instrument_file", {"track_index": 0, "path": "private-path-" * 3000}, "/path", "32767 characters"),
            ("create_track", {"type": "instrument", "name": "large-name-" * 1000}, "/name", "256 characters"),
            ("add_notes_batch", {"track_index": 0, "notes": [{"key": "large-key-" * 1000}]}, "/notes/0/key", "allowed form"),
            ("update_project", {"bpm": 128, "large-property-" * 1000: True}, "arguments", "unsupported properties"),
        ]
        for name, arguments, path, reason in cases:
            with self.subTest(name=name):
                result = await gateway.call(name, arguments)
                self.assertTrue(result.isError)
                self.assertFalse(result.structuredContent["changed"])
                self.assertEqual(json.loads(result.content[0].text), result.structuredContent)
                self.assertIn(path, result.structuredContent["error"])
                self.assertIn(reason, result.structuredContent["error"])
                self.assertLess(len(result.model_dump_json(exclude_none=True)), 1000)
                self.assertNotIn("large-", result.content[0].text)
                self.assertNotIn("private-path", result.content[0].text)
        request.assert_not_called()

    async def test_repeated_errors_do_not_attach_or_refetch_catalog(self):
        gateway, request = self.gateway()
        await gateway.catalog()
        request.reset_mock()
        errors = []
        for _ in range(3):
            errors.append(await gateway.call("update_project", {"bpm": 9}))
            errors.append(await gateway.call("unknown_tool", {}))
        request.assert_not_called()
        native = {"ok": False, "cmd": "update_project", "changed": False, "modified": True, "error": "busy: rendering"}
        request.side_effect = None
        request.return_value = native
        errors.append(await gateway.call("update_project", {"bpm": 128}))
        request.side_effect = BridgeError("pipe disconnected; inspect before retrying")
        errors.append(await gateway.call("update_project", {"bpm": 128}))
        self.assertEqual([call.args[0] for call in request.call_args_list], ["update_project", "update_project"])
        for result in errors:
            self.assertTrue(result.isError)
            self.assertEqual(json.loads(result.content[0].text), result.structuredContent)
            self.assertNotIn("tools", result.structuredContent)
            self.assertNotIn("inputSchema", result.model_dump_json(exclude_none=True))
            self.assertLess(len(result.model_dump_json(exclude_none=True)), 1000)
        request.side_effect = None
        request.return_value = {"ok": True, "cmd": "update_project", "changed": False, "modified": True}
        self.assertFalse((await gateway.call("update_project", {"bpm": 128})).isError)
        self.assertEqual([call.args[0] for call in request.call_args_list], ["update_project"] * 3)

    async def test_render_wait_is_scoped_to_job_and_returns_completion(self):
        native = deepcopy(CATALOG) + [tool("render_project", {"path": TEXT}, ("path",)),
                                     tool("render_status", {"job_id": INDEX}, read_only=True)]
        def request(command, arguments):
            if command == "help":
                return {"ok": True, "tools": native}
            if command == "render_project":
                self.assertEqual(arguments, {"path": "C:/render.wav"})
                return {"ok": True, "job": {"job_id": 7, "state": "running"}}
            self.assertEqual((command, arguments), ("render_status", {"job_id": 7}))
            return {"ok": True, "changed": False, "job": {"job_id": 7, "state": "succeeded", "frames": 44100}}
        gateway = BridgeGateway(request=Mock(side_effect=request))
        result = await gateway.call("render_project", {"path": "C:/render.wav", "wait_seconds": 1})
        self.assertFalse(result.isError)
        self.assertEqual(result.structuredContent["job"]["state"], "succeeded")
        self.assertEqual(result.structuredContent["cmd"], "render_project")

    async def test_render_wait_timeout_does_not_restart_or_cancel_job(self):
        native = deepcopy(CATALOG) + [tool("render_project", {"path": TEXT}, ("path",)),
                                     tool("render_status", {"job_id": INDEX}, read_only=True)]
        request = Mock(side_effect=lambda command, arguments: {"ok": True, "tools": native} if command == "help"
                       else {"ok": True, "job": {"job_id": 8, "state": "running"}})
        result = await BridgeGateway(request=request).call("render_project", {"path": "C:/render.wav", "wait_seconds": 0.01})
        self.assertTrue(result.structuredContent["wait_timed_out"])
        self.assertEqual([call.args[0] for call in request.call_args_list].count("render_project"), 1)
        self.assertNotIn("cancel_render", [call.args[0] for call in request.call_args_list])

    async def test_render_wait_failure_is_an_mcp_error(self):
        native = deepcopy(CATALOG) + [tool("render_project", {"path": TEXT}, ("path",)),
                                     tool("render_status", {"job_id": INDEX}, read_only=True)]
        request = Mock(side_effect=lambda command, arguments: {"ok": True, "tools": native} if command == "help"
                       else {"ok": True, "job": {"job_id": 9, "state": "running" if command == "render_project" else "failed", "error": "encoder failed"}})
        result = await BridgeGateway(request=request).call("render_project", {"path": "C:/render.wav", "wait_seconds": 1})
        self.assertTrue(result.isError)
        self.assertEqual(result.structuredContent["error"], "encoder failed")

    async def test_unpublished_length_cap_is_still_validated(self):
        gateway, request = self.gateway()
        await gateway.catalog()
        request.reset_mock()
        result = await gateway.call("load_instrument_file", {"track_index": 0, "path": "x" * 32768})
        self.assertTrue(result.isError)
        request.assert_not_called()

    async def test_optional_tools_require_explicit_inclusion(self):
        gateway, request = self.gateway(("get_project_xml", "generate_scale"))
        names = {item.name for item in await gateway.catalog()}
        self.assertEqual(names & OPTIONAL, {"get_project_xml", "generate_scale"})
        request.reset_mock()
        self.assertFalse((await gateway.call("get_project_xml")).isError)
        request.assert_called_once_with("get_project_xml", {})

    async def test_invalid_optional_tool_is_rejected(self):
        with self.assertRaises(ValueError):
            self.gateway(("not-a-tool",))

    async def test_missing_consolidated_native_commands_require_rebuild(self):
        native = [item for item in CATALOG if item["name"] != "update_track"]
        gateway = BridgeGateway(request=Mock(return_value={"ok": True, "tools": native}))
        with self.assertRaisesRegex(BridgeError, "rebuild"):
            await gateway.catalog()

    async def test_success_returns_structured_content(self):
        gateway, request = self.gateway()
        result = await gateway.call("update_project", {"bpm": 128})
        self.assertFalse(result.isError)
        self.assertTrue(result.structuredContent["modified"])
        self.assertEqual(request.call_count, 2)
        request.assert_called_with("update_project", {"bpm": 128})

    async def test_invalid_input_never_reaches_native_models(self):
        gateway, request = self.gateway()
        await gateway.catalog()
        request.reset_mock()
        for arguments in ({"bpm": 9}, {"bpm": 120.5}, {"bpm": True}, {"bpm": "120"}, {"bpm": 120, "unexpected": 1}, []):
            with self.subTest(arguments=arguments):
                result = await gateway.call("update_project", arguments)
                self.assertTrue(result.isError)
                self.assertFalse(result.structuredContent["ok"])
        request.assert_not_called()

    async def test_hidden_and_unknown_tools_are_rejected(self):
        gateway, request = self.gateway()
        await gateway.catalog()
        request.reset_mock()
        for name in RETIRED | OPTIONAL | {"not-a-tool"}:
            with self.subTest(name=name):
                result = await gateway.call(name, {})
                self.assertTrue(result.isError)
                self.assertFalse(result.structuredContent["changed"])
        request.assert_not_called()

    async def test_native_errors_are_preserved(self):
        gateway, request = self.gateway()
        await gateway.catalog()
        native = {"ok": False, "cmd": "update_project", "error": "busy: rendering", "modified": True}
        request.side_effect = None
        request.return_value = native
        result = await gateway.call("update_project", {"bpm": 128})
        self.assertTrue(result.isError)
        self.assertEqual(result.structuredContent, native)

    async def test_transport_errors_are_not_retried(self):
        gateway, request = self.gateway()
        await gateway.catalog()
        request.reset_mock()
        request.side_effect = BridgeError("pipe disconnected; inspect before retrying")
        result = await gateway.call("update_project", {"bpm": 128})
        self.assertTrue(result.isError)
        self.assertIsNone(result.structuredContent["changed"])
        self.assertTrue(result.structuredContent["outcome_indeterminate"])
        request.assert_called_once_with("update_project", {"bpm": 128})

    async def test_pattern_reader_normalizes_notes_and_filters_disabled(self):
        gateway, request = self.gateway()
        await gateway.catalog()
        native = {"ok": True, "pattern": {"length": 96, "notes": [native_note(), native_note(1, 0)]}}
        before = deepcopy(native)
        request.side_effect = None
        request.return_value = native
        result = await gateway.call("get_pattern", {"track_index": 0})
        self.assertEqual(result.structuredContent["pattern"], {"length": 96, "notes": [
            {"index": 0, "key": 60, "note_name": "C4", "pos": 0, "length": 48, "volume": 100, "panning": 0},
        ]})
        self.assertEqual(json.loads(result.content[0].text), result.structuredContent)
        result = await gateway.call("get_pattern", {"track_index": 0, "include_disabled": True})
        self.assertEqual([note["index"] for note in result.structuredContent["pattern"]["notes"]], [0, 1])
        self.assertEqual(native, before)

    async def test_nested_track_notes_use_the_same_compact_fields(self):
        gateway, request = self.gateway()
        await gateway.catalog()
        request.side_effect = None
        request.return_value = {"ok": True, "tracks": [{"tcos": [{"notes": [native_note()]}]}]}
        result = await gateway.call("list_tracks")
        note = result.structuredContent["tracks"][0]["tcos"][0]["notes"][0]
        self.assertEqual(note["key"], 60)
        self.assertTrue({"len", "vol", "pan", "midi_key", "lmms_key"}.isdisjoint(note))

    async def test_insert_returns_only_inserted_notes_not_entire_pattern(self):
        gateway, request = self.gateway()
        await gateway.catalog()
        native = {"ok": True, "cmd": "add_notes_batch", "modified": True, "pattern_index": 0,
                  "notes": [native_note(1)], "pattern": {"notes": [native_note(), native_note(1)]}}
        before = deepcopy(native)
        request.side_effect = None
        request.return_value = native
        result = await gateway.call("add_notes_batch", {"track_index": 0, "notes": [{"key": "C4", "pos": 48}]})
        self.assertFalse(result.isError)
        self.assertNotIn("pattern", result.structuredContent)
        self.assertEqual(result.structuredContent["pattern_index"], 0)
        self.assertEqual(result.structuredContent["notes"][0]["key"], 60)
        self.assertEqual(json.loads(result.content[0].text), result.structuredContent)
        request.assert_called_with("add_notes_batch", {"track_index": 0, "notes": [{"key": "C4", "pos": 48}], "include_pattern": False})
        self.assertEqual(native, before)

    async def test_failed_insertion_retains_partial_mutation_details(self):
        gateway, request = self.gateway()
        await gateway.catalog()
        request.side_effect = None
        request.return_value = {"ok": False, "modified": True, "error": "inspect partial mutation",
                                "pattern": {"notes": [native_note()]}}
        result = await gateway.call("add_notes_batch", {"track_index": 0, "notes": [{"key": "C4"}]})
        self.assertTrue(result.isError)
        self.assertTrue(result.structuredContent["modified"])
        self.assertIn("pattern", result.structuredContent)
        self.assertEqual(result.structuredContent["error"], "inspect partial mutation")

    async def test_invalid_catalog_is_rejected(self):
        for response in ({"ok": False, "error": "offline"}, {"ok": True}, {"ok": True, "tools": {}}, {"ok": True, "tools": []}):
            with self.subTest(response=response):
                gateway = BridgeGateway(request=Mock(return_value=response))
                with self.assertRaises(BridgeError):
                    await gateway.catalog()

    async def test_cancelled_request_keeps_serialization_until_io_finishes(self):
        entered, release = threading.Event(), threading.Event()
        calls = []

        def request(command, arguments):
            calls.append(command)
            if len(calls) == 1:
                entered.set()
                if not release.wait(2):
                    raise RuntimeError("test did not release the request")
            return {"ok": True}

        gateway = BridgeGateway(request=request)
        first = asyncio.create_task(gateway._request("first", {}))
        deadline = asyncio.get_running_loop().time() + 1
        while not entered.is_set() and asyncio.get_running_loop().time() < deadline:
            await asyncio.sleep(0.005)
        self.assertTrue(entered.is_set())
        first.cancel()
        second = asyncio.create_task(gateway._request("second", {}))
        try:
            await asyncio.sleep(0.02)
            self.assertEqual(calls, ["first"])
        finally:
            release.set()
        with self.assertRaises(asyncio.CancelledError):
            await first
        self.assertTrue((await second)["ok"])
        self.assertEqual(calls, ["first", "second"])


if __name__ == "__main__":
    unittest.main()
