/*
 * Boost-thread shim for the keygen library.
 *
 * crypto.cpp and epee/mlocker.cpp use boost::mutex only to serialise an RNG /
 * page-lock table -- they need a mutex, not boost specifically. Mapping it to
 * std::mutex lets keygen build for macOS / Android / MinGW with no linked boost
 * library. This shim dir is placed first on the include path so these two
 * source files pick it up; every other (header-only) boost header still
 * resolves to the real boost headers.
 */
#pragma once
#include <mutex>
namespace boost { using mutex = std::mutex; }
