#pragma once

#include <string>
#include <vector>

#include "sparql2sql/ir/RelNode.h"

namespace sparql2sql {

struct TypeCatalog;

/// One pair of natively-comparable columns underlying an equi-join key: the
/// alias-qualified, dialect-quoted uncast column refs to compare instead of the
/// two sides' VARCHAR-cast / template-constructed term text.
struct NativeKeyPair {
	std::string leftRef;
	std::string rightRef;
};

/// Decide whether an equi-join key between `lc` and `rc` can be compared on
/// native base columns rather than on generated term text, and return the
/// column-ref pairs to AND together. An empty result means "no rewrite - use
/// the rendered term expressions".
///
/// Two cases qualify, both requiring the TypeCatalog to prove every compared
/// pair type-comparable (so a missing or partial catalog simply disables the
/// rewrite):
///  - both sides are a pure, uncast base column (rr:column term maps);
///  - both sides are invertible rr:templates with the same "shape"
///    (identical literal segments and placeholder positions - see
///    TemplateUtil::sameTemplateShape; placeholder *names* may differ, e.g.
///    "ex:data/PROD/{PROD_CODE}" vs "ex:data/PROD/{ITEM_PROD_CODE}"). Equal
///    placeholder values always produce equal template text, and
///    invertibility (no two placeholders textually adjacent) gives the
///    converse, so the two equalities are equivalent. This is the case that
///    matters in practice: R2RML subjects are nearly always templates, so
///    without it subject joins compare constructed IRI strings.
///
/// Never call for a null-tolerant key: `col IS NULL` on the term text is not
/// equivalent to it on the placeholder columns once a template has several.
std::vector<NativeKeyPair> nativeKeyPairs(const ColumnInfo &lc, const ColumnInfo &rc, const TypeCatalog *catalog);

} // namespace sparql2sql
