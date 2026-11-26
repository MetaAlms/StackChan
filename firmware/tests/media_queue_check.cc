#include "bounded_media_queue.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>

int main()
{
    webrtc_media::SpscQueue<std::vector<int>, 2> q;
    std::vector<int> first{1,2,3};
    assert(q.Push(std::move(first)));
    assert(q.Push(std::vector<int>{4}));
    assert(!q.Push(std::vector<int>{5}));
    std::vector<int> out;
    assert(q.Pop(out) && out == std::vector<int>({1,2,3}));
    assert(q.Pop(out) && out == std::vector<int>({4}));
    assert(!q.Pop(out));
    // One producer, one consumer: every admitted owned item appears once.
    webrtc_media::SpscQueue<int, 8> concurrent;
    std::atomic<int> admitted{0}, done{0};
    auto producer = [&](int base) {
        for (int i=0;i<10000;++i) {
            if (concurrent.Push(int(base+i))) ++admitted;
        }
        ++done;
    };
    std::thread a(producer,0);
    std::vector<bool> seen(20000,false);
    int received=0, value;
    while (done.load()!=1) {
        if (concurrent.Pop(value)) {
            assert(value>=0 && value<20000 && !seen[value]);
            seen[value]=true; ++received;
        }
    }
    a.join();
    while (concurrent.Pop(value)) {
        assert(value>=0 && value<20000 && !seen[value]);
        seen[value]=true; ++received;
    }
    assert(received==admitted.load());
    puts("PASS: bounded FIFO, owned payload and SPSC producer/consumer accounting");
}
