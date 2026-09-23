#include "sparql2sql/ValuesFolder.h"

#include <set>
#include <string>
#include <vector>

#include "sparql-parser/ast/Expression.h"
#include "sparql-parser/ast/Term.h"
#include "sparql2sql/ExprAnalysis.h"
#include "sparql2sql/TermInfo.h"
#include "sparql2sql/TermMapSql.h"

namespace sparql2sql {

namespace {

using sparql::ast::Bind;
using sparql::ast::ElementKind;
using sparql::ast::Filter;
using sparql::ast::GraphGraphPattern;
using sparql::ast::GroupGraphPattern;
using sparql::ast::InlineData;
using sparql::ast::OptionalGraphPattern;
using sparql::ast::Term;
using sparql::ast::UnionGraphPattern;

/// Whether two VALUES cells denote the very same RDF term - the full term,
/// not just its lexical form: `"1"` and `"1"^^xsd:integer` share a lexical
/// form but are different terms, and folding one in place of the other would
/// change which term maps the inversion accepts.
bool sameTerm(const Term &a, const Term &b) {
	if (a.kind() != b.kind()) {
		return false;
	}
	const TermInfo ia = termInfoOfTerm(a);
	const TermInfo ib = termInfoOfTerm(b);
	return ia.kind == ib.kind && ia.datatypeIri == ib.datatypeIri && ia.lang == ib.lang &&
	       termLexicalForm(a) == termLexicalForm(b);
}

/// The columns of one VALUES block that hold the same constant term in every
/// row, added to `out`. A variable already present with a *different* constant
/// (two VALUES blocks disagreeing) is removed instead: the two inline relations
/// still join to nothing, so the query's answer is empty either way, but only
/// one of the two constants could be folded and neither is the right one to
/// prefer.
void collectConstantColumns(const InlineData &values, ConstantBindings &out) {
	if (values.rows.empty()) {
		return;
	}
	for (std::size_t i = 0; i < values.vars.size(); ++i) {
		const Term *constant = nullptr;
		bool foldable = true;
		for (const auto &row : values.rows) {
			// An UNDEF cell (and a short row, which the grammar does not produce
			// but which costs nothing to tolerate) means the column is not a
			// single constant.
			if (i >= row.size() || !row[i]) {
				foldable = false;
				break;
			}
			if (constant == nullptr) {
				constant = row[i].get();
			} else if (!sameTerm(*constant, *row[i])) {
				foldable = false;
				break;
			}
		}
		if (!foldable || constant == nullptr) {
			continue;
		}
		const std::string &name = values.vars[i]->name;
		ConstantBindings::iterator existing = out.find(name);
		if (existing == out.end()) {
			out.insert(std::make_pair(name, constant));
		} else if (!sameTerm(*existing->second, *constant)) {
			out.erase(existing);
		}
	}
}

/// Variables that must keep being read as variables anywhere in this subtree,
/// plus the "give up entirely" flag an EXISTS sets. See inlineConstantScope's
/// doc comment for what each exclusion protects.
struct Exclusions {
	std::set<std::string> vars;
	bool disabled = false;
};

void noteExpression(const sparql::ast::Expression &expr, Exclusions &out) {
	if (containsExists(expr)) {
		out.disabled = true;
		return;
	}
	std::vector<std::string> refs;
	collectVarRefs(expr, refs);
	out.vars.insert(refs.begin(), refs.end());
}

// Deliberately does not descend into MinusGraphPattern or SubSelectElement:
// neither inherits this scope's bindings, so neither can be broken by them.
void collectExclusions(const GroupGraphPattern &pattern, Exclusions &out) {
	for (const auto &elPtr : pattern.elements) {
		const auto &el = *elPtr;
		switch (el.kind()) {
		case ElementKind::Filter:
			noteExpression(*static_cast<const Filter &>(el).constraint, out);
			break;
		case ElementKind::Bind: {
			const auto &b = static_cast<const Bind &>(el);
			noteExpression(*b.expr, out);
			out.vars.insert(b.var->name);
			break;
		}
		case ElementKind::GroupGraphPattern:
			collectExclusions(static_cast<const GroupGraphPattern &>(el), out);
			break;
		case ElementKind::OptionalGraphPattern:
			collectExclusions(*static_cast<const OptionalGraphPattern &>(el).pattern, out);
			break;
		case ElementKind::UnionGraphPattern:
			for (const auto &branch : static_cast<const UnionGraphPattern &>(el).branches) {
				collectExclusions(*branch, out);
			}
			break;
		case ElementKind::GraphGraphPattern:
			collectExclusions(*static_cast<const GraphGraphPattern &>(el).pattern, out);
			break;
		default:
			break;
		}
	}
}

} // namespace

ConstantBindings inlineConstantScope(const ConstantBindings &inherited, const GroupGraphPattern *pattern,
                                     const InlineData *trailingValues) {
	ConstantBindings scope = inherited;
	if (trailingValues != nullptr) {
		collectConstantColumns(*trailingValues, scope);
	}
	if (pattern != nullptr) {
		for (const auto &elPtr : pattern->elements) {
			if (elPtr->kind() == ElementKind::InlineData) {
				collectConstantColumns(static_cast<const InlineData &>(*elPtr), scope);
			}
		}
	}
	if (scope.empty()) {
		return scope;
	}

	Exclusions excluded;
	if (pattern != nullptr) {
		collectExclusions(*pattern, excluded);
	}
	if (excluded.disabled) {
		return ConstantBindings();
	}
	for (const auto &v : excluded.vars) {
		scope.erase(v);
	}
	return scope;
}

} // namespace sparql2sql
