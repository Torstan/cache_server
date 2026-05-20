#include "expire/expire_sweeper.h"

namespace expire {
namespace {

constexpr std::size_t kMaxExpiredKeysPerSlot = 64;

}  // namespace

ExpireSweeper::ExpireSweeper(cache::CacheEngine* engine,
                             std::size_t slots_per_tick)
    : engine_(engine), slots_per_tick_(slots_per_tick) {}

std::size_t ExpireSweeper::SweepOnce(std::uint64_t now_us) {
  if (engine_ == nullptr || slots_per_tick_ == 0 || engine_->SlotCount() == 0) {
    return 0;
  }

  std::size_t deleted = 0;
  const std::size_t slot_count = engine_->SlotCount();
  for (std::size_t i = 0; i < slots_per_tick_; ++i) {
    deleted += engine_->DeleteExpiredInSlot(next_slot_, kMaxExpiredKeysPerSlot,
                                            now_us);
    next_slot_ = (next_slot_ + 1) % slot_count;
  }
  return deleted;
}

}  // namespace expire
