/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <array>
#include <filesystem>

#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/input/flags.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/logging.h>
#include <rex/ui/virtual_key.h>

REXCVAR_DEFINE_STRING(hid_mappings_file, "gamecontrollerdb.txt", "Input",
                      "Path to SDL gamecontroller mappings file");

REXCVAR_DECLARE(bool, ghwor_joystick_enabled);
REXCVAR_DECLARE(int32_t, ghwor_joystick_instrument);
REXCVAR_DECLARE(bool, mnk_mode);

namespace rex::input::sdl {

// GHWOR DUAL PLAYER ROUTING v19 FIX2
// MnK sigue siendo P1. Con MnK activo, el joystick seleccionado ocupa P2.
static uint32_t GhWoRJoystickGuestUserIndex() {
  if (REXCVAR_GET(ghwor_joystick_enabled) && REXCVAR_GET(mnk_mode)) {
    return uint32_t(1);
  }
  return uint32_t(0);
}

// GHWOR REAL JOYSTICK MAPPER v16 DRIVER BEGIN
REXCVAR_DEFINE_STRING(ghwor_joystick_guid, "", "Input/GHWoR", "Selected SDL joystick GUID");
REXCVAR_DEFINE_INT32(ghwor_joystick_instance, 0, "Input/GHWoR", "Selected occurrence among identical SDL GUIDs").range(0, 31);
REXCVAR_DEFINE_STRING(ghwor_joystick_name, "", "Input/GHWoR", "Selected SDL joystick display name");
REXCVAR_DEFINE_INT32(ghwor_joystick_axis_threshold, 16000, "Input/GHWoR", "Axis threshold used by joystick bindings").range(1000, 32767);

#define GHWOR_JOY_BIND(name, def) REXCVAR_DEFINE_STRING(name, def, "Input/GHWoR/Joystick", #name)
GHWOR_JOY_BIND(ghwor_joy_pad_a, "A"); GHWOR_JOY_BIND(ghwor_joy_pad_b, "B");
GHWOR_JOY_BIND(ghwor_joy_pad_x, "X"); GHWOR_JOY_BIND(ghwor_joy_pad_y, "Y");
GHWOR_JOY_BIND(ghwor_joy_pad_lb, "LB"); GHWOR_JOY_BIND(ghwor_joy_pad_rb, "RB");
GHWOR_JOY_BIND(ghwor_joy_pad_lt, "LT+"); GHWOR_JOY_BIND(ghwor_joy_pad_rt, "RT+");
GHWOR_JOY_BIND(ghwor_joy_pad_up, "DPAD_UP"); GHWOR_JOY_BIND(ghwor_joy_pad_down, "DPAD_DOWN");
GHWOR_JOY_BIND(ghwor_joy_pad_left, "DPAD_LEFT"); GHWOR_JOY_BIND(ghwor_joy_pad_right, "DPAD_RIGHT");
GHWOR_JOY_BIND(ghwor_joy_pad_start, "START"); GHWOR_JOY_BIND(ghwor_joy_pad_back, "BACK");
GHWOR_JOY_BIND(ghwor_joy_pad_ls, "LS"); GHWOR_JOY_BIND(ghwor_joy_pad_rs, "RS");
GHWOR_JOY_BIND(ghwor_joy_drum_green, "A"); GHWOR_JOY_BIND(ghwor_joy_drum_red, "B");
GHWOR_JOY_BIND(ghwor_joy_drum_yellow, "Y"); GHWOR_JOY_BIND(ghwor_joy_drum_blue, "X");
GHWOR_JOY_BIND(ghwor_joy_drum_orange, "RB"); GHWOR_JOY_BIND(ghwor_joy_drum_kick, "LB");
GHWOR_JOY_BIND(ghwor_joy_drum_up, "DPAD_UP"); GHWOR_JOY_BIND(ghwor_joy_drum_down, "DPAD_DOWN");
GHWOR_JOY_BIND(ghwor_joy_drum_left, "DPAD_LEFT"); GHWOR_JOY_BIND(ghwor_joy_drum_right, "DPAD_RIGHT");
GHWOR_JOY_BIND(ghwor_joy_drum_start, "START"); GHWOR_JOY_BIND(ghwor_joy_drum_back, "BACK");
#undef GHWOR_JOY_BIND

static std::string GhWoRGuidString(SDL_JoystickID id) {
  char text[64] = {};
  SDL_GUIDToString(SDL_GetGamepadGUIDForID(id), text, int(sizeof(text)));
  return text;
}
static int GhWoRGuidOccurrence(SDL_JoystickID target) {
  int count = 0; SDL_JoystickID* ids = SDL_GetGamepads(&count); if (!ids) return -1;
  const std::string wanted = GhWoRGuidString(target); int occ = 0; int result = -1;
  for (int i = 0; i < count; ++i) {
    if (GhWoRGuidString(ids[i]) != wanted) continue;
    if (ids[i] == target) { result = occ; break; }
    ++occ;
  }
  SDL_free(ids); return result;
}
static bool GhWoRSelected(SDL_JoystickID id) {
  if (!REXCVAR_GET(ghwor_joystick_enabled)) return true;
  const auto& guid = REXCVAR_GET(ghwor_joystick_guid);
  if (guid.empty() || GhWoRGuidString(id) != guid) return false;
  return GhWoRGuidOccurrence(id) == REXCVAR_GET(ghwor_joystick_instance);
}
static bool GhWoRBindPressed(const X_INPUT_GAMEPAD& p, const std::string& b) {
  if (b.empty() || b == "NONE") return false;
  const uint16_t buttons = static_cast<uint16_t>(p.buttons);
  if (b == "DPAD_UP") return (buttons & 0x0001) != 0; if (b == "DPAD_DOWN") return (buttons & 0x0002) != 0;
  if (b == "DPAD_LEFT") return (buttons & 0x0004) != 0; if (b == "DPAD_RIGHT") return (buttons & 0x0008) != 0;
  if (b == "START") return (buttons & 0x0010) != 0; if (b == "BACK") return (buttons & 0x0020) != 0;
  if (b == "LS") return (buttons & 0x0040) != 0; if (b == "RS") return (buttons & 0x0080) != 0;
  if (b == "LB") return (buttons & 0x0100) != 0; if (b == "RB") return (buttons & 0x0200) != 0;
  if (b == "GUIDE") return (buttons & 0x0400) != 0;
  if (b == "A") return (buttons & 0x1000) != 0; if (b == "B") return (buttons & 0x2000) != 0;
  if (b == "X") return (buttons & 0x4000) != 0; if (b == "Y") return (buttons & 0x8000) != 0;
  const int t = REXCVAR_GET(ghwor_joystick_axis_threshold);
  if (b == "LT+") return static_cast<uint8_t>(p.left_trigger) >= 128;
  if (b == "RT+") return static_cast<uint8_t>(p.right_trigger) >= 128;
  const int lx = static_cast<int16_t>(p.thumb_lx), ly = static_cast<int16_t>(p.thumb_ly);
  const int rx = static_cast<int16_t>(p.thumb_rx), ry = static_cast<int16_t>(p.thumb_ry);
  if (b == "LX+") return lx >= t; if (b == "LX-") return lx <= -t;
  if (b == "LY+") return ly >= t; if (b == "LY-") return ly <= -t;
  if (b == "RX+") return rx >= t; if (b == "RX-") return rx <= -t;
  if (b == "RY+") return ry >= t; if (b == "RY-") return ry <= -t;
  return false;
}
static void GhWoRSetButton(uint16_t& buttons, uint16_t mask, bool down) { if (down) buttons |= mask; }
static void GhWoRMapGamepad(X_INPUT_GAMEPAD& p) {
  const X_INPUT_GAMEPAD src = p; uint16_t buttons = 0;
  GhWoRSetButton(buttons,0x1000,GhWoRBindPressed(src,REXCVAR_GET(ghwor_joy_pad_a)));
  GhWoRSetButton(buttons,0x2000,GhWoRBindPressed(src,REXCVAR_GET(ghwor_joy_pad_b)));
  GhWoRSetButton(buttons,0x4000,GhWoRBindPressed(src,REXCVAR_GET(ghwor_joy_pad_x)));
  GhWoRSetButton(buttons,0x8000,GhWoRBindPressed(src,REXCVAR_GET(ghwor_joy_pad_y)));
  GhWoRSetButton(buttons,0x0100,GhWoRBindPressed(src,REXCVAR_GET(ghwor_joy_pad_lb)));
  GhWoRSetButton(buttons,0x0200,GhWoRBindPressed(src,REXCVAR_GET(ghwor_joy_pad_rb)));
  GhWoRSetButton(buttons,0x0001,GhWoRBindPressed(src,REXCVAR_GET(ghwor_joy_pad_up)));
  GhWoRSetButton(buttons,0x0002,GhWoRBindPressed(src,REXCVAR_GET(ghwor_joy_pad_down)));
  GhWoRSetButton(buttons,0x0004,GhWoRBindPressed(src,REXCVAR_GET(ghwor_joy_pad_left)));
  GhWoRSetButton(buttons,0x0008,GhWoRBindPressed(src,REXCVAR_GET(ghwor_joy_pad_right)));
  GhWoRSetButton(buttons,0x0010,GhWoRBindPressed(src,REXCVAR_GET(ghwor_joy_pad_start)));
  GhWoRSetButton(buttons,0x0020,GhWoRBindPressed(src,REXCVAR_GET(ghwor_joy_pad_back)));
  GhWoRSetButton(buttons,0x0040,GhWoRBindPressed(src,REXCVAR_GET(ghwor_joy_pad_ls)));
  GhWoRSetButton(buttons,0x0080,GhWoRBindPressed(src,REXCVAR_GET(ghwor_joy_pad_rs)));
  p.buttons = buttons;
  p.left_trigger = GhWoRBindPressed(src,REXCVAR_GET(ghwor_joy_pad_lt)) ? 0xFF : 0;
  p.right_trigger = GhWoRBindPressed(src,REXCVAR_GET(ghwor_joy_pad_rt)) ? 0xFF : 0;
  // Analog sticks remain physical for normal joystick mode.
}
static void GhWoRMapDrums(X_INPUT_GAMEPAD& p) {
  const X_INPUT_GAMEPAD src = p;

  const bool g = GhWoRBindPressed(src, REXCVAR_GET(ghwor_joy_drum_green));
  const bool r = GhWoRBindPressed(src, REXCVAR_GET(ghwor_joy_drum_red));
  const bool y = GhWoRBindPressed(src, REXCVAR_GET(ghwor_joy_drum_yellow));
  const bool b = GhWoRBindPressed(src, REXCVAR_GET(ghwor_joy_drum_blue));
  const bool o = GhWoRBindPressed(src, REXCVAR_GET(ghwor_joy_drum_orange));
  const bool k = GhWoRBindPressed(src, REXCVAR_GET(ghwor_joy_drum_kick));

  // Captura fisica: la bateria GH mantiene 0x0040 activo incluso en reposo.
  uint16_t buttons = 0x0040;

  GhWoRSetButton(buttons, 0x1000, g);  // Verde    = A
  GhWoRSetButton(buttons, 0x2000, r);  // Rojo     = B
  GhWoRSetButton(buttons, 0x8000, y);  // Amarillo = Y
  GhWoRSetButton(buttons, 0x4000, b);  // Azul     = X
  GhWoRSetButton(buttons, 0x0200, o);  // Naranja  = RB
  GhWoRSetButton(buttons, 0x0100, k);  // Bombo     = LB

  GhWoRSetButton(buttons, 0x0001,
                 GhWoRBindPressed(src, REXCVAR_GET(ghwor_joy_drum_up)));
  GhWoRSetButton(buttons, 0x0002,
                 GhWoRBindPressed(src, REXCVAR_GET(ghwor_joy_drum_down)));
  GhWoRSetButton(buttons, 0x0004,
                 GhWoRBindPressed(src, REXCVAR_GET(ghwor_joy_drum_left)));
  GhWoRSetButton(buttons, 0x0008,
                 GhWoRBindPressed(src, REXCVAR_GET(ghwor_joy_drum_right)));
  GhWoRSetButton(buttons, 0x0010,
                 GhWoRBindPressed(src, REXCVAR_GET(ghwor_joy_drum_start)));
  GhWoRSetButton(buttons, 0x0020,
                 GhWoRBindPressed(src, REXCVAR_GET(ghwor_joy_drum_back)));

  p = {};
  p.buttons = buttons;

  // Forma de ejes medida en la bateria GH real.
  p.thumb_ly = r ? int16_t(9216) : (g ? int16_t(64) : int16_t(0));
  p.thumb_rx = b ? int16_t(8192) : (y ? int16_t(32) : int16_t(0));
  p.thumb_ry = k ? int16_t(12288) : (o ? int16_t(32) : int16_t(0));
}
// GHWOR REAL JOYSTICK MAPPER v17 FIX1 END


SDLInputDriver::SDLInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order),
      sdl_events_initialized_(false),
      SDL_Gamepad_initialized_(false),
      sdl_events_unflushed_(0),
      sdl_pumpevents_queued_(false),
      controllers_(),
      controllers_mutex_(),
      keystroke_states_() {}

SDLInputDriver::~SDLInputDriver() {}

X_STATUS SDLInputDriver::Setup() {
  if (!TestSDLVersion()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

void SDLInputDriver::OnWindowAvailable(rex::ui::Window* window) {
  if (window && !attached_window_) {
    attached_window_ = window;
    window->AddListener(this);
    window->app_context().CallInUIThreadSynchronous([this]() {
      // Initialize SDL events subsystem
      if (!SDL_InitSubSystem(SDL_INIT_EVENTS)) {
        REXLOG_ERROR("SDL: Failed to init events subsystem: {}", SDL_GetError());
        return;
      }
      sdl_events_initialized_ = true;
      pending_events_.reserve(64);

      // With an event watch we will always get notified, even if the event queue
      // is full, which can happen if another subsystem does not clear its events.
      SDL_AddEventWatch(
          [](void* userdata, SDL_Event* event) -> bool {
            if (!userdata || !event) {
              assert_always();
              return false;
            }

            const auto type = event->type;
            if (type < SDL_EVENT_JOYSTICK_AXIS_MOTION || type >= SDL_EVENT_FINGER_DOWN) {
              return false;
            }

            // If another part of rex uses another SDL subsystem that generates
            // events, this may seem like a bad idea. They will however not
            // subscribe to controller events so we get away with that.
            const auto driver = static_cast<SDLInputDriver*>(userdata);
            driver->HandleEvent(*event);

            return false;
          },
          this);

      // Initialize game controller subsystem
      if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        REXLOG_ERROR("SDL: Failed to init gamecontroller subsystem: {}", SDL_GetError());
        return;
      }
      SDL_Gamepad_initialized_ = true;

      // Load custom controller mappings if available
      if (!REXCVAR_GET(hid_mappings_file).empty()) {
        std::filesystem::path mappings_path(REXCVAR_GET(hid_mappings_file));
        if (!std::filesystem::exists(mappings_path)) {
          REXLOG_WARN("SDL GameControllerDB: file '{}' does not exist.",
                      REXCVAR_GET(hid_mappings_file));
        } else {
          auto mappings_result =
              SDL_AddGamepadMappingsFromFile(REXCVAR_GET(hid_mappings_file).c_str());
          if (mappings_result < 0) {
            REXLOG_ERROR("SDL GameControllerDB: error loading file '{}': {}.",
                         REXCVAR_GET(hid_mappings_file), mappings_result);
          } else {
            REXLOG_INFO("SDL GameControllerDB: loaded {} mappings.", mappings_result);
          }
        }
      }
      REXLOG_INFO("SDL input driver initialized successfully");
    });
  }
}

void SDLInputDriver::OnClosing(rex::ui::UIEvent&) {
  if (attached_window_) {
    attached_window_->RemoveListener(this);
    if (sdl_pumpevents_queued_) {
      attached_window_->app_context().CallInUIThreadSynchronous(
          [this]() { attached_window_->app_context().ExecutePendingFunctionsFromUIThread(); });
    }
    for (size_t i = 0; i < controllers_.size(); i++) {
      if (controllers_.at(i).sdl) {
        SDL_CloseGamepad(controllers_.at(i).sdl);
        controllers_.at(i) = {};
      }
    }
    if (SDL_Gamepad_initialized_) {
      SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
      SDL_Gamepad_initialized_ = false;
    }
    if (sdl_events_initialized_) {
      SDL_QuitSubSystem(SDL_INIT_EVENTS);
      sdl_events_initialized_ = false;
    }
    attached_window_ = nullptr;
  }
}

void SDLInputDriver::OnLostFocus(rex::ui::UISetupEvent&) {}

void SDLInputDriver::OnGotFocus(rex::ui::UISetupEvent&) {}

X_RESULT SDLInputDriver::GetCapabilities(uint32_t user_index, uint32_t flags,
                                         X_INPUT_CAPABILITIES* out_caps) {
  assert(sdl_events_initialized_ && SDL_Gamepad_initialized_);
  if (user_index >= HID_SDL_USER_COUNT || !out_caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  QueueControllerUpdate();

  auto guard = DrainAndLock();

  auto controller = GetControllerState(user_index);
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Unfortunately drivers can't present all information immediately (e.g.
  // battery information) so this needs to be refreshed every time.
  UpdateXCapabilities(*controller);

  // GHWOR GH DRUM PHYSICAL CLONE v17 FIX1 BEGIN
  // Firma capturada directamente de la bateria Guitar Hero Xbox 360 real.
  if (REXCVAR_GET(ghwor_joystick_enabled) &&
      user_index == GhWoRJoystickGuestUserIndex() &&
      REXCVAR_GET(ghwor_joystick_instrument) == 1) {
    auto& c = controller->caps;

    c.type = 0x01;
    c.sub_type = 0x08;
    c.flags = 0x0006;

    c.gamepad.buttons = 0xF37F;
    c.gamepad.left_trigger = 0;
    c.gamepad.right_trigger = 0;
    c.gamepad.thumb_lx = static_cast<int16_t>(5168);
    c.gamepad.thumb_ly = static_cast<int16_t>(2053);
    c.gamepad.thumb_rx = static_cast<int16_t>(1);
    c.gamepad.thumb_ry = static_cast<int16_t>(-64);

    c.vibration.left_motor_speed = 0;
    c.vibration.right_motor_speed = 0;

    static bool ghwor_physical_clone_v17_fix1_logged = false;
    if (!ghwor_physical_clone_v17_fix1_logged) {
      ghwor_physical_clone_v17_fix1_logged = true;
      REXLOG_INFO(
          "GHWOR GH DRUM PHYSICAL CLONE v17 FIX1: "
          "type=0x01 subtype=0x08 flags=0x0006 caps=0xF37F "
          "idle=0x0040 axes=5168/2053/1/-64 XAM=passthrough");
    }
  }
// GHWOR GH DRUM PHYSICAL CLONE v17 FIX1 END

  std::memcpy(out_caps, &controller->caps, sizeof(*out_caps));

  return X_ERROR_SUCCESS;
}

X_RESULT SDLInputDriver::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  assert(sdl_events_initialized_ && SDL_Gamepad_initialized_);
  if (user_index >= HID_SDL_USER_COUNT) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  auto is_active = this->is_active();

  if (is_active) {
    QueueControllerUpdate();
  }

  auto guard = DrainAndLock();

  auto controller = GetControllerState(user_index);
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Make sure packet_number is only incremented by 1, even if there have been
  // multiple updates between GetState calls. Also track `is_active` to
  // increment the packet number if it changed.
  if ((is_active != controller->is_active) || (is_active && controller->state_changed)) {
    controller->state.packet_number++;
    controller->is_active = is_active;
    controller->state_changed = false;
  }
  std::memcpy(out_state, &controller->state, sizeof(*out_state));
  if (REXCVAR_GET(ghwor_joystick_enabled) &&
      user_index == GhWoRJoystickGuestUserIndex() && is_active) {
    if (REXCVAR_GET(ghwor_joystick_instrument) == 1) GhWoRMapDrums(out_state->gamepad);
    else GhWoRMapGamepad(out_state->gamepad);
  }
  if (!is_active) {
    // Simulate an "untouched" controller. When we become active again the
    // pressed buttons aren't lost and will be visible again.
    std::memset(&out_state->gamepad, 0, sizeof(out_state->gamepad));
  }
  return X_ERROR_SUCCESS;
}

X_RESULT SDLInputDriver::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) {
  assert(sdl_events_initialized_ && SDL_Gamepad_initialized_);
  if (user_index >= HID_SDL_USER_COUNT) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  QueueControllerUpdate();

  auto guard = DrainAndLock();

  auto controller = GetControllerState(user_index);
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

#if SDL_VERSION_ATLEAST(2, 0, 9)
  if (SDL_RumbleGamepad(controller->sdl, vibration->left_motor_speed, vibration->right_motor_speed,
                        0)) {
    return X_ERROR_FUNCTION_FAILED;
  } else {
    return X_ERROR_SUCCESS;
  }
#else
  return X_ERROR_SUCCESS;
#endif
}

X_RESULT SDLInputDriver::GetKeystroke(uint32_t users, uint32_t flags,
                                      X_INPUT_KEYSTROKE* out_keystroke) {
  // TODO(JoelLinn): Figure out the flags
  // https://github.com/evilC/UCR/blob/0489929e2a8e39caa3484c67f3993d3fba39e46f/Libraries/XInput.ahk#L85-L98
  assert(sdl_events_initialized_ && SDL_Gamepad_initialized_);
  bool user_any = users == 0xFF;
  if (users >= HID_SDL_USER_COUNT && !user_any) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  if (!out_keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  // The order of this list is also the order in which events are send if
  // multiple buttons change at once.
  static_assert(sizeof(X_INPUT_GAMEPAD::buttons) == 2);
  static constexpr std::array<rex::ui::VirtualKey, 34> kVkLookup = {
      // 00 - True buttons from xinput button field
      rex::ui::VirtualKey::kXInputPadDpadUp,
      rex::ui::VirtualKey::kXInputPadDpadDown,
      rex::ui::VirtualKey::kXInputPadDpadLeft,
      rex::ui::VirtualKey::kXInputPadDpadRight,
      rex::ui::VirtualKey::kXInputPadStart,
      rex::ui::VirtualKey::kXInputPadBack,
      rex::ui::VirtualKey::kXInputPadLThumbPress,
      rex::ui::VirtualKey::kXInputPadRThumbPress,
      rex::ui::VirtualKey::kXInputPadLShoulder,
      rex::ui::VirtualKey::kXInputPadRShoulder,
      rex::ui::VirtualKey::kNone, /* Guide has no VK */
      rex::ui::VirtualKey::kNone, /* Unknown */
      rex::ui::VirtualKey::kXInputPadA,
      rex::ui::VirtualKey::kXInputPadB,
      rex::ui::VirtualKey::kXInputPadX,
      rex::ui::VirtualKey::kXInputPadY,
      // 16 - Fake buttons generated from analog inputs
      rex::ui::VirtualKey::kXInputPadLTrigger,
      rex::ui::VirtualKey::kXInputPadRTrigger,
      // 18
      rex::ui::VirtualKey::kXInputPadLThumbUp,
      rex::ui::VirtualKey::kXInputPadLThumbDown,
      rex::ui::VirtualKey::kXInputPadLThumbRight,
      rex::ui::VirtualKey::kXInputPadLThumbLeft,
      rex::ui::VirtualKey::kXInputPadLThumbUpLeft,
      rex::ui::VirtualKey::kXInputPadLThumbUpRight,
      rex::ui::VirtualKey::kXInputPadLThumbDownRight,
      rex::ui::VirtualKey::kXInputPadLThumbDownLeft,
      // 26
      rex::ui::VirtualKey::kXInputPadRThumbUp,
      rex::ui::VirtualKey::kXInputPadRThumbDown,
      rex::ui::VirtualKey::kXInputPadRThumbRight,
      rex::ui::VirtualKey::kXInputPadRThumbLeft,
      rex::ui::VirtualKey::kXInputPadRThumbUpLeft,
      rex::ui::VirtualKey::kXInputPadRThumbUpRight,
      rex::ui::VirtualKey::kXInputPadRThumbDownRight,
      rex::ui::VirtualKey::kXInputPadRThumbDownLeft,
  };

  auto is_active = this->is_active();

  if (is_active) {
    QueueControllerUpdate();
  }

  auto guard = DrainAndLock();

  for (uint32_t user_index = (user_any ? 0 : users);
       user_index < (user_any ? HID_SDL_USER_COUNT : users + 1); user_index++) {
    auto controller = GetControllerState(user_index);
    if (!controller) {
      if (user_any) {
        continue;
      } else {
        return X_ERROR_DEVICE_NOT_CONNECTED;
      }
    }

    // If input is not active (e.g. due to a dialog overlay), force buttons to
    // "unpressed". The algorithm will automatically send UP events when
    // `is_active()` goes low and DOWN events when it goes high again.
    const uint64_t curr_butts =
        is_active
            ? (controller->state.gamepad.buttons | AnalogToKeyfield(controller->state.gamepad))
            : uint64_t(0);
    KeystrokeState& last = keystroke_states_.at(user_index);

    // Handle repeating
    auto guest_now = rex::chrono::Clock::QueryGuestUptimeMillis();
    static_assert(HID_SDL_REPEAT_DELAY >= HID_SDL_REPEAT_RATE);
    if (last.repeat_state == RepeatState::Waiting &&
        (last.repeat_time + HID_SDL_REPEAT_DELAY < guest_now)) {
      last.repeat_state = RepeatState::Repeating;
    }
    if (last.repeat_state == RepeatState::Repeating &&
        (last.repeat_time + HID_SDL_REPEAT_RATE < guest_now)) {
      last.repeat_time = guest_now;
      rex::ui::VirtualKey vk = kVkLookup.at(last.repeat_butt_idx);
      assert_true(vk != rex::ui::VirtualKey::kNone);
      out_keystroke->virtual_key = uint16_t(vk);
      out_keystroke->unicode = 0;
      out_keystroke->user_index = user_index;
      out_keystroke->hid_code = 0;
      out_keystroke->flags = X_INPUT_KEYSTROKE_KEYDOWN | X_INPUT_KEYSTROKE_REPEAT;
      return X_ERROR_SUCCESS;
    }

    auto butts_changed = curr_butts ^ last.buttons;
    if (!butts_changed) {
      continue;
    }

    // First try to clear buttons with up events. This is to match xinput
    // behaviour when transitioning thumb sticks, e.g. so that THUMB_UPLEFT is
    // up before THUMB_LEFT is down.
    for (auto [clear_pass, i] = std::tuple{true, 0}; i < 2; clear_pass = false, i++) {
      for (uint8_t i = 0; i < uint8_t(std::size(kVkLookup)); i++) {
        auto fbutton = uint64_t(1) << i;
        if (!(butts_changed & fbutton)) {
          continue;
        }
        rex::ui::VirtualKey vk = kVkLookup.at(i);
        if (vk == rex::ui::VirtualKey::kNone) {
          continue;
        }

        out_keystroke->virtual_key = uint16_t(vk);
        out_keystroke->unicode = 0;
        out_keystroke->user_index = user_index;
        out_keystroke->hid_code = 0;

        bool is_pressed = curr_butts & fbutton;
        if (clear_pass && !is_pressed) {
          // up
          out_keystroke->flags = X_INPUT_KEYSTROKE_KEYUP;
          last.buttons &= ~fbutton;
          last.repeat_state = RepeatState::Idle;
          return X_ERROR_SUCCESS;
        }
        if (!clear_pass && is_pressed) {
          // down
          out_keystroke->flags = X_INPUT_KEYSTROKE_KEYDOWN;
          last.buttons |= fbutton;
          last.repeat_state = RepeatState::Waiting;
          last.repeat_butt_idx = i;
          last.repeat_time = guest_now;
          return X_ERROR_SUCCESS;
        }
      }
    }
  }
  return X_ERROR_EMPTY;
}

void SDLInputDriver::HandleEvent(const SDL_Event& event) {
  // This callback will likely run on the thread that posts the event, which
  // may be a dedicated thread SDL has created for the joystick subsystem.

  // Event queue should never be (this) full
  assert(SDL_PeepEvents(nullptr, 0, SDL_PEEKEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) < 0xFFFF);

  // The queue could grow up to 3.5MB since it is never polled.
  if (++sdl_events_unflushed_ > 64) {
    SDL_FlushEvents(SDL_EVENT_JOYSTICK_AXIS_MOTION, SDL_EVENT_FINGER_DOWN - 1);
    sdl_events_unflushed_ = 0;
  }

  // Buffer only - no controllers_mutex_ acquisition here.
  // This breaks the lock ordering inversion between controllers_mutex_ and
  // SDL's internal joystick lock that caused deadlocks.
  std::lock_guard<std::mutex> guard(event_queue_mutex_);
  pending_events_.push_back(event);
}

std::unique_lock<std::mutex> SDLInputDriver::DrainAndLock() {
  std::vector<SDL_Event> events;
  {
    std::lock_guard<std::mutex> guard(event_queue_mutex_);
    events.swap(pending_events_);
  }
  std::unique_lock<std::mutex> guard(controllers_mutex_);
  for (const auto& event : events) {
    ProcessEventLocked(event);
  }
  return guard;
}

void SDLInputDriver::ProcessEventLocked(const SDL_Event& event) {
  switch (event.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
      OnControllerDeviceAddedLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_REMOVED:
      OnControllerDeviceRemovedLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
      OnControllerDeviceAxisMotionLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
      OnControllerDeviceButtonChangedLocked(event);
      break;
    default:
      break;
  }
}

void SDLInputDriver::OnControllerDeviceAddedLocked(const SDL_Event& event) {
  // Open the controller.
  const auto controller = SDL_OpenGamepad(event.cdevice.which);
  if (REXCVAR_GET(ghwor_joystick_enabled) && controller && !GhWoRSelected(event.cdevice.which)) {
    REXLOG_INFO("GHWOR JOYSTICK v15A: ignoring non-selected SDL device {}", GhWoRGuidString(event.cdevice.which));
    SDL_CloseGamepad(controller);
    return;
  }
  if (!controller) {
    assert_always();
    return;
  }
  REXLOG_INFO(
      "SDL OnControllerDeviceAdded: \"{}\", "
      "JoystickType({}), "
      "GameControllerType({}), "
      "VendorID(0x{:04X}), "
      "ProductID(0x{:04X})",
      SDL_GetGamepadName(controller),
      static_cast<int>(SDL_GetJoystickType(SDL_GetGamepadJoystick(controller))),
      static_cast<int>(SDL_GetGamepadType(controller)), SDL_GetGamepadVendor(controller),
      SDL_GetGamepadProduct(controller));
  int user_id = -1;

  // GHWOR DUAL PLAYER ROUTING v19 FIX2
  // Los dispositivos no seleccionados ya fueron descartados arriba.
  // Si MnK esta activo, reservamos P1 para teclado y ponemos SDL en P2.
  if (REXCVAR_GET(ghwor_joystick_enabled) && REXCVAR_GET(mnk_mode)) {
    user_id = 1;
  }

  if (user_id < 0) {
    user_id = SDL_GetGamepadPlayerIndex(controller);
  }

  if (REXCVAR_GET(ghwor_joystick_enabled)) {
    REXLOG_INFO(
        "GHWOR DUAL PLAYER ROUTING v19 FIX2: SDL selected -> P{} "
        "mnk_mode={}",
        user_id + 1, REXCVAR_GET(mnk_mode));
  }
  // Is that id already taken?
  if (user_id < 0 || user_id >= static_cast<int>(controllers_.size()) ||
      controllers_.at(user_id).sdl) {
    user_id = -1;
  }
  // No player index or already taken, just take the first free slot.
  if (user_id < 0) {
    for (size_t i = 0; i < controllers_.size(); i++) {
      if (!controllers_.at(i).sdl) {
        user_id = static_cast<int>(i);
#if SDL_VERSION_ATLEAST(2, 0, 12)
        SDL_SetGamepadPlayerIndex(controller, user_id);
#endif
        break;
      }
    }
  }
  if (user_id >= 0) {
    auto& state = controllers_.at(user_id);
    state = {controller, {}};
    // XInput seems to start with packet_number = 1 .
    state.state_changed = true;
    UpdateXCapabilities(state);

    REXLOG_INFO("SDL OnControllerDeviceAdded: Added at index {}.", user_id);
  } else {
    // No more controllers needed, close it.
    SDL_CloseGamepad(controller);
    REXLOG_WARN("SDL OnControllerDeviceAdded: Ignored. No free slots.");
  }
}

void SDLInputDriver::OnControllerDeviceRemovedLocked(const SDL_Event& event) {
  // Find the disconnected gamecontroller and close it.
  auto idx = GetControllerIndexFromInstanceID(event.cdevice.which);
  if (idx) {
    SDL_CloseGamepad(controllers_.at(*idx).sdl);
    controllers_.at(*idx) = {};
    keystroke_states_.at(*idx) = {};
    REXLOG_INFO("SDL OnControllerDeviceRemoved: Removed at player index {}.", *idx);
  } else {
    // Can happen in case all slots where full previously.
    REXLOG_WARN("SDL OnControllerDeviceRemoved: Ignored. Unused device.");
  }
}

void SDLInputDriver::OnControllerDeviceAxisMotionLocked(const SDL_Event& event) {
  auto idx = GetControllerIndexFromInstanceID(event.gaxis.which);
  assert(idx);
  auto& pad = controllers_.at(*idx).state.gamepad;
  switch (event.gaxis.axis) {
    case SDL_GAMEPAD_AXIS_LEFTX:
      pad.thumb_lx = event.gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_LEFTY:
      pad.thumb_ly = ~event.gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_RIGHTX:
      pad.thumb_rx = event.gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_RIGHTY:
      pad.thumb_ry = ~event.gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
      pad.left_trigger = static_cast<uint8_t>(event.gaxis.value >> 7);
      break;
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
      pad.right_trigger = static_cast<uint8_t>(event.gaxis.value >> 7);
      break;
    default:
      assert_always();
      break;
  }
  controllers_.at(*idx).state_changed = true;
}

void SDLInputDriver::OnControllerDeviceButtonChangedLocked(const SDL_Event& event) {
  // Define a lookup table to map between SDL and XInput button codes.
  // These need to be in the order of the SDL_GamepadButton enum.
  static constexpr std::array<std::underlying_type<X_INPUT_GAMEPAD_BUTTON>::type, 21>
      xbutton_lookup = {
          // Standard buttons:
          X_INPUT_GAMEPAD_A,
          X_INPUT_GAMEPAD_B,
          X_INPUT_GAMEPAD_X,
          X_INPUT_GAMEPAD_Y,
          X_INPUT_GAMEPAD_BACK,
          X_INPUT_GAMEPAD_GUIDE,
          X_INPUT_GAMEPAD_START,
          X_INPUT_GAMEPAD_LEFT_THUMB,
          X_INPUT_GAMEPAD_RIGHT_THUMB,
          X_INPUT_GAMEPAD_LEFT_SHOULDER,
          X_INPUT_GAMEPAD_RIGHT_SHOULDER,
          X_INPUT_GAMEPAD_DPAD_UP,
          X_INPUT_GAMEPAD_DPAD_DOWN,
          X_INPUT_GAMEPAD_DPAD_LEFT,
          X_INPUT_GAMEPAD_DPAD_RIGHT,
          // There are additional buttons only available on some controllers.
          // For now just assign sensible defaults
          // Misc:
          X_INPUT_GAMEPAD_GUIDE,
          // Xbox Elite paddles:
          X_INPUT_GAMEPAD_Y,
          X_INPUT_GAMEPAD_B,
          X_INPUT_GAMEPAD_X,
          X_INPUT_GAMEPAD_A,
          // PS touchpad button
          X_INPUT_GAMEPAD_GUIDE,
      };
  static_assert(SDL_GAMEPAD_BUTTON_SOUTH == 0);
  static_assert(SDL_GAMEPAD_BUTTON_DPAD_RIGHT == 14);

  auto idx = GetControllerIndexFromInstanceID(event.gdevice.which);
  assert(idx);
  auto& controller = controllers_.at(*idx);

  uint16_t xbuttons = controller.state.gamepad.buttons;
  // Lookup the XInput button code.
  if (event.gbutton.button >= xbutton_lookup.size()) {
    // A newer SDL Version may have added new buttons.
    REXLOG_INFO("SDL HID: Unknown button was pressed: {}.", event.gbutton.button);
    return;
  }
  auto xbutton = xbutton_lookup.at(event.gbutton.button);
  // Pressed or released?
  if (event.gbutton.down) {
    if (xbutton == X_INPUT_GAMEPAD_GUIDE && !REXCVAR_GET(guide_button)) {
      return;
    }
    xbuttons |= xbutton;
  } else {
    xbuttons &= ~xbutton;
  }
  controller.state.gamepad.buttons = xbuttons;
  controller.state_changed = true;
}

std::optional<size_t> SDLInputDriver::GetControllerIndexFromInstanceID(SDL_JoystickID instance_id) {
  // Loop through our controllers and try to match the given ID.
  for (size_t i = 0; i < controllers_.size(); i++) {
    auto controller = controllers_.at(i).sdl;
    if (!controller) {
      continue;
    }
    auto joystick = SDL_GetGamepadJoystick(controller);
    assert(joystick);
    auto joy_instance_id = SDL_GetJoystickID(joystick);
    assert(joy_instance_id >= 0);
    if (joy_instance_id == instance_id) {
      return i;
    }
  }
  return std::nullopt;
}

SDLInputDriver::ControllerState* SDLInputDriver::GetControllerState(uint32_t user_index) {
  if (user_index >= controllers_.size()) {
    return nullptr;
  }
  auto controller = &controllers_.at(user_index);
  if (!controller->sdl) {
    return nullptr;
  }
  return controller;
}

bool SDLInputDriver::TestSDLVersion() const {
  REXLOG_INFO("SDL: Using version {}.{}.{}", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
              SDL_MICRO_VERSION);
  return true;
}

void SDLInputDriver::UpdateXCapabilities(ControllerState& state) {
  assert(state.sdl);
  uint16_t cap_flags = 0x0;

  // The RAWINPUT driver combines and enhances input from different APIs. For
  // details, see `SDL_rawinputjoystick.c`. This correlation however has latency
  // which might confuse games calling `GetCapabilities()` (The power level is
  // only available after the controller has been "touched"). Generally that
  // should not be a problem, when in doubt disable the RAWINPUT driver via hint
  // (env var).

  if (SDL_GetJoystickConnectionState(SDL_GetGamepadJoystick(state.sdl)) ==
      SDL_JOYSTICK_CONNECTION_WIRELESS) {
    cap_flags |= X_INPUT_CAPS_WIRELESS;
  }

  // Check if all navigational buttons are present
  static constexpr std::array<SDL_GamepadButton, 6> nav_buttons = {
      SDL_GAMEPAD_BUTTON_START,     SDL_GAMEPAD_BUTTON_BACK,      SDL_GAMEPAD_BUTTON_DPAD_UP,
      SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
  };
  for (auto it = nav_buttons.begin(); it < nav_buttons.end(); it++) {
    if (!SDL_GamepadHasButton(state.sdl, *it)) {
      cap_flags |= X_INPUT_CAPS_NO_NAVIGATION;
      break;
    }
  }

  auto& c = state.caps;
  c.type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
  c.sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
  c.flags = cap_flags;
  c.gamepad.buttons = 0xF3FF | (REXCVAR_GET(guide_button) ? X_INPUT_GAMEPAD_GUIDE : 0x0);
  c.gamepad.left_trigger = 0xFF;
  c.gamepad.right_trigger = 0xFF;
  c.gamepad.thumb_lx = static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_ly = static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_rx = static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_ry = static_cast<int16_t>(0xFFFFu);
  c.vibration.left_motor_speed = 0xFFFFu;
  c.vibration.right_motor_speed = 0xFFFFu;
}

void SDLInputDriver::QueueControllerUpdate() {
  // Pump SDL events to ensure controller state is up to date.
  bool is_queued = false;
  sdl_pumpevents_queued_.compare_exchange_strong(is_queued, true);
  if (!is_queued) {
    attached_window_->app_context().CallInUIThread([this]() {
      SDL_PumpEvents();
      sdl_pumpevents_queued_ = false;
    });
  }
}

// Check if the analog inputs exceed their thresholds to become a button press
// and build the bitfield.
inline uint64_t SDLInputDriver::AnalogToKeyfield(const X_INPUT_GAMEPAD& gamepad) const {
  uint64_t f = 0;

  f |= static_cast<uint64_t>(gamepad.left_trigger > HID_SDL_TRIGG_THRES) << 16;
  f |= static_cast<uint64_t>(gamepad.right_trigger > HID_SDL_TRIGG_THRES) << 17;

  auto thumb_x = static_cast<int16_t>(gamepad.thumb_lx);
  auto thumb_y = static_cast<int16_t>(gamepad.thumb_ly);
  for (size_t i = 0; i <= 8; i = i + 8) {
    uint64_t u = thumb_y > HID_SDL_THUMB_THRES;
    uint64_t d = thumb_y < ~HID_SDL_THUMB_THRES;
    uint64_t r = thumb_x > HID_SDL_THUMB_THRES;
    uint64_t l = thumb_x < ~HID_SDL_THUMB_THRES;
    if (u && l) {
      u = l = 0;
      f |= uint64_t(1) << (22 + i);
    }
    if (u && r) {
      u = r = 0;
      f |= uint64_t(1) << (23 + i);
    }
    if (d && r) {
      d = r = 0;
      f |= uint64_t(1) << (24 + i);
    }
    if (d && l) {
      d = l = 0;
      f |= uint64_t(1) << (25 + i);
    }
    f |= u << (18 + i);
    f |= d << (19 + i);
    f |= r << (20 + i);
    f |= l << (21 + i);

    thumb_x = static_cast<int16_t>(gamepad.thumb_rx);
    thumb_y = static_cast<int16_t>(gamepad.thumb_ry);
  }
  return f;
}

}  // namespace rex::input::sdl
