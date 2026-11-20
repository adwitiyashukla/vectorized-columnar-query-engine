#include "parser.h"

#include <cctype>
#include <cstdlib>

#include "error.h"
#include "hash_table.h"
#include "util.h"

namespace vcqe {
namespace {

enum class TokenKind { kEnd, kIdentifier, kNumber, kString, kSymbol };

struct Token {
  TokenKind kind = TokenKind::kEnd;
  std::string text;
  std::string lower;
};

class Lexer {
public:
  explicit Lexer(const std::string& sql) : sql_(sql) {
    Advance();
  }

  const Token& current() const {
    return current_;
  }

  void Advance() {
    SkipSpace();
    current_ = Token();
    if (position_ >= sql_.size()) {
      current_.kind = TokenKind::kEnd;
      return;
    }
    const char c = sql_[position_];
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      const size_t begin = position_;
      while (
          position_ < sql_.size() &&
          (std::isalnum(static_cast<unsigned char>(sql_[position_])) || sql_[position_] == '_')) {
        ++position_;
      }
      current_.kind = TokenKind::kIdentifier;
      current_.text = sql_.substr(begin, position_ - begin);
      current_.lower = ToLower(current_.text);
      return;
    }
    if (std::isdigit(static_cast<unsigned char>(c))) {
      const size_t begin = position_;
      while (
          position_ < sql_.size() &&
          (std::isdigit(static_cast<unsigned char>(sql_[position_])) || sql_[position_] == '.')) {
        ++position_;
      }
      current_.kind = TokenKind::kNumber;
      current_.text = sql_.substr(begin, position_ - begin);
      current_.lower = current_.text;
      return;
    }
    if (c == '\'') {
      ++position_;
      std::string text;
      while (position_ < sql_.size() && sql_[position_] != '\'') {
        text.push_back(sql_[position_++]);
      }
      Check(position_ < sql_.size(), "unterminated string literal");
      ++position_;
      current_.kind = TokenKind::kString;
      current_.text = text;
      current_.lower = text;
      return;
    }
    static const char* kTwoCharacter[] = {"<=", ">=", "<>", "!="};
    if (position_ + 1 < sql_.size()) {
      const std::string pair = sql_.substr(position_, 2);
      for (const char* candidate : kTwoCharacter) {
        if (pair == candidate) {
          position_ += 2;
          current_.kind = TokenKind::kSymbol;
          current_.text = pair == "!=" ? "<>" : pair;
          current_.lower = current_.text;
          return;
        }
      }
    }
    ++position_;
    current_.kind = TokenKind::kSymbol;
    current_.text = std::string(1, c);
    current_.lower = current_.text;
  }

private:
  void SkipSpace() {
    while (position_ < sql_.size()) {
      if (std::isspace(static_cast<unsigned char>(sql_[position_]))) {
        ++position_;
      } else if (position_ + 1 < sql_.size() && sql_[position_] == '-' &&
                 sql_[position_ + 1] == '-') {
        while (position_ < sql_.size() && sql_[position_] != '\n') {
          ++position_;
        }
      } else {
        return;
      }
    }
  }

  const std::string& sql_;
  size_t position_ = 0;
  Token current_;
};

bool IsKeyword(const Token& token, const char* keyword) {
  return token.kind == TokenKind::kIdentifier && token.lower == keyword;
}

class Parser {
public:
  explicit Parser(const std::string& sql) : lexer_(sql) {}

  SelectStatement ParseStatement() {
    SelectStatement statement;
    Expect("select");
    ParseSelectList(&statement);
    Expect("from");
    ParseTableList(&statement);
    if (IsKeyword(lexer_.current(), "where")) {
      lexer_.Advance();
      statement.where = ParseExpression();
    }
    if (IsKeyword(lexer_.current(), "group")) {
      lexer_.Advance();
      Expect("by");
      statement.group_by.push_back(ParseExpression());
      while (Symbol(",")) {
        statement.group_by.push_back(ParseExpression());
      }
    }
    if (IsKeyword(lexer_.current(), "having")) {
      lexer_.Advance();
      statement.having = ParseExpression();
    }
    if (IsKeyword(lexer_.current(), "order")) {
      lexer_.Advance();
      Expect("by");
      ParseOrderTerm(&statement);
      while (Symbol(",")) {
        ParseOrderTerm(&statement);
      }
    }
    if (IsKeyword(lexer_.current(), "limit")) {
      lexer_.Advance();
      Check(lexer_.current().kind == TokenKind::kNumber, "expected a number after limit");
      statement.limit = std::stoll(lexer_.current().text);
      lexer_.Advance();
    }
    if (Symbol(";")) {
    }
    Check(lexer_.current().kind == TokenKind::kEnd, "unexpected text after the query");
    return statement;
  }

private:
  void Expect(const char* keyword) {
    Check(IsKeyword(lexer_.current(), keyword), std::string("expected ") + keyword);
    lexer_.Advance();
  }

  bool Symbol(const char* symbol) {
    if (lexer_.current().kind == TokenKind::kSymbol && lexer_.current().text == symbol) {
      lexer_.Advance();
      return true;
    }
    return false;
  }

  bool PeekSymbol(const char* symbol) const {
    return lexer_.current().kind == TokenKind::kSymbol && lexer_.current().text == symbol;
  }

  void ParseSelectList(SelectStatement* statement) {
    while (true) {
      SelectItem item;
      if (PeekSymbol("*")) {
        lexer_.Advance();
        item.star = true;
      } else {
        item.expr = ParseExpression();
        if (IsKeyword(lexer_.current(), "as")) {
          lexer_.Advance();
          Check(lexer_.current().kind == TokenKind::kIdentifier, "expected an alias name");
          item.alias = lexer_.current().text;
          lexer_.Advance();
        } else if (lexer_.current().kind == TokenKind::kIdentifier && !IsReserved()) {
          item.alias = lexer_.current().text;
          lexer_.Advance();
        }
      }
      statement->items.push_back(std::move(item));
      if (!Symbol(",")) {
        return;
      }
    }
  }

  bool IsReserved() const {
    static const char* kWords[] = {"from", "where",  "group", "having", "order",
                                   "by",   "limit",  "and",   "or",     "join",
                                   "on",   "select", "as",    "not",    "like"};
    for (const char* word : kWords) {
      if (lexer_.current().lower == word) {
        return true;
      }
    }
    return false;
  }

  void ParseTableList(SelectStatement* statement) {
    while (true) {
      TableRef table;
      Check(lexer_.current().kind == TokenKind::kIdentifier, "expected a table name");
      table.name = ToLower(lexer_.current().text);
      lexer_.Advance();
      if (IsKeyword(lexer_.current(), "as")) {
        lexer_.Advance();
        Check(lexer_.current().kind == TokenKind::kIdentifier, "expected a table alias");
        table.alias = ToLower(lexer_.current().text);
        lexer_.Advance();
      } else if (lexer_.current().kind == TokenKind::kIdentifier && !IsReserved()) {
        table.alias = ToLower(lexer_.current().text);
        lexer_.Advance();
      }
      if (table.alias.empty()) {
        table.alias = table.name;
      }
      statement->tables.push_back(std::move(table));
      if (Symbol(",")) {
        continue;
      }
      if (IsKeyword(lexer_.current(), "join") || IsKeyword(lexer_.current(), "inner")) {
        if (IsKeyword(lexer_.current(), "inner")) {
          lexer_.Advance();
        }
        Expect("join");
        continue;
      }
      return;
    }
  }

  void ParseOrderTerm(SelectStatement* statement) {
    OrderTerm term;
    term.expr = ParseExpression();
    if (IsKeyword(lexer_.current(), "asc")) {
      lexer_.Advance();
    } else if (IsKeyword(lexer_.current(), "desc")) {
      term.descending = true;
      lexer_.Advance();
    }
    statement->order_by.push_back(std::move(term));
  }

  ExprPtr ParseExpression() {
    return ParseOr();
  }

  ExprPtr ParseOr() {
    ExprPtr left = ParseAnd();
    if (!IsKeyword(lexer_.current(), "or")) {
      return left;
    }
    std::vector<ExprPtr> children;
    children.push_back(std::move(left));
    while (IsKeyword(lexer_.current(), "or")) {
      lexer_.Advance();
      children.push_back(ParseAnd());
    }
    return MakeLogical(LogicalOp::kOr, std::move(children));
  }

  ExprPtr ParseAnd() {
    ExprPtr left = ParseNot();
    if (!IsKeyword(lexer_.current(), "and")) {
      return left;
    }
    std::vector<ExprPtr> children;
    children.push_back(std::move(left));
    while (IsKeyword(lexer_.current(), "and")) {
      lexer_.Advance();
      children.push_back(ParseNot());
    }
    return MakeLogical(LogicalOp::kAnd, std::move(children));
  }

  ExprPtr ParseNot() {
    if (IsKeyword(lexer_.current(), "not")) {
      lexer_.Advance();
      std::vector<ExprPtr> children;
      children.push_back(ParseNot());
      return MakeLogical(LogicalOp::kNot, std::move(children));
    }
    return ParseComparison();
  }

  ExprPtr ParseComparison() {
    ExprPtr left = ParseAdditive();
    if (IsKeyword(lexer_.current(), "between")) {
      lexer_.Advance();
      ExprPtr low = ParseAdditive();
      Expect("and");
      ExprPtr high = ParseAdditive();
      std::vector<ExprPtr> children;
      children.push_back(MakeCompare(CompareOp::kGreaterEqual, left, std::move(low)));
      children.push_back(MakeCompare(CompareOp::kLessEqual, left, std::move(high)));
      return MakeLogical(LogicalOp::kAnd, std::move(children));
    }
    if (IsKeyword(lexer_.current(), "in")) {
      lexer_.Advance();
      Check(Symbol("("), "expected ( after in");
      std::vector<ExprPtr> children;
      while (true) {
        children.push_back(MakeCompare(CompareOp::kEqual, left, ParseAdditive()));
        if (Symbol(",")) {
          continue;
        }
        break;
      }
      Check(Symbol(")"), "expected ) after an in list");
      return MakeLogical(LogicalOp::kOr, std::move(children));
    }
    bool negated = false;
    if (IsKeyword(lexer_.current(), "not")) {
      lexer_.Advance();
      negated = true;
    }
    if (IsKeyword(lexer_.current(), "like")) {
      lexer_.Advance();
      Check(lexer_.current().kind == TokenKind::kString, "expected a pattern after like");
      const std::string pattern = lexer_.current().text;
      lexer_.Advance();
      return MakeLike(std::move(left), pattern, negated);
    }
    if (IsKeyword(lexer_.current(), "is")) {
      lexer_.Advance();
      bool not_null = false;
      if (IsKeyword(lexer_.current(), "not")) {
        lexer_.Advance();
        not_null = true;
      }
      Expect("null");
      return MakeIsNull(std::move(left), not_null);
    }
    Check(!negated, "expected like after not");
    CompareOp op = CompareOp::kEqual;
    if (Symbol("=")) {
      op = CompareOp::kEqual;
    } else if (Symbol("<>")) {
      op = CompareOp::kNotEqual;
    } else if (Symbol("<=")) {
      op = CompareOp::kLessEqual;
    } else if (Symbol(">=")) {
      op = CompareOp::kGreaterEqual;
    } else if (Symbol("<")) {
      op = CompareOp::kLess;
    } else if (Symbol(">")) {
      op = CompareOp::kGreater;
    } else {
      return left;
    }
    return MakeCompare(op, std::move(left), ParseAdditive());
  }

  ExprPtr ParseAdditive() {
    ExprPtr left = ParseMultiplicative();
    while (true) {
      if (Symbol("+")) {
        left = MakeArithmetic(ArithOp::kAdd, std::move(left), ParseMultiplicative());
      } else if (Symbol("-")) {
        left = MakeArithmetic(ArithOp::kSubtract, std::move(left), ParseMultiplicative());
      } else {
        return left;
      }
    }
  }

  ExprPtr ParseMultiplicative() {
    ExprPtr left = ParseUnary();
    while (true) {
      if (Symbol("*")) {
        left = MakeArithmetic(ArithOp::kMultiply, std::move(left), ParseUnary());
      } else if (Symbol("/")) {
        left = MakeArithmetic(ArithOp::kDivide, std::move(left), ParseUnary());
      } else {
        return left;
      }
    }
  }

  ExprPtr ParseUnary() {
    if (Symbol("-")) {
      ExprPtr child = ParseUnary();
      if (child->kind == ExprKind::kConstant && child->constant.type != DataType::kString) {
        Value value = child->constant;
        if (value.type == DataType::kDouble) {
          value.real = -value.real;
        } else {
          value.integer = -value.integer;
        }
        return MakeConstant(value);
      }
      return MakeArithmetic(ArithOp::kSubtract, MakeConstant(Value::Int(0)), std::move(child));
    }
    return ParsePrimary();
  }

  ExprPtr ParsePrimary() {
    const Token token = lexer_.current();
    if (token.kind == TokenKind::kNumber) {
      lexer_.Advance();
      if (token.text.find('.') != std::string::npos) {
        const size_t dot = token.text.find('.');
        const uint8_t scale = static_cast<uint8_t>(token.text.size() - dot - 1);
        return MakeConstant(Value::Decimal(ParseDecimal(token.text, scale), scale));
      }
      return MakeConstant(Value::Int(std::stoll(token.text)));
    }
    if (token.kind == TokenKind::kString) {
      lexer_.Advance();
      return MakeConstant(Value::String(token.text));
    }
    if (Symbol("(")) {
      ExprPtr inner = ParseExpression();
      Check(Symbol(")"), "expected )");
      return inner;
    }
    Check(token.kind == TokenKind::kIdentifier, "unexpected token in expression: " + token.text);
    if (token.lower == "date") {
      lexer_.Advance();
      Check(lexer_.current().kind == TokenKind::kString, "expected a date literal");
      const int32_t days = ParseDate(lexer_.current().text);
      lexer_.Advance();
      return MakeConstant(Value::Date(ParseIntervalTail(days)));
    }
    if (token.lower == "interval") {
      Fail("interval is only supported next to a date literal");
    }
    lexer_.Advance();
    if (PeekSymbol("(")) {
      return ParseFunction(token.lower);
    }
    std::string table;
    std::string column = token.text;
    if (PeekSymbol(".")) {
      lexer_.Advance();
      Check(lexer_.current().kind == TokenKind::kIdentifier, "expected a column name");
      table = ToLower(column);
      column = lexer_.current().text;
      lexer_.Advance();
    }
    return MakeColumn(table, ToLower(column));
  }

  int32_t ParseIntervalTail(int32_t days) {
    while (true) {
      const bool plus = PeekSymbol("+");
      const bool minus = PeekSymbol("-");
      if (!plus && !minus) {
        return days;
      }
      lexer_.Advance();
      Check(IsKeyword(lexer_.current(), "interval"), "expected interval after a date literal");
      lexer_.Advance();
      Check(lexer_.current().kind == TokenKind::kString ||
                lexer_.current().kind == TokenKind::kNumber,
            "expected an interval amount");
      const int64_t amount = std::stoll(lexer_.current().text);
      lexer_.Advance();
      Check(lexer_.current().kind == TokenKind::kIdentifier, "expected an interval unit");
      const std::string unit = lexer_.current().lower;
      lexer_.Advance();
      const int64_t signed_amount = minus ? -amount : amount;
      if (unit == "day" || unit == "days") {
        days += static_cast<int32_t>(signed_amount);
      } else if (unit == "month" || unit == "months" || unit == "year" || unit == "years") {
        int year = 0;
        unsigned month = 0;
        unsigned day = 0;
        CivilFromDays(days, &year, &month, &day);
        int64_t total = static_cast<int64_t>(year) * 12 + static_cast<int64_t>(month) - 1;
        total += (unit == "year" || unit == "years") ? signed_amount * 12 : signed_amount;
        year = static_cast<int>(total / 12);
        month = static_cast<unsigned>(total % 12) + 1;
        days = DaysFromCivil(year, month, day);
      } else {
        Fail("unsupported interval unit: " + unit);
      }
    }
  }

  ExprPtr ParseFunction(const std::string& name) {
    Check(Symbol("("), "expected (");
    ExprPtr call = std::make_shared<Expr>();
    call->kind = ExprKind::kAggregate;
    if (name == "count") {
      call->aggregate = static_cast<uint8_t>(AggregateKind::kCount);
    } else if (name == "sum") {
      call->aggregate = static_cast<uint8_t>(AggregateKind::kSum);
    } else if (name == "avg") {
      call->aggregate = static_cast<uint8_t>(AggregateKind::kAvg);
    } else if (name == "min") {
      call->aggregate = static_cast<uint8_t>(AggregateKind::kMin);
    } else if (name == "max") {
      call->aggregate = static_cast<uint8_t>(AggregateKind::kMax);
    } else {
      Fail("unsupported function: " + name);
    }
    if (PeekSymbol("*")) {
      lexer_.Advance();
      Check(name == "count", "only count supports *");
      call->aggregate = static_cast<uint8_t>(AggregateKind::kCountStar);
      call->star = true;
    } else {
      Check(!IsKeyword(lexer_.current(), "distinct"), "distinct aggregates are not supported");
      call->children.push_back(ParseExpression());
    }
    Check(Symbol(")"), "expected ) after function arguments");
    return call;
  }

  Lexer lexer_;
};

}

SelectStatement ParseSelect(const std::string& sql) {
  Parser parser(sql);
  return parser.ParseStatement();
}

}
