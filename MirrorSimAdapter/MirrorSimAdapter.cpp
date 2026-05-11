#include <Windows.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <algorithm>
#include <vector>

#include "Airplay2Head.h"

namespace {

constexpr unsigned int kDefaultFrameDurationUs = 16667;
constexpr unsigned int kMinFrameDurationUs = 8000;
constexpr unsigned int kMaxFrameDurationUs = 50000;

std::string jsonEscape(const std::string& value)
{
	std::string escaped;
	escaped.reserve(value.size() + 8);

	for (char ch : value)
	{
		switch (ch)
		{
		case '\\':
			escaped += "\\\\";
			break;
		case '"':
			escaped += "\\\"";
			break;
		case '\n':
			escaped += "\\n";
			break;
		case '\r':
			escaped += "\\r";
			break;
		case '\t':
			escaped += "\\t";
			break;
		default:
			escaped += ch;
			break;
		}
	}

	return escaped;
}

std::string base64Encode(const unsigned char* data, size_t length)
{
	static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string encoded;
	encoded.reserve(((length + 2) / 3) * 4);

	for (size_t index = 0; index < length; index += 3)
	{
		const unsigned int octet_a = data[index];
		const unsigned int octet_b = index + 1 < length ? data[index + 1] : 0;
		const unsigned int octet_c = index + 2 < length ? data[index + 2] : 0;
		const unsigned int triple = (octet_a << 16) | (octet_b << 8) | octet_c;

		encoded.push_back(alphabet[(triple >> 18) & 0x3F]);
		encoded.push_back(alphabet[(triple >> 12) & 0x3F]);
		encoded.push_back(index + 1 < length ? alphabet[(triple >> 6) & 0x3F] : '=');
		encoded.push_back(index + 2 < length ? alphabet[triple & 0x3F] : '=');
	}

	return encoded;
}

std::string extractJsonString(const std::string& line, const std::string& key)
{
	const std::string needle = std::string("\"") + key + "\"";
	const size_t keyStart = line.find(needle);
	if (keyStart == std::string::npos)
	{
		return std::string();
	}

	size_t colon = line.find(':', keyStart + needle.length());
	if (colon == std::string::npos)
	{
		return std::string();
	}

	colon += 1;
	while (colon < line.size() && (line[colon] == ' ' || line[colon] == '\t'))
	{
		colon += 1;
	}

	if (colon >= line.size() || line[colon] != '"')
	{
		return std::string();
	}

	const size_t valueStart = colon + 1;
	const size_t valueEnd = line.find('"', valueStart);
	if (valueEnd == std::string::npos)
	{
		return std::string();
	}

	return line.substr(valueStart, valueEnd - valueStart);
}

std::vector<std::string> extractJsonStringArray(const std::string& line, const std::string& key)
{
	std::vector<std::string> values;
	const std::string needle = std::string("\"") + key + "\"";
	const size_t keyStart = line.find(needle);
	if (keyStart == std::string::npos)
	{
		return values;
	}

	size_t colon = line.find(':', keyStart + needle.length());
	if (colon == std::string::npos)
	{
		return values;
	}

	size_t cursor = colon + 1;
	while (cursor < line.size() && (line[cursor] == ' ' || line[cursor] == '\t'))
	{
		cursor += 1;
	}

	if (cursor >= line.size() || line[cursor] != '[')
	{
		return values;
	}

	cursor += 1;
	while (cursor < line.size())
	{
		while (cursor < line.size() && (line[cursor] == ' ' || line[cursor] == '\t' || line[cursor] == ','))
		{
			cursor += 1;
		}

		if (cursor >= line.size() || line[cursor] == ']')
		{
			break;
		}

		if (line[cursor] != '"')
		{
			break;
		}

		const size_t valueStart = cursor + 1;
		const size_t valueEnd = line.find('"', valueStart);
		if (valueEnd == std::string::npos)
		{
			break;
		}

		values.push_back(line.substr(valueStart, valueEnd - valueStart));
		cursor = valueEnd + 1;
	}

	return values;
}

bool containsInsensitive(const std::string& value, const std::string& needle)
{
	if (needle.empty() || value.size() < needle.size())
	{
		return false;
	}

	for (size_t index = 0; index + needle.size() <= value.size(); ++index)
	{
		bool matches = true;
		for (size_t offset = 0; offset < needle.size(); ++offset)
		{
			const unsigned char lhs = static_cast<unsigned char>(value[index + offset]);
			const unsigned char rhs = static_cast<unsigned char>(needle[offset]);
			if (std::tolower(lhs) != std::tolower(rhs))
			{
				matches = false;
				break;
			}
		}

		if (matches)
		{
			return true;
		}
	}

	return false;
}

enum class SidecarCommandType {
	Unknown,
	StartSession,
	StopSession,
	ConfirmPairingTrust,
	CancelPairing,
	RequestKeyframe,
	Shutdown,
};

SidecarCommandType parseCommandType(const std::string& line)
{
	const std::string name = extractJsonString(line, "name");
	if (name == "start_session")
	{
		return SidecarCommandType::StartSession;
	}
	if (name == "stop_session")
	{
		return SidecarCommandType::StopSession;
	}
	if (name == "confirm_pairing_trust")
	{
		return SidecarCommandType::ConfirmPairingTrust;
	}
	if (name == "cancel_pairing")
	{
		return SidecarCommandType::CancelPairing;
	}
	if (name == "request_keyframe")
	{
		return SidecarCommandType::RequestKeyframe;
	}
	if (name == "shutdown")
	{
		return SidecarCommandType::Shutdown;
	}
	return SidecarCommandType::Unknown;
}

class MirrorSimCallback : public IAirServerCallback
{
public:
	enum class PairingApprovalState {
		Idle,
		Pending,
		Approved,
		Rejected,
	};

	MirrorSimCallback()
		: m_serverHandle(nullptr)
		, m_sampleIndex(0)
		, m_lastPts(0)
		, m_lastDuration(kDefaultFrameDurationUs)
		, m_hasLastPts(false)
		, m_lastPairingPhase("idle")
		, m_pairingApprovalState(PairingApprovalState::Idle)
	{
	}

	void setServerHandle(void* serverHandle)
	{
		std::lock_guard<std::mutex> lock(m_stateMutex);
		m_serverHandle = serverHandle;
	}

	void setPendingSession(const std::string& sessionId, const std::string& streamId)
	{
		std::lock_guard<std::mutex> lock(m_stateMutex);
		m_sessionId = sessionId;
		m_streamId = streamId;
		m_sampleIndex = 0;
		m_lastPts = 0;
		m_lastDuration = kDefaultFrameDurationUs;
		m_hasLastPts = false;
	}

	void clearSession()
	{
		std::lock_guard<std::mutex> lock(m_stateMutex);
		m_sessionId.clear();
		m_streamId.clear();
		m_deviceName.clear();
		m_deviceId.clear();
		m_deviceModel.clear();
		m_deviceOsName.clear();
		m_deviceOsVersion.clear();
		m_deviceOsBuildVersion.clear();
		m_deviceSourceVersion.clear();
		m_lastPairingPhase = "idle";
		m_sampleIndex = 0;
		m_lastPts = 0;
		m_lastDuration = kDefaultFrameDurationUs;
		m_hasLastPts = false;
		clearPendingApproval();
	}

	void updateTrustPolicy(
		const std::vector<std::string>& trustedDeviceIds,
		const std::vector<std::string>& blockedDeviceIds)
	{
		std::lock_guard<std::mutex> lock(m_pairingMutex);
		m_trustedDeviceIds = trustedDeviceIds;
		m_blockedDeviceIds = blockedDeviceIds;
	}

	void emitReceiverReady()
	{
		emitJson("{\"name\":\"receiver_ready\",\"receiver_id\":\"airplayserver-mirrorsim-adapter\",\"protocol_version\":\"0.4.0\",\"capabilities\":[\"stdio-jsonl\",\"session-control\",\"h264-access-units\",\"device-identity\",\"pairing-status\",\"pairing-trust-control\"]}");
	}

	void emitReceiverError(const std::string& code, const std::string& message, bool recoverable)
	{
		std::ostringstream event;
		event << "{\"name\":\"receiver_error\",\"code\":\"" << jsonEscape(code)
			  << "\",\"message\":\"" << jsonEscape(message)
			  << "\",\"recoverable\":" << (recoverable ? "true" : "false") << "}";
		emitJson(event.str());
	}

	void emitDiscontinuity(const std::string& reason, bool requiresRefresh)
	{
		std::string streamId;
		{
			std::lock_guard<std::mutex> lock(m_stateMutex);
			streamId = m_streamId.empty() ? "unknown-stream" : m_streamId;
		}

		std::ostringstream event;
		event << "{\"name\":\"stream_discontinuity\",\"stream_id\":\"" << jsonEscape(streamId)
			  << "\",\"reason\":\"" << jsonEscape(reason)
			  << "\",\"requires_init_segment_refresh\":" << (requiresRefresh ? "true" : "false") << "}";
		emitJson(event.str());
	}

	void emitPairingStateChanged(
		const std::string& phase,
		const std::string& entryMode,
		const std::string& prompt,
		const std::string& failureMessage,
		bool canTrust)
	{
		std::string deviceName;
		std::string deviceId;
		std::string deviceModel;
		std::string deviceOsName;
		std::string deviceOsVersion;
		std::string deviceOsBuildVersion;
		std::string deviceSourceVersion;
		{
			std::lock_guard<std::mutex> lock(m_stateMutex);
			if (phase == m_lastPairingPhase && prompt.empty() && failureMessage.empty())
			{
				return;
			}

			m_lastPairingPhase = phase;
			deviceName = m_deviceName;
			deviceId = m_deviceId;
			deviceModel = m_deviceModel;
			deviceOsName = m_deviceOsName;
			deviceOsVersion = m_deviceOsVersion;
			deviceOsBuildVersion = m_deviceOsBuildVersion;
			deviceSourceVersion = m_deviceSourceVersion;
		}

		std::ostringstream event;
		event << "{\"name\":\"pairing_state_changed\",\"phase\":\"" << jsonEscape(phase) << "\"";

		if (!entryMode.empty())
		{
			event << ",\"entry_mode\":\"" << jsonEscape(entryMode) << "\"";
		}

		if (!deviceName.empty())
		{
			event << ",\"device_name\":\"" << jsonEscape(deviceName) << "\"";
		}

		if (!deviceId.empty())
		{
			event << ",\"device_id\":\"" << jsonEscape(deviceId) << "\"";
		}

		if (!deviceModel.empty())
		{
			event << ",\"device_model\":\"" << jsonEscape(deviceModel) << "\"";
		}

		if (!deviceOsName.empty())
		{
			event << ",\"device_os_name\":\"" << jsonEscape(deviceOsName) << "\"";
		}

		if (!deviceOsVersion.empty())
		{
			event << ",\"device_os_version\":\"" << jsonEscape(deviceOsVersion) << "\"";
		}

		if (!deviceOsBuildVersion.empty())
		{
			event << ",\"device_os_build_version\":\"" << jsonEscape(deviceOsBuildVersion) << "\"";
		}

		if (!deviceSourceVersion.empty())
		{
			event << ",\"device_source_version\":\"" << jsonEscape(deviceSourceVersion) << "\"";
		}

		if (!prompt.empty())
		{
			event << ",\"prompt\":\"" << jsonEscape(prompt) << "\"";
		}

		if (!failureMessage.empty())
		{
			event << ",\"failure_message\":\"" << jsonEscape(failureMessage) << "\"";
		}

		event << ",\"can_trust\":" << (canTrust ? "true" : "false") << "}";
		emitJson(event.str());
	}

	virtual bool approvePairingRequest(
		const char* remoteName,
		const char* remoteDeviceId,
		const char* remoteModel,
		const char* remoteOsName,
		const char* remoteOsVersion,
		const char* remoteOsBuildVersion,
		const char* remoteSourceVersion) override
	{
		const std::string deviceName = remoteName ? remoteName : "AirPlay sender";
		const std::string deviceId = remoteDeviceId ? remoteDeviceId : "";
		const std::string deviceModel = remoteModel ? remoteModel : "";
		const std::string deviceOsName = remoteOsName ? remoteOsName : "";
		const std::string deviceOsVersion = remoteOsVersion ? remoteOsVersion : "";
		const std::string deviceOsBuildVersion = remoteOsBuildVersion ? remoteOsBuildVersion : "";
		const std::string deviceSourceVersion = remoteSourceVersion ? remoteSourceVersion : "";

		{
			std::lock_guard<std::mutex> lock(m_stateMutex);
			m_deviceName = deviceName;
			m_deviceId = deviceId;
			m_deviceModel = deviceModel;
			m_deviceOsName = deviceOsName;
			m_deviceOsVersion = deviceOsVersion;
			m_deviceOsBuildVersion = deviceOsBuildVersion;
			m_deviceSourceVersion = deviceSourceVersion;
		}

		{
			std::unique_lock<std::mutex> lock(m_pairingMutex);
			if (!deviceId.empty())
			{
				if (std::find(m_blockedDeviceIds.begin(), m_blockedDeviceIds.end(), deviceId) != m_blockedDeviceIds.end())
				{
					lock.unlock();
					emitPairingStateChanged("failed", "confirm-only", std::string(), "This iPhone is blocked on this PC.", false);
					emitReceiverError("pairing_blocked", "This iPhone is blocked on this PC.", true);
					return false;
				}

				if (std::find(m_trustedDeviceIds.begin(), m_trustedDeviceIds.end(), deviceId) != m_trustedDeviceIds.end())
				{
					lock.unlock();
					emitPairingStateChanged("verifying", "confirm-only", "Trusted device recognized on this PC.", std::string(), false);
					return true;
				}
			}

			m_pendingDeviceName = deviceName;
			m_pendingDeviceId = deviceId;
			m_pairingApprovalState = PairingApprovalState::Pending;
			m_pairingFailureReason.clear();
		}

		emitPairingStateChanged(
			"awaiting-trust",
			"confirm-only",
			"Approve this iPhone before MirrorSim starts streaming.",
			std::string(),
			true);

		std::unique_lock<std::mutex> lock(m_pairingMutex);
		const bool resolved = m_pairingCv.wait_for(
			lock,
			std::chrono::seconds(90),
			[this]() {
				return m_pairingApprovalState == PairingApprovalState::Approved
					|| m_pairingApprovalState == PairingApprovalState::Rejected;
			});

		const PairingApprovalState decision = resolved ? m_pairingApprovalState : PairingApprovalState::Rejected;
		const std::string failureReason = resolved
			? (m_pairingFailureReason.empty() ? "Pairing approval was cancelled." : m_pairingFailureReason)
			: "Pairing approval timed out.";
		clearPendingApprovalLocked();
		lock.unlock();

		if (decision == PairingApprovalState::Approved)
		{
			emitPairingStateChanged("verifying", "confirm-only", "MirrorSim approved this iPhone. Finishing the AirPlay handshake.", std::string(), false);
			return true;
		}

		emitPairingStateChanged("failed", "confirm-only", std::string(), failureReason, false);
		emitReceiverError("pairing_rejected", failureReason, true);
		return false;
	}

	void confirmPendingTrust()
	{
		std::lock_guard<std::mutex> lock(m_pairingMutex);
		if (m_pairingApprovalState == PairingApprovalState::Pending)
		{
			m_pairingApprovalState = PairingApprovalState::Approved;
			m_pairingFailureReason.clear();
			m_pairingCv.notify_all();
		}
	}

	void cancelPendingTrust(const std::string& reason)
	{
		std::lock_guard<std::mutex> lock(m_pairingMutex);
		if (m_pairingApprovalState == PairingApprovalState::Pending)
		{
			m_pairingApprovalState = PairingApprovalState::Rejected;
			m_pairingFailureReason = reason;
			m_pairingCv.notify_all();
		}
	}

	void handlePairingLog(const std::string& message)
	{
		if (containsInsensitive(message, "/pair-setup"))
		{
			emitPairingStateChanged(
				"verifying",
				"none",
				"MirrorSim is negotiating AirPlay trust with the sender.",
				std::string(),
				false);
			return;
		}

		if (containsInsensitive(message, "/pair-verify"))
		{
			emitPairingStateChanged(
				"verifying",
				"none",
				"MirrorSim is verifying the AirPlay pairing handshake.",
				std::string(),
				false);
			return;
		}

		if (containsInsensitive(message, "invalid pair-setup data")
			|| containsInsensitive(message, "invalid pair-verify data")
			|| containsInsensitive(message, "error initializing pair-verify handshake")
			|| containsInsensitive(message, "incorrect pair-verify signature"))
		{
			emitPairingStateChanged("failed", "none", std::string(), message, false);
		}
	}

	virtual void connected(const char* remoteName, const char* remoteDeviceId) override
	{
		std::string sessionId;
		std::string streamId;
		std::string deviceName = remoteName ? remoteName : "AirPlay sender";
		std::string deviceId = remoteDeviceId ? remoteDeviceId : "";
		std::string deviceModel;
		std::string deviceOsName;
		std::string deviceOsVersion;
		std::string deviceOsBuildVersion;
		std::string deviceSourceVersion;
		{
			std::lock_guard<std::mutex> lock(m_stateMutex);
			sessionId = m_sessionId.empty() ? "session-unknown" : m_sessionId;
			streamId = m_streamId.empty() ? "stream-unknown" : m_streamId;
			m_deviceName = deviceName;
			m_deviceId = deviceId;
			deviceModel = m_deviceModel;
			deviceOsName = m_deviceOsName;
			deviceOsVersion = m_deviceOsVersion;
			deviceOsBuildVersion = m_deviceOsBuildVersion;
			deviceSourceVersion = m_deviceSourceVersion;
		}

		std::ostringstream event;
		event << "{\"name\":\"session_started\",\"session_id\":\"" << jsonEscape(sessionId)
			  << "\",\"stream_id\":\"" << jsonEscape(streamId)
			  << "\",\"device_name\":\"" << jsonEscape(deviceName) << "\"";

		if (!deviceId.empty())
		{
			event << ",\"device_id\":\"" << jsonEscape(deviceId) << "\"";
		}

		if (!deviceModel.empty())
		{
			event << ",\"device_model\":\"" << jsonEscape(deviceModel) << "\"";
		}

		if (!deviceOsName.empty())
		{
			event << ",\"device_os_name\":\"" << jsonEscape(deviceOsName) << "\"";
		}

		if (!deviceOsVersion.empty())
		{
			event << ",\"device_os_version\":\"" << jsonEscape(deviceOsVersion) << "\"";
		}

		if (!deviceOsBuildVersion.empty())
		{
			event << ",\"device_os_build_version\":\"" << jsonEscape(deviceOsBuildVersion) << "\"";
		}

		if (!deviceSourceVersion.empty())
		{
			event << ",\"device_source_version\":\"" << jsonEscape(deviceSourceVersion) << "\"";
		}

		event << "}";
		emitJson(event.str());
		emitPairingStateChanged("paired", "none", std::string(), std::string(), false);
	}

	virtual void disconnected(const char* remoteName, const char* remoteDeviceId) override
	{
		(void)remoteName;
		(void)remoteDeviceId;
		emitDiscontinuity("sender_disconnected", true);
		clearSession();
	}

	virtual void outputAudio(SFgAudioFrame* data, const char* remoteName, const char* remoteDeviceId) override
	{
		(void)data;
		(void)remoteName;
		(void)remoteDeviceId;
	}

	virtual void outputH264AccessUnit(SFgH264AccessUnit* data, const char* remoteName, const char* remoteDeviceId) override
	{
		(void)remoteName;
		(void)remoteDeviceId;
		std::string streamId;
		unsigned int sampleIndex = 0;
		unsigned int duration = data->duration;
		{
			std::lock_guard<std::mutex> lock(m_stateMutex);
			streamId = m_streamId.empty() ? "stream-unknown" : m_streamId;
			sampleIndex = m_sampleIndex++;

			if (m_hasLastPts && data->pts > m_lastPts)
			{
				const unsigned long long delta = data->pts - m_lastPts;
				if (delta >= kMinFrameDurationUs && delta <= kMaxFrameDurationUs)
				{
					m_lastDuration = static_cast<unsigned int>(delta);
				}
			}

			if (duration < kMinFrameDurationUs || duration > kMaxFrameDurationUs)
			{
				duration = m_lastDuration;
			}

			if (data->pts > 0)
			{
				m_lastPts = data->pts;
				m_hasLastPts = true;
			}
		}

		const std::string payload = base64Encode(data->data, data->dataLen);
		std::ostringstream event;
		event << "{\"name\":\"video_access_unit\",\"stream_id\":\"" << jsonEscape(streamId)
			  << "\",\"sample_index\":" << sampleIndex
			  << ",\"keyframe\":" << (data->isKey ? "true" : "false")
			  << ",\"pts\":" << data->pts
			  << ",\"dts\":" << data->dts
			  << ",\"duration\":" << duration
			  << ",\"payloadBase64\":\"" << payload << "\"}";
		emitJson(event.str());
	}

	virtual void outputVideo(SFgVideoFrame* data, const char* remoteName, const char* remoteDeviceId) override
	{
		(void)data;
		(void)remoteName;
		(void)remoteDeviceId;
	}

	virtual void videoPlay(char* url, double volume, double startPos) override
	{
		(void)url;
		(void)volume;
		(void)startPos;
	}

	virtual void videoGetPlayInfo(double* duration, double* position, double* rate) override
	{
		if (duration) { *duration = 0.0; }
		if (position) { *position = 0.0; }
		if (rate) { *rate = 1.0; }
	}

	virtual void setVolume(float volume, const char* remoteName, const char* remoteDeviceId) override
	{
		(void)volume;
		(void)remoteName;
		(void)remoteDeviceId;
	}

	virtual void log(int level, const char* msg) override
	{
		const std::string message = msg ? msg : "native receiver error";
		handlePairingLog(message);

		if (level <= 3)
		{
			emitReceiverError("native_receiver_log", message, true);
		}
	}

private:
	void clearPendingApproval()
	{
		std::lock_guard<std::mutex> lock(m_pairingMutex);
		clearPendingApprovalLocked();
	}

	void clearPendingApprovalLocked()
	{
		m_pairingApprovalState = PairingApprovalState::Idle;
		m_pairingFailureReason.clear();
		m_pendingDeviceName.clear();
		m_pendingDeviceId.clear();
	}

	void emitJson(const std::string& event)
	{
		std::lock_guard<std::mutex> lock(m_stdoutMutex);
		std::cout << event << std::endl;
	}

	std::mutex m_stateMutex;
	std::mutex m_stdoutMutex;
	void* m_serverHandle;
	std::string m_sessionId;
	std::string m_streamId;
	std::string m_deviceName;
	std::string m_deviceId;
	std::string m_deviceModel;
	std::string m_deviceOsName;
	std::string m_deviceOsVersion;
	std::string m_deviceOsBuildVersion;
	std::string m_deviceSourceVersion;
	std::string m_lastPairingPhase;
	unsigned int m_sampleIndex;
	unsigned long long m_lastPts;
	unsigned int m_lastDuration;
	bool m_hasLastPts;
	std::mutex m_pairingMutex;
	std::condition_variable m_pairingCv;
	PairingApprovalState m_pairingApprovalState;
	std::string m_pairingFailureReason;
	std::string m_pendingDeviceName;
	std::string m_pendingDeviceId;
	std::vector<std::string> m_trustedDeviceIds;
	std::vector<std::string> m_blockedDeviceIds;
};

} // namespace

int main()
{
	MirrorSimCallback callback;
	callback.emitReceiverReady();

	void* serverHandle = nullptr;
	std::string line;

	while (std::getline(std::cin, line))
	{
		switch (parseCommandType(line))
		{
		case SidecarCommandType::StartSession:
		{
			const std::string sessionId = extractJsonString(line, "session_id");
			const std::string streamId = extractJsonString(line, "expected_stream_id");
			const std::string requestedReceiverName = extractJsonString(line, "receiver_name");
			const std::vector<std::string> trustedDeviceIds = extractJsonStringArray(line, "trusted_device_ids");
			const std::vector<std::string> blockedDeviceIds = extractJsonStringArray(line, "blocked_device_ids");
			callback.setPendingSession(sessionId, streamId);
			callback.updateTrustPolicy(trustedDeviceIds, blockedDeviceIds);

			if (!serverHandle)
			{
				char serverName[AIRPLAY_NAME_LEN] = "MirrorSim";
				if (!requestedReceiverName.empty())
				{
					strncpy_s(serverName, requestedReceiverName.c_str(), AIRPLAY_NAME_LEN - 1);
				}
				serverHandle = fgServerStart(serverName, 5001, 7001, &callback);
				callback.setServerHandle(serverHandle);
				if (!serverHandle)
				{
					callback.emitReceiverError("start_failed", "fgServerStart returned null", false);
				}
			}
			break;
		}
		case SidecarCommandType::StopSession:
			if (serverHandle)
			{
				fgServerStop(serverHandle);
				serverHandle = nullptr;
				callback.emitDiscontinuity("session_stopped", false);
				callback.clearSession();
			}
			break;
		case SidecarCommandType::ConfirmPairingTrust:
			callback.confirmPendingTrust();
			break;
		case SidecarCommandType::CancelPairing:
			callback.cancelPendingTrust("Pairing approval was cancelled.");
			callback.emitPairingStateChanged("idle", "none", std::string(), std::string(), false);
			break;
		case SidecarCommandType::RequestKeyframe:
			callback.emitDiscontinuity(extractJsonString(line, "reason"), false);
			break;
		case SidecarCommandType::Shutdown:
			if (serverHandle)
			{
				fgServerStop(serverHandle);
				serverHandle = nullptr;
			}
			return 0;
		case SidecarCommandType::Unknown:
		default:
			callback.emitReceiverError("invalid_command", "Unsupported or malformed command payload", true);
			break;
		}
	}

	if (serverHandle)
	{
		fgServerStop(serverHandle);
	}

	return 0;
}