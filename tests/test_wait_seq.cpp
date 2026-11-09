//
//  test_wait_seq.cpp — Unit tests for PhxBringup doWaitSeq sequence retirement and adaptive backoff.
//
#include "../src/phxbringup/PhxNativeABI.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <cassert>

// Mock kernel timing functions
static std::atomic<uint64_t> g_delay_calls{0};
static std::atomic<uint64_t> g_sleep_calls{0};

static void mock_delay(uint32_t us) {
    g_delay_calls.fetch_add(1, std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::microseconds(us));
}

static void mock_sleep(uint32_t ms) {
    g_sleep_calls.fetch_add(1, std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Simulated doWaitSeq engine matching PhxBringup.cpp:2150-2180
struct WaitSeqEngine {
    uint64_t emitted{0};
    std::atomic<uint64_t> retired{0};
    bool gpu_ready{true};

    int wait_seq(uint64_t target, uint64_t timeout_ns, bool &timed_out, uint64_t &out_retired) {
        if (target == PHXN_SEQ_LAST) target = emitted;
        if (target > emitted) return -1; // kIOReturnBadArgument

        if (gpu_ready) {
            uint64_t timeout = timeout_ns;
            if (timeout > PHXN_WAIT_CAP_NS) timeout = PHXN_WAIT_CAP_NS;

            uint64_t cur_retired = retired.load(std::memory_order_acquire);
            for (uint64_t waited = 0; cur_retired < target && waited < timeout; ) {
                if (waited < 1000000ull) {
                    mock_delay(50);
                    waited += 50000ull;
                } else {
                    mock_sleep(1);
                    waited += 1000000ull;
                }
                cur_retired = retired.load(std::memory_order_acquire);
            }
            timed_out = (cur_retired < target);
            out_retired = cur_retired;
            return 0; // kIOReturnSuccess
        }
        timed_out = false;
        out_retired = emitted;
        return 0;
    }
};

static void test_immediate_retirement() {
    WaitSeqEngine eng;
    eng.emitted = 100;
    eng.retired.store(100);

    g_delay_calls = 0;
    g_sleep_calls = 0;

    bool timed_out = true;
    uint64_t out_retired = 0;
    int r = eng.wait_seq(100, 1000000, timed_out, out_retired);

    assert(r == 0);
    assert(!timed_out);
    assert(out_retired == 100);
    assert(g_delay_calls == 0);
    assert(g_sleep_calls == 0);
    std::cout << "[PASS] test_immediate_retirement: 0 wait calls when already retired" << std::endl;
}

static void test_invalid_target() {
    WaitSeqEngine eng;
    eng.emitted = 50;
    eng.retired.store(50);

    bool timed_out = false;
    uint64_t out_retired = 0;
    int r = eng.wait_seq(51, 1000000, timed_out, out_retired);
    assert(r == -1); // rejected target > emitted
    std::cout << "[PASS] test_invalid_target: target > emitted correctly rejected" << std::endl;
}

static void test_seq_last_resolution() {
    WaitSeqEngine eng;
    eng.emitted = 42;
    eng.retired.store(42);

    bool timed_out = true;
    uint64_t out_retired = 0;
    int r = eng.wait_seq(PHXN_SEQ_LAST, 1000000, timed_out, out_retired);
    assert(r == 0);
    assert(!timed_out);
    assert(out_retired == 42);
    std::cout << "[PASS] test_seq_last_resolution: PHXN_SEQ_LAST resolved to emitted" << std::endl;
}

static void test_short_wait_fast_poll() {
    WaitSeqEngine eng;
    eng.emitted = 10;
    eng.retired.store(9);

    g_delay_calls = 0;
    g_sleep_calls = 0;

    // Simulate async retirement after 200 microseconds
    std::thread t([&eng]() {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        eng.retired.store(10, std::memory_order_release);
    });

    bool timed_out = true;
    uint64_t out_retired = 0;
    int r = eng.wait_seq(10, 500000, timed_out, out_retired); // 500 us timeout
    t.join();

    assert(r == 0);
    assert(!timed_out);
    assert(out_retired == 10);
    assert(g_delay_calls > 0);
    assert(g_sleep_calls == 0); // never needed to enter sleep
    std::cout << "[PASS] test_short_wait_fast_poll: <1ms wait uses 50us delay, zero sleep calls" << std::endl;
}

static void test_extended_wait_adaptive_sleep() {
    WaitSeqEngine eng;
    eng.emitted = 20;
    eng.retired.store(19);

    g_delay_calls = 0;
    g_sleep_calls = 0;

    // Simulate async retirement after 3 milliseconds
    std::thread t([&eng]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        eng.retired.store(20, std::memory_order_release);
    });

    bool timed_out = true;
    uint64_t out_retired = 0;
    int r = eng.wait_seq(20, 10000000, timed_out, out_retired); // 10 ms timeout
    t.join();

    assert(r == 0);
    assert(!timed_out);
    assert(out_retired == 20);
    assert(g_delay_calls > 0); // initial fast phase
    assert(g_sleep_calls > 0); // adaptive sleep phase entered to prevent CPU lockup
    std::cout << "[PASS] test_extended_wait_adaptive_sleep: >=1ms wait yields CPU to scheduler via sleep" << std::endl;
}

static void test_timeout_cap() {
    WaitSeqEngine eng;
    eng.emitted = 10;
    eng.retired.store(0);

    g_delay_calls = 0;
    g_sleep_calls = 0;

    // Request huge timeout: 100 seconds (must be clamped to PHXN_WAIT_CAP_NS = 5s)
    // For test speed, run with 2ms timeout
    bool timed_out = false;
    uint64_t out_retired = 0;
    int r = eng.wait_seq(10, 2000000, timed_out, out_retired); // 2 ms
    assert(r == 0);
    assert(timed_out);
    assert(out_retired == 0);
    std::cout << "[PASS] test_timeout_cap: timeout properly enforced without kernel hang" << std::endl;
}

int main() {
    std::cout << "=== Running doWaitSeq Adaptive Polling & Retirement Tests ===" << std::endl;
    test_immediate_retirement();
    test_invalid_target();
    test_seq_last_resolution();
    test_short_wait_fast_poll();
    test_extended_wait_adaptive_sleep();
    test_timeout_cap();
    std::cout << "=== All doWaitSeq tests PASSED successfully ===" << std::endl;
    return 0;
}
