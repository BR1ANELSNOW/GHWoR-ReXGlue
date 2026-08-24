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

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>

#include <rex/assert.h>
#include <rex/audio/conversion.h>
#include <rex/audio/flags.h>
#include <rex/audio/sdl/sdl_audio_driver.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/perf/counter.h>
#include <SDL3/SDL.h>

REXCVAR_DEFINE_BOOL(audio_mute, false, "Audio", "Mute audio output");

REXCVAR_DEFINE_BOOL(
    audio_sdl_queue_probe, false, "Audio",
    "Log SDL output buffering and producer/consumer counters once per second");

REXCVAR_DEFINE_INT32(
    audio_solo_channel, -1, "Audio",
    "Solo one Xbox 360 render channel to both stereo speakers. "
    "-1=normal mix, 0=FL, 1=FR, 2=FC, 3=LFE, 4=BL, 5=BR.");

namespace rex::audio::sdl {

namespace {
std::atomic<uint64_t> sdl_frames_submitted{0};
std::atomic<uint64_t> sdl_frames_consumed{0};
std::atomic<uint64_t> sdl_silence_frames{0};
std::atomic<uint64_t> sdl_callback_calls{0};
std::atomic<uint64_t> sdl_last_probe_ms{0};
}  // namespace

SDLAudioDriver::SDLAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore)
    : AudioDriver(memory), semaphore_(semaphore) {}

SDLAudioDriver::~SDLAudioDriver() {
  assert_true(frames_queued_.empty());
  assert_true(frames_unused_.empty());
}

bool SDLAudioDriver::Initialize() {
  // Prevent SDL from interfering with timer resolution (causes FPS drops)
  SDL_SetHintWithPriority(SDL_HINT_TIMER_RESOLUTION, "0", SDL_HINT_OVERRIDE);

  // Set audio category for proper OS audio handling
  SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");

  // Set app name for audio device identification
  SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING, "rexglue");

  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    REXAPU_ERROR("SDL_InitSubSystem(SDL_INIT_AUDIO) failed: {}", SDL_GetError());
    return false;
  }
  sdl_initialized_ = true;

  SDL_AudioSpec desired_spec = {};
  SDL_AudioSpec obtained_spec = {};
  desired_spec.freq = frame_frequency_;
  desired_spec.format = SDL_AUDIO_F32LE;
  desired_spec.channels = frame_channels_;
  sdl_device_channels_ = frame_channels_;
  sdl_stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &desired_spec,
                                          SDLCallback, this);
  if (!sdl_stream_) {
    REXAPU_ERROR("SDL_OpenAudioDeviceStream() failed: {}", SDL_GetError());
    return false;
  }

  SDL_AudioDeviceID sdl_device = SDL_GetAudioStreamDevice(sdl_stream_);
  if (!sdl_device) {
    REXAPU_ERROR("SDL_GetAudioStreamDevice() failed: {}", SDL_GetError());
    return false;
  }

  if (!SDL_GetAudioDeviceFormat(sdl_device, &obtained_spec, NULL)) {
    REXAPU_WARN("SDL_GetAudioDeviceFormat() failed: {}", SDL_GetError());
    obtained_spec = desired_spec;
  }

  REXAPU_INFO(
      "SDL audio: requested {} Hz, {} channel(s), format {}; device reports {} Hz, "
      "{} channel(s), format {}",
      desired_spec.freq, desired_spec.channels, int(desired_spec.format), obtained_spec.freq,
      obtained_spec.channels, int(obtained_spec.format));

  if (obtained_spec.channels == 2) {
    SDL_DestroyAudioStream(sdl_stream_);
    sdl_stream_ = nullptr;
    desired_spec.channels = 2;
    sdl_device_channels_ = 2;
    sdl_stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &desired_spec,
                                            SDLCallback, this);
    if (!sdl_stream_) {
      REXAPU_ERROR("SDL_OpenAudioDeviceStream() stereo fallback failed: {}", SDL_GetError());
      return false;
    }
    sdl_device = SDL_GetAudioStreamDevice(sdl_stream_);
    if (!sdl_device) {
      REXAPU_ERROR("SDL_GetAudioStreamDevice() failed after stereo fallback: {}", SDL_GetError());
      return false;
    }
  }

  if (!SDL_ResumeAudioDevice(sdl_device)) {
    REXAPU_ERROR("SDL_ResumeAudioDevice() failed: {}", SDL_GetError());
    return false;
  }

  return true;
}

void SDLAudioDriver::SubmitFrame(uint32_t frame_ptr) {
  const auto input_frame = memory_->TranslateVirtual<float*>(frame_ptr);
  float* output_frame;
  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    if (frames_unused_.empty()) {
      output_frame = new float[frame_samples_];
    } else {
      output_frame = frames_unused_.top();
      frames_unused_.pop();
    }
  }

  std::memcpy(output_frame, input_frame, frame_samples_ * sizeof(float));

  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    frames_queued_.push(output_frame);
    static uint32_t sdl_submit_count = 0;
    if (sdl_submit_count < 10) {
      REXAPU_DEBUG("SDLAudioDriver::SubmitFrame: frame_ptr={:08X} queued_count={}", frame_ptr,
                   frames_queued_.size());
      sdl_submit_count++;
    }
    PROFILE_BUFFER_QUEUE_DEPTH(static_cast<int64_t>(frames_queued_.size()));
  }
  sdl_frames_submitted.fetch_add(1, std::memory_order_relaxed);
}

void SDLAudioDriver::Shutdown() {
  if (sdl_stream_) {
    SDL_DestroyAudioStream(sdl_stream_);
    sdl_stream_ = nullptr;
  }
  if (sdl_initialized_) {
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    sdl_initialized_ = false;
  }
  std::unique_lock<std::mutex> guard(frames_mutex_);
  while (!frames_unused_.empty()) {
    delete[] frames_unused_.top();
    frames_unused_.pop();
  }
  while (!frames_queued_.empty()) {
    delete[] frames_queued_.front();
    frames_queued_.pop();
  }
}

void SDLAudioDriver::SDLCallback(void* userdata, SDL_AudioStream* stream, int additional_amount,
                                 int total_amount) {
  SCOPE_profile_cpu_f("apu");
  if (!userdata || !stream) {
    REXAPU_ERROR("SDLAudioDriver::SDLCallback called with nullptr.");
    return;
  }
  const auto driver = static_cast<SDLAudioDriver*>(userdata);
  const int sample_count =
      static_cast<int>(channel_samples_ * std::max<uint8_t>(driver->sdl_device_channels_, 1));
  const int len = static_cast<int>(sizeof(float) * sample_count);
  float* data = SDL_stack_alloc(float, sample_count);
  if (!data) {
    REXAPU_ERROR("SDLAudioDriver::SDLCallback failed to allocate {} samples", sample_count);
    return;
  }
  const int requested_amount = additional_amount;
  uint64_t callback_consumed = 0;
  uint64_t callback_silence = 0;
  while (additional_amount > 0) {
    static uint32_t sdl_callback_count = 0;
    std::unique_lock<std::mutex> guard(driver->frames_mutex_);
    if (driver->frames_queued_.empty()) {
      if (sdl_callback_count < 10) {
        REXAPU_DEBUG("SDLCallback: no frames queued (silence)");
        sdl_callback_count++;
      }
      std::memset(data, 0, len);
      if (!SDL_PutAudioStreamData(stream, data, len)) {
        REXAPU_ERROR("SDL_PutAudioStreamData() failed while filling silence: {}", SDL_GetError());
        break;
      }
      additional_amount -= len;
      ++callback_silence;
    } else {
      auto buffer = driver->frames_queued_.front();
      driver->frames_queued_.pop();
      if (REXCVAR_GET(audio_mute)) {
        std::memset(data, 0, len);
      } else {
        switch (driver->sdl_device_channels_) {
          case 2: {
            const int32_t solo_channel = REXCVAR_GET(audio_solo_channel);
            if (solo_channel >= 0 && solo_channel < 6) {
              const size_t channel_offset =
                  static_cast<size_t>(solo_channel) * channel_samples_;

              for (size_t sample_index = 0; sample_index < channel_samples_; ++sample_index) {
                const float sample =
                    rex::byte_swap(buffer[channel_offset + sample_index]);

                data[sample_index * 2 + 0] = sample;
                data[sample_index * 2 + 1] = sample;
              }

              static int32_t last_logged_channel = -2;
              if (last_logged_channel != solo_channel) {
                static constexpr const char* kChannelNames[6] = {
                    "FL", "FR", "FC", "LFE", "BL", "BR"};
                REXAPU_WARN("Audio channel probe enabled: {} ({}) -> stereo",
                            solo_channel, kChannelNames[solo_channel]);
                last_logged_channel = solo_channel;
              }
            } else {
              conversion::sequential_6_BE_to_interleaved_2_LE(
                  data, buffer, channel_samples_);
            }
            break;
          }
          case 6:
            conversion::sequential_6_BE_to_interleaved_6_LE(data, buffer, channel_samples_);
            break;
          default:
            assert_unhandled_case(driver->sdl_device_channels_);
            break;
        }
      }
      if (!SDL_PutAudioStreamData(stream, data, len)) {
        REXAPU_ERROR("SDL_PutAudioStreamData() failed: {}", SDL_GetError());
        driver->frames_unused_.push(buffer);
        break;
      }
      driver->frames_unused_.push(buffer);

      auto ret = driver->semaphore_->Release(1, nullptr);
      assert_true(ret);
      additional_amount -= len;
      ++callback_consumed;
    }
  }
  sdl_frames_consumed.fetch_add(callback_consumed, std::memory_order_relaxed);
  sdl_silence_frames.fetch_add(callback_silence, std::memory_order_relaxed);
  sdl_callback_calls.fetch_add(1, std::memory_order_relaxed);

  if (REXCVAR_GET(audio_sdl_queue_probe)) {
    const uint64_t now_ms = SDL_GetTicks();
    uint64_t previous_ms = sdl_last_probe_ms.load(std::memory_order_relaxed);
    if (now_ms - previous_ms >= 1000 &&
        sdl_last_probe_ms.compare_exchange_strong(previous_ms, now_ms,
                                                  std::memory_order_relaxed)) {
      size_t guest_queue_depth = 0;
      {
        std::unique_lock<std::mutex> guard(driver->frames_mutex_);
        guest_queue_depth = driver->frames_queued_.size();
      }
      REXAPU_INFO(
          "SDL AUDIO PROBE: requested={} total={} block={} guest_queue={} "
          "sdl_queued={} sdl_available={} submitted={} consumed={} silence={} callbacks={}",
          requested_amount, total_amount, len, guest_queue_depth,
          SDL_GetAudioStreamQueued(stream), SDL_GetAudioStreamAvailable(stream),
          sdl_frames_submitted.load(std::memory_order_relaxed),
          sdl_frames_consumed.load(std::memory_order_relaxed),
          sdl_silence_frames.load(std::memory_order_relaxed),
          sdl_callback_calls.load(std::memory_order_relaxed));
    }
  }
  SDL_stack_free(data);
}

}  // namespace rex::audio::sdl
