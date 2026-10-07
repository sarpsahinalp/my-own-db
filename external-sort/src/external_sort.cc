#include "moderndbs/external_sort.h"
#include "cstring"
#include "moderndbs/file.h"
#include "vector"
#include <algorithm>
#include <span>
#include <assert.h>

namespace moderndbs {

void mergeRuns(
   File* input,
   File* temp_file,
   size_t total_runs,
   size_t mem_size,
   File& output);

void external_sort(File& input, size_t num_values, File& output, size_t mem_size) {
   size_t total_size = num_values * sizeof(uint64_t);
   size_t total_runs = total_size / mem_size + ((total_size % mem_size) ? 1 : 0);
   output.resize(total_size);

   // If empty return
   if (total_size == 0) {
      return;
   }

   // If everything fits in memory just sort and return
   if (total_runs == 1) {
      size_t remaining = std::min(mem_size, total_size);

      size_t numberCount = remaining / sizeof(uint64_t);
      // Read the numbers and sort them
      std::unique_ptr<char[]> block = input.read_block(0, remaining);
      auto values = reinterpret_cast<uint64_t*>(block.get());
      std::sort(values, values + numberCount);

      output.write_block(reinterpret_cast<const char*>(values), 0, remaining);
      return;
   }

   auto temp_file = File::make_temporary_file();
   temp_file->resize(output.size());
   auto temp_file_temp = File::make_temporary_file();
   temp_file_temp->resize(output.size());

   for (size_t run_offset = 0; run_offset < total_size; run_offset += std::min(mem_size, total_size - run_offset)) {
      size_t remaining = std::min(mem_size, total_size - run_offset);

      size_t numberCount = remaining / sizeof(uint64_t);
      // Read the numbers and sort them
      std::unique_ptr<char[]> block = input.read_block(run_offset, remaining);
      auto values = reinterpret_cast<uint64_t*>(block.get());
      std::sort(values, values + numberCount);

      // Write them to output
      temp_file->write_block(reinterpret_cast<const char*>(values), run_offset, remaining);
      temp_file_temp->write_block(reinterpret_cast<const char*>(values), run_offset, remaining);
   }

   mergeRuns(temp_file.get(), temp_file_temp.get(), total_runs, mem_size, output);
}

void refillBuffer(size_t bufferSize, size_t& run_size, size_t& run_offset, std::vector<uint64_t>& run_values, File& input) {
   // Load into buffer size amount of data from the first run
   size_t run_read_size = std::min(bufferSize, run_size);
   auto block = input.read_block(run_offset, run_read_size);

   // Update offset and size
   run_offset += run_read_size;
   run_size -= run_read_size;

   run_values.resize(run_read_size / sizeof(uint64_t));

   // Turn to numbers
   std::memcpy(
      run_values.data(),
      block.get(),
      run_read_size);
}

void emptyVector(size_t index, std::vector<uint64_t>& vec) {
   if (index + 1 == vec.size()) {
      vec.clear();
   }
}

void flushBuffer(std::vector<uint64_t>& buffer, File* output, size_t& input_offset, size_t start_offset) {
   output->write_block(reinterpret_cast<const char*>(&buffer[start_offset]), input_offset, (buffer.size() - start_offset) * sizeof(uint64_t));
   input_offset += (buffer.size() - start_offset) * sizeof(uint64_t);
   buffer.clear();
}

void insertElemToBuffer(size_t& index, std::vector<uint64_t>& run_values, std::vector<uint64_t>& buffer, uint64_t left_val, size_t capacity) {
   emptyVector(index, run_values);
   index = (index + 1) % capacity;
   buffer.push_back(left_val);
}

void insertToBufferCompare(uint64_t left_val, uint64_t right_val, std::vector<uint64_t>& buffer, size_t& first_index, size_t& second_index, std::vector<uint64_t>& first_run_values, std::vector<uint64_t>& second_run_values, size_t capacity) {
   if (left_val < right_val) {
      insertElemToBuffer(first_index, first_run_values, buffer, left_val, capacity);
   } else {
      insertElemToBuffer(second_index, second_run_values, buffer, right_val, capacity);
   }
}

File* mergeTwoWay(File* input, File* output, size_t run_size, size_t run_index, size_t bufferSize, size_t capacity, std::vector<uint64_t>& first_run_values, std::vector<uint64_t>& second_run_values, std::vector<uint64_t>& buffer) {
   size_t second_run_offset = (run_index + 1) * run_size;
   size_t first_run_offset = second_run_offset - run_size;
   size_t first_run_size = second_run_offset - first_run_offset;
   size_t second_run_size = std::min((output->size() - second_run_offset), run_size);

   size_t input_offset = first_run_offset;
   size_t first_index = 0;
   size_t second_index = 0;

   while (first_run_size >= 8 &&
          second_run_size >= 8) {
      if (first_run_size >= 8 && first_run_values.empty()) {
         refillBuffer(bufferSize, first_run_size, first_run_offset, first_run_values, *input);
      }

      if (second_run_size >= 8 && second_run_values.empty()) {
         refillBuffer(bufferSize, second_run_size, second_run_offset, second_run_values, *input);
      }

      while (!first_run_values.empty() && !second_run_values.empty()) {
         uint64_t left_val = first_run_values[first_index];
         uint64_t right_val = second_run_values[second_index];

         // Insert the smaller value into the buffer
         insertToBufferCompare(left_val, right_val, buffer, first_index, second_index, first_run_values, second_run_values, capacity);

         // Check if buffer is full
         if (buffer.size() == buffer.capacity()) {
            // Write the buffer to output when full
            flushBuffer(buffer, output, input_offset, 0);
         }
      }
   }

   while (first_run_size >= 8) {
      if (first_run_size >= 8 && first_run_values.empty()) {
         refillBuffer(bufferSize, first_run_size, first_run_offset, first_run_values, *input);
      }

      while (!first_run_values.empty()) {
         uint64_t left_val = first_run_values[first_index];

         if (!second_run_values.empty()) {
            uint64_t right_val = second_run_values[second_index];

            insertToBufferCompare(left_val, right_val, buffer, first_index, second_index, first_run_values, second_run_values, capacity);
         } else {
            // Flush the elements inside buffer if there is any
            if (!buffer.empty()) {
               flushBuffer(buffer, output, input_offset, 0);
            }
            flushBuffer(first_run_values, output, input_offset, first_index);
            first_index = 0;
         }

         if (buffer.size() == buffer.capacity()) {
            // Write the buffer to output when full
            flushBuffer(buffer, output, input_offset, 0);
         }
      }
   }

   while (second_run_size >= 8) {
      if (second_run_size >= 8 && second_run_values.empty()) {
         refillBuffer(bufferSize, second_run_size, second_run_offset, second_run_values, *input);
      }

      while (!second_run_values.empty()) {
         uint64_t right_val = second_run_values[second_index];

         if (!first_run_values.empty()) {
            uint64_t left_val = first_run_values[first_index];

            insertToBufferCompare(left_val, right_val, buffer, first_index, second_index, first_run_values, second_run_values, capacity);
         } else {
            // Flush the elements inside buffer if there is any
            if (!buffer.empty()) {
               flushBuffer(buffer, output, input_offset, 0);
            }

            flushBuffer(second_run_values, output, input_offset, second_index);
            second_index = 0;
         }

         if (buffer.size() == buffer.capacity()) {
            flushBuffer(buffer, output, input_offset, 0);
         }
      }
   }

   if (!buffer.empty()) {
      flushBuffer(buffer, output, input_offset, 0);
   }

   // Discard any value in the last memory should be from only one
   while (!first_run_values.empty()) {
      flushBuffer(first_run_values, output, input_offset, first_index);
   }

   while (!second_run_values.empty()) {
      flushBuffer(second_run_values, output, input_offset, second_index);
   }

   return output;
}

void mergeRuns(File* input,
               File* temp_file,
               size_t total_runs,
               size_t mem_size,
               File& output) {
   // Two input buffers one output buffer
   size_t bufferSize = (mem_size) / 3 & ~(size_t) 7;
   size_t capacity = bufferSize / sizeof(uint64_t);
   size_t output_capacity = (mem_size - 2 * bufferSize) / sizeof(uint64_t);

   // Check if the buffer sizes are allocated to fit into the memory
   assert(2 * capacity + output_capacity <= mem_size / sizeof(uint64_t));

   // Input buffers:
   std::vector<uint64_t> first_run_values;
   first_run_values.reserve(capacity);
   std::vector<uint64_t> second_run_values;
   second_run_values.reserve(capacity);

   // Allocate memory for the buffer before hand to avoid resize
   std::vector<uint64_t> buffer;
   buffer.reserve(output_capacity);
   size_t run_size = mem_size;

   // 2 run: 0, 1
   while (total_runs > 1) {
      if (total_runs == 2) {
         mergeTwoWay(input, &output, run_size, 0, bufferSize, capacity, first_run_values, second_run_values, buffer);
         return;
      }
      for (size_t i = 0; i < total_runs; i += 2) {
         if (i == total_runs - 1) {
            break;
         }
         auto toSwitch = mergeTwoWay(input, temp_file, run_size, i, bufferSize, capacity, first_run_values, second_run_values, buffer);

         if (i + 1 >= total_runs - 2) {
            temp_file = input;
            input = toSwitch;
         }
      }

      run_size *= 2;
      if (total_runs % 2 == 0) {
         total_runs >>= 1;
      } else {
         total_runs >>= 1;
         total_runs++;
      }
   }
}

} // namespace moderndbs
