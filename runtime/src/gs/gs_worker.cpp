#include "anyps2/runtime/gs/gs_worker.h"

#include <chrono>
#include <utility>

#include "anyps2/runtime/host_profile.h"

namespace anyps2::rt::gs {

GsWorker::GsWorker(bool threaded) : threaded_(threaded) {
    if (threaded_) thread_ = std::thread([this] { run(); });
}

GsWorker::~GsWorker() {
    stop();
}

void GsWorker::push(Task task) {
    if (!threaded_) {
        task();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(task));
    }
    workCv_.notify_one();
}

void GsWorker::drain() {
    if (!threaded_) return;
    std::unique_lock<std::mutex> lock(mutex_);
    idleCv_.wait(lock, [this] { return queue_.empty() && !busy_; });
    if (error_) {
        const std::exception_ptr e = std::exchange(error_, nullptr);
        lock.unlock();
        std::rethrow_exception(e);
    }
}

void GsWorker::stop() noexcept {
    if (!threaded_ || !thread_.joinable()) return;
    try {
        drain();
    } catch (...) {
        // Erro de uma tarefa já relançado no ponto em que o EE esperou por ele;
        // parar não pode lançar (é chamado pelo destrutor).
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    workCv_.notify_all();
    thread_.join();
}

void GsWorker::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        workCv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        if (queue_.empty()) return;  // parando e sem mais trabalho
        Task task = std::move(queue_.front());
        queue_.pop_front();
        busy_ = true;
        lock.unlock();
        std::exception_ptr failure;
        const bool timed = HostProfile::enabled();
        const auto t0 = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        try {
            task();
        } catch (...) {
            failure = std::current_exception();
        }
        if (timed) {
            HostProfile::addWorker(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
        }
        // Solta o que a tarefa capturou antes de avisar que o worker está livre.
        task = nullptr;
        lock.lock();
        if (failure && !error_) error_ = failure;
        busy_ = false;
        if (queue_.empty()) idleCv_.notify_all();
    }
}

}  // namespace anyps2::rt::gs
