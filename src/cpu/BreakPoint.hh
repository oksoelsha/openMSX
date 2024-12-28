#ifndef BREAKPOINT_HH
#define BREAKPOINT_HH

#include "BreakPointBase.hh"
#include "CommandException.hh"

#include "strCat.hh"

#include <cstdint>
#include <optional>

namespace openmsx {

class BreakPoint final : public BreakPointBase<BreakPoint>
{
public:
	static constexpr std::string_view prefix = "bp#";

public:
	[[nodiscard]] std::optional<uint16_t> getAddress() const { return address; }
	[[nodiscard]] TclObject getAddressString() const { return addrStr; }
	void setAddress(Interpreter& interp, const TclObject& addr) {
		addrStr = addr;
		evaluateAddress(interp);
	}

	std::string evaluateAddress(Interpreter& interp) {
		try {
			auto tmp = addrStr.eval(interp).getInt(interp); // may throw
			if ((tmp < 0) || (tmp > 0xffff)) {
				throw CommandException("address outside of range");
			}
			address = uint16_t(tmp);
			return {}; // success
		} catch (MSXException& e) {
			address = {};
			return e.getMessage();
		}
	}

private:
	TclObject addrStr;
	std::optional<uint16_t> address; // redundant: calculated from 'addrStr'
};

} // namespace openmsx

#endif
