# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""CoreBluetooth GATT peripheral for Muse Gadget setup on macOS.

The same setup service and Transport as ``ble_server.BleServer`` (BlueZ), for
pairing from a Mac. Needs PyObjC's CoreBluetooth bindings
(``pip install pyobjc-framework-CoreBluetooth``) and Bluetooth permission
for the terminal app (System Settings > Privacy & Security > Bluetooth).

What macOS does differently from BlueZ:

- Apps can advertise only a local name and service UUIDs, not the
  manufacturer data the other gadgets carry (the "not paired yet" flag).
- Once connected, a phone reads the GAP name, which is the Mac's computer
  name rather than the advertised one. ``tools/dash_sim/mac_gadget.sh``
  renames the Mac for the setup window.
- A peripheral can't drop a connection, so ``disconnect`` only logs: the
  phone ends it, and when it unsubscribes from TX the session is cleared.

Runs an NSRunLoop on the calling thread; ``send_packets`` must be called from
another thread because it paces notifications.
"""

from __future__ import annotations

import collections
import logging
import queue
import signal
import threading
import time
from typing import Callable

import objc
from CoreBluetooth import (
    CBAdvertisementDataLocalNameKey,
    CBAdvertisementDataServiceUUIDsKey,
    CBATTErrorSuccess,
    CBAttributePermissionsReadable,
    CBAttributePermissionsWriteable,
    CBCharacteristicPropertyNotify,
    CBCharacteristicPropertyRead,
    CBCharacteristicPropertyWrite,
    CBCharacteristicPropertyWriteWithoutResponse,
    CBMutableCharacteristic,
    CBMutableService,
    CBPeripheralManager,
    CBUUID,
)
from Foundation import NSData, NSDate, NSDefaultRunLoopMode, NSObject, NSRunLoop

from musegadget.ble_framing import CHUNK_STAGGER_S, MAX_PACKET_BYTES

log = logging.getLogger(__name__)

# The same service as ble_server.py, which can't be imported here (dbus).
SERVICE_UUID = "7fdd3d1c-38ea-46cf-8b46-314ecf5f240c"
RX_UUID = "4d593029-28a2-4a6e-a1f0-3c2d5e8f9b01"
TX_UUID = "d75dc4ca-7b2b-4e9c-8f0a-1d2e3f4a5b6c"

_ASSUMED_MTU = MAX_PACKET_BYTES + 3
_POWERED_ON = 5  # CBManagerStatePoweredOn
_STATE_NAMES = {0: "unknown", 1: "resetting", 2: "unsupported", 3: "unauthorized",
                4: "powered off", 5: "powered on"}
_TICK_S = 0.02


class _Delegate(NSObject):
    """CBPeripheralManagerDelegate; forwards to the owning server."""

    def initWithServer_(self, server):
        self = objc.super(_Delegate, self).init()
        if self is None:
            return None
        self.server = server
        return self

    def peripheralManagerDidUpdateState_(self, manager):
        self.server._state_changed(manager.state())

    def peripheralManager_didAddService_error_(self, manager, service, error):
        if error:
            self.server._fatal("GATT registration failed", error)
            return
        log.info("GATT service registered")
        manager.startAdvertising_({
            CBAdvertisementDataLocalNameKey: self.server._local_name,
            CBAdvertisementDataServiceUUIDsKey: [CBUUID.UUIDWithString_(SERVICE_UUID)],
        })

    def peripheralManagerDidStartAdvertising_error_(self, manager, error):
        if error:
            self.server._fatal("advertising failed", error)
        else:
            log.info("advertising as %s", self.server._local_name)

    def peripheralManager_central_didSubscribeToCharacteristic_(self, manager, central, chrc):
        log.info("client subscribed to %s", chrc.UUID().UUIDString())
        self.server._subscribed(central)

    def peripheralManager_central_didUnsubscribeFromCharacteristic_(self, manager, central, chrc):
        log.info("client unsubscribed (disconnected)")
        self.server._unsubscribed(central)

    def peripheralManager_didReceiveReadRequest_(self, manager, request):
        request.setValue_(self.server._last_value())
        manager.respondToRequest_withResult_(request, CBATTErrorSuccess)

    def peripheralManager_didReceiveWriteRequests_(self, manager, requests):
        for request in requests:
            self.server._written(request.central(), bytes(request.value() or b""))
        # One response covers the whole batch; writes without response ignore it.
        if requests:
            manager.respondToRequest_withResult_(requests[0], CBATTErrorSuccess)

    def peripheralManagerIsReadyToUpdateSubscribers_(self, manager):
        self.server._flush()


class BleServer:
    """Owns the Mac's Bluetooth peripheral role while setup is open."""

    def __init__(
        self,
        local_name: str,
        on_write: Callable[[bytes], None],
        on_disconnect: Callable[[], None],
    ) -> None:
        self._local_name = local_name
        self._on_write = on_write
        self._on_disconnect = on_disconnect
        self._mtu = _ASSUMED_MTU
        self._central_id: str | None = None
        self._lock = threading.Lock()
        self._actions: queue.Queue = queue.Queue()
        self._pending: collections.deque = collections.deque()
        self._value = b""
        self._running = False
        self._manager = None
        self._delegate = None
        self._tx = None

    # -- Transport --------------------------------------------------------------

    def mtu(self) -> int:
        with self._lock:
            return self._mtu

    def send_packets(self, packets: list[bytes]) -> None:
        for i, packet in enumerate(packets):
            if i:
                time.sleep(CHUNK_STAGGER_S)
            self._actions.put(lambda p=packet: self._notify(p))

    def disconnect(self, delay: float) -> None:
        log.info("macOS can't drop the connection; waiting for the phone to close it")

    # -- Lifecycle --------------------------------------------------------------

    def run(self) -> None:
        """Start advertising and serve until :meth:`stop`. Blocks."""
        self._delegate = _Delegate.alloc().initWithServer_(self)
        self._tx = CBMutableCharacteristic.alloc().initWithType_properties_value_permissions_(
            CBUUID.UUIDWithString_(TX_UUID),
            CBCharacteristicPropertyRead | CBCharacteristicPropertyNotify,
            None, CBAttributePermissionsReadable,
        )
        rx = CBMutableCharacteristic.alloc().initWithType_properties_value_permissions_(
            CBUUID.UUIDWithString_(RX_UUID),
            CBCharacteristicPropertyWrite | CBCharacteristicPropertyWriteWithoutResponse,
            None, CBAttributePermissionsWriteable,
        )
        self._service = CBMutableService.alloc().initWithType_primary_(
            CBUUID.UUIDWithString_(SERVICE_UUID), True)
        self._service.setCharacteristics_([rx, self._tx])
        # A nil queue delivers delegate calls on the main run loop, pumped below.
        self._manager = CBPeripheralManager.alloc().initWithDelegate_queue_(self._delegate, None)

        self._running = True
        previous = {s: signal.signal(s, lambda *_: self.stop()) for s in (signal.SIGTERM, signal.SIGINT)}
        loop = NSRunLoop.currentRunLoop()
        try:
            while self._running:
                loop.runMode_beforeDate_(NSDefaultRunLoopMode,
                                         NSDate.dateWithTimeIntervalSinceNow_(_TICK_S))
                while True:
                    try:
                        self._actions.get_nowait()()
                    except queue.Empty:
                        break
        finally:
            for s, handler in previous.items():
                signal.signal(s, handler)
            self._teardown()

    def stop(self) -> None:
        self._running = False

    # -- Internals (run loop thread) --------------------------------------------

    def _state_changed(self, state: int) -> None:
        log.info("Bluetooth %s", _STATE_NAMES.get(state, state))
        if state == _POWERED_ON:
            self._manager.removeAllServices()
            self._manager.addService_(self._service)
        elif state == 3:
            self._fatal("Bluetooth access denied",
                        "allow your terminal in System Settings > Privacy & Security > Bluetooth")
        elif state == 2:
            self._fatal("Bluetooth LE peripheral mode unsupported", "on this Mac")

    def _track(self, central) -> None:
        mtu = int(central.maximumUpdateValueLength()) + 3
        with self._lock:
            self._central_id = str(central.identifier().UUIDString())
            if mtu != self._mtu:
                self._mtu = mtu
                log.info("ATT MTU %d", mtu)

    def _subscribed(self, central) -> None:
        self._track(central)

    def _unsubscribed(self, central) -> None:
        with self._lock:
            ours = self._central_id in (None, str(central.identifier().UUIDString()))
            if ours:
                self._central_id = None
                self._mtu = _ASSUMED_MTU
        self._pending.clear()
        if ours:
            self._on_disconnect()

    def _written(self, central, value: bytes) -> None:
        if central is not None:
            self._track(central)
        self._on_write(value)

    def _last_value(self):
        return NSData.dataWithBytes_length_(self._value, len(self._value))

    def _notify(self, packet: bytes) -> None:
        self._value = packet
        self._pending.append(packet)
        self._flush()

    def _flush(self) -> None:
        # updateValue returns NO while the transmit queue is full; the rest
        # goes out from peripheralManagerIsReadyToUpdateSubscribers_.
        while self._pending:
            data = NSData.dataWithBytes_length_(self._pending[0], len(self._pending[0]))
            if not self._manager.updateValue_forCharacteristic_onSubscribedCentrals_(
                    data, self._tx, None):
                return
            self._pending.popleft()

    def _fatal(self, what: str, err) -> None:
        log.error("%s: %s", what, err)
        self.stop()

    def _teardown(self) -> None:
        if self._manager is not None:
            self._manager.stopAdvertising()
            self._manager.removeAllServices()
