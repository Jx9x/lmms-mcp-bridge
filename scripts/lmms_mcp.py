import argparse
import asyncio
import json
import math
import sys

from jsonschema import Draft7Validator, ValidationError
from mcp import types
from mcp.server import Server
from mcp.server.stdio import stdio_server

from bridge_client import BridgeClient, BridgeError, encode_request


CONSOLIDATED_TOOLS = frozenset({
    "create_track", "update_track", "update_clip", "update_project", "update_mixer_channel", "update_effect", "transport",
})
OPTIONAL_TOOLS = frozenset({
    "help", "ping", "selftest", "show", "get_project_xml", "note_name_to_key", "key_to_note_name",
    "bars_to_ticks_converter", "ticks_to_bars_converter", "generate_scale", "set_zyn_params", "load_zyn_preset",
})
REPLACED_TOOLS = frozenset({
    "song", "tempo", "get_arrangement", "list_available_plugins", "list_effect_plugins", "add_note", "add_note_by_name",
    "set_parameter", "project.new", "list_zyn_presets", "tracks", "get_tracks", "get_notes", "load_preset",
    "add_instrument_track", "add_sample_track", "add_automation_track", "add_pattern_track",
    "rename_track", "set_track_volume", "set_track_panning", "mute_track", "solo_track", "assign_track_to_mixer",
    "rename_clip", "move_clip", "resize_clip", "set_tempo", "set_time_signature", "set_master_volume", "set_master_pitch",
    "set_mixer_channel_name", "set_mixer_channel_volume", "mute_mixer_channel", "solo_mixer_channel",
    "toggle_effect", "set_effect_wet", "play", "pause", "stop", "set_playhead", "set_automation_point",
})
MCP_DESCRIPTIONS = {
    "get_lmms_info": "Inspect LMMS version, installed instrument/effect registries, preset/sample roots, tick/MIDI limits, and export capabilities.",
    "get_pattern": "Inspect pattern metadata and notes using MIDI keys (C4=60). include_disabled adds zero-volume notes. Current note indices may shift after edits.",
    "add_notes_batch": (
        "Validate then insert one or more notes and extend the pattern. key accepts MIDI 12..119 or a note name (C4=60); "
        "pos/length are pattern-relative ticks (48 per quarter), length=-1 is a drum step. Returns inserted notes and a pattern handle; full_detail includes the pattern."
    ),
}

NOTE_EDITS = frozenset({"add_notes_batch", "set_note", "set_notes_batch", "replace_notes", "remove_note", "clear_notes", "transpose_pattern"})
TRACK_EDITS = frozenset({"create_track", "clone_track", "update_track", "load_instrument", "set_instrument_settings",
                         "load_instrument_file", "add_vst_track", "assign_sample_file"})


def native_options(name, arguments, schema):
    """Keep native compatibility defaults while requesting compact MCP results."""
    options = dict(arguments)
    full = options.pop("full_detail", False)
    options.pop("wait_seconds", None)
    properties = schema.get("properties", {})
    defaults = {"include_notes": name == "get_pattern", "include_parameters": False,
                "include_clips": name not in TRACK_EDITS, "include_pattern": False,
                "include_all_channels": False, "include_all_effects": False}
    for key, default in defaults.items():
        if key in properties:
            options.setdefault(key, True if full else default)
    if "limit" in properties:
        options.setdefault("limit", 5000 if full else 50)
    return options


def compact_schema(value):
    if isinstance(value, dict):
        return {key: compact_schema(item) for key, item in value.items() if not (key == "maxLength" and item == 32767)}
    if isinstance(value, list):
        return [compact_schema(item) for item in value]
    return value


def compact_result(value):
    if isinstance(value, list):
        return [compact_result(item) for item in value]
    if not isinstance(value, dict):
        return value
    result = {key: compact_result(item) for key, item in value.items()}
    if {"midi_key", "lmms_key", "pos", "length"} <= result.keys():
        result["key"] = result.pop("midi_key")
        for alias in ("lmms_key", "len", "vol", "pan"):
            result.pop(alias, None)
    return result


def validation_message(error):
    """Explain the failing constraint without echoing arguments or schemas."""
    path = "/" + "/".join(str(part).replace("~", "~0").replace("/", "~1")[:64]
                          for part in error.absolute_path)
    path = path[:192] if path != "/" else "arguments"
    constraint = error.validator_value
    validator = error.validator
    if validator == "type":
        expected = constraint if isinstance(constraint, str) else " or ".join(constraint)
        reason = f"must be {expected}"
    elif validator in {"minimum", "maximum", "exclusiveMinimum", "exclusiveMaximum", "multipleOf"}:
        relation = {"minimum": "at least", "maximum": "at most", "exclusiveMinimum": "greater than",
                    "exclusiveMaximum": "less than", "multipleOf": "a multiple of"}[validator]
        reason = f"must be {relation} {constraint}"
    elif validator in {"minLength", "maxLength", "minItems", "maxItems", "minProperties", "maxProperties"}:
        relation = "at least" if validator.startswith("min") else "at most"
        unit = "characters" if validator.endswith("Length") else "items" if validator.endswith("Items") else "properties"
        reason = f"must have {relation} {constraint} {unit}"
    elif validator == "required":
        missing = next((name for name in constraint if name not in error.instance), "required property")
        reason = f"missing required property {missing}"
    elif validator == "enum":
        choices = json.dumps(constraint, ensure_ascii=False, separators=(",", ":"))
        reason = f"must be one of {choices}"
    elif validator == "const":
        reason = f"must equal {json.dumps(constraint, ensure_ascii=False)}"
    elif validator == "additionalProperties":
        reason = "contains unsupported properties"
    elif validator in {"oneOf", "anyOf"}:
        reason = "must match exactly one allowed form" if validator == "oneOf" else "must match an allowed form"
    elif validator == "not":
        reason = "contains a disallowed combination of properties"
    elif validator == "dependencies":
        reason = "is missing a dependent property"
    else:
        reason = f"does not satisfy the {validator} constraint"
    return f"invalid arguments at {path}: {reason[:512]}"


class BridgeGateway:
    def __init__(self, pipe=None, timeout=35.0, request=None, include_tools=()):
        self.pipe = pipe
        self.timeout = timeout
        self.request = request or self._pipe_request
        self.include_tools = frozenset(include_tools)
        if not self.include_tools <= OPTIONAL_TOOLS:
            raise ValueError("include_tools must contain only optional LMMS tool names")
        self.lock = asyncio.Lock()
        self.tools = None
        self.native_tools = None

    def _pipe_request(self, command, arguments):
        with BridgeClient(self.pipe, self.timeout) as client:
            return client.call(command, arguments)

    async def _request(self, command, arguments=None):
        encode_request(command, arguments)
        async with self.lock:
            future = asyncio.get_running_loop().run_in_executor(None, self.request, command, arguments)
            try:
                return await asyncio.shield(future)
            except asyncio.CancelledError:
                await future
                raise

    async def catalog(self):
        response = await self._request("help")
        if not response.get("ok") or not isinstance(response.get("tools"), list):
            raise BridgeError(response.get("error", "bridge did not provide a tool catalog"))
        native = {tool.name: tool for tool in (types.Tool.model_validate(item) for item in response["tools"])}
        if not CONSOLIDATED_TOOLS <= native.keys():
            raise BridgeError("native bridge lacks consolidated commands; rebuild the plugin and safely restart LMMS")
        hidden = REPLACED_TOOLS | (OPTIONAL_TOOLS - self.include_tools)
        tools = []
        for name, tool in native.items():
            if name in hidden:
                continue
            schema = compact_schema(tool.inputSchema)
            schema["properties"]["full_detail"] = {"type": "boolean", "description": "Include notes, parameters and complete related objects; explicit include_* options override this."}
            if name == "render_project":
                schema["properties"]["wait_seconds"] = {"type": "number", "minimum": 0, "maximum": 30,
                    "description": "Wait up to this many seconds for this job; a timeout returns its running status and never cancels it."}
            tools.append(tool.model_copy(update={
                "inputSchema": schema, "description": MCP_DESCRIPTIONS.get(name, tool.description),
            }))
        self.native_tools = native
        self.tools = {tool.name: tool for tool in tools}
        return tools

    async def call(self, name, arguments=None):
        arguments = {} if arguments is None else arguments
        native_request_started = False
        try:
            if self.tools is None:
                await self.catalog()
            tool = self.tools.get(name)
            if tool is None:
                raise ValueError("unknown or unexposed MCP tool; inspect tools/list")
            Draft7Validator(tool.inputSchema).validate(arguments)
            native_arguments = native_options(name, arguments, self.native_tools[name].inputSchema)
            Draft7Validator(self.native_tools[name].inputSchema).validate(native_arguments)
            native_request_started = True
            response = await self._request(name, native_arguments)
            if name in NOTE_EDITS and not native_arguments.get("include_pattern", False) and response.get("ok"):
                response = {key: value for key, value in response.items() if key != "pattern"}
            if name == "render_project" and response.get("ok") and arguments.get("wait_seconds", 0) > 0:
                job_id = response.get("job", {}).get("job_id")
                if job_id is None:
                    raise BridgeError("render start did not return a job_id; inspect render_status")
                deadline = asyncio.get_running_loop().time() + arguments["wait_seconds"]
                while response.get("job", {}).get("state") == "running":
                    remaining = deadline - asyncio.get_running_loop().time()
                    if remaining <= 0:
                        response = {**response, "wait_timed_out": True}
                        break
                    await asyncio.sleep(min(0.1, remaining))
                    response = await self._request("render_status", {"job_id": job_id})
                    if not response.get("ok"):
                        break
                response = {**response, "cmd": name}
                if response.get("job", {}).get("state") in {"failed", "cancelled"}:
                    response = {**response, "ok": False, "error": response["job"].get("error") or "render did not succeed"}
            response = compact_result(response)
            if name == "get_pattern" and response.get("ok") and not arguments.get("include_disabled", False):
                pattern = response["pattern"]
                if "notes" in pattern:
                    pattern["notes"] = [note for note in pattern["notes"] if note["volume"] > 0]
        except ValidationError as error:
            response = {"ok": False, "cmd": name, "changed": False, "error": validation_message(error)}
        except ValueError as error:
            response = {"ok": False, "cmd": name, "changed": False, "error": str(error)}
        except (BridgeError, OSError) as error:
            response = {"ok": False, "cmd": name, "changed": None if native_request_started else False,
                        "outcome_indeterminate": native_request_started, "error": str(error)}
        return types.CallToolResult(
            content=[types.TextContent(type="text", text=json.dumps(response, ensure_ascii=False, allow_nan=False))],
            structuredContent=response,
            isError=not response.get("ok", False),
        )


def create_server(gateway):
    server = Server(
        "lmms-native-bridge",
        version="0.4.0",
        instructions=(
            "These tools inspect and edit the LIVE LMMS 1.2.2 project, not an offline project file. "
            "Start LMMS separately with scripts/start-lmms-with-bridge.ps1; this server never starts or closes LMMS. "
            "Use get_project_info and save_project to protect unsaved work. Track/clip handles survive index shifts and expire on new/load/delete. "
            "Use project_token with model_id edits to guard against project replacement. Native parameter IDs are session-local. "
            "Note key inputs and all MCP note.key results use MIDI pitches (C4=60); lmms_key inputs explicitly use native keys (C4=48). "
            "There are 48 ticks per quarter note; note positions are pattern-relative. Use native units for parameters. "
            "get_pattern returns metadata and notes; edits return changed objects by default, and full_detail includes related objects. "
            "list_samples returns absolute loader paths. Preset/sample loading preserves names, mix and sample length by default. "
            "Sample clip names last only for the current session in stock LMMS 1.2.2; inspect warnings and capabilities. "
            "changed reports data changes in this call; modified is the current native song dirty state. No-op setters leave it unchanged. "
            "Playback volume is capped at 40 percent by default; update_project can configure playback_volume_cap. "
            "After a transport error, inspect the project before retrying an edit; edits are never retried automatically. "
            "render_project is asynchronous: poll render_status with job_id and require job.state=succeeded, or set wait_seconds up to 30. "
            "Project edits and playback are blocked during rendering; cancel_render preserves the output destination."
        ),
    )

    @server.list_tools()
    async def list_tools():
        return await gateway.catalog()

    @server.call_tool(validate_input=False)
    async def call_tool(name, arguments):
        return await gateway.call(name, arguments)

    return server


async def run(pipe, timeout, include_tools=()):
    server = create_server(BridgeGateway(pipe, timeout, include_tools=include_tools))
    async with stdio_server() as (reader, writer):
        await server.run(reader, writer, server.create_initialization_options())


def main():
    parser = argparse.ArgumentParser(description="Expose the live LMMS native named-pipe bridge as an MCP stdio server.")
    parser.add_argument("--pipe")
    parser.add_argument("--timeout", type=float, default=35.0)
    parser.add_argument("--include-tool", action="append", choices=sorted(OPTIONAL_TOOLS), default=[],
                        help="Explicitly expose an optional diagnostic, theory, or Zyn tool; repeat for multiple tools.")
    options = parser.parse_args()
    if not math.isfinite(options.timeout) or options.timeout <= 0:
        parser.error("timeout must be positive and finite")
    try:
        asyncio.run(run(options.pipe, options.timeout, options.include_tool))
    except (BridgeError, ValueError, OSError) as error:
        print(str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
