#pragma once

#include <vector>
#include <functional>
#include <utility>
#include <cstdint>
#include <memory>
#include <cassert>

#include <stdlib.h>
#include <iostream>
#include <iomanip>

// xxx: not sure which one I need
#include "emmintrin.h"
#include "immintrin.h"

// TODO:
// * add an extra byte hanging off the end of the metadata table that dictates "end of table"
//   so iterators don't have to hold so much crap. They should just be able to store pointer + offset,
//   maybe even just a pointer with a shift stuffed in the upper bits.
//
// * seed the 57 bits of the hash with aslr >> 12
//
// * max load factor == 7/8
//
// * robin hood hashing == bad bc too many instructions
//
// * use C++ allocators

template<typename T>
struct hash_set_mem
{
        size_t capacity_;

        // bit7 --> sentinal
        // if bit7 == 0:
        //     bits 6:0 dictate empty, erased, or end of table
        //     0x00 == empty/never occupied
        //     0x01 == erased/tombstoned
        //     0x7f == end of table (not used yet)
        // if bit7 == 1:
        //     bits 6:0 are low 7 bits of hash
        struct meta {
                uint8_t m_;

                meta(uint8_t m) : m_{m} {}

                bool is_sentinal() const
                {
                        return (m_ & 0x80) == 0x00;
                }

                bool is_occupied() const
                {
                        return (m_ & 0x80) == 0x80;
                }

                // is_insertable is logically equivalent to "is_never_occupied || is_tombstone",
                // but this spells what we actually want to ask, and it's a few less instructions
                // that the compiler might not have been able to find.
                bool is_insertable() const
                {
                        return (m_ & 0x82) == 0x00;
                }

                bool is_tombstone() const
                {
                        return m_ == 0x01;
                }

                bool is_never_occupied() const
                {
                        return m_ == 0x00;
                }

                bool is_end() const
                {
                        return m_ == 0x7f;
                }

                uint8_t get_hash() const
                {
                        assert(is_occupied());
                        return m_ & 0x7f;
                }

                void make_tombstoned()
                {
                        m_ = 0x01;
                }

                void make_occupied(uint8_t hash)
                {
                        assert((hash & 0x80) == 0);
                        m_ = 0x80 | hash;
                }
        };

        static_assert(sizeof(meta) == 1, "expected meta to be 1 byte");
        
        // lower 2 bits
        //  x0 --> unoccupied
        //  x1 --> occupied
        //  1x --> ever occupied
private:
        void * mem_;

        // XXX: these two functions depend on the child class only using capacities that are
        // divisible by the alignment of T
        ptrdiff_t data_offset() const
        {
                assert(capacity_ % alignof(T) == 0);

                std::ptrdiff_t off = capacity_ * sizeof(meta);

                // TODO: support weirdly alligned types? If alignof(T) > malloc alignment, we
                // don't work at all.
                assert((reinterpret_cast<std::ptrdiff_t>(mem_) + off) % alignof(T) == 0);

                return off;
        }

        size_t alloc_size() const
        {
                assert(capacity_ % alignof(T) == 0);

                return capacity_ * (sizeof(meta) + sizeof(T));
        }

public:
        meta * get_meta()
        {
                return static_cast<meta *>(mem_);
        }

        T * get_data()
        {
                return reinterpret_cast<T*>(static_cast<uint8_t *>(mem_)
                                            + data_offset());
        }

        // only use to use aslr
        const void * __get_mem() const
        {
                return mem_;
        }

        const meta * get_meta() const
        {
                return static_cast<const meta *>(mem_);
        }

        const T * get_data() const
        {
                return reinterpret_cast<const T*>(static_cast<const uint8_t *>(mem_)
                                                  + data_offset());
        }

        // XXX: don't malloc
        hash_set_mem(size_t cap)
                : capacity_{cap},
                  mem_{malloc(alloc_size())}
        {
                assert(mem_);
                memset(get_meta(), 0, capacity_);
        }

        ~hash_set_mem()
        {
                meta * mvec = get_meta();
                T * dvec = get_data();
                for (size_t i = 0; i < capacity_; ++i)
                        if (mvec[i].is_occupied())
                                dvec[i].~T();

                // XXX: exception safety if dtor throws
                free(mem_);
        }

        void swap(hash_set_mem & other)
        {
                std::swap(capacity_, other.capacity_);
                std::swap(mem_, other.mem_);
        }

        hash_set_mem(const hash_set_mem& ) = delete;
        hash_set_mem(hash_set_mem&&) = delete;
        hash_set_mem& operator=(const hash_set_mem&) = delete;
        hash_set_mem& operator=(hash_set_mem&&) = delete;
};

template <typename T>
void swap(hash_set_mem<T> & lhs, hash_set_mem<T> & rhs)
{
        lhs.swap(rhs);
}


template<typename T>
class hash_set : hash_set_mem<T>
{
private:
        using base_t = hash_set_mem<T>;

        using meta = typename base_t::meta;
        

        size_t size_;
        size_t tombstones_;

        size_t sanitize_capacity(size_t cap)
        {
                if (cap < 16) {
                        return 16;
                } if (__builtin_popcount(cap) == 1) {
                        return cap;
                } else {
                        constexpr int size_t_bits = sizeof(size_t) * 8;

                        // xxx: assumes size_t == unsigned long
                        return static_cast<size_t>(1)
                                << (size_t_bits - __builtin_clzl(cap));
                }
        }

public:

        hash_set(size_t capacity)
                : base_t(sanitize_capacity(capacity)), size_(0), tombstones_(0)
        {}
        
        hash_set() : hash_set(16)
        {}

        template <bool is_const>
        class iterator_impl;

        using iterator = iterator_impl<false>;
        using const_iterator = iterator_impl<true>;
        
        // xxx: make this iterator smaller? currently 32 bytes...
        template <bool is_const> 
        class iterator_impl {
                using meta_ptr_t = typename std::conditional<is_const, const meta *, meta *>::type;
        public:
                using iterator_category = std::bidirectional_iterator_tag;
                using value_type = typename std::conditional<is_const, const T, T>::type;
                using difference_type = std::ptrdiff_t;
                using reference = typename std::conditional<is_const, const T&, T&>::type;
                using pointer = value_type *;
                
                // allow construction from non-const to const
                iterator_impl(const iterator& rhs)
                        : capacity(rhs.capacity),
                          meta_start(rhs.meta_start),
                          data_start(rhs.data_start),
                          offset(rhs.offset)
                {}
                
                bool operator==(const iterator_impl& rhs)
                {
                        // this is an extra comparison, but avoids undefined behavior from
                        // comparing across containers 
                        return meta_start == rhs.meta_start
                                && offset == rhs.offset;
                }

                bool operator!=(const iterator_impl& rhs)
                {
                        return !(*this == rhs);
                }

                iterator_impl& operator++()
                {
                        assert(offset != capacity);
                        
                        for (;;) {
                                ++offset;

                                if (offset == capacity || meta_start[offset].is_occupied()) {
                                        break;
                                }
                        }
                        return *this;
                }

                iterator_impl& operator--()
                {
                        assert(offset != 0);

                        do {
                                --offset;
                                if (meta_start[offset].is_occupied()) {
                                        return *this;
                                }
                        } while (offset != 0);

                        return *this;
                }
                
                reference operator*()
                {
                        return data_start[offset];
                }

        private:
                friend class hash_set;
                
                // these 3 fields should all be const, but operator= may need to change them if
                // we assign from one container's iterator to another...
                size_t capacity = 0;
                meta_ptr_t meta_start = nullptr;
                pointer data_start = nullptr;
                
                size_t offset = 0;

                iterator_impl(size_t cap, meta_ptr_t meta, pointer data, size_t off)
                        : capacity{cap}, meta_start{meta}, data_start{data}, offset{off}
                {}
        };

private:
        size_t __find_first_occupied() const
        {
                assert(size_ > 0);

                const meta * mvec = this->get_meta();
                __m128i mask = _mm_set1_epi8(0x80);
                for (size_t i = 0; i < this->capacity(); i += 16) {
                        const __m128i * mem = reinterpret_cast<const __m128i *>(mvec + i);

                        __m128i group = _mm_load_si128(mem);
                        __m128i masked = _mm_and_si128(group, mask);
                        
                        int bitmap = _mm_movemask_epi8(_mm_cmpeq_epi8(mask, masked));
                        if (bitmap != 0) {
                                return i + (__builtin_ffs(bitmap) - 1);
                        }
                }

                // we walked the whole damn table, but found nothing, even though size_ != 0. Bad.
                assert(!"corrupted table");
        }

        iterator iterator_at(size_t i)
        {
                return iterator{this->capacity_, this->get_meta(), this->get_data(), i};
        }

        const_iterator iterator_at(size_t i) const
        {
                return const_iterator{this->capacity_, this->get_meta(), this->get_data(), i};
        }

public:
        iterator end()
        {
                return iterator_at(this->capacity_);
        }
        
        iterator begin()
        {
                if (size() == 0) {
                        return end();
                }

                return iterator_at(__find_first_occupied());
        }

        const_iterator end() const
        {
                return iterator_at(this->capacity_);
        }

        // xxx: can we avoid this duplication ? 
        const_iterator begin() const
        {
                if (size_ == 0) {
                        return end();
                }
                
                return iterator_at(__find_first_occupied());
        }
        
        size_t __find(const T& val, bool & found) const
        {
                const size_t hash = do_hash(val);
                // need 16 byte allignment for _mm_load_si128
                const size_t start = (index_portion(hash) % this->capacity_) & ~size_t{0xf};
                size_t i = start;
                const meta * mvec = this->get_meta();

                found = false;

                do {
                        const __m128i * mem = reinterpret_cast<const __m128i *>(mvec + i);
                        const __m128i group = _mm_load_si128(mem);
                        const __m128i search = _mm_set1_epi8(0x80 | meta_portion(hash));

                        int bitmap = _mm_movemask_epi8(_mm_cmpeq_epi8(search, group));

                        while (bitmap != 0) {
                                int bit = __builtin_ffs(bitmap) - 1;
                                size_t idx = i + bit;
                                const T & v= this->get_data()[idx];
                                if (val == v) {
                                        found = true;
                                        return idx;
                                }
                                bitmap ^= (1 << bit); // builtin for this?
                        }

                        // if anything in this group was ever zero, we can stop
                        bitmap = _mm_movemask_epi8(_mm_cmpeq_epi8(_mm_set1_epi8(0x00), group));
                        if (bitmap) {
                                return 0;
                        }

                        i = (i + 16) % this->capacity_;

                } while (i != start); // stop when we've looped around the whole table. should be
                                      // rare

                assert(!"corrupted table"); // load factor prevents us from ever looping all the
                                            // way around
        }
        
        iterator find(const T& val)
        {
                bool found;
                size_t idx = __find(val, found);

                return found ? iterator_at(idx) : end();
        }

        const_iterator find(const T& val) const
        {
                return find(val); // XXX: const violation, right?
        }

        void erase(const T& val)
        {
                bool found;
                size_t idx = __find(val, found);
                
                if (found) {
                        assert(size_ > 0);
                        
                        this->get_meta()[idx].make_tombstoned();
                        this->get_data()[idx].~T();
                        --size_;

                        // don't shrink because we don't want to invalidate iterators. gross.
                }
        }

private:
        // insert for T& and T&&. 
        template <typename U>
        std::pair<iterator,bool> __insert(U&& val)
        {
                // xxx: this find basically does all the scanning of the insert below,
                // we could avoid that with a good helper...
                iterator where = find(val);
                if (where != end()) {
                        return std::make_pair(where, false);
                }

                if (load() > 0.7) {
                        // xxx: revisit these constants. 
                        hash_set bigger{__size_load() > 0.4 ? this->capacity_ * 2 : this->capacity_};

                        meta * mvec = this->get_meta();
                        T * dvec = this->get_data();
                        for (iterator i = begin(); i != end(); ++i) {
                                bigger.insert(std::move(dvec[i.offset]));
                                mvec[i.offset].make_tombstoned();
                        }

                        swap(bigger);
                }

                const size_t hash = do_hash(val);
                // need 16 byte allignment for _mm_load_si128
                const size_t start = (index_portion(hash) % this->capacity_) & ~size_t{0xf};

                size_t i = start;
                meta * mvec = this->get_meta();
                
                do {
                        const __m128i * mem = reinterpret_cast<const __m128i *>(mvec + i);
                        const __m128i group = _mm_load_si128(mem);
                        const __m128i masked = _mm_and_si128(group, _mm_set1_epi8(0x80));

                        int bitmap = _mm_movemask_epi8(_mm_cmpeq_epi8(masked, _mm_set1_epi8(0x00)));
                        if (bitmap != 0) {
                                int bit = __builtin_ffs(bitmap) - 1;
                                size_t idx = i + bit;

                                meta m{static_cast<uint8_t>(_mm_extract_epi8(group, bit))};

                                // this slot has never been tombstoned before, so we got a new ts
                                // once we eventaully erase things. We morbidly consider live values
                                // as tombstones so that the tombstones_ count basically counts all
                                // slots that an insert might have to consider, which is what we
                                // want to know in load factor
                                if (m.is_never_occupied())
                                        ++tombstones_;

                                mvec[idx].make_occupied(meta_portion(hash));
                                new (this->get_data() + idx) T{std::forward<U>(val)};
                                ++size_;
                                return std::make_pair(iterator_at(idx), true);
                        }

                        i = (i + 16) % this->capacity_;
                } while (i != start);

                // we never get here, we always find a slot
                assert(!"corrupted table");
        }

public:
        std::pair<iterator,bool> insert(const T& val)
        {
                return __insert(val);
        }
        
        std::pair<iterator,bool> insert(T&& val)
        {
                return __insert(std::move(val));
        }

        template <typename... Ts>
        std::pair<iterator,bool> emplace(Ts&& ... args)
        {
                T val{std::forward<Ts>(args)...};
                return __insert(std::move(val));
        }
        
        size_t size() const
        {
                return size_;
        }

        size_t capacity() const
        {
                return this->capacity_;
        }

private:
        size_t index_portion(size_t hash) const
        {
                return hash >> 7;
        }

        uint8_t meta_portion(size_t hash) const
        {
                return hash & 0x7f;
        }
        
        // XXX: do better
        size_t do_hash(const T& val) const
        {
                // seed the hash with ASLR and some random bits
                return std::hash<T>{}(val) ^ (reinterpret_cast<size_t>(this->__get_mem()) >> 12)
                                             ^ 0xf58e33ad9e13e5c1;
                // last part from https://www.random.org/cgi-bin/randbyte?nbytes=8&format=h
                // (you won't get the same result, read the url...)
        }
        
        double __size_load() const
        {
                return size_/double(this->capacity_);
        }

public:
        double load() const
        {
                return tombstones_/double(this->capacity_);
        }

        void swap(hash_set & rhs)
        {
                base_t::swap(rhs);
                std::swap(size_, rhs.size_);
                std::swap(tombstones_, rhs.tombstones_);
        }

        friend std::ostream& operator<<(std::ostream& os, const hash_set& set)
        {
                meta * mvec = set.get_meta();
                for (size_t i = 0; i < set.capacity(); ++i) {
                        os << std::hex << std::setfill('0') << std::setw(2)
                           << int(mvec[i].m_);

                        if (i != set.capacity() - 1) {
                                os << " ";
                        }
                }
                return os;
        }
};

template <typename T>
void swap(hash_set<T> & lhs, hash_set<T> & rhs)
{
        lhs.swap(rhs);
}
