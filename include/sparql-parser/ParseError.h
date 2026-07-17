#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>

namespace sparql {

/// Thrown for any SPARQL grammar (syntax) violation. Derives from
/// std::runtime_error so existing `catch (const std::exception &)` /
/// `catch (const std::runtime_error &)` call sites keep working unchanged,
/// while callers that want structured detail can catch ParseError itself
/// and inspect line()/column()/nearText().
class ParseError : public std::runtime_error {
public:
	ParseError(const std::string &message, std::size_t line, std::size_t column, const std::string &nearText);

	std::size_t line() const noexcept { return line_; }
	std::size_t column() const noexcept { return column_; }
	const std::string &nearText() const noexcept { return nearText_; }
	const std::string &message() const noexcept { return message_; }

private:
	static std::string format(const std::string &message, std::size_t line, std::size_t column,
	                           const std::string &nearText);

	std::string message_;
	std::size_t line_;
	std::size_t column_;
	std::string nearText_;
};

} // namespace sparql
