#include "sparql2sql/PropertyPathTranslator.h"

#include <string>
#include <utility>
#include <vector>

#include "sparql-parser/ast/Term.h"
#include "sparql2sql/PatternFolder.h"
#include "sparql2sql/SqlDialect.h"
#include "sparql2sql/TagSql.h"
#include "sparql2sql/TermMapSql.h"
#include "sparql2sql/TranslationError.h"
#include "sparql2sql/ir/RelNode.h"

namespace sparql2sql {

namespace {

// A relation binding one variable to one fixed term - the zero-length path
// anchored at a bound endpoint. Shaped like translateInlineData's single-row
// case, and like it leaves renderedExpr empty: a Raw node's columns are only
// ever referenced across a derived-table boundary, by mangled name.
//
// Takes the bound Term rather than its lexical form so the column's term kind
// comes from the term itself; it could not be recovered from the string.
RelNodePtr singleTermRelation(const std::string &varName, const sparql::ast::Term &term, TranslationContext &ctx) {
	const SqlDialect &dialect = ctx.dialect();
	RelNodePtr node(new RawRelation());
	RawRelation &raw = static_cast<RawRelation &>(*node);
	raw.sql = "SELECT " + dialect.stringLiteral(termLexicalForm(term)) + " AS " + mangleVar(varName, dialect);
	ColumnInfo col;
	col.var = varName;
	col.nonNull = true;
	col.term = termInfoOfTerm(term);
	col.tagExpr = tagLiteral(col.term, dialect);
	raw.schema().push_back(col);
	return node;
}

std::vector<std::string> iriValues(const std::vector<std::unique_ptr<sparql::ast::Iri>> &iris) {
	std::vector<std::string> values;
	values.reserve(iris.size());
	for (const auto &iri : iris) {
		values.push_back(iri->value);
	}
	return values;
}

// !(iri1|...|irij|^irij+1|...|^irin): the union of a forward match whose
// predicate is none of the un-inverted IRIs and a reverse match (endpoints
// swapped) whose predicate is none of the inverted ones. A set naming only
// inverse IRIs matches nothing in the forward direction, and vice versa, so
// each arm exists only when its own IRI list is non-empty.
RelNodePtr translateNegatedPropertySet(const sparql::ast::NegatedPropertySet &nps, const TermSpec &subject,
                                       const TermSpec &object, TranslationContext &ctx) {
	std::vector<std::string> forwardIris = iriValues(nps.forward);
	std::vector<std::string> inverseIris = iriValues(nps.inverse);

	std::vector<RelNodePtr> arms;
	// The empty-on-both-sides case can't come out of the grammar; reading it
	// as "exclude nothing, forward" keeps this total without a special case.
	if (!forwardIris.empty() || inverseIris.empty()) {
		arms.push_back(translateAtomicPattern(subject, negatedPredicate(forwardIris), object, ctx));
	}
	if (!inverseIris.empty()) {
		arms.push_back(translateAtomicPattern(object, negatedPredicate(inverseIris), subject, ctx));
	}
	return unionAll(std::move(arms), ctx);
}

// E?: the child path unioned with the zero-length path. Section 9.3 makes the
// result set-valued (a solution appears once however many ways it is reached),
// hence the deduplicating union.
RelNodePtr translateZeroOrOne(const sparql::ast::ZeroOrOnePath &path, const TermSpec &subject, const TermSpec &object,
                              TranslationContext &ctx) {
	RelNodePtr zero = zeroLengthPath(subject, object, ctx);

	// Two bound, equal endpoints: the zero-length path already matches
	// unconditionally and binds nothing, so the whole pattern is satisfied and
	// the child path cannot add a distinct solution.
	if (zero->kind() == RelKind::SingleRow) {
		return zero;
	}

	RelNodePtr some = translatePath(*path.child, subject, object, ctx);

	// Two bound, unequal endpoints: only the child path can connect them.
	if (zero->kind() == RelKind::Empty && zero->schema().empty()) {
		return some;
	}

	std::vector<RelNodePtr> arms;
	arms.push_back(std::move(some));
	arms.push_back(std::move(zero));
	return unionAll(std::move(arms), ctx, /*dedup=*/true);
}

// The one-hop "step" relation shared by every directional case of E+: two
// fresh internal endpoint variables, translated exactly like Sequence's own
// intermediate node (PathKind::Sequence above). Kept as a live RelNodePtr
// (not pre-rendered SQL) so it still gets flatten/selfJoinWalk'd by the
// OUTER tree's optimize() before TransitiveClosureNode's own renderer ever
// sees it - typically a UnionByName of several Spj candidate arms.
struct StepRelation {
	RelNodePtr step;
	std::string fromVar;
	std::string toVar;
};

StepRelation buildStep(const sparql::ast::PropertyPathExpr &child, TranslationContext &ctx) {
	StepRelation sr;
	sr.fromVar = ctx.nextInternalVar();
	sr.toVar = ctx.nextInternalVar();
	sr.step = translatePath(child, varTermSpec(sr.fromVar), varTermSpec(sr.toVar), ctx);
	return sr;
}

// E+: dispatch on which of subject/object are bound to pick the cheapest
// correct closure shape - directional seeding from a bound endpoint needs
// only a unary reachable-node CTE, while two unbound endpoints need the full
// (from, to) pairs closure (see TransitiveClosureNode/renderTransitiveClosure).
RelNodePtr translateOneOrMore(const sparql::ast::PropertyPathExpr &child, const TermSpec &subject,
                              const TermSpec &object, TranslationContext &ctx) {
	StepRelation sr = buildStep(child, ctx);
	RelNodePtr node(new TransitiveClosureNode());
	TransitiveClosureNode &tc = static_cast<TransitiveClosureNode &>(*node);
	const ColumnInfo *fromCol = sr.step->column(sr.fromVar);
	const ColumnInfo *toCol = sr.step->column(sr.toVar);
	tc.fromVar = sr.fromVar;
	tc.toVar = sr.toVar;

	const SqlDialect &dialect = ctx.dialect();

	if (!subject.isVar && !object.isVar) {
		tc.mode = TransitiveClosureNode::Mode::BothBound;
		tc.anchorLiteral = dialect.stringLiteral(termLexicalForm(*subject.boundTerm));
		tc.targetLiteral = dialect.stringLiteral(termLexicalForm(*object.boundTerm));
		// No schema columns: this is a pure existence check, mirroring
		// zeroLengthPath's both-bound cases (identityRelation/EmptyNode).
	} else if (!subject.isVar) {
		tc.mode = TransitiveClosureNode::Mode::ForwardFromSubject;
		tc.anchorLiteral = dialect.stringLiteral(termLexicalForm(*subject.boundTerm));
		ColumnInfo col;
		col.var = object.varName;
		col.nonNull = true;
		col.term = toCol != nullptr ? toCol->term : TermInfo();
		col.tagExpr = tagLiteral(col.term, dialect);
		tc.schema().push_back(col);
	} else if (!object.isVar) {
		tc.mode = TransitiveClosureNode::Mode::BackwardFromObject;
		tc.anchorLiteral = dialect.stringLiteral(termLexicalForm(*object.boundTerm));
		ColumnInfo col;
		col.var = subject.varName;
		col.nonNull = true;
		col.term = fromCol != nullptr ? fromCol->term : TermInfo();
		col.tagExpr = tagLiteral(col.term, dialect);
		tc.schema().push_back(col);
	} else {
		tc.mode = TransitiveClosureNode::Mode::BothVars;
		ColumnInfo s;
		s.var = subject.varName;
		s.nonNull = true;
		s.term = fromCol != nullptr ? fromCol->term : TermInfo();
		s.tagExpr = tagLiteral(s.term, dialect);
		tc.schema().push_back(s);
		if (object.varName != subject.varName) {
			ColumnInfo o;
			o.var = object.varName;
			o.nonNull = true;
			o.term = toCol != nullptr ? toCol->term : TermInfo();
			o.tagExpr = tagLiteral(o.term, dialect);
			tc.schema().push_back(o);
		}
	}

	tc.step = std::move(sr.step);
	return node;
}

// E*: the E+ closure unioned with the zero-length path, mirroring
// translateZeroOrOne exactly (including its two both-bound short-circuits) -
// zeroLengthPath already fully implements SPARQL's ZeroLengthPath semantics,
// so this reuses it rather than reimplementing zero-length handling inside
// TransitiveClosureNode itself.
RelNodePtr translateZeroOrMore(const sparql::ast::ZeroOrMorePath &path, const TermSpec &subject, const TermSpec &object,
                               TranslationContext &ctx) {
	RelNodePtr zero = zeroLengthPath(subject, object, ctx);

	if (zero->kind() == RelKind::SingleRow) {
		return zero;
	}

	RelNodePtr some = translateOneOrMore(*path.child, subject, object, ctx);

	if (zero->kind() == RelKind::Empty && zero->schema().empty()) {
		return some;
	}

	std::vector<RelNodePtr> arms;
	arms.push_back(std::move(some));
	arms.push_back(std::move(zero));
	return unionAll(std::move(arms), ctx, /*dedup=*/true);
}

} // namespace

RelNodePtr zeroLengthPath(const TermSpec &subject, const TermSpec &object, TranslationContext &ctx) {
	if (!subject.isVar && !object.isVar) {
		// Both endpoints fixed: the zero-length path holds exactly when they
		// are the same term, and binds nothing either way.
		if (termLexicalForm(*subject.boundTerm) == termLexicalForm(*object.boundTerm)) {
			return identityRelation(ctx);
		}
		return RelNodePtr(new EmptyNode());
	}

	if (subject.isVar != object.isVar) {
		// One endpoint fixed: it anchors the identity relation, binding the
		// other endpoint's variable to that same term. Per Section 18.4 this
		// holds whether or not the term occurs in the graph.
		const TermSpec &boundEnd = subject.isVar ? object : subject;
		const TermSpec &varEnd = subject.isVar ? subject : object;
		return singleTermRelation(varEnd.varName, *boundEnd.boundTerm, ctx);
	}

	// Two variables, and so nothing to anchor to: range over every term the
	// mapping can produce. `?x <p>? ?x` needs the universe only once.
	std::vector<std::string> varNames;
	varNames.push_back(subject.varName);
	if (object.varName != subject.varName) {
		varNames.push_back(object.varName);
	}
	return allTermsRelation(varNames, ctx);
}

RelNodePtr translatePath(const sparql::ast::PropertyPathExpr &path, const TermSpec &subject, const TermSpec &object,
                         TranslationContext &ctx) {
	using sparql::ast::AlternativePath;
	using sparql::ast::InversePath;
	using sparql::ast::NegatedPropertySet;
	using sparql::ast::OneOrMorePath;
	using sparql::ast::PathKind;
	using sparql::ast::PredicatePath;
	using sparql::ast::SequencePath;
	using sparql::ast::VariablePath;
	using sparql::ast::ZeroOrMorePath;
	using sparql::ast::ZeroOrOnePath;

	switch (path.kind()) {
	case PathKind::Predicate:
		return translateAtomicPattern(subject, constantPredicate(static_cast<const PredicatePath &>(path).iri->value),
		                              object, ctx);

	case PathKind::Variable:
		return translateAtomicPattern(subject, variablePredicate(static_cast<const VariablePath &>(path).var->name),
		                              object, ctx);

	case PathKind::Inverse:
		// ^E: the same path with its endpoints exchanged. Nested inverses fold
		// away on their own.
		return translatePath(*static_cast<const InversePath &>(path).child, object, subject, ctx);

	case PathKind::Sequence: {
		// E1/E2: join over a fresh variable standing for the intermediate node.
		// It is internal, so it never reaches query output, but it stays in the
		// relation's schema - which is what gives sequence paths their
		// spec-mandated cardinality (one solution per distinct route).
		const auto &seq = static_cast<const SequencePath &>(path);
		TermSpec mid = varTermSpec(ctx.nextInternalVar());
		RelNodePtr left = translatePath(*seq.left, subject, mid, ctx);
		RelNodePtr right = translatePath(*seq.right, mid, object, ctx);
		return innerJoin(std::move(left), std::move(right), ctx);
	}

	case PathKind::Alternative: {
		// E1|E2: bag-preserving union, matching SPARQL's own UNION operator.
		const auto &alt = static_cast<const AlternativePath &>(path);
		std::vector<RelNodePtr> arms;
		arms.push_back(translatePath(*alt.left, subject, object, ctx));
		arms.push_back(translatePath(*alt.right, subject, object, ctx));
		return unionAll(std::move(arms), ctx);
	}

	case PathKind::ZeroOrOne:
		return translateZeroOrOne(static_cast<const ZeroOrOnePath &>(path), subject, object, ctx);

	case PathKind::NegatedPropertySet:
		return translateNegatedPropertySet(static_cast<const NegatedPropertySet &>(path), subject, object, ctx);

	case PathKind::ZeroOrMore:
		return translateZeroOrMore(static_cast<const ZeroOrMorePath &>(path), subject, object, ctx);

	case PathKind::OneOrMore:
		return translateOneOrMore(*static_cast<const OneOrMorePath &>(path).child, subject, object, ctx);
	}

	throw TranslationError("unsupported: unrecognized property path operator in predicate position");
}

} // namespace sparql2sql
