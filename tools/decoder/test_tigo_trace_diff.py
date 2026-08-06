import json
import tempfile
import unittest
from pathlib import Path

try:
    from .tigo_trace_diff import (
        alignment_tokens,
        build_trace_model,
        compare_traces,
        exclusive_before_release,
        normalized_frame_signature,
        node_alias,
        request_is_state_changing,
        render_markdown,
        TraceModel,
        Transaction,
    )
    from .tigo_tap_decoder_v2 import GatewayFrame
except ImportError:
    from tigo_trace_diff import (
        alignment_tokens,
        build_trace_model,
        compare_traces,
        exclusive_before_release,
        normalized_frame_signature,
        node_alias,
        request_is_state_changing,
        render_markdown,
        TraceModel,
        Transaction,
    )
    from tigo_tap_decoder_v2 import GatewayFrame


class TraceDiffTests(unittest.TestCase):
    def test_success_only_candidates_ignore_failure_commands_after_release_boundary(self):
        def transaction(index, at_ms, signature):
            return Transaction(index, at_ms, signature, 0x003A, "TAP[X]", 0x003B, None, None)

        common = transaction(0, 0.0, "COMMON")
        candidate = transaction(1, 5.0, "CANDIDATE")
        late_candidate = transaction(1, 20.0, "CANDIDATE")
        success = TraceModel("success", "gateway_frames", 0, 0, [], [common, candidate], [], 10.0, None, None, {}, {})
        failure = TraceModel("failure", "gateway_frames", 0, 0, [], [common, late_candidate], [], None, None, None, {}, {})
        self.assertEqual(
            [item["signature"] for item in exclusive_before_release(success, failure)],
            ["CANDIDATE"],
        )

    def test_rf_short_id_is_normalized_to_optimizer_eui_when_known(self):
        aliases = {
            2: "NODE[02AABBCCDDEE1001]",
            11: "NODE[02AABBCCDDEE1001]",
        }
        self.assertEqual(node_alias(2, aliases), node_alias(11, aliases))
        self.assertEqual(node_alias(7, aliases), "NODE#7")

    def test_rsd_stop_and_run_are_state_changing_transactions(self):
        self.assertTrue(request_is_state_changing(0x0B00, b"\x00"))
        self.assertTrue(request_is_state_changing(0x0B00, b"\x01"))
        self.assertFalse(request_is_state_changing(0x0B00, b"\x02"))

    def write_text(self, directory: str, name: str, text: str) -> Path:
        path = Path(directory) / name
        path.write_text(text, encoding="utf-8")
        return path

    def write_mqtt_frames(self, directory: str, name: str, frames: list[dict]) -> Path:
        path = Path(directory) / name
        records = []
        for frame in frames:
            payload = bytes.fromhex(frame.get("payload_hex", ""))
            records.append({
                "ts": frame["ts"],
                "kind": "mqtt",
                "data": {
                    "topic": "openTAPtoX/esp32c6/raw/frame",
                    "value": {
                        "addr_raw_hex": "9209" if frame.get("from_gateway", False) else "1209",
                        "gateway_id_hex": "1209",
                        "type_code_hex": f"{frame['type_code']:04X}",
                        "payload_len": len(payload),
                        "payload_hex": payload.hex().upper(),
                        "payload_truncated": False,
                        "crc_ok": True,
                        **(
                            {"device_ms": frame["device_ms"]}
                            if "device_ms" in frame else {}
                        ),
                    },
                },
            })
        path.write_text("\n".join(json.dumps(record) for record in records) + "\n", encoding="utf-8")
        return path

    def test_serial_diagnostics_separate_rf_response_from_local_ack(self):
        with tempfile.TemporaryDirectory() as directory:
            success = self.write_text(
                directory,
                "success.log",
                "\n".join([
                    "OTX event t=100 pv-subcmd 0x06 tx=0000000608000B5E303056657273696F6E0D",
                    "OTX event t=125 pv-subcmd 0x06 ack status=0x0D rsp=0x07 body=4F4B",
                ]),
            )
            failure = self.write_text(
                directory,
                "failure.log",
                "\n".join([
                    "OTX event t=100 pv-subcmd 0x06 tx=0000000609000B5E303056657273696F6E0D",
                    "OTX event t=112 pv-subcmd 0x06 short ack raw=0002; waiting active ack",
                    "OTX event t=2600 pv_subcmd failed: timeout",
                ]),
            )
            report = compare_traces(success, failure)
            self.assertEqual(report["transactions"]["success"][0]["outcome"], "rf_response_credit_0D")
            self.assertEqual(
                report["transactions"]["failure"][0]["outcome"],
                "tap_local_ack_no_rf_response",
            )
            self.assertIsNotNone(report["alignment"]["first_global_sequence_divergence"])
            self.assertIsNotNone(report["first_common_request_outcome_divergence"])

    def test_pv_response_pairing_requires_matching_subcommand_with_reused_dsn(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_mqtt_frames(directory, "dsn_collision.jsonl", [
                {
                    "ts": "2026-08-04T10:00:00.000+02:00",
                    "type_code": 0x0B0F,
                    "payload_hex": "0000000608000B5E303056657273696F6E0D",
                },
                {
                    "ts": "2026-08-04T10:00:00.010+02:00",
                    "type_code": 0x0B0F,
                    "payload_hex": "0000001708000B0F",
                },
                {
                    "ts": "2026-08-04T10:00:00.020+02:00",
                    "type_code": 0x0B10,
                    "from_gateway": True,
                    "payload_hex": "000D0007084F4B",
                },
            ])
            model = build_trace_model(path)
            self.assertTrue(model.transactions[0].outcome.startswith("rf_response"))
            self.assertEqual(model.transactions[1].outcome, "capture_end_pending")

    def test_empty_node_response_is_not_counted_as_rf(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_mqtt_frames(directory, "empty_node_response.jsonl", [
                {
                    "ts": "2026-08-04T10:00:00.000+02:00",
                    "type_code": 0x0B0F,
                    "payload_hex": "0000000608000B5E303056657273696F6E0D",
                },
                {
                    "ts": "2026-08-04T10:00:00.020+02:00",
                    "type_code": 0x0B10,
                    "from_gateway": True,
                    "payload_hex": "000D000708",
                },
            ])
            model = build_trace_model(path)
            self.assertEqual(
                model.transactions[0].outcome,
                "tap_ack_empty_node_response_credit_0D",
            )
            self.assertIsNone(model.first_rf_response_ms)

    def test_unknown_rf_packet_is_kept_with_full_payload(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "unknown.jsonl"
            status = bytes.fromhex("001F000001")
            packet = bytes.fromhex("55000212340103AABBCC")
            record = {
                "ts": "2026-08-04T10:00:00+02:00",
                "kind": "mqtt",
                "data": {
                    "topic": "openTAPtoX/esp32c6/raw/frame",
                    "value": {
                        "addr_raw_hex": "9209",
                        "gateway_id_hex": "1209",
                        "type_code_hex": "0149",
                        "payload_len": len(status + packet),
                        "payload_hex": (status + packet).hex().upper(),
                        "payload_truncated": False,
                        "crc_ok": True,
                    },
                },
            }
            path.write_text(json.dumps(record) + "\n", encoding="utf-8")
            model = build_trace_model(path)
            self.assertEqual(len(model.unknown_rf_packets), 1)
            self.assertEqual(model.unknown_rf_packets[0]["ptype"], 0x55)
            self.assertEqual(model.unknown_rf_packets[0]["data_hex"], "AABBCC")

    def test_truncated_0149_packet_remainder_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            status = "001F000001"
            truncated_packet = "55000212340105AABB"
            path = self.write_mqtt_frames(directory, "truncated.jsonl", [{
                "ts": "2026-08-04T10:00:00+02:00",
                "type_code": 0x0149,
                "from_gateway": True,
                "payload_hex": status + truncated_packet,
            }])
            model = build_trace_model(path)
            self.assertEqual(len(model.unknown_rf_packets), 1)
            self.assertTrue(model.unknown_rf_packets[0]["malformed_remainder"])
            self.assertEqual(model.unknown_rf_packets[0]["data_hex"], truncated_packet)

    def test_all_0149_packet_classes_are_chronological_alignment_events(self):
        with tempfile.TemporaryDirectory() as directory:
            status = "001F000001"
            topology_data = "1234000211110000" + "02AABBCCDDEE1001" + "4E010001000000"
            power_data = "2582016464012C00000012344E"

            def packet(ptype: int, data_hex: str, dsn: int) -> str:
                return f"{ptype:02X}00021234{dsn:02X}{len(bytes.fromhex(data_hex)):02X}{data_hex}"

            path = self.write_mqtt_frames(directory, "timeline.jsonl", [
                {"ts": "2026-08-04T10:00:00.010+02:00", "type_code": 0x003A},
                {
                    "ts": "2026-08-04T10:00:00.020+02:00",
                    "type_code": 0x0149,
                    "from_gateway": True,
                    "payload_hex": status + packet(0x09, topology_data, 1),
                },
                {"ts": "2026-08-04T10:00:00.030+02:00", "type_code": 0x000A},
                {
                    "ts": "2026-08-04T10:00:00.040+02:00",
                    "type_code": 0x0149,
                    "from_gateway": True,
                    "payload_hex": status + packet(0x31, power_data, 2),
                },
                {
                    "ts": "2026-08-04T10:00:00.050+02:00",
                    "type_code": 0x0149,
                    "from_gateway": True,
                    "payload_hex": status + packet(0x55, "AABBCC", 3),
                },
            ])
            model = build_trace_model(path)
            tokens = alignment_tokens(model)
            self.assertEqual([event.category for event in model.events if event.direction == "rf"], [
                "topology", "power", "unknown_rf_packet",
            ])
            self.assertEqual(tokens, [
                "FRAME:003A: -> capture_end_pending",
                "RF:TOPOLOGY:NODE[02AABBCCDDEE1001]",
                "FRAME:000A: -> capture_end_pending",
                "RF:POWER:NODE[02AABBCCDDEE1001]",
                "RF:UNKNOWN:55:NODE[02AABBCCDDEE1001]:AABBCC",
            ])

    def test_capture_edge_request_is_not_an_outcome_divergence(self):
        with tempfile.TemporaryDirectory() as directory:
            success = self.write_mqtt_frames(directory, "success.jsonl", [
                {"ts": "2026-08-04T10:00:00.000+02:00", "type_code": 0x003A},
                {
                    "ts": "2026-08-04T10:00:00.020+02:00",
                    "type_code": 0x003B,
                    "from_gateway": True,
                    "payload_hex": "02AABBCCDDEE00011209",
                },
            ])
            failure = self.write_mqtt_frames(directory, "failure.jsonl", [
                {"ts": "2026-08-04T10:00:00.000+02:00", "type_code": 0x003A},
            ])
            report = compare_traces(success, failure)
            self.assertEqual(
                report["transactions"]["failure"][0]["outcome"],
                "capture_end_pending",
            )
            self.assertEqual(
                report["failure"]["capture_completeness"]["capture_edge_pending_transactions"],
                1,
            )
            self.assertIsNone(report["first_common_request_outcome_divergence"])

    def test_device_timestamp_drives_latency_instead_of_mqtt_delivery(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_mqtt_frames(directory, "device_clock.jsonl", [
                {
                    "ts": "2026-08-04T10:00:00.000+02:00",
                    "device_ms": 1000,
                    "type_code": 0x003A,
                },
                {
                    "ts": "2026-08-04T10:00:03.000+02:00",
                    "device_ms": 1010,
                    "type_code": 0x003B,
                    "from_gateway": True,
                    "payload_hex": "02AABBCCDDEE00011209",
                },
            ])
            model = build_trace_model(path)
            self.assertEqual(model.transactions[0].latency_ms, 10.0)

    def test_report_exposes_capture_quality_and_distinct_divergences(self):
        with tempfile.TemporaryDirectory() as directory:
            success_lines = []
            for index in range(4):
                dsn = 8 + index
                success_lines.extend([
                    f"OTX event t={100 + index * 100} pv-subcmd 0x06 tx=00000006{dsn:02X}000B5E303056657273696F6E0D",
                    f"OTX event t={125 + index * 100} pv-subcmd 0x06 ack status=0x0D rsp=0x07 body=",
                ])
            success = self.write_text(directory, "success.log", "\n".join(success_lines))
            failure = self.write_text(
                directory,
                "failure.log",
                "\n".join([
                    "OTX event t=100 pv-subcmd 0x06 tx=0000000609000B5E303056657273696F6E0D",
                    "OTX event t=112 pv-subcmd 0x06 short ack raw=0002; waiting active ack",
                ]),
            )
            report = compare_traces(success, failure)
            markdown = render_markdown(report)
            self.assertEqual(report["schema_version"], 2)
            self.assertTrue(report["comparison_quality"]["capture_asymmetry"]["materially_asymmetric"])
            self.assertTrue(report["comparison_quality"]["alignment_coverage"]["low_coverage"])
            self.assertEqual(
                report["success"]["capture_completeness"]["capture_mode"],
                "serial_diagnostic_fallback",
            )
            self.assertFalse(report["success"]["capture_completeness"]["verifiable"])
            self.assertIn("first_global_sequence_divergence", report["alignment"])
            self.assertIn("first_common_request_outcome_divergence", report)
            self.assertIn("## First global sequence-alignment divergence", markdown)
            self.assertIn("## First common request-outcome divergence", markdown)
            self.assertIn("Capture sizes are materially asymmetric", markdown)

    def test_cursor_is_normalized_but_bootstrap_payload_is_not(self):
        aliases = {0x1209: "TAP[X]"}
        first = GatewayFrame("t=1ms", False, 0x1209, 0x0148, bytes.fromhex("0001123404"), True)
        second = GatewayFrame("t=2ms", False, 0x1209, 0x0148, bytes.fromhex("0001ABCD04"), True)
        bootstrap = GatewayFrame("t=3ms", False, 0x1209, 0x0148, bytes.fromhex("0000EEEE00"), True)
        first_signature, _ = normalized_frame_signature(first, aliases, {})
        second_signature, _ = normalized_frame_signature(second, aliases, {})
        bootstrap_signature, _ = normalized_frame_signature(bootstrap, aliases, {})
        self.assertEqual(first_signature, second_signature)
        self.assertNotEqual(first_signature, bootstrap_signature)


if __name__ == "__main__":
    unittest.main()
