# Studio BLE runtime tap dance configuration

The oracle first verifies the peripheral connection and position-state
subscription. The split central answers three requests over real Studio GATT: read empty
slot 0 (interval 200 ms), set interval to 250 ms in RAM, read slot 0 again
(interval 250 ms). The byte oracle checks both empty binding submessages and
capacity 16. Module RPC writes suppress the generic settings notification so
only one reply is emitted per request; this avoids starving the reply while the
RPC workqueue is occupied by its own handler.

The native-only linker shim makes the selected ZMK baseline's mutable local-ID
map writable. It contributes no bytes or entries to the iterable and is enabled
only by this fixture. Bluetooth settings initialize before the dynamic ZMK BLE
profile handler is registered so that main's settings initialization cannot
clear that handler when using the simulator's SETTINGS_NONE backend.
