#pragma once
#include <array>
#include <cstdint>

namespace mdJucePlugin::remotePanel
{
	// Called under RemotePanel's row mutex. Separate contact/session bookkeeping
	// decides whether an edge is new; this table merges logical slots and aliases.
	class RowOwners
	{
	public:
		enum class Change { Unmatched, StillOwned, RowChanged };

		Change change(uint8_t slot, uint8_t row, uint8_t mask, bool down)
		{
			if(slot >= m_slots.size() || row >= m_bits.size()) return Change::Unmatched;
			auto& refs = m_slots[slot];
			if(down)
			{
				if(refs++ != 0) return Change::StillOwned;
				for(unsigned bit = 0; bit < 8; ++bit)
					if(mask & (1u << bit)) ++m_bits[row][bit];
			}
			else
			{
				if(refs == 0) return Change::Unmatched;
				if(--refs != 0) return Change::StillOwned;
				for(unsigned bit = 0; bit < 8; ++bit)
					if((mask & (1u << bit)) && m_bits[row][bit]) --m_bits[row][bit];
			}
			return Change::RowChanged;
		}

		uint8_t mergedRow(uint8_t row, uint8_t desktop) const
		{
			if(row >= m_bits.size()) return desktop;
			for(unsigned bit = 0; bit < 8; ++bit)
				if(m_bits[row][bit]) desktop |= static_cast<uint8_t>(1u << bit);
			return desktop;
		}

		uint32_t count(uint8_t slot) const { return slot < m_slots.size() ? m_slots[slot] : 0; }

	private:
		std::array<uint32_t, 80> m_slots{};
		std::array<std::array<uint32_t, 8>, 7> m_bits{};
	};
}
