// SPDX-License-Identifier: MIT
#include "aec_test_support.h"
#include "reference_ring.h"

#include <thread>

namespace {

using ncs::ReferenceRing;

void ReadsBackWhatWasWritten() {
  ReferenceRing ring;
  ring.Allocate(8);
  const float input[] = {1, 2, 3, 4, 5};
  AEC_REQUIRE(ring.Write(input, 5));
  AEC_REQUIRE(ring.Available() == 5);
  float output[5] = {};
  AEC_REQUIRE(ring.Read(output, 5));
  AEC_REQUIRE(std::memcmp(input, output, sizeof(input)) == 0);
  AEC_REQUIRE(ring.Available() == 0);
}

void WrapsAroundTheEnd() {
  ReferenceRing ring;
  ring.Allocate(8);
  float scratch[6] = {};
  const float first[] = {1, 2, 3, 4, 5, 6};
  AEC_REQUIRE(ring.Write(first, 6));
  AEC_REQUIRE(ring.Read(scratch, 6));
  const float second[] = {7, 8, 9, 10, 11, 12};
  AEC_REQUIRE(ring.Write(second, 6));  // crosses the end of the buffer
  AEC_REQUIRE(ring.Read(scratch, 6));
  AEC_REQUIRE(std::memcmp(second, scratch, sizeof(second)) == 0);
}

void WritesAndReadsAreAllOrNothing() {
  ReferenceRing ring;
  ring.Allocate(4);
  const float input[] = {1, 2, 3, 4, 5};
  AEC_REQUIRE(!ring.Write(input, 5));  // larger than the ring
  AEC_REQUIRE(ring.Available() == 0);
  AEC_REQUIRE(ring.Write(input, 3));
  AEC_REQUIRE(!ring.Write(input, 2));  // would overflow: dropped whole
  AEC_REQUIRE(ring.Available() == 3);
  float output[4] = {};
  AEC_REQUIRE(!ring.Read(output, 4));  // not enough data: nothing consumed
  AEC_REQUIRE(ring.Available() == 3);
  AEC_REQUIRE(!ring.Read(output, 0));
}

void SilenceAndDiscardKeepPositionsConsistent() {
  ReferenceRing ring;
  ring.Allocate(16);
  AEC_REQUIRE(ring.WriteSilence(4));
  const float input[] = {5, 6, 7, 8};
  AEC_REQUIRE(ring.Write(input, 4));
  ring.Discard(2);
  float output[6] = {-1, -1, -1, -1, -1, -1};
  AEC_REQUIRE(ring.Read(output, 6));
  AEC_REQUIRE(output[0] == 0.0F && output[1] == 0.0F && output[2] == 5.0F && output[5] == 8.0F);
  ring.Discard(100);  // clamps to what is available
  AEC_REQUIRE(ring.Available() == 0);
  AEC_REQUIRE(ring.Write(input, 4));
  ring.DiscardAll();
  AEC_REQUIRE(ring.Available() == 0);
}

void UnallocatedRingRejectsEverything() {
  ReferenceRing ring;
  const float input[] = {1};
  float output[1] = {};
  AEC_REQUIRE(!ring.Write(input, 1));
  AEC_REQUIRE(!ring.WriteSilence(1));
  AEC_REQUIRE(!ring.Read(output, 1));
  AEC_REQUIRE(ring.Available() == 0);
}

// One producer and one consumer thread transfer a numbered sequence in uneven block sizes; every
// value must arrive exactly once and in order, mirroring AcceptInput/APOProcess concurrency.
void ConcurrentProducerAndConsumerPreserveOrder() {
  ReferenceRing ring;
  ring.Allocate(1031);
  constexpr std::size_t kTotal = 2'000'000;
  std::thread producer([&ring] {
    std::vector<float> block(97);
    std::size_t next = 0;
    while (next < kTotal) {
      const std::size_t count = std::min(block.size(), kTotal - next);
      for (std::size_t i = 0; i < count; ++i) {
        block[i] = static_cast<float>((next + i) % 8'000'000);
      }
      if (ring.Write(block.data(), count)) {
        next += count;
      } else {
        std::this_thread::yield();
      }
    }
  });
  std::vector<float> block(61);
  std::size_t expected = 0;
  bool ordered = true;
  while (expected < kTotal) {
    const std::size_t count = std::min(block.size(), kTotal - expected);
    if (!ring.Read(block.data(), count)) {
      std::this_thread::yield();
      continue;
    }
    for (std::size_t i = 0; i < count; ++i) {
      ordered = ordered && block[i] == static_cast<float>((expected + i) % 8'000'000);
    }
    expected += count;
  }
  producer.join();
  AEC_REQUIRE(ordered);
  AEC_REQUIRE(ring.Available() == 0);
}

}  // namespace

int main() {
  return ncs::aec_test::Run({
      {"reads back what was written", &ReadsBackWhatWasWritten},
      {"wraps around the end", &WrapsAroundTheEnd},
      {"writes and reads are all or nothing", &WritesAndReadsAreAllOrNothing},
      {"silence and discard keep positions consistent", &SilenceAndDiscardKeepPositionsConsistent},
      {"unallocated ring rejects everything", &UnallocatedRingRejectsEverything},
      {"concurrent producer and consumer preserve order", &ConcurrentProducerAndConsumerPreserveOrder},
  });
}
