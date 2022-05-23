#pragma once

#include <cstring>
#include <chrono>
#include <iostream>

namespace thread_pool {

/**
 * @brief A global variable to control whether or not debug logging code should be enabled.
 * @detail Debug printing may prevent some races from happen. Use with caution.
 */
static constexpr bool debug = false;

static inline uint64_t get_timestamp() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
}

#define __FILENAME__ (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)

// msg should be a single string.
// This is to avoid multiple << resulting interleaved debug message.
#define DBG_THREAD_POOL(msg) \
    do { \
        if constexpr (debug) { \
            std::cout << std::to_string(get_timestamp()) + " " + (std::string(__FILENAME__)  + ":" + std::to_string(__LINE__) \
            + " " + (msg) + "\n"); \
        } \
    } while (0)

}  // namespace thread_pool
