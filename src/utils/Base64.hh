#ifndef BASE64_HH
#define BASE64_HH

#include "MemBuffer.hh"
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace Base64 {
	std::string encode(const uint8_t* input, size_t inSize);
	std::pair<openmsx::MemBuffer<uint8_t>, size_t> decode(std::string_view input);
	bool decode_inplace(std::string_view input, uint8_t* output, size_t outSize);
}

#endif
