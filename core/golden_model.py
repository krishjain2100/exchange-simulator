import csv
import sys
from collections import defaultdict

class GoldenEngine:
    def __init__(self):
        # InstrumentID -> Price -> List of Orders
        self.bids = defaultdict(lambda: defaultdict(list))
        self.asks = defaultdict(lambda: defaultdict(list))
        self.trades = []

    def handle_cancel(self, target_id, inst_id):
        for price_level in self.bids[inst_id].values():
            for i, order in enumerate(price_level):
                if order['SequenceID'] == target_id:
                    del price_level[i]
                    return
        for price_level in self.asks[inst_id].values():
            for i, order in enumerate(price_level):
                if order['SequenceID'] == target_id:
                    del price_level[i]
                    return

    def process_order(self, order):
        inst = order['InstrumentID']

        if order['Type'] == 3: # CANCEL
            self.handle_cancel(order['Price'], inst)
            return

        qty_remaining = order['Qty']

        if order['Side'] == 1: # BUY
            book = self.asks[inst]
            for ask_price in sorted(book.keys()):
                if qty_remaining == 0 or (order['Type'] == 1 and order['Price'] < ask_price):
                    break
                
                resting_orders = book[ask_price]
                i = 0
                while i < len(resting_orders) and qty_remaining > 0:
                    maker = resting_orders[i]
                    match_qty = min(qty_remaining, maker['Qty'])
                    
                    self.trades.append({
                        'InstrumentID': inst,
                        'MakerID': maker['SequenceID'],
                        'TakerID': order['SequenceID'],
                        'Qty': match_qty,
                        'Price': ask_price
                    })
                    
                    qty_remaining -= match_qty
                    maker['Qty'] -= match_qty
                    
                    if maker['Qty'] == 0:
                        del resting_orders[i]
                    else:
                        i += 1
                        
                if not book[ask_price]:
                    del book[ask_price]

            if qty_remaining > 0 and order['Type'] == 1:
                order['Qty'] = qty_remaining
                self.bids[inst][order['Price']].append(order)

        elif order['Side'] == 2: # SELL
            book = self.bids[inst]
            for bid_price in sorted(book.keys(), reverse=True):
                if qty_remaining == 0 or (order['Type'] == 1 and order['Price'] > bid_price):
                    break
                
                resting_orders = book[bid_price]
                i = 0
                while i < len(resting_orders) and qty_remaining > 0:
                    maker = resting_orders[i]
                    match_qty = min(qty_remaining, maker['Qty'])
                    
                    self.trades.append({
                        'InstrumentID': inst,
                        'MakerID': maker['SequenceID'],
                        'TakerID': order['SequenceID'],
                        'Qty': match_qty,
                        'Price': bid_price
                    })
                    
                    qty_remaining -= match_qty
                    maker['Qty'] -= match_qty
                    
                    if maker['Qty'] == 0:
                        del resting_orders[i]
                    else:
                        i += 1

                if not book[bid_price]:
                    del book[bid_price]

            if qty_remaining > 0 and order['Type'] == 1:
                order['Qty'] = qty_remaining
                self.asks[inst][order['Price']].append(order)

    def get_resting_orders(self):
        resting = []
        for inst_book in self.bids.values():
            for price_level in inst_book.values():
                resting.extend(price_level)
        for inst_book in self.asks.values():
            for price_level in inst_book.values():
                resting.extend(price_level)
        return sorted(resting, key=lambda x: x['SequenceID'])

# --- Diffing Logic ---
def read_csv(filepath):
    with open(filepath, 'r') as f:
        return [ {k: int(v) for k, v in row.items()} for row in csv.DictReader(f) ]

def verify(target_dir):
    print(f"\n[Judge] Verifying {target_dir}...")
    
    try:
        input_ledger = read_csv(f"{target_dir}/input_ledger.csv")
        cpp_trades = read_csv(f"{target_dir}/trade_ledger.csv")
        cpp_book = read_csv(f"{target_dir}/final_book.csv")
    except FileNotFoundError as e:
        print(f"FAILED: Missing output file - {e}")
        return False

    engine = GoldenEngine()
    for order in input_ledger:
        engine.process_order(order)
        
    py_trades = engine.trades
    py_book = engine.get_resting_orders()
    cpp_book = sorted(cpp_book, key=lambda x: x['SequenceID'])

    if len(cpp_trades) != len(py_trades):
        print(f"FAILED: Trade count mismatch. C++: {len(cpp_trades)}, Python: {len(py_trades)}")
        return False
        
    for i, (cpp_t, py_t) in enumerate(zip(cpp_trades, py_trades)):
        if cpp_t != py_t:
            print(f"FAILED: Trade mismatch at trade {i}.")
            print(f"Expected: {py_t}")
            print(f"Got:      {cpp_t}")
            return False

    if len(cpp_book) != len(py_book):
        print(f"FAILED: Resting order count mismatch. C++: {len(cpp_book)}, Python: {len(py_book)}")
        return False

    for i, (cpp_o, py_o) in enumerate(zip(cpp_book, py_book)):
        for key in ['SequenceID', 'InstrumentID', 'Side', 'Price', 'Qty']: # Added InstrumentID
            if cpp_o[key] != py_o[key]:
                print(f"FAILED: State mismatch at SequenceID {py_o['SequenceID']}.")
                print(f"Expected: {py_o}")
                print(f"Got:      {cpp_o}")
                return False

    print("SUCCESS: C++ Engine perfectly matches the Golden Model.")
    return True

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 golden_model.py /path/to/team_data_dir")
        sys.exit(1)
    
    success = verify(sys.argv[1])
    sys.exit(0 if success else 1)