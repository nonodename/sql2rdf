# SQL2RDF++ API Reference

SQL2RDF++ converts relational databases to RDF using [R2RML](https://www.w3.org/TR/r2rml/) mappings. The core logic lives in the `sql2rdf_r2rml` static library (no DuckDB dependency); a CLI application and DuckDB-backed implementation are built on top of it.

All public classes are in the `r2rml::` namespace. Headers are under `include/r2rml/`.

---

## Quick Start

```cpp
#include "r2rml/R2RMLParser.h"
#include "r2rml/R2RMLMapping.h"
#include <serd/serd.h>

// 1. Parse an R2RML mapping file (Turtle format)
r2rml::R2RMLParser parser;
r2rml::R2RMLMapping mapping = parser.parse("mapping.ttl");

// 2. Validate
if (!mapping.isValid()) {
    std::cerr << "Invalid mapping\n";
    return 1;
}

// 3. Create a Serd writer (e.g. N-Triples output)
FILE* out = fopen("output.nt", "w");
SerdWriter* writer = serd_writer_new(
    SERD_NTRIPLES, 0, mapping.serdEnvironment,
    nullptr, serd_file_sink, out);

// 4. Provide a database connection (see SQLConnection below)
MyDatabaseConnection db("connection_string");

// 5. Generate RDF
mapping.processDatabase(db, *writer);

// 6. Flush and clean up
serd_writer_finish(writer);
serd_writer_free(writer);
fclose(out);
```

---

## Parsing

### `MappingParser`

Abstract base class implemented by every mapping-format parser (`R2RMLParser` for Turtle, `YARRRMLParser` for YARRRML YAML). Both translate an on-disk mapping document into the same `R2RMLMapping` object model.

```cpp
#include "r2rml/MappingParser.h"

class MappingParser {
public:
    virtual ~MappingParser() = default;
    virtual R2RMLMapping parse(const std::string& mappingFilePath, bool ignoreNonFatalErrors = true) = 0;

    static std::unique_ptr<MappingParser> create(const std::string& mappingFilePath);
    static R2RMLMapping parseMultiple(const std::vector<std::string>& mappingFilePaths,
                                       bool ignoreNonFatalErrors = true, bool forceYarrrml = false);
};
```

| Method | Description |
|--------|-------------|
| `parse(path, ignoreNonFatalErrors)` | Parses the mapping document at `path` and returns a populated `R2RMLMapping`. |
| `create(path)` (static) | Instantiates the concrete parser appropriate for `path`'s extension: `.ttl` → `R2RMLParser`, `.yml`/`.yaml`/`.yarrrml` → `YARRRMLParser`. Throws `std::runtime_error` if no known format matches. |
| `parseMultiple(paths, ignoreNonFatalErrors, forceYarrrml)` (static) | Loads and merges several mapping files — any mix of `.ttl` and YARRRML, format auto-detected per file unless `forceYarrrml` forces every file to YARRRML — into a single `R2RMLMapping`. Merging happens before the object model is built, so a `TriplesMap` in one file may reference (e.g. via `rr:parentTriplesMap`) a `TriplesMap` defined in another file in the list. If the same (non-blank) subject is defined in more than one file, the definition from whichever file appears **earliest** in `mappingFilePaths` wins outright; later files' statements about that subject are dropped and a message is added to the returned mapping's `mergeWarnings` — always, regardless of `ignoreNonFatalErrors`. |

`MappingParser::create` and `MappingParser::parseMultiple` are defined in `src/yarrrml/MappingParserFactory.cpp`, part of the `sql2rdf_yarrrml` library, not `sql2rdf_r2rml` — the core library must never depend on yaml-cpp/YARRRML (see layering rules in `CLAUDE.md`). Calling either therefore requires linking `sql2rdf_yarrrml`, even if the resolved format(s) turn out to be R2RML; consumers that only link `sql2rdf_r2rml` and never touch YARRRML should construct `R2RMLParser` directly instead.

`TripleCollector`, the shared triple-gathering helper used internally by both parsers, is also declared in `r2rml/MappingParser.h` (moved out of `r2rml/R2RMLParser.h`). Besides the `setBase`/`setPrefix`/`statement`/`addError` methods used while parsing a single document, it exposes two methods used by `parseMultiple()` to merge several documents into one collector before a single build phase:

| Method | Description |
|--------|-------------|
| `beginSource(sourceLabel)` | Marks the start of a new source document feeding this collector; call once per file, in load order, before feeding its statements. Scopes that file's blank-node labels so they can't collide with another file's (many mapping tools/authors number blank nodes from 0 in every file), and makes named-subject conflicts across sources detectable — once a subject has been written by an earlier source, a later source's statements about the same subject are dropped and a warning is recorded instead. Callers that never call this (single-file `parse()`/`parseString()`) are unaffected. |
| `addWarning(message)` | Records a warning that isn't a parse error — currently only the "competing definition ignored" messages the `beginSource()`-driven conflict detection produces. Unlike `addError()`/`parseErrors`, warnings are always surfaced via `R2RMLMapping::mergeWarnings` regardless of `ignoreNonFatalErrors`, since "first source wins" is a deliberate merge policy, not a parse failure to reject in strict mode. |

### `R2RMLParser`

Parses a Turtle (`.ttl`) R2RML mapping file into the C++ object model. Inherits `MappingParser`.

```cpp
#include "r2rml/R2RMLParser.h"

class R2RMLParser : public MappingParser {
public:
    R2RMLParser();
    R2RMLMapping parse(const std::string& mappingFilePath, bool ignoreNonFatalErrors = true) override;
    void collectFile(const std::string& mappingFilePath, TripleCollector& collector);
};
```

| Method | Description |
|--------|-------------|
| `parse(path)` | Parses the Turtle file at `path` and returns a populated `R2RMLMapping`. Throws on parse error. |
| `collectFile(path, collector)` | Reads `path` as Turtle and feeds its statements into `collector` without building the object model. Used by `MappingParser::parseMultiple()` to merge several mapping files (of possibly different formats) into one collector before a single `parseCollected()` call; `parse()` is equivalent to `collectFile()` on a fresh collector followed by `parseCollected()`. `YARRRMLParser` has the analogous `collectFile(path, collector)`. |

---

## Top-Level Mapping

### `R2RMLMapping`

Represents a complete R2RML mapping document. This is the primary object you interact with after parsing.

```cpp
#include "r2rml/R2RMLMapping.h"

class R2RMLMapping {
public:
    void loadMapping(const std::string& mappingFilePath);
    void processDatabase(SQLConnection& dbConnection, SerdWriter& rdfWriter);

    bool isValid() const;
    bool isValidInsideOut() const;

    std::vector<std::unique_ptr<TriplesMap>> triplesMaps;
    SerdEnv* serdEnvironment;
    std::vector<std::string> parseErrors;
    std::vector<std::string> mergeWarnings;
};
```

`R2RMLMapping` is **move-only** (no copy constructor or copy assignment).

| Method | Description |
|--------|-------------|
| `loadMapping(path)` | Parses a TTL file and populates the object (alternative to using `R2RMLParser` directly). |
| `processDatabase(db, writer)` | Executes all triples maps against `db` and writes RDF triples to `writer`. |
| `isValid()` | Returns `true` if all contained `TriplesMap` objects are valid. |
| `isValidInsideOut()` | Returns `true` if the mapping contains no constructs prohibited in "inside-out" (SQL-export) mode: no `rr:LogicalTable`, `rr:sqlQuery`, `rr:refObjectMap`, or `rr:JoinCondition`. |

| Field | Description |
|-------|-------------|
| `parseErrors` | Non-fatal parse issues (unresolved `parentTriplesMap`, unrecognised logical-table type, etc.), collected when the mapping was parsed with `ignoreNonFatalErrors=true`. Empty when no issues occurred or when the parser ran in strict mode. |
| `mergeWarnings` | Populated by `MappingParser::parseMultiple()`: one entry per named subject defined by more than one of the merged files, naming the conflict and which file's definition was kept. Always populated regardless of `ignoreNonFatalErrors` — "first file wins" is a merge policy, not a parse failure. Empty for a mapping built from a single file. |

---

## Database Backend

### `SQLConnection`

Pure abstract interface for database backends. Implement this to plug in your own database.

```cpp
#include "r2rml/SQLConnection.h"

class SQLConnection {
public:
    virtual ~SQLConnection() = default;
    virtual std::unique_ptr<SQLResultSet> execute(const std::string& sqlQuery) = 0;
    virtual std::string getDefaultCatalog();  // returns "" by default
    virtual std::string getDefaultSchema();   // returns "" by default
};
```

### `SQLResultSet`

Cursor-style interface for iterating query results. Returned by `SQLConnection::execute()`.

```cpp
#include "r2rml/SQLResultSet.h"

class SQLResultSet {
public:
    virtual ~SQLResultSet() = default;
    virtual bool next() = 0;                   // advance; returns false when exhausted
    virtual SQLRow getCurrentRow() const = 0;  // row at current position
};
```

### `DuckDBConnection`

Concrete `SQLConnection` backed by [DuckDB](https://duckdb.org/). Located in `src/DuckDBConnection.h` (not part of the core library header).

```cpp
#include "DuckDBConnection.h"

// On-disk database
DuckDBConnection db("path/to/database.db");

// In-memory database
DuckDBConnection db(":memory:");
```

| Method | Returns |
|--------|---------|
| `execute(sql)` | `unique_ptr<SQLResultSet>` |
| `getDefaultSchema()` | `"main"` |

---

## Row Data

### `SQLRow`

A single row of SQL results, keyed by column name.

```cpp
#include "r2rml/SQLRow.h"

class SQLRow {
public:
    explicit SQLRow(std::map<std::string, SQLValue> columns);
    SQLValue getValue(const std::string& columnName) const;
    bool isNull(const std::string& columnName) const;
    std::vector<std::string> columnNames() const;  // e.g. for printing result headers
};
```

### `SQLValue`

A typed SQL column value.

```cpp
#include "r2rml/SQLValue.h"

class SQLValue {
public:
    enum class Type { Null, Integer, Double, String, Boolean };

    explicit SQLValue(const std::string& s);
    explicit SQLValue(int i);
    explicit SQLValue(double d);
    explicit SQLValue(bool b);

    Type type() const;
    const std::string& asString() const;
    bool isNull() const;
};
```

---

## Data Model

These classes form the in-memory representation of an R2RML mapping. They are populated by the parser and accessed via `R2RMLMapping::triplesMaps`.

### `TriplesMap`

Maps rows from a logical table to a set of RDF triples that share a common subject.

```cpp
#include "r2rml/TriplesMap.h"

class TriplesMap {
public:
    void generateTriples(const SQLRow& row,
                         SerdWriter& rdfWriter,
                         const R2RMLMapping& mapping,
                         SQLConnection& dbConnection) const;
    bool isValid() const;
    bool isValidInsideOut() const;

    std::string id;
    std::unique_ptr<LogicalTable> logicalTable;
    std::unique_ptr<SubjectMap> subjectMap;
    std::vector<std::unique_ptr<PredicateObjectMap>> predicateObjectMaps;
};
```

`isValidInsideOut()` requires `logicalTable == nullptr` and all predicateObjectMaps to pass their own `isValidInsideOut()`.

### `PredicateObjectMap`

Holds the predicate and object maps (and optional graph maps) that produce triples for each input row.

```cpp
#include "r2rml/PredicateObjectMap.h"

class PredicateObjectMap {
public:
    void processRow(const SQLRow& row,
                    const SerdNode& subject,
                    SerdWriter& rdfWriter,
                    const R2RMLMapping& mapping,
                    SQLConnection& dbConnection) const;
    bool isValid() const;
    bool isValidInsideOut() const;  // fails if any objectMap is a ReferencingObjectMap

    std::vector<std::unique_ptr<TermMap>> predicateMaps;
    std::vector<std::unique_ptr<TermMap>> objectMaps;
    std::vector<std::unique_ptr<GraphMap>> graphMaps;
};
```

### Logical Table Classes

| Class | R2RML property | Description |
|-------|---------------|-------------|
| `LogicalTable` (abstract) | — | Base class; provides `getRows()`, `getColumnNames()` |
| `BaseTableOrView` | `rr:tableName` | References a named table or view |
| `R2RMLView` | `rr:sqlQuery` | Backed by an arbitrary SQL query |

```cpp
#include "r2rml/LogicalTable.h"
#include "r2rml/BaseTableOrView.h"
#include "r2rml/R2RMLView.h"

class LogicalTable {
public:
    virtual std::unique_ptr<SQLResultSet> getRows(SQLConnection& db) = 0;
    virtual std::vector<std::string> getColumnNames() = 0;
    virtual bool isValid() const = 0;
    std::string effectiveSqlQuery;
};

class BaseTableOrView : public LogicalTable {
    std::string tableName;
};

class R2RMLView : public LogicalTable {
    std::string sqlQuery;
    std::vector<std::string> sqlVersions;
};
```

### Term Map Classes

All term maps inherit from `TermMap` and implement `generateRDFTerm()`.

```cpp
#include "r2rml/TermMap.h"

enum class TermType { IRI, BlankNode, Literal };

class TermMap {
public:
    virtual SerdNode generateRDFTerm(const SQLRow& row, const SerdEnv& env) const = 0;
    virtual bool isValid() const;

    TermType termType{TermType::IRI};
    std::unique_ptr<std::string> languageTag;      // literals only
    std::unique_ptr<std::string> datatypeIRI;      // literals only
    std::unique_ptr<std::string> inverseExpression;
};
```

| Subclass | R2RML property | Behaviour |
|----------|---------------|-----------|
| `ConstantTermMap` | `rr:constant` | Always returns the same fixed `SerdNode` |
| `ColumnTermMap` | `rr:column` | Reads the value of the named column |
| `TemplateTermMap` | `rr:template` | Expands an RFC 6570 URI template with column values |
| `SubjectMap` | `rr:subjectMap` | Abstract; carries `classIRIs`/`graphMaps` plus `valueTermMap()`, returning the underlying `rr:template`/`rr:column`/`rr:constant` strategy that actually determines the subject's value |
| `PredicateMap` | `rr:predicateMap` | No additional behaviour |
| `ObjectMap` | `rr:objectMap` | No additional behaviour |
| `GraphMap` | `rr:graphMap` | Generates named-graph IRIs. Like `SubjectMap`, exposes `valueTermMap()` returning the underlying `rr:template`/`rr:column`/`rr:constant` strategy, so a consumer can inspect the strategy's *shape* rather than only evaluate it. Unlike `SubjectMap`'s, it is non-pure (defaulting to `nullptr`) because `GraphMap` is directly derivable |
| `ReferencingObjectMap` | `rr:refObjectMap` | Joins to a parent `TriplesMap`; prohibited in inside-out mode |

### Named graphs (RDF generation)

`rr:graph`/`rr:graphMap` may appear on a subject map or a predicate-object map. Per R2RML §12 the
graphs that apply to a generated triple are the **union** of both sets, and `forEachGraphNode()`
(`include/r2rml/GraphMap.h`) is the single place that resolves it — `TriplesMap::generateTriples` and
`PredicateObjectMap::processRow` both write through it, once per applicable graph.

Three rules follow, and each has a counterpart in the SPARQL-to-SQL direction (see "Supported SPARQL
subset" below), because both directions must agree on what the graph set is:

- **A triple lands in *every* graph of its set**, so a two-member set writes the same triple twice,
  as two quads.
- **`rr:defaultGraph` is a member of the set, not a suppressor.** A set of
  `{rr:defaultGraph, ex:g1}` produces both a quad in `ex:g1` *and* a default-graph statement. An
  empty set, or one where every entry resolves to NULL, is the default graph alone — an all-NULL set
  is indistinguishable from an empty one under the spec's set formulation.
- **`rr:class` (`rdf:type`) triples take only the *subject* map's graphs**, since they have no
  predicate-object map to contribute any. A mapping with `rr:graph` on a predicate-object map but
  not on its subject map therefore leaves its `rdf:type` triples in the default graph.

⚠️ Only a **quad** output syntax can represent a named graph. Serialising as N-Triples or Turtle
silently drops the graph component (and so writes the two-member case above as two identical
triples); use `SERD_NQUADS` or `SERD_TRIG` — `-f nquads|trig` from the CLI.

### `ReferencingObjectMap`

```cpp
#include "r2rml/ReferencingObjectMap.h"

class ReferencingObjectMap : public TermMap {
public:
    bool isValid() const override;

    TriplesMap* parentTriplesMap;
    std::vector<JoinCondition> joinConditions;
};
```

### `JoinCondition`

```cpp
#include "r2rml/JoinCondition.h"

class JoinCondition {
public:
    JoinCondition(const std::string& childColumn, const std::string& parentColumn);
    bool isValid() const;  // both columns must be non-empty

    std::string childColumn;
    std::string parentColumn;
};
```

---

## Inside-Out Mode

"Inside-out" mode is intended for SQL-export / macro scenarios where row data is supplied externally rather than fetched from a database. In this mode the following R2RML constructs are **prohibited**:

- `rr:LogicalTable` (and by extension `rr:tableName` / `rr:sqlQuery`)
- `rr:refObjectMap` and `rr:JoinCondition`

Validate a mapping for this mode before use:

```cpp
if (!mapping.isValidInsideOut()) {
    std::cerr << "Mapping is not valid for inside-out execution\n";
    return 1;
}
```

The check cascades: `R2RMLMapping::isValidInsideOut()` → `TriplesMap::isValidInsideOut()` → `PredicateObjectMap::isValidInsideOut()`.

---

## SPARQL-to-SQL Translation

`sql2rdf_sparql2sql` (namespace `sparql2sql::`) translates an already-parsed SPARQL query
(`sparql::ast::Query`, from `sql2rdf_sparql`) against an already-parsed R2RML mapping
(`r2rml::R2RMLMapping`, from `sql2rdf_r2rml`) into a SQL string for a given `SqlDialect`. It uses
the mapping's `TriplesMap`/`PredicateObjectMap`/`TermMap` structure *in reverse*: for each SPARQL
triple pattern it enumerates every mapping source that could produce a matching triple, generates
one SQL relation per candidate, and composes per-pattern relations via the SPARQL algebra
(AND→inner join, OPTIONAL→left outer join, UNION→schema-extending union, MINUS→anti-join,
FILTER/BIND→applied against everything bound so far).

### Quick Start

```cpp
#include "sparql-parser/Parser.h"
#include "r2rml/R2RMLParser.h"
#include "sparql2sql/Translator.h"
#include "sparql2sql/DuckDbDialect.h"

sparql::Parser sparqlParser;
std::unique_ptr<sparql::ast::Query> query = sparqlParser.parseFile("query.rq");

r2rml::R2RMLParser mappingParser;
r2rml::R2RMLMapping mapping = mappingParser.parse("mapping.ttl");

sparql2sql::DuckDbDialect dialect;
std::string sql = sparql2sql::translateQuery(*query, mapping, dialect);
// sql is a single "SELECT ..." (or, for ASK, "SELECT EXISTS(...) AS ask") statement,
// ready to hand to r2rml::DuckDBConnection::execute() or any other SQLConnection.
```

### API

```cpp
#include "sparql2sql/Translator.h"

std::string sparql2sql::translateQuery(const sparql::ast::Query& query,
                                        const r2rml::R2RMLMapping& mapping,
                                        const sparql2sql::SqlDialect& dialect,
                                        const sparql2sql::TypeCatalog* catalog = nullptr);

// Close a rendered relation over the WITH-clause entries its TranslationContext
// accumulated: the hoisted rr:sqlQuery views, then any property-path closure CTEs.
std::string sparql2sql::prependCtes(const sparql2sql::TranslationContext& ctx,
                                     const std::string& body);
```

`translateQuery()` applies `prependCtes()` to its own result, so ordinary callers never need it. It
matters for the other direction: code driving `translateTriplePattern()`/`renderRelation()` itself
gets a relation that *references* CTEs only this function emits, so it must call `prependCtes()` to
obtain a runnable statement.

The translator builds a small relational-algebra IR (`sparql2sql/ir/RelNode.h`) rather than
emitting SQL strings directly, then applies a fixed pipeline of semantics-preserving rewrites
(`sparql2sql/ir/Optimizer.h`) before rendering once, in this order:

1. **Empty-relation propagation.** A triple pattern no triples map can satisfy (an unmapped
   predicate, a bound position no term map can produce) translates to an empty relation; this pass
   folds away whatever that makes unsatisfiable — an inner join with an empty side, a union's empty
   arms (unwrapping the union entirely when one arm survives), a `MINUS` with nothing to subtract, a
   `FILTER`/`BIND` over nothing, an `E+` closure with no one-hop edge. Needs no catalog: the
   emptiness is a structural fact the mapping already proved, not a claim about the data. A LEFT
   OUTER JOIN with an empty *right* side is deliberately **not** reduced — sound, but it needs NULL
   columns synthesized to preserve the schema.
2. **Key-proven DISTINCT elimination.** A single-source candidate arm's `SELECT DISTINCT` is dropped
   when the `TypeCatalog`'s declared PRIMARY KEY / UNIQUE constraints prove its projected columns
   already determine the row — so no two source rows can produce the same output tuple. Unlike pass
   7 below this does **not** require the enclosing query to dedup, which is what makes it worth
   having: a plain `SELECT` otherwise pays a per-arm dedup on every pattern. Requires injectivity of
   each projected expression, so it declines on a non-invertible `rr:template` (adjacent
   placeholders can render two placeholder tuples as one string) and on any computed or `COALESCE`d
   column. The engine cannot make this deduction — it does not know the constructed IRI is an
   injective function of the key columns.
3. **FILTER pushdown.** Each FILTER is split into its top-level conjuncts and each is routed to the
   smallest relation supplying its variables (inner-join sides, every arm of a UNION, an anti-join's
   left side; LEFT OUTER JOIN, BIND and EXISTS-bearing conjuncts are boundaries). A conjunct that
   reaches an SPJ block is **folded into that block's `WHERE` list** rather than wrapping it in
   another subquery — which is the point of the pass: it keeps the block mergeable by the next
   step. (Merely re-parenting a filter lower buys nothing against an engine that already pushes
   predicates through derived tables; keeping blocks mergeable does.)
4. **Inner-join SPJ flattening.** An N-triple BGP becomes one flat `SELECT ... FROM a, b, c WHERE
   ...` instead of N−1 nested subqueries.
5. **Self-join elimination.** Two patterns over the same table bound to the same subject variable
   collapse to a single scan — the subject map is assumed row-unique.
6. **Redundant-predicate removal.** Two provably vacuous WHERE conjuncts, each arising because the
   two conjuncts are generated independently rather than as a single decision:
   - an `<x> IS NOT NULL` guard is dropped once `<x> = '<literal>'` (optionally
     `CAST(... AS VARCHAR)`-wrapped) is already a conjunct of the same block — the equality itself
     forces `<x>` non-NULL for the row to pass the `WHERE`;
   - a comparison against a column an `rr:sqlQuery` view's own `SELECT` list defines as a fixed
     literal (e.g. `SELECT true AS FLAG FROM ...`) is dropped when it matches that literal, since
     such a column carries no per-row information to test. Detected by a lightweight textual scan of
     the view's `SELECT` list (`sparql2sql::detectConstantSelectColumns`, `LogicalTableSource.h`) —
     recognizing a single-quoted string, `TRUE`/`FALSE`, or a bare integer aliased with `AS`; anything
     else (a real column, a function call) is left alone. This half of the pass runs only when a
     `TranslationContext` is supplied (it needs a `SqlDialect` to render the expected column
     reference).
7. **DISTINCT elimination.** The per-pattern `SELECT DISTINCT` is dropped when the enclosing query
   is `SELECT DISTINCT`/`ASK`, and likewise inside an EXISTS body (an existence check cannot see
   duplicates).

**Each distinct `rr:sqlQuery` view is hoisted into one shared `WITH` CTE** and referenced by name,
rather than inlined as a derived table at every use site. One view commonly backs many sites — each
predicate-object map of its triples map, both sides of a referencing object map, and every arm of a
variable-predicate expansion — so inlining duplicated a substantial body ten or more times and left
the engine no syntactic signal that the occurrences were one relation. Deduplication is keyed on
`logicalTableIdentity()`, i.e. the exact query text, so two triples maps declaring the same query
also share one CTE. Base tables are not hoisted (already a bare name), and an entry nothing
references — a view whose only candidate arm was later pruned — is dropped rather than emitted.

No `AS MATERIALIZED` hint is emitted, and there is no `SqlDialect` seam for one: whether to
materialize is left to the engine, which only does so where it can prove evaluation is unaffected.

An earlier attempt at this was reverted (see commit `40a8f79`): on a 1.6M-row triple table, replacing
three inlined bodies with three CTEs took one benchmark query from 0.2s to unbounded, because the
arms filter a shared view on *different* predicate IRIs and only the inlined copies let the engine
push each arm's predicate down into its own base-table scan. Current DuckDB no longer regresses
there — the reference workload was re-benchmarked before this landed, and the motivating
variable-predicate query is unchanged in wall time (3.19s → 3.07s) while returning identical rows.
That history is the reason the guidance stands: judge any change here on **measured execution time**,
not on SQL size, and re-measure on a real workload rather than assuming pushdown survives.

Everything below the level of these structural simplifications — join ordering, physical join
choice, index selection — is deliberately left to the target engine's own optimizer, which does it
better on the flat, native-typed plan it is handed. The rewrites above are confined to what an
engine provably *cannot* recover on its own: R2RML term construction is not invertible by the
engine, and neither is the mapping's row-uniqueness assumption.

Join and correlation conditions are emitted as **plain equalities wherever that is sound**. A
null-tolerant comparison (`l = r OR l IS NULL OR r IS NULL`) is only required when a side can
actually be unbound, so each `IS NULL` disjunct is emitted only for a side the IR marks nullable.
This matters well beyond the redundant text: an OR'd comparison is a non-equi predicate, which stops
the engine from decorrelating a `FILTER EXISTS` / `MINUS` into a hash semi-join and leaves it a
nested loop over the inner relation.

```cpp
#include "sparql2sql/TypeCatalog.h"

struct sparql2sql::TypeCatalog {
    std::map<std::string, std::map<std::string, std::string>> columnTypes; // table -> column -> SQL type
    // ...

    // True iff the column's declared type is already a VARCHAR-family string type (VARCHAR/CHAR/
    // TEXT/STRING/BPCHAR, any width), so a CAST(... AS VARCHAR) around it is a provable no-op.
    bool isStringType(const std::string& table, const std::string& column) const;
};
```

`catalog` is optional. Every plain `rr:column` reference is otherwise rendered
`CAST(t."COL" AS VARCHAR)` — always correct, since the translator represents every SPARQL variable
as a VARCHAR lexical form. When the catalog says the column is already string-typed, that cast is a
no-op and is omitted (`t."COL"`), both when the column is projected and in the equality generated by
inverting a bound term against the term map — dropping one CAST call per comparison at no cost to
correctness.

When supplied, an equi-join whose two sides are of **comparable declared type** (per the catalog) is
also emitted on the native, uncast columns instead of on generated term text — index-friendly and
materially faster on large tables. Two shapes qualify:

- both sides are a **pure base column** (`rr:column` term maps): `t1."capIQCompanyID" = t2."parent"`
  instead of `CAST(... AS VARCHAR) = CAST(... AS VARCHAR)`;
- both sides are the **same invertible `rr:template`**: `t1."DEPTNO" = t2."DEPTNO"` instead of
  `('http://…/{DEPTNO}' || …) = ('http://…/{DEPTNO}' || …)`. Equal placeholder values always
  produce equal template text, and invertibility (no two placeholders textually adjacent, so the
  text splits unambiguously) gives the converse. This is the case that dominates real mappings,
  since R2RML subjects are nearly always templates. Note a template string embeds its placeholders'
  *column names*, so two maps share a template only if they also share those column names.

Across a derived-table boundary (an OPTIONAL's join, a MINUS's anti-join) the base columns are not
in scope, so the rewrite additionally projects them as hidden `k_N` columns on both sides; this is
attempted only when both children render their own SELECT list (a direct SPJ block).

The catalog is keyed by **logical-table identity** — a base table's declared `rr:tableName`, or
`"view:" + <the rr:sqlQuery text>` for an R2RML view — which is what
`sparql2sql::logicalTableIdentity()` returns for the source. A view's columns appear in no
`information_schema`, so a populator that only sweeps one leaves them untyped; see
`LogicalTableSource.h` below for how to type them, and note that whether a *join key* is eligible
also depends on the two sides being pure columns or the same invertible template, which a view's
columns can be. When `catalog` is null, or a column's type is unknown, or the types aren't
comparable, the join falls back to the always-correct VARCHAR-cast comparison. The catalog is a
plain, dependency-free data structure: the core library never opens a database; the CLI populates it
from `information_schema.columns` plus a `DESCRIBE` per view. Because all downstream variables
remain VARCHAR, only the join *condition* changes — projected values and result semantics are
identical either way.

```cpp
#include "sparql2sql/LogicalTableSource.h"

// The catalog key / self-join identity of a logical table: a base table's declared name, or
// "view:" + the raw rr:sqlQuery text.
std::string sparql2sql::logicalTableIdentity(const r2rml::LogicalTable& logicalTable);

// Strip trailing whitespace and at most one trailing ';', yielding text safe to embed as a derived
// table or hand to a backend's schema-description statement.
std::string sparql2sql::stripTrailingSemicolon(std::string sql);

struct sparql2sql::ViewSource {
    std::string identity; // the TypeCatalog::columnTypes key for this view
    std::string sql;      // the view's SQL, semicolon-stripped and ready to describe
};

// Every distinct rr:sqlQuery logical table the mapping reads from, de-duplicated by identity.
std::vector<sparql2sql::ViewSource> sparql2sql::mappingViewSources(const r2rml::R2RMLMapping& mapping);

// Unescape a single-quoted SQL string literal, e.g. "'ab''c'" -> "ab'c".
std::string sparql2sql::unquoteSqlStringLiteral(const std::string& quoted);

// Column -> the VARCHAR-cast rendering every row would produce for that column, for every column in
// sql's top-level SELECT list that is a bare literal (a quoted string, TRUE/FALSE, or a bare integer)
// rather than a real per-row column reference. A column this textual scan can't confidently classify
// is simply absent from the result. Feeds the optimizer's redundant-predicate pass (see above).
std::map<std::string, std::string> sparql2sql::detectConstantSelectColumns(const std::string& sql);
```

This is the hook for typing an `rr:sqlQuery` view's columns, which matters well beyond join keys:
without it, R2RML §10.2's natural mapping cannot supply a datatype for a bare `rr:column` literal
over a view, and since a predicate's candidate term maps are combined by a **meet**, one untyped
view arm makes `datatype()` refuse an answer for a predicate whose other arms *are* typed (the
common case of a mapping where some `skos:prefLabel`s come from tables and some from queries). A
caller holding a connection walks `mappingViewSources()`, asks the backend for each query's result
schema — in DuckDB `DESCRIBE SELECT * FROM (<sql>) AS v`, which binds the query without executing
it, so it costs a plan and reads no rows — and files the answers under `identity`. The core library
stays connection-free: it only says which queries to describe. `sql2rdf::loadTypeCatalog()`
(`include/sql2rdf/TypeCatalogLoader.h`, backed by the `sql2rdf_type_catalog_loader` target) does
both halves: the `information_schema` sweep for base tables and a `DESCRIBE` per view. It has no
DuckDB dependency of its own — it's written against the abstract `r2rml::SQLConnection` — so it's
exposed unconditionally for any downstream consumer with its own `SQLConnection` backend, not
just the DuckDB-backed CLI.

```cpp
#include "sparql2sql/DialectFactory.h"

std::unique_ptr<sparql2sql::SqlDialect> sparql2sql::createDialect(const std::string& name);
// "duckdb" -> DuckDbDialect; anything else throws std::runtime_error naming the supported set.
```

```cpp
#include "sparql2sql/TranslationError.h"

class TranslationError : public std::runtime_error {
public:
    explicit TranslationError(const std::string& message);
};
// Thrown for any SPARQL construct/expression the translator cannot express as SQL against the
// supplied mapping (see "Known limitations" below). Derives from std::runtime_error, like
// sparql::ParseError, so existing `catch (const std::exception&)` sites keep working.
```

The static RDF term dimension the translator derives from the mapping (see "Known limitations"
below for what it enables) is described by these two headers. Most consumers never touch them
directly — they matter when adding a translator feature that needs to know what kind of term a
column holds.

```cpp
#include "sparql2sql/TermInfo.h"

enum class sparql2sql::RdfTermKind { Unknown, Iri, BlankNode, Literal };

struct sparql2sql::TermInfo {
    RdfTermKind kind = RdfTermKind::Unknown;
    std::string datatypeIri;   // empty == NOT STATICALLY KNOWN, never "absent by default"
    std::string lang;          // ditto; a language-tagged literal also sets datatypeIri to
                               // rdf:langString
    bool kindKnown() const;  bool isKnownLiteral() const;
    bool isNumeric() const;  bool isIntegral() const;
    bool isTemporal() const; bool isStringy() const;
};

// Greatest lower bound: identical infos meet to themselves, disagreement degrades field by field
// and never guesses, and an Unknown kind clears the datatype and language too. Commutative,
// associative, idempotent, with a default-constructed TermInfo as the absorbing element.
sparql2sql::TermInfo sparql2sql::meet(const TermInfo& a, const TermInfo& b);

// Canonical XSD/RDF datatype IRIs (sparql2sql::xsd::kInteger etc., sparql2sql::kRdfLangString)
// plus isXsdNumericIri / isXsdIntegralIri / isXsdTemporalIri / isXsdCastIri.
```

`TermInfo` describes the term whose lexical form the generated SQL **actually produces**, not the
term SPARQL's abstract semantics would produce; where the two differ, SQL truth wins. It appears as
`ColumnInfo::term` on every IR column and as `TranslatedPattern::termInfo` (read it via
`termInfoOf()`, which returns a default `Unknown` for an absent variable).

```cpp
#include "sparql2sql/TermInference.h"

// The TermInfo of the value expr's translated SQL will produce. Pure, and NEVER throws: anything
// it cannot analyse (including an out-of-scope variable) is Unknown. translateExpression remains
// the sole place that rejects an expression.
sparql2sql::TermInfo sparql2sql::inferExprTermInfo(const sparql::ast::Expression& expr,
                                                   const TranslatedPattern& scope);
```

```cpp
#include "sparql2sql/TypeCatalog.h"

// R2RML Section 10.2's natural mapping from a SQL type name to an XSD datatype IRI. Case- and
// precision-insensitive ("DECIMAL(18,2)" == "decimal"); returns "" for an unmapped or unrecognised
// type, meaning "no datatype can be inferred" - never a guess.
std::string sparql2sql::naturalXsdDatatype(const std::string& sqlTypeName);

// Alongside `columnTypes`, the catalog carries the DDL constraint facts. Both are keyed on the same
// logicalTableIdentity() as columnTypes, and both are looked up case-insensitively.
std::map<std::string, std::set<std::string>> notNullColumns;
std::map<std::string, std::vector<std::set<std::string>>> uniqueKeys;

// True iff the DDL declares the column NOT NULL, so R2RML's "null column => drop this term" rule
// can never fire and the "IS NOT NULL" guard is unconditionally true.
bool sparql2sql::TypeCatalog::isNotNull(const std::string& table, const std::string& column) const;

// True iff some declared PRIMARY KEY / UNIQUE constraint of `table` has every one of its columns
// present in `columns` - i.e. `columns` determines the row. Conservative: an unknown table, an
// empty constraint, or no contained key all return false.
bool sparql2sql::TypeCatalog::coversUniqueKey(const std::string& table,
                                              const std::set<std::string>& columns) const;
```

Everything in the catalog is a **schema** fact, never a data statistic — no row counts, no
cardinality estimates. That is deliberate: a rewrite proved against DDL stays valid as rows come and
go, whereas a cardinality snapshot baked into generated SQL goes stale. Cardinality is left to the
target engine, which has better numbers than an `information_schema` sweep could get (see the
closing paragraph of the optimizer pass list above).

Only base tables ever carry constraint facts. An `rr:sqlQuery` view has no constraint metadata in
any backend, so `isNotNull`/`coversUniqueKey` always decline on one and every dependent rewrite
declines with them.

The CLI exposes this via `-T <file.rq> [--dialect <name>]` (default dialect: `duckdb`), paired
with the mapping-file positional argument. If the database-file positional is also given, the CLI
reads that database's column types into a `TypeCatalog` (base tables from `information_schema`, plus
a `DESCRIBE` of each `rr:sqlQuery` view, so native-typed join keys and §10.2 datatype inference are
both enabled, plus the base tables' NOT NULL and PRIMARY KEY / UNIQUE constraints, which enable
guard suppression and key-proven DISTINCT elimination),
translates against it, then executes the SQL via `r2rml::DuckDBConnection` and prints the result
rows; see `-h` for exact stdout/stderr routing. Without a database file, the SQL alone is printed
and joins use the VARCHAR-cast fallback.

### Supported SPARQL subset / Known limitations

- **Query forms**: only `SELECT` and `ASK`. `CONSTRUCT`/`DESCRIBE` throw `TranslationError`
  (they produce an RDF graph, not a row set — a different translation shape, not yet implemented).
- **Property paths**: `^` (inverse), `/` (sequence), `|` (alternative), `?` (zero-or-one),
  `+` (one-or-more), `*` (zero-or-more), and negated property sets (`!(:a|^:b)`) are all supported,
  alongside a constant IRI/`a` or a bare variable. `^`/`/`/`|`/`?`/negated-property-sets desugar into
  the existing (non-recursive) relational algebra exactly as SPARQL 1.1 §18.1.7 defines them: `^E`
  swaps the pattern's endpoints, `E1/E2` joins over a fresh intermediate variable, `E1|E2` unions,
  `E?` unions the one-step path with the zero-length path, and a negated property set becomes a
  "predicate not in {…}" condition on each candidate (plus a reversed arm for its `^`-marked
  members). Three consequences worth knowing:
  - A sequence path's intermediate variable is **internal**: it takes part in joins but is never
    projected, including by `SELECT *`. (Blank-node positions are internal in the same way, and are
    likewise not projected by `SELECT *` — they are not query variables.)
  - `E?` with **both endpoints unbound** (`?x :p? ?y`) has nothing to anchor its zero-length half
    to, so that half enumerates every term the mapping can produce as a subject or object — a UNION
    scan over every logical table in the mapping. This is spec-correct but not cheap; binding either
    endpoint reduces it to a single constant row. The enumeration over-approximates only in that a
    `TriplesMap` whose subject appears solely as the parent of a referencing object map contributes
    its subjects via its own arm; it deliberately excludes predicate IRIs, per SPARQL's `nodes(G)`.
    Inside a `GRAPH` block the enumeration is instead expressed as a projection of the ordinary
    candidate enumerator over a wildcard predicate, so it is restricted to that graph (and picks up
    dataset filtering) rather than spanning the whole mapping.
  - `E+`/`E*` are the one case that genuinely needs recursive SQL rather than a fixed algebra
    expression: `E+` translates to a `WITH RECURSIVE` closure of `E`'s one-hop relation, and `E*` is
    that closure unioned with the zero-length path (reusing the `E?` machinery above). Whichever
    endpoint is bound seeds the recursion directionally, reducing it to a cheap unary
    "reachable-node" CTE (or, with both endpoints bound, a single `EXISTS` membership check); only
    **both endpoints unbound** (`?x :p+ ?y`) forces the expensive general case, a full `(subject,
    object)` pairs closure with no size/depth guard — this can be costly on a densely-connected
    graph, the same cost tradeoff `E?`'s both-unbound case already has above.
- **Named graphs / `GRAPH`**: supported. `rr:graph`/`rr:graphMap` on a subject map or a
  predicate-object map are read *in reverse*, exactly as the term maps for subject/predicate/object
  are: the graph becomes a fourth term position, so `GRAPH <iri>` prunes candidates whose graph map
  provably cannot produce that IRI, and `GRAPH ?g` binds `?g` as an ordinary query variable
  (always an IRI). Per R2RML §12 a candidate's graphs are the union of its subject map's and its
  predicate-object map's, and one triple genuinely lands in *every* graph of that set — so a
  two-member set yields two solutions under `GRAPH ?g`, one per graph.
  - **Strict RDF-dataset semantics.** With no `FROM` clause the default graph contains only
    triples whose applicable graph set is empty or names `rr:defaultGraph`; named-graph triples are
    invisible outside a `GRAPH` block. ⚠️ **This is a behaviour change** for any mapping that uses
    `rr:graph`/`rr:graphMap`: a query with no `GRAPH` clause no longer returns those triples.
    Mappings that never mention `rr:graph` are entirely unaffected and generate byte-identical SQL.
  - `rr:defaultGraph` is a *member* of the graph set, not a suppressor: a set of
    `{rr:defaultGraph, ex:g1}` makes the triple reachable both from the default graph and from
    `ex:g1`.
  - **`rr:class` triples take only the *subject* map's graphs** (matching
    `TriplesMap::generateTriples`), so a mapping with `rr:graph` on a predicate-object map but not
    on its subject map leaves its `rdf:type` triples in the **default** graph while the
    predicate-object map's triples are named. Spec-correct, and easy to be surprised by.
  - A referencing object map uses the **child** triples map's subject graphs; the parent's do not
    apply.
  - **Property paths inside `GRAPH ?g`** work, including the arbitrary-length operators. The graph
    name is held constant across every hop of the recursive closure, so a match cannot start in one
    graph and finish in another; with both endpoints bound, `GRAPH ?g { <a> :p+ <b> }` binds `?g` to
    each graph that connects them rather than reducing to an existence check. A zero-length match
    (`?`/`*`) holds in every graph per §18.4, so with one endpoint bound `?g` ranges over the
    dataset's named graphs. The paths inside `GRAPH <iri>` are unaffected and keep exactly the SQL
    shape they had before, since a bound IRI's condition lives inside the step relation.
    With both endpoints unbound, the zero-length half needs `nodes(graph)` — every term appearing as
    a subject or object of a triple *in that graph* — which is enumerated per graph, so a term of one
    named graph never appears as a reflexive answer under another. There is no `GRAPH` shape the
    translator refuses.
- **`FROM` / `FROM NAMED` (dataset clauses)**: supported, per SPARQL 1.1 §13.2. With no clauses the
  dataset is the whole mapping (the pre-existing behaviour). Otherwise:

  | Clauses present | Default graph (no `GRAPH` block) | What a `GRAPH` block can match |
  |---|---|---|
  | none | graph set empty / `rr:defaultGraph` | any graph the mapping produces |
  | `FROM <g>`… only | the merge of the listed `g`s | **nothing** — no graphs are nameable |
  | `FROM NAMED <g>`… only | **empty** | only the listed `g`s |
  | both | merge of the `FROM` `g`s | only the `FROM NAMED` `g`s |

  Two consequences are worth stating outright, because each makes a query legitimately return zero
  rows in a way that reads like a bug:
  - **`FROM` *replaces* the default graph, it does not add to it.** With `FROM <g>`, a triple that
    has no graph map at all becomes **invisible**.
  - **Naming only `FROM NAMED` graphs leaves the default graph empty**, so any pattern outside a
    `GRAPH` block matches nothing.

  A `FROM`/`FROM NAMED` IRI the mapping cannot produce is not an error — it simply contributes
  nothing, consistent with a triple pattern that provably matches nothing. Listing the same graph
  twice is one graph. The dataset is fixed once for the whole query (the grammar only allows dataset
  clauses on the top-level query), so nested sub-selects and `EXISTS` bodies inherit it.
- **No `SERVICE`** (federated query): always throws, matching `sql2rdf_sparql`'s own "no
  federated-query execution semantics" stance.
- **Every SPARQL variable is a plain SQL `VARCHAR`** holding the RDF term's lexical string form
  (IRI string / literal lexical form / blank node label). That representation is unchanged, and it
  is still all most queries carry. Alongside it the translator tracks the term's **dimension** — is
  it an IRI, a blank node or a literal, and for a literal what datatype IRI and language tag —
  derived from `rr:termType`, `rr:datatype` and `rr:language` on the contributing term maps, or,
  when no `rr:datatype` is declared and a `TypeCatalog` is supplied, from R2RML §10.2's natural
  mapping of the underlying SQL column type.

  The dimension is resolved **statically wherever the mapping determines it**, and folded into a SQL
  constant — so a query over a well-typed mapping generates exactly the SQL it generated before any
  of this machinery existed. Where the mapping does *not* determine it — the several candidate term
  maps of one triple pattern disagree, a `UNION` mixes arms, an `OPTIONAL`'s `COALESCE`d column
  could come from either side — the dimension is instead carried **into the generated SQL** as a
  companion `VARCHAR` column and evaluated per row.

  That companion column is named `d_<var>` beside the value's `v_<var>`, and holds a single
  discriminated tag:

  | Tag | Meaning |
  |---|---|
  | `I` | an IRI |
  | `B` | a blank node |
  | `L` | a literal whose datatype the mapping cannot determine |
  | `D<iri>` | a literal with datatype `<iri>` |
  | `@<tag>` | a language-tagged literal (datatype is implicitly `rdf:langString`) |
  | `NULL` | unbound |

  Two properties are load-bearing. First, the tag column is SQL `NULL` **exactly** when its value
  column is, which is what lets an `OPTIONAL` join's paired `COALESCE` take value and tag from the
  same side and lets a schema-extending union's `NULL` padding agree between the two. Second, `L` is
  a *definite* fact ("the mapping does not pin this datatype down"), not an error state, and every
  consumer treats it as "degrade to the untyped behaviour" — so `FILTER(?price > 10)` over a bare
  `rr:column` with no `rr:datatype` keeps working exactly as it always has.

  Tags are **materialised only on demand**: a variable gets one only when some construct inspects
  its dimension *and* the static answer is insufficient. Demand is computed in two passes — join
  keys before optimization (a merged inner join fixes its key comparison text there), expressions
  after it, so filter pushdown gets first refusal. That second ordering matters in practice: pushdown
  re-renders a predicate against one union arm at a time, where the dimension is often statically
  known even though the arms' meet is not, so `FILTER(isIRI(?x))` over disagreeing arms still folds
  per arm and materialises nothing at all. Consequences:
  - `isIRI()`/`isURI()`/`isBLANK()`/`isLITERAL()`/`lang()`/`datatype()` fold to a SQL constant when
    the mapping determines the dimension — NULL-guarded for a variable that may be unbound, so an
    unbound term yields SQL `NULL`, which a `FILTER` drops and a `BIND` leaves unbound (matching
    SPARQL's treat-an-error-as-unbound). Otherwise they test the runtime tag. They no longer throw
    for a dimension the mapping cannot decide.
  - `datatype()` over a literal whose datatype is genuinely unknown (a bare `rr:column` with no
    `rr:datatype` and no `TypeCatalog`, or a literal from an `rr:sqlQuery` view the catalog's
    populator did not describe) has no IRI to name, and yields SQL `NULL` — a type error, which a
    `FILTER` drops and a `BIND` leaves unbound. Defaulting to `xsd:string` would contradict R2RML's
    own natural mapping. Note this is per *row*: a predicate with one typed arm and one untyped arm
    reports the datatype for the typed arm's rows and an error for the other's, rather than refusing
    the whole query.
  - `lang()` over a literal returns `''` when there is genuinely no language tag, and the row's own
    tag when there is. A predicate that is `rr:language "en"` in one arm and untagged in another
    reports `en` and `''` respectively — the case that used to be indistinguishable from "no
    `rr:language` anywhere" and had to be refused.
  - `STRDT()`/`STRLANG()` are lexical pass-throughs that *construct* a dimension, so the datatype IRI
    or language tag may now be a **per-row value** rather than only a constant. `STRDT()` does not
    validate the lexical form against the datatype (SPARQL permits an ill-formed literal), and a
    non-literal first argument is a type error that is not detected.
  - `IRI()`/`URI()` are supported: the lexical form of an IRI term *is* its IRI string, so they are
    pass-throughs whose whole effect is the tag. No relative-IRI resolution against a base is
    performed. `BNODE()` is supported and mints a label per solution (deterministically from its
    argument when given one, so equal arguments give the same blank node, as the spec requires).
  - `sameTerm()` is RDF term identity: same lexical form **and** same dimension. Two terms of
    statically different kinds (or different declared datatypes/languages) compare `FALSE` without
    touching data; where the mapping proves the dimensions agree it stays plain string equality with
    no change to the SQL; otherwise the tags are compared too. This fixes a real false positive —
    string equality alone matches an IRI against a literal whose text happens to equal it, and
    `"9"^^xsd:integer` against `"9"^^xsd:string`.
  - Comparisons drop their string-fallback `CASE WHEN` when both operands' datatypes are statically
    known and compatible: both numeric compare as `DOUBLE`, both `xsd:date`/`xsd:dateTime` as
    `DATE`/`TIMESTAMP` (which also fixes lexicographic comparison of non-canonical or offset-bearing
    dateTimes), both `xsd:string` as bare `VARCHAR`. `=`/`<>` between two statically *different term
    kinds* folds to a constant, since no IRI is ever equal to a literal.

    When the dimensions are not statically known but both operands carry a tag, the comparison
    dispatches on both operands' **value spaces** per row (numeric / `xsd:string` / `xsd:boolean` /
    `date`-`dateTime` / `rdf:langString` / IRI / blank node). Within one space the comparison is by
    value, so `"1"^^xsd:integer = "1.0"^^xsd:decimal` is true. Across two known spaces, `=`/`<>` is a
    definite `FALSE`/`TRUE` by RDF term inequality, while `<`/`>`/`<=`/`>=` is a **type error**
    (SQL `NULL`, so the row is dropped) — §17.3's operator table defines the ordering comparisons
    only within the numeric, `xsd:string`, `xsd:boolean` and `date`/`dateTime` spaces, so two IRIs or
    two language-tagged literals are an error for those operators too. If either operand's datatype
    is undeterminable (`L`), the whole thing degrades to the untyped
    `TRY_CAST(... AS DOUBLE)` + `CASE WHEN` fallback exactly as before, so `FILTER(?name < "M")`
    still works. Every typed branch keeps `TRY_CAST`, so a value contradicting its declared datatype
    yields `NULL` (row dropped) rather than a runtime error.
  - An equi-**join** on a shared variable is RDF term equality, so where the two sides' dimensions
    are statically known *and different*, the join additionally compares them — two rows with the
    same lexical form but different datatypes do not join. Where they are statically equal the
    conjunct is trivially true and is not emitted, so an ordinary single-mapping join is byte-for-byte
    unchanged. Where either side's dimension is undeterminable there is nothing to compare and the
    join stays lexical-only.
  - Arithmetic over two statically integral operands (`xsd:integer` and its narrower aliases) stays
    integral via `TRY_CAST(... AS BIGINT)`, so `?a + 1` renders `"10"` rather than `"10.0"`. The same
    applies to `SUM()`, and to `ABS()`/`CEIL()`/`FLOOR()`/`ROUND()`. Division, and any operand of
    unknown or non-integral type, still goes through `DOUBLE`; integrality known only at run time is
    not exploited, so such an expression renders as a double.
  - `ORDER BY` sorts **in the key's static type** when it is known numeric or temporal, and
    lexicographically otherwise. When the key's dimension varies per row the single key expands into
    SPARQL §15.1's own layered order: term kind first (unbound < blank node < IRI < literal), then
    value space so comparable literals sort together, then the value read as each orderable type,
    then the raw lexical form. Three caveats: a value failing the `TRY_CAST` sorts as `NULL` (last,
    for `ASC`) rather than raising; a typed `MIN()`/`MAX()` returns the SQL engine's canonical
    rendering of the value, so `MIN()` over `xsd:integer` values `"007"` and `"42"` returns `"7"`, not
    `"007"`; and an `ORDER BY` key that exists only as a SELECT-list alias (e.g. `ORDER BY ?cnt` over
    `(COUNT(?x) AS ?cnt)`) is typed from the expression that defines it, which reaches only the
    single-key path.
  - A bare `GROUP BY ?x` partitions by RDF *term*, so it includes `?x`'s tag as a second grouping key
    when the dimension is not statically determined — grouping on the lexical form alone would merge
    `"9"^^xsd:integer` and `"9"^^xsd:string` into one group.
  - `DISTINCT`/`REDUCED` include a projected variable's tag in the dedup key when its dimension is
    not statically determined, because two terms with the same lexical form and different dimensions
    are two different solutions. Those queries therefore also expose the `d_<var>` column in their
    result set — the only case where a top-level query's output columns are not exactly the variables
    it projected.
  - `MIN()`/`MAX()` over an argument whose dimension varies per row still order **lexicographically**
    (§15.1's full cross-kind ordering is not used for these), but the datatype they report is the
    winning row's own: the tag is aggregated with `arg_min`/`arg_max` over the same key as the value,
    never by an independent `MIN()` that could pick a different row.
  - Where the SQL this translator emits is lossier than SPARQL's abstract semantics, the tracked
    datatype follows the **SQL**, not the spec: `SECONDS()` is reported `xsd:integer` (not
    `xsd:decimal`) because `EXTRACT(SECOND ...)` yields a whole number, and integer division is
    reported `xsd:double` (not `xsd:decimal`). Otherwise `datatype()` would name a datatype whose
    canonical lexical form is not the text actually in the column.
  - Two places a per-row tag cannot be reconstructed, and are therefore emitted eagerly by their
    producer instead: a `VALUES` column whose cells disagree, and a `{ SELECT ... }` subquery
    projecting a variable its own arms disagree about. Both build their SQL during folding and are
    never re-rendered.
  - Two remaining refusals, both narrow. A `+`/`*` transitive-closure path whose step relation's
    endpoint term maps disagree cannot carry a tag: threading one through the recursive CTE would
    change what its terminating `UNION` deduplicates on, so a query asking about such a variable's
    dimension throws with a message saying so. And a variable bound only through a computed
    expression whose dimension the inference cannot analyse has no tag to offer either.
- **Deferred builtin functions** (throw `TranslationError`): the timezone accessors
  `TIMEZONE()`/`TZ()`; `SHA384()`/`SHA512()` (DuckDB has no built-in scalar function for either);
  any non-builtin (IRI-named) function call except the twelve XSD constructor casts described
  below. `TIMEZONE()`/`TZ()` are worth a word now that `xsd:dateTime` *is* statically recognised:
  the UTC offset is present in the literal's lexical form and could in principle be recovered, but
  only by lexically parsing the `Z`/`±hh:mm` suffix, since `TRY_CAST(... AS TIMESTAMP)` discards it.
  That parse — plus `TIMEZONE()`'s `xsd:dayTimeDuration` return type, which has no place in the
  current term model — is deliberately not implemented. Note this is now the *only* permanently
  deferred term-related builtin: `IRI()`/`URI()`/`BNODE()` are supported, and the term-kind
  builtins no longer refuse a dimension the mapping cannot decide.
  `NOW()`/`RAND()`/`UUID()`/`STRUUID()` **are** implemented: `NOW()` is stamped once per translation
  into a fixed `xsd:dateTime` string literal (SPARQL 1.1 §17.4.1.7's same-value-per-query
  requirement) rather than emitted as a per-row `current_timestamp`; `RAND()`/`UUID()`/`STRUUID()`
  are per-row. `MD5()`/`SHA1()`/`SHA256()`/`ABS()`/`CEIL()`/`FLOOR()`/`ROUND()`/
  `CONCAT()`/`STRLEN()`/`SUBSTR()`/`UCASE()`/`LCASE()`/`CONTAINS()`/`STRSTARTS()`/`STRENDS()`/
  `STRBEFORE()`/`STRAFTER()`/`REPLACE()`/`REGEX()`/`COALESCE()`/`IF()`/`isNUMERIC()`/`bound()`/
  `ENCODE_FOR_URI()`/the date/time accessors `YEAR()`/`MONTH()`/`DAY()`/`HOURS()`/`MINUTES()`/
  `SECONDS()` are all implemented. `ENCODE_FOR_URI()` goes through the same dialect
  `percentEncode()` seam used to reconstruct `rr:template`-generated terms (RFC3986
  percent-encoding, matching `r2rml::AbstractMap::percentEncode` exactly). The date/time accessors
  go through `TRY_CAST(... AS TIMESTAMP)` then `EXTRACT(field FROM ...)`, null-tolerantly like the
  numeric idiom below; `SECONDS()` truncates to whole seconds (DuckDB's `EXTRACT(SECOND FROM ...)`
  semantics), not fractional.
- **XSD constructor-function casts** — `xsd:integer()`, `xsd:long()`, `xsd:int()`, `xsd:short()`,
  `xsd:byte()`, `xsd:decimal()`, `xsd:double()`, `xsd:float()`, `xsd:string()`, `xsd:boolean()`,
  `xsd:date()`, `xsd:dateTime()` are the sole supported non-builtin (IRI-named) function calls;
  every other IRI-named call still throws `TranslationError`. These remain **value-level casts**:
  `xsd:integer(?x)` reinterprets the underlying VARCHAR's value null-tolerantly (non-castable input
  yields SQL `NULL`, never a translation-time or runtime error), and it does **not** rewrite `?x`'s
  own tracked datatype. What it does do is give the *result expression* a statically known datatype,
  which feeds the typed comparison / arithmetic / `ORDER BY` / `MIN`/`MAX` paths above — so
  `FILTER(xsd:integer(?x) > 5)` emits a direct numeric comparison rather than the string-fallback
  form. Specifics:
  - `xsd:integer()`, and its narrower aliases `xsd:long()`/`xsd:int()`/`xsd:short()`/`xsd:byte()`,
    all go through the same `TRY_CAST(... AS DOUBLE)` → `CAST(... AS BIGINT)` idiom — truncating
    toward zero via DuckDB's own cast semantics, not a hand-rolled XPath-conformant conversion, and
    with **no range clamping** to the narrower subtypes' XSD bounds (they are pure aliases of
    `xsd:integer()`).
  - `xsd:double()`/`xsd:float()` map to `TRY_CAST(... AS DOUBLE)` (the correct IEEE
    floating-point model for both).
  - `xsd:decimal()` uses a dedicated fixed-point `TRY_CAST(... AS DECIMAL(38,18))` path (DuckDB's
    maximum total precision, 18 fractional digits) rather than `DOUBLE`, so it doesn't silently
    lose precision on values like financial data — XSD decimal is technically arbitrary-precision,
    so this fixed precision/scale is itself a documented fidelity limit, not a full
    arbitrary-precision implementation.
  - `xsd:boolean()` uses `TRY_CAST(... AS BOOLEAN)`; DuckDB's `VARCHAR→BOOLEAN` cast already
    accepts XSD boolean's lexical space (`true`/`false`/`1`/`0`) and its `BOOLEAN→VARCHAR` cast
    renders lowercase `true`/`false`, matching XSD's canonical lexical form.
  - `xsd:date()` uses `TRY_CAST(... AS DATE)`; DuckDB's `DATE→VARCHAR` cast already renders
    `YYYY-MM-DD`, XSD date's canonical form.
  - `xsd:dateTime()` uses `TRY_CAST(... AS TIMESTAMP)` then swaps the space separator DuckDB's
    `TIMESTAMP→VARCHAR` cast produces for `T`, reaching XSD dateTime's canonical
    `YYYY-MM-DDTHH:MM:SS` form — again not a hand-rolled XPath-conformant conversion (no explicit
    fractional-second/timezone-offset handling beyond whatever DuckDB's own `TIMESTAMP` rendering
    gives).
  - `xsd:string()` is an identity pass-through.
- **Out-of-scope variable references** in FILTER/BIND/ORDER BY/HAVING throw `TranslationError` at
  translation time, rather than emulating SPARQL's precise per-row unbound-variable/type-error
  semantics.
- **`BIND`'s new variable is conservatively marked "optional"** whenever any variable it
  references is itself optional, even for expressions (like `COALESCE`) that would actually
  guarantee a non-null result — always safe (over-approximating optionality never produces wrong
  SQL, just occasionally-unnecessary null-safety machinery), just not maximally precise.
- **Subquery (`{ SELECT ... }`) variables are always conservatively marked "optional"** in the
  enclosing pattern, for the same reason.

### Dialects

`SqlDialect` (`include/sparql2sql/SqlDialect.h`) abstracts only the handful of SQL syntax points
that actually vary across engines and are exercised by the translator (identifier/string quoting,
`LIMIT`/`OFFSET` syntax, `EXISTS`, the null-tolerant try-casts to double/bigint/decimal/boolean/
date/timestamp, regex match, string/any-value aggregation, and DuckDB's `UNION [ALL] BY NAME`
schema-extending union). `tryCastToBigInt` is the seam that keeps statically integral arithmetic
integral — without it `?a + 1` would round-trip through `DOUBLE` and render `"10.0"`. Constructs
close enough to universal
across engines are emitted directly rather than routed through the dialect. `DuckDbDialect` is
currently the only implementation; add a new one by implementing `SqlDialect` and registering it
in `createDialect()` (`src/sparql2sql/DialectFactory.cpp`).

---

## Build Targets

| Target | Type | DuckDB required | Description |
|--------|------|-----------------|-------------|
| `sql2rdf_r2rml` | static library | No | Core R2RML library; link this in your project |
| `sql2rdf_yarrrml` | static library | No | YARRRML→R2RML translator (links yaml-cpp privately) |
| `sql2rdf_sparql` | static library | No | Standalone SPARQL 1.1 Query grammar parser |
| `sql2rdf_sparql2sql` | static library | No | SPARQL-to-SQL translator (see below) |
| `sql2rdf_type_catalog_loader` | static library | No | Fills a `TypeCatalog` from a live `SQLConnection` (see above) |
| `SQL2RDF++` | executable | Yes | CLI application |
| `test_runner` | executable | No | Catch2 unit tests |
| `sparql2sql_duckdb_tests` | executable | Yes | SPARQL-to-SQL real-DuckDB execution validation tests (`tests/duckdb/`) |

To link the core library from CMake:

```cmake
target_link_libraries(my_target PRIVATE sql2rdf_r2rml)
target_include_directories(my_target PRIVATE path/to/SQL2RDF++/include)
```
