// C++17, standard library only. See README.md for paper-to-code mapping.
// Build: g++ -O3 -DNDEBUG -std=c++17 config_graph.cpp -o config_graph_cpp
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#ifdef __unix__
#include <sys/resource.h>
#endif

using Clock = std::chrono::steady_clock;
using U64 = std::uint64_t;
using Mask = std::vector<U64>;
static double seconds(Clock::time_point t) {
    return std::chrono::duration<double>(Clock::now() - t).count();
}
struct Options { bool witness = true, trace = false; double timeout = 0; U64 max_frontier = 0; };
struct Result {
    std::string status, reason;
    std::vector<int> witness;
    bool has_witness = false, verified = false;
    U64 configurations = 1, attempted_edges = 0, valid_edges = 0, merged_edges = 0;
    std::size_t peak_frontier = 1, completed_layers = 0;
    std::vector<std::size_t> widths{1};
    double graph_seconds = 0, prepare_seconds = 0, total_seconds = 0;
};
struct StateHash {
    std::size_t operator()(U64 x) const { return std::hash<U64>{}(x); }
    std::size_t operator()(const Mask& value) const {
        std::size_t h = value.size();
        for (U64 word : value)
            h ^= std::hash<U64>{}(word) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

// step(i, memory, bit, child) returns false for a rejected branch.
// Deterministic work between certificate reads is contracted into one edge.
template<class State, class Step, class Accept>
Result explore(std::size_t length, State start, Step step, Accept accept, const Options& options) {
    const auto begin = Clock::now();
    Result result;
    std::vector<State> current{std::move(start)};
    std::vector<std::vector<U64>> parents;
    auto finish = [&](const std::string& status, const std::string& reason = "") -> Result {
        result.status = status;
        result.reason = reason;
        result.graph_seconds = seconds(begin);
        return std::move(result);
    };
    for (std::size_t i = 0; i < length; ++i) {
        if (options.timeout > 0 && seconds(begin) >= options.timeout)
            return finish("UNKNOWN", "timeout");
        std::unordered_map<State, std::size_t, StateHash> index;
        std::vector<State> following;
        std::vector<U64> links;
        for (std::size_t p = 0; p < current.size(); ++p) {
            if (options.timeout > 0 && p % 1024 == 0 && seconds(begin) >= options.timeout)
                return finish("UNKNOWN", "timeout");
            for (int bit = 0; bit <= 1; ++bit) {
                ++result.attempted_edges;
                State child;
                if (!step(i, current[p], bit, child)) continue;
                ++result.valid_edges;
                if (index.find(child) != index.end()) { ++result.merged_edges; continue; }
                if (options.max_frontier && following.size() >= options.max_frontier)
                    return finish("UNKNOWN", "max_frontier");
                index.emplace(child, following.size());
                following.push_back(std::move(child));
                ++result.configurations;
                if (options.witness) links.push_back((static_cast<U64>(p) << 1) | bit);
            }
        }
        current = std::move(following);
        if (options.witness) parents.push_back(std::move(links));
        result.widths.push_back(current.size());
        result.peak_frontier = std::max(result.peak_frontier, current.size());
        result.completed_layers = i + 1;
        if (current.empty()) return finish("REJECT");
    }
    for (std::size_t k = 0; k < current.size(); ++k) {
        if (!accept(current[k])) continue;
        if (options.witness) {
            result.has_witness = true;
            result.witness.resize(length);
            std::size_t p = k;
            for (std::size_t i = length; i > 0; --i) {
                const U64 link = parents[i - 1][p];
                result.witness[i - 1] = link & 1;
                p = link >> 1;
            }
        }
        return finish("ACCEPT");
    }
    return finish("REJECT");
}

// Enumeration of allowed lengths, corresponding to Section 3 of the paper.
// build_and_run(length) constructs the verifier for that length and calls explore.
template<class BuildAndRun>
Result explore_lengths(const std::vector<std::size_t>& lengths, BuildAndRun build_and_run) {
    bool unknown = false;
    for (auto length : lengths) {
        Result result = build_and_run(length);
        if (result.status == "ACCEPT") return result;
        unknown = unknown || result.status == "UNKNOWN";
    }
    Result result;
    result.status = unknown ? "UNKNOWN" : "REJECT";
    return result;
}

static U64 unsigned_number(const std::string& value) {
    if (value.empty() || value[0] == '-') throw std::runtime_error("expected a nonnegative integer");
    std::size_t used = 0;
    U64 parsed = std::stoull(value, &used);
    if (used != value.size()) throw std::runtime_error("invalid integer: " + value);
    return parsed;
}
struct CNF { std::size_t n; std::vector<std::vector<long long>> clauses; };
static CNF read_dimacs(const std::string& path) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("cannot read CNF: " + path);
    CNF formula{};
    bool header = false;
    std::size_t declared = 0;
    std::vector<long long> pending;
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream stream(line);
        std::string token;
        if (!(stream >> token) || token[0] == 'c') continue;
        if (token == "p") {
            std::string kind, n, m, extra;
            if (header || !(stream >> kind >> n >> m) || kind != "cnf" || stream >> extra)
                throw std::runtime_error("invalid DIMACS header");
            formula.n = unsigned_number(n); declared = unsigned_number(m); header = true;
            continue;
        }
        if (!header) throw std::runtime_error("missing p cnf header");
        do {
            std::size_t used;
            const long long literal = std::stoll(token, &used);
            if (used != token.size() || literal == std::numeric_limits<long long>::min())
                throw std::runtime_error("invalid literal");
            const U64 variable = static_cast<U64>(literal < 0 ? -literal : literal);
            if (!literal) { formula.clauses.push_back(std::move(pending)); pending.clear(); }
            else if (variable > formula.n) throw std::runtime_error("literal exceeds variable count");
            else pending.push_back(literal);
        } while (stream >> token);
    }
    if (!header || !pending.empty() || formula.clauses.size() != declared)
        throw std::runtime_error("missing header, unterminated clause, or clause-count mismatch");
    return formula;
}
static std::size_t var(long long literal) { return static_cast<std::size_t>(literal < 0 ? -literal : literal) - 1; }
struct Compiled {
    std::vector<Mask> positive, negative, close;
    std::size_t slots = 0, normalized = 0;
    bool empty = false;
};
static Compiled prepare_cnf(const CNF& formula) {
    Compiled p;
    std::vector<std::vector<long long>> clauses;
    for (auto clause : formula.clauses) {
        if (clause.empty()) { p.empty = true; continue; }
        std::sort(clause.begin(), clause.end(), [](long long a, long long b) {
            return var(a) != var(b) ? var(a) < var(b) : a < b;
        });
        clause.erase(std::unique(clause.begin(), clause.end()), clause.end());
        bool tautology = false;
        for (std::size_t j = 1; j < clause.size(); ++j)
            if (clause[j] == -clause[j-1]) tautology = true;
        if (!tautology) clauses.push_back(std::move(clause));
    }
    p.normalized = clauses.size();
    std::vector<std::vector<std::size_t>> starts(formula.n), ends(formula.n);
    std::vector<std::size_t> assigned(clauses.size());
    for (std::size_t j = 0; j < clauses.size(); ++j) {
        starts[var(clauses[j].front())].push_back(j);
        ends[var(clauses[j].back())].push_back(j);
    }
    std::priority_queue<std::size_t, std::vector<std::size_t>, std::greater<std::size_t>> free;
    for (std::size_t i = 0; i < formula.n; ++i) {
        for (auto j : starts[i]) {
            if (free.empty()) assigned[j] = p.slots++;
            else { assigned[j] = free.top(); free.pop(); }
        }
        for (auto j : ends[i]) free.push(assigned[j]);
    }
    const std::size_t words = (p.slots + 63) / 64;
    p.positive.assign(formula.n, Mask(words));
    p.negative.assign(formula.n, Mask(words));
    p.close.assign(formula.n, Mask(words));
    for (std::size_t j = 0; j < clauses.size(); ++j) {
        const std::size_t word = assigned[j] / 64;
        const U64 bit = U64(1) << (assigned[j] % 64);
        for (auto literal : clauses[j])
            (literal > 0 ? p.positive : p.negative)[var(literal)][word] |= bit;
        p.close[var(clauses[j].back())][word] |= bit;
    }
    return p;
}
static bool verify(const CNF& formula, const std::vector<int>& bits) {
    if (bits.size() != formula.n) return false;
    for (const auto& clause : formula.clauses) {
        bool satisfied = false;
        for (auto literal : clause)
            if (static_cast<bool>(bits[var(literal)]) == (literal > 0)) { satisfied = true; break; }
        if (!satisfied) return false;
    }
    return true;
}
static Result solve_cnf(const CNF& formula, const Compiled& p, const Options& options) {
    Result r;
    if (p.empty) {
        r = explore<U64>(0, 0, [](auto, auto, auto, auto&) { return false; }, [](auto) { return false; }, options);
    } else if (p.slots <= 64) {
        auto step = [&](std::size_t i, U64 state, int bit, U64& child) {
            U64 hit = 0, close = 0;
            if (p.slots) { hit = (bit ? p.positive : p.negative)[i][0]; close = p.close[i][0]; }
            const U64 satisfied = state | hit;
            if ((satisfied & close) != close) return false;
            child = satisfied ^ close;
            return true;
        };
        r = explore<U64>(formula.n, 0, step, [](auto) { return true; }, options);
    } else {
        const std::size_t words = (p.slots + 63) / 64;
        auto step = [&](std::size_t i, const Mask& state, int bit, Mask& child) {
            const auto& hit = (bit ? p.positive : p.negative)[i];
            const auto& close = p.close[i];
            child.resize(words);
            for (std::size_t w = 0; w < words; ++w) {
                const U64 satisfied = state[w] | hit[w];
                if ((satisfied & close[w]) != close[w]) return false;
                child[w] = satisfied ^ close[w];
            }
            return true;
        };
        r = explore<Mask>(formula.n, Mask(words), step, [](const auto&) { return true; }, options);
    }
    if (r.status == "ACCEPT") r.status = "SAT";
    else if (r.status == "REJECT") r.status = "UNSAT";
    if (r.has_witness) {
        if (!verify(formula, r.witness)) throw std::runtime_error("invalid internal CNF witness");
        r.verified = true;
    }
    return r;
}
static Result solve_mod(U64 modulus, std::size_t bits, U64 target, const Options& options) {
    if (!modulus || target >= modulus) throw std::runtime_error("invalid modulus or target");
    auto step = [modulus](std::size_t, U64 r, int bit, U64& next) {
        // Overflow-safe (2*r + bit) mod modulus, even at UINT64_MAX.
        next = r >= modulus - r ? r - (modulus - r) : r + r;
        if (bit) next = next == modulus - 1 ? 0 : next + 1;
        return true;
    };
    auto result = explore<U64>(bits, 0, step, [target](U64 r) { return r == target; }, options);
    if (result.has_witness) {
        U64 r = 0, next;
        for (int bit : result.witness) { step(0, r, bit, next); r = next; }
        if (r != target) throw std::runtime_error("invalid internal modular witness");
        result.verified = true;
    }
    return result;
}
static long peak_rss_kib() {
    // Linux: current address-space high-water mark; excludes a pre-exec floor.
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmHWM:", 0) == 0) {
            std::istringstream value(line.substr(6));
            long kib;
            if (value >> kib) return kib;
        }
    }
#ifdef __unix__
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage)) return -1;
#ifdef __APPLE__
    return usage.ru_maxrss / 1024;
#else
    return usage.ru_maxrss;
#endif
#else
    return -1;
#endif
}
static std::string json_result(const Result& r, const Options& options, const std::string& metadata) {
    std::ostringstream out;
    out << std::setprecision(12) << "{\"engine\":\"cpp\",\"status\":\"" << r.status
        << "\",\"reason\":" << (r.reason.empty() ? "null" : "\"" + r.reason + "\"")
        << ",\"graph_seconds\":" << r.graph_seconds << ",\"prepare_seconds\":" << r.prepare_seconds
        << ",\"total_seconds\":" << r.total_seconds << ",\"peak_rss_kib\":" << peak_rss_kib()
        << ",\"configurations\":" << r.configurations << ",\"attempted_edges\":" << r.attempted_edges
        << ",\"valid_edges\":" << r.valid_edges << ",\"merged_edges\":" << r.merged_edges
        << ",\"peak_frontier\":" << r.peak_frontier << ",\"completed_layers\":" << r.completed_layers
        << ",\"witness_verified\":" << (r.verified ? "true" : "null") << ",\"witness\":";
    if (!r.has_witness) out << "null";
    else {
        out << '[';
        for (std::size_t i = 0; i < r.witness.size(); ++i) { if (i) out << ','; out << r.witness[i]; }
        out << ']';
    }
    if (options.trace) {
        out << ",\"widths\":[";
        for (std::size_t i = 0; i < r.widths.size(); ++i) { if (i) out << ','; out << r.widths[i]; }
        out << ']';
    }
    out << ',' << metadata << '}';
    return out.str();
}
int main(int argc, char** argv) {
    try {
        if (argc < 3) throw std::runtime_error("usage: config_graph_cpp cnf FILE [options] | mod MODULUS BITS TARGET [options]");
        const std::string mode = argv[1];
        if (mode != "cnf" && mode != "mod") throw std::runtime_error("unknown mode");
        int option_start = mode == "cnf" ? 3 : 5;
        if (argc < option_start) throw std::runtime_error("missing arguments");
        Options options;
        std::string output;
        for (int i = option_start; i < argc; ++i) {
            const std::string flag = argv[i];
            if (flag == "--trace") options.trace = true;
            else if (flag == "--decision-only") options.witness = false;
            else if (flag == "--timeout" || flag == "--max-frontier" || flag == "--output") {
                if (++i == argc) throw std::runtime_error("missing flag value");
                if (flag == "--output") output = argv[i];
                else if (flag == "--timeout") {
                    std::size_t used;
                    options.timeout = std::stod(argv[i], &used);
                    if (used != std::string(argv[i]).size() || !std::isfinite(options.timeout) || options.timeout < 0)
                        throw std::runtime_error("timeout must be finite and nonnegative");
                } else options.max_frontier = unsigned_number(argv[i]);
            } else throw std::runtime_error("unknown flag: " + flag);
        }
        const auto begin = Clock::now();
        Result result;
        std::ostringstream metadata;
        if (mode == "cnf") {
            const CNF formula = read_dimacs(argv[2]);
            const auto prep_start = Clock::now();
            const Compiled prepared = prepare_cnf(formula);
            const double prep_time = seconds(prep_start);
            result = solve_cnf(formula, prepared, options);
            result.prepare_seconds = prep_time;
            metadata << "\"mode\":\"cnf\",\"variables\":" << formula.n << ",\"clauses\":" << formula.clauses.size()
                     << ",\"normalized_clauses\":" << prepared.normalized << ",\"state_bits_bound\":" << prepared.slots;
        } else {
            const U64 modulus = unsigned_number(argv[2]), bits = unsigned_number(argv[3]), target = unsigned_number(argv[4]);
            result = solve_mod(modulus, bits, target, options);
            U64 remaining = modulus ? modulus - 1 : 0;
            int state_bits = 0;
            while (remaining) { ++state_bits; remaining >>= 1; }
            metadata << "\"mode\":\"mod\",\"modulus\":" << modulus << ",\"bits\":" << bits
                     << ",\"target\":" << target << ",\"state_bits_bound\":" << state_bits;
        }
        result.total_seconds = seconds(begin);
        const std::string line = json_result(result, options, metadata.str());
        if (!output.empty()) {
            std::ofstream file(output);
            if (!(file << line << '\n')) throw std::runtime_error("cannot write result");
        }
        std::cout << line << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 2;
    }
}
