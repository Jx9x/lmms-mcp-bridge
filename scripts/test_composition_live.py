import json
import math
from pathlib import Path
import struct
import time
import wave
import xml.etree.ElementTree as ET


def run_suite(call, artifacts, process_id):
    checkpoint = str(artifacts / "composition-checkpoint.mmp")

    def save_clean(path=checkpoint):
        saved = call("save_project", {"path": path, "overwrite": True})
        assert not saved["modified"] and "*" not in saved["windowTitle"], saved
        return saved

    def edit(command, arguments=None):
        save_clean()
        response = call(command, arguments or {})
        assert response["modified"] == response["changed"], response
        assert ("*" in response["windowTitle"]) == response["changed"], response
        assert call("selftest")["failed"] == 0
        return response

    def reject(command, arguments):
        save_clean()
        before = call("get_project_xml")["xml"]
        response = call(command, arguments, expect_ok=False)
        assert not response["modified"], response
        assert call("get_project_xml")["xml"] == before, response
        return response

    save_clean(str(artifacts / "dirty-state-saved.mmp"))
    help_response = call("help")
    tools = {tool["name"]: tool for tool in help_response["tools"]}
    assert len(tools) >= 100, len(tools)
    assert set(help_response["commands"]) == set(tools)
    info = call("get_lmms_info")
    assert all(info["audio_export_formats"].values()) and info["midi_export_available"], info
    assert info["playback_volume_cap"] == 40, info
    (artifacts / "native-tools.json").write_text(json.dumps(help_response["tools"], indent=2), encoding="utf-8")
    assert call("note_name_to_key", {"name": "C4"})["key"] == 60
    assert call("note_name_to_key", {"name": "Bb2"})["key"] == 46
    assert call("key_to_note_name", {"key": 69})["name"] == "A4"
    assert call("bars_to_ticks_converter", {"bars": 1})["ticks"] == 192
    assert call("generate_scale", {"root": "C", "scale_type": "major"})["keys"] == [60, 62, 64, 65, 67, 69, 71, 72]
    edit("create_project", {"empty": True, "bpm": 120})
    assert call("song")["trackCount"] == 0
    assert call("song")["masterVolume"] == 40
    reject("export_midi", {"path": str(artifacts / "empty.mid")})
    assert not (artifacts / "empty.mid").exists()
    reject("create_project", {"bpm": 120.5})
    reject("set_time_signature", {"numerator": 4, "denominator": 3})
    reject("set_tempo", {"bpm": "120"})
    reject("add_instrument_track", {"instrument": "not-a-real-plugin"})
    lead = edit("add_instrument_track", {"name": "Bridge Lead", "instrument": "tripleoscillator", "volume": 40})["track_index"]
    bass = edit("add_instrument_track", {"name": "Bridge Bass", "instrument": "tripleoscillator", "volume": 30})["track_index"]
    assert lead == 0 and bass == 1
    edit("set_time_signature", {"numerator": 3, "denominator": 4})
    assert call("bars_to_ticks_converter", {"bars": 1})["ticks"] == 144
    edit("set_time_signature", {"numerator": 4, "denominator": 4})
    edit("set_master_volume", {"volume": 70})
    edit("set_master_pitch", {"pitch": 1})
    edit("set_master_pitch", {"pitch": 0})
    edit("set_tempo", {"bpm": 125})
    edit("rename_track", {"track_index": lead, "name": "Lead"})
    edit("set_track_volume", {"track_index": lead, "volume": 35})
    edit("set_track_panning", {"track_index": lead, "panning": -10})
    edit("mute_track", {"track_index": bass, "muted": True})
    edit("mute_track", {"track_index": bass, "muted": False})
    edit("solo_track", {"track_index": lead, "solo": True})
    edit("solo_track", {"track_index": lead, "solo": False})
    edit("place_pattern", {"track_index": lead, "name": "Melody", "pos_bars": 0, "length_bars": 1})
    notes = [{"key": key, "pos": position, "length": 48, "volume": 85} for key, position in zip((60, 64, 67, 72), (0, 48, 96, 144))]
    inserted = edit("add_notes_batch", {"track_index": lead, "notes": notes})
    assert "pattern" in inserted, "Native insertion must preserve its legacy response by default"
    observed = call("get_notes", {"track_index": lead})["notes"]
    assert [note["key"] for note in observed] == [60, 64, 67, 72], observed
    assert [note["lmms_key"] for note in observed] == [48, 52, 55, 60], observed
    reject("add_notes_batch", {"track_index": lead, "notes": [{"key": 60}, {"key": "bad"}]})
    reject("add_note", {"track_index": lead, "key": 60, "note_name": "D4"})
    reject("add_note", {"track_index": lead, "length": 0})
    reject("add_note", {"track_index": lead, "pos": -1})
    reject("set_note", {"track_index": lead, "note_index": 99, "key": 60})
    edit("set_note", {"track_index": lead, "note_index": 1, "note_name": "D4", "panning": 15})
    assert call("get_notes", {"track_index": lead})["notes"][1]["key"] == 62
    edit("transpose_pattern", {"track_index": lead, "semitones": 1})
    edit("transpose_pattern", {"track_index": lead, "semitones": -1})
    edit("add_note_by_name", {"track_index": lead, "note_name": "C5", "pos": 192, "length": 48})
    assert call("get_pattern", {"track_index": lead})["pattern"]["length"] >= 240
    edit("remove_note", {"track_index": lead, "note_index": 4})
    edit("resize_clip", {"track_index": lead, "clip_index": 0, "length_bars": 1})
    duplicate = edit("duplicate_clip", {"track_index": lead, "clip_index": 0, "new_pos_bars": 1})["clip_index"]
    edit("move_clip", {"track_index": lead, "clip_index": duplicate, "new_pos_bars": 2})
    edit("rename_clip", {"track_index": lead, "clip_index": duplicate, "name": "Melody reprise"})
    assert call("get_clip", {"track_index": lead, "clip_index": duplicate})["clip"]["start"] == 384
    edit("delete_clip", {"track_index": lead, "clip_index": duplicate})
    edit("add_note", {"track_index": bass, "note_name": "C2", "pos": 0, "length": 192, "volume": 70})
    inserted = edit("add_notes_batch", {"track_index": bass, "notes": [{"key": "C2", "pos": 48, "length": 48}], "include_pattern": False})
    assert "pattern" not in inserted and len(inserted["notes"]) == 1, inserted
    edit("remove_note", {"track_index": bass, "note_index": 1})
    temporary = edit("clone_track", {"track_index": bass, "name": "Temporary"})["track_index"]
    edit("clear_notes", {"track_index": temporary})
    edit("remove_track", {"track_index": temporary})
    presets = call("list_presets", {"instrument": "TripleOscillator"})["presets"]
    assert presets, "Installed TripleOscillator presets were not discovered"
    preset = next(preset for preset in presets if preset["name"] == "Square")
    edit("load_preset", {"track_index": lead, "path": preset["path"]})
    assert len(call("get_notes", {"track_index": lead})["notes"]) == 4
    assert call("get_track", {"track_index": lead})["track"]["name"] == "Lead"
    edit("set_instrument_settings", {"track_index": lead, "base_note": 69, "pitch_range": 2, "pitch": 0})
    parameters = call("get_parameters", {"target_type": "instrument", "target_index": lead})["parameters"]
    assert parameters, "Native instrument parameters were not discovered"
    oscillator = next(parameter for parameter in parameters if parameter["maximum"] > parameter["minimum"])
    edit("set_parameter", {"model_id": oscillator["model_id"], "value": oscillator["value"]})
    reject("set_parameters", {"values": [{"model_id": oscillator["model_id"], "value": oscillator["value"]}, {"model_id": 4294967295, "value": 1}]})
    mixer = edit("add_mixer_channel", {"name": "Lead bus", "volume": 0.8})["channel_num"]
    auxiliary = edit("add_mixer_channel", {"name": "Aux", "volume": 0.5})["channel_num"]
    edit("assign_track_to_mixer", {"track_index": lead, "channel_num": mixer})
    edit("set_mixer_channel_volume", {"channel_num": mixer, "volume": 0.7})
    edit("set_mixer_channel_name", {"channel_num": mixer, "name": "Lead Bus"})
    edit("mute_mixer_channel", {"channel_num": auxiliary, "muted": True})
    edit("mute_mixer_channel", {"channel_num": auxiliary, "muted": False})
    edit("solo_mixer_channel", {"channel_num": mixer, "solo": True})
    edit("solo_mixer_channel", {"channel_num": mixer, "solo": False})
    edit("set_mixer_send", {"from_channel": mixer, "to_channel": auxiliary, "amount": 0.2})
    reject("set_mixer_send", {"from_channel": auxiliary, "to_channel": mixer})
    edit("remove_mixer_send", {"from_channel": mixer, "to_channel": auxiliary})
    edit("remove_mixer_channel", {"channel_num": auxiliary})
    reject("remove_mixer_channel", {"channel_num": 0})
    registry = call("list_effect_plugins")["effects"]
    effect_name = next(plugin["name"] for plugin in registry if plugin["name"] == "amplifier")
    target = {"target_type": "mixer", "target_index": mixer}
    effect = edit("add_effect", {**target, "effect_name": effect_name, "wet": 0.5})
    assert len(effect["effects"]) == 1, effect
    edit("toggle_effect", {**target, "effect_index": 0, "enabled": False})
    assert not call("get_effect_chain", target)["effects"][0]["enabled"]
    edit("toggle_effect", {**target, "effect_index": 0, "enabled": True})
    edit("set_effect_wet", {**target, "effect_index": 0, "wet": 0.7})
    assert abs(call("get_effect_chain", target)["effects"][0]["wet"] - 0.7) < 0.01
    effect_parameters = call("get_parameters", {"target_type": "effect", "effect_target_type": "mixer", "target_index": mixer, "effect_index": 0})["parameters"]
    assert effect_parameters, "Effect parameters were not discovered"
    edit("set_parameter", {"model_id": effect_parameters[0]["model_id"], "value": effect_parameters[0]["value"]})
    edit("add_effect", {**target, "effect_name": effect_name, "wet": 0.3})
    chain = call("get_effect_chain", target)["effects"]
    assert len(chain) == 2 and abs(chain[0]["wet"] - 0.7) < 0.01 and abs(chain[1]["wet"] - 0.3) < 0.01, chain
    edit("remove_effect", {**target, "effect_index": 1})
    edit("set_effect_chain_enabled", {**target, "enabled": False})
    edit("set_effect_chain_enabled", {**target, "enabled": True})
    automation = edit("add_automation_track", {"name": "Lead gain"})["track_index"]
    volume_model = call("get_track", {"track_index": lead})["track"]["volume_model_id"]
    auto = edit("add_automation", {"track_index": automation, "model_id": volume_model, "length_bars": 1, "progression": "linear", "points": [{"pos": 0, "value": 20}, {"pos": 191, "value": 35}]})["clip_index"]
    auto_args = {"track_index": automation, "clip_index": auto}
    assert len(call("get_automation", auto_args)["automation"]["points"]) == 2
    reject("set_automation_point", {**auto_args, "pos": 24, "value": 999})
    edit("set_automation_point", {**auto_args, "pos": 48, "value": 25})
    edit("remove_automation_point", {**auto_args, "pos": 48})
    edit("set_automation_points", {**auto_args, "progression": "cubic", "tension": 0.5, "points": [{"pos": 0, "value": 20}, {"pos": 191, "value": 35}]})
    sample_path = artifacts / "sample.wav"
    with wave.open(str(sample_path), "wb") as output:
        output.setparams((2, 2, 44100, 0, "NONE", "not compressed"))
        output.writeframes(b"".join(struct.pack("<hh", int(3000 * math.sin(index * 2 * math.pi * 220 / 44100)), int(3000 * math.sin(index * 2 * math.pi * 220 / 44100))) for index in range(4410)))
    sample_track = edit("add_sample_track", {"name": "Sample"})["track_index"]
    sample_clip = edit("place_sample_clip", {"track_index": sample_track, "file_path": str(sample_path), "pos_bars": 0})["clip_index"]
    assert call("get_clip", {"track_index": sample_track, "clip_index": sample_clip})["clip"]["frames"] > 0
    edit("set_track_volume", {"track_index": sample_track, "volume": 30})
    edit("set_track_panning", {"track_index": sample_track, "panning": 20})
    edit("assign_sample_file", {"track_index": sample_track, "clip_index": sample_clip, "file_path": str(sample_path)})
    bb = edit("add_pattern_track", {"name": "Drums"})["track_index"]
    drum = edit("add_instrument_track", {"container": "bb", "name": "Kick", "instrument": "kicker", "volume": 30})["track_index"]
    edit("add_notes_batch", {"container": "bb", "track_index": drum, "pattern_index": 0, "notes": [{"key": "C4", "pos": position, "length": -1, "volume": 80} for position in (0, 48, 96, 144)]})
    edit("place_bb_clip", {"track_index": bb, "pos_bars": 0, "length_bars": 1})
    assert call("get_notes", {"track_index": bb, "bb_instrument_index": drum})["notes"]
    call("set_master_volume", {"volume": 8})
    save_clean()
    assert call("song")["masterVolume"] == 8
    assert not call("play")["modified"]
    time.sleep(0.15)
    assert call("song")["playing"]
    assert not call("pause")["modified"]
    paused = call("song")
    assert paused["paused"]
    call("play")
    resumed = call("song")
    assert resumed["playing"] and not resumed["paused"] and resumed["positionTicks"] >= paused["positionTicks"]
    call("stop")
    call("set_playhead", {"position": 96})
    position = call("song")
    assert position["positionTicks"] == 96 and position["positionMs"] == int(96 * 60000 / (48 * position["tempo"]))
    call("set_playhead", {"position": 0})
    call("stop")
    project = artifacts / "composition-final.mmp"
    save_clean(str(project))
    xml = ET.parse(project).getroot()
    assert len(xml.findall("./song/trackcontainer/track")) >= 5
    assert len(xml.findall(".//note")) >= 9
    assert xml.findall(".//automationpattern/object"), "Native automation links did not persist"
    assert xml.findall(".//effect"), "Native effects did not persist"
    before_reload = call("list_tracks")["tracks"]
    call("load_project", {"path": str(project)})
    assert not call("song")["modified"]
    after_reload = call("list_tracks")["tracks"]
    assert [track["name"] for track in before_reload] == [track["name"] for track in after_reload]
    assert len(call("get_notes", {"track_index": lead})["notes"]) == 4
    assert call("get_automation", {"track_index": automation})["automation"]["targets"]
    assert len(call("get_effect_chain", target)["effects"]) == 1
    assert call("selftest")["failed"] == 0
    print("PASS: native tracks, MIDI pitch mapping, patterns, notes, clips, presets, parameters, mixer, effects, samples, BB, automation, save and reload")
