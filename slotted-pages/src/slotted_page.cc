#include "moderndbs/slotted_page.h"

#include <algorithm>
#include <cstring>
#include <queue>
#include <gtest/internal/gtest-internal.h>

using moderndbs::SlottedPage;

SlottedPage::Header::Header(uint32_t page_size)
   : slot_count(0),
     first_free_slot(0),
     data_start(page_size),
     free_space(page_size - sizeof(Header)) {}

SlottedPage::SlottedPage(uint32_t page_size)
   : header(page_size) {
   std::memset(get_data() + sizeof(SlottedPage), 0x00, page_size - sizeof(SlottedPage));
}

std::byte *SlottedPage::get_data() {
   return reinterpret_cast<std::byte*>(this);
}

const std::byte *SlottedPage::get_data() const {
   return reinterpret_cast<const std::byte*>(this);
}

SlottedPage::Slot *SlottedPage::get_slots() {
   return reinterpret_cast<SlottedPage::Slot*>(get_data() + sizeof(SlottedPage));
}

const SlottedPage::Slot *SlottedPage::get_slots() const {
   return reinterpret_cast<const SlottedPage::Slot*>(get_data() + sizeof(SlottedPage));
}

uint32_t SlottedPage::get_fragmented_free_space() const {
   auto slot_end = header.slot_count * sizeof(Slot) + sizeof(Header);

   if (header.data_start < slot_end) {
      throw std::runtime_error("Not enough free space for data");
   }

   return header.data_start - slot_end;
}

uint16_t SlottedPage::allocate(uint32_t data_size, uint32_t page_size) {
   if (get_fragmented_free_space() < sizeof(Slot)) {
      compactify(page_size);
   } else if (get_fragmented_free_space() - sizeof(Slot) < data_size) {
      compactify(page_size);
   }

   // Add to the behind change the data slot
   uint32_t offset = header.data_start - data_size;

   if (offset < sizeof(Header) + (header.slot_count+1)*sizeof(Slot)) {
      throw std::runtime_error("Offset inside the slots");
   }

   // Create slot for the data
   Slot* slots = get_slots();
   slots[header.first_free_slot].set_slot(offset, data_size, false);

   auto slot_id = header.first_free_slot;
   header.data_start -= data_size;
   header.slot_count += header.first_free_slot == header.slot_count ? 1 : 0;
   header.free_space -= data_size + sizeof(Slot);

   header.first_free_slot++;
   for (; header.first_free_slot < header.slot_count; header.first_free_slot++) {
      auto curSlot = slots[header.first_free_slot];
      if (curSlot.is_deleted()) {
         break;
      }
   }

   return slot_id;
}

void SlottedPage::relocate(uint16_t slot_id, uint32_t data_size, uint32_t page_size) {
   Slot* slots = get_slots();
   Slot& slot = slots[slot_id];

   if (slot.is_redirect() || slot.is_deleted()) {
      // Do nothing since the data is not in this page??
      return;
   }

   uint32_t old_size = slot.get_size();
   auto old_offset = slot.get_offset();

   std::vector<std::byte> temp_data(old_size);

   std::memcpy(temp_data.data(), get_data() + old_offset, old_size);

   slot.set_slot(0, 0, false);

   header.free_space += old_size;

   if (get_fragmented_free_space() < data_size) {
      compactify(page_size);
   }

   const uint32_t new_offset = header.data_start - data_size;

   uint32_t len = std::min(old_size, data_size);

   std::memcpy(get_data() + new_offset,
                temp_data.data(),
                len);

   slot.set_slot(new_offset, data_size, slot.is_redirect_target());

   header.data_start -= data_size;
   header.free_space -= data_size;
}

void SlottedPage::erase(uint16_t slot_id) {
   if (slot_id >= header.slot_count) {
      throw std::out_of_range("bad slot_id");
   }

   Slot* slots = get_slots();
   Slot& to_del = slots[slot_id];

   uint32_t size_of_record = to_del.get_size();
   uint32_t offset = to_del.get_offset();

   // Mark as deleted
   slots[slot_id].set_slot(0, 0, false);

   if (offset == header.data_start) {
      header.data_start += size_of_record;
   }

   if (header.slot_count - 1 == slot_id) {
      while (header.slot_count > 0 && slots[header.slot_count - 1].is_deleted()) {
         header.slot_count--;
         header.free_space += sizeof(Slot);
      }
   }

   header.first_free_slot = std::min(header.first_free_slot, slot_id);
   header.free_space += size_of_record;
}

void SlottedPage::compactify(uint32_t page_size) {
   Slot* slots = get_slots();

   std::vector<Slot*> live;
   live.reserve(header.slot_count);
   for (uint32_t i = 0; i < header.slot_count; ++i) {
      Slot& s = slots[i];
      if (!s.is_deleted() && !s.is_redirect()) {
         live.push_back(&s);
      }
   }

   // Sort for offsets
   std::ranges::sort(live,
       [](const Slot* a, const Slot* b) {
           return a->get_offset() > b->get_offset();
       }
   );


   uint32_t last_data_start = page_size;
   for (Slot* s : live) {
      uint32_t size   = s->get_size();
      uint32_t offset = s->get_offset();
      uint32_t target = last_data_start - size;

      if (target != offset) {
         std::memmove(get_data() + target,
                      get_data() + offset,
                      size);
         s->set_slot(target, size, s->is_redirect_target());
      }

      last_data_start = s->get_offset();
   }

   header.data_start = last_data_start;
}
