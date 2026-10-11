#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace anyps2::rt::gs {

// Filas do GS em faixas (uma thread por faixa). O EE (produtor) enfileira os
// desenhos na ordem em que o GIF os entrega; cada faixa tem a sua fila e executa
// a sua parte (linhas que lhe cabem, gs_bands.h) na mesma ordem. Duas coisas
// garantem que os pixels são os mesmos do caminho de uma thread:
//  * dentro de uma faixa a ordem é a do produtor, e cada linha pertence a uma
//    faixa só, então um pixel é escrito pelos mesmos desenhos, na mesma ordem;
//  * as operações globais (HOST→LOCAL, LOCAL→LOCAL, CLUT, reset) são barreiras:
//    entram em todas as filas, e só quando todas as faixas chegaram nelas uma
//    executa a operação; depois todas seguem.
//
// Sem thread (threaded = false) cada tarefa roda na hora, na thread do EE, com
// uma faixa só: é o caminho síncrono (ANYPS2_GS_THREAD=0).
class GsWorker {
public:
    // Trabalho de uma faixa: recebe a faixa (0..lanes−1) e o número de faixas.
    using Task = std::function<void(unsigned lane, unsigned lanes)>;
    // Operação global, executada uma vez por uma das faixas.
    using Op = std::function<void()>;

    // Com threaded, `lanes` threads (ao menos uma); sem thread, uma faixa só.
    GsWorker(bool threaded, unsigned lanes);
    ~GsWorker();
    GsWorker(const GsWorker&) = delete;
    GsWorker& operator=(const GsWorker&) = delete;

    bool threaded() const { return threaded_; }
    unsigned lanes() const { return lanes_; }

    // Enfileira um desenho na faixa `lane` (sem thread, executa já). O mesmo
    // Task pode ir a várias faixas.
    void push(unsigned lane, const std::shared_ptr<const Task>& task);
    // Barreira: enfileira `op` em todas as faixas (sem thread, executa já).
    void barrier(Op op);
    // Espera as filas esvaziarem e as faixas ficarem ociosas. Relança a primeira
    // exceção que uma tarefa lançou: o worker não tem para onde propagá-la.
    void drain();
    // Nada enfileirado nem em execução (sem espera). Sem thread, sempre verdadeiro.
    bool idle();
    // Barreiras enfileiradas (só o produtor chama) e concluídas: quando a de número
    // N conclui, tudo o que foi enfileirado antes dela já terminou em todas as faixas.
    std::uint64_t barriersIssued() const { return issued_; }
    std::uint64_t barriersDone() const { return done_.load(std::memory_order_acquire); }
    // Drena sem relançar e junta as threads. Pode ser chamado mais de uma vez.
    void stop() noexcept;

private:
    struct Barrier {
        unsigned arrived = 0;
        bool done = false;
        Op op;
    };
    struct Item {
        std::shared_ptr<const Task> task;
        std::shared_ptr<Barrier> barrier;
    };

    void run(unsigned lane);

    bool threaded_;
    unsigned lanes_;
    std::mutex mutex_;
    std::vector<std::unique_ptr<std::condition_variable>> workCv_;  // uma por faixa
    std::condition_variable barrierCv_;  // uma barreira foi concluída
    std::condition_variable idleCv_;     // não há item pendente
    std::vector<std::deque<Item>> queues_;
    std::uint64_t issued_ = 0;
    std::atomic<std::uint64_t> done_{0};
    std::size_t pending_ = 0;  // itens por faixa ainda não concluídos
    bool stopping_ = false;
    std::exception_ptr error_;
    std::vector<std::thread> threads_;
};

}  // namespace anyps2::rt::gs
