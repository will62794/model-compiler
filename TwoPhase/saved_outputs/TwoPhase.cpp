/**
 * Optimized C++ State Space Generator for TwoPhase TLA+ Specification
 *
 * This generates the full reachable state space equivalent to what TLC would produce.
 * Configuration: RM = {0, 1, 2, 3} (4 resource managers)
 */

#include <cstdint>
#include <vector>
#include <unordered_set>
#include <queue>
#include <string>
#include <fstream>
#include <iostream>
#include <chrono>
#include <functional>
#include <cstring>

// Configuration: Number of resource managers
// Can be overridden at compile time with -DNUM_RM=N
#ifndef NUM_RM
#define NUM_RM 4
#endif


// Enum for RM states
enum class RMState : uint8_t {
    Working = 0,
    Prepared = 1,
    Committed = 2,
    Aborted = 3
};

// Enum for TM states
enum class TMState : uint8_t {
    Init = 0,
    Committed = 1,
    Aborted = 2
};

// Message flags (bitfield)
// Bits 0..NUM_RM-1: Prepared[rm] messages
// Bit NUM_RM: Commit message
// Bit NUM_RM+1: Abort message
using MsgSet = uint16_t;

static constexpr MsgSet MSG_COMMIT = (1 << NUM_RM);
static constexpr MsgSet MSG_ABORT = (1 << (NUM_RM + 1));

inline MsgSet msgPrepared(int rm) { return (1 << rm); }
inline bool hasPrepared(MsgSet msgs, int rm) { return (msgs & (1 << rm)) != 0; }
inline bool hasCommit(MsgSet msgs) { return (msgs & MSG_COMMIT) != 0; }
inline bool hasAbort(MsgSet msgs) { return (msgs & MSG_ABORT) != 0; }

// Compact state representation
// rmState: 2 bits per RM = 16 bits for 8 RMs
// tmState: 2 bits
// tmPrepared: NUM_RM bits (which RMs are in tmPrepared set)
// msgs: NUM_RM+2 bits (10 bits for 8 RMs)
// Total: fits in 64 bits
struct State {
    uint16_t rmStates;     // 2 bits per RM, packed (16 bits for 8 RMs)
    uint8_t tmState;       // TMState enum value
    uint8_t tmPrepared;    // bitfield of prepared RMs (8 bits for 8 RMs)
    uint16_t msgs;         // message set bitfield (10 bits needed)

    inline RMState getRMState(int rm) const {
        return static_cast<RMState>((rmStates >> (rm * 2)) & 0x3);
    }

    inline void setRMState(int rm, RMState s) {
        rmStates = (rmStates & ~(0x3 << (rm * 2))) | (static_cast<uint16_t>(s) << (rm * 2));
    }

    inline TMState getTMState() const {
        return static_cast<TMState>(tmState);
    }

    inline void setTMState(TMState s) {
        tmState = static_cast<uint8_t>(s);
    }

    inline bool isTMPrepared(int rm) const {
        return (tmPrepared & (1 << rm)) != 0;
    }

    inline void addTMPrepared(int rm) {
        tmPrepared |= (1 << rm);
    }

    inline bool allRMsPrepared() const {
        return tmPrepared == ((1 << NUM_RM) - 1);
    }

    bool operator==(const State& other) const {
        return rmStates == other.rmStates &&
               tmState == other.tmState &&
               tmPrepared == other.tmPrepared &&
               msgs == other.msgs;
    }

    // Pack state into a single 64-bit integer for hashing
    uint64_t pack() const {
        return static_cast<uint64_t>(rmStates) |
               (static_cast<uint64_t>(tmState) << 16) |
               (static_cast<uint64_t>(tmPrepared) << 24) |
               (static_cast<uint64_t>(msgs) << 32);
    }

    static State unpack(uint64_t packed) {
        State s;
        s.rmStates = packed & 0xFFFF;
        s.tmState = (packed >> 16) & 0xFF;
        s.tmPrepared = (packed >> 24) & 0xFF;
        s.msgs = (packed >> 32) & 0xFFFF;
        return s;
    }
};

// Custom hash for State
struct StateHash {
    size_t operator()(const State& s) const {
        return std::hash<uint64_t>{}(s.pack());
    }
};

// Fingerprint function (compatible with TLC-style fingerprinting)
uint64_t fingerprint(const State& s) {
    // Use a simple but effective hash combining technique
    uint64_t h = 14695981039346656037ULL; // FNV offset basis
    uint64_t packed = s.pack();
    for (int i = 0; i < 8; i++) {
        h ^= (packed >> (i * 8)) & 0xFF;
        h *= 1099511628211ULL; // FNV prime
    }
    return h;
}

// Initial state
State initialState() {
    State s;
    s.rmStates = 0; // All RMs in Working state
    s.tmState = static_cast<uint8_t>(TMState::Init);
    s.tmPrepared = 0;
    s.msgs = 0;
    return s;
}

// Action: TMRcvPrepared(rm)
bool TMRcvPrepared(const State& s, int rm, State& next) {
    if (s.getTMState() != TMState::Init) return false;
    if (s.isTMPrepared(rm)) return false;
    if (!hasPrepared(s.msgs, rm)) return false;

    next = s;
    next.addTMPrepared(rm);
    return true;
}

// Action: TMCommit
bool TMCommit(const State& s, State& next) {
    if (s.getTMState() != TMState::Init) return false;
    if (!s.allRMsPrepared()) return false;

    next = s;
    next.setTMState(TMState::Committed);
    next.msgs |= MSG_COMMIT;
    return true;
}

// Action: TMAbort
bool TMAbort(const State& s, State& next) {
    if (s.getTMState() != TMState::Init) return false;

    next = s;
    next.setTMState(TMState::Aborted);
    next.msgs |= MSG_ABORT;
    return true;
}

// Action: RMPrepare(rm)
bool RMPrepare(const State& s, int rm, State& next) {
    if (s.getRMState(rm) != RMState::Working) return false;

    next = s;
    next.setRMState(rm, RMState::Prepared);
    next.msgs |= msgPrepared(rm);
    return true;
}

// Action: RMChooseToAbort(rm)
bool RMChooseToAbort(const State& s, int rm, State& next) {
    if (s.getRMState(rm) != RMState::Working) return false;

    next = s;
    next.setRMState(rm, RMState::Aborted);
    return true;
}

// Action: RMRcvCommitMsg(rm)
bool RMRcvCommitMsg(const State& s, int rm, State& next) {
    if (!hasCommit(s.msgs)) return false;
    if (s.getRMState(rm) == RMState::Committed) return false;

    next = s;
    next.setRMState(rm, RMState::Committed);
    return true;
}

// Action: RMRcvAbortMsg(rm)
bool RMRcvAbortMsg(const State& s, int rm, State& next) {
    if (!hasAbort(s.msgs)) return false;
    if (s.getRMState(rm) == RMState::Aborted) return false;

    next = s;
    next.setRMState(rm, RMState::Aborted);
    return true;
}

// Generate all successor states
void getSuccessors(const State& s, std::vector<State>& successors) {
    successors.clear();
    State next;

    // TMCommit
    if (TMCommit(s, next)) successors.push_back(next);

    // TMAbort
    if (TMAbort(s, next)) successors.push_back(next);

    // Per-RM actions
    for (int rm = 0; rm < NUM_RM; rm++) {
        if (TMRcvPrepared(s, rm, next)) successors.push_back(next);
        if (RMPrepare(s, rm, next)) successors.push_back(next);
        if (RMChooseToAbort(s, rm, next)) successors.push_back(next);
        if (RMRcvCommitMsg(s, rm, next)) successors.push_back(next);
        if (RMRcvAbortMsg(s, rm, next)) successors.push_back(next);
    }
}

// Convert RMState to string
const char* rmStateToString(RMState s) {
    switch (s) {
        case RMState::Working: return "working";
        case RMState::Prepared: return "prepared";
        case RMState::Committed: return "committed";
        case RMState::Aborted: return "aborted";
    }
    return "unknown";
}

// Convert TMState to string
const char* tmStateToString(TMState s) {
    switch (s) {
        case TMState::Init: return "init";
        case TMState::Committed: return "committed";
        case TMState::Aborted: return "aborted";
    }
    return "unknown";
}

// JSON output for a state
void stateToJSON(std::ostream& out, const State& s, uint64_t fp, bool isInitial, bool first) {
    if (!first) out << ",\n";
    out << "    {\n";
    out << "      \"fp\": " << fp << ",\n";
    out << "      \"val\": {\n";

    // rmState as record
    out << "        \"rmState\": {";
    for (int rm = 0; rm < NUM_RM; rm++) {
        if (rm > 0) out << ", ";
        out << "\"rm" << (rm + 1) << "\": \"" << rmStateToString(s.getRMState(rm)) << "\"";
    }
    out << "},\n";

    // tmState
    out << "        \"tmState\": \"" << tmStateToString(s.getTMState()) << "\",\n";

    // tmPrepared as set
    out << "        \"tmPrepared\": [";
    bool firstPrepared = true;
    for (int rm = 0; rm < NUM_RM; rm++) {
        if (s.isTMPrepared(rm)) {
            if (!firstPrepared) out << ", ";
            out << "\"rm" << (rm + 1) << "\"";
            firstPrepared = false;
        }
    }
    out << "],\n";

    // msgs as set
    out << "        \"msgs\": [";
    bool firstMsg = true;
    for (int rm = 0; rm < NUM_RM; rm++) {
        if (hasPrepared(s.msgs, rm)) {
            if (!firstMsg) out << ", ";
            out << "{\"type\": \"Prepared\", \"rm\": \"rm" << (rm + 1) << "\"}";
            firstMsg = false;
        }
    }
    if (hasCommit(s.msgs)) {
        if (!firstMsg) out << ", ";
        out << "{\"type\": \"Commit\"}";
        firstMsg = false;
    }
    if (hasAbort(s.msgs)) {
        if (!firstMsg) out << ", ";
        out << "{\"type\": \"Abort\"}";
    }
    out << "]\n";

    out << "      },\n";
    out << "      \"initial\": " << (isInitial ? "true" : "false") << "\n";
    out << "    }";
}

// BFS state space exploration
struct ExplorationResult {
    size_t numStates;
    size_t numTransitions;
    double durationSecs;
};

ExplorationResult explore(int maxDepth = -1, bool dumpJSON = false, const std::string& jsonFile = "") {
    std::unordered_set<uint64_t> visited;
    std::queue<std::pair<State, int>> frontier; // state, depth
    std::vector<std::pair<State, uint64_t>> allStates; // state, fingerprint
    std::unordered_set<uint64_t> initialStates;

    auto startTime = std::chrono::high_resolution_clock::now();

    State init = initialState();
    uint64_t initPacked = init.pack();
    visited.insert(initPacked);
    frontier.push({init, 0});
    initialStates.insert(initPacked);

    if (dumpJSON) {
        allStates.push_back({init, fingerprint(init)});
    }

    size_t numTransitions = 0;
    std::vector<State> successors;

    while (!frontier.empty()) {
        auto [current, depth] = frontier.front();
        frontier.pop();

        if (maxDepth >= 0 && depth >= maxDepth) {
            continue;
        }

        getSuccessors(current, successors);
        numTransitions += successors.size();

        for (const State& next : successors) {
            uint64_t packed = next.pack();
            if (visited.find(packed) == visited.end()) {
                visited.insert(packed);
                frontier.push({next, depth + 1});
                if (dumpJSON) {
                    allStates.push_back({next, fingerprint(next)});
                }
            }
        }
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    double duration = std::chrono::duration<double>(endTime - startTime).count();

    // Write JSON if requested
    if (dumpJSON && !jsonFile.empty()) {
        std::ofstream out(jsonFile);
        out << "{\n  \"states\": [\n";
        bool first = true;
        for (const auto& [s, fp] : allStates) {
            bool isInit = initialStates.find(s.pack()) != initialStates.end();
            stateToJSON(out, s, fp, isInit, first);
            first = false;
        }
        out << "\n  ]\n}\n";
        out.close();
    }

    return {visited.size(), numTransitions, duration};
}

void printUsage(const char* progName) {
    std::cerr << "Usage: " << progName << " [options]\n";
    std::cerr << "Options:\n";
    std::cerr << "  -d, --depth N      Limit exploration depth to N (default: unlimited)\n";
    std::cerr << "  -o, --output FILE  Output JSON file (default: no output)\n";
    std::cerr << "  -n, --no-json      Disable JSON output (for benchmarking)\n";
    std::cerr << "  -h, --help         Show this help message\n";
}

int main(int argc, char* argv[]) {
    int maxDepth = -1;
    std::string outputFile;
    bool dumpJSON = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-d" || arg == "--depth") {
            if (i + 1 < argc) {
                maxDepth = std::stoi(argv[++i]);
            } else {
                std::cerr << "Error: --depth requires a value\n";
                return 1;
            }
        } else if (arg == "-o" || arg == "--output") {
            if (i + 1 < argc) {
                outputFile = argv[++i];
                dumpJSON = true;
            } else {
                std::cerr << "Error: --output requires a filename\n";
                return 1;
            }
        } else if (arg == "-n" || arg == "--no-json") {
            dumpJSON = false;
            outputFile = "";
        } else if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    std::cout << "TwoPhase State Space Generator (C++)\n";
    std::cout << "Configuration: NUM_RM = " << NUM_RM << "\n";
    if (maxDepth >= 0) {
        std::cout << "Depth limit: " << maxDepth << "\n";
    } else {
        std::cout << "Depth limit: unlimited\n";
    }
    std::cout << "JSON output: " << (dumpJSON ? outputFile : "disabled") << "\n";
    std::cout << "\n";

    auto result = explore(maxDepth, dumpJSON, outputFile);

    std::cout << "Exploration complete.\n";
    std::cout << "States found: " << result.numStates << "\n";
    std::cout << "Transitions: " << result.numTransitions << "\n";
    std::cout << "Duration: " << result.durationSecs << " seconds\n";
    std::cout << "Throughput: " << static_cast<size_t>(result.numStates / result.durationSecs) << " states/second\n";

    if (dumpJSON && !outputFile.empty()) {
        std::cout << "States written to: " << outputFile << "\n";
    }

    return 0;
}
