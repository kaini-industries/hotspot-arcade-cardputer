#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <thread>
#include <unordered_set>
#include <vector>

#include "ha_async_queue.h"

static void testBoundsAndFifo() {
    HaAsyncQueue<int, 3> queue;
    assert(queue.push(1));
    assert(queue.push(2));
    assert(queue.push(3));
    assert(!queue.push(4));
    assert(queue.size() == 3);
    assert(queue.dropped() == 1);
    int value = 0;
    assert(queue.pop(value) && value == 1);
    assert(queue.pop(value) && value == 2);
    assert(queue.push(5));
    assert(queue.pop(value) && value == 3);
    assert(queue.pop(value) && value == 5);
    assert(!queue.pop(value));
}

static void testConcurrentProducersAndConsumer() {
    constexpr uint32_t producers = 4;
    constexpr uint32_t attemptsPerProducer = 10000;
    constexpr uint32_t attempts = producers * attemptsPerProducer;
    HaAsyncQueue<uint32_t, 64> queue;
    std::atomic<uint32_t> producersDone{0};
    std::vector<uint32_t> received;
    received.reserve(attempts);

    std::thread consumer([&] {
        uint32_t value = 0;
        while(producersDone.load(std::memory_order_acquire) != producers || queue.size()) {
            if(queue.pop(value)) received.push_back(value);
            else std::this_thread::yield();
        }
    });
    std::vector<std::thread> workers;
    for(uint32_t producer = 0; producer < producers; producer++) {
        workers.emplace_back([&, producer] {
            for(uint32_t sequence = 0; sequence < attemptsPerProducer; sequence++)
                queue.push(producer * attemptsPerProducer + sequence);
            producersDone.fetch_add(1, std::memory_order_release);
        });
    }
    for(auto& worker : workers) worker.join();
    consumer.join();

    assert(received.size() + queue.dropped() == attempts);
    std::unordered_set<uint32_t> unique(received.begin(), received.end());
    assert(unique.size() == received.size());
    for(uint32_t value : received) assert(value < attempts);
}

int main() {
    testBoundsAndFifo();
    testConcurrentProducersAndConsumer();
    std::cout << "native async-queue concurrency tests passed\n";
}
