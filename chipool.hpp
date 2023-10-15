#pragma once
#include <concepts>
#include <functional>
#include <memory>
#if defined(_WIN32) || defined(_WIN64)
#include <Windows.h>
#endif

#if defined(_WIN32) || defined(_WIN64)
#ifndef HAS_DEFAULT_ALLOCATE
#define HAS_DEFAULT_ALLOCATE
#endif // !HAS_DEFAULT_ALLOCATE
#endif

#if defined __clang__
#define CHIPOOL_FORCE_INLINE __attribute__((always_inline)) inline
#elif defined _MSC_VER
#define CHIPOOL_FORCE_INLINE __forceinline
#else
#define CHIPOOL_FORCE_INLINE __attribute__((always_inline)) inline
#endif

namespace chipool {
namespace detail {
constexpr size_t kPageSize = 4096;
constexpr uintptr_t kPageMask = ~0xFFFull;
constexpr uint16_t kInvalidIdx = ~0;

template<class T>
struct SubPool {
	union Chip {
		std::conditional_t<sizeof(T) == 1, uint8_t, uint16_t> next_idx;
		T value;
	};
	struct HeaderNot1Byte {
		uint16_t free_idx;
		uint16_t begin_idx;
		uint16_t used_count;
		uint16_t reserve;
		SubPool* next_pool;
#if defined(_WIN32) && !defined(_WIN64)
		uint32_t reserve2;
#endif
	};
	struct Header1Byte {
		uint16_t free_idx;
		uint16_t begin_idx;
		uint16_t used_count;
		uint16_t reserve;
		SubPool* next_pool;
	};
	using Header = std::conditional_t<sizeof(T) == 1, Header1Byte, HeaderNot1Byte>;

	static constexpr int kUsableSize = (sizeof(T) == 1 ? (kPageSize / 8) : kPageSize) - sizeof(Header);
	static constexpr int kChipCount = kUsableSize / sizeof(Chip);

	Header header;
	Chip usable_mems[kChipCount];

	CHIPOOL_FORCE_INLINE bool IsFreeIdxValid() noexcept {
		return header.free_idx != kInvalidIdx;
	}

	CHIPOOL_FORCE_INLINE bool IsNotBeginInEnd() noexcept {
		return header.begin_idx != kChipCount;
	}

	CHIPOOL_FORCE_INLINE Chip& FreeMem() noexcept {
		return usable_mems[header.free_idx];
	}

	CHIPOOL_FORCE_INLINE Chip& BeginMem() noexcept {
		return usable_mems[header.begin_idx];
	}

	CHIPOOL_FORCE_INLINE bool IsFull() noexcept {
		return header.used_count == kChipCount;
	}

	CHIPOOL_FORCE_INLINE bool IsEmpty() noexcept {
		return header.used_count == 0;
	}
};

template<class T>
constexpr bool is_valid_chip_v = (SubPool<T>::kUsableSize % sizeof(T)) == 0;

#if defined(_WIN32) || defined(_WIN64)
CHIPOOL_FORCE_INLINE void* Allocate(size_t size) noexcept {
	return VirtualAlloc(NULL, size, MEM_COMMIT, PAGE_READWRITE);
}
CHIPOOL_FORCE_INLINE void Deallocate(void* ptr) noexcept {
	VirtualFree(ptr, NULL, MEM_RELEASE);
}
#endif
}

template<class T>
	requires detail::is_valid_chip_v<T>
class Pool
{
public:
	using AllocFunc = std::function<void*(size_t size)>;
	using DeallocFunc = std::function<void(void* ptr)>;

#ifdef HAS_DEFAULT_ALLOCATE
	Pool() noexcept :
		allocate_(detail::Allocate),
		deallocate_(detail::Deallocate),
		cur_pool_(nullptr),
		used_chip_count_(0) {}
#endif // HAS_DEFAULT_ALLOCATE

	Pool(const AllocFunc& allocate, const DeallocFunc& deallocate) noexcept :
		allocate_(allocate),
		deallocate_(deallocate),
		cur_pool_(nullptr),
		used_chip_count_(0) {}

	~Pool() {}

	T* Allocate() noexcept {
		if (!cur_pool_) cur_pool_ = AllocSubPool();
		if (cur_pool_->IsFull()) {
			cur_pool_ = cur_pool_->header.next_pool ? cur_pool_->header.next_pool : AllocSubPool();
		}
		++cur_pool_->header.used_count;
		if (cur_pool_->IsFreeIdxValid()) {
			decltype(auto) res = cur_pool_->FreeMem();
			cur_pool_->header.free_idx = res.next_idx;
			return &res.value;
		}
		else {
			decltype(auto) res = cur_pool_->BeginMem();
			++cur_pool_->header.begin_idx;
			return &res.value;
		}
	}

	void Deallocate(T* ptr) noexcept {
		auto pool = (SubPool*)((uintptr_t)ptr & detail::kPageMask);
		auto chip = reinterpret_cast<SubPool::Chip*>(ptr);
		auto idx = ((uintptr_t)ptr - (uintptr_t)pool->usable_mems) / sizeof(SubPool::Chip);

		if (pool->IsFull()) {
			if (!cur_pool_->IsFull()) {
				pool->header.next_pool = cur_pool_;
			}
			cur_pool_ = pool;
		}
		--pool->header.used_count;
		if (pool->IsEmpty()) {
			pool->header.free_idx = detail::kInvalidIdx;
			pool->header.begin_idx = 0;
		}
		else {
			chip->next_idx = pool->header.free_idx;
			pool->header.free_idx = idx;
		}
	}

private:
	using SubPool = detail::SubPool<T>;

	CHIPOOL_FORCE_INLINE SubPool* AllocSubPool() noexcept {
		auto pool = (SubPool*)allocate_(detail::kPageSize);
		if constexpr (sizeof(T) == 1) {
			constexpr int pool_num = detail::kPageSize / sizeof(SubPool);
			auto tmp = pool;
			for (size_t i = 1; i < pool_num; i++) {
				tmp->header.free_idx = detail::kInvalidIdx;
				tmp->header.next_pool = tmp + 1;
				++tmp;
			}
			tmp->header.free_idx = detail::kInvalidIdx;
		}
		else {
			pool->header.free_idx = detail::kInvalidIdx;
		}
		return pool;
	}

	SubPool* cur_pool_;
	AllocFunc allocate_;
	DeallocFunc deallocate_;
	size_t used_chip_count_;

};
}