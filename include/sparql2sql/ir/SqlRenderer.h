#pragma once

#include <vector>

#include "sparql2sql/TranslatedPattern.h"

namespace sparql2sql {

class RelNode;
struct ColumnInfo;

/// Derive a TranslatedPattern's boundVars, optionalVars and termInfo from a
/// relation's schema. The single place those three views of a schema are
/// produced, so they cannot drift apart - in particular, a construction site
/// that forgot termInfo would silently degrade every consumer to Unknown
/// without failing a single test, since Unknown *is* the fallback behaviour.
void fillScopeFromSchema(TranslatedPattern &out, const std::vector<ColumnInfo> &schema);

/// Render an IR relation into a self-contained TranslatedPattern: a SQL string
/// valid to wrap as "(<sql>) AS aliasN", plus the bound/optional variable sets
/// derived from the node's schema. Fresh subquery-wrapper aliases are minted
/// via `ctx.nextAlias()`; base-table aliases baked into the IR at build time
/// are reused as-is.
TranslatedPattern renderRelation(const RelNode &node, TranslationContext &ctx);

} // namespace sparql2sql
