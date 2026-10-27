#pragma once

// 화면 위 56px 에서 시작한 세로 쓸기만 빠른 설정으로 쓴다. 기존 버튼의 탭은 그대로 둔다.
enum class QuickSwipeAction { None, Open, Close };
class QuickSwipe {
 public:
  QuickSwipeAction sample(int x, int y, bool down, bool open) {
    if (!down) { reset(); return QuickSwipeAction::None; }
    if (!pressed_) {
      pressed_ = true;
      start_x_ = x; start_y_ = y;
      was_open_ = open;
      eligible_ = open || (y >= 0 && y < 56);
      return QuickSwipeAction::None;
    }
    if (!eligible_ || consumed_) return QuickSwipeAction::None;
    int dx = x - start_x_, dy = y - start_y_;
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    if (ax > 32 && ax > ay) eligible_ = false;
    if (eligible_ && ay >= 24 && ay > ax * 3 / 2 && (was_open_ ? dy < 0 : dy > 0)) {
      consumed_ = true;
      return was_open_ ? QuickSwipeAction::Close : QuickSwipeAction::Open;
    }
    return QuickSwipeAction::None;
  }
  bool consumed() const { return consumed_; }
  void reset() { pressed_ = eligible_ = consumed_ = was_open_ = false; }
 private:
  int start_x_ = 0, start_y_ = 0;
  bool pressed_ = false, eligible_ = false, consumed_ = false, was_open_ = false;
};
