#include "anyps2/runtime/host_thread.h"

#include <stdexcept>

#ifdef _WIN32
#define NOMINMAX
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace anyps2::rt {

struct HostThread::Impl {
    std::function<void()> body;
    bool joined = false;
#ifdef _WIN32
    HANDLE handle = nullptr;
    static unsigned __stdcall entry(void* p) {
        static_cast<Impl*>(p)->body();
        return 0;
    }
#else
    pthread_t handle{};
    static void* entry(void* p) {
        static_cast<Impl*>(p)->body();
        return nullptr;
    }
#endif
};

HostThread::HostThread(std::function<void()> body, std::size_t stackBytes) : impl_(std::make_unique<Impl>()) {
    impl_->body = std::move(body);
#ifdef _WIN32
    impl_->handle = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, static_cast<unsigned>(stackBytes),
                                                            &Impl::entry, impl_.get(),
                                                            STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr));
    if (!impl_->handle) throw std::runtime_error("não foi possível criar thread do host");
#else
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stackBytes);
    const int rc = pthread_create(&impl_->handle, &attr, &Impl::entry, impl_.get());
    pthread_attr_destroy(&attr);
    if (rc != 0) throw std::runtime_error("não foi possível criar thread do host");
#endif
}

void HostThread::join() {
    if (impl_->joined) return;
    impl_->joined = true;
#ifdef _WIN32
    WaitForSingleObject(impl_->handle, INFINITE);
    CloseHandle(impl_->handle);
#else
    pthread_join(impl_->handle, nullptr);
#endif
}

HostThread::~HostThread() {
    join();
}

}  // namespace anyps2::rt
