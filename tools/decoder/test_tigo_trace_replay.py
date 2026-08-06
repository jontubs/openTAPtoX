import unittest
import urllib.error
from unittest.mock import patch

import tigo_trace_replay as replay_module

from tigo_trace_replay import (
    abort_trace_replay_if_active,
    append_trace_replay_checked,
    classify_live_outcome,
    ddmin,
    encode_firmware_replay_step,
    execute_step,
    execute_plan_on_esp,
    firmware_replay_outcome,
    frame_risk,
    outcome_matches,
    operational_target,
    pv_command_risk,
    replay_hold_offset_ms,
    resolve_pv_body,
    resolve_simple_payload,
    select_plan,
    start_trace_replay_checked,
)


class FakeReplayClient:
    def node_map(self):
        return {
            "nodes": [{
                "node_id": 7,
                "raw_node_id_hex": "0x8007",
                "long_addr": "02AABBCCDDEE1007",
            }]
        }


class LostStartResponseClient:
    def __init__(self, accepted):
        self.accept_on_first_loss = accepted
        self.running = False
        self.start_calls = 0

    def trace_replay(self):
        if self.running:
            return {"state": "running", "active": True, "step_count": 4, "started_ms": 1234}
        return {"state": "loaded", "active": False, "step_count": 4, "started_ms": 0}

    def trace_replay_plan(self, mode, **values):
        self.start_calls += 1
        if self.start_calls == 1:
            self.running = self.accept_on_first_loss
            raise ConnectionResetError("start response lost")
        self.running = True
        return {"ok": True}


class LostAppendResponseClient:
    def __init__(self, accepted):
        self.accept_on_first_loss = accepted
        self.count = 3
        self.append_calls = 0

    def trace_replay(self):
        return {"state": "loaded", "active": False, "step_count": self.count}

    def trace_replay_plan(self, mode, **values):
        self.append_calls += 1
        if self.append_calls == 1:
            if self.accept_on_first_loss:
                self.count += 1
            raise ConnectionResetError("append response lost")
        self.count += 1
        return {"ok": True, "index": values["index"]}


class CompletedDuringAbortClient:
    def __init__(self):
        self.active = True
        self.abort_calls = 0

    def trace_replay(self):
        return {"state": "running" if self.active else "complete", "active": self.active}

    def trace_replay_plan(self, mode, **values):
        self.abort_calls += 1
        self.active = False
        raise urllib.error.HTTPError("http://esp/abort", 409, "Conflict", {}, None)


class InterruptCleanupClient:
    def __init__(self):
        self.active = False
        self.abort_calls = 0

    def status(self):
        return {
            "gateway_id_hex": "0x1209",
            "gateway_long_addr": "02AABBCCDDEE0001",
            "radio_profile_fingerprint_fnv1a32": "A1306292",
            "node_table_hash_fnv1a32": "4BC80884",
        }

    def journal(self):
        return {}

    def trace_replay(self):
        return {"state": "running" if self.active else "aborted", "active": self.active}

    def trace_replay_plan(self, mode, **values):
        if mode == "abort":
            self.abort_calls += 1
            self.active = False
            return {"ok": True}
        raise AssertionError(mode)


class TraceReplayTests(unittest.TestCase):
    def test_keyboard_interrupt_after_accepted_start_aborts_scheduler(self):
        client = InterruptCleanupClient()

        def interrupted_start(_client, **_values):
            client.active = True
            raise KeyboardInterrupt()

        plan = {"source_tap_euis": [], "steps": []}
        with patch.object(replay_module, "ReplayClient", return_value=client), \
             patch.object(replay_module, "upload_plan_to_esp", return_value=[]), \
             patch.object(replay_module, "start_trace_replay_checked", side_effect=interrupted_start):
            with self.assertRaises(KeyboardInterrupt):
                execute_plan_on_esp(
                    plan,
                    base_url="http://esp",
                    timing_scale=1.0,
                    allow_state_changing=False,
                )
        self.assertFalse(client.active)
        self.assertEqual(client.abort_calls, 1)

    def test_rsd_stop_and_run_are_state_changing_frames(self):
        self.assertEqual(frame_risk(0x0B00, b"\x00"), "state_changing")
        self.assertEqual(frame_risk(0x0B00, b"\x01"), "state_changing")
        self.assertEqual(frame_risk(0x0B00, b"\x02"), "unknown")
        self.assertEqual(frame_risk(0x003A), "read_only")
        self.assertEqual(frame_risk(0x7777), "unknown")

    def test_abort_race_accepts_scheduler_that_completed_itself(self):
        client = CompletedDuringAbortClient()
        result = abort_trace_replay_if_active(client, retry_delay_s=0)
        self.assertFalse(result["active"])
        self.assertEqual(client.abort_calls, 1)

    def test_lost_append_response_uses_step_count_before_retry(self):
        client = LostAppendResponseClient(accepted=True)
        result = append_trace_replay_checked(
            client, {"index": 3}, retry_delay_s=0
        )
        self.assertTrue(result["inferred_after_lost_response"])
        self.assertEqual(client.append_calls, 1)

    def test_unaccepted_append_is_retried_at_same_index(self):
        client = LostAppendResponseClient(accepted=False)
        result = append_trace_replay_checked(
            client, {"index": 3}, retry_delay_s=0
        )
        self.assertTrue(result["ok"])
        self.assertEqual(client.append_calls, 2)

    def test_lost_start_response_is_not_blindly_retried_after_acceptance(self):
        client = LostStartResponseClient(accepted=True)
        result = start_trace_replay_checked(client, retry_delay_s=0)
        self.assertTrue(result["inferred_after_lost_response"])
        self.assertEqual(client.start_calls, 1)

    def test_lost_unaccepted_start_is_retried_after_loaded_state_check(self):
        client = LostStartResponseClient(accepted=False)
        result = start_trace_replay_checked(client, retry_delay_s=0)
        self.assertTrue(result["ok"])
        self.assertEqual(client.start_calls, 2)

    def test_ddmin_removes_irrelevant_blocks(self):
        items = ["address", "radio", "node_version", "reporting"]

        def succeeds(candidate):
            return "node_version" in candidate

        self.assertEqual(ddmin(items, succeeds), ["node_version"])

    def test_rf_node_id_is_resolved_from_live_eui(self):
        step = {
            "index": 4,
            "rf_node": {"source_node_id": 11, "eui64": "02AABBCCDDEE1007"},
            "body_suffix_hex": "5E303056657273696F6E0D",
        }
        self.assertEqual(
            resolve_pv_body(FakeReplayClient(), step),
            "00075E303056657273696F6E0D",
        )

    def test_address_assignment_uses_live_tap_identity(self):
        step = {
            "address_assignment": {
                "desired_target": "0x120A",
                "trailing_hex": "",
            }
        }
        status = {
            "gateway_id_hex": "0x1209",
            "gateway_long_addr": "02AABBCCDDEE0001",
        }
        self.assertEqual(
            resolve_simple_payload(step, status),
            "3724926602AABBCCDDEE0001120A",
        )

    def test_legacy_http_rejects_unmanaged_address_assignment(self):
        step = {
            "action": "simple_frame",
            "target": "0x0001",
            "type": 0x003C,
            "payload_hex": "3724926602AABBCCDDEE00011209",
            "expected_type": 0x003D,
            "expected_outcome": "response",
        }
        status = {
            "gateway_id_hex": "0x1209",
            "gateway_long_addr": "02AABBCCDDEE0001",
        }
        with self.assertRaisesRegex(RuntimeError, "0x003C"):
            execute_step(FakeReplayClient(), step, status)

    def test_temporary_gateway_id_survives_eui_aliasing(self):
        self.assertEqual(operational_target("TAP[02AABBCCDDEE0001]", 0x120A), "0x120A")

    def test_live_rf_outcome_checks_credit(self):
        step = {"action": "pv_subcommand", "subcmd": 0x06, "expected_outcome": "rf_response_credit_0D"}
        status = {
            "command_state": "pv subcommand TAP ack; RF unconfirmed",
            "last_pv_ack_rsp_subcmd_hex": "0x07",
            "last_pv_ack_status_hex": "0x0D",
            "last_pv_ack_body_hex": "4F4B",
        }
        self.assertEqual(classify_live_outcome(step, status), "rf_response_credit_0D")
        self.assertTrue(outcome_matches(step["expected_outcome"], classify_live_outcome(step, status)))

    def test_empty_node_response_is_not_classified_as_rf(self):
        step = {"action": "pv_subcommand", "subcmd": 0x06}
        status = {
            "command_state": "pv subcommand TAP ack; RF unconfirmed",
            "last_pv_ack_rsp_subcmd_hex": "0x07",
            "last_pv_ack_status_hex": "0x0D",
            "last_pv_ack_body_hex": "",
        }
        self.assertEqual(
            classify_live_outcome(step, status),
            "tap_ack_empty_node_response_credit_0D",
        )

    def test_read_only_and_active_rf_are_not_mutations(self):
        self.assertEqual(pv_command_risk(0x0D), "read_only")
        self.assertEqual(pv_command_risk(0x06), "active_rf")
        self.assertEqual(pv_command_risk(0x13), "state_changing")

    def test_plan_selection_preserves_internal_timing(self):
        plan = {"steps": [
            {"index": 1, "block": 0, "relative_offset_ms": 100.0},
            {"index": 2, "block": 1, "relative_offset_ms": 500.0},
            {"index": 3, "block": 1, "relative_offset_ms": 800.0},
        ]}
        selected = select_plan(plan, blocks={1})
        self.assertEqual([step["index"] for step in selected["steps"]], [2, 3])
        self.assertEqual([step["relative_offset_ms"] for step in selected["steps"]], [0.0, 300.0])

    def test_rf_credit_is_encoded_for_local_scheduler(self):
        self.assertEqual(
            firmware_replay_outcome({"expected_outcome": "rf_response_credit_0D"}),
            ("rf_response", 0x0D),
        )
        self.assertEqual(
            firmware_replay_outcome({
                "expected_outcome": "tap_ack_empty_node_response_credit_0E"
            }),
            ("tap_ack", 0x0E),
        )

    def test_local_scheduler_uses_dynamic_current_target_and_scaled_offset(self):
        step = {
            "index": 9,
            "block": 2,
            "relative_offset_ms": 585.0,
            "action": "pv_subcommand",
            "subcmd": 0x0D,
            "body_hex": "0000",
            "expected_response_subcmd": 0x0E,
            "expected_outcome": "tap_ack",
        }
        encoded = encode_firmware_replay_step(
            FakeReplayClient(), step, {"gateway_id_hex": "0x1209"},
            timing_scale=0.5, upload_index=3,
        )
        self.assertEqual(encoded["index"], 3)
        self.assertEqual(encoded["offset_ms"], 292)
        self.assertEqual(encoded["target"], "0xFFFF")
        self.assertEqual(encoded["expected_type"], "0x0B10")
        self.assertEqual(encoded["outcome"], "tap_ack")

    def test_full_trace_hold_reaches_source_release(self):
        plan = {
            "source_first_request_ms": 3529.0,
            "source_first_release_ms": 56787.0,
            "steps": [{"relative_offset_ms": 52318.0}],
        }
        self.assertEqual(
            replay_hold_offset_ms(plan, timing_scale=1.0, post_hold_ms=5000),
            53258,
        )


if __name__ == "__main__":
    unittest.main()
