#include "sparql-parser/Parser.h"

namespace sparql {

using namespace ast;

namespace {
const char *RDF_TYPE_PATH = "http://www.w3.org/1999/02/22-rdf-syntax-ns#type";
} // namespace

// Property path grammar (§9.1, rules 88-96), from lowest to highest
// precedence per the spec's own table: '|' (Alternative), '/' (Sequence),
// '^' (Inverse, unary), '*'/'+'/'?' (unary postfix), negated property sets,
// groups, then a bare IRI/'a'.

std::unique_ptr<PropertyPathExpr> Parser::parsePath() {
	return parsePathAlternative();
}

std::unique_ptr<PropertyPathExpr> Parser::parsePathAlternative() {
	std::unique_ptr<PropertyPathExpr> left = parsePathSequence();
	while (matchType(TokenType::Pipe)) {
		std::unique_ptr<PropertyPathExpr> right = parsePathSequence();
		left.reset(new AlternativePath(std::move(left), std::move(right)));
	}
	return left;
}

std::unique_ptr<PropertyPathExpr> Parser::parsePathSequence() {
	std::unique_ptr<PropertyPathExpr> left = parsePathEltOrInverse();
	while (matchType(TokenType::Slash)) {
		std::unique_ptr<PropertyPathExpr> right = parsePathEltOrInverse();
		left.reset(new SequencePath(std::move(left), std::move(right)));
	}
	return left;
}

std::unique_ptr<PropertyPathExpr> Parser::parsePathEltOrInverse() {
	if (matchType(TokenType::Caret))
		return std::unique_ptr<PropertyPathExpr>(new InversePath(parsePathElt()));
	return parsePathElt();
}

std::unique_ptr<PropertyPathExpr> Parser::parsePathElt() {
	std::unique_ptr<PropertyPathExpr> p = parsePathPrimary();
	if (matchType(TokenType::Question))
		return std::unique_ptr<PropertyPathExpr>(new ZeroOrOnePath(std::move(p)));
	if (matchType(TokenType::Star))
		return std::unique_ptr<PropertyPathExpr>(new ZeroOrMorePath(std::move(p)));
	if (matchType(TokenType::Plus))
		return std::unique_ptr<PropertyPathExpr>(new OneOrMorePath(std::move(p)));
	return p;
}

std::unique_ptr<PropertyPathExpr> Parser::parsePathPrimary() {
	if (matchType(TokenType::A)) {
		return std::unique_ptr<PropertyPathExpr>(new PredicatePath(makeIri(RDF_TYPE_PATH, "a")));
	}
	if (matchType(TokenType::Bang))
		return parsePathNegatedPropertySet();
	if (matchType(TokenType::LParen)) {
		std::unique_ptr<PropertyPathExpr> inner = parsePath();
		expectType(TokenType::RParen, "')' closing a grouped property path");
		return inner;
	}
	return std::unique_ptr<PropertyPathExpr>(new PredicatePath(parseIri()));
}

std::unique_ptr<PropertyPathExpr> Parser::parsePathNegatedPropertySet() {
	std::unique_ptr<NegatedPropertySet> n(new NegatedPropertySet());
	if (matchType(TokenType::LParen)) {
		if (!check(TokenType::RParen)) {
			for (;;) {
				bool inv = matchType(TokenType::Caret);
				std::unique_ptr<Iri> iri = matchType(TokenType::A) ? makeIri(RDF_TYPE_PATH, "a") : parseIri();
				if (inv)
					n->inverse.push_back(std::move(iri));
				else
					n->forward.push_back(std::move(iri));
				if (!matchType(TokenType::Pipe))
					break;
			}
		}
		expectType(TokenType::RParen, "')' closing a negated property set");
	} else {
		bool inv = matchType(TokenType::Caret);
		std::unique_ptr<Iri> iri = matchType(TokenType::A) ? makeIri(RDF_TYPE_PATH, "a") : parseIri();
		if (inv)
			n->inverse.push_back(std::move(iri));
		else
			n->forward.push_back(std::move(iri));
	}
	return std::unique_ptr<PropertyPathExpr>(n.release());
}

} // namespace sparql
