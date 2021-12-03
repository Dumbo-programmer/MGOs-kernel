#include <frg/string.hpp>

#include <thor-internal/universe.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/kerncfg.hpp>
#include <thor-internal/ostrace.hpp>
#include <thor-internal/profile.hpp>
#include <thor-internal/stream.hpp>
#include <thor-internal/timer.hpp>

#include "kerncfg.frigg_pb.hpp"
#include "mbus.frigg_pb.hpp"

#include <thor-internal/ring-buffer.hpp>

namespace thor {

extern frg::manual_box<LaneHandle> mbusClient;
extern frg::manual_box<frg::string<KernelAlloc>> kernelCommandLine;
extern frg::manual_box<LogRingBuffer> allocLog;

namespace {

coroutine<Error> handleReq(LaneHandle boundLane) {
	auto [acceptError, lane] = co_await AcceptSender{boundLane};
	if(acceptError != Error::success)
		co_return acceptError;

	auto [reqError, reqBuffer] = co_await RecvBufferSender{lane};
	assert(reqError == Error::success && "Unexpected mbus transaction");
	managarm::kerncfg::CntRequest<KernelAlloc> req(*kernelAlloc);
	req.ParseFromArray(reqBuffer.data(), reqBuffer.size());

	if(req.req_type() == managarm::kerncfg::CntReqType::GET_CMDLINE) {
		managarm::kerncfg::SvrResponse<KernelAlloc> resp(*kernelAlloc);
		resp.set_error(managarm::kerncfg::Error::SUCCESS);
		resp.set_size(kernelCommandLine->size());

		frg::string<KernelAlloc> ser(*kernelAlloc);
		resp.SerializeToString(&ser);
		frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
		memcpy(respBuffer.data(), ser.data(), ser.size());
		auto respError = co_await SendBufferSender{lane, std::move(respBuffer)};
		assert(respError == Error::success && "Unexpected mbus transaction");
		frg::unique_memory<KernelAlloc> cmdlineBuffer{*kernelAlloc, kernelCommandLine->size()};
		memcpy(cmdlineBuffer.data(), kernelCommandLine->data(), kernelCommandLine->size());
		auto cmdlineError = co_await SendBufferSender{lane, std::move(cmdlineBuffer)};
		assert(cmdlineError == Error::success && "Unexpected mbus transaction");
	}else{
		managarm::kerncfg::SvrResponse<KernelAlloc> resp(*kernelAlloc);
		resp.set_error(managarm::kerncfg::Error::ILLEGAL_REQUEST);

		frg::string<KernelAlloc> ser(*kernelAlloc);
		resp.SerializeToString(&ser);
		frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
		memcpy(respBuffer.data(), ser.data(), ser.size());
		auto respError = co_await SendBufferSender{lane, std::move(respBuffer)};
		assert(respError == Error::success && "Unexpected mbus transaction");
	}

	co_return Error::success;
}

coroutine<Error> handleByteRingReq(LogRingBuffer *ringBuffer, LaneHandle boundLane) {
	auto [acceptError, lane] = co_await AcceptSender{boundLane};
	if(acceptError != Error::success)
		co_return acceptError;

	auto [reqError, reqBuffer] = co_await RecvBufferSender{lane};
	assert(reqError == Error::success && "Unexpected mbus transaction");
	managarm::kerncfg::CntRequest<KernelAlloc> req(*kernelAlloc);
	req.ParseFromArray(reqBuffer.data(), reqBuffer.size());

	if(req.req_type() == managarm::kerncfg::CntReqType::GET_BUFFER_CONTENTS) {
		frg::unique_memory<KernelAlloc> dataBuffer{*kernelAlloc, req.size()};

		size_t progress = 0;

		// Extract the first record. We stop on success.
		uint64_t effectivePtr;
		uint64_t currentPtr;
		while(true) {
			auto [success, recordPtr, nextPtr, actualSize] = ringBuffer->dequeueAt(
					req.dequeue(), dataBuffer.data(), req.size());
			if(success) {
				assert(actualSize); // For now, we do not support size zero records.
				if(actualSize == req.size())
					infoLogger() << "thor: kerncfg truncates a ring buffer record" << frg::endlog;
				effectivePtr = recordPtr;
				currentPtr = nextPtr;
				progress += actualSize;
				break;
			}

			co_await ringBuffer->wait(nextPtr);
		}

		// Extract further records. We stop on failure, or if we miss records.
		while(true) {
			auto [success, recordPtr, nextPtr, actualSize] = ringBuffer->dequeueAt(
					currentPtr, static_cast<std::byte *>(dataBuffer.data()) + progress,
					req.size() - progress);
			if(recordPtr != currentPtr)
				break;
			if(success) {
				assert(actualSize); // For now, we do not support size zero records.
				if(actualSize == req.size() - progress)
					break;
				currentPtr = nextPtr;
				progress += actualSize;
				continue;
			}

			if(progress >= req.watermark())
				break;

			co_await ringBuffer->wait(nextPtr);
		}

		managarm::kerncfg::SvrResponse<KernelAlloc> resp(*kernelAlloc);
		resp.set_error(managarm::kerncfg::Error::SUCCESS);
		resp.set_size(progress);
		resp.set_effective_dequeue(effectivePtr);
		resp.set_new_dequeue(currentPtr);

		frg::string<KernelAlloc> ser(*kernelAlloc);
		resp.SerializeToString(&ser);
		frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
		memcpy(respBuffer.data(), ser.data(), ser.size());
		auto respError = co_await SendBufferSender{lane, std::move(respBuffer)};
		assert(respError == Error::success && "Unexpected mbus transaction");
		auto cmdlineError = co_await SendBufferSender{lane, std::move(dataBuffer)};
		assert(cmdlineError == Error::success && "Unexpected mbus transaction");
	}else{
		managarm::kerncfg::SvrResponse<KernelAlloc> resp(*kernelAlloc);
		resp.set_error(managarm::kerncfg::Error::ILLEGAL_REQUEST);

		frg::string<KernelAlloc> ser(*kernelAlloc);
		resp.SerializeToString(&ser);
		frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
		memcpy(respBuffer.data(), ser.data(), ser.size());
		auto respError = co_await SendBufferSender{lane, std::move(respBuffer)};
		assert(respError == Error::success && "Unexpected mbus transaction");
	}

	co_return Error::success;
}


} // anonymous namespace

// ------------------------------------------------------------------------
// mbus object creation and management.
// ------------------------------------------------------------------------

namespace {

coroutine<void> handleBind(LaneHandle objectLane);
coroutine<void> handleByteRingBind(LogRingBuffer *ringBuffer, LaneHandle objectLane);

coroutine<void> createObject(LaneHandle mbusLane) {
	auto [offerError, lane] = co_await OfferSender{mbusLane};
	assert(offerError == Error::success && "Unexpected mbus transaction");

	managarm::mbus::Property<KernelAlloc> cls_prop(*kernelAlloc);
	cls_prop.set_name(frg::string<KernelAlloc>(*kernelAlloc, "class"));
	auto &cls_item = cls_prop.mutable_item().mutable_string_item();
	cls_item.set_value(frg::string<KernelAlloc>(*kernelAlloc, "kerncfg"));

	managarm::mbus::CntRequest<KernelAlloc> req(*kernelAlloc);
	req.set_req_type(managarm::mbus::CntReqType::CREATE_OBJECT);
	req.set_parent_id(1);
	req.add_properties(std::move(cls_prop));

	frg::string<KernelAlloc> ser(*kernelAlloc);
	req.SerializeToString(&ser);
	frg::unique_memory<KernelAlloc> reqBuffer{*kernelAlloc, ser.size()};
	memcpy(reqBuffer.data(), ser.data(), ser.size());
	auto reqError = co_await SendBufferSender{lane, std::move(reqBuffer)};
	assert(reqError == Error::success && "Unexpected mbus transaction");

	auto [respError, respBuffer] = co_await RecvBufferSender{lane};
	assert(respError == Error::success && "Unexpected mbus transaction");
	managarm::mbus::SvrResponse<KernelAlloc> resp(*kernelAlloc);
	resp.ParseFromArray(respBuffer.data(), respBuffer.size());
	assert(resp.error() == managarm::mbus::Error::SUCCESS);

	auto [objectError, objectDescriptor] = co_await PullDescriptorSender{lane};
	assert(objectError == Error::success && "Unexpected mbus transaction");
	assert(objectDescriptor.is<LaneDescriptor>());
	auto objectLane = objectDescriptor.get<LaneDescriptor>().handle;
	while(true)
		co_await handleBind(objectLane);
}

coroutine<void> createByteRingObject(LogRingBuffer *ringBuffer,
		LaneHandle mbusLane, const char *purpose) {
	auto [offerError, lane] = co_await OfferSender{mbusLane};
	assert(offerError == Error::success && "Unexpected mbus transaction");

	managarm::mbus::Property<KernelAlloc> cls_prop(*kernelAlloc);
	cls_prop.set_name(frg::string<KernelAlloc>(*kernelAlloc, "class"));
	auto &cls_item = cls_prop.mutable_item().mutable_string_item();
	cls_item.set_value(frg::string<KernelAlloc>(*kernelAlloc, "kerncfg-byte-ring"));

	managarm::mbus::Property<KernelAlloc> purpose_prop(*kernelAlloc);
	purpose_prop.set_name(frg::string<KernelAlloc>(*kernelAlloc, "purpose"));
	auto &purpose_item = purpose_prop.mutable_item().mutable_string_item();
	purpose_item.set_value(frg::string<KernelAlloc>(*kernelAlloc, purpose));

	managarm::mbus::CntRequest<KernelAlloc> req(*kernelAlloc);
	req.set_req_type(managarm::mbus::CntReqType::CREATE_OBJECT);
	req.set_parent_id(1);
	req.add_properties(std::move(cls_prop));
	req.add_properties(std::move(purpose_prop));

	frg::string<KernelAlloc> ser(*kernelAlloc);
	req.SerializeToString(&ser);
	frg::unique_memory<KernelAlloc> reqBuffer{*kernelAlloc, ser.size()};
	memcpy(reqBuffer.data(), ser.data(), ser.size());
	auto reqError = co_await SendBufferSender{lane, std::move(reqBuffer)};
	assert(reqError == Error::success && "Unexpected mbus transaction");

	auto [respError, respBuffer] = co_await RecvBufferSender{lane};
	assert(respError == Error::success && "Unexpected mbus transaction");
	managarm::mbus::SvrResponse<KernelAlloc> resp(*kernelAlloc);
	resp.ParseFromArray(respBuffer.data(), respBuffer.size());
	assert(resp.error() == managarm::mbus::Error::SUCCESS);

	auto [objectError, objectDescriptor] = co_await PullDescriptorSender{lane};
	assert(objectError == Error::success && "Unexpected mbus transaction");
	assert(objectDescriptor.is<LaneDescriptor>());
	auto objectLane = objectDescriptor.get<LaneDescriptor>().handle;
	while(true)
		co_await handleByteRingBind(ringBuffer, objectLane);
}

coroutine<void> handleBind(LaneHandle objectLane) {
	auto [acceptError, lane] = co_await AcceptSender{objectLane};
	assert(acceptError == Error::success && "Unexpected mbus transaction");

	auto [reqError, reqBuffer] = co_await RecvBufferSender{lane};
	assert(reqError == Error::success && "Unexpected mbus transaction");
	managarm::mbus::SvrRequest<KernelAlloc> req(*kernelAlloc);
	req.ParseFromArray(reqBuffer.data(), reqBuffer.size());
	assert(req.req_type() == managarm::mbus::SvrReqType::BIND);

	managarm::mbus::CntResponse<KernelAlloc> resp(*kernelAlloc);
	resp.set_error(managarm::mbus::Error::SUCCESS);

	frg::string<KernelAlloc> ser(*kernelAlloc);
	resp.SerializeToString(&ser);
	frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
	memcpy(respBuffer.data(), ser.data(), ser.size());
	auto respError = co_await SendBufferSender{lane, std::move(respBuffer)};
	assert(respError == Error::success && "Unexpected mbus transaction");

	auto stream = createStream();
	auto boundError = co_await PushDescriptorSender{lane, LaneDescriptor{stream.get<1>()}};
	assert(boundError == Error::success && "Unexpected mbus transaction");
	auto boundLane = stream.get<0>();

	async::detach_with_allocator(*kernelAlloc, ([] (LaneHandle boundLane) -> coroutine<void> {
		while(true) {
			auto error = co_await handleReq(boundLane);
			if(error == Error::endOfLane)
				break;
			if(isRemoteIpcError(error))
				infoLogger() << "thor: Aborting kerncfg request"
						" after remote violated the protocol" << frg::endlog;
			assert(error == Error::success);
		}
	})(boundLane));
}

// TODO: maybe don't completely duplicate this function twice?
coroutine<void> handleByteRingBind(LogRingBuffer *ringBuffer, LaneHandle objectLane) {
	auto [acceptError, lane] = co_await AcceptSender{objectLane};
	assert(acceptError == Error::success && "Unexpected mbus transaction");

	auto [reqError, reqBuffer] = co_await RecvBufferSender{lane};
	assert(reqError == Error::success && "Unexpected mbus transaction");
	managarm::mbus::SvrRequest<KernelAlloc> req(*kernelAlloc);
	req.ParseFromArray(reqBuffer.data(), reqBuffer.size());
	assert(req.req_type() == managarm::mbus::SvrReqType::BIND);

	managarm::mbus::CntResponse<KernelAlloc> resp(*kernelAlloc);
	resp.set_error(managarm::mbus::Error::SUCCESS);

	frg::string<KernelAlloc> ser(*kernelAlloc);
	resp.SerializeToString(&ser);
	frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
	memcpy(respBuffer.data(), ser.data(), ser.size());
	auto respError = co_await SendBufferSender{lane, std::move(respBuffer)};
	assert(respError == Error::success && "Unexpected mbus transaction");

	auto stream = createStream();
	auto boundError = co_await PushDescriptorSender{lane, LaneDescriptor{stream.get<1>()}};
	assert(boundError == Error::success && "Unexpected mbus transaction");
	auto boundLane = stream.get<0>();

	async::detach_with_allocator(*kernelAlloc, ([] (LogRingBuffer *ringBuffer,
			LaneHandle boundLane) -> coroutine<void> {
		while(true) {
			auto error = co_await handleByteRingReq(ringBuffer, boundLane);
			if(error == Error::endOfLane)
				break;
			if(isRemoteIpcError(error))
				infoLogger() << "thor: Aborting kerncfg request"
						" after remote violated the protocol" << frg::endlog;
			assert(error == Error::success);
		}
	})(ringBuffer, boundLane));
}

} // anonymous namespace

void initializeKerncfg() {
	// Create a fiber to manage requests to the kerncfg mbus object.
	KernelFiber::run([=] {
		async::detach_with_allocator(*kernelAlloc, createObject(*mbusClient));

#ifdef KERNEL_LOG_ALLOCATIONS
		async::detach_with_allocator(*kernelAlloc,
				createByteRingObject(allocLog.get(), *mbusClient, "heap-trace"));
#endif

		if(wantKernelProfile)
			async::detach_with_allocator(*kernelAlloc,
					createByteRingObject(getGlobalProfileRing(), *mbusClient, "kernel-profile"));
		if(wantOsTrace)
			async::detach_with_allocator(*kernelAlloc,
					createByteRingObject(getGlobalOsTraceRing(), *mbusClient, "os-trace"));
	});
}

} // namespace thor
