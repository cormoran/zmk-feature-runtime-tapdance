# Studio BLE runtime tap dance configuration

The oracle first verifies the peripheral connection and position-state
subscription. The split central answers three requests over real Studio GATT:
read missing ID 0, set the global interval to 250 ms in RAM, then read missing
ID 0 again. The byte oracle checks the missing-ID error and the global response
with capacity 16, name limit 24 bytes, pool size 1024 bytes, and dirty state.
Module RPC writes suppress the generic settings notification so
only one reply is emitted per request; this avoids starving the reply while the
RPC workqueue is occupied by its own handler.

The native-only linker shim makes the selected ZMK baseline's mutable local-ID
map writable. It contributes no bytes or entries to the iterable and is enabled
only by this fixture. Bluetooth settings initialize before the dynamic ZMK BLE
profile handler is registered so that main's settings initialization cannot
clear that handler when using the simulator's SETTINGS_NONE backend.
