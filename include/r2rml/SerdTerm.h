#pragma once

#include "TermMap.h"
#include "rdf/Term.h"

#include <serd/serd.h>

namespace r2rml {

/**
 * The boundary between rdf::Term and Serd.
 *
 * rdf::Term owns its bytes and carries datatype/language inline; a SerdNode
 * owns nothing and carries neither.  Everything needed to cross between the two
 * lives here, in the r2rml layer, because r2rml is the only layer that reads or
 * writes serd.  Keeping it out of include/rdf/ is what lets sql2rdf_rdf stay a
 * stdlib-only target.
 */

/// Iri->SERD_URI, BlankNode->SERD_BLANK, Literal->SERD_LITERAL,
/// Unknown->SERD_NOTHING.  SERD_CURIE is never produced.
SerdType serdTypeOf(rdf::TermKind kind);

/// Inverse.  SERD_CURIE maps to Unknown rather than Iri, deliberately: an
/// unexpanded prefixed name is not a term rdf::Term can represent, and
/// relabelling it an IRI would let it reach a position requiring an absolute
/// one.  Callers with a possible CURIE must serd_env_expand_node() first, as
/// R2RMLParser already does.
rdf::TermKind kindOf(SerdType type);

/// R2RML's rr:termType directive to the kind of term it produces.
///
/// These are separate enums on purpose: rr:termType is a closed three-value
/// declaration (R2RML Section 7.4) with no "unknown" member, whereas
/// rdf::TermKind has one and uses it as the absent term.
rdf::TermKind kindForTermType(TermType term_type);

/**
 * Deep-copy a serd statement's term into an owning rdf::Term.
 *
 * `datatype` and `lang` may be null and are ignored for non-literals.  If both
 * are supplied, `lang` wins - matching RDF 1.1 and rdf::Term's invariant that
 * the two are mutually exclusive.
 *
 * A SERD_BLANK node yields a Term holding serd's BARE label: no "_:" prefix and
 * no merge-scope tagging.  Both of those are TripleCollector's key-space
 * encoding rather than part of the term, and it adds them itself.
 *
 * A null, SERD_NOTHING or SERD_CURIE node yields the absent term.  Note this
 * function does NO env expansion: a relative IRI or CURIE must be resolved by
 * the caller first (see R2RMLParser's expandNode).
 */
rdf::Term termFromSerdNode(const SerdNode *node, const SerdNode *datatype = nullptr, const SerdNode *lang = nullptr);

/**
 * Borrowed SerdNode views into an OWNED copy of a term.
 *
 * serd_writer_write_statement() wants up to three SerdNodes for one RDF term -
 * the term itself plus the datatype and language nodes that serd keeps as
 * separate out-of-band parameters.  Each of those nodes borrows a buffer the
 * caller must keep alive for the duration of the write.
 *
 * This type localises that contract.  It copies the Term into itself and builds
 * the nodes against its own storage, so the pointers stay valid for the whole
 * lifetime of the SerdTermRef regardless of what happens to the source term.
 * Construct one on the stack immediately before the write and let it die at
 * scope exit.
 *
 * Non-copyable AND non-movable: the SerdNodes' `buf` point into this object's
 * own std::strings, and moving a std::string relocates short-string-optimised
 * storage, which would silently leave the nodes dangling.
 */
class SerdTermRef {
public:
	explicit SerdTermRef(const rdf::Term &term);

	SerdTermRef(const SerdTermRef &) = delete;
	SerdTermRef &operator=(const SerdTermRef &) = delete;

	/// The term's own node; null for the absent term.
	const SerdNode *value() const {
		return value_.type == SERD_NOTHING ? nullptr : &value_;
	}

	/// Non-null only for a typed literal - exactly what
	/// serd_writer_write_statement() wants for its `datatype` parameter.
	const SerdNode *datatype() const {
		return datatype_.type == SERD_NOTHING ? nullptr : &datatype_;
	}

	/// Non-null only for a language-tagged literal.
	const SerdNode *lang() const {
		return lang_.type == SERD_NOTHING ? nullptr : &lang_;
	}

	/// True iff there is a term to write at all.
	bool present() const {
		return value_.type != SERD_NOTHING;
	}

private:
	rdf::Term term_;
	SerdNode value_ {SERD_NODE_NULL};
	SerdNode datatype_ {SERD_NODE_NULL};
	SerdNode lang_ {SERD_NODE_NULL};
};

} // namespace r2rml
