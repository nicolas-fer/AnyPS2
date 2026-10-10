#pragma once

#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>

namespace anyps2::rt::gs {

// Fila FIFO de operações do GS com uma thread só. O EE (produtor) enfileira
// desenhos, transferências e cargas de CLUT na ordem em que o GIF os entrega, e
// o worker executa essa mesma sequência: o GS trabalha em paralelo com o EE e o
// VU1, mas nenhuma operação começa antes de terminarem as anteriores, então os
// pixels são os mesmos de antes.
//
// Sem thread (threaded = false) cada tarefa roda na hora, na thread do EE: é o
// caminho síncrono de antes, usado para comparar (ANYPS2_GS_THREAD=0).
class GsWorker {
public:
    using Task = std::function<void()>;

    explicit GsWorker(bool threaded);
    ~GsWorker();
    GsWorker(const GsWorker&) = delete;
    GsWorker& operator=(const GsWorker&) = delete;

    bool threaded() const { return threaded_; }

    // Enfileira uma tarefa (sem thread, executa já).
    void push(Task task);
    // Espera a fila esvaziar e o worker ficar ocioso. Relança a primeira exceção
    // que uma tarefa lançou: o worker não tem para onde propagá-la.
    void drain();
    // Drena sem relançar e junta a thread. Pode ser chamado mais de uma vez.
    void stop() noexcept;

private:
    void run();

    bool threaded_;
    std::mutex mutex_;
    std::condition_variable workCv_;  // há tarefa na fila, ou pedido de parada
    std::condition_variable idleCv_;  // a fila esvaziou e nada está em curso
    std::deque<Task> queue_;
    bool busy_ = false;
    bool stopping_ = false;
    std::exception_ptr error_;
    std::thread thread_;
};

}  // namespace anyps2::rt::gs
