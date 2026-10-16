import array
import hashlib
import json
from pathlib import Path
import struct
import time
import wave


def midi_notes(data):
    assert data[:4] == b"MThd" and len(data) >= 14
    header_length, format_id, tracks, resolution = struct.unpack(">IHHH", data[4:14])
    assert format_id in (0, 1) and tracks > 0 and resolution > 0
    cursor = 8 + header_length
    notes = []
    for unused in range(tracks):
        assert data[cursor:cursor + 4] == b"MTrk"
        length = struct.unpack(">I", data[cursor + 4:cursor + 8])[0]
        body = data[cursor + 8:cursor + 8 + length]
        assert len(body) == length
        cursor += 8 + length
        offset, running = 0, None
        while offset < len(body):
            while body[offset] & 0x80:
                offset += 1
            offset += 1
            status = body[offset]
            if status & 0x80:
                offset += 1
                if status < 0xF0:
                    running = status
            else:
                assert running is not None
                status = running
            if status == 0xFF:
                offset += 1
                size = 0
                while True:
                    part = body[offset]
                    offset += 1
                    size = (size << 7) | (part & 0x7F)
                    if not part & 0x80:
                        break
                offset += size
            elif status in (0xF0, 0xF7):
                size = 0
                while True:
                    part = body[offset]
                    offset += 1
                    size = (size << 7) | (part & 0x7F)
                    if not part & 0x80:
                        break
                offset += size
            else:
                size = 1 if status & 0xF0 in (0xC0, 0xD0) else 2
                if status & 0xF0 == 0x90 and body[offset + 1]:
                    notes.append(body[offset])
                offset += size
        assert offset == len(body)
    assert cursor == len(data)
    return notes


def verify_mp3(data):
    offset = 0
    if data.startswith(b"ID3"):
        offset = 10 + sum((value & 0x7F) << shift for value, shift in zip(data[6:10], (21, 14, 7, 0)))
    frames = 0
    while offset + 4 <= len(data):
        header = int.from_bytes(data[offset:offset + 4], "big")
        if header >> 21 != 0x7FF:
            break
        assert (header >> 19) & 3 == 3 and (header >> 17) & 3 == 1
        bitrate = (0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0)[(header >> 12) & 15]
        frequency = (44100, 48000, 32000, 0)[(header >> 10) & 3]
        assert bitrate and frequency == 44100
        offset += 144000 * bitrate // frequency + ((header >> 9) & 1)
        frames += 1
    assert frames >= 2 and offset <= len(data), (frames, offset, len(data))


def run_suite(call, artifacts):
    project = call("song")["file"]
    muted = call("clone_track", {"track_index": 1, "name": "Muted tail regression"})["track_index"]
    call("move_clip", {"track_index": muted, "clip_index": 0, "new_pos_bars": 3})
    call("mute_track", {"track_index": muted, "muted": True})
    call("save_project", {"path": project, "overwrite": True})
    before = call("song")
    assert not before["modified"] and not before["playing"] and before["lengthBars"] == 4
    volumes = [track.get("volume") for track in call("list_tracks")["tracks"]]
    sentinel = artifacts / "cancel-preserved.wav"
    sentinel.write_bytes(b"existing output must survive cancellation")
    original_hash = hashlib.sha256(sentinel.read_bytes()).hexdigest()
    call("render_project", {"path": str(sentinel)}, expect_ok=False)
    call("render_project", {"path": str(artifacts / "invalid-depth.wav"), "bit_depth": 17}, expect_ok=False)
    call("render_project", {"path": str(sentinel), "overwrite": True, "tail_seconds": 30})
    call("tempo", {"bpm": 140}, expect_ok=False)
    call("project.new", expect_ok=False)
    assert call("render_status")["job"]["state"] == "running"
    assert call("cancel_render")["job"]["state"] == "cancelled"
    assert hashlib.sha256(sentinel.read_bytes()).hexdigest() == original_hash
    assert call("song")["tempo"] == before["tempo"]
    assert not call("song")["modified"]
    assert call("song")["lengthBars"] == before["lengthBars"], "Rendering must restore arrangement length including muted tracks"
    results = []
    for format_name in ("wav", "ogg", "flac", "mp3"):
        output = artifacts / f"render.{format_name}"
        started = call("render_project", {"path": str(output), "format": format_name, "tail_seconds": 0})
        deadline = time.monotonic() + 120
        job = started["job"]
        while job["state"] == "running" and time.monotonic() < deadline:
            time.sleep(0.05)
            job = call("render_status")["job"]
        assert job["state"] == "succeeded", job
        assert output.stat().st_size == job["bytes"] and job["bytes"] > 100
        assert job["frames"] > 20000 and job["peak"] > 0.001 and job["rms"] > 0.001, job
        data = output.read_bytes()
        if format_name == "wav":
            with wave.open(str(output), "rb") as audio:
                assert audio.getnchannels() == 2 and audio.getsampwidth() == 2 and audio.getframerate() == 44100
                assert audio.getnframes() == job["frames"]
                samples = array.array("h", audio.readframes(audio.getnframes()))
                assert max(abs(sample) for sample in samples) > 100
        elif format_name == "ogg":
            assert data[:4] == b"OggS" and b"vorbis" in data[:512]
        elif format_name == "flac":
            assert data[:4] == b"fLaC"
        else:
            verify_mp3(data)
        state = call("song")
        assert not state["modified"] and not state["playing"]
        assert state["tempo"] == before["tempo"]
        assert state["lengthBars"] == before["lengthBars"]
        assert [track.get("volume") for track in call("list_tracks")["tracks"]] == volumes
        assert call("selftest")["failed"] == 0
        results.append(job)
    midi_path = artifacts / "composition.mid"
    exported = call("export_midi", {"path": str(midi_path)})
    assert exported["bytes"] == midi_path.stat().st_size
    notes = midi_notes(midi_path.read_bytes())
    assert 60 in notes and len(notes) >= 9, notes
    midi_hash = hashlib.sha256(midi_path.read_bytes()).hexdigest()
    call("export_midi", {"path": str(midi_path)}, expect_ok=False)
    assert hashlib.sha256(midi_path.read_bytes()).hexdigest() == midi_hash
    assert not call("song")["modified"]
    assert call("selftest")["failed"] == 0
    call("set_playhead", {"position": 96})
    original_volume = call("get_track", {"track_index": 0})["track"]["volume"]
    call("set_track_volume", {"track_index": 0, "volume": original_volume + 5})
    unsaved_xml = call("get_project_xml")["xml"]
    started = call("render_project", {"path": str(artifacts / "unsaved-live.wav"), "tail_seconds": 0})
    deadline = time.monotonic() + 120
    job = started["job"]
    while job["state"] == "running" and time.monotonic() < deadline:
        time.sleep(0.05)
        job = call("render_status")["job"]
    assert job["state"] == "succeeded", job
    assert call("get_project_xml")["xml"] == unsaved_xml, "Rendering changed native project data"
    state = call("song")
    assert state["modified"] and "*" in state["windowTitle"]
    assert state["positionTicks"] == 96 and state["lengthBars"] == 4
    call("set_track_volume", {"track_index": 0, "volume": original_volume})
    call("save_project", {"path": project, "overwrite": True})
    call("set_playhead", {"position": 0})
    call("play")
    time.sleep(0.1)
    assert call("song")["playing"]
    call("stop")
    (artifacts / "export-report.json").write_text(json.dumps({"audio": results, "midi_notes": notes}, indent=2), encoding="utf-8")
    print("PASS: live-song WAV, OGG, FLAC, MP3 and MIDI exports; audible WAV data; cancellation/overwrite guards; project state and playback restored")
