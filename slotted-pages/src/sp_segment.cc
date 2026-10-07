#include "moderndbs/page_id.h"
#include "moderndbs/segment.h"
#include "moderndbs/slotted_page.h"

using moderndbs::Segment;
using moderndbs::SPSegment;
using moderndbs::TID;

SPSegment::SPSegment(uint16_t segment_id, BufferManager& buffer_manager, SchemaSegment& schema, FSISegment& fsi, schema::Table& table)
   : Segment(segment_id, buffer_manager), schema(schema), fsi(fsi), table(table) {
}

TID SPSegment::allocate(uint32_t size) {
   const auto result_page = fsi.find(size + sizeof(SlottedPage::Slot));
   uint64_t page_id;
   const auto page_size = buffer_manager.get_page_size();

   if (result_page.has_value()) {
      // There was a page with sufficient memory
      page_id = construct_page_id(table.sp_segment, result_page.value());
   } else {
      // No page with memory have to create a new one
      page_id = construct_page_id(table.sp_segment, table.allocated_pages++);
      auto& new_page = buffer_manager.fix_page(page_id, false);
      new (new_page.get_data()) SlottedPage(page_size);
   }

   auto& page = buffer_manager.fix_page(page_id, false);
   auto slottedPage = reinterpret_cast<SlottedPage*>(page.get_data());

   auto slot_id = slottedPage->allocate(size, page_size);

   fsi.update(page_id, slottedPage->header.free_space);

   return {page_id, slot_id};
}

uint32_t SPSegment::read(TID tid, std::byte* record, uint32_t capacity) const {
   uint64_t page_id = tid.get_page_id(table.sp_segment);
   uint64_t slot_id = tid.get_slot();

   const SlottedPage* slottedPage;
   SlottedPage::Slot slot;

   auto& page = buffer_manager.fix_page(page_id, false);
   slottedPage = reinterpret_cast<const SlottedPage*>(page.get_data());
   auto slots = slottedPage->get_slots();
   slot = slots[slot_id];

   if (slot.is_redirect()) {
      tid = slot.as_redirect_tid();
      page_id = tid.get_page_id(table.sp_segment);
      slot_id = tid.get_slot();

      auto& redirected_page = buffer_manager.fix_page(page_id, false);
      slottedPage = reinterpret_cast<const SlottedPage*>(redirected_page.get_data());
      slots = slottedPage->get_slots();
      slot = slots[slot_id];
   }

   uint16_t offset = slot.get_offset();
   uint32_t size = slot.get_size();

   uint32_t bytes_to_read = std::min(capacity, size);

   const std::byte* data = slottedPage->get_data();
   std::memcpy(record, data + offset, bytes_to_read);

   return bytes_to_read;
}

uint32_t SPSegment::write(TID tid, std::byte* record, uint32_t record_size) {
   uint64_t page_id = tid.get_page_id(table.sp_segment);
   uint64_t slot_id = tid.get_slot();

   SlottedPage* slottedPage;
   SlottedPage::Slot slot;

   {
      auto& page = buffer_manager.fix_page(page_id, false);
      slottedPage = reinterpret_cast<SlottedPage*>(page.get_data());
      auto slots = slottedPage->get_slots();
      slot = slots[slot_id];

      if (slot.is_redirect()) {
         tid = slot.as_redirect_tid();
         page_id = tid.get_page_id(table.sp_segment);
         slot_id = tid.get_slot();

         auto& redirected_page = buffer_manager.fix_page(page_id, false);
         slottedPage = reinterpret_cast<SlottedPage*>(redirected_page.get_data());
         slots = slottedPage->get_slots();
         slot = slots[slot_id];
      }
   }

   uint16_t offset = slot.get_offset();
   uint32_t size = slot.get_size();

   uint32_t bytes_to_write = std::min(record_size, size);

   std::memcpy(slottedPage->get_data() + offset, record, bytes_to_write);

   return bytes_to_write;
}

void SPSegment::resize(TID tid, uint32_t new_length) {
   auto page_id = tid.get_page_id(table.sp_segment);
   auto slot_id = tid.get_slot();

   auto& page = buffer_manager.fix_page(page_id, false);
   auto* slottedPage = reinterpret_cast<SlottedPage*>(page.get_data());

   auto slots = slottedPage->get_slots();
   auto& slot = slots[slot_id];

   if (slot.is_deleted() || slot_id > slottedPage->header.slot_count) {
      throw std::runtime_error("SPSegment::resize() called with invalid slot");
   }

   if (slot.is_redirect()) {
      // resolve redirect
      TID redirected_tid = slot.as_redirect_tid();
      auto redirected_page_id = redirected_tid.get_page_id(table.sp_segment);
      auto redirected_slot_id = redirected_tid.get_slot();

      auto& redirected_page = buffer_manager.fix_page(redirected_page_id, false);
      auto* redirected_slottedPage = reinterpret_cast<SlottedPage*>(redirected_page.get_data());

      auto redirected_slots = redirected_slottedPage->get_slots();
      auto& redirected_slot = redirected_slots[redirected_slot_id];

      if (new_length <= redirected_slottedPage->get_free_space()) {
         redirected_slottedPage->relocate(redirected_slot_id, new_length, buffer_manager.get_page_size());
         fsi.update(redirected_page_id, redirected_slottedPage->header.free_space);
      } else {
         std::vector<std::byte> temp_data(new_length);

         auto offset = redirected_slot.get_offset();
         auto old_size = redirected_slot.get_size();

         std::memcpy(temp_data.data(),
                     redirected_slottedPage->get_data() + offset,
                     std::min(old_size, new_length));

         erase(redirected_tid);

         TID new_tid = allocate(new_length);

         auto new_page_id = new_tid.get_page_id(table.sp_segment);
         auto new_slot_id = new_tid.get_slot();

         auto& new_page = buffer_manager.fix_page(new_page_id, false);
         auto* new_slottedPage = reinterpret_cast<SlottedPage*>(new_page.get_data());
         auto new_slots = new_slottedPage->get_slots();
         auto& new_slot = new_slots[new_slot_id];

         new_slot.set_slot(new_slot.get_offset(), new_slot.get_size(), true);
         write(new_tid, temp_data.data(), temp_data.size());

         // Handle the case where there is enough space in the same page
         slot.set_redirect_tid(new_tid);
      }

      return;
   }

   // Not redirected
   uint32_t old_size = slot.get_size();

   if (new_length <= slottedPage->get_free_space()) {
      slottedPage->relocate(slot_id, new_length, buffer_manager.get_page_size());
      fsi.update(page_id, slottedPage->header.free_space);
   } else {
      std::vector<std::byte> temp_data(new_length);

      std::memcpy(temp_data.data(),
                  slottedPage->get_data() + slot.get_offset(),
                  std::min(old_size, new_length));

      TID new_tid = allocate(new_length);
      auto new_page_id = new_tid.get_page_id(table.sp_segment);
      auto new_slot_id = new_tid.get_slot();

      auto& new_page = buffer_manager.fix_page(new_page_id, false);
      auto* new_slottedPage = reinterpret_cast<SlottedPage*>(new_page.get_data());

      auto new_slots = new_slottedPage->get_slots();
      auto& new_slot = new_slots[new_slot_id];

      new_slot.set_slot(new_slot.get_offset(), new_slot.get_size(), true);
      write(new_tid, temp_data.data(), temp_data.size());
      slot.set_redirect_tid(new_tid);
      slottedPage->header.free_space += old_size;
      fsi.update(page_id, slottedPage->header.free_space);
   }
}

void SPSegment::erase(TID tid) {
   auto page_id = tid.get_page_id(table.sp_segment);
   auto slot_id = tid.get_slot();
   auto& page = buffer_manager.fix_page(page_id, false);
   auto slottedPage = reinterpret_cast<SlottedPage*>(page.get_data());
   auto slots = slottedPage->get_slots();

   slottedPage->erase(slot_id);

   fsi.update(page_id, slottedPage->header.free_space);
}
