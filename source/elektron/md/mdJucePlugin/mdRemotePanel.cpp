#include "mdRemotePanel.h"

#include "mdLib/mddevice.h"
#include "mdLib/mdfrontpanel.h"
#include "mdLib/mdpanel.h"

#include "synthLib/plugin.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>

namespace mdJucePlugin
{
	using namespace remotePanel;

	namespace
	{
		using Clock = std::chrono::steady_clock;

		// Same ±1 packet bound as the desktop editor's encoder burst cap.
		constexpr int g_encoderBurst = 8;
		// Detents waiting beyond this are dropped (and counted) rather than queued.
		constexpr int g_encoderMaxPending = 24;
		constexpr float g_onsetThreshold = 1.0e-3f;	// about -60 dBFS
		constexpr double g_onsetMinSilenceSeconds = 0.03;
		constexpr uint64_t g_audioProbeTimeoutNs = 1000000000ull;

		uint64_t nowNs()
		{
			return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				Clock::now().time_since_epoch()).count());
		}

		uint64_t nowUs() { return nowNs() / 1000; }

		void put32(std::vector<uint8_t>& _v, const uint32_t _x)
		{
			for(int i = 0; i < 4; ++i)
				_v.push_back(static_cast<uint8_t>(_x >> (8 * i)));
		}

		void put64(std::vector<uint8_t>& _v, const uint64_t _x)
		{
			for(int i = 0; i < 8; ++i)
				_v.push_back(static_cast<uint8_t>(_x >> (8 * i)));
		}

		uint16_t get16(const uint8_t* _p) { return static_cast<uint16_t>(_p[0] | (_p[1] << 8)); }

		uint32_t get32(const uint8_t* _p)
		{
			return uint32_t(_p[0]) | (uint32_t(_p[1]) << 8) | (uint32_t(_p[2]) << 16) | (uint32_t(_p[3]) << 24);
		}

		std::string percentiles(std::vector<uint32_t> _v, const double _scale, const char* _unit)
		{
			if(_v.empty())
				return "n=0";
			std::sort(_v.begin(), _v.end());
			const auto at = [&](const double _q)
			{
				return _v[std::min(_v.size() - 1, static_cast<size_t>(_q * static_cast<double>(_v.size())))] * _scale;
			};
			char buf[160];
			std::snprintf(buf, sizeof(buf), "n=%zu p50=%.2f%s p95=%.2f%s max=%.2f%s", _v.size(),
				at(0.5), _unit, at(0.95), _unit, _v.back() * _scale, _unit);
			return buf;
		}
	}

	RemotePanel::RemotePanel(synthLib::Plugin& _plugin, const md::MachineModel _model, Config _config)
		: m_plugin(_plugin), m_model(_model), m_config(std::move(_config))
		, m_server(Server::Callbacks{
			[this](const ClientPtr& _c) { onConnect(_c); },
			[this](const ClientPtr& _c, const uint8_t* _d, const size_t _n) { onMessage(_c, _d, _n); },
			[this](const ClientPtr& _c) { onDisconnect(_c); },
			[this](Server::Client& _c) { return buildFrame(_c); },
			[this](Server::Client& _c) { return buildPanelFrame(_c); }}, m_config.webRoot)
	{
		if(!m_server.start(m_config.port, m_config.portAttempts))
		{
			log("failed to bind a port in " + std::to_string(m_config.port) + "+"
				+ std::to_string(m_config.portAttempts));
			return;
		}
		m_running = true;
		m_pump = std::thread([this] { pumpLoop(); });
		m_captureWorker = std::thread([this] {
			while(m_running) {
				servicePanelCapture();
				// Woken by the screenshot callback; the timeout only bounds shutdown.
				std::unique_lock lock(m_panelMutex);
				m_captureCv.wait_for(lock, std::chrono::milliseconds(20),
					[this] { return !m_running || !m_pendingCapture.image.isNull(); });
			}
		});
		log(std::string("listening on port ") + std::to_string(m_server.port()) + " model "
			+ (m_model == md::MachineModel::Monomachine ? "MM" : "MD") + " web root " + m_config.webRoot);
	}

	RemotePanel::~RemotePanel()
	{
		stopTimer();
		cancelPendingUpdate();
		m_captureLifetime.reset();
		m_running = false;
		m_captureCv.notify_all();
		if(m_pump.joinable())
			m_pump.join();
		if(m_captureWorker.joinable()) m_captureWorker.join();
		// Disconnect callbacks release every remote hold through the device.
		m_server.stop();
		cancelPendingUpdate();
	}

	bool RemotePanel::pushPanel(const uint8_t _command, const uint8_t _argument, bool& _haveDevice)
	{
		return m_plugin.withDeviceLocked([&](synthLib::Device* const _device)
		{
			auto* const device = dynamic_cast<md::Device*>(_device);
			_haveDevice = device != nullptr;
			return device && device->sendPanelEvent(_command, _argument);
		});
	}

	bool RemotePanel::submitEditorRow(const uint8_t _row, const uint8_t _mask)
	{
		std::lock_guard lock(m_rowMutex);
		m_editorRows[_row - 0x20] = _mask;
		return sendRowLocked(_row) == AckStatus::Accepted;
	}

	AckStatus RemotePanel::sendRowLocked(const uint8_t _row)
	{
		const auto index = static_cast<size_t>(_row - 0x20);
		const auto mask = m_rowOwners.mergedRow(static_cast<uint8_t>(index), m_editorRows[index]);
		bool haveDevice = false;
		if(pushPanel(_row, mask, haveDevice))
			return AckStatus::Accepted;
		if(!haveDevice)
			return AckStatus::NoDevice;
		// The panel queue retains rejected row state and re-sends it itself.
		++m_rejectedRows;
		return AckStatus::Rejected;
	}

	AckStatus RemotePanel::applySlot(const uint8_t _slot, const bool _down)
	{
		std::optional<md::PanelPacket> packet;
		if(_slot < g_buttonSlots)
			packet = md::panelPacket(m_model, static_cast<md::PanelControl>(_slot));
		else
			packet = md::panelEncoderPressPacket(m_model, static_cast<md::PanelEncoder>(_slot - g_buttonSlots));
		if(!packet || !isRowCommand(packet->row))
			return AckStatus::Unsupported;

		std::lock_guard lock(m_rowMutex);
		const auto change = m_rowOwners.change(_slot, packet->row - 0x20, packet->mask, _down);
		if(change == RowOwners::Change::Unmatched) return AckStatus::Ignored;
		if(change == RowOwners::Change::StillOwned) return AckStatus::Accepted;
		return sendRowLocked(packet->row);
	}

	AckStatus RemotePanel::handleHold(const uint32_t _clientId, const uint8_t _slot, const bool _down,
		const uint16_t _contact)
	{
		std::lock_guard lock(m_clientsMutex);
		const auto client=m_clients.find(_clientId);
		if(client==m_clients.end()) return AckStatus::Ignored;
		auto& holds=client->second.holds;
		const auto key=std::make_pair(_contact,_slot);
		if(_down) {
			if(holds.count(key)) return AckStatus::Ignored;
			holds[key]=true;
		} else if(!holds.erase(key)) return AckStatus::Ignored;
		const auto status=applySlot(_slot,_down);
		if(_down && status==AckStatus::Unsupported) holds.erase(key);
		return status;
	}

	AckStatus RemotePanel::handleEncoderDelta(const uint8_t _encoder, const int _detents)
	{
		const auto command = md::panelEncoderCommand(m_model, static_cast<md::PanelEncoder>(_encoder));
		if(!command || _encoder >= g_encoderSlots)
			return AckStatus::Unsupported;

		std::lock_guard lock(m_encoderMutex);
		auto& pending = m_encoderPending[_encoder];
		// A reversal cancels what was still waiting in the old direction.
		if((pending > 0 && _detents < 0) || (pending < 0 && _detents > 0))
			pending = 0;
		pending += _detents;
		auto status = AckStatus::Accepted;
		if(std::abs(pending) > g_encoderMaxPending)
		{
			m_clampedDetents += static_cast<uint64_t>(std::abs(pending) - g_encoderMaxPending);
			pending = pending > 0 ? g_encoderMaxPending : -g_encoderMaxPending;
			status = AckStatus::Clamped;
		}
		const int count = std::min(std::abs(pending), g_encoderBurst);
		const auto argument = static_cast<uint8_t>(pending > 0 ? 0x01 : 0xff);
		const int sign = pending > 0 ? 1 : -1;
		for(int i = 0; i < count; ++i)
		{
			bool haveDevice = false;
			if(!pushPanel(*command, argument, haveDevice))
			{
				if(!haveDevice)
					return AckStatus::NoDevice;
				++m_droppedPulses;
				status = AckStatus::Rejected;
			}
			pending -= sign;
		}
		return status;
	}

	void RemotePanel::flushEncoderRemainders()
	{
		std::lock_guard lock(m_encoderMutex);
		for(uint8_t e = 0; e < g_encoderSlots; ++e)
		{
			auto& pending = m_encoderPending[e];
			if(!pending)
				continue;
			const auto command = md::panelEncoderCommand(m_model, static_cast<md::PanelEncoder>(e));
			if(!command)
			{
				pending = 0;
				continue;
			}
			const int count = std::min(std::abs(pending), g_encoderBurst);
			const auto argument = static_cast<uint8_t>(pending > 0 ? 0x01 : 0xff);
			const int sign = pending > 0 ? 1 : -1;
			for(int i = 0; i < count; ++i)
			{
				bool haveDevice = false;
				if(!pushPanel(*command, argument, haveDevice))
					++m_droppedPulses;
				pending -= sign;
			}
		}
	}

	void RemotePanel::releaseClient(const uint32_t _clientId)
	{
		std::map<std::pair<uint16_t, uint8_t>, bool> holds;
		{
			std::lock_guard lock(m_clientsMutex);
			const auto it = m_clients.find(_clientId);
			if(it == m_clients.end())
				return;
			it->second.lastReleaseUs=nowUs();
			holds.swap(it->second.holds);
			for(const auto& h : holds) (void)applySlot(h.first.second, false);
		}
		{ std::lock_guard lock(m_touchMutex);
			// A release supersedes every queued event from this session; in particular
			// a queued down must not resurrect a hold after disconnect/source loss.
			m_touchEvents.erase(std::remove_if(m_touchEvents.begin(),m_touchEvents.end(),[&](const TouchEvent& e) {
				return (e.client && e.client->id==_clientId) || (e.phase==4 && e.generation==_clientId);
			}),m_touchEvents.end());
			TouchEvent e; e.phase=4; e.generation=_clientId; m_touchEvents.push_back(e);
		}
		triggerAsyncUpdate();
	}

	void RemotePanel::onConnect(const ClientPtr& _client)
	{
		{
			std::lock_guard lock(m_clientsMutex);
			m_clients[_client->id] = {};
		}
		m_server.send(_client, {static_cast<uint8_t>(Msg::Info),
			static_cast<uint8_t>(m_model == md::MachineModel::Monomachine ? 1 : 0), g_protocolVersion});
		m_server.invalidateFrame(_client);	// full frame on (re)connect
		log("client " + std::to_string(_client->id) + " connected from " + _client->peer);
	}

	void RemotePanel::onDisconnect(const ClientPtr& _client)
	{
		if(_client->panelSubscribed.exchange(false)) --m_panelClients;
		releaseClient(_client->id);
		std::lock_guard lock(m_clientsMutex);
		m_clients.erase(_client->id);
		log("client " + std::to_string(_client->id) + " disconnected, holds released");
	}

	void RemotePanel::onMessage(const ClientPtr& _client, const uint8_t* _data, const size_t _size)
	{
		const auto receivedNs = nowNs();
		const auto type = static_cast<Msg>(_data[0]);
		switch(type)
		{
		case Msg::Ping:
			if(_size >= 9)
			{
				std::vector<uint8_t> pong;
				pong.reserve(17);
				pong.push_back(static_cast<uint8_t>(Msg::Pong));
				pong.insert(pong.end(), _data + 1, _data + 9);
				put64(pong, receivedNs / 1000);
				m_server.send(_client, std::move(pong));
			}
			return;
		case Msg::ReleaseAll: {
			std::lock_guard panelLock(m_panelMutex);
			releaseClient(_client->id);
			return;
		}
		case Msg::Hello:
			if(_size == 2 && _data[1] >= 3) {
				if(!_client->panelSubscribed.exchange(true)) ++m_panelClients;
				m_controlsRequested=true;
				sendSource(_client);
			}
			return;
		case Msg::Touch:
			queueTouch(_client, _data, _size, receivedNs/1000);
			return;
		case Msg::PanelReady:
			if(_size==13 && m_sourceAvailable && get32(_data+1)==m_geometryGeneration
				&& get32(_data+1)==_client->panelDeliveredGeneration
				&& (uint64_t(get32(_data+5)) | uint64_t(get32(_data+9))<<32)<=_client->panelDeliveredSequence && (get32(_data+5)||get32(_data+9)))
				_client->panelReadyGeneration=get32(_data+1);
			return;
		case Msg::Button:
		case Msg::EncoderPress:
		case Msg::EncoderDelta:
			break;
		default:
			return;
		}
		if(_size < 18)
			return;

		const uint8_t index = _data[1];
		const uint8_t value = _data[2];
		const uint8_t flags = _data[3];
		const uint16_t contact = get16(_data + 4);
		const uint32_t seq = get32(_data + 6);
		++m_inputs;

		std::unique_lock<std::mutex> panelLock(m_panelMutex, std::defer_lock);
		if(_client->panelSubscribed) panelLock.lock();
		AckStatus status;
		if(_client->panelSubscribed && (!m_sourceAvailable || nowUs()-m_lastCaptureUs.load()>750000 || _client->panelReadyGeneration!=m_geometryGeneration))
			status = AckStatus::Unavailable;
		else if(type == Msg::Button)
			status = index < g_buttonSlots ? handleHold(_client->id, index, value != 0, contact) : AckStatus::Unsupported;
		else if(type == Msg::EncoderPress)
			status = index < g_encoderSlots
				? handleHold(_client->id, static_cast<uint8_t>(g_buttonSlots + index), value != 0, contact)
				: AckStatus::Unsupported;
		else
			status = handleEncoderDelta(index, static_cast<int8_t>(value));

		const auto doneNs = nowNs();
		uint32_t epoch = m_inputEpoch.load();
		if(status == AckStatus::Accepted || status == AckStatus::Clamped || status == AckStatus::Rejected)
		{
			epoch = ++m_inputEpoch;
			uint64_t expected = 0;
			m_lcdProbeStartUs.compare_exchange_strong(expected, doneNs / 1000);
			std::lock_guard lock(m_statsMutex);
			m_stats.enqueueUs.push_back(static_cast<uint32_t>((doneNs - receivedNs) / 1000));
		}
		if((flags & g_flagAudioProbe) && value != 0 && type == Msg::Button)
		{
			uint64_t expected = 0;
			m_audioProbeArmedNs.compare_exchange_strong(expected, receivedNs);
		}

		std::vector<uint8_t> ack;
		ack.reserve(14);
		ack.push_back(static_cast<uint8_t>(Msg::Ack));
		put32(ack, seq);
		ack.push_back(static_cast<uint8_t>(status));
		put32(ack, epoch);
		put32(ack, static_cast<uint32_t>((doneNs - receivedNs) / 1000));
		m_server.send(_client, std::move(ack));
	}

	std::vector<uint8_t> RemotePanel::buildFrame(Server::Client& _client)
	{
		const auto frameUs=nowUs();
		if(frameUs-_client.v2LastFrameUs < 1000000/std::max<uint32_t>(1,m_config.maxFps)) return {};
		Snapshot snap;
		{
			std::lock_guard lock(m_snapshotMutex);
			if(!m_snapshot.sequence)
				return {};
			if(_client.sentValid && _client.sentSnapshot == m_snapshot.sequence)
				return {};
			snap = m_snapshot;
		}
		uint16_t tileMask = 0;
		for(uint32_t t = 0; t < g_tileCount; ++t)
			if(!_client.sentValid || std::memcmp(&snap.vram[t * g_tileBytes], &_client.sentVram[t * g_tileBytes], g_tileBytes) != 0)
				tileMask |= static_cast<uint16_t>(1u << t);
		const bool ledsChanged = !_client.sentValid || snap.leds != _client.sentLeds || snap.written != _client.sentWritten;
		_client.sentSnapshot = snap.sequence;
		if(!tileMask && !ledsChanged)
			return {};

		std::vector<uint8_t> msg;
		msg.reserve(1 + 4 + 4 + 2 + 2 + g_ledBanks + g_vramBytes);
		msg.push_back(static_cast<uint8_t>(Msg::Frame));
		put32(msg, static_cast<uint32_t>(snap.sequence));
		put32(msg, snap.inputEpoch);
		msg.push_back(static_cast<uint8_t>(snap.written));
		msg.push_back(static_cast<uint8_t>(snap.written >> 8));
		msg.push_back(static_cast<uint8_t>(tileMask));
		msg.push_back(static_cast<uint8_t>(tileMask >> 8));
		msg.insert(msg.end(), snap.leds.begin(), snap.leds.end());
		for(uint32_t t = 0; t < g_tileCount; ++t)
			if(tileMask & (1u << t))
				msg.insert(msg.end(), &snap.vram[t * g_tileBytes], &snap.vram[t * g_tileBytes] + g_tileBytes);

		_client.v2LastFrameUs=frameUs;
		_client.sentVram = snap.vram;
		_client.sentLeds = snap.leds;
		_client.sentWritten = snap.written;
		_client.sentValid = true;
		++m_framesSent;
		m_bytesSent += msg.size();
		return msg;
	}

	bool RemotePanel::pollDisplay()
	{
		if(!m_publisher)
			return false;
		const auto epoch = m_inputEpoch.load();
		const auto published = m_publisher->readPublishedState();
		const auto& fp = published.panel;

		Snapshot next;
		for(uint32_t half = 0; half < 2; ++half)
			for(uint32_t page = 0; page < 8; ++page)
				for(uint32_t col = 0; col < 64; ++col)
					next.vram[(half * 8 + page) * 64 + col] = fp.getLcdVram(half, page, col);
		for(uint32_t b = 0; b < g_ledBanks; ++b)
		{
			next.leds[b] = fp.getLedBankRaw(static_cast<uint8_t>(0x20 + b));
			if(fp.wasLedBankWritten(static_cast<uint8_t>(0x20 + b)))
				next.written |= static_cast<uint16_t>(1u << b);
		}

		std::lock_guard lock(m_snapshotMutex);
		if(m_snapshot.sequence && next.vram == m_snapshot.vram && next.leds == m_snapshot.leds
			&& next.written == m_snapshot.written)
			return false;
		next.sequence = m_snapshot.sequence + 1;
		next.inputEpoch = epoch;
		m_snapshot = next;
		++m_displayChanges;
		++m_displaySequence;
		return true;
	}

	void RemotePanel::pumpLoop()
	{
		const auto frameInterval = std::chrono::microseconds(1000000 / std::max<uint32_t>(1, m_config.maxFps));
		auto lastNotify = Clock::now() - std::chrono::seconds(1);
		auto lastPublisher = Clock::now() - std::chrono::seconds(1);
		auto lastStats = Clock::now();
		auto lastReap = Clock::now();
		bool notifyPending = false;

		while(m_running)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(3));
			const auto now = Clock::now();

			if(now - lastPublisher > std::chrono::milliseconds(500))
			{
				lastPublisher = now;
				m_publisher = m_plugin.withDeviceLocked([](synthLib::Device* const _device)
				{
					auto* const device = dynamic_cast<md::Device*>(_device);
					return device ? device->getFrontPanelPublisher() : std::shared_ptr<md::FrontPanelPublisher>{};
				});
			}

			if(m_sourceAvailable && nowUs()-m_lastCaptureUs.load()>750000) sourceUnavailable(3);
			flushEncoderRemainders();

			if(pollDisplay())
			{
				notifyPending = true;
				if(const auto start = m_lcdProbeStartUs.exchange(0))
				{
					std::lock_guard lock(m_statsMutex);
					m_stats.lcdUs.push_back(static_cast<uint32_t>(nowUs() - start));
				}
			}
			if((notifyPending || m_server.clientCount()) && now - lastNotify >= std::min(frameInterval, std::chrono::microseconds(3000)))
			{
				notifyPending = false;
				lastNotify = now;
				m_server.notifyFrame();
			}

			if(const auto onset = m_audioProbeResultNs.exchange(0))
			{
				const auto armed = m_audioProbeArmedNs.exchange(0);
				if(armed && onset > armed)
				{
					{
						std::lock_guard lock(m_statsMutex);
						m_stats.audioUs.push_back(static_cast<uint32_t>((onset - armed) / 1000));
					}
					char buf[128];
					std::snprintf(buf, sizeof(buf), "onset recv->rendered %.2f ms (block offset %u of %u frames)",
						static_cast<double>(onset - armed) / 1.0e6, m_audioProbeOffsetFrames.load(),
						m_audioBlockFrames.load());
					log(buf);
				}
			}
			else if(const auto armed = m_audioProbeArmedNs.load())
			{
				if(nowNs() - armed > g_audioProbeTimeoutNs)
				{
					m_audioProbeArmedNs = 0;
					++m_audioProbeTimeouts;
				}
			}

			if(now - lastReap > std::chrono::milliseconds(100))
			{
				lastReap = now;
				m_server.reap();
			}

			const auto elapsed = std::chrono::duration<double>(now - lastStats).count();
			if(elapsed >= 5.0)
			{
				lastStats = now;
				reportStats(elapsed);
			}
		}
	}

	void RemotePanel::reportStats(const double _seconds)
	{
		Stats stats;
		{
			std::lock_guard lock(m_statsMutex);
			std::swap(stats, m_stats);
		}
		md::PanelInputQueueStatus queue;
		m_plugin.withDeviceLocked([&](synthLib::Device* const _device)
		{
			if(auto* const device = dynamic_cast<md::Device*>(_device))
				queue = device->getPanelInputStatus();
		});
		const auto frames = m_framesSent.exchange(0);
		const auto bytes = m_bytesSent.exchange(0);
		const auto changes = m_displayChanges.exchange(0);
		const auto clients = m_server.clientCount();

		std::ostringstream line;
		line.setf(std::ios::fixed);
		line.precision(1);
		line << "stats clients=" << clients << " displayChanges/s=" << changes / _seconds
			<< " framesSent/s=" << frames / _seconds << " KB/s=" << bytes / _seconds / 1024.0
			<< " captureRequested=" << m_captureRequested.load() << " captureAccepted=" << m_captureAccepted.load()
			<< " captureCompleted=" << m_captureCompleted.load() << " panelBytes=" << m_panelBytes.load()
			<< " panelFrames=" << m_panelFrames.load() << " touches=" << m_touchReceived.load()
			<< " inputs=" << m_inputs.load()
			<< " | recv->enqueued " << percentiles(stats.enqueueUs, 0.001, "ms")
			<< " | enqueued->LCD/LED published " << percentiles(stats.lcdUs, 0.001, "ms")
			<< " | recv->audio onset rendered " << percentiles(stats.audioUs, 0.001, "ms")
			<< " (peak " << 20.0 * std::log10(std::max(1.0e-9, static_cast<double>(m_audioPeak.exchange(0.0f))))
			<< " dBFS, block " << m_audioBlockFrames.load() << " @ " << m_audioSampleRate.load() << " Hz, probe timeouts "
			<< m_audioProbeTimeouts.load() << ")"
			<< " | rejectedRows=" << m_rejectedRows.load() << " droppedPulses=" << m_droppedPulses.load()
			<< " clampedDetents=" << m_clampedDetents.load()
			<< " queue{pending=" << queue.pendingPackets << " overflow=" << queue.overflowCount
			<< " rejected=" << queue.rejectedPackets << " droppedPulse=" << queue.droppedPulsePackets
			<< " recoveredRow=" << queue.recoveredRowPackets << "}"
			<< " droppedControlMsgs=" << m_server.droppedControlMessages();
		log(line.str());

		std::ostringstream json;
		json << "{\"t\":\"stats\",\"clients\":" << clients << ",\"framesPerSec\":" << frames / _seconds
			<< ",\"rejectedRows\":" << m_rejectedRows.load() << ",\"droppedPulses\":" << m_droppedPulses.load()
			<< ",\"clampedDetents\":" << m_clampedDetents.load() << ",\"queueOverflow\":" << queue.overflowCount
			<< ",\"queueDroppedPulse\":" << queue.droppedPulsePackets << "}";
		const auto text = json.str();
		m_server.sendToAll(std::vector<uint8_t>(text.begin(), text.end()), true);
	}

	void RemotePanel::processAudio(const float* _left, const float* _right, const int _frames,
		const double _sampleRate, const uint64_t _callbackStartNs) noexcept
	{
		m_audioBlockFrames.store(static_cast<uint32_t>(_frames), std::memory_order_relaxed);
		m_audioSampleRate.store(static_cast<uint32_t>(_sampleRate), std::memory_order_relaxed);
		if(!_left || _frames <= 0 || _sampleRate <= 0.0)
			return;
		const auto armed = m_audioProbeArmedNs.load(std::memory_order_acquire);
		const auto minSilence = static_cast<uint64_t>(_sampleRate * g_onsetMinSilenceSeconds);
		float peak = m_audioPeak.load(std::memory_order_relaxed);
		for(int i = 0; i < _frames; ++i)
		{
			const float a = std::max(std::fabs(_left[i]), _right ? std::fabs(_right[i]) : 0.0f);
			peak = std::max(peak, a);
			if(a < g_onsetThreshold)
			{
				++m_silentFrames;
				continue;
			}
			if(armed && m_silentFrames >= minSilence)
			{
				const auto onset = _callbackStartNs + static_cast<uint64_t>(i * 1.0e9 / _sampleRate);
				if(onset > armed && m_audioProbeResultNs.load(std::memory_order_relaxed) == 0)
				{
					m_audioProbeOffsetFrames.store(static_cast<uint32_t>(i), std::memory_order_relaxed);
					m_audioProbeResultNs.store(onset, std::memory_order_release);
				}
			}
			m_silentFrames = 0;
		}
		m_audioPeak.store(peak, std::memory_order_relaxed);
	}

	void RemotePanel::log(const std::string& _line)
	{
		std::lock_guard lock(m_logMutex);
		std::printf("[remotePanel] %s\n", _line.c_str());
		std::fflush(stdout);
		if(m_config.logFile.empty())
			return;
		std::ofstream out(m_config.logFile, std::ios::app);
		const auto t = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
		char stamp[32];
		std::snprintf(stamp, sizeof(stamp), "%.3f ", t);
		out << stamp << _line << '\n';
	}
}
