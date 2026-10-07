#ifndef INCLUDE_MODERNDBS_BTREE_H
#define INCLUDE_MODERNDBS_BTREE_H

#include "moderndbs/buffer_manager.h"
#include "moderndbs/segment.h"
#include <optional>
#include <ranges>
#include <algorithm>
#include <functional>
#include <utility>

namespace moderndbs {

template <typename KeyT, typename ValueT, typename ComparatorT, size_t PageSize>
struct BTree : public Segment {
   struct Node {
      /// The level in the tree.
      uint16_t level;
      /// The number of children.
      uint16_t count;

      // Constructor
      Node(uint16_t level, uint16_t count)
         : level(level), count(count) {}

      /// Is the node a leaf node?
      bool is_leaf() const { return level == 0; }
   };

   struct InnerNode : public Node {
      /// The capacity of a node.
      // Try to fit the page here
      static constexpr uint32_t kCapacity = (PageSize - sizeof(uint64_t)) / (sizeof(KeyT) + sizeof(uint64_t));

      /// The keys.
      KeyT keys[kCapacity];
      /// The values.
      uint64_t children[kCapacity + 1];

      /// Constructor.
      InnerNode() : Node(0, 0) {}

      /// Get the index of the first key that is not less than than a provided key.
      /// @param[in] key          The key that should be inserted.
      std::pair<uint32_t, bool> lower_bound(const KeyT& key) {
         auto iterator = std::lower_bound(keys, keys + this->count - 1, key, ComparatorT{});
         uint32_t index = iterator - keys;
         return {index, iterator != keys + this->count};
      }

      /// Insert a key.
      /// @param[in] key          The key that should be inserted.
      /// @param[in] split_page   The child that should be inserted.
      void insert_split(const KeyT& key, uint64_t split_page) {
         auto [pos, found] = this->lower_bound(key);

         if (found) {
            // Shift to back
            std::move_backward(keys + pos, keys + this->count - 1, keys + this->count);

            // Shift to back
            std::move_backward(children + (pos + 1), children + this->count, children + this->count + 1);

            // Insert key and value
            keys[pos] = key;
            children[pos + 1] = split_page;
         } else {
            // Not found so insert in the end
            keys[this->count - 1] = key;
            children[this->count] = split_page;
         }

         ++this->count;
      }

      /// Split the node.
      /// @param[in] buffer       The buffer for the new page.
      /// @return                 The separator key.
      KeyT split(std::byte* buffer) {
         auto mid = (this->count + 1) / 2;

         auto* next_node = new (buffer) InnerNode();

         auto moved_length = this->count - mid;

         // Move the data to the new leafnode
         std::move(
            keys + mid,
            keys + this->count - 1,
            next_node->keys);
         std::move(
            children + mid,
            children + this->count,
            next_node->children);

         this->count -= moved_length;
         next_node->count += moved_length;

         return keys[this->count - 1];
      }

      /// Returns the keys.
      std::vector<KeyT> get_key_vector() {
         return std::vector<KeyT>(keys, keys + this->count);
      }
   };

   struct LeafNode : public Node {
      /// The capacity of a node.
      static constexpr uint32_t kCapacity = (PageSize - sizeof(LeafNode*) - sizeof(ValueT)) / (sizeof(KeyT) + sizeof(ValueT));

      /// The keys.
      KeyT keys[kCapacity]; // adjust this
      /// The values.
      ValueT values[kCapacity]; // adjust this

      // Points to the next leaf node if exists
      LeafNode* next;

      /// Constructor.
      LeafNode() : Node(0, 0) {}

      /// Get the index of the first key that is not less than than a provided key.
      std::pair<uint32_t, bool> lower_bound(const KeyT& key) {
         auto iterator = std::lower_bound(keys, keys + this->count, key, ComparatorT{});
         uint32_t index = iterator - keys;
         return {index, iterator != keys + this->count};
      }

      /// Insert a key.
      /// @param[in] key          The key that should be inserted.
      /// @param[in] value        The value that should be inserted.
      void insert(const KeyT& key, const ValueT& value) {
         // Check if there is capacity
         if (this->count < kCapacity) {
            // Get the index to insert to
            auto index_lower_bound = lower_bound(key);

            if (index_lower_bound.second) {
               // Shift to back
               std::move_backward(keys + index_lower_bound.first, keys + this->count, keys + this->count + 1);

               // Shift to back
               std::move_backward(values + index_lower_bound.first, values + this->count, values + this->count + 1);

               // Insert key and value
               keys[index_lower_bound.first] = key;
               values[index_lower_bound.first] = value;
            } else {
               // Not found so insert in the end
               keys[this->count] = key;
               values[this->count] = value;
            }
         } else {
            // I have to split!!!!
            throw std::runtime_error("Have to split in leaf node??");
         }

         ++this->count;
      }

      /// Erase a key.
      void erase(const KeyT& key) {
         auto [pos, found] = this->lower_bound(key);

         if (!found) {
            // Already erased
            return;
         }

         std::rotate(keys + pos, keys + (pos + 1), keys + this->count);
         std::rotate(values + pos, values + (pos + 1), values + this->count);

         --this->count;
      }

      /// Split the node.
      /// @param[in] buffer       The buffer for the new page.
      /// @return                 The separator key.
      KeyT split(std::byte* buffer) {
         auto mid = (this->count + 1) / 2;

         auto* next_node = new (buffer) LeafNode();

         auto moved_length = this->count - mid;

         // Move the data to the new leafnode
         std::move(
            keys + mid,
            keys + this->count,
            next_node->keys);
         std::move(
            values + mid,
            values + this->count,
            next_node->values);

         this->count -= moved_length;
         next_node->count += moved_length;

         next_node->next = this->next;
         this->next = next_node;

         return keys[this->count - 1];
      }

      /// Returns the keys.
      std::vector<KeyT> get_key_vector() {
         return std::vector<KeyT>(keys, keys + this->count);
      }

      /// Returns the values.
      std::vector<ValueT> get_value_vector() {
         return std::vector<ValueT>(values, values + this->count);
      }
   };

   /// The root.
   uint64_t root;

   // Here is the mutex
   std::shared_mutex mutex;

   bool isEmpty;

   uint64_t next_page_id;

   /// Constructor.
   BTree(uint16_t segment_id, BufferManager& buffer_manager)
      : Segment(segment_id, buffer_manager) {
      root = 0;
      next_page_id = 0;
      isEmpty = true;
   }

   /// Destructor.
   ~BTree() = default;

   /// Lookup an entry in the tree.
   /// @param[in] key      The key that should be searched.
   /// @return             Whether the key was in the tree.
   std::optional<ValueT> lookup(const KeyT& key) {
      std::optional<ValueT> out;

      std::shared_lock guard(mutex);

      uint64_t current_id = this->root;

      if (isEmpty) {
         return out;
      }

      // Start at the root (no parent yet)
      auto parent_id = this->root;
      BufferFrame* parent_frame = nullptr;
      BufferFrame* current_frame = nullptr;

      while (true) {
         // Fix the current page
         current_frame = &buffer_manager.fix_page(current_id, false);
         auto* node = reinterpret_cast<Node*>(current_frame->get_data());

         if (node->is_leaf()) {
            // Found the leaf
            break;
         }

         if (parent_frame) {
            buffer_manager.unfix_page(*parent_frame, false);
            if (parent_id == this->root) {
               guard.unlock();
            }
         }

         parent_id = current_id;
         parent_frame = current_frame;

         // Figure out which child to visit next
         auto* in = static_cast<InnerNode*>(node);
         auto [idx, exists] = in->lower_bound(key);
         current_id = in->children[idx];
      }

      auto* leaf = reinterpret_cast<LeafNode*>(current_frame->get_data());
      auto [pos, found] = leaf->lower_bound(key);
      if (found && leaf->keys[pos] == key) {
         out.emplace(leaf->values[pos]);
      }

      buffer_manager.unfix_page(*current_frame, false);
      if (parent_frame) {
         buffer_manager.unfix_page(*parent_frame, false);
      }
      return out;
   }

   /// Erase an entry in the tree.
   /// @param[in] key      The key that should be searched.
   void erase(const KeyT& key) {
      // Add mutex for the root
      std::unique_lock guard(mutex);

      if (isEmpty) {
         return;
      }

      auto current_page_id = this->root;

      BufferFrame* parent_frame = nullptr;

      auto parent_id = this->root;

      BufferFrame* current_frame = &buffer_manager.fix_page(current_page_id, false);
      Node* current_node = reinterpret_cast<Node*>(current_frame->get_data());

      while (!current_node->is_leaf()) {
         // Get it as an innernode
         auto* current_inner_node = reinterpret_cast<InnerNode*>(current_node);

         // Check if there is enough space for another key to be added
         auto [pos, exists] = current_inner_node->lower_bound(key);
         uint64_t next_child_page_id = current_inner_node->children[pos];

         // Release the parent frame first
         if (parent_frame) {
            buffer_manager.unfix_page(*parent_frame, true);
            if (parent_id == root) {
               guard.unlock();
            }
         }

         // Set the current to parent frame
         parent_id = current_page_id;
         parent_frame = current_frame;

         current_page_id = next_child_page_id;

         // Acquire the child
         current_frame = &buffer_manager.fix_page(current_page_id, false);
         current_node = reinterpret_cast<Node*>(current_frame->get_data());
      }

      // Current node is leaf
      auto* current_leaf_node = reinterpret_cast<LeafNode*>(current_node);

      // Erase the key in the leaf
      current_leaf_node->erase(key);

      // Release the parent frame
      if (parent_frame) {
         buffer_manager.unfix_page(*parent_frame, true);
      }

      buffer_manager.unfix_page(*current_frame, true);
   }

   /// Inserts a new entry into the tree.
   /// @param[in] key      The key that should be inserted.
   /// @param[in] value    The value that should be inserted.
   void insert(const KeyT& key, const ValueT& value) {
      // Empty tree insert new root with a page
      std::unique_lock guard(mutex);

      if (isEmpty) {
         uint64_t leaf_id = next_page_id++;
         root = leaf_id;
         auto& lf = buffer_manager.fix_page(leaf_id, /*exclusive=*/true);
         auto* leaf = reinterpret_cast<LeafNode*>(lf.get_data());
         leaf->insert(key, value);
         buffer_manager.unfix_page(lf, true);
         isEmpty = false;
         return;
      }

      auto current_page_id = this->root;
      uint64_t next_child_page_id;

      BufferFrame* parent_frame = nullptr;
      InnerNode* parent_node = nullptr;

      auto parent_id = this->root;

      BufferFrame* current_frame = &buffer_manager.fix_page(current_page_id, false);
      Node* current_node = reinterpret_cast<Node*>(current_frame->get_data());

      while (!current_node->is_leaf()) {
         // Get it as an innernode
         auto* current_inner_node = reinterpret_cast<InnerNode*>(current_node);

         // Check if there is enough space for another key to be added
         if (current_inner_node->count <= current_inner_node->kCapacity) {
            // Enogh space for another key
            auto [pos, exists] = current_inner_node->lower_bound(key);
            next_child_page_id = current_inner_node->children[pos];
         } else {
            // Not enough space have to split and propagate to parent
            auto new_inner_page_id = next_page_id++;
            auto* new_inner_page = &buffer_manager.fix_page(new_inner_page_id, false);

            auto up_key = current_inner_node->split(reinterpret_cast<std::byte*>(new_inner_page->get_data()));

            auto* new_inner_node = reinterpret_cast<InnerNode*>(new_inner_page->get_data());

            new_inner_node->level = current_inner_node->level;

            if (!parent_frame) {
               // Create a new innernode and use it as root
               auto new_root_page_id = next_page_id++;
               parent_frame = &buffer_manager.fix_page(new_root_page_id, true);

               parent_node = reinterpret_cast<InnerNode*>(parent_frame->get_data());
               parent_node->count += 1;
               parent_node->level = current_node->level + 1;
               parent_node->children[0] = root;

               root = new_root_page_id;
               parent_id = new_root_page_id;
            }

            parent_node->insert_split(up_key, new_inner_page_id);

            // Choose the correct id now to continue
            if (ComparatorT{}(up_key, key)) {
               // Is in the new node
               auto [pos, exists] = new_inner_node->lower_bound(key);
               next_child_page_id = new_inner_node->children[pos];
               buffer_manager.unfix_page(*current_frame, false);
               current_frame = new_inner_page;
               current_inner_node = new_inner_node;
            } else {
               auto [pos, exists] = current_inner_node->lower_bound(key);
               next_child_page_id = current_inner_node->children[pos];
            }
         }

         // Release the parent frame first
         if (parent_frame) {
            buffer_manager.unfix_page(*parent_frame, true);
            if (parent_id == root) {
               guard.unlock();
            }
         }

         // Set the current to parent frame
         parent_id = current_page_id;
         parent_frame = current_frame;
         parent_node = current_inner_node;

         current_page_id = next_child_page_id;

         // Acquire the child
         current_frame = &buffer_manager.fix_page(current_page_id, false);
         current_node = reinterpret_cast<Node*>(current_frame->get_data());
      }

      // Current node is leaf
      auto* current_leaf_node = reinterpret_cast<LeafNode*>(current_node);

      // Check if there is enough space on the leaf, if there is insert and release parent, itself
      if (current_leaf_node->count < current_leaf_node->kCapacity) {
         current_leaf_node->insert(key, value);

         if (parent_frame) {
            buffer_manager.unfix_page(*parent_frame, true);
            if (parent_id == root) {
               guard.unlock();
            }
         }

         buffer_manager.unfix_page(*current_frame, true);
         return;
      }

      // There isn't enough space
      // 1. Split the leaf create a new leaf node
      auto new_leaf_page_id = next_page_id++;
      auto* new_leaf_page = &buffer_manager.fix_page(new_leaf_page_id, true);
      auto* new_leaf_node = reinterpret_cast<LeafNode*>(new_leaf_page->get_data());

      auto up_key = current_leaf_node->split(reinterpret_cast<std::byte*>(new_leaf_node));

      // If there is a parent propogate up
      if (!parent_node) {
         // Create a new innernode and use it as root
         auto new_root_page_id = next_page_id++;
         parent_frame = &buffer_manager.fix_page(new_root_page_id, true);

         parent_node = reinterpret_cast<InnerNode*>(parent_frame->get_data());
         parent_node->count += 1;
         parent_node->level = current_node->level + 1;
         parent_node->children[0] = root;

         root = new_root_page_id;
      }

      parent_node->insert_split(up_key, new_leaf_page_id);

      // Insert the key, value into correct leaf node
      if (ComparatorT{}(up_key, key)) {
         new_leaf_node->insert(key, value);
      } else {
         current_leaf_node->insert(key, value);
      }

      // Release the parent frame
      if (parent_frame) {
         buffer_manager.unfix_page(*parent_frame, true);
      }

      buffer_manager.unfix_page(*current_frame, true);
      buffer_manager.unfix_page(*new_leaf_page, true);
   }
};

} // namespace moderndbs

#endif
