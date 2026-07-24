// tracegen — self-instrumenting workload generator (SPEC section 12 fallback
// for machines without Valgrind).
//
// Runs REAL algorithms over real in-memory data structures and emits one
// lackey-format line per data access, using the actual virtual address of
// the touched element. The result is a genuine record of each algorithm's
// memory behavior — with the advantage that the locality pattern is KNOWN,
// so simulator results are predictable and explainable.
//
// Build:  via the Makefile 'tracegen' target
// Usage:  ./tracegen matmul   N              > traces/matmul.trace
//         ./tracegen listwalk NODES STEPS    > traces/listwalk.trace
//         ./tracegen seqscan  BYTES PASSES   > traces/seqscan.trace
//         ./tracegen randscan BYTES COUNT    > traces/randscan.trace
//
// Workload characters:
//   matmul   — naive i-j-k dense matmul: strong spatial (A row-major) and
//              temporal (C accumulator) locality, strided B column walks.
//   listwalk — pointer chasing over a deliberately SHUFFLED linked list:
//              every hop is an unpredictable jump; spatial locality ~zero.
//   seqscan  — pure sequential sweep: best-case spatial locality.
//   randscan — uniform random touches: worst-case everything.
// Deterministic (fixed RNG seed) so traces are reproducible.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

namespace {

void emit(char op, const void* p, unsigned size) {
    std::printf(" %c %llx,%u\n", op,
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(p)),
                size);
}

void matmul(int n) {
    std::vector<double> A(static_cast<size_t>(n) * n, 1.0);
    std::vector<double> B(static_cast<size_t>(n) * n, 2.0);
    std::vector<double> C(static_cast<size_t>(n) * n, 0.0);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            for (int k = 0; k < n; ++k) {
                emit('L', &A[static_cast<size_t>(i) * n + k], 8);
                emit('L', &B[static_cast<size_t>(k) * n + j], 8);
                emit('L', &C[static_cast<size_t>(i) * n + j], 8);
                C[static_cast<size_t>(i) * n + j] +=
                    A[static_cast<size_t>(i) * n + k] * B[static_cast<size_t>(k) * n + j];
                emit('S', &C[static_cast<size_t>(i) * n + j], 8);
            }
}

struct Node {
    Node* next;
    long  payload;
};

void listwalk(int nodes, int steps) {
    // Nodes sit contiguously in the pool, but the LINKS follow a shuffled
    // permutation — so each ->next hop lands on an unpredictable line,
    // exactly like a heap-allocated list after churn. This is what defeats
    // spatial locality and prefetching in real linked-list code.
    std::vector<Node> pool(static_cast<size_t>(nodes));
    std::vector<int>  order(static_cast<size_t>(nodes));
    for (int i = 0; i < nodes; ++i) order[static_cast<size_t>(i)] = i;
    std::mt19937 rng(42);
    std::shuffle(order.begin(), order.end(), rng);
    for (int i = 0; i < nodes; ++i) {
        Node& cur = pool[static_cast<size_t>(order[static_cast<size_t>(i)])];
        cur.next    = &pool[static_cast<size_t>(order[static_cast<size_t>((i + 1) % nodes)])];
        cur.payload = i;
    }

    Node* p = &pool[static_cast<size_t>(order[0])];
    long  sum = 0;
    for (int s = 0; s < steps; ++s) {
        emit('L', &p->next, 8);
        emit('L', &p->payload, 8);
        sum += p->payload;
        p = p->next;
    }
    if (sum == -1) std::fprintf(stderr, "impossible\n");   // keep `sum` observable
}

void seqscan(long bytes, int passes) {
    std::vector<uint64_t> a(static_cast<size_t>(bytes / 8), 1);
    uint64_t sum = 0;
    for (int pass = 0; pass < passes; ++pass)
        for (size_t i = 0; i < a.size(); ++i) {
            emit('L', &a[i], 8);
            sum += a[i];
        }
    if (sum == 0) std::fprintf(stderr, "impossible\n");
}

void randscan(long bytes, long count) {
    std::vector<uint64_t> a(static_cast<size_t>(bytes / 8), 1);
    std::mt19937 rng(42);
    std::uniform_int_distribution<size_t> pick(0, a.size() - 1);
    uint64_t sum = 0;
    for (long i = 0; i < count; ++i) {
        size_t idx = pick(rng);
        emit('L', &a[idx], 8);
        sum += a[idx];
    }
    if (sum == 0) std::fprintf(stderr, "impossible\n");
}

int usage() {
    std::fprintf(stderr,
        "usage: tracegen matmul N | listwalk NODES STEPS | "
        "seqscan BYTES PASSES | randscan BYTES COUNT\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    std::string kind = argv[1];
    if (kind == "matmul"   && argc == 3) { matmul(std::atoi(argv[2])); return 0; }
    if (kind == "listwalk" && argc == 4) { listwalk(std::atoi(argv[2]), std::atoi(argv[3])); return 0; }
    if (kind == "seqscan"  && argc == 4) { seqscan(std::atol(argv[2]), std::atoi(argv[3])); return 0; }
    if (kind == "randscan" && argc == 4) { randscan(std::atol(argv[2]), std::atol(argv[3])); return 0; }
    return usage();
}
