#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
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

enum class SidecarCommandType {
	Unknown,
	StartSession,
	StopSession,
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
	MirrorSimCallback()
		: m_serverHandle(nullptr)
		, m_sampleIndex(0)
		, m_lastPts(0)
		, m_lastDuration(kDefaultFrameDurationUs)
		, m_hasLastPts(false)
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
		m_sampleIndex = 0;
		m_lastPts = 0;
		m_lastDuration = kDefaultFrameDurationUs;
		m_hasLastPts = false;
	}

	void emitReceiverReady()
	{
		emitJson("{\"name\":\"receiver_ready\",\"receiver_id\":\"airplayserver-mirrorsim-adapter\",\"protocol_version\":\"0.1.0\",\"capabilities\":[\"stdio-jsonl\",\"session-control\",\"h264-access-units\"]}");
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

	virtual void connected(const char* remoteName, const char* remoteDeviceId) override
	{
		(void)remoteDeviceId;
		std::string sessionId;
		std::string streamId;
		{
			std::lock_guard<std::mutex> lock(m_stateMutex);
			sessionId = m_sessionId.empty() ? "session-unknown" : m_sessionId;
			streamId = m_streamId.empty() ? "stream-unknown" : m_streamId;
		}

		std::ostringstream event;
		event << "{\"name\":\"session_started\",\"session_id\":\"" << jsonEscape(sessionId)
			  << "\",\"stream_id\":\"" << jsonEscape(streamId)
			  << "\",\"device_name\":\"" << jsonEscape(remoteName ? remoteName : "AirPlay sender") << "\"}";
		emitJson(event.str());
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
		if (level <= 3)
		{
			emitReceiverError("native_receiver_log", msg ? msg : "native receiver error", true);
		}
	}

private:
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
	unsigned int m_sampleIndex;
	unsigned long long m_lastPts;
	unsigned int m_lastDuration;
	bool m_hasLastPts;
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
			callback.setPendingSession(sessionId, streamId);

			if (!serverHandle)
			{
				char serverName[AIRPLAY_NAME_LEN] = "MirrorSim";
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