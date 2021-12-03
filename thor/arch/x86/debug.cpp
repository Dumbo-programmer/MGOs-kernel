#include <arch/bits.hpp>
#include <arch/io_space.hpp>
#include <arch/register.hpp>
#include <thor-internal/arch/debug.hpp>

namespace thor {

constinit PIOLogHandler pioLogHandler;

inline constexpr arch::scalar_register<uint8_t> data(0);
inline constexpr arch::scalar_register<uint8_t> baudLow(0);
inline constexpr arch::scalar_register<uint8_t> baudHigh(1);
inline constexpr arch::bit_register<uint8_t> lineControl(3);
inline constexpr arch::bit_register<uint8_t> lineStatus(5);

inline constexpr arch::field<uint8_t, bool> txReady(5, 1);

inline constexpr arch::field<uint8_t, int> dataBits(0, 2);
inline constexpr arch::field<uint8_t, bool> stopBit(2, 1);
inline constexpr arch::field<uint8_t, int> parityBits(3, 3);
inline constexpr arch::field<uint8_t, bool> dlab(7, 1);

extern bool debugToSerial;
extern bool debugToBochs;

void setupDebugging() {
	if(debugToSerial) {
		auto base = arch::global_io.subspace(0x3F8);

		// Set the baud rate.
		base.store(lineControl, dlab(true));
		base.store(baudLow, 0x01);
		base.store(baudHigh, 0x00);

		// Configure: 8 data bits, 1 stop bit, no parity.
		base.store(lineControl, dataBits(3) | stopBit(0) | parityBits(0) | dlab(false));
	}

	enableLogHandler(&pioLogHandler);
}

void PIOLogHandler::printChar(char c) {
	auto sendByteSerial = [this](uint8_t val) {
		auto base = arch::global_io.subspace(0x3F8);

		serialBuffer[serialBufferIndex++] = val;
		if (serialBufferIndex == 16) {
			while(!(base.load(lineStatus) & txReady)) {
				// do nothing until the UART is ready to transmit.
			}
			base.store_iterative(data, serialBuffer, 16);
			serialBufferIndex = 0;
		}
	};

	if(debugToSerial) {
		if(c == '\n') {
			sendByteSerial('\r');
		}

		sendByteSerial(c);
	}

	if(debugToBochs) {
		auto base = arch::global_io.subspace(0xE9);
		base.store(data, c);
	}
}

} // namespace thor
