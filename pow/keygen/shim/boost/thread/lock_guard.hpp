/* Boost-thread shim for the keygen library -- see mutex.hpp. */
#pragma once
#include <mutex>
namespace boost { template <class M> using lock_guard = std::lock_guard<M>; }
