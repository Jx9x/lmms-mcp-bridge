import json
import unittest
from unittest.mock import Mock, patch

from bridge_client import BridgeClient, BridgeError, MAX_REQUEST_BYTES, encode_request


class RequestEncodingTests(unittest.TestCase):
    def test_legacy_request(self):
        self.assertEqual(encode_request("tempo 128"), b"tempo 128\n")

    def test_structured_utf8_and_escaped_newlines(self):
        arguments = {"name": "音楽", "text": "one\ntwo"}
        encoded = encode_request("rename_track", arguments)
        self.assertEqual(encoded.count(b"\n"), 1)
        self.assertEqual(json.loads(encoded), {"cmd": "rename_track", "args": arguments})

    def test_invalid_commands(self):
        for command in (None, 1, "", " ", "ping\nsong", "ping\rsong"):
            with self.subTest(command=command), self.assertRaises(ValueError):
                encode_request(command)

    def test_arguments_must_be_an_object(self):
        for arguments in ([], "{}", True, 12):
            with self.subTest(arguments=arguments), self.assertRaises(ValueError):
                encode_request("tempo", arguments)

    def test_nonfinite_values_are_rejected(self):
        for value in (float("nan"), float("inf"), -float("inf")):
            with self.subTest(value=value), self.assertRaises(ValueError):
                encode_request("set_parameter", {"value": value})

    def test_limit_counts_utf8_bytes(self):
        self.assertEqual(len(encode_request("p" * MAX_REQUEST_BYTES)), MAX_REQUEST_BYTES + 1)
        for command in ("p" * (MAX_REQUEST_BYTES + 1), "é" * MAX_REQUEST_BYTES):
            with self.subTest(length=len(command)), self.assertRaises(ValueError):
                encode_request(command)


class ClientFailureTests(unittest.TestCase):
    def client(self):
        client = BridgeClient(timeout=0.1)
        client.connect = Mock()
        client.close = Mock()
        return client

    def test_transport_failure_is_not_retried(self):
        client = self.client()
        client._io = Mock(side_effect=BridgeError("pipe completion failed (Windows error 109)"))
        with self.assertRaisesRegex(BridgeError, "indeterminate.*inspect"):
            client.call("set_tempo", {"bpm": 128})
        self.assertEqual(client._io.call_count, 1)
        client.close.assert_called_once()

    def test_malformed_reply_has_indeterminate_outcome(self):
        for response in (b"not-json\n", b"[]\n", b'{"ok":"true"}\n', b"\xff\n"):
            with self.subTest(response=response):
                client = self.client()

                def transfer(function, buffer, size, deadline):
                    if function == client._kernel.WriteFile:
                        return size
                    buffer.value = response
                    return len(response)

                client._io = transfer
                with self.assertRaisesRegex(BridgeError, "inspect.*retrying"):
                    client.call("ping")
                client.close.assert_called_once()

    def test_response_size_limit(self):
        client = self.client()

        def transfer(function, buffer, size, deadline):
            if function == client._kernel.WriteFile:
                return size
            buffer.value = b'{"ok":true}\n'
            return len(buffer.value)

        client._io = transfer
        with patch("bridge_client.MAX_RESPONSE_BYTES", 5), self.assertRaisesRegex(BridgeError, "response exceeds"):
            client.call("ping")
        client.close.assert_called_once()

    def test_local_pipe_names_only(self):
        for pipe in ("remote/pipe", "\\\\server\\pipe\\test", "bad\\name"):
            with self.subTest(pipe=pipe), self.assertRaises(ValueError):
                BridgeClient(pipe)
        self.assertEqual(BridgeClient("\\\\.\\pipe\\test").pipe, "\\\\.\\pipe\\test")

    def test_invalid_timeouts(self):
        for timeout in (0, -1, float("inf"), float("nan")):
            with self.subTest(timeout=timeout), self.assertRaises(ValueError):
                BridgeClient(timeout=timeout)


if __name__ == "__main__":
    unittest.main()
