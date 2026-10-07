#ifndef INCLUDE_MODERNDBS_BUFFER_MANAGER_H
#define INCLUDE_MODERNDBS_BUFFER_MANAGER_H

#include <condition_variable>
#include <exception>
#include <list>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace moderndbs {

class BufferFrame {
   private:
   friend class BufferManager;

   // Page Number to identify
   uint64_t pageNo;
   // Latch for locking
   mutable std::shared_mutex latch;
   // Check whether page is dirty and has to written to disk
   bool isDirty = false;
   // The data in page
   std::vector<uint64_t> frameData;
   // Pin Count
   int counter = 0;

   bool exclusiveLock = false;

   // Gives where it is inside the queues
   std::optional<std::list<BufferFrame*>::iterator> iterator;

   // bool lru

   public:
   /// Returns a pointer to this page's data.
   BufferFrame(uint64_t pageNo, bool isDirty, uint64_t frameSize);

   char* get_data();
};

class buffer_full_error
   : public std::exception {
   public:
   [[nodiscard]] const char* what() const noexcept override {
      return "buffer is full";
   }
};

class BufferManager {
   private:
   // fifo
   std::list<BufferFrame*> fifo;
   // lru
   std::list<BufferFrame*> lru;

   // fifo frame map
   std::unordered_map<uint64_t, BufferFrame*> fifoFrameMap;
   // lru frame map
   std::unordered_map<uint64_t, BufferFrame*> lruFrameMap;

   // global directory latch
   mutable std::mutex directory_latch;

   // page size
   // page count
   size_t page_size, page_count;

   public:
   BufferManager(const BufferManager&) = delete;
   BufferManager(BufferManager&&) = delete;
   BufferManager& operator=(const BufferManager&) = delete;
   BufferManager& operator=(BufferManager&&) = delete;
   /// Constructor.
   /// @param[in] page_size  Size in bytes that all pages will have.
   /// @param[in] page_count Maximum number of pages that should reside in
   //                        memory at the same time.
   BufferManager(size_t page_size, size_t page_count);

   /// Destructor. Writes all dirty pages to disk.
   ~BufferManager();

   /// Returns a reference to a `BufferFrame` object for a given page id. When
   /// the page is not loaded into memory, it is read from disk. Otherwise the
   /// loaded page is used.
   /// When the page cannot be loaded because the buffer is full, throws the
   /// exception `buffer_full_error`.
   /// Is thread-safe w.r.t. other concurrent calls to `fix_page()` and
   /// `unfix_page()`.
   /// @param[in] page_id   Page id of the page that should be loaded.
   /// @param[in] exclusive If `exclusive` is true, the page is locked
   ///                      exclusively. Otherwise it is locked
   ///                      non-exclusively (shared).
   BufferFrame& fix_page(uint64_t page_id, bool exclusive);

   /// Takes a `BufferFrame` reference that was returned by an earlier call to
   /// `fix_page()` and unfixes it. When `is_dirty` is / true, the page is
   /// written back to disk eventually.
   void unfix_page(BufferFrame& page, bool is_dirty);

   // Writes dirty stuff to disk
   void write_dirty_page(BufferFrame& page) const;

   // Evicts frame
   void evict_frame(BufferFrame& frame) const;

   /// Returns the page ids of all pages (fixed and unfixed) that are in the
   /// FIFO list in FIFO order.
   /// Is not thread-safe.
   [[nodiscard]] std::vector<uint64_t> get_fifo_list() const;

   /// Returns the page ids of all pages (fixed and unfixed) that are in the
   /// LRU list in LRU order.
   /// Is not thread-safe.
   [[nodiscard]] std::vector<uint64_t> get_lru_list() const;

   /// Returns the segment id for a given page id which is contained in the 16
   /// most significant bits of the page id.
   static constexpr uint16_t get_segment_id(uint64_t page_id) {
      return page_id >> 48;
   }

   /// Returns the page id within its segment for a given page id. This
   /// corresponds to the 48 least significant bits of the page id.
   static constexpr uint64_t get_segment_page_id(uint64_t page_id) {
      return page_id & ((1ull << 48) - 1);
   }
};

} // namespace moderndbs

#endif
