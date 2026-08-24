/**
 * @file        input/mnk/mnk_input_driver.cpp
 * @brief       Keyboard/mouse input driver implementation.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/input/mnk/mnk_input_driver.h>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#if REX_PLATFORM_WIN32
#include <Windows.h>
#endif

#include <SDL3/SDL_hints.h>

REXCVAR_DEFINE_BOOL(mnk_mode, false, "Input", "Enable keyboard/mouse controller emulation");
REXCVAR_DEFINE_INT32(mnk_user_index, 0, "Input", "Controller slot (0-3) for MnK").range(0, 3);
REXCVAR_DEFINE_INT32(ghwor_keyboard_instrument, 0, "Input/GHWoR Keyboard",
                     "Keyboard instrument: 0=Guitar, 1=Drums").range(0, 1);

REXCVAR_DEFINE_INT32(ghwor_key_guitar_green_vk, 77, "Input/GHWoR Keyboard", "Guitar Green VK").range(0,255);
REXCVAR_DEFINE_INT32(ghwor_key_guitar_red_vk, 188, "Input/GHWoR Keyboard", "Guitar Red VK").range(0,255);
REXCVAR_DEFINE_INT32(ghwor_key_guitar_yellow_vk, 190, "Input/GHWoR Keyboard", "Guitar Yellow VK").range(0,255);
REXCVAR_DEFINE_INT32(ghwor_key_guitar_blue_vk, 191, "Input/GHWoR Keyboard", "Guitar Blue VK").range(0,255);
REXCVAR_DEFINE_INT32(ghwor_key_guitar_orange_vk, 161, "Input/GHWoR Keyboard", "Guitar Orange VK").range(0,255);
REXCVAR_DEFINE_INT32(ghwor_key_guitar_strum_up_vk, 83, "Input/GHWoR Keyboard", "Guitar Strum Up VK").range(0,255);
REXCVAR_DEFINE_INT32(ghwor_key_guitar_strum_down_vk, 88, "Input/GHWoR Keyboard", "Guitar Strum Down VK").range(0,255);
REXCVAR_DEFINE_INT32(ghwor_key_guitar_whammy_vk, 81, "Input/GHWoR Keyboard", "Guitar Whammy VK").range(0,255);
REXCVAR_DEFINE_INT32(ghwor_key_guitar_star_vk, 32, "Input/GHWoR Keyboard", "Guitar Star Power VK").range(0,255);
REXCVAR_DEFINE_INT32(ghwor_key_guitar_start_vk, 13, "Input/GHWoR Keyboard", "Guitar Start VK").range(0,255);
REXCVAR_DEFINE_INT32(ghwor_key_guitar_back_vk, 8, "Input/GHWoR Keyboard", "Guitar Back VK").range(0,255);
REXCVAR_DEFINE_INT32(ghwor_key_guitar_left_vk, 37, "Input/GHWoR Keyboard", "Guitar Menu Left VK").range(0,255);
REXCVAR_DEFINE_INT32(ghwor_key_guitar_right_vk, 39, "Input/GHWoR Keyboard", "Guitar Menu Right VK").range(0,255);

REXCVAR_DEFINE_INT32(ghwor_key_drum_green_vk, 112, "Input/GHWoR Keyboard", "Drum Green VK").range(0,255);
REXCVAR_DEFINE_INT32(ghwor_key_drum_red_vk, 113, "Input/GHWoR Keyboard", "Drum Red VK").range(0,255);
REXCVAR_DEFINE_INT32(ghwor_key_drum_yellow_vk, 114, "Input/GHWoR Keyboard", "Drum Yellow VK").range(0,255);
REXCVAR_DEFINE_INT32(ghwor_key_drum_blue_vk, 115, "Input/GHWoR Keyboard", "Drum Blue VK").range(0,255);
REXCVAR_DEFINE_INT32(ghwor_key_drum_orange_vk, 116, "Input/GHWoR Keyboard", "Drum Orange VK").range(0,255);
REXCVAR_DEFINE_INT32(ghwor_key_drum_kick_vk, 32, "Input/GHWoR Keyboard", "Drum Kick VK").range(0,255);
REXCVAR_DEFINE_INT32(ghwor_key_drum_up_vk, 38, "Input/GHWoR Keyboard", "Drum Menu Up VK").range(0,255);
REXCVAR_DEFINE_INT32(ghwor_key_drum_down_vk, 40, "Input/GHWoR Keyboard", "Drum Menu Down VK").range(0,255);
REXCVAR_DEFINE_INT32(ghwor_key_drum_left_vk, 37, "Input/GHWoR Keyboard", "Drum Menu Left VK").range(0,255);
REXCVAR_DEFINE_INT32(ghwor_key_drum_right_vk, 39, "Input/GHWoR Keyboard", "Drum Menu Right VK").range(0,255);
REXCVAR_DEFINE_INT32(ghwor_key_drum_start_vk, 13, "Input/GHWoR Keyboard", "Drum Start VK").range(0,255);
REXCVAR_DEFINE_INT32(ghwor_key_drum_back_vk, 8, "Input/GHWoR Keyboard", "Drum Back VK").range(0,255);
REXCVAR_DEFINE_DOUBLE(mnk_sensitivity, 1.0, "Input", "Mouse sensitivity for right stick")
    .range(0.01, 10.0);

REXCVAR_DEFINE_STRING(keybind_a, "Space", "Input/Keybinds/Controller", "A button");
REXCVAR_DEFINE_STRING(keybind_b, "Shift", "Input/Keybinds/Controller", "B button");
REXCVAR_DEFINE_STRING(keybind_x, "R", "Input/Keybinds/Controller", "X button");
REXCVAR_DEFINE_STRING(keybind_y, "E", "Input/Keybinds/Controller", "Y button");
REXCVAR_DEFINE_STRING(keybind_left_trigger, "RMB", "Input/Keybinds/Controller", "Left trigger");
REXCVAR_DEFINE_STRING(keybind_right_trigger, "LMB", "Input/Keybinds/Controller", "Right trigger");
REXCVAR_DEFINE_STRING(keybind_left_shoulder, "Q", "Input/Keybinds/Controller", "Left shoulder");
REXCVAR_DEFINE_STRING(keybind_right_shoulder, "F", "Input/Keybinds/Controller", "Right shoulder");
REXCVAR_DEFINE_STRING(keybind_lstick_up, "W", "Input/Keybinds/Controller", "Left stick up");
REXCVAR_DEFINE_STRING(keybind_lstick_down, "S", "Input/Keybinds/Controller", "Left stick down");
REXCVAR_DEFINE_STRING(keybind_lstick_left, "A", "Input/Keybinds/Controller", "Left stick left");
REXCVAR_DEFINE_STRING(keybind_lstick_right, "D", "Input/Keybinds/Controller", "Left stick right");
REXCVAR_DEFINE_STRING(keybind_lstick_press, "C", "Input/Keybinds/Controller", "Left stick press");
REXCVAR_DEFINE_STRING(keybind_rstick_press, "MMB", "Input/Keybinds/Controller",
                      "Right stick press");
REXCVAR_DEFINE_STRING(keybind_dpad_up, "Up", "Input/Keybinds/Controller", "D-pad up");
REXCVAR_DEFINE_STRING(keybind_dpad_down, "Down", "Input/Keybinds/Controller", "D-pad down");
REXCVAR_DEFINE_STRING(keybind_dpad_left, "Left", "Input/Keybinds/Controller", "D-pad left");
REXCVAR_DEFINE_STRING(keybind_dpad_right, "Right", "Input/Keybinds/Controller", "D-pad right");
REXCVAR_DEFINE_STRING(keybind_back, "Tab", "Input/Keybinds/Controller", "Back button");
REXCVAR_DEFINE_STRING(keybind_start, "Escape", "Input/Keybinds/Controller", "Start button");
REXCVAR_DEFINE_STRING(keybind_guide, "", "Input/Keybinds/Controller", "Guide button");

namespace rex::input::mnk {

using rex::ui::VirtualKey;

MnkInputDriver::MnkInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order) {}

MnkInputDriver::~MnkInputDriver() {
  // Detach handled by OnClosing; if window outlives the driver, clean up here.
  if (attached_window_) {
    attached_window_->RemoveInputListener(this);
    attached_window_->RemoveListener(this);
    attached_window_ = nullptr;
  }
}

X_STATUS MnkInputDriver::Setup() {
  const bool raw_keyboard_set =
      SDL_SetHint(SDL_HINT_WINDOWS_RAW_KEYBOARD, "1");
  const char* raw_keyboard_value =
      SDL_GetHint(SDL_HINT_WINDOWS_RAW_KEYBOARD);

  REXLOG_INFO("MnK input driver initialized");
  REXLOG_INFO(
      "GHWOR KEYBOARD CONFIG v10 ACTIVE: instrument={} guitar=0x06 drums5=0x08 rb_orange=1 lb_kick=1 velocity_axes=1 raw=1 physical_xinput=untouched",
      REXCVAR_GET(ghwor_keyboard_instrument));
  REXLOG_INFO(
      "GHWOR KEYBOARD RAW STATUS: set_ok={} value={}",
      raw_keyboard_set ? 1 : 0,
      raw_keyboard_value ? raw_keyboard_value : "<null>");
  return X_STATUS_SUCCESS;
}

void MnkInputDriver::OnWindowAvailable(rex::ui::Window* window) {
  if (window) {
    attached_window_ = window;
    window->AddInputListener(this, window_z_order());
    window->AddListener(this);

    // GHWoR v3: the game window is already the active input target when the
    // driver is attached. OnLostFocus/OnGotFocus keep this updated afterwards.
    {
      std::lock_guard lock(state_mutex_);
      has_focus_ = true;
    }
    REXLOG_INFO("GHWOR KEYBOARD GUITAR v3 WINDOW ATTACHED: focus_bootstrap=1");
  }
}

void MnkInputDriver::OnClosing(rex::ui::UIEvent&) {
  if (attached_window_) {
    if (mouse_captured_) {
      mouse_captured_ = false;
      attached_window_->SetCursorVisibility(precapture_cursor_visibility_);
      attached_window_->ReleaseMouse();
    }
    attached_window_->RemoveInputListener(this);
    attached_window_->RemoveListener(this);
    attached_window_ = nullptr;
  }
}

uint32_t MnkInputDriver::UserIndex() const {
  return static_cast<uint32_t>(REXCVAR_GET(mnk_user_index));
}

bool MnkInputDriver::IsEnabled() const {
  return REXCVAR_GET(mnk_mode);
}

static bool IsBindPressed(const bool (&key_down)[256], const std::string& cvar_val) {
  VirtualKey vk = rex::ui::ParseVirtualKey(cvar_val);
  if (vk == VirtualKey::kNone)
    return false;
  uint16_t idx = static_cast<uint16_t>(vk);
  return idx < 256 && key_down[idx];
}

X_RESULT MnkInputDriver::GetCapabilities(uint32_t user_index, uint32_t flags,
                                         X_INPUT_CAPABILITIES* out_caps) {
  if (!IsEnabled() || user_index != UserIndex()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (out_caps) {
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = 0x01;
    const bool ghwor_caps_drums =
        REXCVAR_GET(ghwor_keyboard_instrument) == 1;
    out_caps->sub_type = ghwor_caps_drums ? 0x08 : 0x06;
    out_caps->flags = 0;

    if (ghwor_caps_drums) {
      // Guitar Hero Xbox 360 5-lane drum kit:
      // Green=A, Red=B, Yellow=Y, Blue=X, Orange=RB, Kick=LB.
      out_caps->gamepad.buttons =
          X_INPUT_GAMEPAD_DPAD_UP |
          X_INPUT_GAMEPAD_DPAD_DOWN |
          X_INPUT_GAMEPAD_DPAD_LEFT |
          X_INPUT_GAMEPAD_DPAD_RIGHT |
          X_INPUT_GAMEPAD_START |
          X_INPUT_GAMEPAD_BACK |
          X_INPUT_GAMEPAD_LEFT_SHOULDER |
          X_INPUT_GAMEPAD_RIGHT_SHOULDER |
          X_INPUT_GAMEPAD_A |
          X_INPUT_GAMEPAD_B |
          X_INPUT_GAMEPAD_X |
          X_INPUT_GAMEPAD_Y;
      out_caps->gamepad.left_trigger = 0;
      out_caps->gamepad.right_trigger = 0;
      out_caps->gamepad.thumb_lx = 0;
      out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    } else {
      out_caps->gamepad.buttons =
          X_INPUT_GAMEPAD_DPAD_UP |
          X_INPUT_GAMEPAD_DPAD_DOWN |
          X_INPUT_GAMEPAD_DPAD_LEFT |
          X_INPUT_GAMEPAD_DPAD_RIGHT |
          X_INPUT_GAMEPAD_START |
          X_INPUT_GAMEPAD_BACK |
          X_INPUT_GAMEPAD_LEFT_SHOULDER |
          X_INPUT_GAMEPAD_A |
          X_INPUT_GAMEPAD_B |
          X_INPUT_GAMEPAD_X |
          X_INPUT_GAMEPAD_Y;
      out_caps->gamepad.left_trigger = 0;
      out_caps->gamepad.right_trigger = 0;
      out_caps->gamepad.thumb_lx = 0;
      out_caps->gamepad.thumb_ly = 0;
      out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    }

    out_caps->vibration.left_motor_speed = 0;
    out_caps->vibration.right_motor_speed = 0;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  if (!IsEnabled() || user_index != UserIndex()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  UpdateMouseCapture();

  if (!has_focus_) {
    if (out_state) {
      std::memset(out_state, 0, sizeof(*out_state));
      out_state->packet_number = packet_number_;
    }
    return X_ERROR_SUCCESS;
  }

  std::lock_guard lock(state_mutex_);

  uint16_t buttons = 0;

  // GHWoR Alpha 6 v7 - fully configurable keyboard instrument.
  auto HostKeyDown = [this](uint16_t vk) -> bool {
    if (vk == 0 || vk >= 256) {
      return false;
    }
#if REX_PLATFORM_WIN32
    const bool ghwor_left_shift_down =
        (GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0;
    if (vk >= VK_F1 && vk <= VK_F12 && ghwor_left_shift_down) {
      return false;
    }
    if (vk == VK_LSHIFT) {
      for (int ghwor_fn = VK_F1; ghwor_fn <= VK_F12; ++ghwor_fn) {
        if ((GetAsyncKeyState(ghwor_fn) & 0x8000) != 0) {
          return false;
        }
      }
    }
#endif
    bool down = key_down_[vk];
#if REX_PLATFORM_WIN32
    down = down || ((GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0);
#endif
    return down;
  };
  auto ConfigKeyDown = [&HostKeyDown](int32_t vk) -> bool {
    return vk > 0 && vk < 256 && HostKeyDown(static_cast<uint16_t>(vk));
  };

  const bool ghwor_keyboard_drums = REXCVAR_GET(ghwor_keyboard_instrument) == 1;
  if (!ghwor_keyboard_drums) {
    if (ConfigKeyDown(REXCVAR_GET(ghwor_key_guitar_green_vk)))
      buttons |= X_INPUT_GAMEPAD_A;
    if (ConfigKeyDown(REXCVAR_GET(ghwor_key_guitar_red_vk)))
      buttons |= X_INPUT_GAMEPAD_B;
    if (ConfigKeyDown(REXCVAR_GET(ghwor_key_guitar_yellow_vk)))
      buttons |= X_INPUT_GAMEPAD_Y;
    if (ConfigKeyDown(REXCVAR_GET(ghwor_key_guitar_blue_vk)))
      buttons |= X_INPUT_GAMEPAD_X;
    if (ConfigKeyDown(REXCVAR_GET(ghwor_key_guitar_orange_vk)))
      buttons |= X_INPUT_GAMEPAD_LEFT_SHOULDER;
    if (ConfigKeyDown(REXCVAR_GET(ghwor_key_guitar_strum_up_vk)))
      buttons |= X_INPUT_GAMEPAD_DPAD_UP;
    if (ConfigKeyDown(REXCVAR_GET(ghwor_key_guitar_strum_down_vk)))
      buttons |= X_INPUT_GAMEPAD_DPAD_DOWN;
    if (ConfigKeyDown(REXCVAR_GET(ghwor_key_guitar_left_vk)))
      buttons |= X_INPUT_GAMEPAD_DPAD_LEFT;
    if (ConfigKeyDown(REXCVAR_GET(ghwor_key_guitar_right_vk)))
      buttons |= X_INPUT_GAMEPAD_DPAD_RIGHT;
    if (ConfigKeyDown(REXCVAR_GET(ghwor_key_guitar_start_vk)))
      buttons |= X_INPUT_GAMEPAD_START;
    if (ConfigKeyDown(REXCVAR_GET(ghwor_key_guitar_back_vk)))
      buttons |= X_INPUT_GAMEPAD_BACK;
  } else {
    // Genuine Guitar Hero Xbox 360 5-lane drum semantics:
    // A=green, B=red, Y=yellow, X=blue, RB=orange, LB=kick.
    if (ConfigKeyDown(REXCVAR_GET(ghwor_key_drum_green_vk)))
      buttons |= X_INPUT_GAMEPAD_A;
    if (ConfigKeyDown(REXCVAR_GET(ghwor_key_drum_red_vk)))
      buttons |= X_INPUT_GAMEPAD_B;
    if (ConfigKeyDown(REXCVAR_GET(ghwor_key_drum_yellow_vk)))
      buttons |= X_INPUT_GAMEPAD_Y;
    if (ConfigKeyDown(REXCVAR_GET(ghwor_key_drum_blue_vk)))
      buttons |= X_INPUT_GAMEPAD_X;
    if (ConfigKeyDown(REXCVAR_GET(ghwor_key_drum_orange_vk)))
      buttons |= X_INPUT_GAMEPAD_RIGHT_SHOULDER;
    if (ConfigKeyDown(REXCVAR_GET(ghwor_key_drum_kick_vk)))
      buttons |= X_INPUT_GAMEPAD_LEFT_SHOULDER;
    if (ConfigKeyDown(REXCVAR_GET(ghwor_key_drum_up_vk)))
      buttons |= X_INPUT_GAMEPAD_DPAD_UP;
    if (ConfigKeyDown(REXCVAR_GET(ghwor_key_drum_down_vk)))
      buttons |= X_INPUT_GAMEPAD_DPAD_DOWN;
    if (ConfigKeyDown(REXCVAR_GET(ghwor_key_drum_left_vk)))
      buttons |= X_INPUT_GAMEPAD_DPAD_LEFT;
    if (ConfigKeyDown(REXCVAR_GET(ghwor_key_drum_right_vk)))
      buttons |= X_INPUT_GAMEPAD_DPAD_RIGHT;
    if (ConfigKeyDown(REXCVAR_GET(ghwor_key_drum_start_vk)))
      buttons |= X_INPUT_GAMEPAD_START;
    if (ConfigKeyDown(REXCVAR_GET(ghwor_key_drum_back_vk)))
      buttons |= X_INPUT_GAMEPAD_BACK;
  }
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_a)))
    buttons |= X_INPUT_GAMEPAD_A;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_b)))
    buttons |= X_INPUT_GAMEPAD_B;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_x)))
    buttons |= X_INPUT_GAMEPAD_X;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_y)))
    buttons |= X_INPUT_GAMEPAD_Y;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_left_shoulder)))
    buttons |= X_INPUT_GAMEPAD_LEFT_SHOULDER;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_right_shoulder)))
    buttons |= X_INPUT_GAMEPAD_RIGHT_SHOULDER;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_lstick_press)))
    buttons |= X_INPUT_GAMEPAD_LEFT_THUMB;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_rstick_press)))
    buttons |= X_INPUT_GAMEPAD_RIGHT_THUMB;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_back)))
    buttons |= X_INPUT_GAMEPAD_BACK;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_start)))
    buttons |= X_INPUT_GAMEPAD_START;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_guide)))
    buttons |= X_INPUT_GAMEPAD_GUIDE;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_dpad_up)))
    buttons |= X_INPUT_GAMEPAD_DPAD_UP;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_dpad_down)))
    buttons |= X_INPUT_GAMEPAD_DPAD_DOWN;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_dpad_left)))
    buttons |= X_INPUT_GAMEPAD_DPAD_LEFT;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_dpad_right)))
    buttons |= X_INPUT_GAMEPAD_DPAD_RIGHT;

  uint8_t lt = IsBindPressed(key_down_, REXCVAR_GET(keybind_left_trigger)) ? 0xFF : 0;
  uint8_t rt = IsBindPressed(key_down_, REXCVAR_GET(keybind_right_trigger)) ? 0xFF : 0;

  int32_t lx = 0;
  int32_t ly = 0;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_lstick_left)))
    lx -= INT16_MAX;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_lstick_right)))
    lx += INT16_MAX;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_lstick_up)))
    ly += INT16_MAX;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_lstick_down)))
    ly -= INT16_MAX;

  double sensitivity = REXCVAR_GET(mnk_sensitivity);
  constexpr double kBaseScale = 200.0;
  int32_t rx = static_cast<int32_t>(mouse_dx_ * sensitivity * kBaseScale);
  int32_t ry = static_cast<int32_t>(-mouse_dy_ * sensitivity * kBaseScale);
  mouse_dx_ = 0;
  mouse_dy_ = 0;

  auto clamp16 = [](int32_t v) -> int16_t {
    return static_cast<int16_t>(std::clamp(v, (int32_t)INT16_MIN, (int32_t)INT16_MAX));
  };

  packet_number_++;

  if (out_state) {
    out_state->packet_number = packet_number_;
    out_state->gamepad.buttons = buttons;
    out_state->gamepad.left_trigger = lt;
    out_state->gamepad.right_trigger = rt;
    out_state->gamepad.thumb_lx = clamp16(lx);
    out_state->gamepad.thumb_ly = clamp16(ly);
    out_state->gamepad.thumb_rx = clamp16(rx);
    out_state->gamepad.thumb_ry = clamp16(ry);
  }

  if (!ghwor_keyboard_drums) {
    // Guitar whammy: real Xbox guitar probe established -32768 as rest.
    out_state->gamepad.thumb_rx = static_cast<int16_t>(-32768);
    if (ConfigKeyDown(REXCVAR_GET(ghwor_key_guitar_whammy_vk))) {
      out_state->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
    }

    out_state->gamepad.thumb_ry = 0;
    if (ConfigKeyDown(REXCVAR_GET(ghwor_key_guitar_star_vk))) {
      out_state->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    }
  } else {
    // Keyboard has no hit velocity, so emulate a strong digital hit on the
    // same XInput axes used by a physical Guitar Hero Xbox 360 drum kit.
    const bool ghwor_drum_red =
        ConfigKeyDown(REXCVAR_GET(ghwor_key_drum_red_vk));
    const bool ghwor_drum_green =
        ConfigKeyDown(REXCVAR_GET(ghwor_key_drum_green_vk));
    const bool ghwor_drum_yellow =
        ConfigKeyDown(REXCVAR_GET(ghwor_key_drum_yellow_vk));
    const bool ghwor_drum_blue =
        ConfigKeyDown(REXCVAR_GET(ghwor_key_drum_blue_vk));
    const bool ghwor_drum_orange =
        ConfigKeyDown(REXCVAR_GET(ghwor_key_drum_orange_vk));
    const bool ghwor_drum_kick =
        ConfigKeyDown(REXCVAR_GET(ghwor_key_drum_kick_vk));

    out_state->gamepad.thumb_lx = 0;
    out_state->gamepad.thumb_ly =
        (ghwor_drum_red || ghwor_drum_green)
            ? static_cast<int16_t>(0x7FFF)
            : 0;
    out_state->gamepad.thumb_rx =
        (ghwor_drum_yellow || ghwor_drum_blue)
            ? static_cast<int16_t>(0x7FFF)
            : 0;
    out_state->gamepad.thumb_ry =
        (ghwor_drum_orange || ghwor_drum_kick)
            ? static_cast<int16_t>(0x7FFF)
            : 0;
  }

  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) {
  if (!IsEnabled() || user_index != UserIndex()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::GetKeystroke(uint32_t user_index, uint32_t flags,
                                      X_INPUT_KEYSTROKE* out_keystroke) {
  if (!IsEnabled() || user_index != UserIndex()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  std::lock_guard lock(state_mutex_);
  if (keystroke_queue_.empty()) {
    return X_ERROR_EMPTY;
  }
  if (out_keystroke) {
    *out_keystroke = keystroke_queue_.front();
  }
  keystroke_queue_.pop();
  return X_ERROR_SUCCESS;
}

void MnkInputDriver::EnqueueKeystroke(uint16_t vk_pad, bool down) {
  X_INPUT_KEYSTROKE ks = {};
  ks.virtual_key = vk_pad;
  ks.unicode = 0;
  ks.flags = down ? X_INPUT_KEYSTROKE_KEYDOWN : X_INPUT_KEYSTROKE_KEYUP;
  ks.user_index = static_cast<uint8_t>(UserIndex());
  ks.hid_code = 0;
  keystroke_queue_.push(ks);
}

void MnkInputDriver::CenterCursor() {
  if (!attached_window_)
    return;
  int32_t cx = static_cast<int32_t>(attached_window_->GetActualLogicalWidth() / 2);
  int32_t cy = static_cast<int32_t>(attached_window_->GetActualLogicalHeight() / 2);
  prev_mouse_x_ = cx;
  prev_mouse_y_ = cy;
#if REX_PLATFORM_WIN32
  HWND hwnd = static_cast<HWND>(attached_window_->GetNativeWindowHandle());
  if (hwnd) {
    POINT pt = {static_cast<LONG>(cx), static_cast<LONG>(cy)};
    ClientToScreen(hwnd, &pt);
    SetCursorPos(pt.x, pt.y);
  }
#endif
}

void MnkInputDriver::UpdateMouseCapture() {
  if (!attached_window_)
    return;

  bool should_capture = IsEnabled() && has_focus_ && is_active();

  if (should_capture && !mouse_captured_) {
    mouse_captured_ = true;
    precapture_cursor_visibility_ = attached_window_->GetCursorVisibility();
    attached_window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
    attached_window_->CaptureMouse();
    // Reset deltas to avoid a spike on capture start
    mouse_dx_ = 0;
    mouse_dy_ = 0;
  } else if (!should_capture && mouse_captured_) {
    mouse_captured_ = false;
    attached_window_->SetCursorVisibility(precapture_cursor_visibility_);
    attached_window_->ReleaseMouse();
  }

  // Re-center cursor each frame while captured to prevent edge clamping
  if (mouse_captured_) {
    CenterCursor();
  }
}

void MnkInputDriver::SetKeyState(uint16_t vk, bool down) {
  if (vk < 256) {
    key_down_[vk] = down;
  }
}

void MnkInputDriver::OnKeyDown(rex::ui::KeyEvent& e) {
  if (!IsEnabled() || !has_focus_)
    return;
  std::lock_guard lock(state_mutex_);
  uint16_t vk = static_cast<uint16_t>(e.virtual_key());
  SetKeyState(vk, true);
}

void MnkInputDriver::OnKeyUp(rex::ui::KeyEvent& e) {
  if (!IsEnabled())
    return;
  std::lock_guard lock(state_mutex_);
  uint16_t vk = static_cast<uint16_t>(e.virtual_key());
  SetKeyState(vk, false);
}

void MnkInputDriver::OnMouseDown(rex::ui::MouseEvent& e) {
  if (!IsEnabled() || !has_focus_)
    return;
  std::lock_guard lock(state_mutex_);
  switch (e.button()) {
    case rex::ui::MouseEvent::Button::kLeft:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kLButton), true);
      break;
    case rex::ui::MouseEvent::Button::kRight:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kRButton), true);
      break;
    case rex::ui::MouseEvent::Button::kMiddle:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kMButton), true);
      break;
    default:
      break;
  }
}

void MnkInputDriver::OnMouseUp(rex::ui::MouseEvent& e) {
  if (!IsEnabled())
    return;
  std::lock_guard lock(state_mutex_);
  switch (e.button()) {
    case rex::ui::MouseEvent::Button::kLeft:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kLButton), false);
      break;
    case rex::ui::MouseEvent::Button::kRight:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kRButton), false);
      break;
    case rex::ui::MouseEvent::Button::kMiddle:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kMButton), false);
      break;
    default:
      break;
  }
}

void MnkInputDriver::OnMouseMove(rex::ui::MouseEvent& e) {
  if (!IsEnabled() || !has_focus_)
    return;
  std::lock_guard lock(state_mutex_);
  int32_t x = e.x();
  int32_t y = e.y();
  mouse_dx_ += x - prev_mouse_x_;
  mouse_dy_ += y - prev_mouse_y_;
  prev_mouse_x_ = x;
  prev_mouse_y_ = y;
}

void MnkInputDriver::OnLostFocus(rex::ui::UISetupEvent&) {
  std::lock_guard lock(state_mutex_);
  has_focus_ = false;
  std::memset(key_down_, 0, sizeof(key_down_));
  mouse_dx_ = 0;
  mouse_dy_ = 0;
  if (mouse_captured_ && attached_window_) {
    mouse_captured_ = false;
    attached_window_->SetCursorVisibility(precapture_cursor_visibility_);
    attached_window_->ReleaseMouse();
  }
}

void MnkInputDriver::OnGotFocus(rex::ui::UISetupEvent&) {
  std::lock_guard lock(state_mutex_);
  has_focus_ = true;
}

}  // namespace rex::input::mnk
