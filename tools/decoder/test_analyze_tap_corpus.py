import json
import tempfile
import unittest
from pathlib import Path

from analyze_tap_corpus import build_index, session_key


class TapCorpusAnalyzerTests(unittest.TestCase):
    def test_session_key_groups_logger_mirrors(self):
        self.assertEqual(
            session_key(Path("run_2026-07-01.interesting.jsonl")),
            "run_2026-07-01",
        )
        self.assertEqual(
            session_key(Path("run_2026-07-01.raw_mqtt.jsonl.zip")),
            "run_2026-07-01",
        )
        self.assertEqual(
            session_key(Path("site-a/run_2026-07-01.frames.jsonl.gz")),
            "site-a/run_2026-07-01",
        )
        self.assertNotEqual(
            session_key(Path("site-a/capture.frames.jsonl")),
            session_key(Path("site-b/capture.frames.jsonl")),
        )

    def test_build_index_from_predecoded_frame(self):
        record = {
            "ts": "2026-07-01T00:00:00Z",
            "frame": {
                "valid": True,
                "addr_raw_hex": "0x9208",
                "gateway_id_hex": "0x1208",
                "type_code_hex": "0x003B",
                "crc_ok": True,
                "payload_hex": "02AABBCCDDEE00011208",
            },
        }
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "capture.frames.jsonl").write_text(json.dumps(record) + "\n", encoding="utf-8")
            index = build_index(root)
        self.assertEqual(index["file_count"], 1)
        self.assertEqual(index["session_count"], 1)
        self.assertEqual(index["sessions"][0]["tap_euis"], ["02AABBCCDDEE0001"])

    def test_parse_warnings_are_visible_in_index(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "broken.jsonl").write_text("{not-json}\n", encoding="utf-8")
            index = build_index(root)
        self.assertEqual(index["files_with_parse_warnings"], 1)
        self.assertEqual(index["parse_warning_count"], 1)
        self.assertEqual(index["files"][0]["parse_warnings"]["malformed_json_lines"], 1)

    def test_short_0x31_packet_is_not_counted_as_power(self):
        record = {
            "ts": "2026-07-01T00:00:00Z",
            "frame": {
                "valid": True,
                "addr_raw_hex": "0x9208",
                "gateway_id_hex": "0x1208",
                "type_code_hex": "0x0149",
                "crc_ok": True,
                "payload_hex": "001F00000031000212340101AA",
            },
        }
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "capture.frames.jsonl").write_text(json.dumps(record) + "\n", encoding="utf-8")
            index = build_index(root)
        session = index["sessions"][0]
        self.assertEqual(session["power_packet_count"], 0)
        self.assertEqual(session["invalid_power_packet_count"], 1)

    def test_node_table_state_changes_are_retained(self):
        def record(ts, raw_node_id):
            return {
                "ts": ts,
                "frame": {
                    "valid": True,
                    "addr_raw_hex": "0x9208",
                    "gateway_id_hex": "0x1208",
                    "type_code_hex": "0x0B10",
                    "crc_ok": True,
                    "payload_hex": (
                        "000E00270100000001"
                        "02AABBCCDDEE1001"
                        f"{raw_node_id:04X}"
                    ),
                },
            }

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            lines = [record("2026-07-01T00:00:00Z", 0x8002), record("2026-07-01T00:01:00Z", 0x0002)]
            (root / "capture.frames.jsonl").write_text(
                "".join(json.dumps(item) + "\n" for item in lines), encoding="utf-8"
            )
            index = build_index(root)
        changes = index["sessions"][0]["node_table_state_changes"]
        self.assertEqual([item["raw_node_ids"] for item in changes], [["0x8002"], ["0x0002"]])


if __name__ == "__main__":
    unittest.main()
