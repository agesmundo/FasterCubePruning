#include "lm/trie.hh"

#include "lm/quantize.hh"
#include "util/bit_packing.hh"
#include "util/exception.hh"
#include "util/sorted_uniform.hh"

#include <assert.h>

namespace lm {
namespace ngram {
namespace trie {
namespace {

class KeyAccessor {
  public:
    KeyAccessor(const void *base, uint64_t key_mask, uint8_t key_bits, uint8_t total_bits) 
      : base_(reinterpret_cast<const uint8_t*>(base)), key_mask_(key_mask), key_bits_(key_bits), total_bits_(total_bits) {}

    typedef uint64_t Key;

    Key operator()(uint64_t index) const {
      return util::ReadInt57(base_, index * static_cast<uint64_t>(total_bits_), key_bits_, key_mask_);
    }

  private:
    const uint8_t *const base_;
    const WordIndex key_mask_;
    const uint8_t key_bits_, total_bits_;
};

bool FindBitPacked(const void *base, uint64_t key_mask, uint8_t key_bits, uint8_t total_bits, uint64_t begin_index, uint64_t end_index, const uint64_t max_vocab, const uint64_t key, uint64_t &at_index) {
  KeyAccessor accessor(base, key_mask, key_bits, total_bits);
  if (!util::BoundedSortedUniformFind<uint64_t, KeyAccessor, util::PivotSelect<sizeof(WordIndex)>::T>(accessor, begin_index - 1, (uint64_t)0, end_index, max_vocab, key, at_index)) return false;
  return true;
}
} // namespace

std::size_t BitPacked::BaseSize(uint64_t entries, uint64_t max_vocab, uint8_t remaining_bits) {
  uint8_t total_bits = util::RequiredBits(max_vocab) + remaining_bits;
  // Extra entry for next pointer at the end.  
  // +7 then / 8 to round up bits and convert to bytes
  // +sizeof(uint64_t) so that ReadInt57 etc don't go segfault.  
  // Note that this waste is O(order), not O(number of ngrams).
  return ((1 + entries) * total_bits + 7) / 8 + sizeof(uint64_t);
}

void BitPacked::BaseInit(void *base, uint64_t max_vocab, uint8_t remaining_bits) {
  util::BitPackingSanity();
  word_bits_ = util::RequiredBits(max_vocab);
  word_mask_ = (1ULL << word_bits_) - 1ULL;
  if (word_bits_ > 57) UTIL_THROW(util::Exception, "Sorry, word indices more than " << (1ULL << 57) << " are not implemented.  Edit util/bit_packing.hh and fix the bit packing functions.");
  total_bits_ = word_bits_ + remaining_bits;

  base_ = static_cast<uint8_t*>(base);
  insert_index_ = 0;
  max_vocab_ = max_vocab;
}

template <class Quant> std::size_t BitPackedMiddle<Quant>::Size(uint8_t quant_bits, uint64_t entries, uint64_t max_vocab, uint64_t max_ptr) {
  return BaseSize(entries, max_vocab, quant_bits + util::RequiredBits(max_ptr));
}

template <class Quant> BitPackedMiddle<Quant>::BitPackedMiddle(void *base, const Quant &quant, uint64_t max_vocab, uint64_t max_next, const BitPacked &next_source) : BitPacked(), quant_(quant), next_bits_(util::RequiredBits(max_next)), next_mask_((1ULL << next_bits_) - 1), next_source_(&next_source) {
  if (next_bits_ > 57) UTIL_THROW(util::Exception, "Sorry, this does not support more than " << (1ULL << 57) << " n-grams of a particular order.  Edit util/bit_packing.hh and fix the bit packing functions.");
  BaseInit(base, max_vocab, quant.TotalBits() + next_bits_);
}

template <class Quant> void BitPackedMiddle<Quant>::Insert(WordIndex word, float prob, float backoff) {
  assert(word <= word_mask_);
  uint64_t at_pointer = insert_index_ * total_bits_;

  util::WriteInt57(base_, at_pointer, word_bits_, word);
  at_pointer += word_bits_;
  quant_.Write(base_, at_pointer, prob, backoff);
  at_pointer += quant_.TotalBits();
  uint64_t next = next_source_->InsertIndex();
  assert(next <= next_mask_);
  util::WriteInt57(base_, at_pointer, next_bits_, next);

  ++insert_index_;
}

template <class Quant> bool BitPackedMiddle<Quant>::Find(WordIndex word, float &prob, float &backoff, NodeRange &range) const {
  uint64_t at_pointer;
  if (!FindBitPacked(base_, word_mask_, word_bits_, total_bits_, range.begin, range.end, max_vocab_, word, at_pointer)) {
    return false;
  }
  at_pointer *= total_bits_;
  at_pointer += word_bits_;
  quant_.Read(base_, at_pointer, prob, backoff);
  at_pointer += quant_.TotalBits();

  range.begin = util::ReadInt57(base_, at_pointer, next_bits_, next_mask_);
  // Read the next entry's pointer.  
  at_pointer += total_bits_;
  range.end = util::ReadInt57(base_, at_pointer, next_bits_, next_mask_);
  return true;
}

template <class Quant> bool BitPackedMiddle<Quant>::FindNoProb(WordIndex word, float &backoff, NodeRange &range) const {
  uint64_t at_pointer;
  if (!FindBitPacked(base_, word_mask_, word_bits_, total_bits_, range.begin, range.end, max_vocab_, word, at_pointer)) return false;
  at_pointer *= total_bits_;
  at_pointer += word_bits_;
  quant_.ReadBackoff(base_, at_pointer, backoff);
  at_pointer += quant_.TotalBits();
  range.begin = util::ReadInt57(base_, at_pointer, next_bits_, next_mask_);
  // Read the next entry's pointer.  
  at_pointer += total_bits_;
  range.end = util::ReadInt57(base_, at_pointer, next_bits_, next_mask_);
  return true;
}

template <class Quant> void BitPackedMiddle<Quant>::FinishedLoading(uint64_t next_end) {
  assert(next_end <= next_mask_);
  uint64_t last_next_write = (insert_index_ + 1) * total_bits_ - next_bits_;
  util::WriteInt57(base_, last_next_write, next_bits_, next_end);
}

template <class Quant> void BitPackedLongest<Quant>::Insert(WordIndex index, float prob) {
  assert(index <= word_mask_);
  uint64_t at_pointer = insert_index_ * total_bits_;
  util::WriteInt57(base_, at_pointer, word_bits_, index);
  at_pointer += word_bits_;
  quant_.Write(base_, at_pointer, prob);
  ++insert_index_;
}

template <class Quant> bool BitPackedLongest<Quant>::Find(WordIndex word, float &prob, const NodeRange &range) const {
  uint64_t at_pointer;
  if (!FindBitPacked(base_, word_mask_, word_bits_, total_bits_, range.begin, range.end, max_vocab_, word, at_pointer)) return false;
  at_pointer = at_pointer * total_bits_ + word_bits_;
  quant_.Read(base_, at_pointer, prob);
  return true;
}

template class BitPackedMiddle<DontQuantize::Middle>;
template class BitPackedMiddle<SeparatelyQuantize::Middle>;
template class BitPackedLongest<DontQuantize::Longest>;
template class BitPackedLongest<SeparatelyQuantize::Longest>;

} // namespace trie
} // namespace ngram
} // namespace lm
