#include "sparql-parser/Parser.h"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>

#include "sparql-parser/ParseError.h"

namespace sparql {

using ast::AlternativePath;
using ast::BasicGraphPattern;
using ast::BlankNode;
using ast::DatasetClause;
using ast::DatasetClauseKind;
using ast::GroupCondition;
using ast::GroupElement;
using ast::GroupGraphPattern;
using ast::InlineData;
using ast::InversePath;
using ast::Iri;
using ast::NegatedPropertySet;
using ast::OneOrMorePath;
using ast::OrderCondition;
using ast::OrderDirection;
using ast::PathKind;
using ast::PredicatePath;
using ast::PrefixDecl;
using ast::Prologue;
using ast::PropertyPathExpr;
using ast::Query;
using ast::QueryForm;
using ast::RdfLiteral;
using ast::SelectItem;
using ast::SequencePath;
using ast::SolutionModifier;
using ast::Term;
using ast::TermKind;
using ast::TriplePattern;
using ast::Var;
using ast::VarExpr;
using ast::VariablePath;
using ast::ZeroOrMorePath;
using ast::ZeroOrOnePath;

namespace {

// Best-effort relative-IRI resolution against a base IRI: absolute IRIs
// (containing "://", or otherwise looking like "scheme:...") pass through
// unchanged; "/..." resolves against the base's authority; anything else
// resolves against the base's directory. This deliberately does not
// implement the full RFC 3986 §5.2 algorithm (dot-segment removal, query/
// fragment handling, ...) - see the project plan's noted scope limitation.
std::string resolveRelativeIri(const std::string &base, const std::string &ref) {
	if (base.empty() || ref.empty()) {
		return ref;
	}
	if (ref.find("://") != std::string::npos) {
		return ref;
	}
	std::size_t colon = ref.find(':');
	std::size_t slash = ref.find('/');
	if (colon != std::string::npos && (slash == std::string::npos || colon < slash)) {
		return ref;
	}
	if (ref[0] == '/') {
		std::size_t schemeEnd = base.find("://");
		if (schemeEnd == std::string::npos) {
			return ref;
		}
		std::size_t authorityEnd = base.find('/', schemeEnd + 3);
		std::string authority = (authorityEnd == std::string::npos) ? base : base.substr(0, authorityEnd);
		return authority + ref;
	}
	std::size_t lastSlash = base.rfind('/');
	std::string basePrefix = (lastSlash == std::string::npos) ? base : base.substr(0, lastSlash + 1);
	return basePrefix + ref;
}

} // namespace

std::unique_ptr<Query> Parser::parseFile(const std::string &path) {
	FILE *f = std::fopen(path.c_str(), "rb");
	if (!f) {
		throw std::runtime_error("cannot open SPARQL query file '" + path + "'");
	}
	std::string contents;
	char buf[4096];
	std::size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
		contents.append(buf, n);
	}
	std::fclose(f);
	return parseString(contents);
}

std::unique_ptr<Query> Parser::parseString(const std::string &queryText, const std::string &baseIri) {
	blankNodeCounter_ = 0;
	initLexer(queryText);
	std::unique_ptr<Query> query(new Query());
	query->prologue.baseIri = baseIri;
	prologue_ = &query->prologue;

	parsePrologue(query->prologue);

	if (checkKeyword("SELECT")) {
		parseSelectClause(*query);
		parseDatasetClauses(query->datasetClauses);
		query->where = parseWhereClause();
		parseSolutionModifier(query->solutionModifier);
	} else if (checkKeyword("CONSTRUCT")) {
		parseConstructQuery(*query);
	} else if (checkKeyword("DESCRIBE")) {
		parseDescribeQuery(*query);
	} else if (checkKeyword("ASK")) {
		parseAskQuery(*query);
	} else {
		error("expected SELECT, CONSTRUCT, DESCRIBE or ASK");
	}

	query->valuesClause = parseValuesClauseOptional();
	if (!check(TokenType::Eof)) {
		error("unexpected trailing input after query");
	}
	return query;
}

// ---------------------------------------------------------------------------
// Token-stream plumbing
// ---------------------------------------------------------------------------

void Parser::initLexer(const std::string &text) {
	source_ = preprocessCodepointEscapes(text);
	lexer_.reset(new Lexer(source_));
	advance();
}

void Parser::advance() {
	current_ = lexer_->next();
}

bool Parser::check(TokenType type) const {
	return current_.type == type;
}

bool Parser::checkKeyword(const char *kw) const {
	return current_.type == TokenType::Keyword && current_.keyword == kw;
}

bool Parser::matchType(TokenType type) {
	if (!check(type)) {
		return false;
	}
	advance();
	return true;
}

bool Parser::matchKeyword(const char *kw) {
	if (!checkKeyword(kw)) {
		return false;
	}
	advance();
	return true;
}

Token Parser::expectType(TokenType type, const char *what) {
	if (!check(type)) {
		error(std::string("expected ") + what);
	}
	Token t = current_;
	advance();
	return t;
}

void Parser::expectKeyword(const char *kw) {
	if (!checkKeyword(kw)) {
		error(std::string("expected keyword '") + kw + "'");
	}
	advance();
}

void Parser::error(const std::string &message) const {
	std::string near = current_.text.empty() ? current_.keyword : current_.text;
	throw ParseError(message, current_.line, current_.column, near);
}

// ---------------------------------------------------------------------------
// IRIs / prologue
// ---------------------------------------------------------------------------

std::string Parser::lookupPrefix(const std::string &prefix) const {
	if (prologue_) {
		for (const auto &p : prologue_->prefixes) {
			if (p.prefix == prefix) {
				return p.iri;
			}
		}
	}
	error("unknown prefix '" + prefix + ":'");
	return std::string();
}

std::string Parser::resolveIriValue(TokenType type, const std::string &lexicalForm) const {
	if (type == TokenType::Iriref) {
		return resolveRelativeIri(prologue_ ? prologue_->baseIri : std::string(), lexicalForm);
	}
	std::size_t colon = lexicalForm.find(':');
	std::string prefix = lexicalForm.substr(0, colon);
	std::string local = (colon == std::string::npos) ? std::string() : lexicalForm.substr(colon + 1);
	return lookupPrefix(prefix) + local;
}

std::unique_ptr<Iri> Parser::makeIri(const std::string &absoluteValue, const std::string &lexicalForm) {
	return std::unique_ptr<Iri>(new Iri(absoluteValue, lexicalForm));
}

std::unique_ptr<Iri> Parser::parseIri() {
	if (check(TokenType::Iriref)) {
		std::string lex = current_.text;
		std::string val = resolveIriValue(TokenType::Iriref, lex);
		advance();
		return makeIri(val, "<" + lex + ">");
	}
	if (check(TokenType::PnameNs) || check(TokenType::PnameLn)) {
		std::string lex = current_.text;
		std::string val = resolveIriValue(current_.type, lex);
		advance();
		return makeIri(val, lex);
	}
	error("expected an IRI or prefixed name");
	return nullptr;
}

std::unique_ptr<Var> Parser::parseVar() {
	if (!check(TokenType::Var1) && !check(TokenType::Var2)) {
		error("expected a variable");
	}
	std::unique_ptr<Var> v(new Var(current_.text));
	advance();
	return v;
}

std::unique_ptr<Iri> Parser::cloneIri(const Iri &iri) {
	return std::unique_ptr<Iri>(new Iri(iri.value, iri.lexicalForm));
}

std::unique_ptr<Term> Parser::cloneTerm(const Term &term) {
	switch (term.kind()) {
	case TermKind::Iri: {
		const auto &i = static_cast<const Iri &>(term);
		return std::unique_ptr<Term>(new Iri(i.value, i.lexicalForm));
	}
	case TermKind::Var: {
		const auto &v = static_cast<const Var &>(term);
		return std::unique_ptr<Term>(new Var(v.name));
	}
	case TermKind::BlankNode: {
		const auto &b = static_cast<const BlankNode &>(term);
		return std::unique_ptr<Term>(new BlankNode(b.label, b.anonymous));
	}
	case TermKind::Literal: {
		const auto &l = static_cast<const RdfLiteral &>(term);
		std::unique_ptr<RdfLiteral> lit(new RdfLiteral(l.lexicalForm));
		lit->languageTag = l.languageTag;
		if (l.datatype) {
			lit->datatype = cloneIri(*l.datatype);
		}
		return std::unique_ptr<Term>(lit.release());
	}
	}
	return nullptr;
}

std::unique_ptr<PropertyPathExpr> Parser::clonePath(const PropertyPathExpr &path) {
	switch (path.kind()) {
	case PathKind::Predicate: {
		const auto &p = static_cast<const PredicatePath &>(path);
		return std::unique_ptr<PropertyPathExpr>(new PredicatePath(cloneIri(*p.iri)));
	}
	case PathKind::Variable: {
		const auto &p = static_cast<const VariablePath &>(path);
		return std::unique_ptr<PropertyPathExpr>(new VariablePath(std::unique_ptr<Var>(new Var(p.var->name))));
	}
	case PathKind::Inverse: {
		const auto &p = static_cast<const InversePath &>(path);
		return std::unique_ptr<PropertyPathExpr>(new InversePath(clonePath(*p.child)));
	}
	case PathKind::Sequence: {
		const auto &p = static_cast<const SequencePath &>(path);
		return std::unique_ptr<PropertyPathExpr>(new SequencePath(clonePath(*p.left), clonePath(*p.right)));
	}
	case PathKind::Alternative: {
		const auto &p = static_cast<const AlternativePath &>(path);
		return std::unique_ptr<PropertyPathExpr>(new AlternativePath(clonePath(*p.left), clonePath(*p.right)));
	}
	case PathKind::ZeroOrMore: {
		const auto &p = static_cast<const ZeroOrMorePath &>(path);
		return std::unique_ptr<PropertyPathExpr>(new ZeroOrMorePath(clonePath(*p.child)));
	}
	case PathKind::OneOrMore: {
		const auto &p = static_cast<const OneOrMorePath &>(path);
		return std::unique_ptr<PropertyPathExpr>(new OneOrMorePath(clonePath(*p.child)));
	}
	case PathKind::ZeroOrOne: {
		const auto &p = static_cast<const ZeroOrOnePath &>(path);
		return std::unique_ptr<PropertyPathExpr>(new ZeroOrOnePath(clonePath(*p.child)));
	}
	case PathKind::NegatedPropertySet: {
		const auto &p = static_cast<const NegatedPropertySet &>(path);
		std::unique_ptr<NegatedPropertySet> n(new NegatedPropertySet());
		for (const auto &f : p.forward) {
			n->forward.push_back(cloneIri(*f));
		}
		for (const auto &inv : p.inverse) {
			n->inverse.push_back(cloneIri(*inv));
		}
		return std::unique_ptr<PropertyPathExpr>(n.release());
	}
	}
	return nullptr;
}

std::string Parser::freshBlankNodeLabel() {
	return "__gen" + std::to_string(blankNodeCounter_++);
}

void Parser::parsePrologue(Prologue &prologue) {
	for (;;) {
		if (checkKeyword("BASE")) {
			parseBaseDecl(prologue);
		} else if (checkKeyword("PREFIX")) {
			parsePrefixDecl(prologue);
		} else {
			break;
		}
	}
}

void Parser::parseBaseDecl(Prologue &prologue) {
	advance(); // BASE
	Token t = expectType(TokenType::Iriref, "IRIREF after BASE");
	prologue.baseIri = resolveRelativeIri(prologue.baseIri, t.text);
}

void Parser::parsePrefixDecl(Prologue &prologue) {
	advance(); // PREFIX
	Token ns = expectType(TokenType::PnameNs, "prefix (e.g. 'foaf:') after PREFIX");
	Token iri = expectType(TokenType::Iriref, "IRIREF after PREFIX declaration");
	PrefixDecl decl;
	decl.prefix = ns.text.substr(0, ns.text.size() - 1); // strip trailing ':'
	decl.iri = resolveRelativeIri(prologue.baseIri, iri.text);
	// A later PREFIX for the same label overrides an earlier one.
	for (auto &existing : prologue.prefixes) {
		if (existing.prefix == decl.prefix) {
			existing.iri = decl.iri;
			return;
		}
	}
	prologue.prefixes.push_back(decl);
}

// ---------------------------------------------------------------------------
// Query forms
// ---------------------------------------------------------------------------

void Parser::parseSelectClause(Query &query) {
	query.form = QueryForm::Select;
	expectKeyword("SELECT");
	if (matchKeyword("DISTINCT")) {
		query.distinct = true;
	} else if (matchKeyword("REDUCED")) {
		query.reduced = true;
	}

	if (matchType(TokenType::Star)) {
		query.selectStar = true;
		return;
	}

	do {
		SelectItem item;
		if (check(TokenType::Var1) || check(TokenType::Var2)) {
			item.var = parseVar();
		} else if (matchType(TokenType::LParen)) {
			item.expr = parseExpression();
			expectKeyword("AS");
			item.var = parseVar();
			expectType(TokenType::RParen, "')' after select expression");
		} else {
			error("expected a variable or '(' expression AS variable ')' in SELECT clause");
		}
		query.selectItems.push_back(std::move(item));
	} while (check(TokenType::Var1) || check(TokenType::Var2) || check(TokenType::LParen));

	if (query.selectItems.empty()) {
		error("SELECT clause must project at least one variable, or use '*'");
	}
}

void Parser::parseDatasetClauses(std::vector<DatasetClause> &clauses) {
	while (checkKeyword("FROM")) {
		advance();
		DatasetClause clause;
		if (matchKeyword("NAMED")) {
			clause.kind = DatasetClauseKind::Named;
		} else {
			clause.kind = DatasetClauseKind::Default;
		}
		clause.iri = parseIri();
		clauses.push_back(std::move(clause));
	}
}

std::unique_ptr<GroupGraphPattern> Parser::parseWhereClause() {
	matchKeyword("WHERE");
	return parseGroupGraphPattern();
}

void Parser::parseConstructQuery(Query &query) {
	query.form = QueryForm::Construct;
	advance(); // CONSTRUCT
	if (matchKeyword("WHERE")) {
		// CONSTRUCT WHERE { TriplesTemplate? } - short form: template == pattern.
		expectType(TokenType::LBrace, "'{' after CONSTRUCT WHERE");
		std::vector<TriplePattern> triples;
		if (!check(TokenType::RBrace)) {
			parseConstructTriples(triples);
		}
		expectType(TokenType::RBrace, "'}' closing CONSTRUCT WHERE");

		query.hasConstructTemplate = true;
		std::unique_ptr<GroupGraphPattern> group(new GroupGraphPattern());
		std::unique_ptr<BasicGraphPattern> bgp(new BasicGraphPattern());
		for (auto &tp : triples) {
			TriplePattern copy;
			copy.subject = cloneTerm(*tp.subject);
			copy.predicate = clonePath(*tp.predicate);
			copy.object = cloneTerm(*tp.object);
			query.constructTemplate.push_back(std::move(copy));
			bgp->triples.push_back(std::move(tp));
		}
		group->elements.push_back(std::unique_ptr<GroupElement>(bgp.release()));
		query.where = std::move(group);
		parseDatasetClauses(query.datasetClauses); // grammar allows dataset clauses before WHERE too;
		                                           // tolerated here as a no-op position for the short form.
		parseSolutionModifier(query.solutionModifier);
		return;
	}

	query.hasConstructTemplate = true;
	query.constructTemplate = parseConstructTemplate();
	parseDatasetClauses(query.datasetClauses);
	query.where = parseWhereClause();
	parseSolutionModifier(query.solutionModifier);
}

std::vector<TriplePattern> Parser::parseConstructTemplate() {
	expectType(TokenType::LBrace, "'{' to start a CONSTRUCT template");
	std::vector<TriplePattern> triples;
	if (!check(TokenType::RBrace)) {
		parseConstructTriples(triples);
	}
	expectType(TokenType::RBrace, "'}' closing CONSTRUCT template");
	return triples;
}

void Parser::parseConstructTriples(std::vector<TriplePattern> &out) {
	parseTriplesSameSubject(out, /*pathMode=*/false);
	while (matchType(TokenType::Dot)) {
		if (check(TokenType::RBrace) || check(TokenType::Eof)) {
			break;
		}
		parseTriplesSameSubject(out, /*pathMode=*/false);
	}
}

void Parser::parseDescribeQuery(Query &query) {
	query.form = QueryForm::Describe;
	advance(); // DESCRIBE
	if (matchType(TokenType::Star)) {
		query.describeStar = true;
	} else {
		do {
			if (check(TokenType::Var1) || check(TokenType::Var2)) {
				query.describeTargets.push_back(std::unique_ptr<Term>(parseVar().release()));
			} else {
				query.describeTargets.push_back(std::unique_ptr<Term>(parseIri().release()));
			}
		} while (check(TokenType::Var1) || check(TokenType::Var2) || check(TokenType::Iriref) ||
		         check(TokenType::PnameNs) || check(TokenType::PnameLn));
	}
	parseDatasetClauses(query.datasetClauses);
	if (checkKeyword("WHERE") || check(TokenType::LBrace)) {
		query.where = parseWhereClause();
	}
	parseSolutionModifier(query.solutionModifier);
}

void Parser::parseAskQuery(Query &query) {
	query.form = QueryForm::Ask;
	advance(); // ASK
	parseDatasetClauses(query.datasetClauses);
	query.where = parseWhereClause();
	parseSolutionModifier(query.solutionModifier);
}

std::unique_ptr<Query> Parser::parseSubSelect() {
	std::unique_ptr<Query> query(new Query());
	parseSelectClause(*query);
	query->where = parseWhereClause();
	parseSolutionModifier(query->solutionModifier);
	query->valuesClause = parseValuesClauseOptional();
	return query;
}

// ---------------------------------------------------------------------------
// Solution modifiers
// ---------------------------------------------------------------------------

void Parser::parseSolutionModifier(SolutionModifier &modifier) {
	if (checkKeyword("GROUP")) {
		parseGroupClause(modifier);
	}
	if (checkKeyword("HAVING")) {
		parseHavingClause(modifier);
	}
	if (checkKeyword("ORDER")) {
		parseOrderClause(modifier);
	}
	parseLimitOffsetClauses(modifier);
}

void Parser::parseGroupClause(SolutionModifier &modifier) {
	advance(); // GROUP
	expectKeyword("BY");
	modifier.groupBy.push_back(parseGroupCondition());
	while (startsExpression()) {
		modifier.groupBy.push_back(parseGroupCondition());
	}
}

GroupCondition Parser::parseGroupCondition() {
	GroupCondition cond;
	if (check(TokenType::Var1) || check(TokenType::Var2)) {
		cond.expr.reset(new VarExpr(parseVar()));
		return cond;
	}
	if (matchType(TokenType::LParen)) {
		cond.expr = parseExpression();
		if (matchKeyword("AS")) {
			cond.asVar = parseVar();
		}
		expectType(TokenType::RParen, "')' closing GROUP BY condition");
		return cond;
	}
	if (check(TokenType::Iriref) || check(TokenType::PnameNs) || check(TokenType::PnameLn)) {
		cond.expr = parseIriOrFunctionCall();
		return cond;
	}
	cond.expr = parseBuiltInCallOrAggregateOrExists();
	return cond;
}

void Parser::parseHavingClause(SolutionModifier &modifier) {
	advance(); // HAVING
	modifier.having.push_back(parseExpression());
	while (startsExpression()) {
		modifier.having.push_back(parseExpression());
	}
}

void Parser::parseOrderClause(SolutionModifier &modifier) {
	advance(); // ORDER
	expectKeyword("BY");
	modifier.orderBy.push_back(parseOrderCondition());
	while (startsExpression() || checkKeyword("ASC") || checkKeyword("DESC")) {
		modifier.orderBy.push_back(parseOrderCondition());
	}
}

OrderCondition Parser::parseOrderCondition() {
	OrderCondition cond;
	if (matchKeyword("ASC")) {
		cond.direction = OrderDirection::Asc;
		expectType(TokenType::LParen, "'(' after ASC");
		cond.expr = parseExpression();
		expectType(TokenType::RParen, "')' closing ASC(...)");
		return cond;
	}
	if (matchKeyword("DESC")) {
		cond.direction = OrderDirection::Desc;
		expectType(TokenType::LParen, "'(' after DESC");
		cond.expr = parseExpression();
		expectType(TokenType::RParen, "')' closing DESC(...)");
		return cond;
	}
	cond.direction = OrderDirection::Asc;
	if (check(TokenType::Var1) || check(TokenType::Var2)) {
		cond.expr.reset(new VarExpr(parseVar()));
	} else {
		cond.expr = parseExpression();
	}
	return cond;
}

void Parser::parseLimitOffsetClauses(SolutionModifier &modifier) {
	bool sawOne = false;
	do {
		sawOne = false;
		if (checkKeyword("LIMIT")) {
			advance();
			Token n = expectType(TokenType::Integer, "an integer after LIMIT");
			modifier.hasLimit = true;
			modifier.limit = std::strtol(n.text.c_str(), nullptr, 10);
			sawOne = true;
		} else if (checkKeyword("OFFSET")) {
			advance();
			Token n = expectType(TokenType::Integer, "an integer after OFFSET");
			modifier.hasOffset = true;
			modifier.offset = std::strtol(n.text.c_str(), nullptr, 10);
			sawOne = true;
		}
	} while (sawOne && (checkKeyword("LIMIT") || checkKeyword("OFFSET")));
}

std::unique_ptr<InlineData> Parser::parseValuesClauseOptional() {
	if (!checkKeyword("VALUES")) {
		return nullptr;
	}
	advance();
	if (check(TokenType::Var1) || check(TokenType::Var2)) {
		return parseInlineDataOneVar();
	}
	return parseInlineDataFull();
}

} // namespace sparql
