#include "anyps2/runtime/gs/gs_worker.h"

#include <chrono>
#include <utility>

#include "anyps2/runtime/host_profile.h"

namespace anyps2::rt::gs {

namespace {

// Roda uma tarefa e devolve a exceção dela (se houver), somando o tempo de
// execução ao perfil do worker. A espera numa barreira não entra no tempo.
template <class F>
std::exception_ptr runTimed(F&& fn) {
    const bool timed = HostProfile::enabled();
    const auto t0 = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    std::exception_ptr failure;
    try {
        fn();
    } catch (...) {
        failure = std::current_exception();
    }
    if (timed) {
        HostProfile::addWorker(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    }
    return failure;
}

}  // namespace

GsWorker::GsWorker(bool threaded, unsigned lanes)
    : threaded_(threaded), lanes_(threaded && lanes > 0 ? lanes : 1), queues_(lanes_) {
    if (!threaded_) return;
    HostProfile::setWorkerLanes(lanes_);
    waitNs_ = std::make_unique<std::atomic<std::int64_t>[]>(std::size_t{lanes_} * kBarrierKinds);
    for (std::size_t i = 0; i < std::size_t{lanes_} * kBarrierKinds; ++i) waitNs_[i].store(0, std::memory_order_relaxed);
    workCv_.reserve(lanes_);
    for (unsigned i = 0; i < lanes_; ++i) workCv_.push_back(std::make_unique<std::condition_variable>());
    threads_.reserve(lanes_);
    for (unsigned i = 0; i < lanes_; ++i) threads_.emplace_back([this, i] { run(i); });
}

GsWorker::~GsWorker() {
    stop();
}

void GsWorker::push(unsigned lane, const std::shared_ptr<const Task>& task) {
    if (!threaded_) {
        (*task)(0, 1);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queues_[lane].push_back(Item{task, nullptr});
        ++pending_;
    }
    workCv_[lane]->notify_one();
}

std::int64_t GsWorker::barrierWaitNs(unsigned lane, unsigned kind) const {
    if (!waitNs_ || lane >= lanes_ || kind >= kBarrierKinds) return 0;
    return waitNs_[std::size_t{lane} * kBarrierKinds + kind].load(std::memory_order_relaxed);
}

void GsWorker::barrier(Op op, unsigned kind) {
    ++issued_;
    if (!threaded_) {
        op();
        done_.fetch_add(1, std::memory_order_release);
        return;
    }
    auto b = std::make_shared<Barrier>();
    b->op = std::move(op);
    b->kind = kind < kBarrierKinds ? kind : kBarrierKinds - 1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& q : queues_) q.push_back(Item{nullptr, b});
        pending_ += lanes_;
    }
    for (auto& cv : workCv_) cv->notify_one();
}

void GsWorker::drain() {
    if (!threaded_) return;
    std::unique_lock<std::mutex> lock(mutex_);
    idleCv_.wait(lock, [this] { return pending_ == 0; });
    if (error_) {
        const std::exception_ptr e = std::exchange(error_, nullptr);
        lock.unlock();
        std::rethrow_exception(e);
    }
}

bool GsWorker::idle() {
    if (!threaded_) return true;
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_ == 0;
}

void GsWorker::stop() noexcept {
    if (threads_.empty()) return;
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
    for (auto& cv : workCv_) cv->notify_all();
    for (auto& t : threads_) t.join();
    threads_.clear();
}

void GsWorker::run(unsigned lane) {
    std::unique_lock<std::mutex> lock(mutex_);
    std::deque<Item>& q = queues_[lane];
    for (;;) {
        workCv_[lane]->wait(lock, [this, &q] { return stopping_ || !q.empty(); });
        if (q.empty()) return;  // parando e sem mais trabalho
        Item item = std::move(q.front());
        q.pop_front();
        std::exception_ptr failure;
        if (item.barrier) {
            Barrier& b = *item.barrier;
            const bool timed = forceTiming_.load(std::memory_order_relaxed) || HostProfile::enabled();
            const auto arrival = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            if (++b.arrived == lanes_) {
                // Última faixa a chegar: as outras esperam em barrierCv_, então
                // a operação roda sozinha, sem o mutex.
                lock.unlock();
                failure = runTimed([&b] { b.op(); });
                lock.lock();
                done_.fetch_add(1, std::memory_order_release);
                b.done = true;
                barrierCv_.notify_all();
            } else {
                barrierCv_.wait(lock, [&b] { return b.done; });
            }
            if (timed) {
                waitNs_[std::size_t{lane} * kBarrierKinds + b.kind].fetch_add(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - arrival)
                        .count(),
                    std::memory_order_relaxed);
            }
        } else {
            lock.unlock();
            failure = runTimed([&] { (*item.task)(lane, lanes_); });
            lock.lock();
        }
        if (failure && !error_) error_ = failure;
        if (--pending_ == 0) idleCv_.notify_all();
    }
}

}  // namespace anyps2::rt::gs
