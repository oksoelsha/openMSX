#ifndef TEMPORARYSTRING_HH
#define TEMPORARYSTRING_HH

#include "StringStorage.hh"
#include "zstring_view.hh"
#include <array>
#include <concepts>
#include <iostream>
#include <string_view>


/** TemporaryString
 *
 * This a type meant to store temporary strings. Typically it stores the string
 * in the object itself. Except when the string is too large, then it falls
 * back to heap-allocation. So it's more or less like a std::string but with a
 * relatively large (127 character) "small-string-buffer". (Typical std::string
 * implementations only have a buffer for 15 or 23 characters).
 *
 * Because of this internal buffer, TemporaryString objects are quite large.
 * The intention is that they are only used as local variable (so stored on the
 * stack). Not as member variables.
 *
 * The API of this type is still very minimal. It can be extended when needed.
 */
class TemporaryString {
public:
	static constexpr size_t BUFSIZE = 127;

	TemporaryString(size_t n_, std::invocable<char*> auto fillOp)
		: n(n_)
	{
		if (n <= BUFSIZE) {
			ptr = buffer.data();
		} else {
			owner = allocate_string_storage(n + 1);
			ptr = owner.get();
		}
		fillOp(ptr);
		ptr[n] = '\0';
	}
	TemporaryString(const TemporaryString&) = delete;
	TemporaryString(TemporaryString&&) = delete;
	TemporaryString& operator=(const TemporaryString&) = delete;
	TemporaryString& operator=(TemporaryString&&) = delete;
	~TemporaryString() = default;

	[[nodiscard]]       char* data()        { return ptr; }
	[[nodiscard]] const char* c_str() const { return ptr; }

	[[nodiscard]] operator std::string_view() const { return {ptr, n}; }
	[[nodiscard]] operator     zstring_view() const { return {ptr, n}; }

private:
	size_t n;
	char* ptr;
	StringStorage owner;
	std::array<char, BUFSIZE + 1> buffer;
};

inline std::ostream& operator<<(std::ostream& os, const TemporaryString& str)
{
	os << std::string_view(str);
	return os;
}

#endif
