import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import subprocess
import time


def windows_for_process(process_id):
    user = ctypes.WinDLL("user32", use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    user.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    user.IsWindowVisible.argtypes = [wintypes.HWND]
    found = []

    @callback_type
    def callback(handle, unused):
        owner = wintypes.DWORD()
        user.GetWindowThreadProcessId(handle, ctypes.byref(owner))
        if owner.value == process_id and user.IsWindowVisible(handle):
            title = ctypes.create_unicode_buffer(2048)
            class_name = ctypes.create_unicode_buffer(256)
            user.GetWindowTextW(handle, title, len(title))
            user.GetClassNameW(handle, class_name, len(class_name))
            found.append({"handle": int(handle), "title": title.value, "class": class_name.value})
        return True

    user.EnumWindows(callback, 0)
    return found


def verify_close_prompt(call, artifacts, process_id):
    before = call("song")
    assert before["modified"] and "*" in before["windowTitle"], before
    prefix = before["windowTitle"].split("*", 1)[0]
    subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                    str(Path(__file__).with_name("request-test-close.ps1")), "-ProcessId", str(int(process_id)),
                    "-ExpectedProject", prefix], check=True)
    deadline = time.monotonic() + 5
    prompt = None
    while time.monotonic() < deadline:
        prompt = next((window for window in windows_for_process(int(process_id)) if window["title"] == "Project not saved"), None)
        if prompt:
            break
        time.sleep(0.05)
    assert prompt is not None, "LMMS did not show its native unsaved-project close prompt"
    (artifacts / "close-prompt.json").write_text(json.dumps(prompt, indent=2), encoding="utf-8")
    user = ctypes.WinDLL("user32", use_last_error=True)
    user.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user.PostMessageW(prompt["handle"], 0x0100, 0x1B, 1)
    user.PostMessageW(prompt["handle"], 0x0101, 0x1B, -1073741823)
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if not any(window["title"] == "Project not saved" for window in windows_for_process(int(process_id))):
            break
        time.sleep(0.05)
    assert not any(window["title"] == "Project not saved" for window in windows_for_process(int(process_id))), "The close prompt did not cancel"
    assert call("song")["modified"], "Cancelling close must keep test edits intact"
    print("PASS: native LMMS close prompt appears; Cancel preserves the edits")
