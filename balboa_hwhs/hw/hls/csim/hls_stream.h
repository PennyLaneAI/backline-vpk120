// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Xanadu Quantum Technologies Inc.
//
// hls_stream.h - the part of Vitis HLS's hls::stream that fpga-network-stack's C++ uses
// (read, write, empty, full), for running BALBOA's HLS code as plain C++ in hw/hls/csim.
// An unbounded queue: the FIFO depths of the hardware do not matter for what the test checks.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>

namespace hls {

template <typename T>
class stream {
  public:
    stream() {}
    explicit stream(const char *name) : name_(name) {}

    bool empty() const { return q_.empty(); }
    bool full() const { return false; }
    size_t size() const { return q_.size(); }

    T read()
    {
        if (q_.empty()) {
            std::fprintf(stderr, "hls::stream %s: read while empty\n", name_.c_str());
            std::abort();
        }
        T v = q_.front();
        q_.pop_front();
        return v;
    }
    void read(T &v) { v = read(); }
    void write(const T &v) { q_.push_back(v); }
    void operator>>(T &v) { v = read(); }
    void operator<<(const T &v) { write(v); }

  private:
    std::deque<T> q_;
    std::string name_;
};

}  // namespace hls
