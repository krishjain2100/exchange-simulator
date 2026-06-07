#pragma once
#include "Order.h"
#include <cstdint>
#include <vector>

uint64_t HashTrade(uint16_t instrument_id, uint64_t maker_sequence_id,
                   uint64_t taker_sequence_id, uint32_t executed_quantity,
                   uint64_t execution_price);

uint64_t HashSortedBook(std::vector<Order> &resting_orders);
