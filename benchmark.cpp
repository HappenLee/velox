/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "velox/dwio/parquet/reader/BooleanDecoder.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>
#include <vector>

using facebook::velox::parquet::BooleanDecoder;
template <typename T>
class BenchmarkVisitor {
 public:
  using DataType = T;
  static constexpr bool dense = true;

  BenchmarkVisitor(T* output, int32_t numRows)
      : output_(output), numRows_(numRows) {}

  bool allowNulls() const {
    return false;
  }

  int32_t start() {
    return 0;
  }

  int32_t checkAndSkipNulls(const uint64_t*, int32_t&, bool&) {
    return 0;
  }

  int32_t processNull(bool& atEnd) {
    atEnd = (++outputIdx_ >= numRows_);
    return 0;
  }

  int32_t process(T value, bool& atEnd) {
    output_[outputIdx_++] = value;
    atEnd = (outputIdx_ >= numRows_);
    return 0;
  }

 private:
  T* output_;
  int32_t numRows_;
  int32_t outputIdx_ = 0;
};

static void require(bool condition) {
  if (!condition) {
    std::abort();
  }
}

template <bool bulk>
[[gnu::noinline]] void decode(const char* data, const char* end, int8_t* output, int total, int batch) {
  BooleanDecoder decoder(data, end);
  for (int offset = 0; offset < total; offset += batch) {
    const int size = std::min(batch, total - offset);
    if constexpr (bulk) {
      decoder.readDense(size, output + offset);
    } else {
      decoder.readWithVisitor<false>(nullptr, BenchmarkVisitor(output + offset, size));
    }
  }
}

void verify() {
  std::array<char, 102> bytes;
  std::mt19937 rng(42);
  for (auto& byte : bytes) {
    byte = static_cast<char>(rng());
  }
  const char* start = bytes.data() + 1;
  for (int count = 1; count <= 793; ++count) {
    const char* end = start + (count + 7) / 8;
    for (int batch : {1, 3, 7, 8, 9, 15, 27, 31, 32, 33, 67, 127, 128, count}) {
      std::vector<int8_t> output(count + 2, -1);
      decode<true>(start, end, output.data() + 1, count, batch);
      require(output.front() == -1 && output.back() == -1);
      for (int i = 0; i < count; ++i) {
        require(output[i + 1] == ((static_cast<uint8_t>(start[i / 8]) >> (i % 8)) & 1));
      }
    }
  }
  for (int skipped = 0; skipped < 128; ++skipped) {
    BooleanDecoder decoder(start, start + 16);
    decoder.skip(skipped);
    std::array<int8_t, 128> out;
    decoder.readDense(128 - skipped, out.data());
    for (int i = skipped; i < 128; ++i) {
      require(out[i - skipped] == ((static_cast<uint8_t>(start[i / 8]) >> (i % 8)) & 1));
    }
    decoder.reset(start + 1, start + 2);
    decoder.readDense(8, out.data());
    for (int i = 0; i < 8; ++i) {
      require(out[i] == ((static_cast<uint8_t>(start[1]) >> i) & 1));
    }
  }
  for (int remaining = 0; remaining <= 7; ++remaining) {
    auto* oneByte = new char[1]{0x75};
    BooleanDecoder decoder(oneByte, oneByte + 1);
    std::array<int8_t, 16> out;
    decoder.readDense(8 - remaining, out.data());
    decoder.readDense(remaining, out.data());
    for (int i = 0; i < remaining; ++i) {
      require(out[i] == ((0x75 >> (8 - remaining + i)) & 1));
    }
    delete[] oneByte;
  }
  {
    BooleanDecoder decoder(start, start + 1);
    std::array<int8_t, 9> output;
    bool threw = false;
    try {
      decoder.readDense(9, output.data());
    } catch (const facebook::velox::VeloxUserError&) {
      threw = true;
    }
    require(threw);
  }
  std::puts("Decoder correctness checks passed");
}

int64_t threadTime() {
  timespec t;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
  return t.tv_sec * int64_t{1'000'000'000} + t.tv_nsec;
}

template <bool bulk>
double measure(const std::vector<char>& bytes, std::vector<int8_t>& output, int batch) {
  const auto total = static_cast<int>(output.size());
  const auto repeats = std::max(128, 20'000'000 / total);
  const auto begin = threadTime();
  for (int i = 0; i < repeats; ++i) {
    decode<bulk>(bytes.data(), bytes.data() + bytes.size(), output.data(), total, batch);
    asm volatile("" : : "g"(output.data()) : "memory");
  }
  return double(threadTime() - begin) / (repeats * int64_t{total});
}

int main(int argc, char**) {
  verify();
  if (argc > 1) {
    return 0;
  }
  std::mt19937 rng(42);
  std::puts("values per stream | batch | baseline (ns/value) | proposed (ns/value) | speedup");
  for (const auto [total, batch] : {std::pair{8, 8}, {128, 128}, {4096, 4096}, {100000, 100000}, {100000, 4096}, {100000, 127}, {1000000, 4096}}) {
    std::vector<char> bytes((total + 7) / 8);
    for (auto& byte : bytes) {
      byte = static_cast<char>(rng());
    }
    std::vector<int8_t> baseline(total), proposed(total);
    decode<false>(bytes.data(), bytes.data() + bytes.size(), baseline.data(), total, batch);
    decode<true>(bytes.data(), bytes.data() + bytes.size(), proposed.data(), total, batch);
    require(baseline == proposed);
    std::array<double, 11> baseSamples, newSamples;
    for (size_t sample = 0; sample < baseSamples.size(); ++sample) {
      if ((sample & 1) == 0) {
        baseSamples[sample] = measure<false>(bytes, baseline, batch);
        newSamples[sample] = measure<true>(bytes, proposed, batch);
      } else {
        newSamples[sample] = measure<true>(bytes, proposed, batch);
        baseSamples[sample] = measure<false>(bytes, baseline, batch);
      }
    }
    std::ranges::sort(baseSamples);
    std::ranges::sort(newSamples);
    auto x = baseSamples[baseSamples.size() / 2];
    auto y = newSamples[newSamples.size() / 2];
    std::printf("%d | %d | %.4f | %.4f | %.2fx\n", total, batch, x, y, x / y);
  }
}
