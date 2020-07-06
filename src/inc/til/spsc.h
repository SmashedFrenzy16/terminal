// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

// til::spsc::details::arc requires std::atomic<size_type>::wait() and ::notify_one() and at the time of writing no
// STL supports these. Since both Windows and Linux offer a Futex implementation we can easily implement this though.
// On other platforms we fall back to using a std::condition_variable. We prefer our own Windows/Linux implementations
// over a potential C++20 one, due to our implementation already being "optimal" on those platforms.
// For instance the atomic "size_type" is 32 Bit, which happens to be the exact size Linux futex
// supports, allowing us to leverage that futex without any indirections like hash tables.
#if defined(_WIN32_WINNT) && _WIN32_WINNT >= _WIN32_WINNT_WIN8
#define _TIL_SPSC_DETAIL_POSITION_IMPL_WIN 1
#elif __linux__
#define _TIL_SPSC_DETAIL_POSITION_IMPL_LINUX 1
#elif __cpp_lib_atomic_wait >= 201907
#define _TIL_SPSC_DETAIL_POSITION_IMPL_NATIVE 1
#else
#define _TIL_SPSC_DETAIL_POSITION_IMPL_FALLBACK 1
#endif

namespace til::spsc // Terminal Implementation Library. Also: "Today I Learned"
{
    using size_type = uint32_t;

    namespace details
    {
        static constexpr size_type position_mask = std::numeric_limits<size_type>::max() >> 2u; // 0b00111....
        static constexpr size_type revolution_flag = 1u << (std::numeric_limits<size_type>::digits - 2u); // 0b01000....
        static constexpr size_type drop_flag = 1u << (std::numeric_limits<size_type>::digits - 1u); // 0b10000....

        inline void validate_size(size_t v)
        {
            if (v > static_cast<size_t>(position_mask))
            {
                throw std::overflow_error{ "size too large for spsc" };
            }
        }

#if _TIL_SPSC_DETAIL_POSITION_IMPL_NATIVE
        using atomic_size_type = std::atomic<size_type>;
#else
        struct atomic_size_type
        {
            size_type load(std::memory_order order) const noexcept
            {
                return _value.load(order);
            }

            void store(size_type desired, std::memory_order order) noexcept
            {
#if _TIL_SPSC_DETAIL_POSITION_IMPL_FALLBACK
                // We must use a lock here to prevent us from modifying the value
                // in between wait() reading the value and the thread being suspended.
                std::lock_guard<std::mutex> lock{ _m };
#endif
                _value.store(desired, order);
            }

            void wait(size_type old, [[maybe_unused]] std::memory_order order) noexcept
            {
#if _TIL_SPSC_DETAIL_POSITION_IMPL_WIN
                WaitOnAddress(&_value, &old, sizeof(_value), INFINITE);
#elif _TIL_SPSC_DETAIL_POSITION_IMPL_LINUX
                futex(FUTEX_WAIT_PRIVATE, old);
#elif _TIL_SPSC_DETAIL_POSITION_IMPL_FALLBACK
                std::unique_lock<std::mutex> lock{ _m };
                _cv.wait(lock, [&]() { return _value.load(order) != old; });
#endif
            }

            void notify_one() noexcept
            {
#if _TIL_SPSC_DETAIL_POSITION_IMPL_WIN
                WakeByAddressSingle(&_value);
#elif _TIL_SPSC_DETAIL_POSITION_IMPL_LINUX
                futex(FUTEX_WAKE_PRIVATE, 1);
#elif _TIL_SPSC_DETAIL_POSITION_IMPL_FALLBACK
                _cv.notify_one();
#endif
            }

        private:
#if _TIL_SPSC_DETAIL_POSITION_IMPL_LINUX
            inline void futex(int futex_op, size_type val)
            {
                // See: https://man7.org/linux/man-pages/man2/futex.2.html
                static_assert(sizeof(std::atomic<size_type>) == 4);
                syscall(SYS_futex, &_value, futex_op, val, nullptr, nullptr, 0);
            }
#endif

            std::atomic<size_type> _value{ 0 };

#if _TIL_SPSC_DETAIL_POSITION_IMPL_FALLBACK
        private:
            std::mutex _m;
            std::condition_variable _cv;
#endif
        };
#endif

        template<typename T>
        inline T* alloc_raw_memory(size_t size)
        {
            constexpr auto alignment = alignof(T);
            if constexpr (alignment <= __STDCPP_DEFAULT_NEW_ALIGNMENT__)
            {
                return static_cast<T*>(::operator new(size));
            }
            else
            {
                return static_cast<T*>(::operator new(size, std::align_val_t(alignment)));
            }
        }

        template<typename T>
        inline void free_raw_memory(T* ptr) noexcept
        {
            constexpr auto alignment = alignof(T);
            if constexpr (alignment <= __STDCPP_DEFAULT_NEW_ALIGNMENT__)
            {
                ::operator delete(ptr);
            }
            else
            {
                ::operator delete(ptr, std::align_val_t(alignment));
            }
        }

        struct acquisition
        {
            size_type begin = 0;
            size_type end = 0;
            size_type next = 0;

            constexpr explicit operator bool() const noexcept
            {
                return end != 0;
            }
        };

        // arc follows the classic spsc design and manages a ring buffer with two positions: _producer and _consumer.
        // They contain the position the producer / consumer will next write to / read from respectively.
        // The producer's writable range is [_producer, _consumer) and the consumer's readable is [_consumer, _producer).
        // As these are symmetric, the logic for acquiring and releasing ranges is the same for both sides.
        // The producer will acquire() and release() ranges with its own position as "mine" and the consumer's position as "theirs".
        // The arguments are correspondingly flipped for the consumer.
        //
        // While the producer is logically always ahead of the consumer, due to the underlying
        // buffer being a ring buffer, the producer's position might be smaller than the consumer's
        // position, if both are calculated modulo the buffer's capacity, as we're doing here.
        // As such the logic range [_producer, _consumer) might actually be the two ranges
        //   [_producer, _capacity) & [0, _consumer)
        // if _producer > _consumer, modulo _capacity, since the range wraps around the end of the ring buffer.
        //
        // The producer cannot write more values ahead of the consumer than the buffer's capacity.
        // Inversely the consumer must wait until the producer has written at least one value ahead.
        // In order to implement the first requirement the positions will flip their "revolution_flag" bit each other
        // revolution around the ring buffer. If the positions are identical, except for their "revolution_flag"
        // value it signals to the producer that it's ahead by one "revolution", or capacity-many values.
        // The second requirement can similarly be detected if the two positions are identical including this bit.
        template<typename T>
        struct arc
        {
            arc(size_type capacity) noexcept :
                _data(alloc_raw_memory<T>(size_t(capacity) * sizeof(T))),
                _capacity(capacity)
            {
            }

            ~arc()
            {
                auto beg = _consumer.load(std::memory_order_acquire);
                auto end = _producer.load(std::memory_order_acquire);
                auto differentRevolution = ((beg ^ end) & revolution_flag) != 0;

                beg &= position_mask;
                end &= position_mask;

                // The producer position will always be ahead of the consumer, but since we're dealing
                // with a ring buffer the producer may be wrapped around the end of the buffer.
                // We thus need to deal with 3 potential cases:
                // * No valid data.
                //   If both positions including their revolution bits are identical.
                // * Valid data in the middle of the ring buffer.
                //   If _producer > _consumer.
                // * Valid data at both ends of the ring buffer.
                //   If the revolution bits differ, even if the positions are otherwise identical,
                //   which they might be if the channel contains exactly as many values as its capacity.
                if (end > beg)
                {
                    std::destroy(_data + beg, _data + end);
                }
                else if (differentRevolution)
                {
                    std::destroy(_data, _data + end);
                    std::destroy(_data + beg, _data + _capacity);
                }

                free_raw_memory(_data);
            }

            void drop_producer()
            {
                drop(_producer);
            }

            void drop_consumer()
            {
                drop(_consumer);
            }

            acquisition producer_acquire(size_type slots) noexcept
            {
                return acquire(_producer, _consumer, revolution_flag, slots);
            }

            void producer_release(acquisition acquisition) noexcept
            {
                release(_producer, acquisition);
            }

            acquisition consumer_acquire(size_type slots) noexcept
            {
                return acquire(_consumer, _producer, 0, slots);
            }

            void consumer_release(acquisition acquisition) noexcept
            {
                release(_consumer, acquisition);
            }

            T* data() const noexcept
            {
                return _data;
            }

        private:
            void drop(atomic_size_type& mine)
            {
                // Signal the other side we're dropped. See acquire() for the handling of the drop_flag.
                // We don't need to use release ordering like release() does as each call to
                // any of the sender/receiver methods already results in a call to release().
                // Another release ordered write can't possibly synchronize any more data anyways at this point.
                const auto myPos = mine.load(std::memory_order_relaxed);
                mine.store(myPos | drop_flag, std::memory_order_relaxed);
                mine.notify_one();

                // The first time SPSCBase is dropped (destroyed) we'll set
                // the flag to true and get false, causing us to return early.
                // Only the second time we'll get true.
                // --> The contents are only deleted when both sides have been dropped.
                if (_eitherSideDropped.exchange(true))
                {
                    delete this;
                }
            }

            // NOTE: waitMask MUST be either 0 (consumer) or revolution_flag (producer).
            acquisition acquire(atomic_size_type& mine, atomic_size_type& theirs, size_type waitMask, size_type slots) noexcept
            {
                size_type myPos = mine.load(std::memory_order_relaxed);
                size_type theirPos;

                while (true)
                {
                    // This acquire read synchronizes with the release write in release().
                    theirPos = theirs.load(std::memory_order_acquire);
                    if ((myPos ^ theirPos) != waitMask)
                    {
                        break;
                    }

                    theirs.wait(theirPos, std::memory_order_relaxed);
                }

                // If the other side's position contains a drop flag, as a X -> we need to...
                // * producer -> stop immediately
                //   Only the producer's waitMask is != 0.
                // * consumer -> finish consuming all values and then stop
                //   We're finished if the only difference between our
                //   and the other side's position is the drop flag.
                if ((theirPos & drop_flag) != 0 && (waitMask != 0 || (myPos ^ theirPos) == drop_flag))
                {
                    // An empty acquire struct is equal to false,
                    // which signals that the other side has been dropped.
                    return {};
                }

                auto begin = myPos & position_mask;
                auto end = theirPos & position_mask;

                // [begin, end) is the writable/readable range for the producer/consumer.
                // The following detects whether we'd be wrapping around the end of the ring buffer
                // and splits the range into the first half [mine, _capacity).
                // If acquire() is called again it'll return [0, theirs).
                end = end > begin ? end : _capacity;

                // Of course we also need to ensure to not return more than we've been asked for.
                end = std::min(end, begin + slots);

                // "next" will contain the value that's stored into "mine" when release() is called.
                // It's basically the same as "end", but with the revolution flag spliced in.
                // If we acquired the range [mine, _capacity) "end" will equal _capacity
                // and thus wrap around the ring buffer. The next value for "mine" is thus the
                // position zero | the flipped "revolution" (and 0 | x == x).
                auto revolution = myPos & revolution_flag;
                auto next = end != _capacity ? end | revolution : revolution ^ revolution_flag;

                return {
                    begin,
                    end,
                    next,
                };
            }

            void release(atomic_size_type& mine, acquisition acquisition) noexcept
            {
                // This release write synchronizes with the acquire read in acquire().
                mine.store(acquisition.next, std::memory_order_release);
                mine.notify_one();
            }

            T* const _data;
            const size_type _capacity;

            std::atomic<bool> _eitherSideDropped{ false };

            atomic_size_type _producer;
            atomic_size_type _consumer;
        };
    }

    template<typename T>
    struct sender
    {
        explicit sender(details::arc<T>* arc) noexcept :
            _arc(arc) {}

        sender<T>(const sender<T>&) = delete;
        sender<T>& operator=(const sender<T>&) = delete;

        sender(sender<T>&& other) noexcept
        {
            drop();
            _arc = std::exchange(other._arc, nullptr);
        }

        sender<T>& operator=(sender<T>&& other) noexcept
        {
            drop();
            _arc = std::exchange(other._arc, nullptr);
        }

        ~sender()
        {
            drop();
        }

        // emplace constructs an item in-place at the end of the queue.
        // It returns true, if the item was successfully placed within the queue.
        // The return value will be false, if the receiver is gone.
        template<typename... Args>
        bool emplace(Args&&... args) const
        {
            auto acquisition = _arc->producer_acquire(1);
            if (!acquisition)
            {
                return false;
            }

            auto data = _arc->data();
            auto begin = data + acquisition.begin;
            new (begin) T(std::forward<Args>(args)...);

            _arc->producer_release(acquisition);
            return true;
        }

        // move_n moves count items from the input iterator in into the queue.
        // The resulting iterator is returned as the first pair field.
        // The second pair field will be false if the receiver is gone.
        template<typename InputIt>
        std::pair<InputIt, bool> move_n(InputIt in, size_t count) const
        {
            details::validate_size(count);

            auto data = _arc->data();
            auto ok = true;

            while (count != 0)
            {
                auto acquisition = _arc->producer_acquire(size_type(count));
                if (!acquisition)
                {
                    ok = false;
                    break;
                }

                auto begin = data + acquisition.begin;
                auto got = acquisition.end - acquisition.begin;
                in = std::uninitialized_move_n(in, got, begin).first;
                count -= got;

                _arc->producer_release(acquisition);
            }

            return { in, ok };
        }

    private:
        void drop()
        {
            if (_arc)
            {
                _arc->drop_producer();
            }
        }

        details::arc<T>* _arc = nullptr;
    };

    template<typename T>
    struct receiver
    {
        explicit receiver(details::arc<T>* arc) noexcept :
            _arc(arc) {}

        receiver<T>(const receiver<T>&) = delete;
        receiver<T>& operator=(const receiver<T>&) = delete;

        receiver(receiver<T>&& other) noexcept
        {
            drop();
            _arc = std::exchange(other._arc, nullptr);
        }

        receiver<T>& operator=(receiver<T>&& other) noexcept
        {
            drop();
            _arc = std::exchange(other._arc, nullptr);
        }

        ~receiver()
        {
            drop();
        }

        // pop returns the next item in the queue, or std::nullopt
        // if no items are available and the sender is gone.
        // The call blocks until either of these events occur.
        std::optional<T> pop() const
        {
            auto acquisition = _arc->consumer_acquire(1);
            if (!acquisition)
            {
                return std::nullopt;
            }

            auto data = _arc->data();
            auto begin = data + acquisition.begin;

            auto record = std::move(*begin);
            std::destroy_at(begin);

            _arc->consumer_release(acquisition);
            return record;
        }

        // pop_n writes up to count items into the given output iterator out.
        // The resulting iterator is returned as the first pair field.
        // The second pair field will be false if no items are available and the sender is gone.
        template<typename OutputIt>
        std::pair<OutputIt, bool> pop_n(OutputIt out, size_t count) const
        {
            details::validate_size(count);

            auto data = _arc->data();
            auto ok = true;

            while (count != 0)
            {
                auto acquisition = _arc->consumer_acquire(size_type(count));
                if (!acquisition)
                {
                    ok = false;
                    break;
                }

                auto beg = data + acquisition.begin;
                auto end = data + acquisition.end;
                auto got = acquisition.end - acquisition.begin;

                out = std::move(beg, end, out);
                std::destroy(beg, end);
                count -= got;

                _arc->consumer_release(acquisition);
            }

            return { out, ok };
        }

    private:
        void drop()
        {
            if (_arc)
            {
                _arc->drop_consumer();
            }
        }

        details::arc<T>* _arc = nullptr;
    };

    // channel returns a bounded, lock-free, single-producer, single-consumer
    // FIFO queue ("channel") with the given maximum capacity.
    template<typename T>
    std::pair<sender<T>, receiver<T>> channel(uint32_t capacity)
    {
        if (capacity == 0)
        {
            throw std::invalid_argument{ "invalid capacity" };
        }

        const auto arc = new details::arc<T>(capacity);
        return { std::piecewise_construct, std::forward_as_tuple(arc), std::forward_as_tuple(arc) };
    }
}
