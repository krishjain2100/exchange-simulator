#include "ExchangeEngine.h"
#include "Telemetry.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstdio> 
#include <fstream>

// Queue Data Structures
std::queue<Order> order_queue;
std::mutex queue_mutex;
std::condition_variable queue_cv;
std::atomic<bool> load_test_complete{false};

// In-Memory Ledgers
std::vector<Order> input_ledger;

struct TradeRecord {
    uint16_t instrument_id; 
    uint64_t maker_id, taker_id;
    uint32_t qty;
    uint64_t price;
};


std::vector<TradeRecord> trade_ledger;

// Telemetry 
namespace Telemetry {
    void ReportTrade(uint16_t inst_id, uint64_t maker_id, uint64_t taker_id, uint32_t qty, uint64_t price) {
        trade_ledger.push_back({inst_id, maker_id, taker_id, qty, price});
    }
}

// The Dispatcher Thread 
void DispatcherLoop(ExchangeEngine* engine) {
    std::cout << "[Wrapper] Dispatcher thread started." << std::endl;
    
    // Pre-allocate to avoid resizing overheads
    input_ledger.reserve(100000); 
    trade_ledger.reserve(100000);

    uint64_t total_algo_latency_ns = 0;
    uint64_t total_orders_processed = 0;
    
    while (true) {
        Order current_order;
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            queue_cv.wait(lock, [] { return !order_queue.empty() || load_test_complete.load(); });
            
            if (load_test_complete.load() && order_queue.empty()) break; 
            
            current_order = order_queue.front();
            order_queue.pop();
        } 

        // 1. Record input order in ledger
        input_ledger.push_back(current_order);

        // 2. Process order and measure latency
        auto start_time = std::chrono::high_resolution_clock::now();
        engine->ProcessOrder(current_order);
        auto end_time = std::chrono::high_resolution_clock::now();

        total_algo_latency_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
        total_orders_processed++;
    }
    
    //  Shutdown Sequence
    
    std::cout << "[Wrapper] Engine shutdown triggered. Flushing state to disk..." << std::endl;

     // 1. Flush Ledgers to CSVs
    std::ofstream input_file("input_ledger.csv");
    input_file << "SequenceID,InstrumentID,Type,Side,Price,Qty" << std::endl;
    for (const auto& o : input_ledger) {
        input_file << o.sequence_id << "," << o.instrument_id << "," << static_cast<int>(o.type) << "," 
                   << static_cast<int>(o.side) << "," << o.price << "," << o.quantity << "" << std::endl;
    }
    input_file.close();

    // 2. Flush Trades
    std::ofstream trade_file("trade_ledger.csv");
    trade_file << "InstrumentID,MakerID,TakerID,Qty,Price" << std::endl;
    for (const auto& t : trade_ledger) {
        trade_file << t.instrument_id << "," << t.maker_id << "," << t.taker_id << "," << t.qty << "," << t.price << "" << std::endl;
    }
    trade_file.close();

    // 3. Flush Final Book State
    std::ofstream book_file("final_book.csv");
    book_file << "SequenceID,InstrumentID,Type,Side,Price,Qty" << std::endl;
    std::vector<Order> resting = engine->GetRestingOrders();
    for (const auto& o : resting) {
        book_file << o.sequence_id << "," << o.instrument_id << "," << static_cast<int>(o.type) << "," 
                  << static_cast<int>(o.side) << "," << o.price << "," << o.quantity << "" << std::endl;
    }
    book_file.close();

    uint64_t average_latency = (total_orders_processed > 0) ? (total_algo_latency_ns / total_orders_processed) : 0;
    
    std::ofstream metrics_file("latency.txt");
    metrics_file << average_latency;
    metrics_file.close();

    std::cout << "[Wrapper] CSVs generated: input_ledger.csv, trade_ledger.csv, final_book.csv" << std::endl;
}

//  The TCP Client Session 
void ClientSession(int client_sock) {
    char buffer[1024]; 
    while (true) {
        ssize_t bytes_received = recv(client_sock, buffer, sizeof(buffer) - 1, 0);
        if (bytes_received <= 0) break; 

        buffer[bytes_received] = '\0';
        uint64_t ingress_ns = 0; // just for checking wrapper latency, not the algo latency
        uint64_t s, p;
        uint32_t i, t, q, side;
        
        int parsed_count = sscanf(buffer, 
            "{\"s\": %llu, \"i\": %u, \"t\": %u, \"p\": %llu, \"q\": %u, \"side\": %u}", 
            &s, &i, &t, &p, &q, &side);

        if (parsed_count == 6) {
            Order new_order;
            new_order.sequence_id = s;
            new_order.timestamp_ns = ingress_ns;
            new_order.instrument_id = static_cast<uint16_t>(i);
            new_order.type = static_cast<OrderType>(t);
            new_order.side = static_cast<Side>(side);

            if (new_order.type == OrderType::CANCEL) {
                new_order.price = p; 
                new_order.quantity = 0; 
            } else {
                new_order.price = p;
                new_order.quantity = q;
            }

            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                order_queue.push(new_order);
            }
            queue_cv.notify_one();
        }
    }
    close(client_sock);
}

//  The TCP Listener Thread 
void TCPServer() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == 0) {
        perror("[Server ERROR] Socket creation failed");
        exit(1);
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(8080);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("[Server ERROR] Bind failed. Port 8080 is likely still in use!");
        exit(1);
    }
    if (listen(server_fd, 10) < 0) {
        perror("[Server ERROR] Listen failed");
        exit(1);
    }

    std::cout << "[Server] Listening on Port 8080. Awaiting Bot Fleet..." << std::endl;

    std::vector<std::thread> client_threads;
    int bots_connected = 0;
    
    // Waiting for exactly 2 bots (for micro-fleet)
    // Will change this logic ofcourse.

    while (bots_connected < 2) {
        int new_socket = accept(server_fd, nullptr, nullptr);
        if (new_socket < 0) continue;

        std::cout << "[Server] Bot " << ++bots_connected << " connected!" << std::endl;
        client_threads.emplace_back(ClientSession, new_socket);
    }

    for (auto& th : client_threads) {
        if (th.joinable()) th.join();
    }

    std::cout << "[Server] All bots disconnected. Initiating sequence..." << std::endl;
    close(server_fd);
    
    load_test_complete.store(true);
    queue_cv.notify_all();
}

int main() {
    ExchangeEngine* engine = CreateEngine();
    engine->Init();

    std::thread dispatcher_thread(DispatcherLoop, engine);
    std::thread network_thread(TCPServer);

    network_thread.join();
    dispatcher_thread.join();

    delete engine;
    return 0;
}