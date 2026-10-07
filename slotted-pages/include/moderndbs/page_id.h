#pragma once
#include <cstdint>

namespace moderndbs {

static constexpr uint64_t construct_page_id(uint16_t segment_id, uint64_t segment_page_id) {
   return (static_cast<uint64_t>(segment_id) << 48) | segment_page_id;
}

static constexpr uint16_t get_segment_id(uint64_t page_id) {
   return page_id >> 48;
}

static constexpr uint64_t get_segment_page_id(uint64_t page_id) {
   return page_id & ((1ull << 48) - 1);
}

}  // namespace moderndbs
