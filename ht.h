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
// * use 1 bit for sentinal|value, use 7 bits for hash
//
// * add an extra byte hanging off the end of the metadata table that dictates "end of table"
//   so iterators don't have to hold so much crap. They should just be able to store pointer + offset,
//   maybe even just a pointer with a shift stuffed in the upper bits.
//
// * use top bits of hash as index into table, use bottom 7 bits to compare with metadata
//   via functions H1 and H2
//
// * implement lookup with _mm_movemask_epi8(_mm_cmpeq_epi8(_mm_set1_epi8(hash_byte), meta_bytes))
//   that give you back a 16-bit bitmap which you can then scan with __builtin_ffs or whatever
//
// * don't store the upper bits of the hash
//
// * seed the 57 bits of the hash with aslr >> 12
//
// * max load factor == 7/8
//
// * robin hood hashing == bad bc too many instructions
//
// * use only a single memory allocation
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
        meta * meta_vec_;
        T * data_vec_;

        // XXX: don't malloc
        hash_set_mem(size_t cap)
                : capacity_{cap},
                  meta_vec_{(meta *)malloc(capacity_)},
                  data_vec_{(T *)malloc(capacity_ * sizeof(T))}
        {
                memset(meta_vec_, 0, capacity_);
                assert(meta_vec_);
                assert(data_vec_);
        }

        ~hash_set_mem()
        {
                for (size_t i = 0; i < capacity_; ++i)
                        if (meta_vec_[i].is_occupied())
                                data_vec_[i].~T();
                free(meta_vec_);
                free(data_vec_);
        }

        void swap(hash_set_mem & other)
        {
                std::swap(capacity_, other.capacity_);
                std::swap(meta_vec_, other.meta_vec_);
                std::swap(data_vec_, other.data_vec_);
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

                for (size_t i = 0; i < this->capacity_; ++i) {
                        if (this->meta_vec_[i].is_occupied()) {
                                return i;
                        }
                }

                // we walked the whole damn table, but found nothing, even though size_ != 0. Bad.
                assert(!"corrupted table");
        }

        iterator iterator_at(size_t i)
        {
                return iterator{this->capacity_, this->meta_vec_, this->data_vec_, i};
        }

        const_iterator iterator_at(size_t i) const
        {
                return const_iterator{this->capacity_, this->meta_vec_, this->data_vec_, i};
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
                const size_t start = (hash >> 7) % this->capacity_;
                size_t i = start;

                found = false;

                do {
                        meta m = this->meta_vec_[i];
                        
                        // linear probing: we found a never-occupied, so we done
                        if (m.is_never_occupied()) {
                                break;
                        }

                        if (m.is_occupied() && m.get_hash() == (hash & 0x7f)) {
                                T & v= this->data_vec_[i];
                                if (val == v) {
                                        found = true;
                                        break;
                                }
                        }

                        i = (i + 1) % this->capacity_;

                } while (i != start); // stop when we've looped around the whole table. should be
                                      // rare

                return i;
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
                        
                        this->meta_vec_[idx].make_tombstoned();
                        this->data_vec_[idx].~T();
                        --size_;

                        // don't shrink because we don't want to invalidate iterators. gross.
                }
        }

private:
        // insert for T& and T&&. 
        template <typename U>
        std::pair<iterator,bool> __insert(U&& val)
        {
                iterator where = find(val);
                if (where != end()) {
                        return std::make_pair(where, false);
                }

                if (load() > 0.7) {
                        // xxx: revisit these constants. 
                        hash_set bigger{__size_load() > 0.4 ? this->capacity_ * 2 : this->capacity_};

                        for (iterator i = begin(); i != end(); ++i) {
                                bigger.insert(std::move(this->data_vec_[i.offset]));
                                this->meta_vec_[i.offset].make_tombstoned();
                        }

                        swap(bigger);
                }

                const size_t hash = do_hash(val);
                const size_t start = (hash >> 7) % this->capacity_;

                size_t i = start;
                
                do {
                        meta & m = this->meta_vec_[i];

                        if (m.is_insertable()) {

                                // this slot has never been tombstoned before, so we got a new ts
                                // once we eventaully erase things. We morbidly consider live values
                                // as tombstones so that the tombstones_ count basically counts all
                                // slots that an insert might have to consider, which is what we want
                                // to know in load factor
                                if (m.is_never_occupied())
                                        ++tombstones_;

                                this->meta_vec_[i].make_occupied(hash & 0x7f);
                                new (&this->data_vec_[i]) T{std::forward<U>(val)};
                                ++size_;
                                return std::make_pair(iterator_at(i), true);
                        }

                        i = (i + 1) % this->capacity_;
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
        // XXX: do better
        size_t do_hash(const T& val) const
        {
                return std::hash<T>{}(val);
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
                for (size_t i = 0; i < set.capacity(); ++i) {
                        os << std::hex << std::setfill('0') << std::setw(2)
                           << int(set.meta_vec_[i].m_);

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
