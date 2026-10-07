#include "moderndbs/buffer_manager.h"

#include "moderndbs/file.h"

#include <iostream>

namespace moderndbs {

char* BufferFrame::get_data() {
   auto data = reinterpret_cast<char*>(frameData.data());
   return data;
}

BufferManager::BufferManager(size_t page_size, size_t page_count) {
   this->page_size = page_size;
   this->page_count = page_count;
}

BufferFrame::BufferFrame(uint64_t pageNo, bool isDirty, uint64_t frameSize) {
   this->pageNo = pageNo;
   this->isDirty = isDirty;
   this->frameData.resize(frameSize / sizeof(uint64_t));
}

void BufferManager::write_dirty_page(BufferFrame& frame) const {
   if (!frame.isDirty) {
      return;
   }

   const char* filename = std::to_string(get_segment_id(frame.pageNo)).c_str();
   auto file = File::open_file(filename, File::WRITE);
   uint64_t offset = get_segment_page_id(frame.pageNo) * page_size;
   file->write_block(frame.get_data(), offset, page_size);
}

BufferManager::~BufferManager() {
   // Write dirty stuff back to the disk in the end
   for (auto frame : fifo) {
      if (frame->isDirty) {
         write_dirty_page(*frame);
      }
      delete frame;
   }

   for (auto frame : lru) {
      if (frame->isDirty) {
         write_dirty_page(*frame);
      }
      delete frame;
   }
}

BufferFrame& BufferManager::fix_page(uint64_t page_id, bool exclusive) {
   directory_latch.lock();

   // Case 1 both fifo and lru does not contain page_id have to create new frame and load data
   if (!fifoFrameMap.contains(page_id) && !lruFrameMap.contains(page_id)) {
      auto newFrame = new BufferFrame(page_id, false, page_size);

      // Lock the new frame
      if (exclusive) {
         newFrame->exclusiveLock = true;
         newFrame->latch.lock();
      } else {
         newFrame->latch.lock_shared();
      }

      // Case 1.1 fifo has free space
      if (fifoFrameMap.size() + lruFrameMap.size() < page_count) {
         fifoFrameMap.emplace(page_id, newFrame);
         auto it = fifo.insert(fifo.end(), newFrame);

         newFrame->iterator = it;
         // Increase pin counter
         newFrame->counter += 1;

         // Unlock before reading from disk
         directory_latch.unlock();

         // Read data from disk
         const char* filename = std::to_string(get_segment_id(newFrame->pageNo)).c_str();
         auto file = File::open_file(filename, File::WRITE);
         uint64_t offset = get_segment_page_id(newFrame->pageNo) * page_size;
         file->read_block(offset, page_size, reinterpret_cast<char*>(newFrame->frameData.data()));

         // Return frame
         return *newFrame;
      }

      // Case 1.2 have to evict a page, if it cannot evict throw error
      // Check if something can be removed from fifo
      for (const auto toEvictFrame : fifo) {
         if (toEvictFrame->counter != 0) {
            continue;
         }

         if (!toEvictFrame->latch.try_lock()) {
            continue;
         }

         fifo.erase(toEvictFrame->iterator.value());
         fifoFrameMap.erase(toEvictFrame->pageNo);

         fifoFrameMap.emplace(page_id, newFrame);
         auto it = fifo.insert(fifo.end(), newFrame);

         newFrame->iterator = it;
         // Increase pin counter
         newFrame->counter += 1;
         directory_latch.unlock();

         // Write dirty stuff to disk
         write_dirty_page(*toEvictFrame);
         toEvictFrame->latch.unlock();

         delete toEvictFrame;

         // Read data from disk
         const char* filename = std::to_string(get_segment_id(newFrame->pageNo)).c_str();
         auto file = File::open_file(filename, File::WRITE);
         uint64_t offset = get_segment_page_id(newFrame->pageNo) * page_size;
         file->read_block(offset, page_size, reinterpret_cast<char*>(newFrame->frameData.data()));

         // Return frame
         return *newFrame;
      }

      for (const auto toEvictFrame : lru) {
         if (toEvictFrame->counter != 0) {
            continue;
         }

         if (!toEvictFrame->latch.try_lock()) {
            continue;
         }

         lru.erase(toEvictFrame->iterator.value());
         lruFrameMap.erase(toEvictFrame->pageNo);

         fifoFrameMap.emplace(page_id, newFrame);
         auto it = fifo.insert(fifo.end(), newFrame);

         newFrame->iterator = it;
         // Increase pin counter
         newFrame->counter += 1;
         directory_latch.unlock();

         // Write dirty stuff to disk
         write_dirty_page(*toEvictFrame);
         toEvictFrame->latch.unlock();

         delete toEvictFrame;

         // Read data from disk
         const char* filename = std::to_string(get_segment_id(newFrame->pageNo)).c_str();
         auto file = File::open_file(filename, File::WRITE);
         uint64_t offset = get_segment_page_id(newFrame->pageNo) * page_size;
         file->read_block(offset, page_size, reinterpret_cast<char*>(newFrame->frameData.data()));

         // Return frame
         return *newFrame;
      }

      directory_latch.unlock();
      delete newFrame;
      throw buffer_full_error{};
   }

   // Case 2 page_id already in my fifoFrameMap
   if (fifoFrameMap.contains(page_id)) {
      const auto frame = fifoFrameMap[page_id];
      frame->counter += 1;

      fifoFrameMap.erase(page_id);

      fifo.erase(frame->iterator.value());

      auto it = lru.insert(lru.end(), frame);
      frame->iterator = it;

      lruFrameMap.emplace(page_id, frame);

      directory_latch.unlock();

      if (exclusive) {
         frame->exclusiveLock = true;
         frame->latch.lock();
      } else {
         frame->exclusiveLock = false;
         frame->latch.lock_shared();
      }

      return *frame;
   }

   // Case 3 page_id in lruFrameMap
   if (lruFrameMap.contains(page_id)) {
      const auto frame = lruFrameMap[page_id];

      lru.erase(frame->iterator.value());
      auto it = lru.insert(lru.end(), frame);
      frame->iterator = it;

      frame->counter += 1;

      directory_latch.unlock();

      if (exclusive) {
         frame->exclusiveLock = true;
         frame->latch.lock();
      } else {
         frame->exclusiveLock = false;
         frame->latch.lock_shared();
      }

      return *frame;
   }

   directory_latch.unlock();
   throw buffer_full_error{};
}

void BufferManager::unfix_page(BufferFrame& page, bool is_dirty) {
   page.isDirty = is_dirty;
   page.counter--;

   if (page.exclusiveLock) {
      page.latch.unlock();
   } else {
      page.latch.unlock_shared();
   }
}

std::vector<uint64_t> BufferManager::get_fifo_list() const {
   std::vector<uint64_t> out;
   for (auto frame : fifo) {
      out.push_back(frame->pageNo);
   }
   return out;
}

std::vector<uint64_t> BufferManager::get_lru_list() const {
   std::vector<uint64_t> out;
   for (auto frame : lru) {
      out.push_back(frame->pageNo);
   }
   return out;
}

} // namespace moderndbs
