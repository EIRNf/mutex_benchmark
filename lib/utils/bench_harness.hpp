#ifndef __BENCH_HARNESS_HPP_
#define __BENCH_HARNESS_HPP_

#pragma once

#include "../lock/lock.hpp"

// Runs lock->destroy() and frees the lock object the way get_mutex()
// allocated it: numa_delete() in a hardware_cxl build (numa_new<T>()), plain
// `delete` otherwise. Shared by all benchmark apps so cleanup cannot drift.
void destroy_and_delete_lock(SoftwareMutex *lock);

#endif // __BENCH_HARNESS_HPP_
