#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <random>

int BuildOrderJSON(char* buffer, size_t max_len, uint64_t seq_id, uint16_t instr_id, int type, uint64_t price, uint32_t qty, int side) {
    return snprintf(buffer, max_len, 
        R"({"s": %llu, "i": %u, "t": %d, "p": %llu, "q": %u, "side": %d})", 
        seq_id, instr_id, type, price, qty, side);
}

void BotWorker(int bot_id, int orders_to_send) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return;

    sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(8080);
    inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        std::cerr << "[Bot " << bot_id << "] Connection failed.\n";
        close(sock);
        return;
    }

    char send_buffer[128]; 

    // Randomization
    // Seed the random number generator uniquely for each bot
    std::mt19937 rng(1337 + bot_id); 
    std::uniform_int_distribution<int> side_dist(1, 2); 
    std::uniform_int_distribution<int> inst_dist(1, 3);     
    std::uniform_int_distribution<int> price_dist(98, 102); 
    std::uniform_int_distribution<int> qty_dist(1, 5); 

    for (int i = 0; i < orders_to_send; ++i) {
        uint64_t seq_id = (bot_id * 1000ULL) + i; 
        int side = side_dist(rng);
        uint16_t inst = inst_dist(rng);
        uint64_t price = price_dist(rng);
        uint32_t qty = qty_dist(rng) * 10; 

        int len = BuildOrderJSON(send_buffer, sizeof(send_buffer), seq_id, inst, 1, price, qty, side);
        send(sock, send_buffer, len, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); 
    }
}

int main() {
    const int NUM_BOTS = 2;
    const int ORDERS_PER_BOT = 15; // Total 30 orders hitting the engine
    
    std::cout << "Starting Micro-Fleet (" << NUM_BOTS << " bots, " << ORDERS_PER_BOT << " orders each)...\n";

    std::vector<std::thread> fleet;
    
    for (int i = 1; i <= NUM_BOTS; ++i) {
        fleet.emplace_back(BotWorker, i, ORDERS_PER_BOT);
    }

    for (auto& bot : fleet) {
        bot.join();
    }

    std::cout << "Micro-Fleet execution complete.\n";
    return 0;
}