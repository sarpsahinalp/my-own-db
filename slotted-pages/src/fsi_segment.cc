#include "moderndbs/page_id.h"
#include "moderndbs/segment.h"

using FSISegment = moderndbs::FSISegment;

FSISegment::FSISegment(uint16_t segment_id, BufferManager& buffer_manager, schema::Table& table)
   : Segment(segment_id, buffer_manager), table(table) {
}

// I use linear scaling
uint8_t FSISegment::encode_free_space(uint32_t free_space) {
   uint64_t page_size = buffer_manager.get_page_size();
   uint64_t bits = 1ULL << 4;
   // each block size
   uint64_t divisor = page_size / bits;
   return (free_space / divisor) & 0x0F;
}

uint32_t FSISegment::decode_free_space(uint8_t free_space) {
   uint64_t page_size = buffer_manager.get_page_size();
   uint64_t bits = 1ULL << 4;
   // each block size
   uint64_t divisor = page_size / bits;
   return free_space * divisor;
}

void FSISegment::update(uint64_t target_page, uint32_t free_space) {
   target_page = get_segment_page_id(target_page);

   uint8_t encoded = encode_free_space(free_space);
   auto index = (target_page / 2) % buffer_manager.get_page_size();

   auto page_id = construct_page_id(table.fsi_segment, target_page / (buffer_manager.get_page_size() * 2));
   auto& fsi_page = buffer_manager.fix_page(page_id, false);

   auto* bitmap = reinterpret_cast<uint8_t*>(fsi_page.get_data());

   uint8_t& two_pages = bitmap[index];

   if (target_page % 2 == 0) {
      // Update the first (upper 4 bits)
      uint8_t lower = two_pages & 0x0F;
      uint8_t upper = encoded << 4;
      bitmap[index] = upper | lower;
   } else {
      // Update the second (lower 4 bits)
      uint8_t lower = encoded & 0x0F;
      uint8_t upper = two_pages & 0xF0;
      bitmap[index] = upper | lower;
   }
}

std::optional<uint64_t> FSISegment::find(uint32_t required_space) {
   // Go through the bitmap until there is enough space!!!!
   size_t pageNumber = 0;
   auto fsi_page_number = 0;

   auto page_id = construct_page_id(table.fsi_segment, fsi_page_number);
   auto fsi_page = buffer_manager.fix_page(page_id, false);

   auto* bitmap = reinterpret_cast<uint8_t*>(fsi_page.get_data());

   while (pageNumber < table.allocated_pages) {
      auto index = (pageNumber / 2) % buffer_manager.get_page_size();
      auto pair_page = bitmap[index];

      // Check first page
      auto first_page_space = (pair_page >> 4) & 0x0F;

      auto free_space = decode_free_space(first_page_space);

      if (free_space >= required_space) {
         return pageNumber;
      }

      pageNumber++;

      auto second_page_space = pair_page & 0x0F;

      free_space = decode_free_space(second_page_space);

      if (free_space >= required_space) {
         return pageNumber;
      }

      pageNumber++;

      auto need_new_fsi_page = pageNumber / (buffer_manager.get_page_size() * 2);

      if (need_new_fsi_page > fsi_page_number) {
         page_id = construct_page_id(table.fsi_segment, pageNumber / (buffer_manager.get_page_size() * 2));
         fsi_page = buffer_manager.fix_page(page_id, false);
         fsi_page_number = need_new_fsi_page;
         bitmap = reinterpret_cast<uint8_t*>(fsi_page.get_data());
      }
   }

   return std::nullopt;
}
