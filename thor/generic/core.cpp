#include <thor-internal/cpu-data.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/kasan.hpp>
#include <thor-internal/kernel-io.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/ring-buffer.hpp>

// This is required for virtual destructors. It should not be called though.
void operator delete(void *, size_t) {
	thor::panicLogger() << "thor: operator delete() called" << frg::endlog;
}

namespace thor {

size_t kernelVirtualUsage = 0;
size_t kernelMemoryUsage = 0;

// --------------------------------------------------------
// Locking primitives
// --------------------------------------------------------

void IrqSpinlock::lock() {
	irqMutex().lock();
	_spinlock.lock();
}

void IrqSpinlock::unlock() {
	_spinlock.unlock();
	irqMutex().unlock();
}

// --------------------------------------------------------
// Memory management
// --------------------------------------------------------

KernelVirtualMemory::KernelVirtualMemory() {
	// The size is chosen arbitrarily here; 2 GiB of kernel heap is sufficient for now.
	uintptr_t vmBase = 0xFFFF'E000'0000'0000;
	size_t desiredSize = 0x8000'0000;

	// Setup a buddy allocator.
	auto tableOrder = BuddyAccessor::suitableOrder(desiredSize >> kPageShift);
	auto guessedRoots = desiredSize >> (kPageShift + tableOrder);
	auto overhead = BuddyAccessor::determineSize(guessedRoots, tableOrder);
	overhead = (overhead + (kPageSize - 1)) & ~(kPageSize - 1);

	size_t availableSize = desiredSize - overhead;
	auto availableRoots = availableSize >> (kPageShift + tableOrder);

	for(size_t pg = 0; pg < overhead; pg += kPageSize) {
		PhysicalAddr physical = physicalAllocator->allocate(0x1000);
		assert(physical != static_cast<PhysicalAddr>(-1) && "OOM");
		KernelPageSpace::global().mapSingle4k(vmBase + availableSize + pg, physical,
				page_access::write, CachingMode::null);
	}
	auto tablePtr = reinterpret_cast<int8_t *>(vmBase + availableSize);
	unpoisonKasanShadow(tablePtr, overhead);
	BuddyAccessor::initialize(tablePtr, availableRoots, tableOrder);

	buddy_ = BuddyAccessor{vmBase, kPageShift,
				tablePtr, availableRoots, tableOrder};
}

void *KernelVirtualMemory::allocate(size_t length) {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&mutex_);

	// TODO: use a smarter implementation here.
	int order = 0;
	while(length > (size_t{1} << (kPageShift + order)))
		++order;

	if(order > buddy_.tableOrder())
		panicLogger() << "\e[31m" "thor: Kernel virtual memory allocation is too large"
				" to be satisfied (order " << order << " while buddy order is "
				<< buddy_.tableOrder() << ")" "\e[39m" << frg::endlog;

	auto address = buddy_.allocate(order, 64);
	if(address == BuddyAccessor::illegalAddress) {
		infoLogger() << "thor: Failed to allocate 0x" << frg::hex_fmt(length)
				<< " bytes of kernel virtual memory" << frg::endlog;
		infoLogger() << "thor:"
				" Physical usage: " << (physicalAllocator->numUsedPages() * 4) << " KiB,"
				" kernel VM: " << (kernelVirtualUsage / 1024) << " KiB"
				" kernel RSS: " << (kernelMemoryUsage / 1024) << " KiB"
				<< frg::endlog;
		panicLogger() << "\e[31m" "thor: Out of kernel virtual memory" "\e[39m"
				<< frg::endlog;
	}
	kernelVirtualUsage += (size_t{1} << (kPageShift + order));

	auto pointer = reinterpret_cast<void *>(address);
	unpoisonKasanShadow(pointer, size_t{1} << (kPageShift + order));

	return pointer;
}

void KernelVirtualMemory::deallocate(void *pointer, size_t length) {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&mutex_);

	// TODO: use a smarter implementation here.
	int order = 0;
	while(length > (size_t{1} << (kPageShift + order)))
		++order;

	poisonKasanShadow(pointer, size_t{1} << (kPageShift + order));
	buddy_.free(reinterpret_cast<uintptr_t>(pointer), order);
	assert(kernelVirtualUsage >= (size_t{1} << (kPageShift + order)));
	kernelVirtualUsage -= (size_t{1} << (kPageShift + order));
}

frg::manual_box<KernelVirtualMemory> kernelVirtualMemory;

KernelVirtualMemory &KernelVirtualMemory::global() {
	// TODO: This should be initialized at a well-defined stage in the
	// kernel's boot process.
	if(!kernelVirtualMemory)
		kernelVirtualMemory.initialize();
	return *kernelVirtualMemory;
}

KernelVirtualAlloc::KernelVirtualAlloc() { }

uintptr_t KernelVirtualAlloc::map(size_t length) {
	auto p = KernelVirtualMemory::global().allocate(length);

	// TODO: The slab_pool unpoisons memory before calling this.
	//       It would be better not to unpoison in the kernel's VMM code.
	poisonKasanShadow(p, length);

	for(size_t offset = 0; offset < length; offset += kPageSize) {
		PhysicalAddr physical = physicalAllocator->allocate(kPageSize);
		assert(physical != static_cast<PhysicalAddr>(-1) && "OOM");
		KernelPageSpace::global().mapSingle4k(VirtualAddr(p) + offset, physical,
				page_access::write, CachingMode::null);
	}
	kernelMemoryUsage += length;

	return uintptr_t(p);
}

void KernelVirtualAlloc::unmap(uintptr_t address, size_t length) {
	assert((address % kPageSize) == 0);
	assert((length % kPageSize) == 0);

	// TODO: The slab_pool poisons memory before calling this.
	//       It would be better not to poison in the kernel's VMM code.
	unpoisonKasanShadow(reinterpret_cast<void *>(address), length);

	for(size_t offset = 0; offset < length; offset += kPageSize) {
		PhysicalAddr physical = KernelPageSpace::global().unmapSingle4k(address + offset);
		physicalAllocator->free(physical, kPageSize);
	}
	kernelMemoryUsage -= length;

	struct Closure final : ShootNode {
		void complete() override {
			KernelVirtualMemory::global().deallocate(reinterpret_cast<void *>(address), size);
			auto physical = thisPage;
			Closure::~Closure();
			asm volatile ("" : : : "memory");
			physicalAllocator->free(physical, kPageSize);
		}

		PhysicalAddr thisPage;
	};
	static_assert(sizeof(Closure) <= kPageSize);

	// We need some memory to store the closure that waits until shootdown completes.
	// For now, our stategy consists of allocating one page of *physical* memory
	// and accessing it through the global physical mapping.
	auto physical = physicalAllocator->allocate(kPageSize);
	PageAccessor accessor{physical};
	auto p = new (accessor.get()) Closure;
	p->thisPage = physical;
	p->address = address;
	p->size = length;
	if(KernelPageSpace::global().submitShootdown(p))
		p->complete();
}

frg::manual_box<LogRingBuffer> allocLog;

namespace {
	initgraph::Task initAllocTraceSink{&globalInitEngine, "generic.init-alloc-trace-sink",
		initgraph::Requires{getFibersAvailableStage(),
			getIoChannelsDiscoveredStage()},
		[] {
#ifndef KERNEL_LOG_ALLOCATIONS
			return;
#endif // KERNEL_LOG_ALLOCATIONS

			auto channel = solicitIoChannel("kernel-alloc-trace");
			if(channel) {
				infoLogger() << "thor: Connecting alloc-trace to I/O channel" << frg::endlog;
				async::detach_with_allocator(*kernelAlloc,
						dumpRingToChannel(allocLog.get(), std::move(channel), 2048));
			}
		}
	};
}

void KernelVirtualAlloc::unpoison(void *pointer, size_t size) {
	unpoisonKasanShadow(pointer, size);
}

void KernelVirtualAlloc::unpoison_expand(void *pointer, size_t size) {
	cleanKasanShadow(pointer, size);
}

void KernelVirtualAlloc::poison(void *pointer, size_t size) {
	poisonKasanShadow(pointer, size);
}

void KernelVirtualAlloc::output_trace(void *buffer, size_t size) {
	if (!allocLog)
		allocLog.initialize(0xFFFF'F000'0000'0000, 268435456);

	allocLog->enqueue(buffer, size);
}

constinit frg::manual_box<PhysicalChunkAllocator> physicalAllocator = {};

constinit frg::manual_box<KernelVirtualAlloc> kernelVirtualAlloc = {};

constinit frg::manual_box<
	frg::slab_pool<
		KernelVirtualAlloc,
		IrqSpinlock
	>
> kernelHeap = {};

constinit frg::manual_box<KernelAlloc> kernelAlloc = {};

// --------------------------------------------------------
// CpuData
// --------------------------------------------------------

ExecutorContext::ExecutorContext() { }

CpuData::CpuData()
: scheduler{this}, activeFiber{nullptr}, heartbeat{0} { }

} // namespace thor
