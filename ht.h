#pragma once

#include <vector>
#include <functional>
#include <utility>
#include <cstdint>
#include <memory>
#include <cassert>

#include <stdlib.h>
#include <iostream>

using namespace std;

template<typename T>
struct hash_set_mem
{
        struct value {
                size_t hash;
                T val;
        };

        size_t capacity_;
        
        // lower 2 bits
        //  x0 --> unoccupied
        //  x1 --> occupied
        //  1x --> ever occupied
        uint8_t * meta_vec_;
        value * data_vec_;

        // XXX: don't malloc
        hash_set_mem(size_t cap)
                : capacity_{cap},
                  meta_vec_{(uint8_t *)malloc(capacity_)},
                  data_vec_{(value *)malloc(capacity_ * sizeof(value))}
        {
                memset(meta_vec_, 0, capacity_);
                assert(meta_vec_);
                assert(data_vec_);
        }

        ~hash_set_mem()
        {
                for (size_t i = 0; i < capacity_; ++i)
                        if (meta_vec_[i] & 0x1)
                                data_vec_[i].val.~T();
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
public:
        using base_t = hash_set_mem<T>;
        using value = typename base_t::value;

        size_t size_;
        size_t tombstones_;

private:
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

        public:
                using meta_ptr_t = typename std::conditional<is_const, const uint8_t *, uint8_t *>::type;
                using data_ptr_t = typename std::conditional<is_const, const value *, value *>::type;

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

                                if (offset == capacity || meta_start[offset] & 0x1) {
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
                                if (meta_start[offset] & 0x1) {
                                        return *this;
                                }
                        } while (offset != 0);

                        return *this;
                }
                
                reference operator*()
                {
                        return data_start[offset].val;
                }

        private:
                friend class hash_set;
                
                // these 3 fields should all be const, but operator= may need to change them if
                // we assign from one container's iterator to another...
                size_t capacity = 0;
                meta_ptr_t meta_start = nullptr;
                data_ptr_t data_start = nullptr;
                
                size_t offset = 0;

                iterator_impl(size_t cap, meta_ptr_t meta, data_ptr_t data, size_t off)
                        : capacity{cap}, meta_start{meta}, data_start{data}, offset{off}
                {}
        };

        iterator end()
        {
                return iterator{this->capacity_, this->meta_vec_, this->data_vec_, this->capacity_};
        }

private:
        size_t __find_first_occupied() const
        {
                assert(size_ > 0);

                for (size_t i = 0; i < this->capacity_; ++i) {
                        if (this->meta_vec_[i] & 0x1) {
                                return i;
                        }
                }

                // we walked the whole damn table, but found nothing, even though size_ != 0. Bad.
                assert(!"corrupted table");
        }

public: 
        iterator begin()
        {
                if (size_ == 0) {
                        return end();
                }

                return iterator{this->capacity_, this->meta_vec_, this->data_vec_,
                                __find_first_occupied()};
        }

        const_iterator end() const
        {
                return const_iterator{this->capacity_, this->meta_vec_, this->data_vec_,
                                this->capacity_};
        }

        // xxx: can we avoid this duplication ? 
        const_iterator begin() const
        {
                if (size_ == 0) {
                        return end();
                }
                
                return const_iterator{this->capacity_, this->meta_vec_, this->data_vec_,
                                __find_first_occupied()};
        }
        
        size_t __find(const T& val, bool & found) const
        {
                const size_t hash = do_hash(val);
                const size_t start = hash % this->capacity_;
                size_t i = start;

                found = false;

                do {
                        uint8_t meta = this->meta_vec_[i];
                        
                        // linear probing: we found a never-occupied, so we done
                        if (!(meta & 0x2)) {
                                break;
                        }

                        // is this slot currently occupied and do the lower bits of the hash and
                        // tht occupied bits. 0xfd == b11111101
                        if ((meta & 0xfd) == (((hash & 0x3f) << 2) | 1)) {
                                value & v= this->data_vec_[i];
                                if (v.hash == hash && val == v.val) {
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

                return found ? iterator{this->capacity_, this->meta_vec_, this->data_vec_, idx}
                             : end();
        }

        const_iterator find(const T& val) const
        {
                return find(val);
        }

        void erase(const T& val)
        {
                bool found;
                size_t idx = __find(val, found);
                
                if (found) {
                        assert(size_ > 0);
                        
                        this->meta_vec_[idx] = 0x2;
                        this->data_vec_[idx].val.~T();
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
                        return make_pair(where, false);
                }

                if (load() > 0.7) {
                        // xxx: revisit these constants. 
                        hash_set bigger{__size_load() > 0.4 ? this->capacity_ * 2 : this->capacity_};

                        for (iterator i = begin(); i != end(); ++i) {
                                // xxx: this insert() makes us recompute the hash, which isn't great
                                bigger.insert(std::move(this->data_vec_[i.offset].val));
                                this->meta_vec_[i.offset] &= ~0x1;
                        }

                        swap(bigger);
                }

                const size_t hash = do_hash(val);
                const size_t start = hash % this->capacity_;

                size_t i = start;
                
                do {
                        uint8_t meta = this->meta_vec_[i];

                        if (!(meta & 0x1)) {
                                this->meta_vec_[i] = 0x3 | ((hash & 0x3f) << 2);

                                // this slot has never been tombstoned before, so we got a new ts
                                if (!(meta & 0x2))
                                        ++tombstones_;

                                this->data_vec_[i].hash = hash;
                                new (&this->data_vec_[i].val) T{std::forward<U>(val)};
                                ++size_;
                                return make_pair(iterator{this->capacity_, this->meta_vec_,
                                                        this->data_vec_, i}, true);
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

        size_t do_hash(const T& val) const
        {
                return std::hash<T>{}(val);
        }

private:
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
};

template <typename T>
void swap(hash_set<T> & lhs, hash_set<T> & rhs)
{
        lhs.swap(rhs);
}
