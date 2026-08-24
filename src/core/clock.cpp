/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2019 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_set>
#include <unordered_map>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/math.h>

REXCVAR_DEFINE_BOOL(clock_no_scaling, false, "Clock",
                    "Disable clock scaling (inverted: false = scaling enabled)");

REXCVAR_DEFINE_BOOL(clock_source_raw, false, "Clock", "Use raw clock source without scaling");

REXCVAR_DEFINE_BOOL(
    ghwor_sync_clock_probe, false, "Clock",
    "Diagnostic only: log host, guest raw and GHWoR scaled game time once per second");

REXCVAR_DEFINE_DOUBLE(
    game_timebase_scale, 1.0, "Clock",
    "Scale applied only to mftb reads made by recompiled game code")
    .range(0.95, 1.05);

namespace rex::chrono {

// Time scalar applied to all time operations.
double guest_time_scalar_ = 1.0;
// Tick frequency of guest.
uint64_t guest_tick_frequency_ = Clock::host_tick_frequency_platform();
// Base FILETIME of the guest system from app start.
uint64_t guest_system_time_base_ = Clock::QueryHostSystemTime();
// Combined time and frequency ratio between host and guest.
// Split in numerator (first) and denominator (second).
// Computed by RecomputeGuestTickScalar.
std::pair<uint64_t, uint64_t> guest_tick_ratio_ = std::make_pair(1, 1);

// Native guest ticks.
uint64_t last_guest_tick_count_ = 0;
// Last sampled host tick count.
uint64_t last_host_tick_count_ = Clock::QueryHostTickCount();
// Mutex to ensure last_host_tick_count_ and last_guest_tick_count_ are in sync
std::mutex tick_mutex_;

// GHWoR synchronization diagnostic. This is intentionally read-only: it never
// changes the guest clock, the game timebase scale or scheduling.
bool ghwor_sync_probe_started_ = false;
uint64_t ghwor_sync_probe_host_base_ = 0;
uint64_t ghwor_sync_probe_guest_base_ = 0;
uint64_t ghwor_sync_probe_last_host_ = 0;

void ProbeGHWoRSyncClock(uint64_t host_tick_count, uint64_t guest_tick_count) {
  if (!REXCVAR_GET(ghwor_sync_clock_probe)) {
    return;
  }

  const uint64_t host_frequency = Clock::QueryHostTickFrequency();
  if (!ghwor_sync_probe_started_) {
    ghwor_sync_probe_started_ = true;
    ghwor_sync_probe_host_base_ = host_tick_count;
    ghwor_sync_probe_guest_base_ = guest_tick_count;
    ghwor_sync_probe_last_host_ = host_tick_count;
    REXSYS_INFO(
        "GHWOR SYNC CLOCK PROBE START: host_hz={} guest_hz={} ratio={}/{} game_scale={:.9f}",
        host_frequency, guest_tick_frequency_, guest_tick_ratio_.first,
        guest_tick_ratio_.second, REXCVAR_GET(game_timebase_scale));
    return;
  }

  if (host_tick_count - ghwor_sync_probe_last_host_ < host_frequency) {
    return;
  }
  ghwor_sync_probe_last_host_ = host_tick_count;

  const uint64_t host_delta = host_tick_count - ghwor_sync_probe_host_base_;
  const uint64_t guest_delta = guest_tick_count - ghwor_sync_probe_guest_base_;
  const long double host_ms =
      static_cast<long double>(host_delta) * 1000.0L /
      static_cast<long double>(host_frequency);
  const long double guest_ms =
      static_cast<long double>(guest_delta) * 1000.0L /
      static_cast<long double>(guest_tick_frequency_);
  const long double scale = static_cast<long double>(
      std::clamp(REXCVAR_GET(game_timebase_scale), 0.95, 1.05));
  const long double game_ms = guest_ms * scale;

  REXSYS_INFO(
      "GHWOR SYNC CLOCK PROBE: host_ms={:.3f} guest_ms={:.3f} game_ms={:.3f} "
      "guest_minus_host_ms={:+.3f} game_minus_host_ms={:+.3f} game_scale={:.9f}",
      static_cast<double>(host_ms), static_cast<double>(guest_ms),
      static_cast<double>(game_ms), static_cast<double>(guest_ms - host_ms),
      static_cast<double>(game_ms - host_ms), static_cast<double>(scale));
}

void RecomputeGuestTickScalar() {
  // Create a rational number with numerator (first) and denominator (second)
  auto frac = std::make_pair(guest_tick_frequency_, Clock::QueryHostTickFrequency());
  // Doing it this way ensures we don't mess up our frequency scaling and
  // precisely controls the precision the guest_time_scalar_ can have.
  if (guest_time_scalar_ > 1.0) {
    frac.first *= static_cast<uint64_t>(guest_time_scalar_ * 10.0);
    frac.second *= 10;
  } else {
    frac.first *= 10;
    frac.second *= static_cast<uint64_t>(10.0 / guest_time_scalar_);
  }
  // Keep this a rational calculation and reduce the fraction
  reduce_fraction(frac);

  std::lock_guard<std::mutex> lock(tick_mutex_);
  guest_tick_ratio_ = frac;
}

// Update the guest timer for all threads.
// Return a copy of the value so locking is reduced.
uint64_t UpdateGuestClock() {
  uint64_t host_tick_count = Clock::QueryHostTickCount();

  if (REXCVAR_GET(clock_no_scaling)) {
    // Nothing to update, calculate on the fly
    return host_tick_count * guest_tick_ratio_.first / guest_tick_ratio_.second;
  }

  std::unique_lock<std::mutex> lock(tick_mutex_, std::defer_lock);
  if (lock.try_lock()) {
    // Translate host tick count to guest tick count.
    uint64_t host_tick_delta =
        host_tick_count > last_host_tick_count_ ? host_tick_count - last_host_tick_count_ : 0;
    last_host_tick_count_ = host_tick_count;
    uint64_t guest_tick_delta =
        host_tick_delta * guest_tick_ratio_.first / guest_tick_ratio_.second;
    last_guest_tick_count_ += guest_tick_delta;
    ProbeGHWoRSyncClock(host_tick_count, last_guest_tick_count_);
    return last_guest_tick_count_;
  } else {
    // Wait until another thread has finished updating the clock.
    lock.lock();
    return last_guest_tick_count_;
  }
}

// Offset of the current guest system file time relative to the guest base time.
inline uint64_t QueryGuestSystemTimeOffset() {
  if (REXCVAR_GET(clock_no_scaling)) {
    return Clock::QueryHostSystemTime() - guest_system_time_base_;
  }

  auto guest_tick_count = UpdateGuestClock();

  uint64_t numerator = 10000000;  // 100ns/10MHz resolution
  uint64_t denominator = guest_tick_frequency_;
  reduce_fraction(numerator, denominator);

  return guest_tick_count * numerator / denominator;
}

uint64_t Clock::QueryHostTickFrequency() {
#if REX_CLOCK_RAW_AVAILABLE
  if (REXCVAR_GET(clock_source_raw)) {
    return host_tick_frequency_raw();
  }
#endif
  return host_tick_frequency_platform();
}
uint64_t Clock::QueryHostTickCount() {
#if REX_CLOCK_RAW_AVAILABLE
  if (REXCVAR_GET(clock_source_raw)) {
    return host_tick_count_raw();
  }
#endif
  return host_tick_count_platform();
}

double Clock::guest_time_scalar() {
  return guest_time_scalar_;
}

void Clock::set_guest_time_scalar(double scalar) {
  if (REXCVAR_GET(clock_no_scaling)) {
    return;
  }

  guest_time_scalar_ = scalar;
  RecomputeGuestTickScalar();
}

std::pair<uint64_t, uint64_t> Clock::guest_tick_ratio() {
  std::lock_guard<std::mutex> lock(tick_mutex_);
  return guest_tick_ratio_;
}

uint64_t Clock::guest_tick_frequency() {
  return guest_tick_frequency_;
}

void Clock::set_guest_tick_frequency(uint64_t frequency) {
  guest_tick_frequency_ = frequency;
  RecomputeGuestTickScalar();
}

uint64_t Clock::guest_system_time_base() {
  return guest_system_time_base_;
}

void Clock::set_guest_system_time_base(uint64_t time_base) {
  guest_system_time_base_ = time_base;
}

uint64_t Clock::QueryGuestTickCount() {
  auto guest_tick_count = UpdateGuestClock();
  return guest_tick_count;
}

uint64_t Clock::QueryGameTimebaseTickCount() {
  const uint64_t guest_tick_count = UpdateGuestClock();
  const long double scale = static_cast<long double>(
      std::clamp(REXCVAR_GET(game_timebase_scale), 0.95, 1.05));
  return static_cast<uint64_t>(static_cast<long double>(guest_tick_count) * scale);
}

uint64_t Clock::QueryGameTimebaseTickCountAt(const char* source_file,
                                             uint32_t source_line) {
  (void)source_file;
  (void)source_line;
  return QueryGameTimebaseTickCount();
}
uint64_t Clock::QueryGuestSystemTime() {
  if (REXCVAR_GET(clock_no_scaling)) {
    return Clock::QueryHostSystemTime();
  }

  auto guest_system_time_offset = QueryGuestSystemTimeOffset();
  return guest_system_time_base_ + guest_system_time_offset;
}

uint32_t Clock::QueryGuestUptimeMillis() {
  return static_cast<uint32_t>(std::min<uint64_t>(QueryGuestSystemTimeOffset() / 10000,
                                                  std::numeric_limits<uint32_t>::max()));
}

void Clock::SetGuestSystemTime(uint64_t system_time) {
  if (REXCVAR_GET(clock_no_scaling)) {
    // Time is fixed to host time.
    return;
  }

  // Query the filetime offset to calculate a new base time.
  auto guest_system_time_offset = QueryGuestSystemTimeOffset();
  guest_system_time_base_ = system_time - guest_system_time_offset;
}

uint32_t Clock::ScaleGuestDurationMillis(uint32_t guest_ms) {
  if (REXCVAR_GET(clock_no_scaling)) {
    return guest_ms;
  }

  constexpr uint64_t max = std::numeric_limits<uint32_t>::max();

  if (guest_ms >= max) {
    return max;
  } else if (!guest_ms) {
    return 0;
  }
  uint64_t scaled_ms =
      static_cast<uint64_t>((static_cast<uint64_t>(guest_ms) * guest_time_scalar_));
  return static_cast<uint32_t>(std::min(scaled_ms, max));
}

int64_t Clock::ScaleGuestDurationFileTime(int64_t guest_file_time) {
  if (REXCVAR_GET(clock_no_scaling)) {
    return static_cast<uint64_t>(guest_file_time);
  }

  if (!guest_file_time) {
    return 0;
  } else if (guest_file_time > 0) {
    // Absolute time.
    uint64_t guest_time = Clock::QueryGuestSystemTime();
    int64_t relative_time = guest_file_time - static_cast<int64_t>(guest_time);
    int64_t scaled_time = static_cast<int64_t>(relative_time * guest_time_scalar_);
    return static_cast<int64_t>(guest_time) + scaled_time;
  } else {
    // Relative time.
    uint64_t scaled_file_time =
        static_cast<uint64_t>((static_cast<uint64_t>(guest_file_time) * guest_time_scalar_));
    // TODO(benvanik): check for overflow?
    return scaled_file_time;
  }
}

void Clock::ScaleGuestDurationTimeval(int32_t* tv_sec, int32_t* tv_usec) {
  if (REXCVAR_GET(clock_no_scaling)) {
    return;
  }

  uint64_t scaled_sec = static_cast<uint64_t>(static_cast<uint64_t>(*tv_sec) * guest_time_scalar_);
  uint64_t scaled_usec =
      static_cast<uint64_t>(static_cast<uint64_t>(*tv_usec) * guest_time_scalar_);
  if (scaled_usec > std::numeric_limits<uint32_t>::max()) {
    uint64_t overflow_sec = scaled_usec / 1000000;
    scaled_usec -= overflow_sec * 1000000;
    scaled_sec += overflow_sec;
  }
  *tv_sec = int32_t(scaled_sec);
  *tv_usec = int32_t(scaled_usec);
}

}  // namespace rex::chrono


