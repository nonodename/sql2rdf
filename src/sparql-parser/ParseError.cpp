#include "sparql-parser/ParseError.h"

namespace sparql {

std::string ParseError::format(const std::string &message, std::size_t line, std::size_t column,
                               const std::string &nearText) {
	std::string result = "Parse error at line " + std::to_string(line) + ", column " + std::to_string(column);
	if (!nearText.empty()) {
		result += ", near '" + nearText + "'";
	}
	result += ": " + message;
	return result;
}

ParseError::ParseError(const std::string &message, std::size_t line, std::size_t column, const std::string &nearText)
    : std::runtime_error(format(message, line, column, nearText)), message_(message), line_(line), column_(column),
      nearText_(nearText) {
}

} // namespace sparql
