#pragma once

// Finger direction: right from % opens token value; either horizontal swipe returns from value.
inline int usage_swipe_column(bool right, bool left, bool has_value, int column) {
  if (!has_value) return -1;
  if (column == 0 && right) return 1;
  if (column == 1 && (right || left)) return 0;
  return -1;
}
