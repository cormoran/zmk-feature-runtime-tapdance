/* SPDX-License-Identifier: MIT */
/* The named ZMK baseline declares mutable local-ID entries const. Native
 * linking then places their iterable output in a read-only segment, although
 * settings/CRC initialization writes local_id. This empty writable input makes
 * that output writable without adding bytes or an invalid iterable element.
 * Only the native BLE fixture enables this source; firmware is unaffected. */
.section ._zmk_behavior_local_id_map.static.runtime_tapdance_native_writable,"aw",@progbits
.section .note.GNU-stack,"",@progbits
