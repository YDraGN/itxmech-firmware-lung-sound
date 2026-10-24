#pragma once
// RIFF/WAVE PCM header helpers. Pure C++ for host tests.
#include <stddef.h>
#include <stdint.h>
#include "core/Config.h"

namespace wav {

constexpr size_t kHeaderBytes = 44;

void buildHeader(uint8_t out[kHeaderBytes], uint32_t pcmBytes,
                 uint32_t sampleRate = cfg::kSampleRate);
// True only for the exact mono/16-bit/sampleRate layout with this data size.
bool validHeader(const uint8_t h[kHeaderBytes], uint32_t pcmBytes,
                 uint32_t sampleRate = cfg::kSampleRate);

}  // namespace wav
