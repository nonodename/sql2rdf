#include "sparql-parser/Parser.h"

namespace sparql {

using namespace ast;

namespace {
const char *RDF_TYPE = "http://www.w3.org/1999/02/22-rdf-syntax-ns#type";
const char *RDF_FIRST = "http://www.w3.org/1999/02/22-rdf-syntax-ns#first";
const char *RDF_REST = "http://www.w3.org/1999/02/22-rdf-syntax-ns#rest";
const char *RDF_NIL = "http://www.w3.org/1999/02/22-rdf-syntax-ns#nil";
const char *XSD_INTEGER = "http://www.w3.org/2001/XMLSchema#integer";
const char *XSD_DECIMAL = "http://www.w3.org/2001/XMLSchema#decimal";
const char *XSD_DOUBLE = "http://www.w3.org/2001/XMLSchema#double";
const char *XSD_BOOLEAN = "http://www.w3.org/2001/XMLSchema#boolean";
} // namespace

bool Parser::startsTriple() const {
	return check(TokenType::Var1) || check(TokenType::Var2) || check(TokenType::Iriref) ||
	       check(TokenType::PnameNs) || check(TokenType::PnameLn) || check(TokenType::A) ||
	       check(TokenType::BlankNodeLabel) || check(TokenType::Anon) || check(TokenType::LBracket) ||
	       check(TokenType::LParen) || check(TokenType::Nil) || check(TokenType::StringLiteral) ||
	       check(TokenType::Integer) || check(TokenType::Decimal) || check(TokenType::Double) ||
	       checkKeyword("TRUE") || checkKeyword("FALSE");
}

bool Parser::startsGraphPatternNotTriples() const {
	return check(TokenType::LBrace) || checkKeyword("OPTIONAL") || checkKeyword("MINUS") || checkKeyword("GRAPH") ||
	       checkKeyword("SERVICE") || checkKeyword("FILTER") || checkKeyword("BIND") || checkKeyword("VALUES");
}

std::unique_ptr<GroupGraphPattern> Parser::parseGroupGraphPattern() {
	expectType(TokenType::LBrace, "'{' to start a graph pattern");
	std::unique_ptr<GroupGraphPattern> group(new GroupGraphPattern());
	if (checkKeyword("SELECT")) {
		std::unique_ptr<Query> sub = parseSubSelect();
		group->elements.push_back(std::unique_ptr<GroupElement>(new SubSelectElement(std::move(sub))));
	} else {
		parseGroupGraphPatternSub(*group);
	}
	expectType(TokenType::RBrace, "'}' closing a graph pattern");
	return group;
}

void Parser::parseGroupGraphPatternSub(GroupGraphPattern &group) {
	if (startsTriple()) {
		std::unique_ptr<BasicGraphPattern> bgp(new BasicGraphPattern());
		parseTriplesBlock(*bgp);
		group.elements.push_back(std::unique_ptr<GroupElement>(bgp.release()));
	}
	while (startsGraphPatternNotTriples()) {
		group.elements.push_back(parseGraphPatternNotTriples());
		matchType(TokenType::Dot);
		if (startsTriple()) {
			std::unique_ptr<BasicGraphPattern> bgp(new BasicGraphPattern());
			parseTriplesBlock(*bgp);
			group.elements.push_back(std::unique_ptr<GroupElement>(bgp.release()));
		}
	}
}

std::unique_ptr<GroupElement> Parser::parseGraphPatternNotTriples() {
	if (check(TokenType::LBrace)) return parseGroupOrUnionGraphPattern();
	if (checkKeyword("OPTIONAL")) return parseOptionalGraphPattern();
	if (checkKeyword("MINUS")) return parseMinusGraphPattern();
	if (checkKeyword("GRAPH")) return parseGraphGraphPattern();
	if (checkKeyword("SERVICE")) return parseServiceGraphPattern();
	if (checkKeyword("FILTER")) return parseFilterElement();
	if (checkKeyword("BIND")) return parseBindElement();
	if (checkKeyword("VALUES")) return parseInlineDataElement();
	error("expected OPTIONAL, MINUS, GRAPH, SERVICE, FILTER, BIND, VALUES, or '{'");
	return nullptr;
}

std::unique_ptr<GroupElement> Parser::parseGroupOrUnionGraphPattern() {
	std::unique_ptr<GroupGraphPattern> first = parseGroupGraphPattern();
	if (!checkKeyword("UNION")) return std::unique_ptr<GroupElement>(first.release());
	std::unique_ptr<UnionGraphPattern> u(new UnionGraphPattern());
	u->branches.push_back(std::move(first));
	while (matchKeyword("UNION")) {
		u->branches.push_back(parseGroupGraphPattern());
	}
	return std::unique_ptr<GroupElement>(u.release());
}

std::unique_ptr<GroupElement> Parser::parseOptionalGraphPattern() {
	advance(); // OPTIONAL
	std::unique_ptr<OptionalGraphPattern> opt(new OptionalGraphPattern());
	opt->pattern = parseGroupGraphPattern();
	return std::unique_ptr<GroupElement>(opt.release());
}

std::unique_ptr<GroupElement> Parser::parseMinusGraphPattern() {
	advance(); // MINUS
	std::unique_ptr<MinusGraphPattern> m(new MinusGraphPattern());
	m->pattern = parseGroupGraphPattern();
	return std::unique_ptr<GroupElement>(m.release());
}

std::unique_ptr<GroupElement> Parser::parseGraphGraphPattern() {
	advance(); // GRAPH
	std::unique_ptr<GraphGraphPattern> g(new GraphGraphPattern());
	if (check(TokenType::Var1) || check(TokenType::Var2)) {
		g->graphNameOrVar = std::unique_ptr<Term>(parseVar().release());
	} else {
		g->graphNameOrVar = std::unique_ptr<Term>(parseIri().release());
	}
	g->pattern = parseGroupGraphPattern();
	return std::unique_ptr<GroupElement>(g.release());
}

std::unique_ptr<GroupElement> Parser::parseServiceGraphPattern() {
	advance(); // SERVICE
	std::unique_ptr<ServiceGraphPattern> s(new ServiceGraphPattern());
	s->silent = matchKeyword("SILENT");
	if (check(TokenType::Var1) || check(TokenType::Var2)) {
		s->endpoint = std::unique_ptr<Term>(parseVar().release());
	} else {
		s->endpoint = std::unique_ptr<Term>(parseIri().release());
	}
	s->pattern = parseGroupGraphPattern();
	return std::unique_ptr<GroupElement>(s.release());
}

std::unique_ptr<GroupElement> Parser::parseFilterElement() {
	advance(); // FILTER
	std::unique_ptr<Filter> f(new Filter());
	f->constraint = parsePrimaryExpression();
	return std::unique_ptr<GroupElement>(f.release());
}

std::unique_ptr<GroupElement> Parser::parseBindElement() {
	advance(); // BIND
	expectType(TokenType::LParen, "'(' after BIND");
	std::unique_ptr<Bind> b(new Bind());
	b->expr = parseExpression();
	expectKeyword("AS");
	b->var = parseVar();
	expectType(TokenType::RParen, "')' closing BIND(... AS ?var)");
	return std::unique_ptr<GroupElement>(b.release());
}

std::unique_ptr<GroupElement> Parser::parseInlineDataElement() {
	advance(); // VALUES
	if (check(TokenType::Var1) || check(TokenType::Var2)) {
		return std::unique_ptr<GroupElement>(parseInlineDataOneVar().release());
	}
	return std::unique_ptr<GroupElement>(parseInlineDataFull().release());
}

std::unique_ptr<Term> Parser::parseDataBlockValue() {
	if (matchKeyword("UNDEF")) return nullptr;
	if (check(TokenType::Iriref) || check(TokenType::PnameNs) || check(TokenType::PnameLn)) {
		return std::unique_ptr<Term>(parseIri().release());
	}
	if (check(TokenType::StringLiteral)) return std::unique_ptr<Term>(parseRdfLiteral().release());
	if (check(TokenType::Integer) || check(TokenType::Decimal) || check(TokenType::Double)) {
		return std::unique_ptr<Term>(parseNumericLiteral().release());
	}
	if (checkKeyword("TRUE") || checkKeyword("FALSE")) return std::unique_ptr<Term>(parseBooleanLiteral().release());
	error("expected a VALUES data block value (IRI, literal, or UNDEF)");
	return nullptr;
}

std::unique_ptr<InlineData> Parser::parseInlineDataOneVar() {
	std::unique_ptr<InlineData> data(new InlineData());
	data->vars.push_back(parseVar());
	expectType(TokenType::LBrace, "'{' after VALUES variable");
	while (!check(TokenType::RBrace)) {
		std::vector<std::unique_ptr<Term>> row;
		row.push_back(parseDataBlockValue());
		data->rows.push_back(std::move(row));
	}
	expectType(TokenType::RBrace, "'}' closing VALUES block");
	return data;
}

std::unique_ptr<InlineData> Parser::parseInlineDataFull() {
	std::unique_ptr<InlineData> data(new InlineData());
	if (matchType(TokenType::Nil)) {
		// zero variables
	} else {
		expectType(TokenType::LParen, "'(' to start a VALUES variable list");
		while (check(TokenType::Var1) || check(TokenType::Var2)) data->vars.push_back(parseVar());
		expectType(TokenType::RParen, "')' closing a VALUES variable list");
	}
	expectType(TokenType::LBrace, "'{' after VALUES variable list");
	while (!check(TokenType::RBrace)) {
		std::vector<std::unique_ptr<Term>> row;
		if (!matchType(TokenType::Nil)) {
			expectType(TokenType::LParen, "'(' to start a VALUES row");
			while (!check(TokenType::RParen)) row.push_back(parseDataBlockValue());
			expectType(TokenType::RParen, "')' closing a VALUES row");
		}
		if (row.size() != data->vars.size()) {
			error("VALUES row has " + std::to_string(row.size()) + " value(s) but " +
			      std::to_string(data->vars.size()) + " variable(s) were declared");
		}
		data->rows.push_back(std::move(row));
	}
	expectType(TokenType::RBrace, "'}' closing VALUES block");
	return data;
}

bool Parser::startsVerb(bool pathMode) const {
	if (check(TokenType::A)) return true;
	if (check(TokenType::Var1) || check(TokenType::Var2)) return true;
	if (check(TokenType::Iriref) || check(TokenType::PnameNs) || check(TokenType::PnameLn)) return true;
	if (pathMode && (check(TokenType::Caret) || check(TokenType::Bang) || check(TokenType::LParen))) return true;
	return false;
}

bool Parser::startsTriplesNode() const {
	return check(TokenType::LParen) || check(TokenType::Nil) || check(TokenType::LBracket) || check(TokenType::Anon);
}

void Parser::parseTriplesBlock(BasicGraphPattern &bgp) {
	parseTriplesSameSubject(bgp.triples, /*pathMode=*/true);
	while (matchType(TokenType::Dot)) {
		if (!startsTriple()) break;
		parseTriplesSameSubject(bgp.triples, /*pathMode=*/true);
	}
}

void Parser::parseTriplesSameSubject(std::vector<TriplePattern> &out, bool pathMode) {
	if (startsTriplesNode()) {
		std::unique_ptr<Term> subject = parseTriplesNode(out, pathMode);
		if (startsVerb(pathMode)) parsePropertyListNotEmpty(*subject, out, pathMode);
	} else {
		std::unique_ptr<Term> subject = parseVarOrTerm();
		parsePropertyListNotEmpty(*subject, out, pathMode);
	}
}

void Parser::parsePropertyListNotEmpty(const Term &subject, std::vector<TriplePattern> &out, bool pathMode) {
	std::unique_ptr<PropertyPathExpr> predicate = parsePredicate(pathMode);
	parseObjectList(subject, *predicate, out, pathMode);
	while (matchType(TokenType::Semicolon)) {
		if (!startsVerb(pathMode)) continue;
		predicate = parsePredicate(pathMode);
		parseObjectList(subject, *predicate, out, pathMode);
	}
}

std::unique_ptr<PropertyPathExpr> Parser::parsePredicate(bool pathMode) {
	if (check(TokenType::Var1) || check(TokenType::Var2)) {
		return std::unique_ptr<PropertyPathExpr>(new VariablePath(parseVar()));
	}
	if (pathMode) return parsePath();
	return parseVerbSimple();
}

void Parser::parseObjectList(const Term &subject, const PropertyPathExpr &predicate, std::vector<TriplePattern> &out,
                              bool pathMode) {
	for (;;) {
		TriplePattern tp;
		tp.subject = cloneTerm(subject);
		tp.predicate = clonePath(predicate);
		tp.object = parseGraphNode(out, pathMode);
		out.push_back(std::move(tp));
		if (!matchType(TokenType::Comma)) break;
	}
}

std::unique_ptr<PropertyPathExpr> Parser::parseVerbSimple() {
	if (matchType(TokenType::A)) {
		return std::unique_ptr<PropertyPathExpr>(new PredicatePath(makeIri(RDF_TYPE, "a")));
	}
	if (check(TokenType::Var1) || check(TokenType::Var2)) {
		return std::unique_ptr<PropertyPathExpr>(new VariablePath(parseVar()));
	}
	return std::unique_ptr<PropertyPathExpr>(new PredicatePath(parseIri()));
}

std::unique_ptr<Term> Parser::parseVarOrTerm() {
	if (check(TokenType::Var1) || check(TokenType::Var2)) return std::unique_ptr<Term>(parseVar().release());
	return parseGraphTerm();
}

std::unique_ptr<Term> Parser::parseGraphTerm() {
	if (check(TokenType::Iriref) || check(TokenType::PnameNs) || check(TokenType::PnameLn)) {
		return std::unique_ptr<Term>(parseIri().release());
	}
	if (check(TokenType::StringLiteral)) return std::unique_ptr<Term>(parseRdfLiteral().release());
	if (check(TokenType::Integer) || check(TokenType::Decimal) || check(TokenType::Double)) {
		return std::unique_ptr<Term>(parseNumericLiteral().release());
	}
	if (checkKeyword("TRUE") || checkKeyword("FALSE")) return std::unique_ptr<Term>(parseBooleanLiteral().release());
	if (check(TokenType::BlankNodeLabel)) {
		std::unique_ptr<Term> bn(new BlankNode(current_.text, false));
		advance();
		return bn;
	}
	if (matchType(TokenType::Anon)) return std::unique_ptr<Term>(new BlankNode(freshBlankNodeLabel(), true));
	if (matchType(TokenType::Nil)) return std::unique_ptr<Term>(makeIri(RDF_NIL, "()").release());
	error("expected an IRI, literal, blank node, or '()'");
	return nullptr;
}

std::unique_ptr<Term> Parser::parseGraphNode(std::vector<TriplePattern> &out, bool pathMode) {
	if (startsTriplesNode()) return parseTriplesNode(out, pathMode);
	return parseVarOrTerm();
}

std::unique_ptr<Term> Parser::parseTriplesNode(std::vector<TriplePattern> &out, bool pathMode) {
	if (check(TokenType::LBracket) || check(TokenType::Anon)) return parseBlankNodePropertyList(out, pathMode);
	return parseCollection(out, pathMode);
}

std::unique_ptr<Term> Parser::parseCollection(std::vector<TriplePattern> &out, bool pathMode) {
	if (matchType(TokenType::Nil)) return std::unique_ptr<Term>(makeIri(RDF_NIL, "()").release());

	expectType(TokenType::LParen, "'(' to start a collection");
	std::vector<std::unique_ptr<Term>> elements;
	do {
		elements.push_back(parseGraphNode(out, pathMode));
	} while (!check(TokenType::RParen));
	expectType(TokenType::RParen, "')' closing a collection");

	// Desugar into an rdf:first/rdf:rest chain terminated by rdf:nil (§4.2.3).
	std::unique_ptr<Term> head;
	std::unique_ptr<Term> previousNode;
	for (std::size_t i = 0; i < elements.size(); ++i) {
		std::unique_ptr<Term> node(new BlankNode(freshBlankNodeLabel(), true));
		if (i == 0) head = cloneTerm(*node);
		if (previousNode) {
			TriplePattern restTp;
			restTp.subject = cloneTerm(*previousNode);
			restTp.predicate.reset(new PredicatePath(makeIri(RDF_REST, "rdf:rest")));
			restTp.object = cloneTerm(*node);
			out.push_back(std::move(restTp));
		}
		TriplePattern firstTp;
		firstTp.subject = cloneTerm(*node);
		firstTp.predicate.reset(new PredicatePath(makeIri(RDF_FIRST, "rdf:first")));
		firstTp.object = std::move(elements[i]);
		out.push_back(std::move(firstTp));
		previousNode = std::move(node);
	}
	TriplePattern lastRest;
	lastRest.subject = cloneTerm(*previousNode);
	lastRest.predicate.reset(new PredicatePath(makeIri(RDF_REST, "rdf:rest")));
	lastRest.object = std::unique_ptr<Term>(makeIri(RDF_NIL, "rdf:nil").release());
	out.push_back(std::move(lastRest));
	return head;
}

std::unique_ptr<Term> Parser::parseBlankNodePropertyList(std::vector<TriplePattern> &out, bool pathMode) {
	if (matchType(TokenType::Anon)) return std::unique_ptr<Term>(new BlankNode(freshBlankNodeLabel(), true));
	expectType(TokenType::LBracket, "'[' to start a blank node property list");
	std::unique_ptr<Term> subject(new BlankNode(freshBlankNodeLabel(), true));
	parsePropertyListNotEmpty(*subject, out, pathMode);
	expectType(TokenType::RBracket, "']' closing a blank node property list");
	return subject;
}

std::unique_ptr<RdfLiteral> Parser::parseRdfLiteral() {
	Token str = expectType(TokenType::StringLiteral, "a string literal");
	std::unique_ptr<RdfLiteral> lit(new RdfLiteral(str.text));
	if (check(TokenType::LangTag)) {
		lit->languageTag = current_.text;
		advance();
	} else if (check(TokenType::Caret) && current_.text == "^^") {
		advance();
		lit->datatype = parseIri();
	}
	return lit;
}

std::unique_ptr<RdfLiteral> Parser::parseNumericLiteral() {
	TokenType t = current_.type;
	std::string lex = current_.text;
	advance();
	std::unique_ptr<RdfLiteral> lit(new RdfLiteral(lex));
	const char *dt = XSD_DOUBLE;
	if (t == TokenType::Integer) dt = XSD_INTEGER;
	else if (t == TokenType::Decimal) dt = XSD_DECIMAL;
	lit->datatype = makeIri(dt, "");
	return lit;
}

std::unique_ptr<RdfLiteral> Parser::parseBooleanLiteral() {
	bool isTrue = checkKeyword("TRUE");
	if (!isTrue && !checkKeyword("FALSE")) error("expected 'true' or 'false'");
	advance();
	std::unique_ptr<RdfLiteral> lit(new RdfLiteral(isTrue ? "true" : "false"));
	lit->datatype = makeIri(XSD_BOOLEAN, "");
	return lit;
}

} // namespace sparql
