"""Live regressions for the exploratory MCP findings and follow-up controls."""
import json
from pathlib import Path

from jsonschema import Draft7Validator
from lmms_mcp import BridgeGateway


async def run_suite(call, artifacts):
    original = call("song")["file"]
    assert not call("song")["modified"]
    call("create_project", {"empty": True, "bpm": 120})
    assert call("song")["masterVolume"] == 40
    checkpoint = str(artifacts / "review-fixes.mmp")
    checks, report = [], []

    def check(condition, name):
        assert condition, name
        checks.append(name)

    def clean():
        call("save_project", {"path": checkpoint, "overwrite": True})

    def unchanged(command, arguments, rejected=False):
        clean()
        xml = call("get_project_xml")["xml"]
        response = call(command, arguments, expect_ok=not rejected)
        check(not response["changed"] and not response["modified"], f"{command}: unchanged flags")
        check(call("get_project_xml")["xml"] == xml, f"{command}: preserved full XML")
        return response

    # Every channel is inspected after repeated no-op and real solo edits,
    # including refreshes, switching solos, parameter writes and restoration.
    buses = [call("add_mixer_channel", {"name": name})["channel_num"] for name in ("Lead", "Bass", "Return")]
    call("mute_mixer_channel", {"channel_num": buses[1], "muted": True})

    def states():
        return [(c["channel_num"], c["muted"], c["solo"]) for c in call("get_mixer_channels", {"include_parameters": False})["channels"]]

    baseline = states()
    for channel in range(len(baseline)):
        for repeat in range(3):
            unchanged("update_mixer_channel", {"channel_num": channel, "solo": False})
            check(states() == baseline, "unsoloed no-op preserves every bus")
    for selected in range(len(baseline)):
        call("update_mixer_channel", {"channel_num": selected, "solo": True})
        expected = [(i, False if i == 0 or i == selected else True, i == selected) for i in range(len(baseline))]
        check(states() == expected, f"solo channel {selected} applies once")
        for repeat in range(4):
            unchanged("update_mixer_channel", {"channel_num": selected, "solo": True})
            call("update_mixer_channel", {"channel_num": buses[0], "volume": 0.8 if repeat % 2 else 0.7})
            check(states() == expected, "refresh preserves all mute/solo states")
        call("update_mixer_channel", {"channel_num": selected, "solo": False})
        check(states() == baseline, "unsolo restores original mute states")
    call("solo_mixer_channel", {"channel_num": buses[0], "solo": True})
    call("solo_mixer_channel", {"channel_num": buses[2], "solo": True})
    check([i for i, muted, solo in states() if solo] == [buses[2]], "switching solo clears the previous bus")
    solo_model = next(p for p in call("get_parameters", {"target_type": "mixer", "target_index": buses[2]})["parameters"] if p["path"].endswith("/solo"))
    call("set_parameter", {"model_id": solo_model["model_id"], "value": False})
    check(states() == baseline, "parameter-based solo restores every bus")
    unchanged("set_parameter", {"model_id": solo_model["model_id"], "value": False})

    disposable = call("create_track", {"type": "instrument", "name": "Delete first"})["track"]
    lead = call("create_track", {"type": "instrument", "name": "Keep lead", "volume": 33, "panning": -9, "fx_channel": buses[0]})["track"]
    handle = lead["track_handle"]
    pattern = call("place_pattern", {"track_handle": handle, "name": "Named phrase", "length_bars": 2})["clip"]
    clip_handle = pattern["clip_handle"]
    call("remove_track", {"track_handle": disposable["track_handle"]})
    check(call("get_track", {"track_handle": handle})["track"]["index"] == 0, "track handle survives index shift")
    unchanged("get_track", {"track_handle": disposable["track_handle"]}, rejected=True)
    notes = [{"key": 60, "pos": 0, "length": 48, "volume": 75}, {"key": 64, "pos": 48, "length": 48, "volume": 65}]
    call("replace_notes", {"clip_handle": clip_handle, "notes": notes})
    unchanged("replace_notes", {"clip_handle": clip_handle, "notes": notes})
    unchanged("set_notes_batch", {"clip_handle": clip_handle, "edits": [{"note_index": 0, "pos": 96}, {"note_index": 999, "key": 67}]}, rejected=True)
    unchanged("set_notes_batch", {"clip_handle": clip_handle, "edits": [{"note_index": 0, "volume": 55}, {"note_index": 0, "panning": 10}]}, rejected=True)
    call("set_notes_batch", {"clip_handle": clip_handle, "edits": [{"note_index": 0, "pos": 96, "key": 67}, {"note_index": 1, "pos": 0, "volume": 80}]})
    changed_notes = call("get_notes", {"clip_handle": clip_handle})["notes"]
    check([(n["key"], n["pos"]) for n in changed_notes] == [(64, 0), (67, 96)], "batch indices use original ordering before sorting")
    copied = call("duplicate_clip", {"clip_handle": clip_handle, "new_pos_bars": 2})["clip"]
    check(copied["name"] == "Named phrase" and copied["length"] == 384 and len(copied["notes"]) == 2, "pattern duplication preserves metadata")
    renamed = call("duplicate_clip", {"clip_handle": clip_handle, "new_pos_bars": 4, "name": "New phrase"})["clip"]
    check(renamed["name"] == "New phrase", "duplicate accepts a new name")
    unchanged("place_patterns_batch", {"placements": [{"track_handle": handle, "pos_bars": 6}, {"track_index": 9999, "pos_bars": 8}]}, rejected=True)
    placed = call("place_patterns_batch", {"placements": [{"track_handle": handle, "pos_bars": 6, "source_pattern_handle": clip_handle}, {"track_handle": handle, "pos_bars": 8, "name": "Empty phrase"}]})["clips"]
    check(len(placed) == 2 and placed[0]["name"] == "Named phrase" and len(placed[0]["notes"]) == 2, "batch placement preserves source names and notes")
    check([c["start"] for c in placed] == [1152, 1536], "batch placement preserves each song position, including empty patterns")
    call("delete_clip", {"clip_handle": copied["clip_handle"]})
    check(call("get_clip", {"clip_handle": renamed["clip_handle"]})["clip"]["name"] == "New phrase", "clip handle survives deletion of an earlier clip")
    unchanged("get_clip", {"clip_handle": copied["clip_handle"]}, rejected=True)
    clone = call("clone_track", {"track_handle": handle, "name": "Cloned lead"})["track"]
    check([c["name"] for c in clone["tcos"]] == [c["name"] for c in call("get_track", {"track_handle": handle})["track"]["tcos"]], "track cloning preserves clip names")
    call("remove_track", {"track_handle": clone["track_handle"]})

    organic = call("list_presets", {"instrument": "Organic", "search": "pad_ethereal"})["presets"]
    check(len(organic) == 1 and organic[0]["compatible"], "legacy Organic preset advertised as compatible")
    call("set_instrument_settings", {"track_handle": handle, "base_note": 65, "pitch": 50})
    loaded = call("load_instrument_file", {"track_handle": handle, "path": organic[0]["path"]})
    track = loaded["track"]
    check(track["instrument_id"] == "organic" and track["name"] == "Keep lead", "legacy native upgrade loads Organic and preserves name")
    check(track["volume"] == 33 and track["panning"] == -9 and track["fx_channel"] == buses[0] and track["base_note"] == 65 and track["pitch"] == 50, "preset preserves intentional mix and pitch settings")
    check(not loaded["side_effects"], "preserved loading has no reported mix/name side effects")
    raw = call("load_instrument_file", {"track_handle": handle, "path": organic[0]["path"], "preserve_name": False, "preserve_mix": False, "preserve_instrument_settings": False})
    check(bool(raw["side_effects"]), "opt-out loading reports setting changes")
    parameters = call("get_parameters", {"target_type": "track", "track_handle": handle})["parameters"]
    check(all({"type", "unit", "path", "writable", "automation_capable"} <= p.keys() for p in parameters), "parameter metadata is complete")
    check(len({p["path"] for p in parameters}) == len(parameters), "parameter paths distinguish every model")
    song_parameters = call("get_parameters", {"target_type": "song"})["parameters"]
    check({p["path"]: p["unit"] for p in song_parameters if p["path"].startswith("song/")} ==
          {"song/tempo": "bpm", "song/master_volume": "percent", "song/master_pitch": "semitones"}, "song parameters expose actual units and paths")
    ladspa = next(s for s in call("list_effect_subplugins", {"effect_name": "ladspaeffect"})["subplugins"] if "amplifier" in s["name"].lower())
    effect_target = {"target_type": "track", "track_handle": handle}
    call("add_effect", {**effect_target, "effect_name": "ladspaeffect", "subplugin_index": ladspa["index"]})
    controls = call("get_effect_chain", effect_target)["effects"][0]["parameters"]
    ports = [p for p in controls if "port_index" in p]
    check(bool(ports) and len({p["name"] for p in ports}) == len(ports) and len({p["path"] for p in ports}) == len(ports), "LADSPA processors and roles distinguish control names and paths")
    check(any(p["writable"] for p in ports) and any(not p["writable"] for p in ports), "LADSPA metadata distinguishes active and inactive models")
    inactive = next(p for p in ports if not p["writable"])
    unchanged("set_parameter", {"model_id": inactive["model_id"], "value": inactive["value"]}, rejected=True)
    active = next(p for p in ports if p["writable"] and p["type"] == "number")
    call("set_parameter", {"model_id": active["model_id"], "value": (active["minimum"] + active["maximum"]) / 2})
    call("remove_effect", {**effect_target, "effect_index": 0})

    sample_track = call("create_track", {"type": "sample", "name": "Samples"})["track"]["track_handle"]
    discovered = call("list_samples", {"search": "cymbal", "limit": 2})
    check(bool(discovered["samples"]) and all(Path(s["path"]).is_absolute() and Path(s["path"]).is_file() for s in discovered["samples"]), "discovery returns usable absolute sample paths")
    page2 = call("list_samples", {"search": "cymbal", "limit": 2, "offset": 2})
    check(not ({s["path"] for s in discovered["samples"]} & {s["path"] for s in page2["samples"]}), "sample pages are stable and distinct")
    sample = call("place_sample_clip", {"track_handle": sample_track, "file_path": discovered["samples"][0]["path"], "name": "Named cymbal", "length_ticks": 240})["clip"]
    check(not sample["name_persistent"] and bool(sample["warnings"]), "sample names expose native persistence limitation")
    sample_copy = call("duplicate_clip", {"clip_handle": sample["clip_handle"], "new_pos_bars": 2})["clip"]
    check(sample_copy["name"] == "Named cymbal" and sample_copy["length"] == 240, "sample duplication preserves session name and length")
    assigned = call("assign_sample_file", {"clip_handle": sample["clip_handle"], "file_path": str(artifacts / "sample.wav")})["clip"]
    check(assigned["name"] == "Named cymbal" and assigned["length"] == 240, "sample assignment preserves name and length")
    unchanged("assign_sample_file", {"clip_handle": sample["clip_handle"], "file_path": str(artifacts / "sample.wav")})
    afp = call("create_track", {"type": "instrument", "instrument": "audiofileprocessor", "name": "Keep AFP"})["track"]["track_handle"]
    assigned = call("assign_sample_file", {"track_handle": afp, "file_path": str(artifacts / "sample.wav")})
    check(assigned["track"]["name"] == "Keep AFP" and Path(assigned["track"]["resolved_file_path"]).is_file(), "AFP assignment preserves name and resolves path")
    unchanged("assign_sample_file", {"track_handle": afp, "file_path": str(artifacts / "sample.wav")})

    gateway = BridgeGateway()
    catalog = {t.name: t for t in await gateway.catalog()}
    check(len(catalog) == 60, "60 default MCP tools")
    for tool in catalog.values():
        Draft7Validator.check_schema(tool.inputSchema)
    async def mcp(name, arguments=None, rejected=False):
        result = await gateway.call(name, arguments)
        report.append({"tool": name, "args": arguments, "result": result.structuredContent})
        check(result.isError == rejected, f"MCP {name}: expected error flag")
        return result.structuredContent
    compact = await mcp("update_track", {"track_handle": handle, "volume": 25})
    check("tcos" not in compact["track"], "simple track edits omit clips")
    detailed = await mcp("update_track", {"track_handle": handle, "volume": 25, "full_detail": True})
    check(bool(detailed["track"]["tcos"][0]["notes"]), "full_detail restores complete track notes")
    channel = await mcp("update_mixer_channel", {"channel_num": buses[0], "volume": 0.5})
    check("channel" in channel and "channels" not in channel and all("parameters" not in e for e in channel["channel"]["effects"]), "mixer edits return only selected channel")
    inserted = await mcp("add_notes_batch", {"clip_handle": clip_handle, "notes": [{"key": 72, "pos": 144}]})
    check("pattern" not in inserted and inserted["notes"][0]["key"] == 72, "insertion returns compact MIDI notes")
    inspect = await mcp("get_pattern", {"clip_handle": clip_handle, "include_notes": False})
    check("notes" not in inspect["pattern"] and inspect["pattern"]["note_count"] == 3, "pattern metadata without notes is supported")
    clean()
    token = call("song")["project_token"]
    for name, args in [("update_track", {"track_handle": handle, "volume": 25}), ("update_project", {"master_volume": 40}),
                       ("set_notes_batch", {"clip_handle": clip_handle, "edits": [{"note_index": 0, "pos": 0}]})]:
        unchanged(name, args)
    clean()
    call("load_project", {"path": checkpoint})
    unchanged("update_track", {"track_handle": handle, "volume": 40}, rejected=True)
    unchanged("set_parameters", {"project_token": token, "values": [{"model_id": parameters[0]["model_id"], "value": parameters[0]["value"]}]}, rejected=True)
    check(call("song")["project_token"] != token, "reload expires project handles")
    samples = call("get_track", {"track_index": 1})["track"]["tcos"]
    check(all(not s["name_persistent"] for s in samples), "save/reload continues to expose sample-name limitation")

    # Use the known installed real VST from the exploratory report when present.
    vst_path = Path("C:/Program Files/VstPlugins/SyndtSphere.dll")
    if vst_path.is_file():
        vst = call("add_vst_track", {"plugin_path": str(vst_path), "name": "VST regression"})
        check(vst["vst"]["plugin_loaded"] and vst["vst"]["parameters_supported"], "real VST load status and parameter initialization")
        vst_handle = vst["track"]["track_handle"]
        params = call("get_parameters", {"target_type": "instrument", "track_handle": vst_handle})
        check(bool(params["parameters"]) and all(p["unit"] == "normalized" for p in params["parameters"]), "real external VST parameters exposed")
        parameter = params["parameters"][0]
        call("set_parameter", {"model_id": parameter["model_id"], "value": 0.55})
        edited = call("get_parameters", {"target_type": "instrument", "track_handle": vst_handle})["parameters"][0]
        check(abs(edited["value"] - 0.55) < 0.001, "VST parameter edit reflects native model")
        check(edited["display_value"] == "0.55", "readable parameter display retains exact native edit value separately")
        call("add_notes_batch", {"track_handle": vst_handle, "notes": [{"key": 60, "pos": 0, "length": 180, "volume": 70}]})
        for track in call("list_tracks")["tracks"]:
            if track["track_handle"] != vst_handle:
                call("update_track", {"track_handle": track["track_handle"], "muted": True})
        rendered = await mcp("render_project", {"path": str(artifacts / "review-vst.wav"), "tail_seconds": 0, "wait_seconds": 30})
        check(rendered["job"]["state"] == "succeeded" and rendered["job"]["peak"] > 0, "isolated VST render succeeds with nonzero audio")
        await mcp("render_status", {"job_id": rendered["job"]["job_id"] + 1}, rejected=True)
    clean()
    call("load_project", {"path": original})
    check(call("song")["masterVolume"] == 8 and not call("song")["modified"], "suite restores its original clean project at safe gain")
    (artifacts / "review-fixes-report.json").write_text(json.dumps({"checks": checks, "mcp": report}, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"PASS: {len(checks)} review-fix live checks; repeated mixer state, legacy presets, handles, atomic batches, preservation, sample discovery, compact MCP and VST parameters")
