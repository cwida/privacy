//
// Created by ila on 12/19/25.
//

#ifndef PAC_MIN_MAX_HPP
#define PAC_MIN_MAX_HPP

// benchmarking defines that disable certain optimizations (and some flags imply others)
// #define PAC_NOBUFFERING 1 // to disable the buffering optimization.
// #define PAC_NOBOUNDOPT 1 // to disable global bound optimization.
// #define PAC_NOSIMD 1 // to get the IF..THEN SIMD-unfriendly aggregate computation kernel
#ifdef PAC_NOSIMD
#define PAC_NOCASCADING 1
#endif

// PAC_GODBOLT mode: cpp -DPAC_GODBOLT -P -E -w src/include/as_min_max.hpp
// Isolates the SIMD kernel for Godbolt analysis (-P removes line markers)
#ifdef PAC_GODBOLT
using uint8_t = unsigned char;
using int8_t = signed char;
using uint16_t = unsigned short;
using int16_t = signed short;
using uint32_t = unsigned int;
using int32_t = signed int;
using uint64_t = unsigned long long;
using int64_t = signed long long;
#else
#include "duckdb.hpp"
#include "as_aggregate.hpp"
namespace duckdb {
void RegisterPacMinFunctions(ExtensionLoader &loader);
void RegisterPacMaxFunctions(ExtensionLoader &loader);
void RegisterPacMinCountersFunctions(ExtensionLoader &loader);
void RegisterPacMaxCountersFunctions(ExtensionLoader &loader);
void RegisterDpSampleMinFunctions(ExtensionLoader &loader);
void RegisterDpSampleMaxFunctions(ExtensionLoader &loader);
void RegisterDpSampleMMinFunctions(ExtensionLoader &loader);
void RegisterDpSampleMMaxFunctions(ExtensionLoader &loader);
void RegisterPacNoisedClipMinFunctions(ExtensionLoader &loader);
void RegisterPacNoisedClipMaxFunctions(ExtensionLoader &loader);
void RegisterPacClipMinFunctions(ExtensionLoader &loader);
void RegisterPacClipMaxFunctions(ExtensionLoader &loader);

// ============================================================================
// PAC_MIN/PAC_MAX(hash_key, value) aggregate functions
// ============================================================================
// State: 64 extreme values, one for each bit position
// Update: for each (key_hash, value), update extremes[i] if bit i of key_hash is set
// Finalize: compute the PAC-noised min/max from the 64 counters
//
// we compute the minimum of all maximums for MAX (and vice versa for MIN)
// we do this every many value updates, in order to keep costs down
// if a new value is worse than the worst of all bounds, it can be skipped
// this is called BOUNDOPT
//
// the state directly keeps TYPE probabilistic totals[64] (no cascading with smaller types)
// we tried cascading, but it does not help much, and usually BOUNDOPT means that most
// aggregations actually do not need to do an actual MIN/MAX and do not touch the array,
// hence optimizing its size does not reduce the footprint much, and for code
// simplicity it was removed.
//
// In order to keep the size of the states low, it is more important to delay the state
// allocation until multiple values have been received (buffering). Processing a buffer
// rather than individual values reduces cache misses and increases chances for SIMD.
#endif
#ifndef PAC_GODBOLT
static constexpr uint16_t BOUND_RECOMPUTE_INTERVAL = 2048;
#endif

// Comparison macros (IS_MAX must be a template parameter in scope)
#define PAC_IS_BETTER(a, b) (IS_MAX ? ((a) > (b)) : ((a) < (b)))
#define PAC_BETTER(a, b)    (PAC_IS_BETTER(a, b) ? (a) : (b))
#define PAC_WORSE(a, b)     (PAC_IS_BETTER(a, b) ? (b) : (a))

// SIMD-friendly branchless update (bit-select approach)
// Works for all sizes: 8-bit (SHIFTS=8), 16-bit (SHIFTS=16), 32-bit (SHIFTS=32), 64-bit (SHIFTS=64)
// FLOAT=true: for float/double, uses union to convert between T and UintT
// FLOAT=false: for integers, uses neutral value approach (SIGNED distinguishes signed vs unsigned)
template <typename T, typename UintT, typename BitsT, int SHIFTS, uint64_t MASK, bool IS_MAX, bool FLOAT, bool SIGNED>
#ifndef PAC_GODBOLT
AUTOVECTORIZE inline
#endif
    void
    UpdateExtremesSIMD(T *extremes, uint64_t key_hash, T value) {
	union {                   // union to deal with bit extraction of uint_t key_hash and differently sized T
		uint64_t u64[SHIFTS]; // an array of 64-bit uints that is equally big as 64 values of T
		BitsT bits[64];       // signed type with width equal to T (the type being aggregated)
	} buf;
	for (int i = 0; i < SHIFTS; i++) {       // fewer iterations than 64 here (SWAR)
		buf.u64[i] = (key_hash >> i) & MASK; // this operates on the uint64_t key_hash granularity
	}
	for (int i = 0; i < 64; i++) {                     // 64 iterations on 64 T values => hopefully will autovectorize
		UintT mask = static_cast<UintT>(-buf.bits[i]); // 1->0xFF..., 0->0x00
		if (FLOAT) {
			union {    // union to mitigate between type T and its bitwise representation (for 0x00/0xFF minmax masking)
				T val; // float or double, i.e. non-integer
				UintT bits; // because float is 32-bits, if T=float => UintT = uint32_t
			} extreme_u, result_u;
			extreme_u.val = extremes[i];
			result_u.val = PAC_BETTER(value, extreme_u.val);
			result_u.bits = (result_u.bits & mask) | (extreme_u.bits & ~mask); // masking on bits
			extremes[i] = result_u.val; // same value but now viewed as (multiple SWAR) values of type T
		} else {                        // integer path that directly mask on value
			UintT UNSIGNED_NOOP = (UintT(SIGNED) & ~mask) << (sizeof(T) * 8 - 1);
			UintT val_bits = static_cast<UintT>(value);
			UintT res_bits = IS_MAX ? ((val_bits & mask) | UNSIGNED_NOOP) : ((val_bits | ~mask) & ~UNSIGNED_NOOP);
			extremes[i] = PAC_BETTER(static_cast<T>(res_bits), extremes[i]); // operation on values
		}
	}
}

#ifdef PAC_GODBOLT
// Explicit instantiations for Godbolt analysis (omit hugeint)
// Template params: <T, UintT, BitsT, SHIFTS, MASK, IS_MAX, FLOAT, SIGNED>
template void UpdateExtremesSIMD<uint8_t, uint8_t, uint8_t, 8, 0x0101010101010101ULL, true, false, false>(uint8_t *,
                                                                                                          uint64_t,
                                                                                                          uint8_t);
template void
UpdateExtremesSIMD<int8_t, uint8_t, int8_t, 8, 0x0101010101010101ULL, false, false, true>(int8_t *, uint64_t, int8_t);
template void
UpdateExtremesSIMD<uint16_t, uint16_t, uint16_t, 16, 0x0001000100010001ULL, true, false, false>(uint16_t *, uint64_t,
                                                                                                uint16_t);
template void UpdateExtremesSIMD<int16_t, uint16_t, int16_t, 16, 0x0001000100010001ULL, false, false, true>(int16_t *,
                                                                                                            uint64_t,
                                                                                                            int16_t);
template void
UpdateExtremesSIMD<uint32_t, uint32_t, uint32_t, 32, 0x0000000100000001ULL, true, false, false>(uint32_t *, uint64_t,
                                                                                                uint32_t);
template void UpdateExtremesSIMD<int32_t, uint32_t, int32_t, 32, 0x0000000100000001ULL, false, false, true>(int32_t *,
                                                                                                            uint64_t,
                                                                                                            int32_t);
template void UpdateExtremesSIMD<uint64_t, uint64_t, uint64_t, 64, 1ULL, true, false, false>(uint64_t *, uint64_t,
                                                                                             uint64_t);
template void UpdateExtremesSIMD<int64_t, uint64_t, int64_t, 64, 1ULL, false, false, true>(int64_t *, uint64_t,
                                                                                           int64_t);
template void
UpdateExtremesSIMD<float, uint32_t, int32_t, 32, 0x0000000100000001ULL, true, true, false>(float *, uint64_t, float);
template void UpdateExtremesSIMD<double, uint64_t, int64_t, 64, 1ULL, true, true, false>(double *, uint64_t, double);
#else
// Specialization for hugeint_t - branchless 128-bit update
template <bool IS_MAX, bool SIGNED>
AUTOVECTORIZE inline void UpdateExtremesSIMD(hugeint_t *extremes, uint64_t key_hash, hugeint_t value) {
	for (int i = 0; i < 64; i++) {
		uint64_t mask = -((key_hash >> i) & 1);
		hugeint_t better = PAC_BETTER(value, extremes[i]);
		extremes[i].lower = (better.lower & mask) | (extremes[i].lower & ~mask);
		extremes[i].upper = static_cast<int64_t>((static_cast<uint64_t>(better.upper) & mask) |
		                                         (static_cast<uint64_t>(extremes[i].upper) & ~mask));
	}
}

// Specialization for uhugeint_t - branchless 128-bit update
template <bool IS_MAX, bool SIGNED>
AUTOVECTORIZE inline void UpdateExtremesSIMD(uhugeint_t *extremes, uint64_t key_hash, uhugeint_t value) {
	for (int i = 0; i < 64; i++) {
		uint64_t mask = -((key_hash >> i) & 1);
		uhugeint_t better = PAC_BETTER(value, extremes[i]);
		extremes[i].lower = (better.lower & mask) | (extremes[i].lower & ~mask);
		extremes[i].upper = (better.upper & mask) | (extremes[i].upper & ~mask);
	}
}
#endif

#ifndef PAC_GODBOLT
// Simple branching update for PAC_NOSIMD benchmarking
template <typename T, bool IS_MAX>
inline void UpdateExtremesScalar(T *extremes, uint64_t key_hash, T value) {
	constexpr idx_t SZ = sizeof(T) <= 8 ? sizeof(T) : 8;
	constexpr idx_t ELEMS = 8 / SZ;
	constexpr idx_t SHIFTS = 64 / ELEMS;
	for (int i = 0; i < 64; i++) {
		if ((key_hash >> i) & 1) { // IF..THEN cannot be simd-ized (and is 50%:has heavy branch misprediction cost)
			// Finalization expects the same interleaved layout as SIMD updates.
			auto &extreme = extremes[(i % SHIFTS) * ELEMS + i / SHIFTS];
			extreme = PAC_BETTER(value, extreme);
		}
	}
}

// ============================================================================
// Simple min/max state - T[64] extremes array
// ============================================================================
template <typename T, bool IS_MAX>
struct PacMinMaxState;
template <typename T, bool IS_MAX>
struct PacMinMaxNarrowState;

// The width tag occupies padding in the original metadata header. Payloads are
// separate, correctly sized C++ objects; wide states retain their original size.
template <typename T, bool IS_MAX>
struct PacMinMaxStateHeader {
	bool initialized;
	bool compact;
	uint64_t update_count;
	uint64_t key_hash;
	T global_bound;

	void Initialize();
	void Update(uint64_t h, T value);
	void RecomputeBound();
	T GetValue(idx_t bit) const;
	void GetTotals(PAC_FLOAT *dst) const;
	void GetValues(T *dst) const;
	void SetValues(const T *src);
	void CombineWith(const PacMinMaxStateHeader &src);
};

template <typename T, bool IS_MAX>
struct PacMinMaxState : PacMinMaxStateHeader<T, IS_MAX> {
	using PacMinMaxStateHeader<T, IS_MAX>::initialized;
	using PacMinMaxStateHeader<T, IS_MAX>::compact;
	using PacMinMaxStateHeader<T, IS_MAX>::update_count;
	using PacMinMaxStateHeader<T, IS_MAX>::key_hash;
	using PacMinMaxStateHeader<T, IS_MAX>::global_bound;
	using ValueType = T;

	static inline T InitValue() {
		if (std::is_floating_point<T>::value) {
			return IS_MAX ? -std::numeric_limits<T>::infinity() : std::numeric_limits<T>::infinity();
		}
		return IS_MAX ? NumericLimits<T>::Minimum() : NumericLimits<T>::Maximum();
	}

	T extremes[64];

	void Initialize() {
		compact = false;
		T init = InitValue();
		for (int i = 0; i < 64; i++) {
			extremes[i] = init;
		}
		global_bound = init;
		update_count = 0;
		key_hash = 0;
		initialized = true;
	}

	AUTOVECTORIZE void Update(uint64_t key_hash, T value);

	void RecomputeBound() {
#ifndef PAC_NOBOUNDOPT
		if ((++update_count & (BOUND_RECOMPUTE_INTERVAL - 1)) == 0) {
			T bound = extremes[0];
			for (int i = 1; i < 64; i++) {
				bound = PAC_WORSE(bound, extremes[i]);
			}
			global_bound = bound;
		}
#endif
	}

	// Convert extremes to sequential bit order, undoing the SWAR interleaving.
	// SWAR bit extraction maps: extremes[j] ← bit (j/ELEMS + (j%ELEMS)*SHIFTS) of key_hash
	// where ELEMS = 8/sizeof(T), SHIFTS = 64/ELEMS. We permute so dst[bit] = extremes[swar_pos(bit)].
	T GetValue(idx_t bit) const {
		constexpr idx_t SZ = sizeof(T) <= 8 ? sizeof(T) : 8;
		constexpr idx_t ELEMS = 8 / SZ;
		constexpr idx_t SHIFTS = 64 / ELEMS;
		return extremes[(bit % SHIFTS) * ELEMS + bit / SHIFTS];
	}
	void GetValues(T *dst) const {
		memcpy(dst, extremes, sizeof(extremes));
	}
	void SetValues(const T *src) {
		memcpy(extremes, src, sizeof(extremes));
	}
	void GetTotals(PAC_FLOAT *dst) const {
		constexpr int SZ = sizeof(T) <= 8 ? static_cast<int>(sizeof(T)) : 8;
		constexpr int ELEMS = 8 / SZ;
		constexpr int SHIFTS = 64 / ELEMS;
		for (int bit = 0; bit < 64; bit++) {
			int swar_pos = (bit % SHIFTS) * ELEMS + bit / SHIFTS;
			dst[bit] = static_cast<PAC_FLOAT>(ToDouble(extremes[swar_pos]));
		}
	}

	void CombineWith(const PacMinMaxState &src) {
		if (!src.initialized) {
			return;
		}
		if (!initialized) {
			Initialize();
		}
		key_hash |= src.key_hash;
		update_count += src.update_count;
		for (int i = 0; i < 64; i++) {
			extremes[i] = PAC_BETTER(extremes[i], src.extremes[i]);
		}
		RecomputeBound();
	}

	PacMinMaxState *GetState() {
		return this;
	}
	PacMinMaxState *EnsureState(ArenaAllocator &) {
		if (!initialized) {
			Initialize();
		}
		return this;
	}

	static idx_t StateSize() {
		return sizeof(PacMinMaxState);
	}
};

// ============================================================================
// Update specializations - instantiated where called
// PAC_NOSIMD: use simple branching scalar loop (for benchmarking)
// Default: use branchless SIMD-friendly updates
// ============================================================================

// Macro to define Update specializations for regular types (8/16/32/64-bit)
#ifdef PAC_NOSIMD
#define DEFINE_UPDATE(T, UINT_T, BITS_T, SHIFTS, MASK, FLOAT, SIGNED)                                                  \
	template <>                                                                                                        \
	inline void PacMinMaxState<T, true>::Update(uint64_t h, T v) {                                                     \
		UpdateExtremesScalar<T, true>(extremes, h, v);                                                                 \
	}                                                                                                                  \
	template <>                                                                                                        \
	inline void PacMinMaxState<T, false>::Update(uint64_t h, T v) {                                                    \
		UpdateExtremesScalar<T, false>(extremes, h, v);                                                                \
	}
#else
#define DEFINE_UPDATE(T, UINT_T, BITS_T, SHIFTS, MASK, FLOAT, SIGNED)                                                  \
	template <>                                                                                                        \
	inline void PacMinMaxState<T, true>::Update(uint64_t h, T v) {                                                     \
		UpdateExtremesSIMD<T, UINT_T, BITS_T, SHIFTS, MASK, true, FLOAT, SIGNED>(extremes, h, v);                      \
	}                                                                                                                  \
	template <>                                                                                                        \
	inline void PacMinMaxState<T, false>::Update(uint64_t h, T v) {                                                    \
		UpdateExtremesSIMD<T, UINT_T, BITS_T, SHIFTS, MASK, false, FLOAT, SIGNED>(extremes, h, v);                     \
	}
#endif

// Macro to define Update specializations for hugeint types (128-bit)
#ifdef PAC_NOSIMD
#define DEFINE_UPDATE_HUGE(T, SIGNED)                                                                                  \
	template <>                                                                                                        \
	inline void PacMinMaxState<T, true>::Update(uint64_t h, T v) {                                                     \
		UpdateExtremesScalar<T, true>(extremes, h, v);                                                                 \
	}                                                                                                                  \
	template <>                                                                                                        \
	inline void PacMinMaxState<T, false>::Update(uint64_t h, T v) {                                                    \
		UpdateExtremesScalar<T, false>(extremes, h, v);                                                                \
	}
#else
#define DEFINE_UPDATE_HUGE(T, SIGNED)                                                                                  \
	template <>                                                                                                        \
	inline void PacMinMaxState<T, true>::Update(uint64_t h, T v) {                                                     \
		UpdateExtremesSIMD<true, SIGNED>(extremes, h, v);                                                              \
	}                                                                                                                  \
	template <>                                                                                                        \
	inline void PacMinMaxState<T, false>::Update(uint64_t h, T v) {                                                    \
		UpdateExtremesSIMD<false, SIGNED>(extremes, h, v);                                                             \
	}
#endif

// 8-bit types (SWAR 8-way)
DEFINE_UPDATE(int8_t, uint8_t, int8_t, 8, 0x0101010101010101ULL, false, true)
DEFINE_UPDATE(uint8_t, uint8_t, int8_t, 8, 0x0101010101010101ULL, false, false)

// 16-bit types (SWAR 4-way)
DEFINE_UPDATE(int16_t, uint16_t, int16_t, 16, 0x0001000100010001ULL, false, true)
DEFINE_UPDATE(uint16_t, uint16_t, int16_t, 16, 0x0001000100010001ULL, false, false)

// 32-bit types (SWAR 2-way)
DEFINE_UPDATE(int32_t, uint32_t, int32_t, 32, 0x0000000100000001ULL, false, true)
DEFINE_UPDATE(uint32_t, uint32_t, int32_t, 32, 0x0000000100000001ULL, false, false)
DEFINE_UPDATE(float, uint32_t, int32_t, 32, 0x0000000100000001ULL, true, false)

// 64-bit types (SWAR 1-way)
DEFINE_UPDATE(int64_t, uint64_t, int64_t, 64, 1ULL, false, true)
DEFINE_UPDATE(uint64_t, uint64_t, int64_t, 64, 1ULL, false, false)
DEFINE_UPDATE(double, uint64_t, int64_t, 64, 1ULL, true, false)

// 128-bit types (hugeint)
DEFINE_UPDATE_HUGE(hugeint_t, true)
DEFINE_UPDATE_HUGE(uhugeint_t, false)

#undef DEFINE_UPDATE
#undef DEFINE_UPDATE_HUGE

// Only buffered 64-bit integer states use compact payloads. SQL input/output,
// bounds, and buffered values retain T; all other kernels keep the wide layout.
template <typename T, bool IS_MAX>
struct PacMinMaxNarrowState : PacMinMaxStateHeader<T, IS_MAX> {
	using Narrow = typename std::conditional<std::is_signed<T>::value, int8_t, uint8_t>::type;
	Narrow extremes[64];
};

template <typename T, bool IS_MAX>
void PacMinMaxStateHeader<T, IS_MAX>::Initialize() {
	if (!compact) {
		static_cast<PacMinMaxState<T, IS_MAX> *>(this)->Initialize();
		return;
	}
	using Small = PacMinMaxNarrowState<T, IS_MAX>;
	using N = typename Small::Narrow;
	auto *small = static_cast<Small *>(this);
	for (idx_t i = 0; i < 64; i++) {
		small->extremes[i] = IS_MAX ? NumericLimits<N>::Minimum() : NumericLimits<N>::Maximum();
	}
	global_bound = PacMinMaxState<T, IS_MAX>::InitValue();
	update_count = key_hash = 0;
	initialized = true;
}

template <typename T, bool IS_MAX>
void PacMinMaxStateHeader<T, IS_MAX>::Update(uint64_t h, T value) {
	if (!compact) {
		static_cast<PacMinMaxState<T, IS_MAX> *>(this)->Update(h, value);
		return;
	}
	using Small = PacMinMaxNarrowState<T, IS_MAX>;
	using N = typename Small::Narrow;
	auto *small = static_cast<Small *>(this);
#ifdef PAC_NOSIMD
	UpdateExtremesScalar<N, IS_MAX>(small->extremes, h, static_cast<N>(value));
#else
	UpdateExtremesSIMD<N, uint8_t, int8_t, 8, 0x0101010101010101ULL, IS_MAX, false, std::is_signed<T>::value>(
	    small->extremes, h, static_cast<N>(value));
#endif
}

template <typename T, bool IS_MAX>
T PacMinMaxStateHeader<T, IS_MAX>::GetValue(idx_t bit) const {
	if (!compact) {
		return static_cast<const PacMinMaxState<T, IS_MAX> *>(this)->GetValue(bit);
	}
	if (!((key_hash >> bit) & 1)) {
		return PacMinMaxState<T, IS_MAX>::InitValue();
	}
	auto *small = static_cast<const PacMinMaxNarrowState<T, IS_MAX> *>(this);
	return static_cast<T>(small->extremes[(bit % 8) * 8 + bit / 8]);
}

template <typename T, bool IS_MAX>
void PacMinMaxStateHeader<T, IS_MAX>::RecomputeBound() {
	if (!compact) {
		static_cast<PacMinMaxState<T, IS_MAX> *>(this)->RecomputeBound();
		return;
	}
#ifndef PAC_NOBOUNDOPT
	if ((++update_count & (BOUND_RECOMPUTE_INTERVAL - 1)) == 0) {
		T bound = GetValue(0);
		for (idx_t bit = 1; bit < 64; bit++) {
			bound = PAC_WORSE(bound, GetValue(bit));
		}
		global_bound = bound;
	}
#endif
}

template <typename T, bool IS_MAX>
void PacMinMaxStateHeader<T, IS_MAX>::GetTotals(PAC_FLOAT *dst) const {
	if (!compact) {
		static_cast<const PacMinMaxState<T, IS_MAX> *>(this)->GetTotals(dst);
		return;
	}
	auto *small = static_cast<const PacMinMaxNarrowState<T, IS_MAX> *>(this);
	const auto neutral = static_cast<PAC_FLOAT>(ToDouble(PacMinMaxState<T, IS_MAX>::InitValue()));
	for (idx_t bit = 0; bit < 64; bit++) {
		auto value = static_cast<PAC_FLOAT>(small->extremes[(bit % 8) * 8 + bit / 8]);
		dst[bit] = ((key_hash >> bit) & 1) ? value : neutral;
	}
}

template <typename T, bool IS_MAX>
void PacMinMaxStateHeader<T, IS_MAX>::GetValues(T *dst) const {
	// Header consumers are 64-bit integers, whose wide SWAR order is bit order.
	for (idx_t bit = 0; bit < 64; bit++) {
		dst[bit] = GetValue(bit);
	}
}

template <typename T, bool IS_MAX>
void PacMinMaxStateHeader<T, IS_MAX>::SetValues(const T *src) {
	D_ASSERT(!compact);
	static_cast<PacMinMaxState<T, IS_MAX> *>(this)->SetValues(src);
}

template <typename T, bool IS_MAX>
void PacMinMaxStateHeader<T, IS_MAX>::CombineWith(const PacMinMaxStateHeader &src) {
	D_ASSERT(!compact);
	auto &wide = *static_cast<PacMinMaxState<T, IS_MAX> *>(this);
	if (!src.compact) {
		wide.CombineWith(*static_cast<const PacMinMaxState<T, IS_MAX> *>(&src));
		return;
	}
	if (!src.initialized) {
		return;
	}
	key_hash |= src.key_hash;
	update_count += src.update_count;
	for (idx_t bit = 0; bit < 64; bit++) {
		wide.extremes[bit] = PAC_BETTER(wide.extremes[bit], src.GetValue(bit));
	}
	wide.RecomputeBound();
}

// ============================================================================
// PacMinMaxUpdateOne: direct state update (always available)
// ============================================================================
template <typename T, bool IS_MAX>
inline void PacMinMaxUpdateOne(PacMinMaxState<T, IS_MAX> &state, uint64_t key_hash, T value, ArenaAllocator &a) {
	state.EnsureState(a);
	state.key_hash |= key_hash;
#ifndef PAC_NOBOUNDOPT
	if (!PAC_IS_BETTER(value, state.global_bound)) {
		return;
	}
#endif
	state.Update(key_hash, value);
	state.RecomputeBound();
}

#ifndef PAC_NOBUFFERING
// ============================================================================
// PacMinMaxStateWrapper: buffers (hash, value) pairs before allocating state
// Buffer size: 2 for types <= 8 bytes, 1 for hugeint (16 bytes)
// ============================================================================
template <typename T, bool IS_MAX>
struct PacMinMaxStateWrapper {
	static constexpr bool COMPACT = std::is_integral<T>::value && sizeof(T) == 8;
	using Wide = PacMinMaxState<T, IS_MAX>;
	using Small = PacMinMaxNarrowState<T, IS_MAX>;
	using State = typename std::conditional<COMPACT, PacMinMaxStateHeader<T, IS_MAX>, Wide>::type;
	static constexpr int BUF_SIZE = sizeof(T) <= 8 ? 2 : 1;
	static constexpr uint64_t BUF_MASK = sizeof(T) <= 8 ? 3ULL : 1ULL;

	T val_buf[BUF_SIZE];
	uint64_t hash_buf[BUF_SIZE];
	union {
		uint64_t n_buffered;
		State *state;
	};

	State *GetState() const {
		return reinterpret_cast<State *>(reinterpret_cast<uintptr_t>(state) & ~7ULL);
	}

	State *EnsureState(ArenaAllocator &a, const T *values = nullptr, idx_t count = 0) {
		State *s = GetState();
		if constexpr (COMPACT) {
			if (!s || s->compact) {
				bool fits = values != nullptr;
				using N = typename Small::Narrow;
				for (idx_t i = 0; fits && i < count; i++) {
					fits = values[i] >= static_cast<T>(NumericLimits<N>::Minimum()) &&
					       values[i] <= static_cast<T>(NumericLimits<N>::Maximum());
				}
				if (!s) {
					if (fits) {
						auto *narrow = new (a.AllocateAligned(sizeof(Small))) Small {};
						narrow->compact = true;
						s = narrow;
					} else {
						s = new (a.AllocateAligned(sizeof(Wide))) Wide {};
					}
					state = s;
				} else if (!fits) {
					auto *wide = new (a.AllocateAligned(sizeof(Wide))) Wide {};
					wide->Initialize();
					wide->key_hash = s->key_hash;
					wide->update_count = s->update_count;
					wide->global_bound = s->global_bound;
					for (idx_t bit = 0; bit < 64; bit++) {
						wide->extremes[bit] = s->GetValue(bit);
					}
					auto buffered = n_buffered & BUF_MASK;
					state = s = wide;
					n_buffered |= buffered;
				}
			}
		} else if (!s) {
			s = reinterpret_cast<State *>(a.Allocate(sizeof(State)));
			memset(s, 0, sizeof(State));
			state = s;
		}
		if (!s->initialized) {
			s->Initialize();
		}
		return s;
	}

	AUTOVECTORIZE static inline void FlushBufferInternal(State &dst, const T *PAC_RESTRICT vals,
	                                                     const uint64_t *PAC_RESTRICT hashes, uint64_t cnt) {
		for (uint64_t i = 0; i < cnt; i++) {
			dst.Update(hashes[i], vals[i]);
		}
		dst.RecomputeBound();
	}

	inline void FlushBuffer(PacMinMaxStateWrapper &dst_wrapper, ArenaAllocator &a) {
		// Flush THIS wrapper's buffer into dst_wrapper's inner state
		uint64_t cnt = n_buffered & BUF_MASK;
		if (cnt > 0) {
			State &dst = *dst_wrapper.EnsureState(a, val_buf, cnt);
			// OR the buffered hash values into key_hash
			for (uint64_t i = 0; i < cnt; i++) {
				dst.key_hash |= hash_buf[i];
			}
			FlushBufferInternal(dst, val_buf, hash_buf, cnt);
			n_buffered &= ~BUF_MASK;
		}
	}
};

// Wrapper update with buffering (for scatter/grouped updates)
template <typename T, bool IS_MAX>
AUTOVECTORIZE inline void PacMinMaxBufferOrUpdateOne(PacMinMaxStateWrapper<T, IS_MAX> &agg, uint64_t key_hash, T value,
                                                     ArenaAllocator &a) {
	using Wrapper = PacMinMaxStateWrapper<T, IS_MAX>;
	using State = typename Wrapper::State;

#ifndef PAC_NOBOUNDOPT
	// Bounds check if we already have a state with a valid bound
	// Neutral-valued rows can still introduce previously unseen worlds.
	State *s = agg.GetState();
	if (s && s->initialized && !PAC_IS_BETTER(value, s->global_bound) && (s->key_hash & key_hash) == key_hash) {
		return;
	}
#endif
	uint64_t cnt = agg.n_buffered & Wrapper::BUF_MASK;
	if (DUCKDB_UNLIKELY(cnt == Wrapper::BUF_SIZE)) {
		T vals[Wrapper::BUF_SIZE + 1];
		uint64_t hashes[Wrapper::BUF_SIZE + 1];
		for (int i = 0; i < Wrapper::BUF_SIZE; i++) {
			vals[i] = agg.val_buf[i];
			hashes[i] = agg.hash_buf[i];
		}
		vals[Wrapper::BUF_SIZE] = value;
		hashes[Wrapper::BUF_SIZE] = key_hash;
		State &dst = *agg.EnsureState(a, vals, Wrapper::BUF_SIZE + 1);
		for (idx_t i = 0; i <= Wrapper::BUF_SIZE; i++) {
			dst.key_hash |= hashes[i];
		}
		Wrapper::FlushBufferInternal(dst, vals, hashes, Wrapper::BUF_SIZE + 1);
		agg.n_buffered &= ~Wrapper::BUF_MASK;
	} else {
		agg.val_buf[cnt] = value;
		agg.hash_buf[cnt] = key_hash;
		agg.n_buffered++;
	}
}

// Wrapper update without buffering (for non-grouped updates)
template <typename T, bool IS_MAX>
inline void PacMinMaxUpdateOne(PacMinMaxStateWrapper<T, IS_MAX> &agg, uint64_t key_hash, T value, ArenaAllocator &a) {
	PacMinMaxUpdateOne(*static_cast<PacMinMaxState<T, IS_MAX> *>(agg.EnsureState(a)), key_hash, value, a);
}
#endif // PAC_NOBUFFERING

} // namespace duckdb
#endif // PAC_GODBOLT

#endif // PAC_MIN_MAX_HPP
