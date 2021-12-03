#include <thor-internal/arch/cpu.hpp>
#include <thor-internal/arch/ints.hpp>
#include <thor-internal/arch/system.hpp>

namespace thor {

void initializeArchitecture() {
	setupBootCpuContext();
	setupEarlyInterruptHandlers();
}

} // namespace thor
