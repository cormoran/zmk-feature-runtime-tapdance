#!/usr/bin/env python3
"""Hardware-free functional test: boot this module's firmware as a wired split
pair in Renode and exercise its own custom Studio RPC over the central's
emulated USB CDC. This is the one test file a module built from this template
rewrites for its own RPC surface; the generic checks (both halves booting, the
wired split link, a core GetDeviceInfo round trip over USB) already ran in the
action's smoke step.

Run by `west zmk-renode-test tests/renode --mode wired-split --elf <CENTRAL>
--peripheral-elf <PERIPHERAL>`, which sets the `ZMK_RENODE_*` env contract (see
zmk-west-commands' docs/renode-testing.md, "Module-test env contract"):
  ZMK_RENODE_MODE           = wired-split
  ZMK_RENODE_ELF            = the split CENTRAL ELF
  ZMK_RENODE_PERIPHERAL_ELF = the split PERIPHERAL ELF
  ZMK_RENODE_STORAGE_ADDR / _SIZE = the central's NVS storage_partition overrides
and puts `renode_harness` (zmk-west-commands' scripts/lib/renode) on PYTHONPATH.

(Named `renode_test.py`, not `test_renode.py`, so it stays out of
`python3 -m unittest`'s `test*.py` auto-discovery -- it needs real ELFs.)
"""

from __future__ import annotations

import os
import sys
import subprocess
import socket
import threading
from collections import deque
import time
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# renode_harness comes from the zmk-west-commands checkout the command puts on
# PYTHONPATH. Support running this file directly too by falling back to
# conventional relative locations: first the zmk-west-commands west dependency
# this repo has (west/west-dependency/west-test-dependency.yml -- nicer than
# requiring a sibling checkout, since `west update` already fetches it), then a
# sibling `zmk-west-commands` checkout next to this repo.
try:
    import renode_harness
except ImportError:  # pragma: no cover - convenience fallback for local dev
    fallback_candidates = [
        REPO_ROOT / "dependencies" / "zmk-west-commands" / "scripts" / "lib" / "renode",
        REPO_ROOT.parent / "zmk-west-commands" / "scripts" / "lib" / "renode",
    ]
    for fallback in fallback_candidates:
        if fallback.is_dir():
            sys.path.insert(0, str(fallback))
            import renode_harness

            break
    else:
        raise


SUBSYSTEM_IDENTIFIER = "cormoran__runtime_tapdance"
# This template registers exactly one custom subsystem, so its index is
# deterministically 0.
KNOWN_SUBSYSTEM_INDEX = 0
# Always out of range regardless of how many custom subsystems a given module
# registers -- used to exercise the fast-path dispatch (see
# test_custom_rpc_invalid_index_dispatch).
INVALID_SUBSYSTEM_INDEX = 99

# attach_dual_cdc_bridge's default bridge name -> monitor object prefix.
BRIDGE_NAME = "bridge"


def _mon_is_true(mon, command: str) -> bool:
    return "True" in mon.execute(command, settle=0.3)


class RenodeWiredSplitModuleTests(unittest.TestCase):
    """Boots the module's own wired-split pair once for the whole class (boot is
    the slow part) and exercises the custom subsystem over the central's USB
    CDC. (The wired split link itself is covered by the action's built-in
    smoke; see the note near the bottom of this class about the split relay.)"""

    renode_path: str
    studio_pb2 = None
    template_pb2 = None

    @classmethod
    def setUpClass(cls):
        cls.renode_path = renode_harness.find_or_install_renode()
        if cls.renode_path is None:
            raise unittest.SkipTest(
                "Renode is not installed and could not be auto-installed"
            )

        # Env contract (see docs/renode-testing.md "Module-test env contract").
        mode = os.environ.get("ZMK_RENODE_MODE", "wired-split")
        if mode != "wired-split":
            raise unittest.SkipTest(
                f"ZMK_RENODE_MODE={mode!r}: this template's Renode test targets "
                "wired-split mode -- run `west zmk-renode-test tests/renode "
                "--mode wired-split --elf build/usb_wired_central/zephyr/zmk.elf "
                "--peripheral-elf build/usb_wired_peripheral/zephyr/zmk.elf`"
            )

        central_env = os.environ.get("ZMK_RENODE_ELF")
        peripheral_env = os.environ.get("ZMK_RENODE_PERIPHERAL_ELF")
        if not central_env or not peripheral_env:
            raise unittest.SkipTest(
                "ZMK_RENODE_ELF / ZMK_RENODE_PERIPHERAL_ELF not set -- build the "
                "wired-split pair first (see README.md)"
            )
        cls.central_elf = Path(central_env)
        cls.peripheral_elf = Path(peripheral_env)
        for elf in (cls.central_elf, cls.peripheral_elf):
            if not elf.is_file():
                raise AssertionError(f"Configured ELF does not exist: {elf}")

        storage_addr = int(
            os.environ.get("ZMK_RENODE_STORAGE_ADDR")
            or hex(renode_harness.STORAGE_ADDR_DEFAULT),
            0,
        )
        storage_size = int(
            os.environ.get("ZMK_RENODE_STORAGE_SIZE")
            or hex(renode_harness.STORAGE_SIZE_DEFAULT),
            0,
        )

        # Core zmk.studio.* messages (Request/Response envelope, core.proto,
        # custom.proto for the generic custom-subsystem envelope).
        # The module may be a shared-profile worktree rather than the West root.
        try:
            messages = subprocess.check_output(
                ["west", "list", "zmk-studio-messages", "-f", "{abspath}"],
                cwd=REPO_ROOT,
                text=True,
            ).strip()
            studio_proto_dir = Path(messages) / "proto" / "zmk"
        except (FileNotFoundError, subprocess.CalledProcessError):
            studio_proto_dir = renode_harness.find_studio_proto_dir(REPO_ROOT)
        cls.studio_pb2 = renode_harness.load_studio_pb2(studio_proto_dir)

        # This module's own proto (package cormoran.runtime_tapdance) -- protoc
        # normalizes the hyphenated "runtime-tapdance" path to the "runtime_tapdance" package.
        out_dir = renode_harness.compile_protos(
            [
                REPO_ROOT
                / "proto"
                / "cormoran"
                / "runtime-tapdance"
                / "runtime_tapdance.proto"
            ],
            include_dirs=[REPO_ROOT / "proto"],
        )
        sys.path.insert(0, str(out_dir))
        import cormoran.runtime_tapdance.runtime_tapdance_pb2 as template_pb2  # type: ignore

        cls.template_pb2 = template_pb2

        # Boot the pair and attach the DualCdcAcmBridge USB host to reach the
        # central's Studio CDC (the same steps run_usb_wired_smoke uses).
        import random

        cls.port_base = random.randint(26000, 40000)
        (
            cls.session,
            cls.central_console,
            cls.peripheral_console,
        ) = renode_harness.boot_usb_wired_split(
            cls.renode_path,
            central_elf=cls.central_elf,
            peripheral_elf=cls.peripheral_elf,
            storage_addr=storage_addr,
            storage_size=storage_size,
            port_base=cls.port_base,
        )
        cls.addClassCleanup(cls.session.stop)
        cls.addClassCleanup(cls.central_console.close)
        cls.addClassCleanup(cls.peripheral_console.close)

        banner = renode_harness.wait_for_text(
            cls.central_console._sock, "Welcome to ZMK", timeout=20
        )
        if "Welcome to ZMK" not in banner:
            raise AssertionError(
                f"central never saw the ZMK boot banner on uart0; got:\n{banner}"
            )

        # Let the guest finish USB bring-up before the host attaches (a SETUP
        # fired before the guest's INTEN is set is silently lost).
        # Unread UART logs can backpressure Renode's terminal and stall the
        # firmware logger. Keep every non-Studio socket draining during RPC.
        cls.console_log = deque(maxlen=3000)
        cls.peripheral_log = deque(maxlen=3000)

        def drain_console(stream, captured):
            stream.settimeout(0.5)
            while True:
                try:
                    chunk = stream.recv(4096)
                    if not chunk:
                        return
                    captured.append(chunk.decode(errors="replace"))
                except socket.timeout:
                    continue
                except OSError:
                    return

        for stream, captured in (
            (cls.central_console._sock, cls.console_log),
            (cls.peripheral_console._sock, cls.peripheral_log),
        ):
            threading.Thread(
                target=drain_console, args=(stream, captured), daemon=True
            ).start()
        time.sleep(8.0)

        cdc0, cdc1 = renode_harness.attach_dual_cdc_bridge(
            cls.session, cls.port_base + 4, cls.port_base + 5
        )
        cls.addClassCleanup(cdc0.close)
        cls.addClassCleanup(cdc1.close)

        mon = cls.session.mon
        wiring_deadline = time.monotonic() + 30.0
        while time.monotonic() < wiring_deadline:
            if _mon_is_true(mon, f"sysbus.{BRIDGE_NAME}_cdc0 IsWired"):
                break
        else:
            raise AssertionError(
                f"USB enumeration never wired the first CDC channel "
                f"(no sysbus.{BRIDGE_NAME}_cdc0 IsWired within 30s)"
            )
        # Console stays on uart0 here, so USB is normally a single Studio CDC;
        # auto-detect anyway (a build that also put console on USB would
        # enumerate console first, Studio second).
        dual_cdc = _mon_is_true(mon, f"sysbus.{BRIDGE_NAME}_cdc1 IsWired")
        time.sleep(2.0)
        cls.studio = cdc1 if dual_cdc else cdc0

    def _send_call(self, subsystem_index: int, payload: bytes, request_id: int = 1):
        req = self.studio_pb2.Request()
        req.request_id = request_id
        req.custom.call.subsystem_index = subsystem_index
        req.custom.call.payload = payload
        self.studio.send(req.SerializeToString())

    def _read_response(self, timeout: float = 10.0):
        resp_bytes = self.studio.read_frame(timeout=timeout)
        if resp_bytes is None:
            console = "".join(self.console_log)
            pc = self.session.mon.execute("sysbus.cpu PC", settle=0.2)
            self.fail(
                f"no Studio RPC response frame (timeout); CPU={pc}; console:\n{console}"
            )
        resp = self.studio_pb2.Response()
        resp.ParseFromString(resp_bytes)
        return resp

    # -- Affirmative proof the custom-subsystem envelope works ---------------

    def test_custom_rpc_invalid_index_dispatch(self):
        """`custom.call` to a subsystem index that doesn't exist proves the
        whole custom-subsystem envelope round-trips correctly end to end
        (Request.custom oneof selection, CallRequest field encoding,
        subsystem-count/index validation, meta.simple_error response) -- the
        fast, callback-free path."""
        self._send_call(INVALID_SUBSYSTEM_INDEX, b"", request_id=7)
        resp = self._read_response()
        self.assertEqual(resp.WhichOneof("type"), "request_response")
        self.assertEqual(resp.request_response.request_id, 7)
        self.assertEqual(resp.request_response.WhichOneof("subsystem"), "meta")
        self.assertEqual(
            resp.request_response.meta.WhichOneof("response_type"), "simple_error"
        )
        # zmk.meta.ErrorConditions.RPC_NOT_FOUND == 2
        self.assertEqual(resp.request_response.meta.simple_error, 2)

    # -- The real thing: this module's own custom RPC, over USB --------------

    def _module_call(self, request, request_id=10):
        self._send_call(KNOWN_SUBSYSTEM_INDEX, request.SerializeToString(), request_id)
        envelope = self._read_response()
        self.assertEqual(envelope.request_response.request_id, request_id)
        self.assertEqual(envelope.request_response.WhichOneof("subsystem"), "custom")
        result = self.template_pb2.Response()
        result.ParseFromString(envelope.request_response.custom.call.payload)
        return result

    def _reboot_central_preserving_flash(self):
        """Reset the existing machine, retaining its mapped flash/NVS contents."""
        mon = self.session.mon
        self.console_log.clear()
        mon.execute('mach set "central"')
        mon.execute("pause")
        mon.execute("machine Reset")
        # The UF2 application starts at this vector-table offset, as on first boot.
        mon.execute("sysbus.cpu VectorTableOffset 0x27000")
        mon.execute("start")
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline and "Welcome to ZMK" not in "".join(
            self.console_log
        ):
            time.sleep(0.1)
        self.assertIn("Welcome to ZMK", "".join(self.console_log))
        time.sleep(8)
        cdc0, cdc1 = renode_harness.attach_dual_cdc_bridge(
            self.session,
            self.port_base + 6,
            self.port_base + 7,
            name="reboot_bridge",
            machines=["central", "peripheral"],
        )
        self.addClassCleanup(cdc0.close)
        self.addClassCleanup(cdc1.close)
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            if _mon_is_true(mon, "sysbus.reboot_bridge_cdc0 IsWired"):
                break
        else:
            self.fail("Studio USB CDC did not re-enumerate after reset")
        dual_cdc = _mon_is_true(mon, "sysbus.reboot_bridge_cdc1 IsWired")
        time.sleep(2)
        # The next test method shares this reset machine and the new host bridge.
        type(self).studio = cdc1 if dual_cdc else cdc0

    def test_runtime_configuration_crud_over_usb(self):
        """Round-trip slot config and globals, including real validation errors."""
        proto = self.template_pb2
        initial = self._module_call(proto.Request(get=proto.GetRequest(index=0)))
        self.assertEqual(initial.WhichOneof("response_type"), "state")
        self.assertEqual(initial.state.max_count, 16)
        self.assertTrue(initial.state.HasField("config"))
        self.assertTrue(initial.state.config.HasField("double_binding"))
        self.assertTrue(initial.state.config.HasField("triple_binding"))

        interval = self._module_call(
            proto.Request(
                set_interval=proto.SetIntervalRequest(interval_ms=250, persist=True)
            )
        )
        self.assertEqual(interval.state.interval_ms, 250)
        config = proto.Config(
            enabled=True,
            position=0,
            delay_single=True,
            double_binding=proto.Binding(),
            triple_binding=proto.Binding(),
        )
        written = self._module_call(
            proto.Request(set=proto.SetRequest(index=0, config=config, persist=True))
        )
        self.assertEqual(written.state.config, config)
        reread = self._module_call(proto.Request(get=proto.GetRequest(index=0)))
        self.assertEqual(reread.state.config, config)
        self.assertEqual(reread.state.interval_ms, 250)
        # Overwrite RAM only so the post-reset oracle cannot pass by reading
        # the same in-memory value: flash must restore both independent settings.
        self._module_call(
            proto.Request(
                set=proto.SetRequest(
                    index=0,
                    config=proto.Config(enabled=False, position=1, delay_single=False),
                )
            )
        )
        self._module_call(
            proto.Request(set_interval=proto.SetIntervalRequest(interval_ms=300))
        )
        self._reboot_central_preserving_flash()
        restored = self._module_call(proto.Request(get=proto.GetRequest(index=0)))
        self.assertEqual(restored.state.config, config)
        self.assertEqual(restored.state.interval_ms, 250)

        duplicate = self._module_call(
            proto.Request(set=proto.SetRequest(index=1, config=config))
        )
        self.assertEqual(duplicate.WhichOneof("response_type"), "error")
        self.assertLess(duplicate.error.code, 0)
        removed = self._module_call(
            proto.Request(remove=proto.RemoveRequest(index=0, persist=True))
        )
        self.assertFalse(removed.state.config.enabled)
        self.assertEqual(removed.state.config.double_binding.behavior_id, 0)
        self.assertEqual(removed.state.config.triple_binding.behavior_id, 0)
        self._module_call(
            proto.Request(set_interval=proto.SetIntervalRequest(interval_ms=200))
        )

    def test_runtime_rpc_invalid_requests(self):
        proto = self.template_pb2
        bad_requests = [
            proto.Request(),
            proto.Request(get=proto.GetRequest(index=16)),
            proto.Request(set=proto.SetRequest(index=0)),
            proto.Request(set_interval=proto.SetIntervalRequest(interval_ms=0)),
            proto.Request(set_interval=proto.SetIntervalRequest(interval_ms=2001)),
            proto.Request(
                set=proto.SetRequest(
                    index=0,
                    config=proto.Config(
                        enabled=True,
                        position=0,
                        double_binding=proto.Binding(behavior_id=0xFFFFFFFF),
                    ),
                )
            ),
        ]
        for request in bad_requests:
            with self.subTest(request=request):
                result = self._module_call(request)
                self.assertEqual(result.WhichOneof("response_type"), "error")
                self.assertLess(result.error.code, 0)
                self.assertTrue(result.error.message)


if __name__ == "__main__":
    unittest.main()
