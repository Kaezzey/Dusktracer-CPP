#include "core/preview_worker.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <stdexcept>

static int checks = 0;
static void require(bool ok,const char* message) {
    ++checks; if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
static preview_worker::result wait_for(preview_worker& worker,const std::string& key) {
    auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        for (auto& result : worker.take_results()) if (result.key == key) return result;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(false,"worker finishes bounded test workload"); return {};
}
int main() {
    {
        preview_worker worker;
        std::promise<void> started, release;
        auto gate = release.get_future().share(); auto main_thread = std::this_thread::get_id();
        std::atomic<bool> off_thread{false}; std::atomic<int> executed{0};
        worker.request("blocker",1,[&] { off_thread = std::this_thread::get_id() != main_thread; started.set_value(); gate.wait(); return preview_image{}; });
        require(started.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready,"worker starts independently");
        for (int revision = 1; revision <= 100; ++revision) {
            // These submissions return while the CPU job is still blocked.
            require(worker.request("material",revision,[&,revision] {
                ++executed; preview_image image; image.pixels = {(unsigned char)revision}; return image;
            }),"rapid edits coalesce without blocking");
        }
        release.set_value(); auto result = wait_for(worker,"material");
        require(off_thread,"CPU work never executes on caller thread");
        require(executed == 1 && result.revision == 100 && result.image.pixels[0] == 100,"only the newest pending snapshot executes");
        worker.request("throw",1,[]() -> preview_image { throw std::runtime_error("decode failed"); });
        require(wait_for(worker,"throw").image.pixels.empty(),"failed work returns a harmless empty result");
        worker.request("recovery",2,[] { preview_image image; image.pixels = {42}; return image; });
        require(wait_for(worker,"recovery").image.pixels[0] == 42,"worker survives a failed decode");
    }
    {
        preview_worker worker; std::promise<void> started, release;
        auto gate = release.get_future().share(); std::atomic<int> discarded{0};
        worker.request("active",1,[&] { started.set_value(); gate.wait(); return preview_image{}; });
        require(started.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready,"bounded queue blocker starts");
        for (size_t i = 0; i < preview_worker::capacity; ++i)
            require(worker.request(std::to_string(i),1,[&] { ++discarded; return preview_image{}; }),"queue accepts capacity");
        require(!worker.request("overflow",1,[] { return preview_image{}; }),"queue bounds retained snapshot memory");
        require(worker.request("0",99,[] { return preview_image{}; }),"same-asset update remains accepted at capacity");
        auto stopped = std::async(std::launch::async,[&] { worker.stop(); });
        require(stopped.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout,"shutdown joins the active job");
        release.set_value(); require(stopped.wait_for(std::chrono::seconds(2)) == std::future_status::ready,"shutdown completes after active job");
        require(discarded == 0,"shutdown discards queued work");
        require(!worker.request("late",1,[] { return preview_image{}; }),"stopped worker rejects further work");
    }
    {
        preview_worker worker;
        for (size_t i = 0; i < preview_worker::capacity; ++i)
            worker.request(std::to_string(i),1,[] { return preview_image{}; });
        std::promise<void> produced; auto ready = produced.get_future();
        auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(5);
        bool accepted = false;
        while (!accepted && std::chrono::steady_clock::now() < deadline) {
            accepted = worker.request("last",33,[&] { produced.set_value(); return preview_image{}; });
            if (!accepted) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(accepted && ready.wait_for(std::chrono::seconds(5)) == std::future_status::ready,"completed result queue reaches capacity");
        require(worker.take_results().size() == preview_worker::capacity,"full result queue preserves every completed asset");
        require(wait_for(worker,"last").revision == 33,"draining results releases worker backpressure without dropping work");
    }
    std::cout << checks << " worker checks passed\n";
}
