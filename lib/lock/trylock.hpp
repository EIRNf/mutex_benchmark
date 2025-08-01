#ifndef LOCK_TRYLOCK_HPP
#define LOCK_TRYLOCK_HPP

#pragma once

#include "lock.hpp"

class TryLock : public virtual SoftwareMutex {
    virtual bool trylock(size_t thread_id) = 0;
    virtual void region_init(size_t num_threads, volatile char *_cxl_region) = 0;
};

#endif // LOCK_TRYLOCK_HPP