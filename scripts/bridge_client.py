import argparse
import ctypes
from ctypes import wintypes
import json
import math
import os
from pathlib import Path
import sys
import time


MAX_REQUEST_BYTES = 64 * 1024
MAX_RESPONSE_BYTES = 64 * 1024 * 1024


class BridgeError(RuntimeError):
    pass


def encode_request(command, arguments=None):
    if not isinstance(command, str) or not command.strip():
        raise ValueError("command must be a nonempty string")
    if arguments is not None:
        if not isinstance(arguments, dict):
            raise ValueError("arguments must be an object")
        line = json.dumps({"cmd": command, "args": arguments}, ensure_ascii=False, allow_nan=False, separators=(",", ":"))
    else:
        line = command
    if "\n" in line or "\r" in line:
        raise ValueError("one command per line is required")
    data = line.encode("utf-8")
    if len(data) > MAX_REQUEST_BYTES:
        raise ValueError("request exceeds the 64 KiB pipe limit; split large note batches")
    return data + b"\n"


class BridgeClient:
    def __init__(self, pipe=None, timeout=35.0):
        if os.name != "nt":
            raise BridgeError("the live LMMS bridge requires Windows")
        if not math.isfinite(timeout) or timeout <= 0:
            raise ValueError("timeout must be positive and finite")
        pipe = pipe or os.environ.get("LMMS_AGENT_BRIDGE_PIPE", "lmms-agent-bridge")
        if pipe.startswith("\\\\.\\pipe\\"):
            pipe = pipe[9:]
        if not pipe or "\\" in pipe or "/" in pipe:
            raise ValueError("pipe must be a local named-pipe name")
        self.pipe = "\\\\.\\pipe\\" + pipe
        self.timeout = timeout
        self.handle = None
        self._pending = bytearray()
        self._kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        pointer = ctypes.POINTER

        class Overlapped(ctypes.Structure):
            _fields_ = [("Internal", ctypes.c_size_t), ("InternalHigh", ctypes.c_size_t),
                        ("Offset", wintypes.DWORD), ("OffsetHigh", wintypes.DWORD), ("hEvent", wintypes.HANDLE)]

        self._overlapped = Overlapped
        signatures = {
            "CreateFileW": ([wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE], wintypes.HANDLE),
            "WaitNamedPipeW": ([wintypes.LPCWSTR, wintypes.DWORD], wintypes.BOOL),
            "CreateEventW": ([ctypes.c_void_p, wintypes.BOOL, wintypes.BOOL, wintypes.LPCWSTR], wintypes.HANDLE),
            "ReadFile": ([wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, pointer(wintypes.DWORD), pointer(Overlapped)], wintypes.BOOL),
            "WriteFile": ([wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, pointer(wintypes.DWORD), pointer(Overlapped)], wintypes.BOOL),
            "WaitForSingleObject": ([wintypes.HANDLE, wintypes.DWORD], wintypes.DWORD),
            "GetOverlappedResult": ([wintypes.HANDLE, pointer(Overlapped), pointer(wintypes.DWORD), wintypes.BOOL], wintypes.BOOL),
            "CancelIoEx": ([wintypes.HANDLE, pointer(Overlapped)], wintypes.BOOL),
            "CloseHandle": ([wintypes.HANDLE], wintypes.BOOL),
        }
        for name, (args, result) in signatures.items():
            function = getattr(self._kernel, name)
            function.argtypes = args
            function.restype = result

    def connect(self):
        if self.handle is not None:
            return self
        deadline = time.monotonic() + self.timeout
        while True:
            handle = self._kernel.CreateFileW(self.pipe, 0xC0000000, 0, None, 3, 0x40000000, None)
            if handle != ctypes.c_void_p(-1).value:
                self.handle = handle
                return self
            error = ctypes.get_last_error()
            remaining = deadline - time.monotonic()
            if error not in (2, 231) or remaining <= 0:
                raise BridgeError(f"cannot connect to {self.pipe} (Windows error {error}); start LMMS with scripts/start-lmms-with-bridge.ps1")
            if error == 231:
                self._kernel.WaitNamedPipeW(self.pipe, max(1, int(min(remaining, 0.1) * 1000)))
            else:
                time.sleep(min(remaining, 0.05))

    def close(self):
        if self.handle is not None:
            self._kernel.CloseHandle(self.handle)
            self.handle = None
        self._pending.clear()

    def __enter__(self):
        return self.connect()

    def __exit__(self, *unused):
        self.close()

    def _io(self, function, buffer, size, deadline):
        event = self._kernel.CreateEventW(None, True, False, None)
        if not event:
            raise BridgeError(f"cannot create pipe I/O event (Windows error {ctypes.get_last_error()})")
        overlapped = self._overlapped()
        overlapped.hEvent = event
        transferred = wintypes.DWORD()
        try:
            if not function(self.handle, buffer, size, ctypes.byref(transferred), ctypes.byref(overlapped)):
                error = ctypes.get_last_error()
                if error != 997:
                    raise BridgeError(f"pipe I/O failed (Windows error {error})")
                milliseconds = max(0, int((deadline - time.monotonic()) * 1000))
                wait = self._kernel.WaitForSingleObject(event, milliseconds)
                if wait != 0:
                    self._kernel.CancelIoEx(self.handle, ctypes.byref(overlapped))
                    self._kernel.GetOverlappedResult(self.handle, ctypes.byref(overlapped), ctypes.byref(transferred), True)
                    raise BridgeError("pipe request timed out; its outcome may be indeterminate, so inspect the project before retrying an edit")
                if not self._kernel.GetOverlappedResult(self.handle, ctypes.byref(overlapped), ctypes.byref(transferred), False):
                    raise BridgeError(f"pipe completion failed (Windows error {ctypes.get_last_error()})")
            return transferred.value
        finally:
            self._kernel.CloseHandle(event)

    def call(self, command, arguments=None):
        data = encode_request(command, arguments)
        self.connect()
        deadline = time.monotonic() + self.timeout
        try:
            offset = 0
            while offset < len(data):
                buffer = ctypes.create_string_buffer(data[offset:])
                written = self._io(self._kernel.WriteFile, buffer, len(data) - offset, deadline)
                if not written:
                    raise BridgeError("pipe closed during write; inspect the project before retrying an edit")
                offset += written
            while b"\n" not in self._pending:
                buffer = ctypes.create_string_buffer(8192)
                received = self._io(self._kernel.ReadFile, buffer, 8192, deadline)
                if not received:
                    raise BridgeError("pipe closed before replying; inspect the project before retrying an edit")
                self._pending.extend(buffer.raw[:received])
                if len(self._pending) > MAX_RESPONSE_BYTES:
                    raise BridgeError("pipe response exceeds the 64 MiB client limit")
            line, _, remainder = self._pending.partition(b"\n")
            self._pending = bytearray(remainder)
            reply = json.loads(line.decode("utf-8"))
            if not isinstance(reply, dict) or not isinstance(reply.get("ok"), bool):
                raise BridgeError("invalid bridge response")
            return reply
        except (BridgeError, UnicodeError, ValueError) as error:
            self.close()
            message = str(error)
            if "inspect" not in message:
                message += "; command outcome may be indeterminate, so inspect the project before retrying an edit"
            raise BridgeError(message) from error


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("commands", nargs="*")
    parser.add_argument("--args", type=json.loads)
    parser.add_argument("--command-file", type=Path)
    parser.add_argument("--pipe")
    parser.add_argument("--timeout", type=float, default=35.0)
    options = parser.parse_args()
    commands = list(options.commands)
    if options.command_file:
        commands.extend(line for line in options.command_file.read_text(encoding="utf-8-sig").splitlines() if line.strip())
    if not commands or (options.args is not None and len(commands) != 1):
        parser.error("provide commands, or one command with --args")
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    failed = False
    try:
        with BridgeClient(options.pipe, options.timeout) as client:
            for command in commands:
                reply = client.call(command, options.args)
                print(json.dumps(reply, ensure_ascii=False, separators=(",", ":")))
                failed |= not reply["ok"]
    except (BridgeError, ValueError, OSError) as error:
        print(str(error), file=sys.stderr)
        return 1
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
