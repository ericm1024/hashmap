#include <algorithm>
#include <array>
#include <unordered_set>
#include <vector>

#include <benchmark/benchmark.h>

#include "ht.h"

typedef struct { uint64_t state;  uint64_t inc; } pcg32_random_t;

static pcg32_random_t g_state;

static uint32_t pcg32_random_r(pcg32_random_t* rng)
{
    uint64_t oldstate = rng->state;
    // Advance internal state
    rng->state = oldstate * 6364136223846793005ULL + (rng->inc|1);
    // Calculate output function (XSH RR), uses old state for max ILP
    uint32_t xorshifted = ((oldstate >> 18u) ^ oldstate) >> 27u;
    uint32_t rot = oldstate >> 59u;
    return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
}

static uint32_t pcg32_random()
{
        return pcg32_random_r(&g_state);
}

struct pcg32_generator {
        using result_type = uint32_t;

        static constexpr result_type min()
        {
                return std::numeric_limits<result_type>::min();
        }

        static constexpr result_type max()
        {
                return std::numeric_limits<result_type>::max();
        }

        result_type operator()() const
        {
                return pcg32_random();
        }
};

template <typename T>
struct get_random_impl;

template <typename T>
static T get_random()
{
        return get_random_impl<T>{}();
}

template <>
struct get_random_impl<uint32_t>
{
        uint32_t operator()()
        {
                return pcg32_random();
        }
};

template <>
struct get_random_impl<char>
{
        char operator()()
        {
                // xxx: is constructing this cheap?
                std::uniform_int_distribution<unsigned char> uni(32,126);
                pcg32_generator gen;
                return uni(gen);
        }
};

template <size_t N>
struct get_random_impl<std::array<uint32_t, N>>
{
        std::array<uint32_t, N> operator()()
        {
                std::array<uint32_t, N> a;

                std::generate(a.begin(), a.end(), get_random<uint32_t>);
                
                return a;
        }
};

// https://stackoverflow.com/a/8980550/3775803
template <class T> inline void hash_combine(std::size_t& seed, const T& v)
{
        std::hash<T> hasher;
        const std::size_t kMul = 0x9ddfea08eb382d69ULL;
        std::size_t a = (hasher(v) ^ seed) * kMul;
        a ^= (a >> 47);
        std::size_t b = (seed ^ a) * kMul;
        b ^= (b >> 47);
        seed = b * kMul;
}

namespace std {
template <typename T, size_t N>
struct hash<std::array<T,N>> {
        using argument_type = std::array<T,N>;
        using result_type = std::size_t;

        result_type operator()(argument_type const& s)
        {
                size_t hash = 0;

                for (size_t i = 0; i < s.size(); ++i) {
                        hash_combine(hash, s.at(i));
                }

                return hash;
        }
};
}

template <typename S> 
static void BM_insert(benchmark::State& state)
{
        using T = typename std::decay<decltype(*std::declval<S>().begin())>::type;
        
        for (auto _ : state) {
                S s;
                for (int i = 0; i < state.range(0); ++i) {
                        s.insert(get_random<T>());
                }
        }
}
BENCHMARK_TEMPLATE(BM_insert, hash_set<uint32_t>)->Range(8, 8<<20);
BENCHMARK_TEMPLATE(BM_insert, std::unordered_set<uint32_t>)->Range(8, 8<<20);
BENCHMARK_TEMPLATE(BM_insert, hash_set<std::array<uint32_t, 16>>)->Range(8, 8<<20);
BENCHMARK_TEMPLATE(BM_insert, std::unordered_set<std::array<uint32_t, 16>>)->Range(8, 8<<20);
BENCHMARK_TEMPLATE(BM_insert, hash_set<std::array<uint32_t, 64>>)->Range(8, 8<<20);
BENCHMARK_TEMPLATE(BM_insert, std::unordered_set<std::array<uint32_t, 64>>)->Range(8, 8<<20);
BENCHMARK_TEMPLATE(BM_insert, hash_set<std::array<uint32_t, 256>>)->Range(8, 8<<20);
BENCHMARK_TEMPLATE(BM_insert, std::unordered_set<std::array<uint32_t, 256>>)->Range(8, 8<<20);
BENCHMARK_TEMPLATE(BM_insert, hash_set<std::array<uint32_t, 1024>>)->Range(8, 8<<20);
BENCHMARK_TEMPLATE(BM_insert, std::unordered_set<std::array<uint32_t, 1024>>)->Range(8, 8<<20);



template <typename S>
static void BM_find_exists(benchmark::State& state)
{
        using T = typename std::decay<decltype(*std::declval<S>().find(1))>::type;
        
        for (auto _ : state) {
                state.PauseTiming();
                S s;
                for (int i = 0; i < state.range(0) * 2; ++i) {
                        s.insert(pcg32_random());
                }
                std::vector<T> all{s.begin(), s.end()};
                std::random_shuffle(all.begin(), all.end());
                std::vector<T> to_find{all.begin(), all.begin() + state.range(0)};
                state.ResumeTiming();
                for (int i = 0; i < state.range(0); ++i) {
                        s.find(to_find[i]);
                }
        }
}
BENCHMARK_TEMPLATE(BM_find_exists, hash_set<uint32_t>)->Range(8, 8<<20);
BENCHMARK_TEMPLATE(BM_find_exists, std::unordered_set<uint32_t>)->Range(8, 8<<20);

BENCHMARK_MAIN();
