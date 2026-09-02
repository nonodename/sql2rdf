#include "r2rml/SerdTerm.h"

#include <string>

namespace r2rml {

namespace {

/// Copy a SerdNode's bytes out into a std::string.  Uses n_bytes rather than
/// treating buf as NUL-terminated, since serd reports the length explicitly.
std::string nodeText(const SerdNode *node) {
	if (!node || !node->buf) {
		return std::string();
	}
	return std::string(reinterpret_cast<const char *>(node->buf), node->n_bytes);
}

/// Build a borrowing SerdNode over an already-stable buffer.
///
/// Always via serd_node_from_string, never by filling the struct by hand: serd
/// computes n_chars and the SERD_HAS_QUOTE / SERD_HAS_NEWLINE flags from the
/// bytes (see serd_node_from_string in external/serd/src/node.c), and those
/// flags are what select """long""" quoting in Turtle output.  Hand-filling
/// would be the one way to get a node whose flags disagree with its content.
SerdNode nodeOver(const std::string &text, SerdType type) {
	if (type == SERD_NOTHING) {
		return SERD_NODE_NULL;
	}
	return serd_node_from_string(type, reinterpret_cast<const uint8_t *>(text.c_str()));
}

} // namespace

SerdType serdTypeOf(rdf::TermKind kind) {
	switch (kind) {
	case rdf::TermKind::Iri:
		return SERD_URI;
	case rdf::TermKind::BlankNode:
		return SERD_BLANK;
	case rdf::TermKind::Literal:
		return SERD_LITERAL;
	case rdf::TermKind::Unknown:
		break;
	}
	return SERD_NOTHING;
}

rdf::TermKind kindOf(SerdType type) {
	switch (type) {
	case SERD_URI:
		return rdf::TermKind::Iri;
	case SERD_BLANK:
		return rdf::TermKind::BlankNode;
	case SERD_LITERAL:
		return rdf::TermKind::Literal;
	case SERD_CURIE:
	case SERD_NOTHING:
		break;
	}
	// SERD_CURIE lands here on purpose - see the header.
	return rdf::TermKind::Unknown;
}

rdf::TermKind kindForTermType(TermType term_type) {
	switch (term_type) {
	case TermType::IRI:
		return rdf::TermKind::Iri;
	case TermType::BlankNode:
		return rdf::TermKind::BlankNode;
	case TermType::Literal:
		break;
	}
	return rdf::TermKind::Literal;
}

rdf::Term termFromSerdNode(const SerdNode *node, const SerdNode *datatype, const SerdNode *lang) {
	if (!node || !node->buf) {
		return rdf::Term();
	}
	const rdf::TermKind kind = kindOf(node->type);
	switch (kind) {
	case rdf::TermKind::Iri:
		return rdf::Term::iri(nodeText(node));
	case rdf::TermKind::BlankNode:
		return rdf::Term::blankNode(nodeText(node));
	case rdf::TermKind::Literal:
		break;
	case rdf::TermKind::Unknown:
		return rdf::Term();
	}

	// Language wins over datatype when both are somehow supplied, matching
	// RDF 1.1 and Term's mutually-exclusive invariant.
	if (lang && lang->buf && lang->n_bytes > 0) {
		return rdf::Term::langLiteral(nodeText(node), nodeText(lang));
	}
	if (datatype && datatype->buf && datatype->n_bytes > 0) {
		return rdf::Term::typedLiteral(nodeText(node), nodeText(datatype));
	}
	return rdf::Term::literal(nodeText(node));
}

SerdTermRef::SerdTermRef(const rdf::Term &term) : term_(term) {
	value_ = nodeOver(term_.lexical(), serdTypeOf(term_.kind()));
	if (!term_.isLiteral()) {
		return;
	}
	// Term's invariant guarantees at most one of these is set, so no
	// arbitration is needed here - which is the whole reason the invariant
	// exists.
	if (term_.hasLang()) {
		lang_ = nodeOver(term_.lang(), SERD_LITERAL);
	} else if (!term_.datatypeIri().empty()) {
		datatype_ = nodeOver(term_.datatypeIri(), SERD_URI);
	}
}

} // namespace r2rml
