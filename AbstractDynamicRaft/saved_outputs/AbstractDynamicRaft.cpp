// AbstractDynamicRaft - Optimized C++ State Space Explorer
// Generated from AbstractDynamicRaft.tla
// Configuration: Server={n1,n2,n3}, MaxLogLen=2, MaxTerm=2, MaxConfigVersion=2, InitTerm=0

#include <iostream>
#include <fstream>
#include <vector>
#include <set>
#include <unordered_set>
#include <unordered_map>
#include <array>
#include <algorithm>
#include <cstdint>
#include <string>
#include <sstream>
#include <chrono>
#include <functional>
#include <bitset>

// Configuration constants
constexpr int NUM_SERVERS = 3;
constexpr int MAX_LOG_LEN = 2;
constexpr int MAX_TERM = 2;
constexpr int MAX_CONFIG_VERSION = 2;
constexpr int INIT_TERM = 0;

// Server indices
constexpr int N1 = 0;
constexpr int N2 = 1;
constexpr int N3 = 2;

// State constants
enum NodeState : uint8_t { Secondary = 0, Primary = 1 };

// Compact state representation
// Each server has: currentTerm (3 bits for 0-2), state (1 bit), log (variable),
// configVersion (2 bits for 1-2), configTerm (3 bits), config (3 bits bitmask)
// Log: up to 2 entries, each entry is a term (3 bits each)
// immediatelyCommitted: set of (index, term) pairs

struct ServerState {
    uint8_t currentTerm;     // 0-2
    uint8_t state;           // 0=Secondary, 1=Primary
    uint8_t logLen;          // 0-2
    uint8_t logTerms[MAX_LOG_LEN]; // term of each log entry
    uint8_t configVersion;   // 1-2
    uint8_t configTerm;      // 0-2
    uint8_t config;          // bitmask of servers in config
};

// Committed entry (index, term)
struct CommittedEntry {
    uint8_t index;
    uint8_t term;

    bool operator==(const CommittedEntry& o) const {
        return index == o.index && term == o.term;
    }
    bool operator<(const CommittedEntry& o) const {
        return index < o.index || (index == o.index && term < o.term);
    }
};

struct State {
    ServerState servers[NUM_SERVERS];
    std::set<CommittedEntry> immediatelyCommitted;

    bool operator==(const State& o) const {
        for (int i = 0; i < NUM_SERVERS; i++) {
            if (servers[i].currentTerm != o.servers[i].currentTerm) return false;
            if (servers[i].state != o.servers[i].state) return false;
            if (servers[i].logLen != o.servers[i].logLen) return false;
            for (int j = 0; j < servers[i].logLen; j++) {
                if (servers[i].logTerms[j] != o.servers[i].logTerms[j]) return false;
            }
            if (servers[i].configVersion != o.servers[i].configVersion) return false;
            if (servers[i].configTerm != o.servers[i].configTerm) return false;
            if (servers[i].config != o.servers[i].config) return false;
        }
        return immediatelyCommitted == o.immediatelyCommitted;
    }
};

// Hash function for State
struct StateHash {
    size_t operator()(const State& s) const {
        uint64_t h = 0;
        for (int i = 0; i < NUM_SERVERS; i++) {
            h = h * 31 + s.servers[i].currentTerm;
            h = h * 31 + s.servers[i].state;
            h = h * 31 + s.servers[i].logLen;
            for (int j = 0; j < s.servers[i].logLen; j++) {
                h = h * 31 + s.servers[i].logTerms[j];
            }
            h = h * 31 + s.servers[i].configVersion;
            h = h * 31 + s.servers[i].configTerm;
            h = h * 31 + s.servers[i].config;
        }
        for (const auto& c : s.immediatelyCommitted) {
            h = h * 31 + c.index;
            h = h * 31 + c.term;
        }
        return h;
    }
};

// Compute fingerprint for state
uint64_t computeFingerprint(const State& s) {
    StateHash hasher;
    return hasher(s);
}

// Helper: get cardinality of config bitmask
inline int configCardinality(uint8_t config) {
    return __builtin_popcount(config);
}

// Helper: check if server is in config
inline bool inConfig(uint8_t config, int server) {
    return (config >> server) & 1;
}

// Helper: get all non-empty subsets of Server set
std::vector<uint8_t> getAllConfigs() {
    std::vector<uint8_t> configs;
    for (uint8_t c = 1; c < (1 << NUM_SERVERS); c++) {
        configs.push_back(c);
    }
    return configs;
}

// Helper: get all quorums of a config
std::vector<uint8_t> getQuorums(uint8_t config) {
    std::vector<uint8_t> quorums;
    int card = configCardinality(config);
    // A quorum is a subset Q where |Q| * 2 > |config|
    for (uint8_t q = 1; q < (1 << NUM_SERVERS); q++) {
        // q must be a subset of config
        if ((q & config) != q) continue;
        if (configCardinality(q) * 2 > card) {
            quorums.push_back(q);
        }
    }
    return quorums;
}

// Helper: check if all quorums of x and y overlap
bool quorumsOverlap(uint8_t x, uint8_t y) {
    auto qx = getQuorums(x);
    auto qy = getQuorums(y);
    for (auto q1 : qx) {
        for (auto q2 : qy) {
            if ((q1 & q2) == 0) return false;
        }
    }
    return true;
}

// Helper: last term of log
inline int lastTerm(const ServerState& s) {
    if (s.logLen == 0) return 0;
    return s.logTerms[s.logLen - 1];
}

// Helper: get term at index (1-indexed, 0 if index=0)
inline int getTerm(const ServerState& s, int index) {
    if (index == 0) return 0;
    return s.logTerms[index - 1];
}

// Helper: InLog check
inline bool inLog(const ServerState& s, int index, int term) {
    if (index < 1 || index > s.logLen) return false;
    return s.logTerms[index - 1] == term;
}

// Helper: can rollback i against j
bool canRollback(const State& st, int i, int j) {
    const auto& si = st.servers[i];
    const auto& sj = st.servers[j];
    if (si.logLen == 0) return false;
    if (lastTerm(si) >= lastTerm(sj)) return false;
    if (si.logLen > sj.logLen) return true;
    // si.logLen <= sj.logLen
    return lastTerm(si) != getTerm(sj, si.logLen);
}

// Helper: can vote for oplog
bool canVoteForOplog(const State& st, int i, int j, int term) {
    const auto& si = st.servers[i];
    const auto& sj = st.servers[j];

    // logOk: j's log is at least as up-to-date as i's
    bool logOk = (lastTerm(sj) > lastTerm(si)) ||
                 (lastTerm(sj) == lastTerm(si) && sj.logLen >= si.logLen);

    return si.currentTerm < term && logOk;
}

// Helper: is newer config (i's config is newer than j's)
bool isNewerConfig(const State& st, int i, int j) {
    const auto& si = st.servers[i];
    const auto& sj = st.servers[j];
    return (si.configTerm > sj.configTerm) ||
           (si.configTerm == sj.configTerm && si.configVersion > sj.configVersion);
}

// Helper: is newer or equal config
bool isNewerOrEqualConfig(const State& st, int i, int j) {
    const auto& si = st.servers[i];
    const auto& sj = st.servers[j];
    return (si.configTerm == sj.configTerm && si.configVersion == sj.configVersion) ||
           isNewerConfig(st, i, j);
}

// Helper: can vote for config
bool canVoteForConfig(const State& st, int i, int j, int term) {
    return st.servers[i].currentTerm < term && isNewerOrEqualConfig(st, j, i);
}

// Helper: ImmediatelyCommitted check
bool immediatelyCommittedCheck(const State& st, int eind, int eterm, uint8_t Q) {
    for (int s = 0; s < NUM_SERVERS; s++) {
        if (!inConfig(Q, s)) continue;
        const auto& ss = st.servers[s];
        if (ss.logLen < eind) return false;
        if (!inLog(ss, eind, eterm)) return false;
        if (ss.currentTerm != eterm) return false;
    }
    return true;
}

// Helper: ConfigQuorumCheck
bool configQuorumCheck(const State& st, int i) {
    const auto& si = st.servers[i];
    auto quorums = getQuorums(si.config);
    for (auto Q : quorums) {
        bool ok = true;
        for (int t = 0; t < NUM_SERVERS && ok; t++) {
            if (!inConfig(Q, t)) continue;
            const auto& st_t = st.servers[t];
            if (st_t.configVersion != si.configVersion || st_t.configTerm != si.configTerm) {
                ok = false;
            }
        }
        if (ok) return true;
    }
    return false;
}

// Helper: TermQuorumCheck
bool termQuorumCheck(const State& st, int i) {
    const auto& si = st.servers[i];
    auto quorums = getQuorums(si.config);
    for (auto Q : quorums) {
        bool ok = true;
        for (int t = 0; t < NUM_SERVERS && ok; t++) {
            if (!inConfig(Q, t)) continue;
            if (st.servers[t].currentTerm != si.currentTerm) {
                ok = false;
            }
        }
        if (ok) return true;
    }
    return false;
}

// Helper: IsCommitted
bool isCommitted(const State& st, int index, int primary) {
    const auto& sp = st.servers[primary];
    if (sp.logLen < index) return false;
    if (getTerm(sp, index) != sp.currentTerm) return false;

    auto quorums = getQuorums(sp.config);
    for (auto Q : quorums) {
        bool allOk = true;
        for (int s = 0; s < NUM_SERVERS && allOk; s++) {
            if (!inConfig(Q, s)) continue;
            const auto& ss = st.servers[s];
            if (index > ss.logLen) { allOk = false; continue; }
            if (ss.logTerms[index-1] != sp.logTerms[index-1]) { allOk = false; continue; }
            if (ss.currentTerm != sp.currentTerm) { allOk = false; continue; }
        }
        if (allOk) return true;
    }
    return false;
}

// Helper: OplogCommitment
bool oplogCommitment(const State& st, int s) {
    const auto& ss = st.servers[s];

    // Check: immediatelyCommitted is empty OR there exists a commit in current term
    bool hasCommitInTerm = false;
    for (const auto& c : st.immediatelyCommitted) {
        if (c.term == ss.currentTerm) {
            hasCommitInTerm = true;
            break;
        }
    }
    if (!st.immediatelyCommitted.empty() && !hasCommitInTerm) return false;

    // All entries committed in primary's term are committed in its current config
    for (const auto& c : st.immediatelyCommitted) {
        if (c.term == ss.currentTerm) {
            if (!isCommitted(st, c.index, s)) return false;
        }
    }
    return true;
}

// State space explorer
class StateSpaceExplorer {
public:
    std::unordered_set<State, StateHash> visited;
    std::vector<State> frontier;
    std::vector<State> initialStates;
    bool enableJson;
    std::vector<std::pair<uint64_t, State>> allStates; // (fingerprint, state) for JSON

    StateSpaceExplorer(bool json = true) : enableJson(json) {}

    void generateInitialStates() {
        auto allConfigs = getAllConfigs();

        for (auto initConfig : allConfigs) {
            State s;
            for (int i = 0; i < NUM_SERVERS; i++) {
                s.servers[i].currentTerm = INIT_TERM;
                s.servers[i].state = Secondary;
                s.servers[i].logLen = 0;
                s.servers[i].configVersion = 1;
                s.servers[i].configTerm = INIT_TERM;
                s.servers[i].config = initConfig;
            }
            s.immediatelyCommitted.clear();

            if (visited.find(s) == visited.end()) {
                visited.insert(s);
                frontier.push_back(s);
                initialStates.push_back(s);
                if (enableJson) {
                    allStates.push_back({computeFingerprint(s), s});
                }
            }
        }
    }

    // Check state constraint
    bool stateConstraint(const State& s) {
        for (int i = 0; i < NUM_SERVERS; i++) {
            if (s.servers[i].currentTerm > MAX_TERM) return false;
            if (s.servers[i].logLen > MAX_LOG_LEN) return false;
            if (s.servers[i].configVersion > MAX_CONFIG_VERSION) return false;
        }
        return true;
    }

    // Action: ClientRequest
    void clientRequest(const State& s, int i, std::vector<State>& successors) {
        if (s.servers[i].state != Primary) return;
        if (s.servers[i].logLen >= MAX_LOG_LEN) return; // constraint

        State next = s;
        next.servers[i].logTerms[next.servers[i].logLen] = next.servers[i].currentTerm;
        next.servers[i].logLen++;

        if (stateConstraint(next)) {
            successors.push_back(next);
        }
    }

    // Action: GetEntries
    void getEntries(const State& s, int i, int j, std::vector<State>& successors) {
        if (s.servers[i].state != Secondary) return;
        if (s.servers[j].logLen <= s.servers[i].logLen) return;

        const auto& si = s.servers[i];
        const auto& sj = s.servers[j];

        // Log consistency check
        bool logOk = (si.logLen == 0) ||
                     (sj.logTerms[si.logLen - 1] == si.logTerms[si.logLen - 1]);
        if (!logOk) return;

        int newEntryIndex = (si.logLen == 0) ? 0 : si.logLen;
        if (newEntryIndex >= MAX_LOG_LEN) return; // constraint

        State next = s;
        next.servers[i].logTerms[newEntryIndex] = sj.logTerms[newEntryIndex];
        next.servers[i].logLen = newEntryIndex + 1;

        if (stateConstraint(next)) {
            successors.push_back(next);
        }
    }

    // Action: RollbackEntries
    void rollbackEntries(const State& s, int i, int j, std::vector<State>& successors) {
        if (s.servers[i].state != Secondary) return;
        if (!canRollback(s, i, j)) return;

        State next = s;
        next.servers[i].logLen--;

        if (stateConstraint(next)) {
            successors.push_back(next);
        }
    }

    // Action: BecomeLeader
    void becomeLeader(const State& s, int i, uint8_t voteQuorum, std::vector<State>& successors) {
        if (!inConfig(s.servers[i].config, i)) return;
        if (!inConfig(voteQuorum, i)) return;

        int newTerm = s.servers[i].currentTerm + 1;
        if (newTerm > MAX_TERM) return; // constraint

        // Check voteQuorum is a quorum of i's config
        auto quorums = getQuorums(s.servers[i].config);
        bool isQuorum = false;
        for (auto q : quorums) {
            if (q == voteQuorum) { isQuorum = true; break; }
        }
        if (!isQuorum) return;

        // All voters must be able to vote
        for (int v = 0; v < NUM_SERVERS; v++) {
            if (!inConfig(voteQuorum, v)) continue;
            if (!canVoteForConfig(s, v, i, newTerm)) return;
            if (!canVoteForOplog(s, v, i, newTerm)) return;
        }

        State next = s;
        for (int v = 0; v < NUM_SERVERS; v++) {
            if (inConfig(voteQuorum, v)) {
                next.servers[v].currentTerm = newTerm;
                next.servers[v].state = (v == i) ? Primary : Secondary;
            }
        }
        next.servers[i].configTerm = newTerm;

        if (stateConstraint(next)) {
            successors.push_back(next);
        }
    }

    // Action: CommitEntry
    void commitEntry(const State& s, int i, uint8_t commitQuorum, std::vector<State>& successors) {
        const auto& si = s.servers[i];
        if (si.logLen == 0) return;
        if (si.state != Primary) return;

        int ind = si.logLen;
        if (si.logTerms[ind - 1] != si.currentTerm) return;

        // Check commitQuorum is quorum of config
        auto quorums = getQuorums(si.config);
        bool isQuorum = false;
        for (auto q : quorums) {
            if (q == commitQuorum) { isQuorum = true; break; }
        }
        if (!isQuorum) return;

        if (!immediatelyCommittedCheck(s, ind, si.currentTerm, commitQuorum)) return;

        // Check not already committed
        CommittedEntry ce{(uint8_t)ind, si.currentTerm};
        if (s.immediatelyCommitted.count(ce) > 0) return;

        State next = s;
        next.immediatelyCommitted.insert(ce);

        if (stateConstraint(next)) {
            successors.push_back(next);
        }
    }

    // Action: UpdateTerms
    void updateTerms(const State& s, int i, int j, std::vector<State>& successors) {
        if (s.servers[i].currentTerm <= s.servers[j].currentTerm) return;

        State next = s;
        next.servers[j].currentTerm = s.servers[i].currentTerm;
        next.servers[j].state = Secondary;

        if (stateConstraint(next)) {
            successors.push_back(next);
        }
    }

    // Action: Reconfig
    void reconfig(const State& s, int i, uint8_t newConfig, std::vector<State>& successors) {
        const auto& si = s.servers[i];
        if (si.state != Primary) return;
        if (!configQuorumCheck(s, i)) return;
        if (!termQuorumCheck(s, i)) return;
        if (!quorumsOverlap(si.config, newConfig)) return;
        if (!oplogCommitment(s, i)) return;
        if (!inConfig(newConfig, i)) return;

        int newVersion = si.configVersion + 1;
        if (newVersion > MAX_CONFIG_VERSION) return; // constraint

        State next = s;
        next.servers[i].configTerm = si.currentTerm;
        next.servers[i].configVersion = newVersion;
        next.servers[i].config = newConfig;

        if (stateConstraint(next)) {
            successors.push_back(next);
        }
    }

    // Action: SendConfig
    void sendConfig(const State& s, int i, int j, std::vector<State>& successors) {
        if (s.servers[j].state != Secondary) return;
        if (!isNewerConfig(s, i, j)) return;

        State next = s;
        next.servers[j].configVersion = s.servers[i].configVersion;
        next.servers[j].configTerm = s.servers[i].configTerm;
        next.servers[j].config = s.servers[i].config;

        if (stateConstraint(next)) {
            successors.push_back(next);
        }
    }

    void getSuccessors(const State& s, std::vector<State>& successors) {
        auto allConfigs = getAllConfigs();

        for (int i = 0; i < NUM_SERVERS; i++) {
            clientRequest(s, i, successors);

            for (int j = 0; j < NUM_SERVERS; j++) {
                getEntries(s, i, j, successors);
                rollbackEntries(s, i, j, successors);
                updateTerms(s, i, j, successors);
                sendConfig(s, i, j, successors);
            }

            auto quorums = getQuorums(s.servers[i].config);
            for (auto q : quorums) {
                becomeLeader(s, i, q, successors);
                commitEntry(s, i, q, successors);
            }

            for (auto newConfig : allConfigs) {
                reconfig(s, i, newConfig, successors);
            }
        }
    }

    void explore() {
        generateInitialStates();

        while (!frontier.empty()) {
            State current = frontier.back();
            frontier.pop_back();

            std::vector<State> successors;
            getSuccessors(current, successors);

            for (const auto& next : successors) {
                if (visited.find(next) == visited.end()) {
                    visited.insert(next);
                    frontier.push_back(next);
                    if (enableJson) {
                        allStates.push_back({computeFingerprint(next), next});
                    }
                }
            }
        }
    }

    std::string serverName(int i) {
        switch (i) {
            case 0: return "n1";
            case 1: return "n2";
            case 2: return "n3";
            default: return "unknown";
        }
    }

    std::string configToJson(uint8_t config) {
        std::ostringstream ss;
        ss << "[";
        bool first = true;
        for (int i = 0; i < NUM_SERVERS; i++) {
            if (inConfig(config, i)) {
                if (!first) ss << ", ";
                ss << "\"" << serverName(i) << "\"";
                first = false;
            }
        }
        ss << "]";
        return ss.str();
    }

    std::string logToJson(const ServerState& s) {
        std::ostringstream ss;
        ss << "[";
        for (int i = 0; i < s.logLen; i++) {
            if (i > 0) ss << ", ";
            ss << (int)s.logTerms[i];
        }
        ss << "]";
        return ss.str();
    }

    std::string committedToJson(const std::set<CommittedEntry>& committed) {
        std::ostringstream ss;
        ss << "[";
        bool first = true;
        for (const auto& c : committed) {
            if (!first) ss << ", ";
            ss << "[" << (int)c.index << ", " << (int)c.term << "]";
            first = false;
        }
        ss << "]";
        return ss.str();
    }

    std::string stateToJson(const State& s) {
        std::ostringstream ss;
        ss << "{";

        // currentTerm
        ss << "\"currentTerm\": {";
        for (int i = 0; i < NUM_SERVERS; i++) {
            if (i > 0) ss << ", ";
            ss << "\"" << serverName(i) << "\": " << (int)s.servers[i].currentTerm;
        }
        ss << "}, ";

        // state
        ss << "\"state\": {";
        for (int i = 0; i < NUM_SERVERS; i++) {
            if (i > 0) ss << ", ";
            ss << "\"" << serverName(i) << "\": \""
               << (s.servers[i].state == Primary ? "Primary" : "Secondary") << "\"";
        }
        ss << "}, ";

        // log
        ss << "\"log\": {";
        for (int i = 0; i < NUM_SERVERS; i++) {
            if (i > 0) ss << ", ";
            ss << "\"" << serverName(i) << "\": " << logToJson(s.servers[i]);
        }
        ss << "}, ";

        // immediatelyCommitted
        ss << "\"immediatelyCommitted\": " << committedToJson(s.immediatelyCommitted) << ", ";

        // configVersion
        ss << "\"configVersion\": {";
        for (int i = 0; i < NUM_SERVERS; i++) {
            if (i > 0) ss << ", ";
            ss << "\"" << serverName(i) << "\": " << (int)s.servers[i].configVersion;
        }
        ss << "}, ";

        // configTerm
        ss << "\"configTerm\": {";
        for (int i = 0; i < NUM_SERVERS; i++) {
            if (i > 0) ss << ", ";
            ss << "\"" << serverName(i) << "\": " << (int)s.servers[i].configTerm;
        }
        ss << "}, ";

        // config
        ss << "\"config\": {";
        for (int i = 0; i < NUM_SERVERS; i++) {
            if (i > 0) ss << ", ";
            ss << "\"" << serverName(i) << "\": " << configToJson(s.servers[i].config);
        }
        ss << "}";

        ss << "}";
        return ss.str();
    }

    void dumpJson(const std::string& filename) {
        std::ofstream out(filename);
        out << "{\"states\": [\n";

        for (size_t i = 0; i < allStates.size(); i++) {
            const auto& [fp, state] = allStates[i];
            bool isInitial = std::find(initialStates.begin(), initialStates.end(), state) != initialStates.end();

            out << "  {\"fp\": " << fp
                << ", \"val\": " << stateToJson(state)
                << ", \"initial\": " << (isInitial ? "true" : "false") << "}";
            if (i < allStates.size() - 1) out << ",";
            out << "\n";
        }

        out << "]}\n";
        out.close();
    }

    size_t getStateCount() const {
        return visited.size();
    }
};

int main(int argc, char* argv[]) {
    bool dumpJson = true;
    std::string outputFile = "states_cpp.json";

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--no-json") {
            dumpJson = false;
        } else if (arg == "-o" && i + 1 < argc) {
            outputFile = argv[++i];
        }
    }

    auto startTime = std::chrono::high_resolution_clock::now();

    StateSpaceExplorer explorer(dumpJson);
    explorer.explore();

    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

    std::cout << "State space exploration complete." << std::endl;
    std::cout << "Total distinct states: " << explorer.getStateCount() << std::endl;
    std::cout << "Time: " << duration.count() << " ms" << std::endl;

    if (dumpJson) {
        explorer.dumpJson(outputFile);
        std::cout << "States dumped to: " << outputFile << std::endl;
    }

    return 0;
}
