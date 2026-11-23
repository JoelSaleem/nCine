#ifndef CLASS_NCINE_CLOCK
#define CLASS_NCINE_CLOCK

#include <cstdint>
#include <nctl/Atomic.h>

namespace ncine {

/// The class interfacing with the system clock
class Clock
{
  public:
	/// Returns elapsed time in ticks since base time, or the virtual time when it is enabled
	inline uint64_t now() const { return isVirtual_ ? static_cast<uint64_t>(virtualNow_.load()) : counter() - baseCount_; }

	/// Returns true when time only moves through `advanceVirtual()` (enabled by `NCINE_CAPTURE`)
	inline bool isVirtual() const { return isVirtual_; }
	/// Moves the virtual time forward by the specified number of ticks
	inline void advanceVirtual(uint64_t ticks) { virtualNow_.fetchAdd(static_cast<int64_t>(ticks)); }

	/// Returns current value of the counter
	uint64_t counter() const;

	/// Returns the counter frequency in counts per second
	inline uint32_t frequency() const { return frequency_; }
	/// Retruns the counter value at initialization time
	inline uint64_t baseCount() const { return baseCount_; }

  private:
#if defined(_WIN32)
	bool hasPerfCounter_ = false;
#elif !defined(__APPLE__)
	bool hasMonotonicClock_ = false;
#endif

	/// Counter frequency in counts per second
	uint32_t frequency_;
	/// Counter value at initialization time
	uint64_t baseCount_;
	/// Frame captures need every timestamp to be reproducible, so they run on a virtual time starting at zero
	bool isVirtual_;
	nctl::Atomic64 virtualNow_;

	/// Private constructor
	Clock();

	friend Clock &clock();
};

// Meyers' Singleton
extern Clock &clock();

}

#endif
