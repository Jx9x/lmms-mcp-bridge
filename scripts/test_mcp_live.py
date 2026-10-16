import asyncio
from datetime import timedelta
import json
from pathlib import Path
import sys

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

from lmms_mcp import CONSOLIDATED_TOOLS, OPTIONAL_TOOLS, REPLACED_TOOLS


async def run_suite(call, artifacts, process_id):
    parameters = StdioServerParameters(command=sys.executable, args=[str(Path(__file__).with_name("lmms_mcp.py"))])
    report = []
    assert call("ping")["pid"] == process_id
    async with stdio_client(parameters) as (reader, writer):
        async with ClientSession(reader, writer, read_timeout_seconds=timedelta(seconds=40)) as session:
            initialized = await session.initialize()
            assert initialized.serverInfo.name == "lmms-native-bridge"
            catalog = await session.list_tools()
            direct = call("help")["tools"]
            names = {tool.name for tool in catalog.tools}
            assert names == {tool["name"] for tool in direct} - REPLACED_TOOLS - OPTIONAL_TOOLS
            assert CONSOLIDATED_TOOLS <= names and {"scan_vst_directory", "add_vst_track"} <= names
            assert len(catalog.tools) == 60, len(catalog.tools)
            assert {"list_samples", "place_patterns_batch", "set_notes_batch", "replace_notes"} <= names
            definitions = [tool.model_dump(exclude_none=True) for tool in catalog.tools]
            assert '"maxLength": 32767' not in json.dumps(definitions)
            (artifacts / "tools.json").write_text(json.dumps(definitions, indent=2), encoding="utf-8")
            legacy = [tool for tool in direct if tool["name"] not in CONSOLIDATED_TOOLS]
            size = {
                "native_tools": len(direct), "legacy_tools": len(legacy), "mcp_tools": len(definitions),
                "legacy_schema_chars": len(json.dumps(legacy, ensure_ascii=False, separators=(",", ":"))),
                "mcp_schema_chars": len(json.dumps(definitions, ensure_ascii=False, separators=(",", ":"))),
            }
            (artifacts / "mcp-catalog-size.json").write_text(json.dumps(size, indent=2), encoding="utf-8")

            async def invoke(name, arguments=None, expect_ok=True):
                result = await session.call_tool(name, arguments or {})
                payload = result.structuredContent
                assert isinstance(payload, dict), result
                assert result.isError is not expect_ok and payload["ok"] is expect_ok, result
                assert json.loads(result.content[0].text) == payload, result
                report.append({"tool": name, "args": arguments, "result": payload})
                return payload

            async def save_clean():
                saved = await invoke("save_project", {"path": str(artifacts / "mcp-checkpoint.mmp"), "overwrite": True})
                assert not saved["modified"] and "*" not in saved["windowTitle"], saved

            async def reject(name, arguments):
                await save_clean()
                xml = call("get_project_xml")["xml"]
                response = await invoke(name, arguments, expect_ok=False)
                assert not response.get("modified", False), response
                assert not (await invoke("get_project_info"))["modified"], response
                assert call("get_project_xml")["xml"] == xml, response

            def compact_notes(notes):
                assert all({"len", "vol", "pan", "midi_key", "lmms_key"}.isdisjoint(note) for note in notes), notes
                assert all({"key", "note_name", "pos", "length", "volume", "panning"} <= note.keys() for note in notes), notes

            before = await invoke("get_project_info")
            assert not before["modified"]
            original = before["file"]
            for name in ("ping", "selftest", "get_notes", "set_tempo", "add_note", "generate_scale"):
                await reject(name, {})
            await reject("update_project", {"bpm": 126.5})
            await reject("update_project", {"bpm": 126, "numerator": 4, "denominator": 3})
            await reject("update_project", {"bpm": 126, "numerator": 3})
            await reject("update_project", {})
            await invoke("update_project", {"bpm": 126, "numerator": 4, "denominator": 4, "master_volume": 70, "master_pitch": 0})
            project = await invoke("get_project_info")
            assert project["modified"] and project["tempo"] == 126 and project["timeSig"] == [4, 4], project
            assert project["masterVolume"] == 70 and project["masterPitch"] == 0, project
            await invoke("create_project", expect_ok=False)
            assert (await invoke("get_project_info"))["file"] == project["file"]
            await reject("create_track", {"type": "instrument", "instrument": "not-a-real-plugin"})
            await reject("create_track", {"type": "sample", "volume": 30})
            await reject("create_track", {"type": "bb", "container": "bb"})
            lead = (await invoke("create_track", {"type": "instrument", "name": "MCP Lead", "volume": 20}))["track_index"]
            sample = (await invoke("create_track", {"type": "sample", "name": "MCP Sample"}))["track_index"]
            automation = (await invoke("create_track", {"type": "automation", "name": "MCP Automation"}))["track_index"]
            bb = (await invoke("create_track", {"type": "bb", "name": "MCP Drums"}))["track_index"]
            drum = (await invoke("create_track", {"type": "instrument", "container": "bb", "name": "MCP Kick", "instrument": "kicker"}))["track_index"]
            await invoke("add_notes_batch", {"container": "bb", "track_index": drum, "pattern_index": 0,
                                              "notes": [{"key": "C4", "length": -1}]})
            await invoke("place_bb_clip", {"track_index": bb, "pos_bars": 0})
            assert (await invoke("get_pattern", {"container": "bb", "track_index": drum, "pattern_index": 0}))["pattern"]["notes"][0]["key"] == 60
            mixer = (await invoke("add_mixer_channel", {"name": "MCP Bus"}))["channel_num"]
            await reject("update_track", {"track_index": lead, "name": "Must not rename", "volume": 35, "fx_channel": 9999})
            await reject("update_track", {"track_index": sample, "name": "Must not rename", "fx_channel": 0})
            await reject("update_track", {"track_index": lead})
            track = (await invoke("update_track", {"track_index": lead, "name": "MCP Lead Updated", "volume": 35,
                                                     "panning": -10, "muted": True, "solo": False, "fx_channel": mixer}))["track"]
            assert track["name"] == "MCP Lead Updated" and track["volume"] == 35 and track["panning"] == -10, track
            assert track["muted"] and not track["solo"] and track["fx_channel"] == mixer, track
            await invoke("update_track", {"track_index": lead, "muted": False})
            await invoke("update_track", {"track_index": sample, "volume": 30, "panning": 20})
            inserted = await invoke("add_notes_batch", {"track_index": lead, "notes": [
                {"key": "C4", "pos": 0, "length": 48}, {"key": 64, "pos": 48, "length": 48, "volume": 0},
            ]})
            assert "pattern" not in inserted and inserted["pattern_index"] == 0, inserted
            compact_notes(inserted["notes"])
            assert [note["key"] for note in inserted["notes"]] == [60, 64], inserted
            pattern = (await invoke("get_pattern", {"track_index": lead}))["pattern"]
            compact_notes(pattern["notes"])
            assert len(pattern["notes"]) == 1 and pattern["notes"][0]["key"] == 60, pattern
            all_notes = (await invoke("get_pattern", {"track_index": lead, "include_disabled": True}))["pattern"]["notes"]
            assert [note["index"] for note in all_notes] == [0, 1], all_notes
            nested = (await invoke("get_clip", {"track_index": lead, "clip_index": 0, "full_detail": True}))["clip"]["notes"]
            compact_notes(nested)
            assert [note["key"] for note in nested] == [60, 64], nested
            await reject("update_clip", {"track_index": lead, "clip_index": 0, "name": "Must not rename", "length_ticks": 0})
            await reject("update_clip", {"track_index": lead, "clip_index": 0, "name": "Must not rename", "new_pos_ticks": 0, "new_pos_bars": 1})
            await reject("update_clip", {"track_index": lead, "clip_index": 0})
            clip = (await invoke("update_clip", {"track_index": lead, "clip_index": 0, "name": "MCP Phrase",
                                                   "new_pos_bars": 1, "length_bars": 2}))["clip"]
            assert clip["name"] == "MCP Phrase" and clip["start"] == 192 and clip["length"] == 384, clip
            await invoke("update_clip", {"track_index": lead, "old_pos_bars": 1, "new_pos_ticks": 384})
            await reject("update_mixer_channel", {"channel_num": mixer, "name": "Must not rename", "volume": 3})
            await reject("update_mixer_channel", {"channel_num": mixer})
            await invoke("update_mixer_channel", {"channel_num": mixer, "name": "MCP Bus Updated", "volume": 0.6, "muted": True, "solo": False})
            channel = next(channel for channel in (await invoke("get_mixer_channels"))["channels"] if channel["channel_num"] == mixer)
            assert channel["name"] == "MCP Bus Updated" and abs(channel["volume"] - 0.6) < 0.01 and channel["muted"] and not channel["solo"], channel
            await invoke("update_mixer_channel", {"channel_num": mixer, "muted": False, "solo": True})
            await invoke("update_mixer_channel", {"channel_num": mixer, "solo": False})
            target = {"target_type": "mixer", "target_index": mixer}
            await invoke("add_effect", {**target, "effect_name": "amplifier"})
            await reject("update_effect", {**target, "effect_index": 0, "enabled": False, "wet": 2})
            await reject("update_effect", {**target, "effect_index": 99, "enabled": False, "wet": 0.25})
            await reject("update_effect", {**target, "effect_index": 0})
            effect = (await invoke("update_effect", {**target, "effect_index": 0, "enabled": False, "wet": 0.25}))["effect"]
            assert not effect["enabled"] and abs(effect["wet"] - 0.25) < 0.01, effect
            presets = (await invoke("list_presets", {"instrument": "TripleOscillator", "search": "Square"}))["presets"]
            preset = next(preset for preset in presets if preset["name"] == "Square")
            loaded = (await invoke("load_instrument_file", {"track_index": lead, "path": preset["path"], "full_detail": True}))["track"]
            assert loaded["name"] == "MCP Lead Updated" and len(loaded["tcos"][0]["notes"]) == 2, loaded
            await invoke("load_instrument_file", {"track_index": lead, "path": preset["path"], "preserve_name": False})
            afp = (await invoke("create_track", {"type": "instrument", "instrument": "audiofileprocessor"}))["track_index"]
            await invoke("load_instrument_file", {"track_index": afp, "path": str(artifacts / "sample.wav")})
            volume_model = (await invoke("get_track", {"track_index": lead}))["track"]["volume_model_id"]
            auto = (await invoke("add_automation", {"track_index": automation, "model_id": volume_model, "length_bars": 1,
                                                     "points": [{"pos": 0, "value": 20}, {"pos": 191, "value": 35}]}))["clip_index"]
            auto_args = {"track_index": automation, "clip_index": auto}
            await reject("set_automation_points", {**auto_args, "mode": "upsert", "points": [{"pos": 48, "value": 25}, {"pos": 96, "value": 999}]})
            await invoke("set_automation_points", {**auto_args, "mode": "upsert", "points": [{"pos": 48, "value": 25}]})
            points = (await invoke("get_automation", auto_args))["automation"]["points"]
            assert [point["pos"] for point in points] == [0, 48, 191], points
            await invoke("set_automation_points", {**auto_args, "mode": "replace", "progression": "linear", "points": [{"pos": 0, "value": 15}]})
            assert len((await invoke("get_automation", auto_args))["automation"]["points"]) == 1
            assert isinstance((await invoke("scan_vst_directory", {"directory": str(artifacts)}))["candidates"], list)
            await reject("add_vst_track", {"plugin_path": str(artifacts / "missing-vst.dll")})
            await reject("transport", {"action": "seek"})
            await reject("transport", {"action": "play", "position": 96})
            await invoke("update_project", {"master_volume": 8})
            await save_clean()
            assert (await invoke("get_project_info"))["masterVolume"] == 8
            seek = await invoke("transport", {"action": "seek", "position": 96})
            assert seek["position"] == 96 and not seek["modified"], seek
            await invoke("transport", {"action": "play"})
            await asyncio.sleep(0.1)
            paused = await invoke("transport", {"action": "pause"})
            assert paused["paused"] and not paused["modified"], paused
            resumed = await invoke("transport", {"action": "play"})
            assert resumed["playing"] and not resumed["paused"] and not resumed["modified"], resumed
            stopped = await invoke("transport", {"action": "stop"})
            assert not stopped["playing"] and not stopped["modified"], stopped
            path = str(artifacts / "mcp-saved.mmp")
            saved = await invoke("save_project", {"path": path})
            assert Path(path).exists() and not saved["modified"]
            await invoke("load_project", {"path": original})
            assert not (await invoke("get_project_info"))["modified"]
            assert (await invoke("get_pattern", {"track_index": 0}))["pattern"]["notes"]
            await invoke("unknown_tool", expect_ok=False)
    parameters.args.extend(["--include-tool", "get_project_xml", "--include-tool", "generate_scale"])
    async with stdio_client(parameters) as (reader, writer):
        async with ClientSession(reader, writer, read_timeout_seconds=timedelta(seconds=40)) as session:
            await session.initialize()
            optional = {tool.name for tool in (await session.list_tools()).tools}
            assert optional == names | {"get_project_xml", "generate_scale"}, optional
            assert (await session.call_tool("get_project_xml", {})).structuredContent["xml"]
            scale = await session.call_tool("generate_scale", {"root": "C", "scale_type": "major"})
            assert not scale.isError and scale.structuredContent["keys"] == [60, 62, 64, 65, 67, 69, 71, 72], scale
    (artifacts / "mcp-report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"PASS: MCP compact catalog ({size['legacy_tools']} -> {size['mcp_tools']} tools; "
          f"{size['legacy_schema_chars']} -> {size['mcp_schema_chars']} schema characters), consolidated edits, "
          "preflight guards, compact MIDI notes, unified loading, automation modes, transport and opt-in tools")
