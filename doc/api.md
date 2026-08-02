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
};
```

| Method | Description |
|--------|-------------|
| `parse(path, ignoreNonFatalErrors)` | Parses the mapping document at `path` and returns a populated `R2RMLMapping`. |
| `create(path)` (static) | Instantiates the concrete parser appropriate for `path`'s extension: `.ttl` → `R2RMLParser`, `.yml`/`.yaml`/`.yarrrml` → `YARRRMLParser`. Throws `std::runtime_error` if no known format matches. |

`MappingParser::create` is defined in `src/yarrrml/MappingParserFactory.cpp`, part of the `sql2rdf_yarrrml` library, not `sql2rdf_r2rml` — the core library must never depend on yaml-cpp/YARRRML (see layering rules in `CLAUDE.md`). Calling `create()` therefore requires linking `sql2rdf_yarrrml`, even if the resolved format turns out to be R2RML; consumers that only link `sql2rdf_r2rml` and never touch YARRRML should construct `R2RMLParser` directly instead.

`TripleCollector`, the shared triple-gathering helper used internally by both parsers, is also declared in `r2rml/MappingParser.h` (moved out of `r2rml/R2RMLParser.h`).

### `R2RMLParser`

Parses a Turtle (`.ttl`) R2RML mapping file into the C++ object model. Inherits `MappingParser`.

```cpp
#include "r2rml/R2RMLParser.h"

class R2RMLParser : public MappingParser {
public:
    R2RMLParser();
    R2RMLMapping parse(const std::string& mappingFilePath, bool ignoreNonFatalErrors = true) override;
};
```

| Method | Description |
|--------|-------------|
| `parse(path)` | Parses the Turtle file at `path` and returns a populated `R2RMLMapping`. Throws on parse error. |

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
};
```

`R2RMLMapping` is **move-only** (no copy constructor or copy assignment).

| Method | Description |
|--------|-------------|
| `loadMapping(path)` | Parses a TTL file and populates the object (alternative to using `R2RMLParser` directly). |
| `processDatabase(db, writer)` | Executes all triples maps against `db` and writes RDF triples to `writer`. |
| `isValid()` | Returns `true` if all contained `TriplesMap` objects are valid. |
| `isValidInsideOut()` | Returns `true` if the mapping contains no constructs prohibited in "inside-out" (SQL-export) mode: no `rr:LogicalTable`, `rr:sqlQuery`, `rr:refObjectMap`, or `rr:JoinCondition`. |

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
| `GraphMap` | `rr:graphMap` | Generates named-graph IRIs |
| `ReferencingObjectMap` | `rr:refObjectMap` | Joins to a parent `TriplesMap`; prohibited in inside-out mode |

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
```

The translator builds a small relational-algebra IR (`sparql2sql/ir/RelNode.h`) rather than
emitting SQL strings directly, then applies a fixed pipeline of semantics-preserving rewrites
(`sparql2sql/ir/Optimizer.h`) before rendering once, in this order:

1. **FILTER pushdown.** Each FILTER is split into its top-level conjuncts and each is routed to the
   smallest relation supplying its variables (inner-join sides, every arm of a UNION, an anti-join's
   left side; LEFT OUTER JOIN, BIND and EXISTS-bearing conjuncts are boundaries). A conjunct that
   reaches an SPJ block is **folded into that block's `WHERE` list** rather than wrapping it in
   another subquery — which is the point of the pass: it keeps the block mergeable by the next
   step. (Merely re-parenting a filter lower buys nothing against an engine that already pushes
   predicates through derived tables; keeping blocks mergeable does.)
2. **Inner-join SPJ flattening.** An N-triple BGP becomes one flat `SELECT ... FROM a, b, c WHERE
   ...` instead of N−1 nested subqueries.
3. **Self-join elimination.** Two patterns over the same table bound to the same subject variable
   collapse to a single scan — the subject map is assumed row-unique.
4. **DISTINCT elimination.** The per-pattern `SELECT DISTINCT` is dropped when the enclosing query
   is `SELECT DISTINCT`/`ASK`, and likewise inside an EXISTS body (an existence check cannot see
   duplicates).

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
};
```

`catalog` is optional. When supplied, an equi-join whose two sides are of **comparable declared
type** (per the catalog) is emitted on the native, uncast columns instead of on generated term text
— index-friendly and materially faster on large tables. Two shapes qualify:

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

The catalog is keyed by **logical-table identity**, which for an `rr:sqlQuery` view is the view's
SQL text rather than a table name. Columns of an R2RML view therefore never resolve to a declared
type, so joins involving one always keep the VARCHAR-cast/term-text form. Mapping a base table
(`rr:tableName`) rather than a view is what makes a join key eligible. When `catalog` is null, or a column's type is unknown, or the types aren't comparable, the
join falls back to the always-correct VARCHAR-cast comparison. The catalog is a plain,
dependency-free data structure: the core library never opens a database; the CLI populates it from
`information_schema.columns`. Because all downstream variables remain VARCHAR, only the join
*condition* changes — projected values and result semantics are identical either way.

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

The CLI exposes this via `-T <file.rq> [--dialect <name>]` (default dialect: `duckdb`), paired
with the mapping-file positional argument. If the database-file positional is also given, the CLI
reads that database's column types into a `TypeCatalog` (so native-typed join keys are enabled),
translates against it, then executes the SQL via `r2rml::DuckDBConnection` and prints the result
rows; see `-h` for exact stdout/stderr routing. Without a database file, the SQL alone is printed
and joins use the VARCHAR-cast fallback.

### Supported SPARQL subset / Known limitations

- **Query forms**: only `SELECT` and `ASK`. `CONSTRUCT`/`DESCRIBE` throw `TranslationError`
  (they produce an RDF graph, not a row set — a different translation shape, not yet implemented).
- **Property paths**: `^` (inverse), `/` (sequence), `|` (alternative), `?` (zero-or-one) and
  negated property sets (`!(:a|^:b)`) are supported, alongside a constant IRI/`a` or a bare
  variable. They are translated by desugaring into the relational algebra exactly as SPARQL 1.1
  §18.1.7 defines them: `^E` swaps the pattern's endpoints, `E1/E2` joins over a fresh intermediate
  variable, `E1|E2` unions, `E?` unions the one-step path with the zero-length path, and a negated
  property set becomes a "predicate not in {…}" condition on each candidate (plus a reversed arm
  for its `^`-marked members). Two consequences worth knowing:
  - A sequence path's intermediate variable is **internal**: it takes part in joins but is never
    projected, including by `SELECT *`. (Blank-node positions are internal in the same way, and are
    likewise not projected by `SELECT *` — they are not query variables.)
  - `E?` with **both endpoints unbound** (`?x :p? ?y`) has nothing to anchor its zero-length half
    to, so that half enumerates every term the mapping can produce as a subject or object — a UNION
    scan over every logical table in the mapping. This is spec-correct but not cheap; binding either
    endpoint reduces it to a single constant row. The enumeration over-approximates only in that a
    `TriplesMap` whose subject appears solely as the parent of a referencing object map contributes
    its subjects via its own arm; it deliberately excludes predicate IRIs, per SPARQL's `nodes(G)`.
- **`*` and `+` property paths are not supported** and throw `TranslationError` naming the operator.
  Unlike the operators above they are not syntactic sugar over a fixed relational algebra
  expression: an arbitrary-length path needs recursive SQL (`WITH RECURSIVE`), so supporting them
  requires both a transitive-closure IR node and a matching `SqlDialect` seam.
- **No `GRAPH`/named graphs**: this R2RML mapping model never populates `rr:graph`/`rr:graphMap`,
  so `GRAPH` patterns have nothing to translate against and always throw.
- **No `SERVICE`** (federated query): always throws, matching `sql2rdf_sparql`'s own "no
  federated-query execution semantics" stance.
- **Every SPARQL variable is a plain SQL `VARCHAR`** holding the RDF term's lexical string form
  (IRI string / literal lexical form / blank node label) — no term-kind, datatype, or language
  dimension is tracked. This is the single biggest scope simplification and disables/approximates:
  - `isIRI()`/`isBLANK()`/`isLITERAL()`/`datatype()`/`lang()`/`langMatches()`/`STRLANG()`/
    `STRDT()`/`IRI()`/`URI()`/`BNODE()` — all throw `TranslationError` (would need the missing
    term-kind dimension).
  - `sameTerm()` is approximated as plain string equality (not full type/datatype/language
    matching).
  - `ORDER BY` and `MIN()`/`MAX()` sort/compare **lexicographically**, not numerically, for
    variables that happen to hold numbers — there is no static per-variable type inference across
    the UNION-ALL-combined candidate relations that make up a triple pattern's translation.
  - Numeric comparisons/arithmetic (`<`, `>`, `+`, aggregates, etc.) go through
    `TRY_CAST(... AS DOUBLE)` at point of use; `=`/`<>`/`<`/`>`/`<=`/`>=` fall back to a plain
    VARCHAR comparison when either side isn't numeric-castable, so e.g. `FILTER(?name < "M")`
    (string ordering) still works correctly.
  - The one exception is an equi-**join** key: with a `TypeCatalog`, a join between two pure base
    columns — or between two same-invertible-template terms — of comparable declared type is
    compared on the native (uncast) columns (see the `translateQuery` `catalog` parameter above).
    This changes only the join condition, not the VARCHAR representation of any projected
    variable.
- **Template matching (`rr:template`) assumes RFC3986-unreserved-only column values**: forward
  R2RML generation percent-encodes substituted column values, but the translator's reverse
  direction (reconstructing/matching a template) does not apply percent-encoding — correct as
  long as template-referenced columns hold only unreserved characters (typical for numeric/simple
  string IDs), not guaranteed in general.
- **Deferred builtin functions** (throw `TranslationError`): `ENCODE_FOR_URI()`; date/time
  accessors (`YEAR()`/`MONTH()`/`DAY()`/`HOURS()`/`MINUTES()`/`SECONDS()`/`TIMEZONE()`/`TZ()`);
  non-deterministic/context functions (`NOW()`/`RAND()`/`UUID()`/`STRUUID()`); `SHA384()`/`SHA512()`
  (DuckDB has no built-in scalar function for either); any non-builtin (IRI-named) function call
  except the five XSD constructor casts described below. `MD5()`/`SHA1()`/`SHA256()`/`ABS()`/
  `CEIL()`/`FLOOR()`/`ROUND()`/`CONCAT()`/`STRLEN()`/`SUBSTR()`/`UCASE()`/`LCASE()`/`CONTAINS()`/
  `STRSTARTS()`/`STRENDS()`/`STRBEFORE()`/`STRAFTER()`/`REPLACE()`/`REGEX()`/`COALESCE()`/`IF()`/
  `isNUMERIC()`/`bound()` are all implemented.
- **XSD constructor-function casts** — `xsd:integer()`, `xsd:decimal()`, `xsd:double()`,
  `xsd:float()`, `xsd:string()` are the sole supported non-builtin (IRI-named) function calls;
  every other IRI-named call (including `xsd:boolean()` and the rest of the XSD constructor
  family) still throws `TranslationError`. These are **value-level casts, not XSD datatype
  tagging** — this translator has no term-kind/datatype dimension at all (see above), so
  `xsd:integer(?x)` doesn't give `?x` a tracked datatype, it just reinterprets the underlying
  VARCHAR's numeric value, null-tolerantly, via the same `TRY_CAST(... AS DOUBLE)` idiom used
  elsewhere in this file (non-numeric input yields SQL `NULL`, never a translation-time or
  runtime error). `xsd:decimal()`/`xsd:double()`/`xsd:float()` all map to the same DOUBLE
  round-trip (no fixed-precision `DECIMAL` type is modeled anywhere in this translator);
  `xsd:integer()` additionally truncates through `BIGINT`; `xsd:string()` is an identity
  pass-through.
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
`LIMIT`/`OFFSET` syntax, `EXISTS`, numeric try-cast, regex match, string/any-value aggregation,
and DuckDB's `UNION [ALL] BY NAME` schema-extending union). Constructs close enough to universal
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
| `SQL2RDF++` | executable | Yes | CLI application |
| `test_runner` | executable | No | Catch2 unit tests |
| `sparql2sql_duckdb_tests` | executable | Yes | SPARQL-to-SQL real-DuckDB execution validation tests (`tests/duckdb/`) |

To link the core library from CMake:

```cmake
target_link_libraries(my_target PRIVATE sql2rdf_r2rml)
target_include_directories(my_target PRIVATE path/to/SQL2RDF++/include)
```
