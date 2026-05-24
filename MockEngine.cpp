#include "ExchangeEngine.h"
#include "Telemetry.h"
#include <iostream>
#include <map>
#include <list>
#include <algorithm>
#include <unordered_map>

class MockEngine : public ExchangeEngine {
private:
    // Outer Map: Instrument ID -> Inner Map: The Order Book
    std::unordered_map<uint16_t, std::map<uint64_t, std::list<Order>, std::greater<uint64_t>>> bids;
    std::unordered_map<uint16_t, std::map<uint64_t, std::list<Order>, std::less<uint64_t>>> asks;

    void HandleCancel(const Order& cancel_order) {
        uint16_t inst = cancel_order.instrument_id;
        uint64_t target_id = cancel_order.price; 
        
        for (auto& [price, order_list] : bids[inst]) {
            for (auto it = order_list.begin(); it != order_list.end(); ++it) {
                if (it->sequence_id == target_id) {
                    order_list.erase(it);
                    return; 
                }
            }
        }
        for (auto& [price, order_list] : asks[inst]) {
            for (auto it = order_list.begin(); it != order_list.end(); ++it) {
                if (it->sequence_id == target_id) {
                    order_list.erase(it);
                    return; 
                }
            }
        }
    }

public:
    void Init() override {
        std::cout << "[MockEngine] Initializing memory pools..." << std::endl;
        bids.clear();
        asks.clear();
    }

    void ProcessOrder(const Order& order) override {
        if (order.type == OrderType::CANCEL) {
            HandleCancel(order);
            return;
        }

        Order current = order; 
        uint16_t inst = current.instrument_id; 

        if (current.side == Side::BUY) {
            auto& ask_book = asks[inst];
            auto it = ask_book.begin();
            
            while (it != ask_book.end() && current.quantity > 0) {
                uint64_t ask_price = it->first;
                if (current.type == OrderType::LIMIT && current.price < ask_price) break; 

                auto& orders_at_price = it->second;
                auto order_it = orders_at_price.begin();

                while (order_it != orders_at_price.end() && current.quantity > 0) {
                    Order& maker = *order_it;
                    uint32_t match_qty = std::min(current.quantity, maker.quantity);

                    Telemetry::ReportTrade(inst, maker.sequence_id, current.sequence_id, match_qty, ask_price);

                    current.quantity -= match_qty;
                    maker.quantity -= match_qty;

                    if (maker.quantity == 0) {
                        order_it = orders_at_price.erase(order_it);
                    } else {
                        ++order_it;
                    }
                }
                if (orders_at_price.empty()) it = ask_book.erase(it);
                else ++it;
            }
            if (current.quantity > 0 && current.type == OrderType::LIMIT) {
                bids[inst][current.price].push_back(current);
            }
        } 
        else if (current.side == Side::SELL) {
            auto& bid_book = bids[inst];
            auto it = bid_book.begin();
            
            while (it != bid_book.end() && current.quantity > 0) {
                uint64_t bid_price = it->first;
                if (current.type == OrderType::LIMIT && current.price > bid_price) break;

                auto& orders_at_price = it->second;
                auto order_it = orders_at_price.begin();

                while (order_it != orders_at_price.end() && current.quantity > 0) {
                    Order& maker = *order_it;
                    uint32_t match_qty = std::min(current.quantity, maker.quantity);

                    Telemetry::ReportTrade(inst, maker.sequence_id, current.sequence_id, match_qty, bid_price);

                    current.quantity -= match_qty;
                    maker.quantity -= match_qty;

                    if (maker.quantity == 0) {
                        order_it = orders_at_price.erase(order_it);
                    } else {
                        ++order_it;
                    }
                }
                if (orders_at_price.empty()) it = bid_book.erase(it);
                else ++it;
            }
            if (current.quantity > 0 && current.type == OrderType::LIMIT) {
                asks[inst][current.price].push_back(current);
            }
        }
    }

    std::vector<Order> GetRestingOrders() const override {
        std::vector<Order> resting;
        // Iterate through all instrument books
        for (const auto& [inst_id, book] : bids) {
            for (const auto& [price, order_list] : book) {
                for (const auto& order : order_list) resting.push_back(order);
            }
        }
        for (const auto& [inst_id, book] : asks) {
            for (const auto& [price, order_list] : book) {
                for (const auto& order : order_list) resting.push_back(order);
            }
        }
        return resting;
    }
};

ExchangeEngine* CreateEngine() {
    return new MockEngine();
}