import json
import tempfile
import unittest
import zipfile
from pathlib import Path

from tigo_tap_decoder_v2 import (
    GatewayFrame,
    ParseDiagnostics,
    counter16_is_newer_or_equal,
    crc16_tigo,
    decode_active_command,
    decode_node_table_response,
    decode_radio_descriptor,
    decode_topology_report,
    read_gateway_frames,
)


class TigoTapDecoderTests(unittest.TestCase):
    def active(self, type_code: int, payload_hex: str):
        frame = GatewayFrame(
            timestamp="test",
            from_gateway=(type_code == 0x0B10),
            gateway_id=0x1208,
            type_code=type_code,
            payload=bytes.fromhex(payload_hex),
            crc_ok=True,
        )
        decoded = decode_active_command(frame)
        self.assertIsNotNone(decoded)
        return decoded

    def test_known_crc_vector(self):
        self.assertEqual(crc16_tigo(bytes.fromhex("00000014372492661235")), 0x1A06)

    def test_pending_node_table_entries(self):
        body = bytes.fromhex(
            "00020002"
            "02AABBCCDDEE10018002"
            "02AABBCCDDEE10020003"
        )
        table = decode_node_table_response(body)
        self.assertEqual(table["entry_count"], 2)
        self.assertEqual(table["entries"][0]["node_id"], 2)
        self.assertTrue(table["entries"][0]["pending"])
        self.assertFalse(table["entries"][1]["pending"])
        self.assertIsNone(decode_node_table_response(body[:-1]))

    def test_node_table_request_and_selector_response(self):
        request = self.active(0x0B0F, "0000002601" + "0000")
        self.assertEqual(request.decoded["node_table_request"], {"start_index": 0})

        response = self.active(0x0B10, "000E002301" + "01")
        self.assertEqual(response.decoded["selector"]["value"], 1)

    def test_radio_descriptor_and_write(self):
        descriptor_hex = (
            "0010A8240C020201008D15001C015A"
            "00112233445566778899AABBCCDDEEFF000002003C"
        )
        descriptor = decode_radio_descriptor(bytes.fromhex(descriptor_hex + "00"))
        self.assertEqual(descriptor["channel"], 16)
        self.assertEqual(descriptor["pan_id_hex"], "A824")
        self.assertEqual(descriptor["probable_network_key_hex"], "00112233445566778899AABBCCDDEEFF")
        self.assertEqual(descriptor["result"], 0)
        self.assertEqual(descriptor["result_state"], "stable")

        busy_descriptor = decode_radio_descriptor(bytes.fromhex(descriptor_hex + "01"))
        self.assertEqual(busy_descriptor["result"], 1)
        self.assertEqual(busy_descriptor["result_state"], "learn_busy")

        request = self.active(0x0B0F, "0000000D01" + "0100" + descriptor_hex)
        self.assertEqual(request.decoded["radio_descriptor_write"]["channel"], 16)

        response = self.active(0x0B10, "000E000E01" + descriptor_hex + "00")
        self.assertEqual(response.decoded["tx_buffers_free"], 14)
        self.assertEqual(response.decoded["radio_descriptor"]["fingerprint_sha256_12"], "DBA7A465141F")

    def test_learn_control_and_status(self):
        request = self.active(0x0B0F, "0000002D0C" + "BABE020384000A0100")
        learn = request.decoded["learn_control"]
        self.assertEqual(learn["action"], 2)
        self.assertEqual(learn["countdown_seconds"], 900)
        self.assertEqual(learn["expected_nodes"], 10)

        response = self.active(0x0B10, "000E002F0C" + "01038400010001000A")
        status = response.decoded["network_status"]
        self.assertEqual(status["confirmed_nodes"], 1)
        self.assertEqual(status["expected_nodes"], 10)
        self.assertEqual(status["active_nodes"], 1)
        self.assertEqual(status["configured_nodes"], 10)

    def test_topology_packet_is_23_bytes(self):
        topology = decode_topology_report(
            bytes.fromhex("123400021111000202AABBCCDDEE100155010123001328")
        )
        self.assertIsNotNone(topology)
        self.assertEqual(topology["parent_short_addr"], 0x1111)
        self.assertEqual(topology["hop_depth"], 1)

    def test_zipped_mqtt_jsonl(self):
        record = {
            "ts": "2026-07-05T12:43:38.113772+00:00",
            "frame": {
                "gateway_id_hex": "0x1208",
                "addr_raw_hex": "0x1208",
                "type_code_hex": "0x0148",
                "crc_ok": True,
                "payload_hex": "0001000004",
            },
        }
        with tempfile.TemporaryDirectory() as tmp:
            archive_path = Path(tmp) / "capture.jsonl.zip"
            with zipfile.ZipFile(archive_path, "w") as archive:
                archive.writestr("capture.jsonl", json.dumps(record) + "\n")
            frames = read_gateway_frames(archive_path)
        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0].type_code, 0x0148)
        self.assertEqual(frames[0].payload, bytes.fromhex("0001000004"))

    def test_support_logger_interesting_frame_schema(self):
        record = {
            "ts": "2026-07-09T08:53:00Z",
            "kind": "interesting_frame",
            "data": {
                "gateway_id_hex": "0x1209",
                "addr_raw_hex": "0x9209",
                "type_hex": "0x0149",
                "crc_ok": True,
                "payload_preview_hex": "001F000000",
                "payload_truncated": False,
            },
        }
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "interesting.jsonl"
            path.write_text(json.dumps(record) + "\n", encoding="utf-8")
            frames = read_gateway_frames(path)
        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0].gateway_id, 0x1209)
        self.assertTrue(frames[0].from_gateway)

    def test_experiment_logger_nested_mqtt_frame_schema(self):
        record = {
            "ts": "2026-08-02T11:24:14+02:00",
            "kind": "mqtt",
            "data": {
                "topic": "openTAPtoX/esp32c6/raw/frame",
                "value": {
                    "gateway_id_hex": "0x1209",
                    "addr_raw_hex": "0x9209",
                    "type_code_hex": "0x0149",
                    "crc_ok": True,
                    "payload_hex": "001F000000",
                },
            },
        }
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "experiment.raw_mqtt.jsonl"
            path.write_text(json.dumps(record) + "\n", encoding="utf-8")
            frames = read_gateway_frames(path)
        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0].gateway_id, 0x1209)
        self.assertEqual(frames[0].type_code, 0x0149)

    def test_16_bit_counter_comparison_accepts_wraparound(self):
        self.assertTrue(counter16_is_newer_or_equal(0x0010, 0xFFF0))
        self.assertTrue(counter16_is_newer_or_equal(0x1234, 0x1234))
        self.assertFalse(counter16_is_newer_or_equal(0xFFF0, 0x0010))

    def test_t_zero_predecoded_frame_before_first_raw_chunk_is_retained(self):
        data = bytes.fromhex("120801480001000004")
        checksum = crc16_tigo(data).to_bytes(2, "little")
        escape = {
            0x7E: bytes.fromhex("7E00"),
            0x24: bytes.fromhex("7E01"),
            0x23: bytes.fromhex("7E02"),
            0x25: bytes.fromhex("7E03"),
            0xA4: bytes.fromhex("7E04"),
            0xA3: bytes.fromhex("7E05"),
            0xA5: bytes.fromhex("7E06"),
        }
        encoded = b"".join(escape.get(byte, bytes([byte])) for byte in data + checksum)
        raw_frame = bytes.fromhex("7E07") + encoded + bytes.fromhex("7E08")
        raw_hex = " ".join(f"{byte:02X}" for byte in raw_frame)
        content = (
            "FRAME t=0 dir=tap_to_host addr_raw=9208 gateway=1208 "
            "type=000B crc=ok payload_len=1 payload=41\n"
            f"RAW t=100 len={len(raw_frame)} hex={raw_hex}\n"
            "FRAME t=200 dir=tap_to_host addr_raw=9208 gateway=1208 "
            "type=000F crc=ok payload_len=2 payload=20 10\n"
        )
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "mixed.log"
            path.write_text(content, encoding="utf-8")
            frames = read_gateway_frames(path)
        self.assertEqual([frame.type_code for frame in frames], [0x000B, 0x0148, 0x000F])
        self.assertEqual(frames[0].timestamp, "t=0ms")

    def test_declared_payload_length_mismatch_is_reported_and_skipped(self):
        content = (
            "FRAME t=1 dir=tap_to_host addr_raw=9208 gateway=1208 "
            "type=000F crc=ok payload_len=3 payload=20 10\n"
        )
        diagnostics = ParseDiagnostics()
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "bad-length.log"
            path.write_text(content, encoding="utf-8")
            frames = read_gateway_frames(path, diagnostics)
        self.assertEqual(frames, [])
        self.assertEqual(diagnostics.declared_length_mismatches, 1)


if __name__ == "__main__":
    unittest.main()
