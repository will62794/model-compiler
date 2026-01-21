// Optimized C++ State Space Explorer for Lamport's Bakery Algorithm
// Generated from Bakery.tla
// Parameters: N=2, Nat={0,1,2,3}

#include <iostream>
#include <fstream>
#include <vector>
#include <unordered_set>
#include <queue>
#include <string>
#include <cstdint>
#include <chrono>
#include <functional>

// Configuration constants
constexpr int N = 3;           // Number of processes
constexpr int MAX_NAT = 3;     // Maximum value in Nat set (0..MAX_NAT)
constexpr int NUM_PCS = 10;    // Number of PC states

// PC labels as enum for efficiency
enum PC : uint8_t {
    PC_ncs = 0,
    PC_e1 = 1,
    PC_e2 = 2,
    PC_e3 = 3,
    PC_e4 = 4,
    PC_w1 = 5,
    PC_w2 = 6,
    PC_cs = 7,
    PC_exit = 8
};

const char* pc_names[] = {"ncs", "e1", "e2", "e3", "e4", "w1", "w2", "cs", "exit"};

// Compact state representation
// For N=2, Nat={0..3}:
// - num[i]: 2 bits each (0-3), total 4 bits
// - flag[i]: 1 bit each, total 2 bits
// - pc[i]: 4 bits each (0-8), total 8 bits
// - unchecked[i]: N bits each (bitmask), total 4 bits
// - max[i]: 2 bits each (0-3), total 4 bits
// - nxt[i]: 1 bit each (1 or 2, stored as 0 or 1), total 2 bits
// Total: ~24 bits, fits in uint32_t but we use uint64_t for fingerprint

struct State {
    uint8_t num[N];           // num[i] in 0..MAX_NAT
    bool flag[N];             // flag[i] boolean
    uint8_t pc[N];            // pc[i] enum PC
    uint8_t unchecked[N];     // unchecked[i] as bitmask (bit j set means j+1 is in set)
    uint8_t max_val[N];       // max[i] in 0..MAX_NAT
    uint8_t nxt[N];           // nxt[i] in 1..N

    // Compute fingerprint for hash-based deduplication
    uint64_t fingerprint() const {
        uint64_t fp = 0;
        uint64_t mult = 1;
        for (int i = 0; i < N; i++) {
            fp += num[i] * mult; mult *= (MAX_NAT + 1);
            fp += flag[i] * mult; mult *= 2;
            fp += pc[i] * mult; mult *= NUM_PCS;
            fp += unchecked[i] * mult; mult *= (1 << N);
            fp += max_val[i] * mult; mult *= (MAX_NAT + 1);
            fp += (nxt[i] - 1) * mult; mult *= N;
        }
        return fp;
    }

    bool operator==(const State& other) const {
        for (int i = 0; i < N; i++) {
            if (num[i] != other.num[i]) return false;
            if (flag[i] != other.flag[i]) return false;
            if (pc[i] != other.pc[i]) return false;
            if (unchecked[i] != other.unchecked[i]) return false;
            if (max_val[i] != other.max_val[i]) return false;
            if (nxt[i] != other.nxt[i]) return false;
        }
        return true;
    }
};

struct StateHash {
    size_t operator()(const State& s) const {
        return s.fingerprint();
    }
};

// JSON output helpers
std::string state_to_json(const State& s) {
    std::string json = "{";

    // num array
    json += "\"num\":{";
    for (int i = 0; i < N; i++) {
        if (i > 0) json += ",";
        json += "\"" + std::to_string(i+1) + "\":" + std::to_string(s.num[i]);
    }
    json += "},";

    // flag array
    json += "\"flag\":{";
    for (int i = 0; i < N; i++) {
        if (i > 0) json += ",";
        json += "\"" + std::to_string(i+1) + "\":" + (s.flag[i] ? "true" : "false");
    }
    json += "},";

    // pc array
    json += "\"pc\":{";
    for (int i = 0; i < N; i++) {
        if (i > 0) json += ",";
        json += "\"" + std::to_string(i+1) + "\":\"" + pc_names[s.pc[i]] + "\"";
    }
    json += "},";

    // unchecked array (as sets)
    json += "\"unchecked\":{";
    for (int i = 0; i < N; i++) {
        if (i > 0) json += ",";
        json += "\"" + std::to_string(i+1) + "\":[";
        bool first = true;
        for (int j = 0; j < N; j++) {
            if (s.unchecked[i] & (1 << j)) {
                if (!first) json += ",";
                json += std::to_string(j+1);
                first = false;
            }
        }
        json += "]";
    }
    json += "},";

    // max array
    json += "\"max\":{";
    for (int i = 0; i < N; i++) {
        if (i > 0) json += ",";
        json += "\"" + std::to_string(i+1) + "\":" + std::to_string(s.max_val[i]);
    }
    json += "},";

    // nxt array
    json += "\"nxt\":{";
    for (int i = 0; i < N; i++) {
        if (i > 0) json += ",";
        json += "\"" + std::to_string(i+1) + "\":" + std::to_string(s.nxt[i]);
    }
    json += "}";

    json += "}";
    return json;
}

// Lexicographic comparison for bakery: (a1, a2) < (b1, b2)
inline bool prec(int a1, int a2, int b1, int b2) {
    return (a1 < b1) || (a1 == b1 && a2 < b2);
}

// Helper: create bitmask for Procs \ {self}
inline uint8_t procs_minus_self(int self) {
    uint8_t mask = (1 << N) - 1;  // all procs
    mask &= ~(1 << (self));        // remove self (0-indexed)
    return mask;
}

// State transition functions - return all successor states
class BakeryExplorer {
public:
    std::unordered_set<State, StateHash> visited;
    std::vector<State> all_states;
    std::vector<bool> is_initial;

    // Generate initial state
    State init_state() {
        State s;
        for (int i = 0; i < N; i++) {
            s.num[i] = 0;
            s.flag[i] = false;
            s.pc[i] = PC_ncs;
            s.unchecked[i] = 0;
            s.max_val[i] = 0;
            s.nxt[i] = 1;
        }
        return s;
    }

    // ncs(self): pc[self] = "ncs" -> pc' = "e1"
    void ncs(const State& s, int self, std::vector<State>& successors) {
        if (s.pc[self] != PC_ncs) return;
        State next = s;
        next.pc[self] = PC_e1;
        successors.push_back(next);
    }

    // e1(self): Two branches
    void e1(const State& s, int self, std::vector<State>& successors) {
        if (s.pc[self] != PC_e1) return;

        // Branch 1: flag[self] := ~flag[self], goto e1
        {
            State next = s;
            next.flag[self] = !s.flag[self];
            next.pc[self] = PC_e1;
            successors.push_back(next);
        }

        // Branch 2: flag[self] := TRUE, unchecked := Procs \ {self}, max := 0, goto e2
        {
            State next = s;
            next.flag[self] = true;
            next.unchecked[self] = procs_minus_self(self);
            next.max_val[self] = 0;
            next.pc[self] = PC_e2;
            successors.push_back(next);
        }
    }

    // e2(self): Loop through unchecked
    void e2(const State& s, int self, std::vector<State>& successors) {
        if (s.pc[self] != PC_e2) return;

        if (s.unchecked[self] != 0) {
            // For each i in unchecked[self]
            for (int i = 0; i < N; i++) {
                if (s.unchecked[self] & (1 << i)) {
                    State next = s;
                    next.unchecked[self] = s.unchecked[self] & ~(1 << i);
                    if (s.num[i] > s.max_val[self]) {
                        next.max_val[self] = s.num[i];
                    }
                    next.pc[self] = PC_e2;
                    successors.push_back(next);
                }
            }
        } else {
            // unchecked = {} -> goto e3
            State next = s;
            next.pc[self] = PC_e3;
            successors.push_back(next);
        }
    }

    // e3(self): Two branches with Nat values
    void e3(const State& s, int self, std::vector<State>& successors) {
        if (s.pc[self] != PC_e3) return;

        // Branch 1: num[self] := k for any k in Nat, goto e3
        for (int k = 0; k <= MAX_NAT; k++) {
            State next = s;
            next.num[self] = k;
            next.pc[self] = PC_e3;
            successors.push_back(next);
        }

        // Branch 2: num[self] := i for i > max[self], goto e4
        for (int i = s.max_val[self] + 1; i <= MAX_NAT; i++) {
            State next = s;
            next.num[self] = i;
            next.pc[self] = PC_e4;
            successors.push_back(next);
        }
    }

    // e4(self): Two branches
    void e4(const State& s, int self, std::vector<State>& successors) {
        if (s.pc[self] != PC_e4) return;

        // Branch 1: flag[self] := ~flag[self], goto e4
        {
            State next = s;
            next.flag[self] = !s.flag[self];
            next.pc[self] = PC_e4;
            successors.push_back(next);
        }

        // Branch 2: flag[self] := FALSE, unchecked := Procs \ {self}, goto w1
        {
            State next = s;
            next.flag[self] = false;
            next.unchecked[self] = procs_minus_self(self);
            next.pc[self] = PC_w1;
            successors.push_back(next);
        }
    }

    // w1a(self): Pick nxt, check ~flag[nxt], goto w2
    void w1a(const State& s, int self, std::vector<State>& successors) {
        if (s.pc[self] != PC_w1) return;
        if (s.unchecked[self] == 0) return;

        // For each i in unchecked[self], if ~flag[i]
        for (int i = 0; i < N; i++) {
            if (s.unchecked[self] & (1 << i)) {
                // Check await condition: ~flag[nxt']
                if (!s.flag[i]) {
                    State next = s;
                    next.nxt[self] = i + 1;  // nxt is 1-indexed
                    next.pc[self] = PC_w2;
                    successors.push_back(next);
                }
            }
        }
    }

    // w1b(self): unchecked = {} -> goto cs
    void w1b(const State& s, int self, std::vector<State>& successors) {
        if (s.pc[self] != PC_w1) return;
        if (s.unchecked[self] != 0) return;

        State next = s;
        next.pc[self] = PC_cs;
        successors.push_back(next);
    }

    // w2(self): await condition, remove from unchecked, goto w1
    void w2(const State& s, int self, std::vector<State>& successors) {
        if (s.pc[self] != PC_w2) return;

        int nxt_idx = s.nxt[self] - 1;  // 0-indexed

        // await: num[nxt] = 0 \/ <<num[self], self>> \prec <<num[nxt], nxt>>
        bool can_proceed = (s.num[nxt_idx] == 0) ||
                          prec(s.num[self], self + 1, s.num[nxt_idx], s.nxt[self]);

        if (can_proceed) {
            State next = s;
            next.unchecked[self] = s.unchecked[self] & ~(1 << nxt_idx);
            next.pc[self] = PC_w1;
            successors.push_back(next);
        }
    }

    // cs(self): skip, goto exit
    void cs(const State& s, int self, std::vector<State>& successors) {
        if (s.pc[self] != PC_cs) return;

        State next = s;
        next.pc[self] = PC_exit;
        successors.push_back(next);
    }

    // exit(self): Two branches
    void exit_action(const State& s, int self, std::vector<State>& successors) {
        if (s.pc[self] != PC_exit) return;

        // Branch 1: num[self] := k for any k in Nat, goto exit
        for (int k = 0; k <= MAX_NAT; k++) {
            State next = s;
            next.num[self] = k;
            next.pc[self] = PC_exit;
            successors.push_back(next);
        }

        // Branch 2: num[self] := 0, goto ncs
        {
            State next = s;
            next.num[self] = 0;
            next.pc[self] = PC_ncs;
            successors.push_back(next);
        }
    }

    // Get all successors of a state
    void get_successors(const State& s, std::vector<State>& successors) {
        for (int self = 0; self < N; self++) {
            ncs(s, self, successors);
            e1(s, self, successors);
            e2(s, self, successors);
            e3(s, self, successors);
            e4(s, self, successors);
            w1a(s, self, successors);
            w1b(s, self, successors);
            w2(s, self, successors);
            cs(s, self, successors);
            exit_action(s, self, successors);
        }
    }

    // BFS exploration
    void explore(bool dump_json = true, const std::string& output_file = "") {
        State initial = init_state();

        std::queue<State> frontier;
        frontier.push(initial);
        visited.insert(initial);
        all_states.push_back(initial);
        is_initial.push_back(true);

        std::vector<State> successors;

        while (!frontier.empty()) {
            State current = frontier.front();
            frontier.pop();

            successors.clear();
            get_successors(current, successors);

            for (const State& next : successors) {
                if (visited.find(next) == visited.end()) {
                    visited.insert(next);
                    all_states.push_back(next);
                    is_initial.push_back(false);
                    frontier.push(next);
                }
            }
        }

        if (dump_json && !output_file.empty()) {
            write_json(output_file);
        }
    }

    void write_json(const std::string& filename) {
        std::ofstream out(filename);
        out << "{\"states\":[\n";

        for (size_t i = 0; i < all_states.size(); i++) {
            if (i > 0) out << ",\n";
            out << "{\"fp\":" << all_states[i].fingerprint()
                << ",\"val\":" << state_to_json(all_states[i])
                << ",\"initial\":" << (is_initial[i] ? "true" : "false") << "}";
        }

        out << "\n]}\n";
        out.close();
    }

    size_t state_count() const {
        return all_states.size();
    }
};

int main(int argc, char* argv[]) {
    bool dump_json = true;
    std::string output_file = "bakery_states.json";

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--no-dump") {
            dump_json = false;
        } else if (arg == "-o" && i + 1 < argc) {
            output_file = argv[++i];
        } else if (arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [options]\n";
            std::cout << "Options:\n";
            std::cout << "  --no-dump    Disable JSON state dumping\n";
            std::cout << "  -o FILE      Output JSON to FILE (default: bakery_states.json)\n";
            std::cout << "  --help       Show this help\n";
            return 0;
        }
    }

    auto start = std::chrono::high_resolution_clock::now();

    BakeryExplorer explorer;
    explorer.explore(dump_json, output_file);

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    std::cout << "State space exploration complete.\n";
    std::cout << "Distinct states: " << explorer.state_count() << "\n";
    std::cout << "Time: " << duration.count() / 1000.0 << " ms\n";
    std::cout << "Throughput: " << (explorer.state_count() * 1000000.0 / duration.count()) << " states/sec\n";

    if (dump_json) {
        std::cout << "States written to: " << output_file << "\n";
    }

    return 0;
}
