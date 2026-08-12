#pragma once

// Board capability bitmask. A board's board_caps() returns the OR of the
// features it physically supports; the application queries it before using
// optional subsystems (e.g. skip Wi-Fi config UI on a wireless-less board).

#define BOARD_CAP_WIFI       (1u << 0)
#define BOARD_CAP_BLE        (1u << 1)
#define BOARD_CAP_MIC        (1u << 2)
#define BOARD_CAP_SD         (1u << 3)
#define BOARD_CAP_GRAYSCALE  (1u << 4)
#define BOARD_CAP_BATTERY    (1u << 5)
