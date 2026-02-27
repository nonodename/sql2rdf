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

### `R2RMLParser`

Parses a Turtle (`.ttl`) R2RML mapping file into the C++ object model.

```cpp
#include "r2rml/R2RMLParser.h"

class R2RMLParser {
public:
    R2RMLParser();
    R2RMLMapping parse(const std::string& mappingFilePath);
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
| `SubjectMap` | `rr:subjectMap` | Like `TermMap`; also carries `classIRIs` and `graphMaps` |
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

## Build Targets

| Target | Type | DuckDB required | Description |
|--------|------|-----------------|-------------|
| `sql2rdf_r2rml` | static library | No | Core R2RML library; link this in your project |
| `SQL2RDF++` | executable | Yes | CLI application |
| `test_runner` | executable | No | Catch2 unit tests |

To link the core library from CMake:

```cmake
target_link_libraries(my_target PRIVATE sql2rdf_r2rml)
target_include_directories(my_target PRIVATE path/to/SQL2RDF++/include)
```
