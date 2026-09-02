#pragma once

namespace rdf {

/**
 * The kind of an RDF term, shared by every layer of this project.
 *
 * `Unknown` is the bottom element, and it carries two meanings which on
 * inspection are the same meaning:
 *
 *   - In a value context (rdf::Term) it means "there is no term here".  This is
 *     the replacement for Serd's in-band SERD_NODE_NULL / SERD_NOTHING null.
 *   - In an inference context (sparql2sql::TermInfo) it means "nothing can be
 *     proven about this term" - the absorbing element of its meet().
 *
 * It is deliberately NOT a legal value of R2RML's rr:termType, which is always
 * one of rr:IRI, rr:BlankNode or rr:Literal (R2RML Section 7.4).  That is why
 * r2rml::TermType stays a separate, closed three-value enum rather than an
 * alias of this one; r2rml::kindForTermType() converts between them.
 *
 * Serd's SERD_CURIE has no counterpart here, on purpose.  This codebase expands
 * every CURIE to an absolute IRI at parse time (R2RMLParser's expandNode), and
 * no SERD_CURIE node is ever constructed in src/ or tests/.  Mapping one to
 * `Iri` would let an unexpanded prefixed name reach a position that requires an
 * absolute IRI, so the conversion maps it to `Unknown` instead.
 */
enum class TermKind { Unknown, Iri, BlankNode, Literal };

/// "Unknown", "Iri", "BlankNode" or "Literal".  Never null; total over the enum.
const char *termKindName(TermKind kind);

} // namespace rdf
