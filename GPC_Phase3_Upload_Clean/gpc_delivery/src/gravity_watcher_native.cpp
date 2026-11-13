// Five Gravity Engine — experimental solver for VOG Five in a Row (9x6, gravity, connect 5).
// Build: g++ -O3 -march=native -std=c++17 -DNDEBUG five_gravity_engine.cpp -o five_engine
// Run:   ./five_engine --selftest
//        ./five_engine --analyze e d e c ...   (moves listed from first to last, columns a-i)
//        ./five_engine --play [time_ms] [human-first|engine-first]
//
// Important: this is a strong search prototype, NOT a mathematically solved-game certificate.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <functional>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <random>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace five {
constexpr int W = 9;
constexpr int H = 7;
constexpr int K = 5;
constexpr int STRIDE = H + 1; // sentinel bit above each column
constexpr int MAX_MOVES = W * H;
constexpr int WIN_SCORE = 1'000'000;
constexpr int INF = 2'000'000;
using U64 = unsigned __int128;
using U64Low = std::uint64_t;
inline U64Low mix64low(U64Low x) noexcept {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}
inline U64Low hash128(U64 x) noexcept {
    const U64Low lo = static_cast<U64Low>(x);
    const U64Low hi = static_cast<U64Low>(x >> 64);
    return mix64low(lo ^ (mix64low(hi) + 0x517cc1b727220a95ULL));
}

constexpr U64 low_bits(int n) { return (U64(1) << n) - 1; }
const std::array<U64, W> COL_MASK = [] {
    std::array<U64, W> a{};
    for (int c = 0; c < W; ++c) a[c] = low_bits(H) << (c * STRIDE);
    return a;
}();
const std::array<U64, W> BOTTOM = [] {
    std::array<U64, W> a{};
    for (int c = 0; c < W; ++c) a[c] = U64(1) << (c * STRIDE);
    return a;
}();
const std::array<int, W> CENTER_ORDER = {4, 3, 5, 2, 6, 1, 7, 0, 8};

inline int popcount(U64 x) {
#if defined(_MSC_VER)
    return static_cast<int>(__popcnt64(x));
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_popcountll(static_cast<std::uint64_t>(x)) + __builtin_popcountll(static_cast<std::uint64_t>(x >> 64));
#else
    int n = 0;
    while (x) { x &= (x - 1); ++n; }
    return n;
#endif
}
inline int height_of(U64 mask, int c) { return popcount(mask & COL_MASK[c]); }
inline U64 drop_bit(U64 mask, int c) {
    if (c < 0 || c >= W) return 0;
    return (mask + BOTTOM[c]) & COL_MASK[c];
}
inline bool has_five(U64 bits) {
    // Any contiguous 5-run in vertical, horizontal or either diagonal direction.
    constexpr int shifts[] = {1, STRIDE, STRIDE - 1, STRIDE + 1};
    for (int d : shifts) {
        U64 x = bits;
        x &= bits >> d;
        x &= bits >> (2 * d);
        x &= bits >> (3 * d);
        x &= bits >> (4 * d);
        if (x) return true;
    }
    return false;
}
inline U64 mirror_bits(U64 b) {
    U64 out = 0;
    for (int c = 0; c < W; ++c) {
        U64 col = (b >> (c * STRIDE)) & low_bits(H);
        out |= col << ((W - 1 - c) * STRIDE);
    }
    return out;
}

struct State {
    U64 pos = 0;  // stones belonging to the player whose turn it is
    U64 mask = 0; // all occupied cells
    int moves = 0;
};
inline U64 opponent_bits(const State& s) { return s.mask ^ s.pos; }

struct Window {
    U64 bits;
    std::array<int, K> cols;
    std::array<int, K> rows;
};
const std::vector<Window> WINDOWS = [] {
    std::vector<Window> out;
    const int dc[4] = {1, 0, 1, 1};
    const int dr[4] = {0, 1, 1, -1};
    for (int r = 0; r < H; ++r) {
        for (int c = 0; c < W; ++c) {
            for (int dir = 0; dir < 4; ++dir) {
                int ec = c + (K - 1) * dc[dir];
                int er = r + (K - 1) * dr[dir];
                if (ec < 0 || ec >= W || er < 0 || er >= H) continue;
                Window w{};
                for (int i = 0; i < K; ++i) {
                    int cc = c + i * dc[dir];
                    int rr = r + i * dr[dir];
                    w.bits |= U64(1) << (cc * STRIDE + rr);
                    w.cols[i] = cc;
                    w.rows[i] = rr;
                }
                out.push_back(w);
            }
        }
    }
    return out;
}();

struct TTEntry {
    U64 mask = 0;
    U64 pos = 0;
    int score = 0;
    int16_t depth = -1;
    uint8_t bound = 0; // 0 empty, 1 exact, 2 lower, 3 upper
    int8_t best = -1;  // canonical column
    uint32_t generation = 0;
};

struct SearchResult {
    int best_col = -1;
    int score = 0;
    int depth = 0;
    std::uint64_t nodes = 0;
    std::uint64_t qnodes = 0;
    std::uint64_t tt_hits = 0;
    std::uint64_t cutoffs = 0;
    double seconds = 0.0;
    bool timed_out = false;
    bool exact_wdl = false; // set only when a complete game-theoretic W/D/L result is known
    int exact_outcome = 2;  // +1 win, 0 draw, -1 loss; 2 unresolved
    std::vector<int> pv;
};

struct ExactResult {
    // outcome is from the side-to-move perspective: +1 win, 0 draw, -1 loss, 2 unresolved.
    int outcome = 2;
    int best_col = -1;
    std::uint64_t nodes = 0;
    double seconds = 0.0;
    bool eligible = false;
    bool solved = false;
    bool timed_out = false;
    bool resource_limited = false;
    bool invalid_position = false;
};

struct ProofResult {
    // status is from the root side-to-move perspective: +1 forced win proven,
    // -1 forced win refuted, 0 unresolved. Refuted does NOT distinguish draw from loss.
    int status = 0;
    int best_col = -1;
    std::uint64_t nodes = 0;
    double seconds = 0.0;
    bool timed_out = false;
    bool resource_limited = false;
    bool invalid_position = false;
};

struct DfpnResult {
    // Same status semantics as ProofResult; proof/disproof numbers are search diagnostics.
    int status = 0;
    int best_col = -1;
    std::uint64_t proof_number = 1;
    std::uint64_t disproof_number = 1;
    std::uint64_t nodes = 0;
    double seconds = 0.0;
    bool timed_out = false;
    bool resource_limited = false;
    bool invalid_position = false;
    bool reached_frontier = false;
    std::size_t cache_entries = 0;
};

struct ExactKey {
    U64 pos = 0;
    U64 mask = 0;
    bool operator==(const ExactKey& other) const noexcept {
        return pos == other.pos && mask == other.mask;
    }
};

struct ExactKeyHash {
    std::size_t operator()(const ExactKey& k) const noexcept {
        const std::uint64_t a = hash128(k.mask);
        const std::uint64_t b = hash128(k.pos);
        return static_cast<std::size_t>(mix64low(a ^ (b + 0x517cc1b727220a95ULL)));
    }
};

struct ExactMemoValue {
    int8_t outcome = 2;
    int8_t best = -1; // stored in canonical mirrored orientation
};

class Engine {
public:
    explicit Engine(std::size_t tt_megabytes = 64, bool use_transposition_table = true,
                    bool use_pvs = true)
        : use_tt_(use_transposition_table), use_pvs_(use_pvs) {
        // Exact WDL and proof modes do not use this table: avoid even a small wasted
        // allocation when a caller explicitly disables it.
        if (!use_tt_) return;
        std::size_t requested_mb = std::max<std::size_t>(1, std::min<std::size_t>(1024, tt_megabytes));
        for (std::size_t mb = requested_mb; mb >= 1; mb = (mb == 1 ? 0 : std::max<std::size_t>(1, mb / 2))) {
            std::size_t n = 1;
            const std::size_t bytes = mb * 1024 * 1024; // capped at 1 GiB above
            const std::size_t target = std::max<std::size_t>(1024, bytes / sizeof(TTEntry));
            while ((n << 1) <= target) n <<= 1;
            try {
                tt_.resize(n);
                tt_mask_ = n - 1;
                tt_allocated_mb_ = (n * sizeof(TTEntry)) / (1024 * 1024);
                return;
            } catch (const std::bad_alloc&) {
                std::vector<TTEntry>().swap(tt_);
            }
            if (mb == 1) break;
        }
        // Safe low-memory fallback: search remains correct, merely slower without a TT.
        use_tt_ = false;
        tt_mask_ = 0;
        tt_allocated_mb_ = 0;
    }

    bool transposition_table_enabled() const noexcept { return use_tt_ && !tt_.empty(); }
    std::size_t transposition_table_megabytes() const noexcept { return tt_allocated_mb_; }

    SearchResult think(const State& root, int time_ms = 1500, int max_depth = MAX_MOVES) {
        reset_counters();
        stopped_ = false;
        timed_out_ = false;
        ++generation_;
        start_ = Clock::now();
        deadline_ = start_ + std::chrono::milliseconds(std::max(1, time_ms));

        SearchResult result;
        std::uint64_t exact_probe_nodes = 0;
        int last_score = 0;
        int last_move = -1;
        const int root_legal = legal_count(root.mask);
        if (root_legal == 0) {
            result.score = 0;
            result.seconds = elapsed_seconds();
            result.nodes = nodes_ + exact_probe_nodes;
            result.qnodes = qnodes_;
            result.tt_hits = tt_hits_;
            result.cutoffs = cutoffs_;
            result.exact_wdl = (root.moves >= MAX_MOVES);
            result.exact_outcome = result.exact_wdl ? 0 : 2;
            return result;
        }

        // In small endgames, try exact WDL solving first. Use only a fraction of the
        // remaining clock so a hard endgame can fall back to practical search.
        const int empty_cells = MAX_MOVES - root.moves;
        if (empty_cells <= 18 && valid_structural_state(root)) {
            const int exact_budget = std::max(1, std::min(500, std::max(1, time_ms / 3)));
            ExactResult exact = solve_exact_endgame(root, exact_budget, 18);
            exact_probe_nodes = exact.nodes;
            if (exact.solved) {
                result.best_col = exact.best_col;
                result.score = exact.outcome;
                result.depth = empty_cells;
                result.nodes = exact.nodes;
                result.seconds = elapsed_seconds();
                result.timed_out = false;
                result.exact_wdl = true;
                result.exact_outcome = exact.outcome;
                if (exact.best_col >= 0) result.pv = {exact.best_col};
                return result;
            }
        }

        // Do not iterate over artificial depths when the root already has a proven tactic.
        const auto root_wins = winning_columns(root.pos, root.mask);
        if (!root_wins.empty()) {
            int best = root_wins.front();
            for (int c : root_wins) if (std::abs(c - 4) < std::abs(best - 4)) best = c;
            result.best_col = best;
            result.score = WIN_SCORE - (root.moves + 1);
            result.depth = 1;
            result.pv = {best};
            result.seconds = elapsed_seconds();
            result.nodes = nodes_ + exact_probe_nodes;
            result.qnodes = qnodes_;
            result.tt_hits = tt_hits_;
            result.cutoffs = cutoffs_;
            return result;
        }
        const auto root_enemy_wins = winning_columns(root.mask ^ root.pos, root.mask);
        if (root_enemy_wins.size() >= 2) {
            const auto legal = legal_moves(root);
            result.best_col = legal.empty() ? -1 : legal.front();
            result.score = -WIN_SCORE + (root.moves + 2);
            result.depth = 1;
            if (result.best_col >= 0) result.pv = {result.best_col};
            result.seconds = elapsed_seconds();
            result.nodes = nodes_ + exact_probe_nodes;
            result.qnodes = qnodes_;
            result.tt_hits = tt_hits_;
            result.cutoffs = cutoffs_;
            return result;
        }

        // Iterative deepening; only completed depths are accepted.
        for (int d = 1; d <= max_depth && d <= MAX_MOVES - root.moves; ++d) {
            int candidate_move = last_move;
            int candidate_score = 0;
            bool complete = false;
            // Aspiration window after a few iterations, with a full-window retry on failure.
            if (d >= 5 && std::abs(last_score) < WIN_SCORE - 1000) {
                int delta = 120;
                int a = std::max(-INF, last_score - delta);
                int b = std::min(INF, last_score + delta);
                candidate_score = search_root(root, d, a, b, candidate_move, complete);
                if (complete && (candidate_score <= a || candidate_score >= b) && !stopped_) {
                    candidate_move = last_move;
                    complete = false;
                    candidate_score = search_root(root, d, -INF, INF, candidate_move, complete);
                }
            } else {
                candidate_score = search_root(root, d, -INF, INF, candidate_move, complete);
            }
            if (!complete || stopped_) break;
            last_score = candidate_score;
            last_move = candidate_move;
            result.depth = d;
            result.score = candidate_score;
            result.best_col = candidate_move;
            result.pv = principal_variation(root, std::min(d, 20));
        }

        result.nodes = nodes_ + exact_probe_nodes;
        result.qnodes = qnodes_;
        result.tt_hits = tt_hits_;
        result.cutoffs = cutoffs_;
        result.seconds = elapsed_seconds();
        result.timed_out = timed_out_;
        if (result.best_col < 0) {
            auto moves = ordered_moves(root, -1);
            if (!moves.empty()) result.best_col = moves.front();
        }
        return result;
    }

    int evaluate_for_side_to_move(const State& s) const { return evaluate(s); }

    std::vector<int> legal_moves(const State& s) const {
        std::vector<int> out;
        for (int c : CENTER_ORDER) if (drop_bit(s.mask, c)) out.push_back(c);
        return out;
    }

    std::vector<int> winning_columns(U64 player_bits, U64 mask) const {
        std::vector<int> out;
        for (int c = 0; c < W; ++c) {
            U64 m = drop_bit(mask, c);
            if (m && has_five(player_bits | m)) out.push_back(c);
        }
        return out;
    }

    bool test_tactical_position(const State& s, int expected_col, int time_ms = 1000) {
        SearchResult r = think(s, time_ms, 12);
        return r.best_col == expected_col;
    }

    // A genuinely exact W/D/L solver intended for endgames only. It searches all legal
    // continuations unless it proves a win, and caches only fully solved states.
    // max_empty protects callers from accidentally attempting an unbounded opening solve.
    ExactResult solve_exact_endgame(const State& root, int time_ms = 5000, int max_empty = 12) {
        ExactResult result;
        const auto started = Clock::now();
        result.invalid_position = !valid_structural_state(root);
        if (result.invalid_position) {
            result.seconds = std::chrono::duration<double>(Clock::now() - started).count();
            return result;
        }
        const int empty = MAX_MOVES - root.moves;
        if (max_empty < 0 || empty > max_empty) {
            result.seconds = std::chrono::duration<double>(Clock::now() - started).count();
            return result;
        }
        result.eligible = true;
        ExactContext context;
        context.deadline = started + std::chrono::milliseconds(std::max(1, time_ms));
        context.max_entries = 1'000'000;
        ExactEval solved{2, false};
        try {
            context.memo.reserve(65'536);
            solved = exact_wdl(root, context);
        } catch (const std::bad_alloc&) {
            // Never report an incomplete search as a game-theoretic result.
            context.resource_limited = true;
            solved = {2, false};
        }
        result.nodes = context.nodes;
        result.timed_out = context.timed_out;
        result.resource_limited = context.resource_limited;
        result.seconds = std::chrono::duration<double>(Clock::now() - started).count();
        result.solved = solved.complete;
        if (solved.complete) {
            result.outcome = solved.outcome;
            const Canonical canonical = canonicalize(root.pos, root.mask);
            const ExactKey key{canonical.pos, canonical.mask};
            auto it = context.memo.find(key);
            if (it != context.memo.end() && it->second.best >= 0) {
                const int canonical_move = it->second.best;
                result.best_col = canonical.flipped ? W - 1 - canonical_move : canonical_move;
            }
        }
        return result;
    }

    // AND/OR proof search for a forced win by the player to move at the root.
    // A proven result is exact: target-to-move nodes are OR nodes; opponent-to-move
    // nodes are AND nodes. Depth/time/memory cutoffs yield unresolved, never refuted.
    ProofResult prove_forced_win(const State& root, int time_ms = 1500, int max_plies = 24) {
        ProofResult result;
        const auto started = Clock::now();
        result.invalid_position = !valid_structural_state(root);
        if (result.invalid_position) {
            result.seconds = std::chrono::duration<double>(Clock::now() - started).count();
            return result;
        }
        ProofContext context;
        context.deadline = started + std::chrono::milliseconds(std::max(1, time_ms));
        context.max_entries = 500'000;
        context.root_moves = root.moves;
        const int bounded_plies = std::max(0, std::min(max_plies, MAX_MOVES - root.moves));
        ProofEval solved{0, -1};
        try {
            context.memo.reserve(32'768);
            solved = proof_search(root, true, bounded_plies, context, 0);
        } catch (const std::bad_alloc&) {
            context.resource_limited = true;
            solved = {0, -1};
        }
        result.status = solved.status;
        result.best_col = solved.best_col;
        result.nodes = context.nodes;
        result.timed_out = context.timed_out;
        result.resource_limited = context.resource_limited;
        result.seconds = std::chrono::duration<double>(Clock::now() - started).count();
        return result;
    }

    // Depth-first proof-number search (DFPN) for the proposition "the side to move can
    // force a win". Unlike plain bounded AND/OR DFS, it selects the most-proving child
    // using proof/disproof numbers and threshold propagation. A horizon leaf remains
    // unresolved; only pn==0 or dn==0 yields a proof/refutation.
    DfpnResult prove_forced_win_dfpn(const State& root, int time_ms = 1500, int max_plies = 24) {
        DfpnResult result;
        const auto started = Clock::now();
        result.invalid_position = !valid_structural_state(root);
        if (result.invalid_position) {
            result.seconds = std::chrono::duration<double>(Clock::now() - started).count();
            return result;
        }
        DfpnContext context;
        DfpnMemoTable local_memo;
        context.deadline = started + std::chrono::milliseconds(std::max(1, time_ms));
        context.max_entries = 1'000'000;
        context.root_moves = root.moves;
        context.memo = &local_memo;
        const int bounded_plies = std::max(0, std::min(max_plies, MAX_MOVES - root.moves));
        DfpnEval solved{};
        try {
            context.memo->configure(context.max_entries);
            solved = dfpn_search(root, true, bounded_plies, DFPN_INF, DFPN_INF, context);
        } catch (const std::bad_alloc&) {
            context.resource_limited = true;
            solved = {};
        }
        result.proof_number = solved.pn;
        result.disproof_number = solved.dn;
        result.status = solved.pn == 0 ? 1 : (solved.dn == 0 ? -1 : 0);
        result.best_col = result.status == 1 ? solved.best_col : -1;
        result.nodes = context.nodes;
        result.timed_out = context.timed_out;
        result.resource_limited = context.resource_limited;
        result.seconds = std::chrono::duration<double>(Clock::now() - started).count();
        return result;
    }

    struct WatcherResult {
        std::string status = "UNKNOWN";
        std::string certificate;
        std::uint64_t proof_number = 1, disproof_number = 1, nodes = 0, frontiers = 0;
        std::size_t cache_entries = 0;
        double seconds = 0.0;
        bool timed_out = false, resource_limited = false, exact_wdl = false;
        bool certificate_built = false;
        bool root_frontier = false;
        std::uint64_t gravity_nodes_evaluated = 0;
    };

    bool valid_state_for_watcher(const State& s) const { return valid_structural_state(s); }
    std::size_t watcher_cache_entries() const { return watcher_memo_.size(); }
    bool watcher_memory_self_test(std::ostream& out);
    void configure_distributed_checkpoint(const std::string& path, std::size_t capacity) {
        watcher_checkpoint_path_ = path;
        watcher_capacity_ = std::max<std::size_t>(100'000, capacity);
        watcher_checkpoint_loaded_ = false;
        watcher_memo_ = DfpnMemoTable{};
        watcher_cpu_side_ = -1;
    }
    bool export_distributed_solved(const std::string& path) const {
        return watcher_memo_.export_solved(path);
    }

    void reset_watcher(int cpu_side = -1) {
        watcher_memo_ = DfpnMemoTable{};
        watcher_exact_context_.memo.clear();
        watcher_exact_context_.nodes = 0;
        watcher_exact_context_.timed_out = false;
        watcher_exact_context_.resource_limited = false;
        watcher_cpu_side_ = cpu_side;
        watcher_horizon_ = -1;
        watcher_checkpoint_loaded_ = false;
    }

    WatcherResult watcher_slice(const State& root, int cpu_side, const std::vector<int>& history,
                                int time_ms, int max_plies, std::uint64_t node_limit = 180000) {
        WatcherResult out;
        const auto started = Clock::now();
        if (!valid_structural_state(root) || cpu_side < 0 || cpu_side > 1) {
            out.status = "INVALID";
            out.seconds = std::chrono::duration<double>(Clock::now() - started).count();
            return out;
        }
        if (watcher_cpu_side_ != cpu_side) reset_watcher(cpu_side);
        current_history_ = history;
        ++watcher_queries_;
        gravity_nodes_evaluated_ = 0;
        if (watcher_memo_.slots.empty()) {
            watcher_memo_.configure(watcher_capacity_);
            if (!watcher_checkpoint_loaded_ && !watcher_checkpoint_path_.empty())
                watcher_memo_.load_checkpoint(watcher_checkpoint_path_, root, cpu_side, watcher_capacity_);
            watcher_checkpoint_loaded_ = true;
        }
        // Compact before reaching a hard stop. All exact proof/refutation entries
        // survive; only unresolved ordering/work hints can be discarded.
        if (watcher_memo_.size() >= watcher_memo_.max_entries * 9 / 10)
            watcher_memo_.compact();
        const int bounded_plies = std::max(0, std::min(max_plies, MAX_MOVES - root.moves));
        // Do not flush the table when the root horizon grows or a new game starts.
        // Each frontier entry carries the remaining horizon at which it was cut;
        // dfpn_search reopens it when a later path supplies more remaining plies.
        // Non-frontier PN/DN values are recomputed from their children on revisit.
        watcher_horizon_ = std::max(watcher_horizon_, bounded_plies);

        // Exact WDL is attempted only in sufficiently small endings. Its memo is
        // retained across slices and across positions in this game.
        const int empty = MAX_MOVES - root.moves;
        if (empty <= 18) {
            auto& exact = watcher_exact_context_;
            exact.deadline = started + std::chrono::milliseconds(std::max(1, time_ms / 2));
            exact.nodes = 0; exact.timed_out = false; exact.resource_limited = false;
            exact.max_entries = 500'000;
            ExactEval solved{2, false};
            try { solved = exact_wdl(root, exact); }
            catch (const std::bad_alloc&) { exact.resource_limited = true; solved = {2, false}; }
            const bool target_to_move = ((root.moves & 1) == cpu_side);
            if (solved.complete) {
                const bool target_wins = target_to_move ? (solved.outcome == 1) : (solved.outcome == -1);
                out.exact_wdl = true;
                if (!target_wins) {
                    out.status = "REFUTED_EXACT_WDL";
                } else {
                    out.status = "PROVEN_WIN";
                    out.certificate = build_strategy_dag(root, cpu_side, current_history_, true, 100'000);
                    out.certificate_built = !out.certificate.empty();
                    if (!out.certificate_built) out.status = "UNKNOWN_CERTIFICATE_BUILD_FAILED";
                }
                out.nodes = exact.nodes;
                out.cache_entries = exact.memo.size();
                out.seconds = std::chrono::duration<double>(Clock::now() - started).count();
                if (!watcher_checkpoint_path_.empty()) watcher_memo_.save_checkpoint(watcher_checkpoint_path_, root, cpu_side);
                return out;
            }
        }

        DfpnContext context;
        context.memo = &watcher_memo_;
        context.deadline = Clock::now() + std::chrono::milliseconds(std::max(1, time_ms));
        context.node_limit = std::max<std::uint64_t>(1000, node_limit);
        context.max_entries = watcher_memo_.max_entries;
        context.root_moves = root.moves;
        const bool target_to_move = ((root.moves & 1) == cpu_side);
        DfpnEval solved{};
        try { solved = dfpn_search(root, target_to_move, bounded_plies, DFPN_INF, DFPN_INF, context); }
        catch (const std::bad_alloc&) { context.resource_limited = true; solved = dfpn_load(root, context); }
        out.proof_number = solved.pn;
        out.disproof_number = solved.dn;
        out.root_frontier = solved.frontier;
        out.nodes = context.nodes;
        out.frontiers = context.frontier_nodes;
        out.gravity_nodes_evaluated = gravity_nodes_evaluated_;
        out.cache_entries = watcher_memo_.size();
        out.timed_out = context.timed_out;
        out.resource_limited = context.resource_limited;
        out.seconds = std::chrono::duration<double>(Clock::now() - started).count();
        if (solved.pn == 0) {
            out.status = "PROVEN_WIN";
            out.certificate = build_strategy_dag(root, cpu_side, current_history_, false, 100'000);
            out.certificate_built = !out.certificate.empty();
            if (!out.certificate_built) out.status = "UNKNOWN_CERTIFICATE_BUILD_FAILED";
        } else if (solved.dn == 0) out.status = "REFUTED_WITHIN_HORIZON";
        else out.status = "UNKNOWN";
        if (!watcher_checkpoint_path_.empty()) watcher_memo_.save_checkpoint(watcher_checkpoint_path_, root, cpu_side);
        return out;
    }

private:
    using Clock = std::chrono::steady_clock;
    std::vector<TTEntry> tt_;
    std::size_t tt_mask_ = 0;
    std::size_t tt_allocated_mb_ = 0;
    bool use_tt_ = true;
    bool use_pvs_ = true;
    uint32_t generation_ = 1;
    bool stopped_ = false;
    bool timed_out_ = false;
    Clock::time_point start_{}, deadline_{};
    std::uint64_t nodes_ = 0, qnodes_ = 0, tt_hits_ = 0, cutoffs_ = 0;
    std::array<std::array<int, W>, 2> history_{};
    std::array<std::array<int, W>, 64> killers_{};

    struct ExactEval {
        int outcome = 2;
        bool complete = false;
    };
    struct ExactContext {
        std::unordered_map<ExactKey, ExactMemoValue, ExactKeyHash> memo;
        Clock::time_point deadline{};
        std::uint64_t nodes = 0;
        std::size_t max_entries = 1'000'000;
        bool timed_out = false;
        bool resource_limited = false;
    };

    struct ProofEval { int status = 0; int best_col = -1; };
    struct ProofContext {
        std::unordered_map<ExactKey, ExactMemoValue, ExactKeyHash> memo;
        Clock::time_point deadline{};
        std::uint64_t nodes = 0;
        std::size_t max_entries = 500'000;
        int root_moves = 0;
        bool timed_out = false;
        bool resource_limited = false;
    };

    static constexpr std::uint64_t DFPN_INF = (std::uint64_t(1) << 60);
    struct DfpnMemoValue {
        std::uint64_t pn = 1;
        std::uint64_t dn = 1;
        int8_t best = -1; // stored in canonical mirrored orientation
        bool frontier = false;
        int16_t frontier_remaining = -1; // horizon remaining when this node was cut
    };
    // Flat open-addressed table for DFPN. Full (128-bit position, 128-bit mask)
    // keys are retained, so no probabilistic hash-only proof caching is used.
    struct DfpnSlot {
        ExactKey key{};
        DfpnMemoValue value{};
        bool occupied = false;
    };
    struct DfpnMemoTable {
        std::vector<DfpnSlot> slots;
        std::size_t count = 0;
        std::size_t max_entries = 0;
        std::size_t mask = 0;
        void configure(std::size_t cap) {
            max_entries = cap;
            std::size_t n = 1;
            const std::size_t target = std::max<std::size_t>(1024, cap * 2);
            while (n < target) n <<= 1;
            slots.clear(); slots.resize(n); count = 0; mask = n - 1;
        }
        DfpnMemoValue* find(const ExactKey& key) {
            if (slots.empty()) return nullptr;
            std::size_t i = ExactKeyHash{}(key) & mask;
            for (std::size_t probe = 0; probe < slots.size(); ++probe) {
                DfpnSlot& slot = slots[i];
                if (!slot.occupied) return nullptr;
                if (slot.key == key) return &slot.value;
                i = (i + 1) & mask;
            }
            return nullptr;
        }
        bool insert(const ExactKey& key, const DfpnMemoValue& value) {
            if (DfpnMemoValue* existing = find(key)) { *existing = value; return true; }
            if (count >= max_entries || slots.empty()) return false;
            std::size_t i = ExactKeyHash{}(key) & mask;
            for (std::size_t probe = 0; probe < slots.size(); ++probe) {
                DfpnSlot& slot = slots[i];
                if (!slot.occupied) {
                    slot.key = key; slot.value = value; slot.occupied = true; ++count; return true;
                }
                i = (i + 1) & mask;
            }
            return false;
        }
        bool set(const ExactKey& key, const DfpnMemoValue& value) {
            if (DfpnMemoValue* slot = find(key)) { *slot = value; return true; }
            return insert(key, value);
        }
        std::size_t size() const noexcept { return count; }
        // Bounded-memory maintenance. Keep every mathematically settled proof or
        // refutation, then retain the most promising unresolved PN/DN records.
        // Dropping an unresolved cache record can cost time, never proof soundness.
        std::size_t compact() {
            if (slots.empty() || count < max_entries * 9 / 10) return 0;
            std::vector<std::pair<ExactKey, DfpnMemoValue>> settled;
            std::vector<std::pair<ExactKey, DfpnMemoValue>> unresolved;
            settled.reserve(count / 8 + 1);
            unresolved.reserve(count);
            for (const DfpnSlot& slot : slots) if (slot.occupied) {
                auto item = std::make_pair(slot.key, slot.value);
                if (slot.value.pn == 0 || slot.value.dn == 0) settled.push_back(item);
                else unresolved.push_back(item);
            }
            auto rank = [](const DfpnMemoValue& v) {
                if (v.pn >= DFPN_INF || v.dn >= DFPN_INF || v.pn > DFPN_INF - v.dn)
                    return DFPN_INF;
                return v.pn + v.dn;
            };
            std::stable_sort(unresolved.begin(), unresolved.end(), [&](const auto& a, const auto& b) {
                const auto ra = rank(a.second), rb = rank(b.second);
                if (ra != rb) return ra < rb;
                if (a.second.frontier != b.second.frontier) return !a.second.frontier;
                return a.second.frontier_remaining > b.second.frontier_remaining;
            });
            const std::size_t keep_target = std::max(settled.size(), max_entries * 4 / 5);
            const std::size_t unresolved_keep = keep_target > settled.size() ? keep_target - settled.size() : 0;
            if (unresolved.size() > unresolved_keep) unresolved.resize(unresolved_keep);
            slots.assign(slots.size(), DfpnSlot{});
            count = 0;
            for (const auto& item : settled) insert(item.first, item.second);
            for (const auto& item : unresolved) insert(item.first, item.second);
            return count;
        }
        struct DiskHeader {
            char magic[8];
            std::uint32_t version;
            std::uint32_t target;
            std::uint64_t root_pos_lo, root_pos_hi, root_mask_lo, root_mask_hi;
            std::uint64_t capacity, entries;
        };
        struct DiskEntry {
            std::uint64_t pos_lo, pos_hi, mask_lo, mask_hi, pn, dn;
            std::int8_t best;
            std::uint8_t frontier;
            std::int16_t frontier_remaining;
        };
        static std::pair<std::uint64_t,std::uint64_t> split128(U64 v) {
            return {static_cast<std::uint64_t>(v), static_cast<std::uint64_t>(v >> 64)};
        }
        bool save_checkpoint(const std::string& path, const State& root, int target) const {
            if (path.empty() || slots.empty()) return false;
            const std::string tmp = path + ".tmp";
            const auto rp = split128(root.pos), rm = split128(root.mask);
            DiskHeader h{};
            std::memcpy(h.magic, "GPCDFPN1", 8);
            h.version = 1; h.target = static_cast<std::uint32_t>(target);
            h.root_pos_lo=rp.first; h.root_pos_hi=rp.second;
            h.root_mask_lo=rm.first; h.root_mask_hi=rm.second;
            h.capacity=max_entries;
            for (const DfpnSlot& slot : slots) if (slot.occupied) ++h.entries;
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) return false;
            out.write(reinterpret_cast<const char*>(&h), sizeof(h));
            for (const DfpnSlot& slot : slots) if (slot.occupied) {
                const auto kp=split128(slot.key.pos), km=split128(slot.key.mask);
                DiskEntry e{kp.first,kp.second,km.first,km.second,slot.value.pn,slot.value.dn,
                            slot.value.best,static_cast<std::uint8_t>(slot.value.frontier),
                            slot.value.frontier_remaining};
                out.write(reinterpret_cast<const char*>(&e), sizeof(e));
            }
            out.flush();
            if (!out) { out.close(); std::remove(tmp.c_str()); return false; }
            out.close();
            // POSIX rename replaces the destination atomically; do not delete the
            // previous checkpoint first, or a crash can destroy the last good copy.
            return std::rename(tmp.c_str(), path.c_str()) == 0;
        }
        bool load_checkpoint(const std::string& path, const State& root, int target,
                             std::size_t capacity) {
            if (path.empty()) return false;
            std::ifstream in(path, std::ios::binary);
            if (!in) return false;
            DiskHeader h{};
            in.read(reinterpret_cast<char*>(&h), sizeof(h));
            const auto rp=split128(root.pos), rm=split128(root.mask);
            if (!in || std::memcmp(h.magic,"GPCDFPN1",8)!=0 || h.version!=1 ||
                h.target!=static_cast<std::uint32_t>(target) || h.capacity>capacity ||
                h.root_pos_lo!=rp.first || h.root_pos_hi!=rp.second ||
                h.root_mask_lo!=rm.first || h.root_mask_hi!=rm.second ||
                h.entries>h.capacity || h.entries>capacity) return false;
            if (slots.empty()) configure(capacity);
            if (max_entries!=capacity) return false;
            for (std::uint64_t i=0;i<h.entries;i++) {
                DiskEntry e{}; in.read(reinterpret_cast<char*>(&e),sizeof(e));
                if (!in || e.best < -1 || e.best >= W || e.frontier > 1 ||
                    e.frontier_remaining < -1 || e.frontier_remaining > MAX_MOVES) return false;
                const U64 pos=(U64(e.pos_hi)<<64)|e.pos_lo;
                const U64 mask=(U64(e.mask_hi)<<64)|e.mask_lo;
                if ((pos & ~mask)!=0) return false;
                DfpnMemoValue v{e.pn,e.dn,e.best,e.frontier!=0,e.frontier_remaining};
                if (!insert(ExactKey{pos,mask},v)) return false;
            }
            return bool(in);
        }
        bool export_solved(const std::string& path) const {
            std::ofstream out(path, std::ios::trunc);
            if (!out) return false;
            for (const DfpnSlot& slot : slots) if (slot.occupied &&
                (slot.value.pn==0 || slot.value.dn==0)) {
                const auto kp=split128(slot.key.pos), km=split128(slot.key.mask);
                out << kp.first << ' ' << kp.second << ' ' << km.first << ' ' << km.second << ' '
                    << slot.value.pn << ' ' << slot.value.dn << ' ' << int(slot.value.best) << ' '
                    << int(slot.value.frontier) << ' ' << slot.value.frontier_remaining << '\n';
            }
            return bool(out);
        }
    };
    struct DfpnEval {
        std::uint64_t pn = 1;
        std::uint64_t dn = 1;
        int best_col = -1;
        bool frontier = false;
        int frontier_remaining = -1;
    };
    struct DfpnContext {
        DfpnMemoTable* memo = nullptr; // points at persistent watcher table or a local one-shot table
        Clock::time_point deadline{};
        std::uint64_t nodes = 0;
        std::uint64_t node_limit = 1'000'000;
        std::uint64_t frontier_nodes = 0;
        std::size_t max_entries = 500'000;
        int root_moves = 0;
        bool timed_out = false;
        bool resource_limited = false;
    };

    // One long-lived proof table is shared by all short watcher slices.
    DfpnMemoTable watcher_memo_;
    std::string watcher_checkpoint_path_;
    std::size_t watcher_capacity_ = 1'500'000;
    bool watcher_checkpoint_loaded_ = false;
    int watcher_cpu_side_ = -1;
    int watcher_horizon_ = -1;
    std::uint64_t watcher_queries_ = 0;
    mutable std::uint64_t gravity_nodes_evaluated_ = 0;
    std::vector<int> current_history_;
    ExactContext watcher_exact_context_;

    struct GravityFeatures { int lambda_level = 5; int score = 0; int live_windows = 0; int shared_support = 0; };

    // Recompute the live gravity-dependency network for the current node. For each
    // uncontested target window, count the target drops still required in each column;
    // support cells shared by several windows receive higher dependency centrality.
    GravityFeatures gravity_features(const State& s, bool target_to_move) const {
        ++gravity_nodes_evaluated_;
        GravityFeatures out;
        const U64 target = target_to_move ? s.pos : (s.mask ^ s.pos);
        const U64 defender = s.mask ^ target;
        std::array<int, W> heights{};
        for (int c = 0; c < W; ++c) heights[c] = height_of(s.mask, c);
        std::array<int, W * H> support_use{};
        int total_score = 0;
        int best_lambda = 5;
        for (const Window& w : WINDOWS) {
            if (defender & w.bits) continue;
            int target_count = popcount(target & w.bits);
            if (target_count == 0) continue;
            std::array<int, W> max_drop{};
            for (int i = 0; i < K; ++i) {
                const int c = w.cols[i], r = w.rows[i];
                const U64 bit = U64(1) << (c * STRIDE + r);
                if (s.mask & bit) continue;
                const int need = std::max(0, r - heights[c] + 1);
                max_drop[c] = std::max(max_drop[c], need);
                for (int y = heights[c]; y < r; ++y) ++support_use[y * W + c];
            }
            int support_cost = 0;
            for (int c = 0; c < W; ++c) support_cost += max_drop[c];
            int lambda = 4;
            if (target_count >= 4) lambda = support_cost <= 1 ? 0 : (support_cost <= 3 ? 1 : 2);
            else if (target_count == 3) lambda = support_cost <= 2 ? 1 : (support_cost <= 4 ? 2 : 3);
            else if (target_count == 2) lambda = support_cost <= 2 ? 2 : 3;
            best_lambda = std::min(best_lambda, lambda);
            const int base = target_count >= 4 ? 12000 : target_count == 3 ? 1800 : target_count == 2 ? 220 : 18;
            total_score += base / (1 + support_cost);
            ++out.live_windows;
        }
        for (int v : support_use) if (v > 1) out.shared_support += (v - 1);
        out.lambda_level = out.live_windows ? best_lambda : 5;
        out.score = total_score + std::min(10000, out.shared_support * 7);
        return out;
    }

    struct CertNode {
        std::string state_key;
        std::string type;
        int move = -1;
        int child = -1;
        std::vector<std::pair<int,int>> children;
    };

    static std::string state_key(const State& s) {
        std::uint64_t p0 = 0, p1 = 0;
        const int side = s.moves & 1;
        const U64 xbits = side == 0 ? s.pos : (s.mask ^ s.pos);
        const U64 obits = side == 1 ? s.pos : (s.mask ^ s.pos);
        for (int c = 0; c < W; ++c) for (int r = 0; r < H; ++r) {
            const U64 bit = U64(1) << (c * STRIDE + r);
            const std::uint64_t packed = std::uint64_t(1) << (r * W + c);
            if (xbits & bit) p0 |= packed;
            if (obits & bit) p1 |= packed;
        }
        std::ostringstream out;
        out << std::hex << std::setfill('0') << std::setw(16) << p0 << ':'
            << std::setw(16) << p1 << ':' << std::dec << side;
        return out.str();
    }

    std::string build_strategy_dag(const State& root, int cpu_side, const std::vector<int>& history,
                                   bool use_exact, int node_cap) {
        std::vector<CertNode> nodes;
        std::unordered_map<ExactKey, int, ExactKeyHash> seen;
        bool failed = false;
        std::function<int(const State&, int)> visit = [&](const State& s, int depth) -> int {
            if (failed || static_cast<int>(nodes.size()) >= node_cap || depth > MAX_MOVES) { failed = true; return -1; }
            const ExactKey raw_key{s.pos, s.mask};
            auto prior = seen.find(raw_key);
            if (prior != seen.end()) return prior->second;
            const int id = static_cast<int>(nodes.size());
            seen.emplace(raw_key, id);
            nodes.emplace_back();
            nodes.back().state_key = state_key(s);
            // The previous move ended the game. The only acceptable terminal is a win
            // by the fixed attacker (CPU side).
            if (s.moves > 0 && has_five(s.mask ^ s.pos)) {
                if (((s.moves - 1) & 1) != cpu_side) { failed = true; return -1; }
                nodes[id].type = "WIN";
                return id;
            }
            if (s.moves >= MAX_MOVES) { failed = true; return -1; }
            const bool target_to_move = ((s.moves & 1) == cpu_side);
            const Canonical canonical = canonicalize(s.pos, s.mask);
            const ExactKey ckey{canonical.pos, canonical.mask};
            auto legal = legal_moves(s);
            if (legal.empty()) { failed = true; return -1; }
            if (target_to_move) {
                int chosen = -1;
                const auto direct_wins = winning_columns(s.pos, s.mask);
                if (!direct_wins.empty()) chosen = direct_wins.front();
                if (chosen < 0 && use_exact) {
                    auto it = watcher_exact_context_.memo.find(ckey);
                    if (it == watcher_exact_context_.memo.end() || it->second.outcome != 1 || it->second.best < 0) { failed = true; return -1; }
                    chosen = it->second.best;
                    if (canonical.flipped) chosen = W - 1 - chosen;
                } else if (chosen < 0) {
                    DfpnMemoValue* value = watcher_memo_.find(ckey);
                    if (!value || value->pn != 0 || value->frontier) { failed = true; return -1; }
                    chosen = value->best;
                    if (chosen >= 0 && canonical.flipped) chosen = W - 1 - chosen;
                }
                if (chosen < 0 || std::find(legal.begin(), legal.end(), chosen) == legal.end()) {
                    chosen = -1;
                    for (int c : legal) {
                        const U64 bit = drop_bit(s.mask, c);
                        State child{s.pos ^ s.mask, s.mask | bit, s.moves + 1};
                        const Canonical cc = canonicalize(child.pos, child.mask);
                        const ExactKey ck{cc.pos, cc.mask};
                        bool wins = false;
                        if (use_exact) {
                            auto it = watcher_exact_context_.memo.find(ck);
                            wins = it != watcher_exact_context_.memo.end() && it->second.outcome == -1;
                        } else {
                            DfpnMemoValue* v = watcher_memo_.find(ck);
                            wins = v && v->pn == 0 && !v->frontier;
                        }
                        if (wins) { chosen = c; break; }
                    }
                }
                if (chosen < 0) { failed = true; return -1; }
                const U64 bit = drop_bit(s.mask, chosen);
                State child{s.pos ^ s.mask, s.mask | bit, s.moves + 1};
                const int child_id = visit(child, depth + 1);
                if (child_id < 0) { failed = true; return -1; }
                nodes[id].type = "OR";
                nodes[id].move = chosen;
                nodes[id].child = child_id;
            } else {
                nodes[id].type = "AND";
                for (int c : legal) {
                    const U64 bit = drop_bit(s.mask, c);
                    State child{s.pos ^ s.mask, s.mask | bit, s.moves + 1};
                    const Canonical cc = canonicalize(child.pos, child.mask);
                    const ExactKey ck{cc.pos, cc.mask};
                    bool proven = false;
                    if (use_exact) {
                        auto it = watcher_exact_context_.memo.find(ck);
                        // An immediate attacker win is a terminal child and does not need a memo entry.
                        proven = it != watcher_exact_context_.memo.end() && it->second.outcome == 1;
                    } else {
                        DfpnMemoValue* v = watcher_memo_.find(ck);
                        proven = v && v->pn == 0 && !v->frontier;
                    }
                    const bool immediate_attacker_win = has_five(child.mask ^ child.pos) && (((child.moves - 1) & 1) == cpu_side);
                    const bool next_attacker_has_direct_win = (((child.moves & 1) == cpu_side) && !winning_columns(child.pos, child.mask).empty());
                    if (!proven && !immediate_attacker_win && !next_attacker_has_direct_win) { failed = true; return -1; }
                    const int child_id = visit(child, depth + 1);
                    if (child_id < 0) { failed = true; return -1; }
                    nodes[id].children.push_back({c, child_id});
                }
            }
            return id;
        };
        const int root_id = visit(root, 0);
        if (failed || root_id < 0) return {};
        std::ostringstream json;
        json << "{\"ruleset\":\"gravity-connect5-9x7-v1\",\"format\":\"gravity-strategy-dag-v1\",\"attacker\":\"" << (cpu_side == 0 ? 'X' : 'O')
             << "\",\"history\":[";
        for (std::size_t i = 0; i < history.size(); ++i) { if (i) json << ','; json << history[i]; }
        json << "],\"root\":" << root_id << ",\"nodes\":[";
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            if (i) json << ',';
            const CertNode& n = nodes[i];
            json << "{\"id\":" << i << ",\"state\":\"" << n.state_key << "\",\"type\":\"" << n.type << "\"";
            if (n.type == "OR") json << ",\"move\":" << n.move << ",\"child\":" << n.child;
            else if (n.type == "AND") {
                json << ",\"children\":[";
                for (std::size_t j = 0; j < n.children.size(); ++j) {
                    if (j) json << ',';
                    json << "{\"move\":" << n.children[j].first << ",\"node\":" << n.children[j].second << '}';
                }
                json << ']';
            }
            json << '}';
        }
        json << "]}";
        return json.str();
    }

    bool valid_structural_state(const State& s) const {
        if (s.moves < 0 || s.moves > MAX_MOVES) return false;
        if (popcount(s.mask) != s.moves) return false;
        if ((s.pos & ~s.mask) != 0) return false;
        // State.pos denotes the side to move. Under alternating turns it must own
        // floor(moves/2) stones; the other side owns ceil(moves/2).
        if (popcount(s.pos) != s.moves / 2) return false;
        if (popcount(s.mask ^ s.pos) != (s.moves + 1) / 2) return false;
        U64 playable_mask = 0;
        for (int c = 0; c < W; ++c) playable_mask |= COL_MASK[c];
        if ((s.mask & ~playable_mask) != 0) return false; // rejects sentinel/unused bits
        for (int c = 0; c < W; ++c) {
            const int h = height_of(s.mask, c);
            const U64 expected = h ? (low_bits(h) << (c * STRIDE)) : 0;
            if ((s.mask & COL_MASK[c]) != expected) return false;
        }
        // A legal game terminates at the first completed five; a state with an existing
        // line therefore cannot be a valid position to solve from.
        if (has_five(s.pos) || has_five(s.mask ^ s.pos)) return false;
        return true;
    }

    bool cache_exact(ExactContext& context, const ExactKey& key, ExactMemoValue value) {
        if (context.memo.size() >= context.max_entries) {
            context.resource_limited = true;
            return false;
        }
        context.memo.emplace(key, value);
        return true;
    }

    ProofEval proof_search(const State& s, bool target_to_move, int remaining_plies,
                           ProofContext& context, int ply) {
        ++context.nodes;
        if (context.timed_out || context.resource_limited) return {0, -1};
        if ((context.nodes & 255ULL) == 0ULL && Clock::now() >= context.deadline) {
            context.timed_out = true;
            return {0, -1};
        }

        auto finish = [&](int status, int best_col, const Canonical& canonical,
                          const ExactKey& key, bool cacheable) -> ProofEval {
            if (cacheable) {
                if (context.memo.size() < context.max_entries) {
                    const int canonical_move = best_col < 0 ? -1 :
                        (canonical.flipped ? W - 1 - best_col : best_col);
                    context.memo.emplace(key, ExactMemoValue{static_cast<int8_t>(status),
                                                             static_cast<int8_t>(canonical_move)});
                } else {
                    context.resource_limited = true;
                }
            }
            return {status, best_col};
        };

        if (s.moves >= MAX_MOVES) return {-1, -1}; // draw cannot be a forced win
        const Canonical canonical = canonicalize(s.pos, s.mask);
        const ExactKey key{canonical.pos, canonical.mask};
        auto found = context.memo.find(key);
        if (found != context.memo.end()) {
            int best = found->second.best;
            if (best >= 0 && canonical.flipped) best = W - 1 - best;
            return {found->second.outcome, best};
        }

        const auto legal = legal_moves(s);
        if (legal.empty()) return finish(-1, -1, canonical, key, true);

        std::vector<int> moves = legal;
        if (target_to_move) {
            const auto target_wins = winning_columns(s.pos, s.mask);
            if (!target_wins.empty()) {
                int best = target_wins.front();
                for (int c : target_wins) if (std::abs(c - 4) < std::abs(best - 4)) best = c;
                return finish(1, best, canonical, key, true);
            }
            const auto opponent_wins = winning_columns(s.mask ^ s.pos, s.mask);
            if (opponent_wins.size() >= 2) return finish(-1, -1, canonical, key, true);
            if (opponent_wins.size() == 1) {
                const int forced_block = opponent_wins.front();
                moves.erase(std::remove_if(moves.begin(), moves.end(),
                    [forced_block](int c) { return c != forced_block; }), moves.end());
            }
        } else {
            // The opponent can take an immediate win, so the root side cannot force one.
            if (!winning_columns(s.pos, s.mask).empty())
                return finish(-1, -1, canonical, key, true);
            // If the target already has two different winning landing columns, a single
            // opponent move cannot occupy both. This is an exact AND-node shortcut.
            const auto target_wins = winning_columns(s.mask ^ s.pos, s.mask);
            if (target_wins.size() >= 2)
                return finish(1, -1, canonical, key, true);
            if (target_wins.size() == 1) {
                // As the opponent's only non-losing reply is to occupy the unique target
                // winning square, all other replies immediately lose to the target.
                const int forced_block = target_wins.front();
                moves.erase(std::remove_if(moves.begin(), moves.end(),
                    [forced_block](int c) { return c != forced_block; }), moves.end());
            }
        }

        // Still inspect immediate proofs/refutations at the horizon; only nonterminal
        // continuations beyond it become unknown.
        if (remaining_plies <= 0) return {0, -1};

        struct Candidate { int col; int target_eval; };
        std::vector<Candidate> ranked;
        ranked.reserve(moves.size());
        for (int c : moves) {
            const U64 bit = drop_bit(s.mask, c);
            if (!bit) continue;
            if (has_five(s.pos | bit)) {
                if (target_to_move) return finish(1, c, canonical, key, true);
                // Current mover is the opponent in this branch: a direct opponent win
                // refutes the root target's forced-win claim.
                return finish(-1, -1, canonical, key, true);
            }
            State child{s.pos ^ s.mask, s.mask | bit, s.moves + 1};
            int target_eval = evaluate(child);
            if (target_to_move) {
                target_eval = -target_eval; // child is opponent to move
                const int target_threats = static_cast<int>(
                    winning_columns(child.mask ^ child.pos, child.mask).size());
                if (target_threats >= 2) target_eval += 1'000'000;
                else if (target_threats == 1) target_eval += 100'000;
            } else {
                const int target_threats = static_cast<int>(
                    winning_columns(child.pos, child.mask).size());
                if (target_threats >= 2) target_eval += 1'000'000;
                else if (target_threats == 1) target_eval += 100'000;
            }
            ranked.push_back({c, target_eval});
        }
        std::stable_sort(ranked.begin(), ranked.end(), [target_to_move](const Candidate& a,
                                                                        const Candidate& b) {
            return target_to_move ? a.target_eval > b.target_eval : a.target_eval < b.target_eval;
        });
        moves.clear();
        for (const Candidate& item : ranked) moves.push_back(item.col);

        bool saw_unknown = false;
        for (int c : moves) {
            const U64 bit = drop_bit(s.mask, c);
            if (!bit) continue;
            State child{s.pos ^ s.mask, s.mask | bit, s.moves + 1};
            ProofEval child_result = proof_search(child, !target_to_move, remaining_plies - 1,
                                                  context, ply + 1);
            if (target_to_move) {
                // OR node: one child that proves a forced target win is sufficient.
                if (child_result.status == 1)
                    return finish(1, c, canonical, key, true);
                if (child_result.status == 0) saw_unknown = true;
            } else {
                // AND node: one opponent reply that refutes the target win is sufficient.
                if (child_result.status == -1)
                    return finish(-1, -1, canonical, key, true);
                if (child_result.status == 0) saw_unknown = true;
            }
            if (context.timed_out || context.resource_limited) return {0, -1};
        }
        if (saw_unknown) return {0, -1};
        return finish(target_to_move ? -1 : 1,
                      target_to_move ? -1 : -1, canonical, key, true);
    }

    static std::uint64_t dfpn_add(std::uint64_t a, std::uint64_t b) noexcept {
        if (a >= DFPN_INF || b >= DFPN_INF || a > DFPN_INF - b) return DFPN_INF;
        return a + b;
    }

    DfpnEval dfpn_load(const State& s, DfpnContext& context, int remaining_plies = -1) const {
        const Canonical canonical = canonicalize(s.pos, s.mask);
        const ExactKey key{canonical.pos, canonical.mask};
        DfpnMemoValue* value = context.memo->find(key);
        if (!value) return {};
        // A frontier is not a terminal game result. If this transposition is now
        // reached with more remaining plies, reopen it before the parent ranks
        // expandable children; otherwise every child may look permanently capped.
        if (value->frontier && remaining_plies > value->frontier_remaining) {
            context.memo->set(key, DfpnMemoValue{1, 1, -1, false, -1});
            return {};
        }
        int best = value->best;
        if (best >= 0 && canonical.flipped) best = W - 1 - best;
        return {value->pn, value->dn, best, value->frontier, value->frontier_remaining};
    }

    DfpnEval dfpn_search(const State& s, bool target_to_move, int remaining_plies,
                         std::uint64_t phi, std::uint64_t delta,
                         DfpnContext& context) {
        ++context.nodes;
        if (context.nodes > context.node_limit) context.resource_limited = true;
        if ((context.nodes & 127ULL) == 0ULL && Clock::now() >= context.deadline)
            context.timed_out = true;
        if (context.timed_out || context.resource_limited) return dfpn_load(s, context);

        const Canonical canonical = canonicalize(s.pos, s.mask);
        const ExactKey key{canonical.pos, canonical.mask};
        DfpnMemoValue* found = context.memo->find(key);
        if (found) {
            DfpnEval saved = dfpn_load(s, context);
            if (saved.pn == 0 || saved.dn == 0) return saved;
            // A frontier is reusable only when the new call has not deepened beyond
            // the remaining horizon at which that node was cut. Otherwise reopen it.
            if (saved.frontier && remaining_plies <= saved.frontier_remaining) return saved;
            if (saved.frontier && remaining_plies > saved.frontier_remaining) {
                context.memo->set(key, DfpnMemoValue{1, 1, -1, false, -1});
            }
        } else {
            if (!context.memo->insert(key, DfpnMemoValue{})) {
                context.resource_limited = true;
                return {};
            }
        }

        auto save = [&](std::uint64_t pn, std::uint64_t dn, int best_col,
                        bool frontier = false) -> DfpnEval {
            int canonical_best = best_col;
            if (canonical_best >= 0 && canonical.flipped) canonical_best = W - 1 - canonical_best;
            const int16_t frontier_remaining = frontier ? static_cast<int16_t>(remaining_plies) : -1;
            context.memo->set(key, DfpnMemoValue{pn, dn, static_cast<int8_t>(canonical_best), frontier, frontier_remaining});
            if (frontier) ++context.frontier_nodes;
            return {pn, dn, best_col, frontier, frontier_remaining};
        };

        // Terminal positions are represented after the last drop, with `pos` holding
        // the new side-to-move stones and `mask ^ pos` holding the previous mover.
        // The previous mover's existing five must be recognized before expanding any
        // further move. `target_to_move` identifies the player who did NOT just move.
        // This is a game-theoretic terminal rule, not a heuristic.
        if (has_five(s.mask ^ s.pos)) {
            const bool previous_mover_was_target = !target_to_move;
            return previous_mover_was_target
                ? save(0, DFPN_INF, -1)
                : save(DFPN_INF, 0, -1);
        }

        // The goal is only "target can force a win". Draws and wins by the opponent
        // disprove it; no heuristic score can alter proof/disproof status.
        if (s.moves >= MAX_MOVES) return save(DFPN_INF, 0, -1);
        std::vector<int> moves = legal_moves(s);
        if (moves.empty()) return save(DFPN_INF, 0, -1);

        if (target_to_move) {
            const auto target_wins = winning_columns(s.pos, s.mask);
            if (!target_wins.empty()) {
                int best = target_wins.front();
                for (int c : target_wins) if (std::abs(c - 4) < std::abs(best - 4)) best = c;
                return save(0, DFPN_INF, best);
            }
            const auto opponent_wins = winning_columns(s.mask ^ s.pos, s.mask);
            if (opponent_wins.size() >= 2) return save(DFPN_INF, 0, -1);
            if (opponent_wins.size() == 1) {
                const int block = opponent_wins.front();
                moves.erase(std::remove_if(moves.begin(), moves.end(),
                    [block](int c) { return c != block; }), moves.end());
            }
        } else {
            // If the opponent can win immediately, a target forced win is impossible.
            if (!winning_columns(s.pos, s.mask).empty()) return save(DFPN_INF, 0, -1);
            const auto target_wins = winning_columns(s.mask ^ s.pos, s.mask);
            if (target_wins.size() >= 2) return save(0, DFPN_INF, -1);
            if (target_wins.size() == 1) {
                // All non-block replies leave a direct target win; only the unique block
                // can refute the target's forced-win claim, so analyze that reply deeply.
                const int block = target_wins.front();
                moves.erase(std::remove_if(moves.begin(), moves.end(),
                    [block](int c) { return c != block; }), moves.end());
            }
        }

        // A horizon node may still be settled by exact tactical rules above, but otherwise
        // remains an unresolved frontier leaf and must never be treated as a refutation.
        if (remaining_plies <= 0) return save(1, 1, -1, true);

        struct Candidate { int col; int lambda_level; int dependency_score; int target_eval; };
        std::vector<Candidate> ranked;
        ranked.reserve(moves.size());
        for (int c : moves) {
            const U64 bit = drop_bit(s.mask, c);
            if (!bit) continue;
            State child{s.pos ^ s.mask, s.mask | bit, s.moves + 1};
            int target_eval = evaluate(child);
            if (target_to_move) target_eval = -target_eval;
            const U64 target_bits = target_to_move ? child.mask ^ child.pos : child.pos;
            const auto target_wins = winning_columns(target_bits, child.mask);
            if (!target_wins.empty()) target_eval += 1'000'000 * static_cast<int>(target_wins.size());
            const GravityFeatures gf = gravity_features(child, !target_to_move);
            // Lambda-style threat tiers are ordering information only. PN/DN arithmetic
            // remains authoritative and every legal move is kept in the proof tree.
            ranked.push_back({c, gf.lambda_level, gf.score, target_eval});
        }
        std::stable_sort(ranked.begin(), ranked.end(), [target_to_move](const Candidate& a,
                                                                         const Candidate& b) {
            if (a.lambda_level != b.lambda_level)
                return target_to_move ? a.lambda_level < b.lambda_level : a.lambda_level > b.lambda_level;
            if (a.dependency_score != b.dependency_score)
                return target_to_move ? a.dependency_score > b.dependency_score : a.dependency_score < b.dependency_score;
            return target_to_move ? a.target_eval > b.target_eval : a.target_eval < b.target_eval;
        });
        moves.clear();
        for (const Candidate& candidate : ranked) moves.push_back(candidate.col);

        for (;;) {
            if (context.timed_out || context.resource_limited) return dfpn_load(s, context);
            std::uint64_t pn = target_to_move ? DFPN_INF : 0;
            std::uint64_t dn = target_to_move ? 0 : DFPN_INF;
            int proven_move = -1;
            int selected_index = -1;
            std::uint64_t best_metric = DFPN_INF;
            std::uint64_t second_metric = DFPN_INF;
            std::vector<DfpnEval> child_values;
            child_values.reserve(moves.size());

            for (std::size_t i = 0; i < moves.size(); ++i) {
                const int c = moves[i];
                const U64 bit = drop_bit(s.mask, c);
                if (!bit) { child_values.push_back({DFPN_INF, 0, -1, false}); continue; }
                State child{s.pos ^ s.mask, s.mask | bit, s.moves + 1};
                DfpnEval value = dfpn_load(child, context, remaining_plies - 1);
                child_values.push_back(value);
                if (target_to_move) {
                    pn = std::min(pn, value.pn);
                    dn = dfpn_add(dn, value.dn);
                    if (value.pn == 0 && proven_move < 0) proven_move = c;
                } else {
                    pn = dfpn_add(pn, value.pn);
                    dn = std::min(dn, value.dn);
                }
            }

            // A zero proof number means the goal is proved; zero disproof number means
            // every OR branch failed or at least one AND branch failed, respectively.
            if (pn == 0) return save(0, dn, proven_move >= 0 ? proven_move : -1);
            if (dn == 0) return save(pn, 0, -1);
            save(pn, dn, -1);
            if (pn >= phi || dn >= delta) return dfpn_load(s, context);

            // Select the most-proving *expandable* child. Frontier children cannot be
            // deepened under this call's ply cap, so we explore remaining alternatives.
            selected_index = -1;
            best_metric = DFPN_INF;
            second_metric = DFPN_INF;
            for (std::size_t i = 0; i < child_values.size(); ++i) {
                const DfpnEval& value = child_values[i];
                // Solved children cannot make progress when revisited. In a mixed
                // solved/frontier node, skip both exact results and depth-capped leaves.
                if (value.frontier || value.pn == 0 || value.dn == 0) continue;
                const std::uint64_t metric = target_to_move ? value.pn : value.dn;
                if (selected_index < 0 || metric < best_metric) {
                    if (selected_index >= 0) second_metric = std::min(second_metric, best_metric);
                    best_metric = metric;
                    selected_index = static_cast<int>(i);
                } else {
                    second_metric = std::min(second_metric, metric);
                }
            }
            if (selected_index < 0) return save(pn, dn, -1, true);

            // DFPN threshold propagation. At OR nodes, bound the chosen child's proof
            // by the runner-up and disproof by the remaining siblings' disproof sum.
            // At AND nodes, the dual formulas apply to proof sum and disproof minimum.
            std::uint64_t child_phi = phi;
            std::uint64_t child_delta = delta;
            if (target_to_move) {
                child_phi = std::min(phi, dfpn_add(second_metric, 1));
                std::uint64_t other_dn = 0;
                for (std::size_t i = 0; i < child_values.size(); ++i)
                    if (static_cast<int>(i) != selected_index)
                        other_dn = dfpn_add(other_dn, child_values[i].dn);
                child_delta = delta > other_dn ? delta - other_dn : 1;
            } else {
                std::uint64_t other_pn = 0;
                for (std::size_t i = 0; i < child_values.size(); ++i)
                    if (static_cast<int>(i) != selected_index)
                        other_pn = dfpn_add(other_pn, child_values[i].pn);
                child_phi = phi > other_pn ? phi - other_pn : 1;
                child_delta = std::min(delta, dfpn_add(second_metric, 1));
            }
            const int chosen_col = moves[static_cast<std::size_t>(selected_index)];
            const U64 chosen_bit = drop_bit(s.mask, chosen_col);
            State chosen_child{s.pos ^ s.mask, s.mask | chosen_bit, s.moves + 1};
            (void)dfpn_search(chosen_child, !target_to_move, remaining_plies - 1,
                              std::max<std::uint64_t>(1, child_phi),
                              std::max<std::uint64_t>(1, child_delta), context);
        }
    }

    ExactEval exact_wdl(const State& s, ExactContext& context) {
        ++context.nodes;
        if (context.timed_out || context.resource_limited) return {2, false};
        if ((context.nodes & 255ULL) == 0ULL && Clock::now() >= context.deadline) {
            context.timed_out = true;
            return {2, false};
        }
        if (s.moves >= MAX_MOVES) return {0, true};

        const Canonical canonical = canonicalize(s.pos, s.mask);
        const ExactKey key{canonical.pos, canonical.mask};
        auto found = context.memo.find(key);
        if (found != context.memo.end()) return {found->second.outcome, true};
        if (context.memo.size() >= context.max_entries) {
            context.resource_limited = true;
            return {2, false};
        }

        int first_legal = -1;
        int draw_move = -1;
        // A direct win is a proof regardless of unexplored branches.
        for (int c : CENTER_ORDER) {
            const U64 move = drop_bit(s.mask, c);
            if (!move) continue;
            if (first_legal < 0) first_legal = c;
            if (has_five(s.pos | move)) {
                const int canonical_move = canonical.flipped ? W - 1 - c : c;
                if (!cache_exact(context, key, ExactMemoValue{1, static_cast<int8_t>(canonical_move)}))
                    return {2, false};
                return {1, true};
            }
        }
        if (first_legal < 0) {
            if (!cache_exact(context, key, ExactMemoValue{0, -1})) return {2, false};
            return {0, true};
        }

        // Exact tactical reduction: with two distinct opponent winning columns, one drop
        // cannot block both. With exactly one, every non-block move loses immediately, so
        // only the forced block needs an exact continuation search.
        const auto enemy_wins = winning_columns(s.mask ^ s.pos, s.mask);
        if (enemy_wins.size() >= 2) {
            const int canonical_move = canonical.flipped ? W - 1 - first_legal : first_legal;
            if (!cache_exact(context, key, ExactMemoValue{-1, static_cast<int8_t>(canonical_move)}))
                return {2, false};
            return {-1, true};
        }
        if (enemy_wins.size() == 1) {
            const int block_col = enemy_wins.front();
            const U64 block = drop_bit(s.mask, block_col);
            if (!block) return {2, false}; // malformed state guard; valid gravity states cannot reach this
            State child{s.pos ^ s.mask, s.mask | block, s.moves + 1};
            const ExactEval child_result = exact_wdl(child, context);
            if (!child_result.complete) return {2, false};
            const int outcome = -child_result.outcome;
            const int canonical_move = canonical.flipped ? W - 1 - block_col : block_col;
            if (!cache_exact(context, key, ExactMemoValue{static_cast<int8_t>(outcome),
                                                           static_cast<int8_t>(canonical_move)}))
                return {2, false};
            return {outcome, true};
        }

        for (int c : CENTER_ORDER) {
            const U64 move = drop_bit(s.mask, c);
            if (!move) continue;
            State child{s.pos ^ s.mask, s.mask | move, s.moves + 1};
            const ExactEval child_result = exact_wdl(child, context);
            if (!child_result.complete) return {2, false};
            // If the child is a forced loss for the opponent, this move proves a win.
            if (child_result.outcome == -1) {
                const int canonical_move = canonical.flipped ? W - 1 - c : c;
                if (!cache_exact(context, key, ExactMemoValue{1, static_cast<int8_t>(canonical_move)}))
                    return {2, false};
                return {1, true};
            }
            if (child_result.outcome == 0 && draw_move < 0) draw_move = c;
        }

        const int outcome = draw_move >= 0 ? 0 : -1;
        const int chosen_move = draw_move >= 0 ? draw_move : first_legal;
        const int canonical_move = canonical.flipped ? W - 1 - chosen_move : chosen_move;
        if (!cache_exact(context, key, ExactMemoValue{static_cast<int8_t>(outcome),
                                                      static_cast<int8_t>(canonical_move)}))
            return {2, false};
        return {outcome, true};
    }

    void reset_counters() {
        nodes_ = qnodes_ = tt_hits_ = cutoffs_ = 0;
        for (auto& h : history_) h.fill(0);
        for (auto& k : killers_) k.fill(-1);
    }
    double elapsed_seconds() const {
        return std::chrono::duration<double>(Clock::now() - start_).count();
    }
    void check_time() {
        if (!stopped_ && ((nodes_ + qnodes_) & 2047ULL) == 0ULL && Clock::now() >= deadline_) {
            stopped_ = true;
            timed_out_ = true;
        }
    }
    int legal_count(U64 mask) const {
        int n = 0;
        for (int c = 0; c < W; ++c) if (drop_bit(mask, c)) ++n;
        return n;
    }

    struct Canonical {
        U64 pos;
        U64 mask;
        bool flipped;
    };
    Canonical canonicalize(U64 pos, U64 mask) const {
        U64 mp = mirror_bits(pos);
        U64 mm = mirror_bits(mask);
        bool flip = (mm < mask) || (mm == mask && mp < pos);
        return flip ? Canonical{mp, mm, true} : Canonical{pos, mask, false};
    }
    static std::uint64_t mix64(U64 x) { return hash128(x); }
    std::size_t index_for(U64 pos, U64 mask) const {
        const std::uint64_t a = hash128(mask);
        const std::uint64_t b = hash128(pos);
        return static_cast<std::size_t>(mix64low(a ^ (b + 0x517cc1b727220a95ULL))) & tt_mask_;
    }
    TTEntry* lookup(U64 pos, U64 mask, bool& flipped) {
        if (!use_tt_) { flipped = false; return nullptr; }
        Canonical c = canonicalize(pos, mask);
        flipped = c.flipped;
        TTEntry& e = tt_[index_for(c.pos, c.mask)];
        if (e.bound != 0 && e.mask == c.mask && e.pos == c.pos) return &e;
        return nullptr;
    }
    void store(U64 pos, U64 mask, int depth, int score, uint8_t bound, int best_col) {
        if (!use_tt_) return;
        Canonical c = canonicalize(pos, mask);
        const int canonical_move = best_col < 0 ? -1 : (c.flipped ? W - 1 - best_col : best_col);
        TTEntry& e = tt_[index_for(c.pos, c.mask)];
        bool same = e.bound != 0 && e.mask == c.mask && e.pos == c.pos;
        if (same && e.depth > depth) return;
        if (same && e.depth == depth && e.bound == 1 && bound != 1) return;
        if (!same && e.bound != 0 && e.generation == generation_ && e.depth > depth) return;
        e.mask = c.mask;
        e.pos = c.pos;
        e.score = score;
        e.depth = static_cast<int16_t>(depth);
        e.bound = bound;
        e.best = static_cast<int8_t>(canonical_move);
        e.generation = generation_;
    }
    int tt_move_for(TTEntry* e, bool flipped) const {
        if (!e || e->best < 0 || e->best >= W) return -1;
        return flipped ? W - 1 - e->best : e->best;
    }
    int player_index(const State& s) const { return (s.moves & 1); }

    // The evaluation explicitly discounts empty winning cells that are buried under gravity.
    // This is a heuristic only; forced tactics are handled separately by search/quiescence.
    int evaluate(const State& s) const {
        U64 own = s.pos;
        U64 opp = s.mask ^ s.pos;
        std::array<int, W> heights{};
        for (int c = 0; c < W; ++c) heights[c] = height_of(s.mask, c);
        int score = 0;
        for (const Window& w : WINDOWS) {
            int a = popcount(own & w.bits);
            int b = popcount(opp & w.bits);
            if (a && b) continue;
            int n = a ? a : b;
            if (!n) continue;
            double access = 0.0;
            int min_depth = H + 1;
            for (int i = 0; i < K; ++i) {
                U64 bit = U64(1) << (w.cols[i] * STRIDE + w.rows[i]);
                if (s.mask & bit) continue;
                int d = w.rows[i] - heights[w.cols[i]];
                if (d < 0) continue; // should be impossible for a legal gravity state
                min_depth = std::min(min_depth, d);
                // d=0 is available now; buried targets get steeply discounted.
                double weight = (d == 0) ? 1.0 : (0.32 / (d + 0.25));
                access += weight;
            }
            static const int base[6] = {0, 2, 10, 55, 650, 0};
            double value = base[n];
            if (n == 4) {
                // Four pieces are genuinely dangerous only when the fifth cell is playable now.
                value = (min_depth == 0) ? 9000.0 : (260.0 / ((min_depth + 1.0) * (min_depth + 1.0)));
            } else if (n == 3) {
                value *= 0.5 + 0.5 * std::min(1.0, access / 2.0);
            } else if (n == 2) {
                value *= 0.75 + 0.25 * std::min(1.0, access / 3.0);
            } else {
                value *= 0.8 + 0.2 * std::min(1.0, access / 4.0);
            }
            // Slight preference for potential windows nearer the center, while preserving symmetry.
            int min_col = W, max_col = -1;
            for (int i = 0; i < K; ++i) {
                min_col = std::min(min_col, w.cols[i]);
                max_col = std::max(max_col, w.cols[i]);
            }
            double center_bonus = 1.0 + 0.025 * (4 - std::abs((min_col + max_col) - 8));
            value *= center_bonus;
            score += (a ? 1 : -1) * static_cast<int>(value);
        }
        // Small, color-symmetric center-occupancy bonus. The old version tested whether
        // an occupied side owned the *next empty landing bit*, which is impossible and
        // unintentionally penalized every legal column. Score actual stones instead.
        for (int c = 0; c < W; ++c) {
            const int center_weight = 4 - std::abs(c - 4); // 0..4, mirror symmetric
            if (center_weight == 0) continue;
            for (int r = 0; r < H; ++r) {
                const U64 bit = U64(1) << (c * STRIDE + r);
                if (own & bit) score += 2 * center_weight;
                else if (opp & bit) score -= 2 * center_weight;
            }
        }
        return score;
    }

    std::vector<int> ordered_moves(const State& s, int tt_move, int ply = 0) {
        struct Item { int c; int score; };
        std::vector<Item> items;
        items.reserve(W);
        U64 opp = s.mask ^ s.pos;
        for (int c = 0; c < W; ++c) {
            U64 m = drop_bit(s.mask, c);
            if (!m) continue;
            int sc = history_[player_index(s)][c];
            sc += (4 - std::abs(c - 4)) * 7;
            const bool immediate_win = has_five(s.pos | m);
            if (c == tt_move) sc += 2'000'000;
            if (immediate_win) sc += 8'000'000; // a terminal win outranks TT/history ordering
            // Gravity-specific trap detection: only the played column's next landing cell
            // changes height. If the opponent can win by dropping there after this move,
            // search it last. A move that wins now is terminal, so no reply can exploit it.
            if (!immediate_win) {
                const U64 next_mask = s.mask | m;
                const U64 opponent_next = drop_bit(next_mask, c);
                const bool opens_opponent_win = opponent_next && has_five(opp | opponent_next);
                if (opens_opponent_win) sc -= 4'000'000;
            }
            if (ply < static_cast<int>(killers_.size())) {
                if (killers_[ply][0] == c) sc += 30'000;
                else if (killers_[ply][1] == c) sc += 20'000;
            }
            items.push_back({c, sc});
        }
        std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
            return a.score > b.score;
        });
        std::vector<int> result;
        result.reserve(items.size());
        for (auto x : items) result.push_back(x.c);
        return result;
    }

    int quiescence(const State& s, int alpha, int beta, int qply) {
        ++qnodes_;
        check_time();
        if (stopped_) return evaluate(s);
        if (s.moves >= MAX_MOVES) return 0;
        U64 opp = s.mask ^ s.pos;
        auto own_wins = winning_columns(s.pos, s.mask);
        if (!own_wins.empty()) return WIN_SCORE - (s.moves + 1);
        auto enemy_wins = winning_columns(opp, s.mask);
        if (enemy_wins.size() >= 2) return -WIN_SCORE + (s.moves + 2);
        // Forced single-column defense must be followed even if the optional extension budget is spent.
        if (enemy_wins.size() == 1) {
            int block_col = enemy_wins.front();
            U64 move = drop_bit(s.mask, block_col);
            if (!move) return -WIN_SCORE + (s.moves + 2);
            State child{ s.pos ^ s.mask, s.mask | move, s.moves + 1 };
            return -quiescence(child, -beta, -alpha, qply + 1);
        }

        // Stand-pat is only an evaluation baseline, never a proof. Extend forcing moves that
        // create at least one immediately playable winning drop, provided they don't allow the
        // opponent to win immediately. This is a gravity-aware threat extension (bounded to 5
        // optional plies); direct wins and forced blocks above are not subject to this cap.
        int best = evaluate(s);
        if (best >= beta) return best;
        if (best > alpha) alpha = best;
        if (qply >= 5) return best;
        std::vector<int> candidates = ordered_moves(s, -1, std::min(qply, 63));
        for (int c : candidates) {
            U64 move = drop_bit(s.mask, c);
            if (!move || has_five(s.pos | move)) continue;
            State child{ s.pos ^ s.mask, s.mask | move, s.moves + 1 };
            U64 attacker_after = child.mask ^ child.pos;
            // If the new side to move can win immediately, this was not a forcing attack.
            if (!winning_columns(child.pos, child.mask).empty()) continue;
            // Only extend a move that gives the mover at least one direct winning target.
            if (winning_columns(attacker_after, child.mask).empty()) continue;
            int score = -quiescence(child, -beta, -alpha, qply + 1);
            if (stopped_) return best;
            if (score > best) best = score;
            if (score > alpha) alpha = score;
            if (alpha >= beta) {
                ++cutoffs_;
                break;
            }
        }
        return best;
    }

    int negamax(const State& s, int depth, int alpha, int beta, int ply) {
        ++nodes_;
        check_time();
        if (stopped_) return evaluate(s);
        if (s.moves >= MAX_MOVES) return 0;

        // Resolve immediate wins and double threats before consulting heuristic TT data.
        // These outcomes are independent of search depth and therefore safe to treat as proofs.
        auto own_wins = winning_columns(s.pos, s.mask);
        if (!own_wins.empty()) {
            int best = own_wins.front();
            for (int c : own_wins) if (std::abs(c - 4) < std::abs(best - 4)) best = c;
            const int score = WIN_SCORE - (s.moves + 1);
            store(s.pos, s.mask, std::max(depth, 0), score, 1, best);
            return score;
        }
        const auto enemy_wins = winning_columns(s.mask ^ s.pos, s.mask);
        if (enemy_wins.size() >= 2) {
            const std::vector<int> legal = legal_moves(s);
            const int best = legal.empty() ? -1 : legal.front();
            const int score = -WIN_SCORE + (s.moves + 2);
            store(s.pos, s.mask, std::max(depth, 0), score, 1, best);
            return score;
        }

        // Preserve the caller's full window: a TT entry may narrow alpha/beta, but
        // bound classification must still be correct for the original search window.
        const int alpha_orig = alpha;
        const int beta_orig = beta;
        bool flipped = false;
        TTEntry* entry = lookup(s.pos, s.mask, flipped);
        int tt_move = tt_move_for(entry, flipped);
        if (entry) {
            ++tt_hits_;
            if (entry->depth >= depth) {
                if (entry->bound == 1) return entry->score;
                if (entry->bound == 2) alpha = std::max(alpha, entry->score);
                else if (entry->bound == 3) beta = std::min(beta, entry->score);
                if (alpha >= beta) return entry->score;
            }
        }

        if (depth <= 0) return quiescence(s, alpha, beta, 0);

        std::vector<int> moves = ordered_moves(s, tt_move, ply);
        if (enemy_wins.size() == 1) {
            const int forced_block = enemy_wins.front();
            moves.erase(std::remove_if(moves.begin(), moves.end(),
                                       [forced_block](int c) { return c != forced_block; }), moves.end());
        }
        if (moves.empty()) return 0;
        int best_score = -INF;
        int best_move = moves.front();
        int side = player_index(s);
        int searched_moves = 0;
        for (int c : moves) {
            U64 move = drop_bit(s.mask, c);
            if (has_five(s.pos | move)) {
                int score = WIN_SCORE - (s.moves + 1);
                if (score > best_score) { best_score = score; best_move = c; }
                alpha = std::max(alpha, score);
                ++searched_moves;
                if (alpha >= beta) break;
                continue;
            }
            State child{ s.pos ^ s.mask, s.mask | move, s.moves + 1 };
            int score;
            if (use_pvs_ && searched_moves > 0 && alpha + 1 < beta) {
                // PVS: later moves first get a zero-window scout search. A full-window
                // re-search is mandatory whenever the scout improves alpha without cutting off.
                score = -negamax(child, depth - 1, -alpha - 1, -alpha, ply + 1);
                if (!stopped_ && score > alpha && score < beta)
                    score = -negamax(child, depth - 1, -beta, -alpha, ply + 1);
            } else {
                score = -negamax(child, depth - 1, -beta, -alpha, ply + 1);
            }
            if (stopped_) return best_score == -INF ? evaluate(s) : best_score;
            if (score > best_score) { best_score = score; best_move = c; }
            if (score > alpha) alpha = score;
            ++searched_moves;
            if (alpha >= beta) {
                ++cutoffs_;
                if (!has_five(s.pos | move) && ply < static_cast<int>(killers_.size())) {
                    if (killers_[ply][0] != c) {
                        killers_[ply][1] = killers_[ply][0];
                        killers_[ply][0] = c;
                    }
                    history_[side][c] = std::min(1'000'000, history_[side][c] + depth * depth);
                }
                break;
            }
        }
        uint8_t bound = 1;
        if (best_score <= alpha_orig) bound = 3;
        else if (best_score >= beta_orig) bound = 2;
        store(s.pos, s.mask, depth, best_score, bound, best_move);
        return best_score;
    }

    int search_root(const State& root, int depth, int alpha, int beta, int& best_move, bool& complete) {
        complete = false;
        if (root.moves >= MAX_MOVES) { best_move = -1; complete = true; return 0; }
        const auto root_wins = winning_columns(root.pos, root.mask);
        if (!root_wins.empty()) {
            best_move = root_wins.front();
            for (int c : root_wins) if (std::abs(c - 4) < std::abs(best_move - 4)) best_move = c;
            const int score = WIN_SCORE - (root.moves + 1);
            store(root.pos, root.mask, depth, score, 1, best_move);
            complete = true;
            return score;
        }
        const auto enemy_wins = winning_columns(root.mask ^ root.pos, root.mask);
        std::vector<int> root_legal = legal_moves(root);
        if (root_legal.empty()) { best_move = -1; complete = true; return 0; }
        if (enemy_wins.size() >= 2) {
            best_move = root_legal.front();
            const int score = -WIN_SCORE + (root.moves + 2);
            store(root.pos, root.mask, depth, score, 1, best_move);
            complete = true;
            return score;
        }
        const int alpha_orig = alpha;
        const int beta_orig = beta;
        bool flipped = false;
        TTEntry* entry = lookup(root.pos, root.mask, flipped);
        int tt_move = tt_move_for(entry, flipped);
        std::vector<int> moves = ordered_moves(root, tt_move, 0);
        if (enemy_wins.size() == 1) {
            const int forced_block = enemy_wins.front();
            moves.erase(std::remove_if(moves.begin(), moves.end(),
                                       [forced_block](int c) { return c != forced_block; }), moves.end());
        }
        if (moves.empty()) { best_move = -1; complete = true; return 0; }
        int best_score = -INF;
        int local_best = moves.front();
        int searched_moves = 0;
        for (int c : moves) {
            check_time();
            if (stopped_) return best_score == -INF ? evaluate(root) : best_score;
            U64 move = drop_bit(root.mask, c);
            int score;
            if (has_five(root.pos | move)) {
                score = WIN_SCORE - (root.moves + 1);
            } else {
                State child{ root.pos ^ root.mask, root.mask | move, root.moves + 1 };
                if (use_pvs_ && searched_moves > 0 && alpha + 1 < beta) {
                    score = -negamax(child, depth - 1, -alpha - 1, -alpha, 1);
                    if (!stopped_ && score > alpha && score < beta)
                        score = -negamax(child, depth - 1, -beta, -alpha, 1);
                } else {
                    score = -negamax(child, depth - 1, -beta, -alpha, 1);
                }
            }
            if (stopped_) return best_score == -INF ? evaluate(root) : best_score;
            if (score > best_score) { best_score = score; local_best = c; }
            if (score > alpha) alpha = score;
            ++searched_moves;
            if (alpha >= beta) { ++cutoffs_; break; }
        }
        uint8_t bound = 1;
        if (best_score <= alpha_orig) bound = 3;
        else if (best_score >= beta_orig) bound = 2;
        store(root.pos, root.mask, depth, best_score, bound, local_best);
        best_move = local_best;
        complete = true;
        return best_score;
    }

    std::vector<int> principal_variation(State s, int max_len) {
        std::vector<int> pv;
        for (int i = 0; i < max_len && s.moves < MAX_MOVES; ++i) {
            bool flipped = false;
            TTEntry* e = lookup(s.pos, s.mask, flipped);
            int c = tt_move_for(e, flipped);
            if (c < 0 || c >= W) break;
            U64 move = drop_bit(s.mask, c);
            if (!move) break;
            pv.push_back(c);
            if (has_five(s.pos | move)) break;
            s = State{ s.pos ^ s.mask, s.mask | move, s.moves + 1 };
        }
        return pv;
    }
};

bool Engine::watcher_memory_self_test(std::ostream& out) {
    DfpnMemoTable table;
    table.configure(64);
    std::vector<ExactKey> solved_keys;
    for (std::uint64_t i = 0; i < 58; ++i) {
        ExactKey key{U64(i + 1), U64(i + 257) << 1};
        DfpnMemoValue value{};
        if (i % 6 == 0) { value.pn = 0; value.dn = DFPN_INF; solved_keys.push_back(key); }
        else if (i % 7 == 0) { value.pn = DFPN_INF; value.dn = 0; solved_keys.push_back(key); }
        else {
            value.pn = 200 + i;
            value.dn = 400 + i;
            value.frontier = (i % 2) == 0;
            value.frontier_remaining = value.frontier ? 2 : -1;
        }
        if (!table.insert(key, value)) return false;
    }
    const std::size_t before = table.size();
    const std::size_t after = table.compact();
    bool all_solved_retained = true;
    for (const ExactKey& key : solved_keys) {
        const DfpnMemoValue* value = table.find(key);
        if (!value || (value->pn != 0 && value->dn != 0)) all_solved_retained = false;
    }
    const bool bounded = after < before && after <= 64 && after >= solved_keys.size();
    out << (bounded && all_solved_retained ? "PASS" : "FAIL")
        << " DFPN cache compaction preserves every solved proof/refutation while trimming unresolved hints"
        << " (before=" << before << ", after=" << after << ", solved=" << solved_keys.size() << ")\n";
    const std::string checkpoint_path = "gpc-dfpn-capacity-upgrade-selftest.bin";
    std::remove(checkpoint_path.c_str());
    DfpnMemoTable legacy;
    legacy.configure(64);
    std::vector<std::pair<ExactKey, DfpnMemoValue>> records;
    bool migration_ok = true;
    for (std::uint64_t i = 1; i <= 40; ++i) {
        const ExactKey key{U64(i), U64(i) | (U64(1) << 100)};
        DfpnMemoValue value{};
        value.pn = i + 1;
        value.dn = i + 101;
        value.best = static_cast<std::int8_t>(i % W);
        value.frontier = false;
        value.frontier_remaining = -1;
        if (!legacy.insert(key, value)) {
            migration_ok = false;
            break;
        }
        records.emplace_back(key, value);
    }
    const State empty_root{};
    migration_ok = migration_ok && records.size() == 40 &&
        legacy.save_checkpoint(checkpoint_path, empty_root, 1);

    DfpnMemoTable expanded;
    expanded.configure(128);
    migration_ok = migration_ok &&
        expanded.load_checkpoint(checkpoint_path, empty_root, 1, 128) &&
        expanded.size() == records.size();
    if (migration_ok) {
        for (const auto& item : records) {
            const DfpnMemoValue* loaded = expanded.find(item.first);
            if (!loaded || loaded->pn != item.second.pn ||
                loaded->dn != item.second.dn || loaded->best != item.second.best) {
                migration_ok = false;
                break;
            }
        }
    }
    std::remove(checkpoint_path.c_str());
    out << (migration_ok ? "PASS" : "FAIL")
        << " DFPN checkpoint migration preserves 40 entries from capacity 64 into capacity 128\n";
    return bounded && all_solved_retained && migration_ok;
}

void print_board(const State& s) {
    U64 opp = s.mask ^ s.pos;
    // State.pos is always relative to side-to-move; display colors by absolute ply parity for normal game
    // For CLI game we maintain the absolute board separately; this printer is mainly for analyzed positions.
    std::cout << "  a b c d e f g h i\n";
    for (int r = H - 1; r >= 0; --r) {
        std::cout << r + 1 << ' ';
        for (int c = 0; c < W; ++c) {
            U64 bit = U64(1) << (c * STRIDE + r);
            char ch = (s.pos & bit) ? 'X' : ((opp & bit) ? 'O' : '.');
            std::cout << ch << (c == W - 1 ? '\n' : ' ');
        }
    }
}

bool apply_move(State& s, int c) {
    U64 move = drop_bit(s.mask, c);
    if (!move) return false;
    s = State{ s.pos ^ s.mask, s.mask | move, s.moves + 1 };
    return true;
}

bool parse_col(const std::string& token, int& col) {
    if (token.size() != 1) return false;
    char c = token[0];
    if (c >= 'A' && c <= 'I') c = char(c - 'A' + 'a');
    if (c < 'a' || c > 'i') return false;
    col = c - 'a';
    return true;
}

void print_result(const SearchResult& r) {
    std::cout << "best_move=" << (r.best_col >= 0 ? char('a' + r.best_col) : '-')
              << " score=" << r.score
              << " depth=" << r.depth
              << " nodes=" << r.nodes
              << " qnodes=" << r.qnodes
              << " tt_hits=" << r.tt_hits
              << " cutoffs=" << r.cutoffs
              << " seconds=" << std::fixed << std::setprecision(3) << r.seconds
              << " nps=" << (r.seconds > 0 ? static_cast<std::uint64_t>(static_cast<double>(r.nodes + r.qnodes) / r.seconds) : 0)
              << " timed_out=" << (r.timed_out ? "yes" : "no");
    if (r.exact_wdl) {
        const char* outcome = r.exact_outcome == 1 ? "win" : (r.exact_outcome == 0 ? "draw" : "loss");
        std::cout << " status=exact_for_this_position outcome=" << outcome;
    } else if (r.score >= WIN_SCORE - MAX_MOVES) {
        std::cout << " status=forced_win_found_not_full_game_solved";
    } else if (r.score <= -WIN_SCORE + MAX_MOVES) {
        std::cout << " status=forced_loss_found_not_full_game_solved";
    } else {
        std::cout << " status=heuristic_unresolved";
    }
    std::cout << "\n";
    if (!r.pv.empty()) {
        std::cout << "pv=";
        for (std::size_t i = 0; i < r.pv.size(); ++i) {
            if (i) std::cout << ' ';
            std::cout << char('a' + r.pv[i]);
        }
        std::cout << "\n";
    }
}

void print_exact_result(const ExactResult& r) {
    std::cout << "exact_eligible=" << (r.eligible ? "yes" : "no")
              << " solved=" << (r.solved ? "yes" : "no")
              << " nodes=" << r.nodes
              << " seconds=" << std::fixed << std::setprecision(3) << r.seconds;
    if (r.invalid_position) std::cout << " status=invalid_position";
    else if (!r.eligible) std::cout << " status=outside_empty_cell_limit";
    else if (r.solved) {
        const char* outcome = r.outcome == 1 ? "win" : (r.outcome == 0 ? "draw" : "loss");
        std::cout << " outcome=" << outcome << " status=exact_for_this_position";
        if (r.best_col >= 0) std::cout << " best_move=" << char('a' + r.best_col);
    } else if (r.timed_out) std::cout << " status=timeout_unresolved";
    else if (r.resource_limited) std::cout << " status=memory_state_cap_unresolved";
    else std::cout << " status=unresolved";
    std::cout << "\n";
}

void print_proof_result(const ProofResult& r) {
    std::cout << "forced_win_proof=";
    if (r.status == 1) std::cout << "proven";
    else if (r.status == -1) std::cout << "refuted";
    else std::cout << "unresolved";
    std::cout << " nodes=" << r.nodes
              << " seconds=" << std::fixed << std::setprecision(3) << r.seconds
              << " timed_out=" << (r.timed_out ? "yes" : "no")
              << " resource_limited=" << (r.resource_limited ? "yes" : "no");
    if (r.invalid_position) std::cout << " status=invalid_position";
    if (r.best_col >= 0) std::cout << " winning_move=" << char('a' + r.best_col);
    if (r.status == -1) std::cout << " note=no_forced_win_proven_not_a_WDL_result";
    std::cout << "\n";
}

void print_dfpn_result(const DfpnResult& r) {
    std::cout << "dfpn_forced_win_proof=";
    if (r.status == 1) std::cout << "proven";
    else if (r.status == -1) std::cout << "refuted";
    else std::cout << "unresolved";
    std::cout << " proof_number=" << r.proof_number
              << " disproof_number=" << r.disproof_number
              << " nodes=" << r.nodes
              << " seconds=" << std::fixed << std::setprecision(3) << r.seconds
              << " timed_out=" << (r.timed_out ? "yes" : "no")
              << " resource_limited=" << (r.resource_limited ? "yes" : "no");
    if (r.invalid_position) std::cout << " status=invalid_position";
    if (r.best_col >= 0) std::cout << " winning_move=" << char('a' + r.best_col);
    if (r.status == -1) std::cout << " note=no_forced_win_proven_not_a_WDL_result";
    std::cout << "\n";
}

bool selftest() {
    Engine engine(16);
    int passed = 0, failed = 0;
    auto check = [&](bool ok, const char* name) {
        std::cout << (ok ? "PASS " : "FAIL ") << name << "\n";
        ok ? ++passed : ++failed;
    };
    check(WINDOWS.size() == 68, "exactly 68 five-cell winning windows on 9x6 board");
    check(drop_bit(0, 0) == BOTTOM[0], "first drop lands at bottom of an empty column");
    U64 full_col = COL_MASK[0];
    check(drop_bit(full_col, 0) == 0, "full column rejects extra drop");
    check(!has_five(0), "empty board has no winning line");
    // Four horizontal, vertical and diagonal patterns.
    auto bits_at = [](std::initializer_list<std::pair<int,int>> cells) {
        U64 b = 0;
        for (auto [c,r] : cells) b |= U64(1) << (c * STRIDE + r);
        return b;
    };
    check(has_five(bits_at({{0,0},{1,0},{2,0},{3,0},{4,0}})), "detect horizontal five");
    check(has_five(bits_at({{2,0},{2,1},{2,2},{2,3},{2,4}})), "detect vertical five");
    check(has_five(bits_at({{0,0},{1,1},{2,2},{3,3},{4,4}})), "detect rising diagonal five");
    check(has_five(bits_at({{0,4},{1,3},{2,2},{3,1},{4,0}})), "detect falling diagonal five");
    check(has_five(bits_at({{0,0},{1,0},{2,0},{3,0},{4,0},{5,0}})), "five-or-more is a win");
    U64 playable_bits = 0;
    for (U64 cm : COL_MASK) playable_bits |= cm;
    std::mt19937_64 geometry_rng(0xFA11C0DEULL);
    bool win_detector_matches_windows = true;
    for (int sample = 0; sample < 10000; ++sample) {
        U64 bits = geometry_rng() & playable_bits;
        bool reference_win = false;
        for (const Window& w : WINDOWS) {
            if ((bits & w.bits) == w.bits) { reference_win = true; break; }
        }
        if (has_five(bits) != reference_win) { win_detector_matches_windows = false; break; }
    }
    check(win_detector_matches_windows,
          "bitboard win detection matches all 68 windows on 10,000 randomized boards");
    bool mirror_involution = true;
    for (int sample = 0; sample < 1000; ++sample) {
        U64 bits = geometry_rng() & playable_bits;
        if (mirror_bits(mirror_bits(bits)) != bits) { mirror_involution = false; break; }
    }
    check(mirror_involution, "horizontal mirroring is an exact involution on random bitboards");
    // Reachable tactical current-player win: X has a-d; e is the winning drop.
    State win_now{};
    for (const char* token : {"a", "i", "b", "i", "c", "h", "d", "h"}) {
        int c = -1;
        parse_col(token, c);
        apply_move(win_now, c);
    }
    auto wins = engine.winning_columns(win_now.pos, win_now.mask);
    check(std::find(wins.begin(), wins.end(), 4) != wins.end(), "recognize immediate winning drop in reachable state");
    // Reachable defensive case: X owns a-d and O must block e on move 8.
    State must_block{};
    for (const char* token : {"a", "i", "b", "i", "c", "h", "d"}) {
        int c = -1;
        parse_col(token, c);
        apply_move(must_block, c);
    }
    auto enemy_wins = engine.winning_columns(must_block.mask ^ must_block.pos, must_block.mask);
    check(std::find(enemy_wins.begin(), enemy_wins.end(), 4) != enemy_wins.end(), "recognize opponent's direct threat in reachable state");
    check(engine.test_tactical_position(win_now, 4), "engine takes immediate win");
    check(engine.test_tactical_position(must_block, 4), "engine blocks single immediate loss");
    ExactResult exact_win = engine.solve_exact_endgame(win_now, 1000, 50);
    check(exact_win.solved && exact_win.outcome == 1 && exact_win.best_col == 4,
          "exact WDL solver proves an immediate win in a reachable position");
    ProofResult immediate_proof = engine.prove_forced_win(win_now, 500, 4);
    check(immediate_proof.status == 1 && immediate_proof.best_col == 4,
          "AND/OR proof search certifies an immediate forced win and winning move");
    ProofResult horizon_guard = engine.prove_forced_win(State{}, 100, 0);
    check(horizon_guard.status == 0 && !horizon_guard.timed_out,
          "proof search returns unresolved—not false—when depth horizon hides the opening result");
    DfpnResult dfpn_immediate = engine.prove_forced_win_dfpn(win_now, 500, 4);
    check(dfpn_immediate.status == 1 && dfpn_immediate.best_col == 4 && dfpn_immediate.proof_number == 0,
          "DFPN proves an immediate forced win with proof number zero");
    DfpnResult dfpn_horizon = engine.prove_forced_win_dfpn(State{}, 100, 0);
    check(dfpn_horizon.status == 0 && dfpn_horizon.proof_number != 0 && dfpn_horizon.disproof_number != 0,
          "DFPN leaves an unresolved horizon as unknown rather than refuting it");
    ExactResult opening_guard = engine.solve_exact_endgame(State{}, 50, 12);
    check(!opening_guard.eligible && !opening_guard.solved,
          "exact solver refuses to pretend the empty opening is a small endgame");
    State malformed{};
    malformed.mask = U64(1) << (2 * STRIDE + 3); // floating stone violates gravity
    malformed.pos = malformed.mask;
    malformed.moves = 1;
    ExactResult invalid_exact = engine.solve_exact_endgame(malformed, 100, 10);
    check(invalid_exact.invalid_position && !invalid_exact.solved,
          "exact solver rejects structurally invalid floating-stone positions");
    State sentinel_state{};
    sentinel_state.mask = U64(1) << (2 * STRIDE + H);
    sentinel_state.pos = sentinel_state.mask;
    sentinel_state.moves = 1;
    ExactResult sentinel_exact = engine.solve_exact_endgame(sentinel_state, 100, 10);
    check(sentinel_exact.invalid_position,
          "exact solver rejects occupancy in guard/sentinel bits");
    State parity_bad{win_now.mask, win_now.mask, win_now.moves};
    ExactResult parity_exact = engine.solve_exact_endgame(parity_bad, 100, 10);
    check(parity_exact.invalid_position,
          "exact solver rejects piece counts inconsistent with alternating turns");
    State eval_state{};
    for (int c : {4, 3, 5, 3, 2, 6, 4, 5, 1, 7}) apply_move(eval_state, c);
    State eval_mirror{mirror_bits(eval_state.pos), mirror_bits(eval_state.mask), eval_state.moves};
    check(engine.evaluate_for_side_to_move(eval_state) == engine.evaluate_for_side_to_move(eval_mirror),
          "static evaluation is invariant under horizontal reflection");
    State after_d{};
    apply_move(after_d, 3);
    State after_f{};
    apply_move(after_f, 5);
    Engine symmetry_search(8, true, true);
    SearchResult d_search = symmetry_search.think(after_d, 1500, 8);
    SearchResult f_search = symmetry_search.think(after_f, 1500, 8);
    check(d_search.depth == 8 && f_search.depth == 8 && d_search.score == f_search.score,
          "fixed-depth search score is invariant under mirroring the opening move d/f");

    // Differentially compare the normal alpha-beta engine against the same search with TT disabled.
    std::mt19937 tt_rng(0x7A2B441U);
    bool tt_consistent = true;
    Engine tt_enabled(8, true, true);
    Engine tt_disabled(1, false, true);
    Engine pvs_disabled(8, true, false);
    int tt_cases = 0;
    bool pvs_consistent = true;
    int pvs_cases = 0;
    for (int attempt = 0; attempt < 60 && tt_cases < 8; ++attempt) {
        State probe{};
        int target_moves = 8 + static_cast<int>(tt_rng() % 8);
        for (int j = 0; j < target_moves; ++j) {
            std::vector<int> safe;
            for (int c = 0; c < W; ++c) {
                U64 m = drop_bit(probe.mask, c);
                if (m && !has_five(probe.pos | m)) safe.push_back(c);
            }
            if (safe.empty()) break;
            int c = safe[tt_rng() % safe.size()];
            apply_move(probe, c);
        }
        if (probe.moves < 8 || probe.moves >= MAX_MOVES ||
            has_five(probe.pos) || has_five(probe.mask ^ probe.pos)) continue;
        SearchResult with_tt = tt_enabled.think(probe, 4000, 5);
        SearchResult without_tt = tt_disabled.think(probe, 4000, 5);
        SearchResult no_pvs = pvs_disabled.think(probe, 4000, 5);
        if (with_tt.depth != without_tt.depth || with_tt.score != without_tt.score ||
            with_tt.timed_out || without_tt.timed_out) {
            tt_consistent = false;
            std::cout << "  TT differential mismatch: with=" << with_tt.score
                      << " without=" << without_tt.score
                      << " depth_with=" << with_tt.depth
                      << " depth_without=" << without_tt.depth << "\n";
            break;
        }
        if (with_tt.depth != no_pvs.depth || with_tt.score != no_pvs.score || no_pvs.timed_out) {
            pvs_consistent = false;
            std::cout << "  PVS differential mismatch: with=" << with_tt.score
                      << " without=" << no_pvs.score
                      << " depth_with=" << with_tt.depth
                      << " depth_without=" << no_pvs.depth << "\n";
            break;
        }
        ++tt_cases;
        ++pvs_cases;
    }
    check(tt_consistent && tt_cases >= 8 && !tt_disabled.transposition_table_enabled() &&
          tt_enabled.transposition_table_enabled(),
          "alpha-beta scores match with and without the TT; disabled mode allocates no table");
    check(pvs_consistent && pvs_cases >= 8,
          "principal-variation search matches full-window alpha-beta on random positions");
    U64 four_be = bits_at({{1,0},{2,0},{3,0},{4,0}});
    State double_threat_loss{0, four_be, 4};
    SearchResult unavoidable_loss = engine.think(double_threat_loss, 500, 1);
    check(unavoidable_loss.score < -900000,
          "two separate opponent winning columns are recognized as an unavoidable loss");
    State mirrored_win{four_be, four_be, 4};
    SearchResult left_right_a = engine.think(mirrored_win, 500, 8);
    U64 four_mirrored = mirror_bits(four_be);
    State mirrored_win_b{four_mirrored, four_mirrored, 4};
    SearchResult left_right_b = engine.think(mirrored_win_b, 500, 8);
    check(left_right_a.best_col == 5 && left_right_b.best_col == 3,
          "tactical best move mirrors correctly across the board");

    // A reachable 18-ply position where d creates two direct winning columns (c and g).
    // The current player has no immediate win and the opponent has no immediate win before d.
    State fork_state{};
    const char* fork_history[] = {"a","a","h","f","b","e","h","d","i","g","e","a","c","e","b","h","f","h"};
    bool history_legal = true;
    for (const char* token : fork_history) {
        int c = -1;
        if (!parse_col(token, c)) { history_legal = false; break; }
        U64 m = drop_bit(fork_state.mask, c);
        if (!m || has_five(fork_state.pos | m)) { history_legal = false; break; }
        apply_move(fork_state, c);
    }
    auto fork_own_wins = engine.winning_columns(fork_state.pos, fork_state.mask);
    auto fork_enemy_wins = engine.winning_columns(fork_state.mask ^ fork_state.pos, fork_state.mask);
    SearchResult fork_result = engine.think(fork_state, 1000, 1);
    check(history_legal && fork_state.moves == 18 && fork_own_wins.empty() &&
          fork_enemy_wins.empty() && fork_result.depth == 1 && fork_result.best_col == 3,
          "forcing-threat extension finds a double-winning fork at depth 1");
    ProofResult fork_proof = engine.prove_forced_win(fork_state, 1000, 2);
    check(fork_proof.status == 1 && fork_proof.best_col == 3,
          "AND/OR proof search certifies a non-immediate double-winning fork");
    State fork_state_mirror{mirror_bits(fork_state.pos), mirror_bits(fork_state.mask), fork_state.moves};
    ProofResult fork_proof_mirror = engine.prove_forced_win(fork_state_mirror, 1000, 2);
    check(fork_proof_mirror.status == 1 && fork_proof_mirror.best_col == W - 1 - fork_proof.best_col,
          "AND/OR proof search returns the mirrored winning move after horizontal reflection");
    DfpnResult dfpn_fork = engine.prove_forced_win_dfpn(fork_state, 1000, 2);
    DfpnResult dfpn_fork_mirror = engine.prove_forced_win_dfpn(fork_state_mirror, 1000, 2);
    check(dfpn_fork.status == 1 && dfpn_fork.best_col == 3,
          "DFPN finds a non-immediate forced win via proof-number guidance");
    check(dfpn_fork_mirror.status == 1 && dfpn_fork_mirror.best_col == W - 1 - dfpn_fork.best_col,
          "DFPN preserves the winning move under horizontal reflection");

    // A reachable late-game trap: playing f supports an opponent's immediate horizontal win
    // in that same column, while b/c/d/i avoid the immediate loss.
    State support_trap{};
    const char* trap_history[] = {
        "c","b","e","b","h","h","h","g","d","b","h","a","g","h","a","i",
        "e","e","b","f","a","a","a","i","a","h","g","c","g","g","b","f",
        "e","i","g","e","e","d"
    };
    bool trap_history_legal = true;
    for (const char* token : trap_history) {
        int c = -1;
        if (!parse_col(token, c)) { trap_history_legal = false; break; }
        U64 m = drop_bit(support_trap.mask, c);
        if (!m || has_five(support_trap.pos | m)) { trap_history_legal = false; break; }
        apply_move(support_trap, c);
    }
    const auto trap_own_wins = engine.winning_columns(support_trap.pos, support_trap.mask);
    const auto trap_enemy_wins = engine.winning_columns(support_trap.mask ^ support_trap.pos, support_trap.mask);
    const U64 trap_f = drop_bit(support_trap.mask, 5);
    State after_trap_f{support_trap.pos ^ support_trap.mask,
                       support_trap.mask | trap_f, support_trap.moves + 1};
    const auto revealed_enemy_wins = engine.winning_columns(after_trap_f.pos, after_trap_f.mask);
    SearchResult trap_result = engine.think(support_trap, 1000, 1);
    check(trap_history_legal && support_trap.moves == 38 && trap_own_wins.empty() &&
          trap_enemy_wins.empty() && std::find(revealed_enemy_wins.begin(), revealed_enemy_wins.end(), 5) != revealed_enemy_wins.end() &&
          trap_result.best_col >= 0 && trap_result.best_col != 5,
          "gravity-aware move ordering avoids a support move that opens an opponent win");
    DfpnResult dfpn_mixed_horizon = engine.prove_forced_win_dfpn(support_trap, 100, 1);
    check(trap_history_legal && dfpn_mixed_horizon.status == 0 && !dfpn_mixed_horizon.timed_out &&
          !dfpn_mixed_horizon.resource_limited,
          "DFPN safely stops on a mixed frontier with both refuted and unresolved child states");

    // Mirror invariance: mirrored states must return mirrored move in a symmetric empty-root search.
    State empty{};
    SearchResult r = engine.think(empty, 250, 6);
    check(r.best_col >= 0 && r.best_col < W, "engine returns a legal move from empty board");
    check(!r.exact_wdl, "opening search never labels an unproven position as exactly solved");
    // Fuzz the public move selector over reproducible legal histories. Only non-winning
    // moves are admitted while constructing positions; the result must always be legal.
    std::mt19937 legal_rng(0x19BADC0DU);
    bool random_legal_moves = true;
    int random_positions_checked = 0;
    for (int attempt = 0; attempt < 1000 && random_positions_checked < 40; ++attempt) {
        State probe{};
        const int target = 5 + static_cast<int>(legal_rng() % 36); // 5..40 plies
        for (int ply = 0; ply < target; ++ply) {
            std::vector<int> safe;
            for (int c = 0; c < W; ++c) {
                U64 bit = drop_bit(probe.mask, c);
                if (bit && !has_five(probe.pos | bit)) safe.push_back(c);
            }
            if (safe.empty()) break;
            const int c = safe[legal_rng() % safe.size()];
            if (!apply_move(probe, c)) break;
        }
        if (probe.moves < 5 || probe.moves >= MAX_MOVES ||
            has_five(probe.pos) || has_five(probe.mask ^ probe.pos)) continue;
        SearchResult random_result = engine.think(probe, 5, 4);
        if (random_result.best_col < 0 || random_result.best_col >= W ||
            drop_bit(probe.mask, random_result.best_col) == 0) {
            random_legal_moves = false;
            break;
        }
        ++random_positions_checked;
    }
    check(random_legal_moves && random_positions_checked == 40,
          "engine returns legal moves across 40 randomized reachable positions under bounded time");
    // Gravity/stack sanity: all column heights remain compact after repeated drops.
    State s{};
    bool stack_ok = true;
    for (int c : {4,4,3,5,4,3,5,4,2,6,4}) {
        U64 bit = drop_bit(s.mask, c);
        if (!bit) { stack_ok = false; break; }
        s = State{s.pos ^ s.mask, s.mask | bit, s.moves + 1};
    }
    for (int c = 0; c < W; ++c) {
        int h = height_of(s.mask, c);
        U64 expected = h ? ((low_bits(h) << (c * STRIDE))) : 0;
        if ((s.mask & COL_MASK[c]) != expected) stack_ok = false;
    }
    check(stack_ok, "gravity maintains a contiguous stack in every column");

    // Differentially verify full-width search against a deliberately simple exhaustive solver
    // on reachable late-game positions. Construction stops before any win is played.
    auto reference = [&](auto&& self, const State& q) -> int {
        if (q.moves >= MAX_MOVES) return 0;
        int best = -1;
        for (int c = 0; c < W; ++c) {
            U64 m = drop_bit(q.mask, c);
            if (!m) continue;
            if (has_five(q.pos | m)) return 1;
            State child{q.pos ^ q.mask, q.mask | m, q.moves + 1};
            int value = -self(self, child);
            if (value > best) best = value;
            if (best == 1) return 1;
        }
        return best;
    };
    std::mt19937 rng(0x5F1A2026U);
    int exact_cases = 0;
    int proof_checked = 0;
    int proof_complete = 0;
    int dfpn_checked = 0;
    int dfpn_complete = 0;
    bool exact_ok = true;
    bool proof_sound = true;
    bool dfpn_sound = true;
    for (int attempt = 0; attempt < 2000 && exact_cases < 100; ++attempt) {
        U64 p0 = 0, p1 = 0, mask = 0;
        int moves = 0, side = 0;
        while (moves < 47) {
            std::vector<int> safe;
            for (int c = 0; c < W; ++c) {
                U64 m = drop_bit(mask, c);
                if (!m) continue;
                U64 mine = (side == 0 ? p0 : p1);
                if (!has_five(mine | m)) safe.push_back(c);
            }
            if (safe.empty()) break;
            std::shuffle(safe.begin(), safe.end(), rng);
            int c = safe.front();
            U64 m = drop_bit(mask, c);
            if (side == 0) p0 |= m; else p1 |= m;
            mask |= m;
            ++moves;
            side ^= 1;
        }
        if (moves < 46 || moves >= MAX_MOVES) continue;
        State late{side == 0 ? p0 : p1, mask, moves};
        if (has_five(p0) || has_five(p1)) continue;
        int remaining = MAX_MOVES - moves;
        int reference_value = reference(reference, late);
        ExactResult exact_result = engine.solve_exact_endgame(late, 5000, remaining);
        bool chosen_move_ok = exact_result.best_col >= 0 && exact_result.best_col < W;
        if (chosen_move_ok) {
            const U64 chosen_bit = drop_bit(late.mask, exact_result.best_col);
            if (!chosen_bit) chosen_move_ok = false;
            else if (has_five(late.pos | chosen_bit)) chosen_move_ok = (reference_value == 1);
            else {
                State chosen_child{late.pos ^ late.mask, late.mask | chosen_bit, late.moves + 1};
                chosen_move_ok = (-reference(reference, chosen_child) == reference_value);
            }
        }
        if (!exact_result.solved || exact_result.outcome != reference_value || !chosen_move_ok) {
            exact_ok = false;
            std::cout << "  exact differential mismatch: moves=" << moves
                      << " remaining=" << remaining
                      << " ref=" << reference_value
                      << " engine=" << exact_result.outcome
                      << " chosen_move=" << exact_result.best_col
                      << " chosen_move_ok=" << chosen_move_ok
                      << " solved=" << exact_result.solved
                      << " timeout=" << exact_result.timed_out << "\n";
            break;
        }
        // Cross-check the separate AND/OR proof search against the independently solved WDL
        // outcome on a reproducible subset. Unresolved is acceptable under the time cap;
        // any completed proof/refutation must agree with exact minimax.
        if (exact_cases % 2 == 0 && proof_checked < 50) {
            ProofResult proof_check = engine.prove_forced_win(late, 100, remaining);
            if (proof_check.status != 0) {
                ++proof_complete;
                if ((proof_check.status == 1 && reference_value != 1) ||
                    (proof_check.status == -1 && reference_value == 1)) {
                    proof_sound = false;
                    std::cout << "  proof/WDL mismatch: moves=" << moves
                              << " exact=" << reference_value
                              << " proof=" << proof_check.status << "\n";
                }
            }
            ++proof_checked;
        }
        if (exact_cases % 2 == 0 && dfpn_checked < 50) {
            DfpnResult dfpn_check = engine.prove_forced_win_dfpn(late, 100, remaining);
            if (dfpn_check.status != 0) {
                ++dfpn_complete;
                if ((dfpn_check.status == 1 && reference_value != 1) ||
                    (dfpn_check.status == -1 && reference_value == 1)) {
                    dfpn_sound = false;
                    std::cout << "  DFPN/WDL mismatch: moves=" << moves
                              << " exact=" << reference_value
                              << " dfpn=" << dfpn_check.status << "\n";
                }
            }
            ++dfpn_checked;
        }
        ++exact_cases;
    }
    check(exact_ok && exact_cases >= 100,
          "dedicated exact WDL solver agrees with independent exhaustive minimax on late-game positions");
    std::cout << "  exact endgame differential cases: " << exact_cases << "\n";
    check(proof_sound && proof_checked == 50,
          "bounded AND/OR proof results never contradict exact WDL on sampled late-game positions");
    std::cout << "  proof cross-checks completed: " << proof_complete << "/" << proof_checked << "\n";
    check(dfpn_sound && dfpn_checked == 50,
          "DFPN proof numbers never contradict exact WDL on sampled late-game positions");
    std::cout << "  DFPN cross-checks completed: " << dfpn_complete << "/" << dfpn_checked << "\n";

    State known_draw_endgame{};
    const char* draw_history[] = {
        "g","g","a","e","i","h","g","e","h","f","d","i","c","e","c","b",
        "e","i","c","e","b","b","f","h","i","b","f","g","f","d","i","h",
        "h","i","e","a","h","f","a","a","g","d","g","a","f","d","c"
    };
    bool draw_history_legal = true;
    for (const char* token : draw_history) {
        int c = -1;
        if (!parse_col(token, c)) { draw_history_legal = false; break; }
        const U64 move = drop_bit(known_draw_endgame.mask, c);
        if (!move || has_five(known_draw_endgame.pos | move)) { draw_history_legal = false; break; }
        apply_move(known_draw_endgame, c);
    }
    ExactResult known_draw = engine.solve_exact_endgame(known_draw_endgame, 5000, 8);
    check(draw_history_legal && known_draw_endgame.moves == 47 && known_draw.solved &&
          known_draw.outcome == 0 && known_draw.best_col == 2,
          "reproducible 47-ply late-game fixture is exactly drawn and recommends column c");
    State known_draw_mirror{mirror_bits(known_draw_endgame.pos),
                            mirror_bits(known_draw_endgame.mask), known_draw_endgame.moves};
    ExactResult mirrored_draw = engine.solve_exact_endgame(known_draw_mirror, 5000, 8);
    check(mirrored_draw.solved && mirrored_draw.outcome == known_draw.outcome &&
          mirrored_draw.best_col == W - 1 - known_draw.best_col,
          "exact WDL solver preserves outcome and mirrors the recommended move");
    ProofResult draw_proof = engine.prove_forced_win(known_draw_endgame, 1200, 8);
    check(draw_proof.status == -1 && !draw_proof.timed_out && !draw_proof.resource_limited,
          "AND/OR proof search refutes a forced win in the exactly drawn endgame");
    DfpnResult dfpn_draw = engine.prove_forced_win_dfpn(known_draw_endgame, 1200, 8);
    check(dfpn_draw.status == -1 && dfpn_draw.disproof_number == 0 &&
          !dfpn_draw.timed_out && !dfpn_draw.resource_limited,
          "DFPN refutes a forced win in the exactly drawn endgame");
    SearchResult integrated_endgame = engine.think(known_draw_endgame, 1200);
    check(integrated_endgame.exact_wdl && integrated_endgame.exact_outcome == 0 &&
          integrated_endgame.best_col == known_draw.best_col,
          "normal engine automatically switches to exact WDL search on a solvable <=18-empty endgame");
    State quiet_18_empty{};
    const char* quiet_history[] = {
        "g","e","i","b","g","b","d","e","g","a","b","g",
        "i","d","d","g","a","h","d","d","i","f","d","e",
        "e","e","a","a","e","a","h","b","c","b","f","b"
    };
    bool quiet_history_legal = true;
    for (const char* token : quiet_history) {
        int c = -1;
        if (!parse_col(token, c)) { quiet_history_legal = false; break; }
        const U64 bit = drop_bit(quiet_18_empty.mask, c);
        if (!bit || has_five(quiet_18_empty.pos | bit)) { quiet_history_legal = false; break; }
        apply_move(quiet_18_empty, c);
    }
    const bool quiet_no_direct_wins = engine.winning_columns(quiet_18_empty.pos, quiet_18_empty.mask).empty() &&
        engine.winning_columns(quiet_18_empty.mask ^ quiet_18_empty.pos, quiet_18_empty.mask).empty();
    SearchResult integrated_18 = engine.think(quiet_18_empty, 1000);
    check(quiet_history_legal && quiet_18_empty.moves == 36 && quiet_no_direct_wins &&
          integrated_18.exact_wdl && integrated_18.exact_outcome == -1 && integrated_18.best_col == 5,
          "normal engine solves a quiet 18-empty endgame exactly within its capped probe");
    std::cout << "selftest: " << passed << " passed, " << failed << " failed\n";
    return failed == 0;
}

bool selftest_9x7() {
    int passed = 0, failed = 0;
    auto check = [&](bool ok, const char* name) {
        std::cout << (ok ? "PASS " : "FAIL ") << name << "\n";
        ok ? ++passed : ++failed;
    };
    check(WINDOWS.size() == 92, "9x7 geometry has exactly 92 five-cell winning windows");
    bool landing = true;
    for (int c = 0; c < W; ++c) {
        U64 mask = 0;
        for (int h = 0; h < H; ++h) {
            if (drop_bit(mask, c) != (U64(1) << (c * STRIDE + h))) landing = false;
            mask |= U64(1) << (c * STRIDE + h);
        }
        if (drop_bit(mask, c) != 0) landing = false;
    }
    check(landing, "gravity landing and full-column rejection are correct for all 9 columns");
    auto bits_for = [](std::initializer_list<std::pair<int,int>> cells) {
        U64 b = 0;
        for (auto [c,r] : cells) b |= U64(1) << (c * STRIDE + r);
        return b;
    };
    check(has_five(bits_for({{0,0},{1,0},{2,0},{3,0},{4,0}})), "horizontal five detection");
    check(has_five(bits_for({{2,0},{2,1},{2,2},{2,3},{2,4}})), "vertical five detection");
    check(has_five(bits_for({{0,0},{1,1},{2,2},{3,3},{4,4}})), "positive diagonal five detection");
    check(has_five(bits_for({{0,4},{1,3},{2,2},{3,1},{4,0}})), "negative diagonal five detection");
    std::mt19937_64 rng(0x9A7C0FFEEULL);
    U64 playable = 0;
    for (int c=0;c<W;++c) playable |= COL_MASK[c];
    bool random_ok = true, mirror_ok = true;
    for (int n=0;n<10000;++n) {
        U64 b = ((U64(rng()) << 64) | U64(rng())) & playable;
        bool scanned = false;
        for (const auto& w : WINDOWS) if ((b & w.bits) == w.bits) { scanned = true; break; }
        if (has_five(b) != scanned) random_ok = false;
        if (mirror_bits(mirror_bits(b)) != b || has_five(mirror_bits(b)) != has_five(b)) mirror_ok = false;
    }
    check(random_ok, "bitboard five detection matches 92-window scan on 10,000 random 72-bit boards");
    check(mirror_ok, "horizontal mirror is an exact involution and preserves win detection");
    // A reachable-in-principle balanced root: one side has four bottom-row stones
    // in a-c-d? specifically columns 0..3; the other side has four in columns 5..8.
    U64 target = 0, all = 0;
    for (int c=0;c<4;++c) { U64 b=U64(1)<<(c*STRIDE); target|=b; all|=b; }
    for (int c=5;c<9;++c) all |= U64(1)<<(c*STRIDE);
    State tactical{target, all, 8};
    Engine proof_engine(1, false);
    const DfpnResult proof = proof_engine.prove_forced_win_dfpn(tactical, 1000, 8);
    check(proof.status == 1 && proof.best_col == 4,
          "DFPN proves a concrete immediate winning move on the 9x7 board");
    const ExactResult exact = proof_engine.solve_exact_endgame(tactical, 1000, 55);
    check(exact.solved && exact.outcome == 1 && exact.best_col == 4,
          "exact WDL search certifies the same reachable 9x7 win");
    std::cout << "selftest-9x7: " << passed << " passed, " << failed << " failed\n";
    return failed == 0;
}

bool parse_history_spec(const std::string& raw, std::vector<int>& history, State& state) {
    history.clear(); state = State{};
    if (raw == "-" || raw.empty()) return true;
    std::string token;
    auto consume = [&](const std::string& item) -> bool {
        if (item.empty()) return true;
        int col = -1;
        if (item.size() == 1 && ((item[0] >= 'a' && item[0] <= 'i') || (item[0] >= 'A' && item[0] <= 'I'))) {
            if (!parse_col(item, col)) return false;
        } else {
            char* end = nullptr;
            long v = std::strtol(item.c_str(), &end, 10);
            if (end == item.c_str() || *end != '\0' || v < 0 || v >= W) return false;
            col = static_cast<int>(v);
        }
        const U64 bit = drop_bit(state.mask, col);
        if (!bit || has_five(state.pos | bit)) return false;
        state = State{state.pos ^ state.mask, state.mask | bit, state.moves + 1};
        history.push_back(col);
        return true;
    };
    for (char ch : raw) {
        if (ch == ',' || ch == ';' || ch == ' ') {
            if (!consume(token)) return false;
            token.clear();
        } else token.push_back(ch);
    }
    return consume(token) && history.size() <= MAX_MOVES;
}

int watcher_server() {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    Engine engine(1, false);
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        std::istringstream in(line);
        std::string command;
        in >> command;
        if (command == "QUIT") {
            std::cout << "{\"ok\":true,\"status\":\"STOPPED\"}" << std::endl;
            break;
        }
        if (command == "RESET") {
            engine.reset_watcher();
            std::cout << "{\"ok\":true,\"status\":\"RESET\"}" << std::endl;
            continue;
        }
        if (command == "STATUS") {
            std::cout << "{\"ok\":true,\"cacheEntries\":" << engine.watcher_cache_entries() << "}" << std::endl;
            continue;
        }
        if (command != "SEARCH") {
            std::cout << "{\"ok\":false,\"error\":\"unknown command\"}" << std::endl;
            continue;
        }
        int cpu_side = -1, time_ms = 150, max_plies = 4;
        std::string moves_text;
        if (!(in >> cpu_side >> time_ms >> max_plies >> moves_text)) {
            std::cout << "{\"ok\":false,\"error\":\"SEARCH requires cpuSide timeMs maxPlies history\"}" << std::endl;
            continue;
        }
        std::vector<int> history;
        State root;
        if (cpu_side < 0 || cpu_side > 1 || !parse_history_spec(moves_text, history, root) ||
            !engine.valid_state_for_watcher(root)) {
            std::cout << "{\"ok\":false,\"error\":\"invalid or terminal history\"}" << std::endl;
            continue;
        }
        time_ms = std::max(10, std::min(2000, time_ms));
        max_plies = std::max(0, std::min(MAX_MOVES - root.moves, max_plies));
        auto result = engine.watcher_slice(root, cpu_side, history, time_ms, max_plies, 220'000);
        std::cout << "{\"ok\":true,\"status\":\"" << result.status << "\","
                  << "\"proofNumber\":" << result.proof_number << ','
                  << "\"disproofNumber\":" << result.disproof_number << ','
                  << "\"nodes\":" << result.nodes << ','
                  << "\"frontierNodes\":" << result.frontiers << ','
                  << "\"rootFrontier\":" << (result.root_frontier ? "true" : "false") << ','
                  << "\"gravityNodesEvaluated\":" << result.gravity_nodes_evaluated << ','
                  << "\"cacheEntries\":" << result.cache_entries << ','
                  << "\"elapsedMs\":" << std::fixed << std::setprecision(3) << result.seconds * 1000.0 << ','
                  << "\"timedOut\":" << (result.timed_out ? "true" : "false") << ','
                  << "\"resourceLimited\":" << (result.resource_limited ? "true" : "false") << ','
                  << "\"exactWdl\":" << (result.exact_wdl ? "true" : "false") << ','
                  << "\"certificateBuilt\":" << (result.certificate_built ? "true" : "false") << ','
                  << "\"certificate\":" << (result.certificate_built ? result.certificate : "null") << "}" << std::endl;
    }
    return 0;
}

int main_impl(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "--distributed-worker") {
        if (argc < 10) {
            std::cerr << "usage: --distributed-worker target(0|1) budget_seconds slice_ms node_limit max_entries checkpoint solved_dump result_json moves(a-i...)\n";
            return 2;
        }
        auto integer = [](const char* text, long long fallback) -> long long {
            try { std::size_t n=0; long long v=std::stoll(text,&n); return text[n]=='\0'?v:fallback; }
            catch (...) { return fallback; }
        };
        const int target=static_cast<int>(integer(argv[2],-1));
        const int budget_seconds=static_cast<int>(integer(argv[3],-1));
        const int slice_ms=static_cast<int>(integer(argv[4],-1));
        const std::uint64_t node_limit=static_cast<std::uint64_t>(integer(argv[5],-1));
        const std::size_t max_entries=static_cast<std::size_t>(integer(argv[6],-1));
        const std::string checkpoint=argv[7], solved_dump=argv[8], result_path=argv[9];
        if (target<0 || target>1 || budget_seconds<1 || slice_ms<1000 || slice_ms>300000 ||
            node_limit<1000 || max_entries<100'000 || max_entries>8'000'000) {
            std::cerr << "invalid distributed worker limits\n"; return 2;
        }
        State root{};
        std::vector<int> history;
        for (int i=10;i<argc;i++) {
            int c=-1;
            if (!parse_col(argv[i],c)) { std::cerr << "invalid move token: " << argv[i] << "\n"; return 2; }
            const U64 bit=drop_bit(root.mask,c);
            if (!bit || has_five(root.pos | bit)) { std::cerr << "illegal history or history continues after terminal\n"; return 2; }
            root=State{root.pos ^ root.mask,root.mask | bit,root.moves+1};
            history.push_back(c);
        }
        Engine engine(1,false);
        engine.configure_distributed_checkpoint(checkpoint,max_entries);
        const auto started=std::chrono::steady_clock::now();
        std::uint64_t total_nodes=0, total_frontiers=0, slices=0;
        Engine::WatcherResult last;
        bool solved=false;
        auto write_result = [&](const std::string& status) {
            const std::string tmp=result_path+".tmp";
            std::ofstream out(tmp,std::ios::trunc);
            if (!out) return false;
            const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
            out << "{\"format\":\"GPC-DISTRIBUTED-WORKER-1\",\"target\":" << target
                << ",\"history\":[";
            for (std::size_t i=0;i<history.size();++i) { if(i) out << ','; out << history[i]; }
            out << "],\"status\":\"" << status << "\",\"proof_number\":" << last.proof_number
                << ",\"disproof_number\":" << last.disproof_number << ",\"nodes\":" << total_nodes
                << ",\"frontiers\":" << total_frontiers << ",\"cache_entries\":" << last.cache_entries
                << ",\"slices\":" << slices << ",\"elapsed_seconds\":" << elapsed
                << ",\"resource_limited\":" << (last.resource_limited?"true":"false")
                << ",\"timed_out\":" << (last.timed_out?"true":"false") << "}\n";
            out.flush(); if(!out) return false; out.close();
            // Atomic replace preserves the previous status file until the new one is complete.
            return std::rename(tmp.c_str(),result_path.c_str())==0;
        };
        while (std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count() < budget_seconds) {
            last=engine.watcher_slice(root,target,history,slice_ms,MAX_MOVES-root.moves,node_limit);
            ++slices; total_nodes+=last.nodes; total_frontiers+=last.frontiers;
            if (last.proof_number==0) { solved=true; write_result("target_win"); break; }
            if (last.disproof_number==0) { solved=true; write_result("target_not_win"); break; }
            write_result("unresolved");
            std::cout << "SLICE " << slices << " nodes=" << total_nodes
                      << " pn=" << last.proof_number << " dn=" << last.disproof_number
                      << " cache=" << last.cache_entries << " frontiers=" << last.frontiers
                      << " resource_limited=" << (last.resource_limited?"yes":"no") << std::endl;
        }
        const std::string final_status=last.proof_number==0?"target_win":(last.disproof_number==0?"target_not_win":"unresolved");
        if (!engine.export_distributed_solved(solved_dump)) { std::cerr << "failed to export solved-state table\n"; return 4; }
        if (!write_result(final_status)) { std::cerr << "failed to write result file\n"; return 4; }
        const bool checkpoint_ok=std::ifstream(checkpoint,std::ios::binary).good();
        std::cout << "FINAL status=" << final_status << " target=" << target
                  << " nodes=" << total_nodes << " pn=" << last.proof_number
                  << " dn=" << last.disproof_number << " slices=" << slices
                  << " checkpoint=" << (checkpoint_ok?"saved":"missing") << std::endl;
        return checkpoint_ok?0:4;
    }
    if (argc >= 2 && std::string(argv[1]) == "--watcher-server") return watcher_server();
    if (argc >= 2 && std::string(argv[1]) == "--watcher-memory-selftest") {
        Engine e(1, false);
        return e.watcher_memory_self_test(std::cout) ? 0 : 1;
    }
    if (argc >= 2 && std::string(argv[1]) == "--selftest") return selftest_9x7() ? 0 : 1;
    if (argc >= 2 && std::string(argv[1]) == "--selftest-9x7") return selftest_9x7() ? 0 : 1;
    if (argc >= 2 && std::string(argv[1]) == "--solve-endgame") {
        int max_empty = argc >= 3 ? std::atoi(argv[2]) : 12;
        int ms = argc >= 4 ? std::max(1, std::atoi(argv[3])) : 5000;
        if (max_empty < 0 || max_empty > MAX_MOVES) {
            std::cerr << "max_empty must be between 0 and 63\n";
            return 2;
        }
        State s{};
        for (int i = 4; i < argc; ++i) {
            int c = -1;
            if (!parse_col(argv[i], c)) {
                std::cerr << "Invalid column token: " << argv[i] << " (use a-i)\n";
                return 2;
            }
            const U64 move = drop_bit(s.mask, c);
            if (!move) {
                std::cerr << "Illegal move: column " << char('a' + c) << " is full\n";
                return 2;
            }
            if (has_five(s.pos | move)) {
                std::cerr << "History continues after a winning move\n";
                return 2;
            }
            s = State{s.pos ^ s.mask, s.mask | move, s.moves + 1};
        }
        print_board(s);
        Engine exact_engine(1, false); // exact WDL search does not use the alpha-beta TT
        ExactResult r = exact_engine.solve_exact_endgame(s, ms, max_empty);
        print_exact_result(r);
        return r.solved ? 0 : (r.invalid_position ? 2 : 3);
    }
    if (argc >= 2 && std::string(argv[1]) == "--prove-win") {
        int ms = argc >= 3 ? std::max(1, std::atoi(argv[2])) : 2000;
        int max_plies = argc >= 4 ? std::atoi(argv[3]) : 24;
        if (max_plies < 0 || max_plies > MAX_MOVES) {
            std::cerr << "max_plies must be between 0 and 63\n";
            return 2;
        }
        State s{};
        for (int i = 4; i < argc; ++i) {
            int c = -1;
            if (!parse_col(argv[i], c)) {
                std::cerr << "Invalid column token: " << argv[i] << " (use a-i)\n";
                return 2;
            }
            const U64 move = drop_bit(s.mask, c);
            if (!move) {
                std::cerr << "Illegal move: column " << char('a' + c) << " is full\n";
                return 2;
            }
            if (has_five(s.pos | move)) {
                std::cerr << "History continues after a winning move\n";
                return 2;
            }
            s = State{s.pos ^ s.mask, s.mask | move, s.moves + 1};
        }
        print_board(s);
        Engine proof_engine(1, false); // proof search uses its own collision-safe memo
        ProofResult r = proof_engine.prove_forced_win(s, ms, max_plies);
        print_proof_result(r);
        return r.invalid_position ? 2 : (r.status != 0 ? 0 : 3);
    }
    // Default to a useful TT while allowing constrained devices to reduce memory use.
    // Parse with strtol so malformed/overflowing environment strings are handled safely.
    std::size_t tt_megabytes = 128;
    if (const char* env = std::getenv("FIVE_TT_MB")) {
        char* end = nullptr;
        const long parsed = std::strtol(env, &end, 10);
        if (end != env && *end == '\0' && parsed >= 1 && parsed <= 1024)
            tt_megabytes = static_cast<std::size_t>(parsed);
        else
            std::cerr << "Ignoring invalid FIVE_TT_MB='" << env << "' (expected integer 1..1024)\n";
    }
    if (argc >= 2 && std::string(argv[1]) == "--dfpn-win") {
        int ms = argc >= 3 ? std::max(1, std::atoi(argv[2])) : 2000;
        int max_plies = argc >= 4 ? std::atoi(argv[3]) : 24;
        if (max_plies < 0 || max_plies > MAX_MOVES) {
            std::cerr << "max_plies must be between 0 and 63\n";
            return 2;
        }
        State s{};
        for (int i = 4; i < argc; ++i) {
            int c = -1;
            if (!parse_col(argv[i], c)) {
                std::cerr << "Invalid column token: " << argv[i] << " (use a-i)\n";
                return 2;
            }
            const U64 move = drop_bit(s.mask, c);
            if (!move) {
                std::cerr << "Illegal move: column " << char('a' + c) << " is full\n";
                return 2;
            }
            if (has_five(s.pos | move)) {
                std::cerr << "History continues after a winning move\n";
                return 2;
            }
            s = State{s.pos ^ s.mask, s.mask | move, s.moves + 1};
        }
        print_board(s);
        Engine dfpn_engine(1, false);
        DfpnResult r = dfpn_engine.prove_forced_win_dfpn(s, ms, max_plies);
        print_dfpn_result(r);
        return r.invalid_position ? 2 : (r.status != 0 ? 0 : 3);
    }
    Engine engine(tt_megabytes); // large alpha-beta TT only for play/analyze modes
    if (!engine.transposition_table_enabled()) {
        std::cerr << "Warning: unable to allocate a transposition table; continuing without it.\n";
    } else if (engine.transposition_table_megabytes() < tt_megabytes) {
        std::cerr << "Note: transposition table reduced to "
                  << engine.transposition_table_megabytes() << " MiB due to memory limits.\n";
    }
    if (argc >= 2 && std::string(argv[1]) == "--analyze") {
        State s{};
        for (int i = 2; i < argc; ++i) {
            int c;
            if (!parse_col(argv[i], c)) {
                std::cerr << "Invalid column token: " << argv[i] << " (use a-i)\n";
                return 2;
            }
            U64 move = drop_bit(s.mask, c);
            if (!move) {
                std::cerr << "Illegal move: column " << char('a' + c) << " is full\n";
                return 2;
            }
            if (has_five(s.pos | move)) {
                std::cout << "The game already ended with a win on move " << (s.moves + 1) << ".\n";
                return 0;
            }
            s = State{s.pos ^ s.mask, s.mask | move, s.moves + 1};
        }
        print_board(s);
        int ms = 1500;
        if (const char* env = std::getenv("FIVE_TIME_MS")) ms = std::max(1, std::atoi(env));
        int max_depth = MAX_MOVES;
        if (const char* env = std::getenv("FIVE_MAX_DEPTH")) {
            char* end = nullptr;
            const long parsed = std::strtol(env, &end, 10);
            if (end != env && *end == '\0' && parsed >= 1 && parsed <= MAX_MOVES)
                max_depth = static_cast<int>(parsed);
            else
                std::cerr << "Ignoring invalid FIVE_MAX_DEPTH='" << env << "' (expected integer 1..54)\n";
        }
        SearchResult r = engine.think(s, ms, max_depth);
        print_result(r);
        return 0;
    }
    if (argc >= 2 && std::string(argv[1]) == "--play") {
        int ms = argc >= 3 ? std::max(1, std::atoi(argv[2])) : 1500;
        bool human_first = !(argc >= 4 && std::string(argv[3]) == "engine-first");
        // Store absolute players explicitly for clean interactive rendering.
        U64 p0 = 0, p1 = 0, mask = 0;
        int moves = 0;
        auto display = [&]() {
            std::cout << "  a b c d e f g h i\n";
            for (int r = H - 1; r >= 0; --r) {
                std::cout << r + 1 << ' ';
                for (int c = 0; c < W; ++c) {
                    U64 b = U64(1) << (c * STRIDE + r);
                    char ch = (p0 & b) ? 'X' : ((p1 & b) ? 'O' : '.');
                    std::cout << ch << (c == W - 1 ? '\n' : ' ');
                }
            }
        };
        while (moves < MAX_MOVES) {
            display();
            bool human_turn = ((moves % 2 == 0) == human_first);
            int c = -1;
            if (human_turn) {
                std::cout << "Your move (a-i): ";
                std::string tok;
                if (!(std::cin >> tok)) return 0;
                if (!parse_col(tok, c) || drop_bit(mask, c) == 0) {
                    std::cout << "Invalid/full column. Try again.\n";
                    continue;
                }
            } else {
                State s;
                if (moves % 2 == 0) s = State{p0, mask, moves};
                else s = State{p1, mask, moves};
                // Engine State.pos must describe side-to-move. It does here because turns alternate.
                SearchResult r = engine.think(s, ms);
                c = r.best_col;
                std::cout << "Engine: " << char('a' + c) << " | ";
                print_result(r);
            }
            U64 bit = drop_bit(mask, c);
            if (!bit) { std::cerr << "Internal error: illegal move\n"; return 3; }
            if (moves % 2 == 0) p0 |= bit; else p1 |= bit;
            mask |= bit;
            ++moves;
            U64 just = (moves % 2 == 1) ? p0 : p1;
            if (has_five(just)) {
                display();
                std::cout << ((moves % 2 == 1) == human_first ? "You win!\n" : "Engine wins.\n");
                return 0;
            }
        }
        display();
        std::cout << "Draw.\n";
        return 0;
    }
    std::cout << "Five Gravity Engine (VOG Five in a Row, 9x6)\n"
              << "Usage:\n"
              << "  ./five_engine --selftest\n"
              << "  ./five_engine --solve-endgame [max_empty=12] [time_ms=5000] [moves ...]\n"
              << "  ./five_engine --prove-win [time_ms=2000] [max_plies=24] [moves ...]\n"
              << "  ./five_engine --dfpn-win [time_ms=2000] [max_plies=24] [moves ...]\n"
              << "  ./five_engine --analyze e d e c ...\n"
              << "  FIVE_TIME_MS=3000 FIVE_MAX_DEPTH=12 FIVE_TT_MB=128 ./five_engine --analyze e d e c ...\n"
              << "  ./five_engine --play [time_ms] [human-first|engine-first]\n";
    return 0;
}
} // namespace five

int main(int argc, char** argv) { return five::main_impl(argc, argv); }
