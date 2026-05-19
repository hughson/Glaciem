/*
 * Minimal stand-in for epee's syncobj.h for the keygen library.
 *
 * mlocker.cpp #includes syncobj.h purely for CRITICAL_REGION_LOCAL. The real
 * header pulls all of boost::thread (condition_variable, recursive_mutex,
 * thread); its win32 variant forward-declares `class boost::mutex`, which
 * collides with the boost::mutex shim. mlocker only needs a scoped lock over
 * its std::mutex, so this provides exactly that.
 */
#pragma once
#include <mutex>

#define CRITICAL_REGION_LOCAL(x)  std::lock_guard<std::mutex> critical_region_var(x)
