#include <algorithm>
#include <condition_variable>
#include <iostream>
#include <functional>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>
#include <cstdlib>
#include <sstream>

#include <host\ginclude.h>
#include <common\asio.h>

#include "../FlexASIOUtil/log.h"
#include "..\FlexASIO\cflexasio.h"
#include "..\FlexASIOUtil\asio.h"
#include "..\FlexASIOUtil\find.h"
#include "..\FlexASIOUtil\string.h"

// The global ASIO driver pointer that the ASIO host library internally uses.
extern IASIO* theAsioDriver;

namespace flexasio {
	namespace {

		class LogState final {
		public:
			LogSink& sink() { return preamble_sink;  }

		private:
			StreamLogSink stream_sink{ std::cout };
			ThreadSafeLogSink thread_safe_sink{ stream_sink };
			PreambleLogSink preamble_sink{ thread_safe_sink };
		};

		Logger Log() {
			static LogState logState;
			return Logger(&logState.sink());
		}

		template <typename FunctionPointer> struct function_pointer_traits;
		template <typename ReturnValue, typename... Args> struct function_pointer_traits<ReturnValue(*)(Args...)> {
			using function = std::function<ReturnValue(Args...)>;
		};

		ASIOError PrintError(ASIOError error) {
			Log() << "-> " << GetASIOErrorString(error);
			return error;
		}

		std::optional<ASIODriverInfo> Init() {
			ASIODriverInfo asioDriverInfo = { 0 };
			asioDriverInfo.asioVersion = 2;
			Log() << "ASIOInit(asioVersion = " << asioDriverInfo.asioVersion << ")";
			const auto initError = PrintError(ASIOInit(&asioDriverInfo));
			Log() << "asioVersion = " << asioDriverInfo.asioVersion << " driverVersion = " << asioDriverInfo.asioVersion << " name = " << asioDriverInfo.name << " errorMessage = " << asioDriverInfo.errorMessage << " sysRef = " << asioDriverInfo.sysRef;
			if (initError != ASE_OK) return std::nullopt;
			return asioDriverInfo;
		}

		std::pair<long, long> GetChannels() {
			Log() << "ASIOGetChannels()";
			long numInputChannels, numOutputChannels;
			const auto error = PrintError(ASIOGetChannels(&numInputChannels, &numOutputChannels));
			if (error != ASE_OK) return { 0, 0 };
			Log() << "Channel count: " << numInputChannels << " input, " << numOutputChannels << " output";
			return { numInputChannels, numOutputChannels };
		}

		struct BufferSize {
			long min = LONG_MIN;
			long max = LONG_MIN;
			long preferred = LONG_MIN;
			long granularity = LONG_MIN;
		};

		std::optional<BufferSize> GetBufferSize() {
			Log() << "ASIOGetBufferSize()";
			BufferSize bufferSize;
			const auto error = PrintError(ASIOGetBufferSize(&bufferSize.min, &bufferSize.max, &bufferSize.preferred, &bufferSize.granularity));
			if (error != ASE_OK) return std::nullopt;
			Log() << "Buffer size: min " << bufferSize.min << " max " << bufferSize.max << " preferred " << bufferSize.preferred << " granularity " << bufferSize.granularity;
			return bufferSize;
		}

		std::optional<ASIOSampleRate> GetSampleRate() {
			Log() << "ASIOGetSampleRate()";
			ASIOSampleRate sampleRate = NAN;
			const auto error = PrintError(ASIOGetSampleRate(&sampleRate));
			if (error != ASE_OK) return std::nullopt;
			Log() << "Sample rate: " << sampleRate;
			return sampleRate;
		}

		bool CanSampleRate(ASIOSampleRate sampleRate) {
			Log() << "ASIOCanSampleRate(" << sampleRate << ")";
			return PrintError(ASIOCanSampleRate(sampleRate)) == ASE_OK;
		}

		bool SetSampleRate(ASIOSampleRate sampleRate) {
			Log() << "ASIOSetSampleRate(" << sampleRate << ")";
			return PrintError(ASIOSetSampleRate(sampleRate)) == ASE_OK;
		}

		bool OutputReady() {
			Log() << "ASIOOutputReady()";
			return PrintError(ASIOOutputReady()) == ASE_OK;
		}

		std::optional<ASIOChannelInfo> GetChannelInfo(long channel, ASIOBool isInput) {
			Log() << "ASIOGetChannelInfo(channel = " << channel << " isInput = " << isInput << ")";
			ASIOChannelInfo channelInfo;
			channelInfo.channel = channel;
			channelInfo.isInput = isInput;
			if (PrintError(ASIOGetChannelInfo(&channelInfo)) != ASE_OK) return std::nullopt;
			Log() << "isActive = " << channelInfo.isActive << " channelGroup = " << channelInfo.channelGroup << " type = " << GetASIOSampleTypeString(channelInfo.type) << " name = " << channelInfo.name;
			return channelInfo;
		}

		void GetAllChannelInfo(std::pair<long, long> ioChannelCounts) {
			for (long inputChannel = 0; inputChannel < ioChannelCounts.first; ++inputChannel) GetChannelInfo(inputChannel, true);
			for (long outputChannel = 0; outputChannel < ioChannelCounts.second; ++outputChannel) GetChannelInfo(outputChannel, false);
		}

		struct Buffers {
			Buffers() = default;
			explicit Buffers(std::vector<ASIOBufferInfo> info) : info(std::move(info)) {}
			Buffers(const Buffers&) = delete;
			Buffers(Buffers&&) = default;
			~Buffers() {
				if (info.size() == 0) return;
				Log();
				Log() << "ASIODisposeBuffers()";
				PrintError(ASIODisposeBuffers());
			}

			std::vector<ASIOBufferInfo> info;
		};

		// TODO: we should also test with not all channels active.
		Buffers CreateBuffers(std::pair<long, long> ioChannelCounts, long bufferSize, ASIOCallbacks callbacks) {
			std::vector<ASIOBufferInfo> bufferInfos;
			for (long inputChannel = 0; inputChannel < ioChannelCounts.first; ++inputChannel) {
				auto& bufferInfo = bufferInfos.emplace_back();
				bufferInfo.isInput = true;
				bufferInfo.channelNum = inputChannel;
			}
			for (long outputChannel = 0; outputChannel < ioChannelCounts.second; ++outputChannel) {
				auto& bufferInfo = bufferInfos.emplace_back();
				bufferInfo.isInput = false;
				bufferInfo.channelNum = outputChannel;
			}

			Log() << "ASIOCreateBuffers(";
			for (const auto& bufferInfo : bufferInfos) {
				Log() << "isInput = " << bufferInfo.isInput << " channelNum = " << bufferInfo.channelNum << " ";
			}
			Log() << ", bufferSize = " << bufferSize << ", bufferSwitch = " << (void*)(callbacks.bufferSwitch) << " sampleRateDidChange = " << (void*)(callbacks.sampleRateDidChange) << " asioMessage = " << (void*)(callbacks.asioMessage) << " bufferSwitchTimeInfo = " << (void*)(callbacks.bufferSwitchTimeInfo) << ")";

			if (PrintError(ASIOCreateBuffers(bufferInfos.data(), long(bufferInfos.size()), bufferSize, &callbacks)) != ASE_OK) return {};
			return Buffers(bufferInfos);
		}

		void GetLatencies() {
			long inputLatency = LONG_MIN, outputLatency = LONG_MIN;
			Log() << "ASIOGetLatencies()";
			if (PrintError(ASIOGetLatencies(&inputLatency, &outputLatency)) != ASE_OK) return;
			Log() << "Latencies: input " << inputLatency << " samples, output " << outputLatency << " samples";
		}

		bool Start() {
			Log() << "ASIOStart()";
			return PrintError(ASIOStart()) == ASE_OK;
		}

		bool Stop() {
			Log() << "ASIOStop()";
			return PrintError(ASIOStop()) == ASE_OK;
		}

		void GetSamplePosition() {
			Log() << "ASIOGetSamplePosition()";
			ASIOSamples samples;
			ASIOTimeStamp timeStamp;
			if (PrintError(ASIOGetSamplePosition(&samples, &timeStamp)) != ASE_OK) return;
			Log() << "Sample position: " << ASIOToInt64(samples) << " timestamp: " << ASIOToInt64(timeStamp);
		}

		using ASIOMessageHandler = decltype(ASIOCallbacks::asioMessage);

		long HandleSelectorSupportedMessage(long, long value, void*, double*);

		long HandleSupportsTimeInfoMessage(long, long, void*, double*) { return 1; }

		constexpr std::pair<long, ASIOMessageHandler> message_selector_handlers[] = {
				{kAsioSelectorSupported, HandleSelectorSupportedMessage},
				{kAsioSupportsTimeInfo, HandleSupportsTimeInfoMessage},
		};

		long HandleSelectorSupportedMessage(long, long value, void*, double*) {
			Log() << "Being queried for message selector " << GetASIOMessageSelectorString(value);
			return Find(value, message_selector_handlers).has_value() ? 1 : 0;
		}

		long HandleASIOMessage(long selector, long value, void* message, double* opt) {
			const auto handler = Find(selector, message_selector_handlers);
			if (!handler.has_value()) return 0;
			return (*handler)(selector, value, message, opt);
		}

		// Allows the use of capturing lambdas for ASIO callbacks, even though ASIO doesn't provide any mechanism to pass user context to callbacks.
		// This works by assuming that we will only use one set of callbacks at a time, such that we can use global state as a side channel.
		struct Callbacks {
			Callbacks() {
				if (global != nullptr) abort();
				global = this;
			}
			~Callbacks() {
				if (global != this) abort();
				global = nullptr;
			}

			function_pointer_traits<decltype(ASIOCallbacks::bufferSwitch)>::function bufferSwitch;
			function_pointer_traits<decltype(ASIOCallbacks::sampleRateDidChange)>::function sampleRateDidChange;
			function_pointer_traits<decltype(ASIOCallbacks::asioMessage)>::function asioMessage;
			function_pointer_traits<decltype(ASIOCallbacks::bufferSwitchTimeInfo)>::function bufferSwitchTimeInfo;

			ASIOCallbacks GetASIOCallbacks() const {
				ASIOCallbacks callbacks;
				callbacks.bufferSwitch = GetASIOCallback<&Callbacks::bufferSwitch>();
				callbacks.sampleRateDidChange = GetASIOCallback<&Callbacks::sampleRateDidChange>();
				callbacks.asioMessage = GetASIOCallback<&Callbacks::asioMessage>();
				callbacks.bufferSwitchTimeInfo = GetASIOCallback<&Callbacks::bufferSwitchTimeInfo>();
				return callbacks;
			}

		private:
			template <auto memberFunction> auto GetASIOCallback() const {
				return [](auto... args) {
					if (global == nullptr) abort();
					return (global->*memberFunction)(args...);
				};
			}

			static Callbacks* global;
		};

		Callbacks* Callbacks::global = nullptr;

		bool Run() {
			if (!Init()) return false;

			Log();

			const auto ioChannelCounts = GetChannels();
			if (ioChannelCounts.first == 0 && ioChannelCounts.second == 0) return false;

			Log();

			const auto initialSampleRate = GetSampleRate();
			if (!initialSampleRate.has_value()) return false;

			Log();

			for (const auto sampleRate : { 44100.0, 48000.0, 96000.0, 192000.0, *initialSampleRate }) {
				if (CanSampleRate(sampleRate)) {
					if (!SetSampleRate(sampleRate)) return false;
					if (GetSampleRate() != sampleRate) return false;
				}
			}

			Log();

			const auto bufferSize = GetBufferSize();
			if (!bufferSize.has_value()) return false;

			Log();

			OutputReady();

			Log();

			GetAllChannelInfo(ioChannelCounts);

			Log();

			std::mutex bufferSwitchCountMutex;
			std::condition_variable bufferSwitchCountCondition;
			size_t bufferSwitchCount = 0;
			const auto incrementBufferSwitchCount = [&] {
				{
					std::scoped_lock bufferSwitchCountLock(bufferSwitchCountMutex);
					++bufferSwitchCount;
					Log() << "Buffer switch count: " << bufferSwitchCount;
				}
				bufferSwitchCountCondition.notify_all();
			};

			Callbacks callbacks;
			callbacks.bufferSwitch = [&](long doubleBufferIndex, ASIOBool directProcess) {
				Log() << "bufferSwitch(doubleBufferIndex = " << doubleBufferIndex << ", directProcess = " << directProcess << ")";
				GetSamplePosition();
				Log() << "<-";
				incrementBufferSwitchCount();
			};
			callbacks.sampleRateDidChange = [&](ASIOSampleRate sampleRate) {
				Log() << "sampleRateDidChange(" << sampleRate << ")";
				Log() << "<-";
			};
			callbacks.asioMessage = [&](long selector, long value, void* message, double* opt) {
				Log() << "asioMessage(selector = " << GetASIOMessageSelectorString(selector) << ", value = " << value << ", message = " << message << ", opt = " << opt << ")";
				const auto result = HandleASIOMessage(selector, value, message, opt);
				Log() << "<- " << result;
				return result;
			};
			callbacks.bufferSwitchTimeInfo = [&](ASIOTime* params, long doubleBufferIndex, ASIOBool directProcess) {
				Log() << "bufferSwitchTimeInfo(params = (" << (params == nullptr ? "none" : DescribeASIOTime(*params)) << "), doubleBufferIndex = " << doubleBufferIndex << ", directProcess = " << directProcess << ")";
				GetSamplePosition();
				Log() << "<- nullptr";
				incrementBufferSwitchCount();
				return nullptr;
			};

			const auto buffers = CreateBuffers(ioChannelCounts, bufferSize->preferred, callbacks.GetASIOCallbacks());
			if (buffers.info.size() == 0) return false;

			Log();

			GetSampleRate();
			GetAllChannelInfo(ioChannelCounts);

			Log();

			GetLatencies();

			Log();

			if (!Start()) return false;

			Log();

			// Run enough buffer switches such that we can trigger failure modes like https://github.com/dechamps/FlexASIO/issues/29.
			constexpr size_t bufferSwitchCountThreshold = 30;
			Log() << "Now waiting for " << bufferSwitchCountThreshold << " buffer switches...";
			Log();

			{
				std::unique_lock bufferSwitchCountLock(bufferSwitchCountMutex);
				bufferSwitchCountCondition.wait(bufferSwitchCountLock, [&] { return bufferSwitchCount >= bufferSwitchCountThreshold;  });
			}

			Log();
			Log() << "Reached " << bufferSwitchCountThreshold << " buffer switches, stopping";

			if (!Stop()) return false;

			// Note: we don't call ASIOExit() because it gets confused by our driver setup trickery (see InitAndRun()).
			// That said, this doesn't really matter because ASIOExit() is basically a no-op in our case, anyway.
			return true;
		}

		bool InitAndRun() {
			// This basically does an end run around the ASIO host library driver loading system, simulating what loadAsioDriver() does.
			// This allows us to trick the ASIO host library into using a specific instance of an ASIO driver (the one this program is linked against),
			// as opposed to whatever ASIO driver might be currently installed on the system.
			theAsioDriver = CreateFlexASIO();

			const bool result = Run();

			// There are cases in which the ASIO host library will nullify the driver pointer.
			// For example, it does that if the driver fails to initialize.
			// (Sadly the ASIO host library won't call Release() in that case, because memory leaks are fun!)
			if (theAsioDriver != nullptr) {
				ReleaseFlexASIO(theAsioDriver);
				theAsioDriver = nullptr;
			}

			return result;
		}

	}
}

int main(int, char**) {
	if (!::flexasio::InitAndRun()) return 1;
	return 0;
}