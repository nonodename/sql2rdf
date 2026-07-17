#include "sparql-parser/PrettyPrinter.h"

#include <string>

namespace sparql {

using namespace ast;

namespace {

void printIndent(std::ostream &os, int indent) {
	for (int i = 0; i < indent; ++i)
		os << "  ";
}

std::string termStr(const Term &t) {
	switch (t.kind()) {
	case TermKind::Iri: {
		const auto &i = static_cast<const Iri &>(t);
		return i.lexicalForm.empty() ? ("<" + i.value + ">") : i.lexicalForm;
	}
	case TermKind::Var:
		return "?" + static_cast<const Var &>(t).name;
	case TermKind::BlankNode:
		return "_:" + static_cast<const BlankNode &>(t).label;
	case TermKind::Literal: {
		const auto &l = static_cast<const RdfLiteral &>(t);
		std::string s = "\"" + l.lexicalForm + "\"";
		if (!l.languageTag.empty()) {
			s += "@" + l.languageTag;
		} else if (l.datatype) {
			s += "^^<" + l.datatype->value + ">";
		}
		return s;
	}
	}
	return "<?>";
}

std::string pathStr(const PropertyPathExpr &p) {
	switch (p.kind()) {
	case PathKind::Predicate:
		return static_cast<const PredicatePath &>(p).iri->lexicalForm;
	case PathKind::Variable:
		return "?" + static_cast<const VariablePath &>(p).var->name;
	case PathKind::Inverse:
		return "^" + pathStr(*static_cast<const InversePath &>(p).child);
	case PathKind::Sequence: {
		const auto &s = static_cast<const SequencePath &>(p);
		return pathStr(*s.left) + "/" + pathStr(*s.right);
	}
	case PathKind::Alternative: {
		const auto &a = static_cast<const AlternativePath &>(p);
		return pathStr(*a.left) + "|" + pathStr(*a.right);
	}
	case PathKind::ZeroOrMore:
		return pathStr(*static_cast<const ZeroOrMorePath &>(p).child) + "*";
	case PathKind::OneOrMore:
		return pathStr(*static_cast<const OneOrMorePath &>(p).child) + "+";
	case PathKind::ZeroOrOne:
		return pathStr(*static_cast<const ZeroOrOnePath &>(p).child) + "?";
	case PathKind::NegatedPropertySet: {
		const auto &n = static_cast<const NegatedPropertySet &>(p);
		std::string s = "!(";
		bool first = true;
		for (const auto &f : n.forward) {
			if (!first)
				s += "|";
			s += f->lexicalForm;
			first = false;
		}
		for (const auto &inv : n.inverse) {
			if (!first)
				s += "|";
			s += "^" + inv->lexicalForm;
			first = false;
		}
		return s + ")";
	}
	}
	return "<?>";
}

const char *binaryOpSymbol(BinaryOp op) {
	switch (op) {
	case BinaryOp::Or:
		return "||";
	case BinaryOp::And:
		return "&&";
	case BinaryOp::Eq:
		return "=";
	case BinaryOp::Ne:
		return "!=";
	case BinaryOp::Lt:
		return "<";
	case BinaryOp::Gt:
		return ">";
	case BinaryOp::Le:
		return "<=";
	case BinaryOp::Ge:
		return ">=";
	case BinaryOp::Add:
		return "+";
	case BinaryOp::Sub:
		return "-";
	case BinaryOp::Mul:
		return "*";
	case BinaryOp::Div:
		return "/";
	}
	return "?";
}

const char *aggregateName(AggregateKind k) {
	switch (k) {
	case AggregateKind::Count:
		return "COUNT";
	case AggregateKind::Sum:
		return "SUM";
	case AggregateKind::Min:
		return "MIN";
	case AggregateKind::Max:
		return "MAX";
	case AggregateKind::Avg:
		return "AVG";
	case AggregateKind::Sample:
		return "SAMPLE";
	case AggregateKind::GroupConcat:
		return "GROUP_CONCAT";
	}
	return "?";
}

const char *builtinName(BuiltinFunction fn) {
	switch (fn) {
	case BuiltinFunction::Str:
		return "STR";
	case BuiltinFunction::Lang:
		return "LANG";
	case BuiltinFunction::LangMatches:
		return "LANGMATCHES";
	case BuiltinFunction::Datatype:
		return "DATATYPE";
	case BuiltinFunction::Bound:
		return "BOUND";
	case BuiltinFunction::IriFn:
		return "IRI";
	case BuiltinFunction::UriFn:
		return "URI";
	case BuiltinFunction::Bnode:
		return "BNODE";
	case BuiltinFunction::Rand:
		return "RAND";
	case BuiltinFunction::Abs:
		return "ABS";
	case BuiltinFunction::Ceil:
		return "CEIL";
	case BuiltinFunction::Floor:
		return "FLOOR";
	case BuiltinFunction::Round:
		return "ROUND";
	case BuiltinFunction::Concat:
		return "CONCAT";
	case BuiltinFunction::Strlen:
		return "STRLEN";
	case BuiltinFunction::Substr:
		return "SUBSTR";
	case BuiltinFunction::Ucase:
		return "UCASE";
	case BuiltinFunction::Lcase:
		return "LCASE";
	case BuiltinFunction::EncodeForUri:
		return "ENCODE_FOR_URI";
	case BuiltinFunction::Contains:
		return "CONTAINS";
	case BuiltinFunction::Strstarts:
		return "STRSTARTS";
	case BuiltinFunction::Strends:
		return "STRENDS";
	case BuiltinFunction::Strbefore:
		return "STRBEFORE";
	case BuiltinFunction::Strafter:
		return "STRAFTER";
	case BuiltinFunction::Replace:
		return "REPLACE";
	case BuiltinFunction::Regex:
		return "REGEX";
	case BuiltinFunction::Year:
		return "YEAR";
	case BuiltinFunction::Month:
		return "MONTH";
	case BuiltinFunction::Day:
		return "DAY";
	case BuiltinFunction::Hours:
		return "HOURS";
	case BuiltinFunction::Minutes:
		return "MINUTES";
	case BuiltinFunction::Seconds:
		return "SECONDS";
	case BuiltinFunction::Timezone:
		return "TIMEZONE";
	case BuiltinFunction::Tz:
		return "TZ";
	case BuiltinFunction::Now:
		return "NOW";
	case BuiltinFunction::Uuid:
		return "UUID";
	case BuiltinFunction::Struuid:
		return "STRUUID";
	case BuiltinFunction::Md5:
		return "MD5";
	case BuiltinFunction::Sha1:
		return "SHA1";
	case BuiltinFunction::Sha256:
		return "SHA256";
	case BuiltinFunction::Sha384:
		return "SHA384";
	case BuiltinFunction::Sha512:
		return "SHA512";
	case BuiltinFunction::Coalesce:
		return "COALESCE";
	case BuiltinFunction::If:
		return "IF";
	case BuiltinFunction::Strlang:
		return "STRLANG";
	case BuiltinFunction::Strdt:
		return "STRDT";
	case BuiltinFunction::SameTerm:
		return "sameTerm";
	case BuiltinFunction::IsIri:
		return "isIRI";
	case BuiltinFunction::IsUri:
		return "isURI";
	case BuiltinFunction::IsBlank:
		return "isBLANK";
	case BuiltinFunction::IsLiteral:
		return "isLITERAL";
	case BuiltinFunction::IsNumeric:
		return "isNUMERIC";
	}
	return "?";
}

std::string exprStr(const Expression &e) {
	switch (e.kind()) {
	case ExprKind::Literal:
		return termStr(*static_cast<const LiteralExpr &>(e).literal);
	case ExprKind::VarRef:
		return "?" + static_cast<const VarExpr &>(e).var->name;
	case ExprKind::IriRef:
		return static_cast<const IriExpr &>(e).iri->lexicalForm;
	case ExprKind::Unary: {
		const auto &u = static_cast<const UnaryExpr &>(e);
		const char *sym = (u.op == UnaryOp::Not) ? "!" : ((u.op == UnaryOp::Plus) ? "+" : "-");
		return std::string(sym) + exprStr(*u.operand);
	}
	case ExprKind::Binary: {
		const auto &b = static_cast<const BinaryExpr &>(e);
		return "(" + exprStr(*b.left) + " " + binaryOpSymbol(b.op) + " " + exprStr(*b.right) + ")";
	}
	case ExprKind::In: {
		const auto &in = static_cast<const InExpr &>(e);
		std::string s = exprStr(*in.lhs) + (in.negated ? " NOT IN (" : " IN (");
		for (std::size_t i = 0; i < in.list.size(); ++i) {
			if (i)
				s += ", ";
			s += exprStr(*in.list[i]);
		}
		return s + ")";
	}
	case ExprKind::FunctionCall: {
		const auto &f = static_cast<const FunctionCallExpr &>(e);
		std::string s = f.iri->lexicalForm + "(";
		if (f.distinct)
			s += "DISTINCT ";
		for (std::size_t i = 0; i < f.args.size(); ++i) {
			if (i)
				s += ", ";
			s += exprStr(*f.args[i]);
		}
		return s + ")";
	}
	case ExprKind::BuiltInCall: {
		const auto &b = static_cast<const BuiltInCallExpr &>(e);
		std::string s = std::string(builtinName(b.fn)) + "(";
		for (std::size_t i = 0; i < b.args.size(); ++i) {
			if (i)
				s += ", ";
			s += exprStr(*b.args[i]);
		}
		return s + ")";
	}
	case ExprKind::Aggregate: {
		const auto &a = static_cast<const AggregateExpr &>(e);
		std::string s = std::string(aggregateName(a.aggKind)) + "(";
		if (a.distinct)
			s += "DISTINCT ";
		if (a.star) {
			s += "*";
		} else if (a.arg) {
			s += exprStr(*a.arg);
		}
		if (a.hasSeparator)
			s += "; SEPARATOR=\"" + a.separator + "\"";
		return s + ")";
	}
	case ExprKind::Exists: {
		const auto &ex = static_cast<const ExistsExpr &>(e);
		return ex.negated ? "NOT EXISTS {...}" : "EXISTS {...}";
	}
	}
	return "<?>";
}

void printTriplePattern(std::ostream &os, int indent, const TriplePattern &tp) {
	printIndent(os, indent);
	os << "TriplePattern " << termStr(*tp.subject) << " " << pathStr(*tp.predicate) << " " << termStr(*tp.object)
	   << "\n";
}

void printQuery(std::ostream &os, int indent, const Query &q);

void printGroupGraphPattern(std::ostream &os, int indent, const GroupGraphPattern &g) {
	printIndent(os, indent);
	os << "GroupGraphPattern\n";
	for (const auto &el : g.elements) {
		switch (el->kind()) {
		case ElementKind::BasicGraphPattern: {
			const auto &b = static_cast<const BasicGraphPattern &>(*el);
			printIndent(os, indent + 1);
			os << "BasicGraphPattern\n";
			for (const auto &tp : b.triples)
				printTriplePattern(os, indent + 2, tp);
			break;
		}
		case ElementKind::GroupGraphPattern:
			printGroupGraphPattern(os, indent + 1, static_cast<const GroupGraphPattern &>(*el));
			break;
		case ElementKind::UnionGraphPattern: {
			const auto &u = static_cast<const UnionGraphPattern &>(*el);
			printIndent(os, indent + 1);
			os << "Union\n";
			for (const auto &br : u.branches)
				printGroupGraphPattern(os, indent + 2, *br);
			break;
		}
		case ElementKind::OptionalGraphPattern:
			printIndent(os, indent + 1);
			os << "Optional\n";
			printGroupGraphPattern(os, indent + 2, *static_cast<const OptionalGraphPattern &>(*el).pattern);
			break;
		case ElementKind::MinusGraphPattern:
			printIndent(os, indent + 1);
			os << "Minus\n";
			printGroupGraphPattern(os, indent + 2, *static_cast<const MinusGraphPattern &>(*el).pattern);
			break;
		case ElementKind::GraphGraphPattern: {
			const auto &gg = static_cast<const GraphGraphPattern &>(*el);
			printIndent(os, indent + 1);
			os << "Graph " << termStr(*gg.graphNameOrVar) << "\n";
			printGroupGraphPattern(os, indent + 2, *gg.pattern);
			break;
		}
		case ElementKind::ServiceGraphPattern: {
			const auto &s = static_cast<const ServiceGraphPattern &>(*el);
			printIndent(os, indent + 1);
			os << "Service" << (s.silent ? " SILENT" : "") << " " << termStr(*s.endpoint) << "\n";
			printGroupGraphPattern(os, indent + 2, *s.pattern);
			break;
		}
		case ElementKind::Filter: {
			const auto &f = static_cast<const Filter &>(*el);
			printIndent(os, indent + 1);
			os << "Filter " << exprStr(*f.constraint) << "\n";
			if (f.constraint->kind() == ExprKind::Exists) {
				printGroupGraphPattern(os, indent + 2, *static_cast<const ExistsExpr &>(*f.constraint).pattern);
			}
			break;
		}
		case ElementKind::Bind: {
			const auto &b = static_cast<const Bind &>(*el);
			printIndent(os, indent + 1);
			os << "Bind ?" << b.var->name << " = " << exprStr(*b.expr) << "\n";
			break;
		}
		case ElementKind::InlineData: {
			const auto &v = static_cast<const InlineData &>(*el);
			printIndent(os, indent + 1);
			os << "Values (";
			for (std::size_t i = 0; i < v.vars.size(); ++i) {
				if (i)
					os << " ";
				os << "?" << v.vars[i]->name;
			}
			os << ") " << v.rows.size() << " row(s)\n";
			break;
		}
		case ElementKind::SubSelect:
			printIndent(os, indent + 1);
			os << "SubSelect\n";
			printQuery(os, indent + 2, *static_cast<const SubSelectElement &>(*el).query);
			break;
		}
	}
}

void printQuery(std::ostream &os, int indent, const Query &q) {
	printIndent(os, indent);
	const char *formName = "SELECT";
	if (q.form == QueryForm::Construct)
		formName = "CONSTRUCT";
	else if (q.form == QueryForm::Describe)
		formName = "DESCRIBE";
	else if (q.form == QueryForm::Ask)
		formName = "ASK";
	os << "Query [" << formName << (q.distinct ? " DISTINCT" : "") << (q.reduced ? " REDUCED" : "") << "]\n";

	if (!q.prologue.baseIri.empty() || !q.prologue.prefixes.empty()) {
		printIndent(os, indent + 1);
		os << "Prologue\n";
		if (!q.prologue.baseIri.empty()) {
			printIndent(os, indent + 2);
			os << "Base <" << q.prologue.baseIri << ">\n";
		}
		for (const auto &p : q.prologue.prefixes) {
			printIndent(os, indent + 2);
			os << "Prefix " << p.prefix << " -> " << p.iri << "\n";
		}
	}
	for (const auto &dc : q.datasetClauses) {
		printIndent(os, indent + 1);
		os << (dc.kind == DatasetClauseKind::Named ? "FromNamed " : "From ") << dc.iri->lexicalForm << "\n";
	}

	if (q.form == QueryForm::Select) {
		printIndent(os, indent + 1);
		os << "Select\n";
		if (q.selectStar) {
			printIndent(os, indent + 2);
			os << "*\n";
		}
		for (const auto &item : q.selectItems) {
			printIndent(os, indent + 2);
			if (item.expr) {
				os << "(" << exprStr(*item.expr) << " AS ?" << item.var->name << ")\n";
			} else {
				os << "?" << item.var->name << "\n";
			}
		}
	} else if (q.form == QueryForm::Construct) {
		printIndent(os, indent + 1);
		os << "ConstructTemplate\n";
		for (const auto &tp : q.constructTemplate)
			printTriplePattern(os, indent + 2, tp);
	} else if (q.form == QueryForm::Describe) {
		printIndent(os, indent + 1);
		os << "Describe" << (q.describeStar ? " *" : "") << "\n";
		for (const auto &t : q.describeTargets) {
			printIndent(os, indent + 2);
			os << termStr(*t) << "\n";
		}
	}

	if (q.where) {
		printIndent(os, indent + 1);
		os << "Where:\n";
		printGroupGraphPattern(os, indent + 2, *q.where);
	}

	const SolutionModifier &sm = q.solutionModifier;
	if (!sm.groupBy.empty()) {
		printIndent(os, indent + 1);
		os << "GroupBy\n";
		for (const auto &g : sm.groupBy) {
			printIndent(os, indent + 2);
			os << exprStr(*g.expr);
			if (g.asVar)
				os << " AS ?" << g.asVar->name;
			os << "\n";
		}
	}
	if (!sm.having.empty()) {
		printIndent(os, indent + 1);
		os << "Having\n";
		for (const auto &h : sm.having) {
			printIndent(os, indent + 2);
			os << exprStr(*h) << "\n";
		}
	}
	if (!sm.orderBy.empty()) {
		printIndent(os, indent + 1);
		os << "OrderBy\n";
		for (const auto &o : sm.orderBy) {
			printIndent(os, indent + 2);
			os << (o.direction == OrderDirection::Desc ? "DESC " : "ASC ") << exprStr(*o.expr) << "\n";
		}
	}
	if (sm.hasLimit) {
		printIndent(os, indent + 1);
		os << "Limit " << sm.limit << "\n";
	}
	if (sm.hasOffset) {
		printIndent(os, indent + 1);
		os << "Offset " << sm.offset << "\n";
	}
	if (q.valuesClause) {
		printIndent(os, indent + 1);
		os << "Values (";
		for (std::size_t i = 0; i < q.valuesClause->vars.size(); ++i) {
			if (i)
				os << " ";
			os << "?" << q.valuesClause->vars[i]->name;
		}
		os << ") " << q.valuesClause->rows.size() << " row(s)\n";
	}
}

} // namespace

void print(std::ostream &os, const Query &query) {
	printQuery(os, 0, query);
}

std::ostream &operator<<(std::ostream &os, const Query &query) {
	print(os, query);
	return os;
}

} // namespace sparql
