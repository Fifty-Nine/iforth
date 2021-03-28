#include <cctype>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <fstream>
#include <regex>
#include <string>
#include <unistd.h>

std::string forthString = R"(
  : FIB 2 < IF ELSE 1 - DUP 1 - THEN ;
  : HELLO "hello world\n" .s ;
  HELLO 1 2 + .
)";


enum class tokens {
  start_definition,
  end_definition,
  print,
  identifier,
  number,
  string,
  last_token = string
};
using token_kind = tokens;
constexpr size_t num_token_kinds = (size_t)tokens::last_token + 1;

struct machine_state;
struct token;
class token_opt;

using interp_fn = std::function<void(machine_state&, const token&)>;
using lex_fn = std::function<token_opt(const char*, const char*)>;
using token_iterator = std::vector<token>::const_iterator;

/*
 * Represents a lexed token from the input stream.
 */
struct token {
  token_kind kind;
  const char *start;
  const char *end;
  interp_fn interpret;

  std::string to_string() const
  {
    return std::string { start, end };
  }

  friend std::ostream& operator<<(std::ostream& out, const token& t)
  {
    return (out << t.to_string());
  }
};

/*
 * Represents an optional token. In C++17 this would just be
 * std::optional<token> but Codility only supports C++14.
 */
class token_opt
{
public:
  token_opt(token token_) :
    init { true }, token_ { token_ }
  { }

  token_opt() { }

  const token& operator*() const
  {
    if (!init) { abort(); }
    return token_;
  }

  const token* operator->() const
  {
    return init ? &token_ : NULL;
  }

  explicit operator bool() const
  {
    return init;
  }

private:
  bool init = false;
  token token_ { };
};

void noop(machine_state& m, const token& t);
lex_fn lexChar(token_kind kind, char c, interp_fn interp = &noop)
{
  return [=](const char *text, const char *end)
  {
    return (text != end && *text == c) ?
      token { kind, text, text + 1, interp } :
      token_opt { };
  };
}

lex_fn lexRegex(
    token_kind kind, const std::string& re_text, interp_fn interp = &noop)
{
  return [
    re = std::regex { "^" + re_text },
    kind,
    interp
  ](const char *begin, const char *end) -> token_opt
  {
    std::cmatch m;
    if (std::regex_search(begin, end, m, re)) {
      return token { kind, begin, begin + m.length(), interp };
    }
    return token_opt { };
  };
}

std::string toLower(std::string s)
{
  std::transform(
    s.begin(), s.end(), s.end(),
    [](unsigned char c) -> unsigned char
    {
      if (c >= 'A' && c <= 'Z') return c - ('A' - 'a');
      return c;
    }
  );
  return s;
}

bool isTokenWithId(const std::string& id, const token& tok)
{
  if (tok.kind != tokens::identifier) {
    return false;
  }

  return toLower(tok.to_string()) == id;
}


struct machine_state
{
  machine_state(std::vector<token> tokens) :
    token_stream { std::move(tokens) },
    curr_token { token_stream.begin() }
  { }

  void push(int n)
  {
    dstack.push_back(n);
  }

  static void print_stack(std::ostream& out, const std::deque<int>& s)
  {
    out << "[";
    for (size_t i = 0; i < s.size(); ++i) {
      auto idx = s.size() - i - 1;
      out << idx << ":" << s[i] << (idx == 0 ? "" : " ");
    }
    out << "]\n";
  }

  void debug(std::ostream& out) const
  {
    out << "========= machine state =========\n";
    out << "token stream:\n";
    for (auto i = 0; i < (int)token_stream.size(); ++i) {
      out << i << ":[" << token_stream[i] << "] ";
    }
    out << "\n\ndata stack:\n";
    print_stack(out, dstack);
    out << "\nreturn stack:\n";
    print_stack(out, rstack);
    out << "\nip: " << ip() << " "
        << (atEnd() ? std::string { "\n"}
                    : ("(" + curr_token->to_string() + ")\n"));
    out << "=================================";
    out << std::endl;
  }

  struct error_state
  {
    error_state() : m { nullptr } { }
    error_state(const machine_state& m) : m { &m } { }
    error_state(error_state&& es) :
      m { es.m }, ss { std::move(es.ss) }
    {
      es.m = nullptr;
    }
    ~error_state() {
      if (m) {
        ss << "\n";
        m->debug(ss);
        std::cerr << ss.str() << std::endl;
        exit(1);
      }
    }

    const machine_state *m;
    std::stringstream ss;

    template<class T>
    error_state& operator<<(T&& op)
    {
      ss << std::forward<T&&>(op);
      return *this;
    }

    error_state& operator<<(std::basic_ostream<char>& (*fn)(std::basic_ostream<char>&)) {
      ss << fn;
      return *this;
    }
  };

  error_state error() const
  {
    error_state es { *this };
    es << "error interpreting token " << *curr_token << ": ";
    return es;
  }

  error_state assert(bool val) const
  {
    if (!val) {
      error_state es { *this };
      es << "assertion while interpreting token " << *curr_token << ": ";
      return es;
    }
    return error_state { };
  }

  int pop()
  {
    if (dstack.empty()) {
      error() << "tried to pop from empty stack";
    }
    auto result = dstack.back();
    dstack.pop_back();
    return result;
  }

  bool pop(int& out)
  {
    if (dstack.empty()) {
      return false;
    }
    out = pop();
    return true;
  }

  int rpop()
  {
    if (rstack.empty()) {
      error() << "tried to pop from empty return stack";
    }
    auto result = rstack.back();
    rstack.pop_back();
    return result;
  }

  bool rpop(int& out)
  {
    if (rstack.empty()) {
      return false;
    }
    out = rpop();
    return true;
  }

  void rpush(int v) {
    rstack.push_back(v);
  }

  void rpush(token_iterator it) {
    rpush(addr(it));
  }

  void rpush() {
    rpush(ip());
  }

  int addr(token_iterator it) const {
    return it - token_stream.begin();
  }

  int ip() const
  {
    return addr(curr_token);
  }

  int end_addr() const
  {
    return (int)token_stream.size();
  }

  token_iterator abs_inst(int addr)
  {
    return
      (addr >= 0 && addr < end_addr()) ? token_stream.begin() + addr :
      (addr < 0)                       ? token_stream.begin()        :
                                         token_stream.end();

  }

  token_iterator rel_inst(int off)
  {
    return abs_inst(ip() + off);
  }

  int top()
  {
    if (dstack.empty()) {
      error() << "tried to peek empty stack";
    }
    return dstack.back();
  }

  void rbranch(int off)
  {
    curr_token = rel_inst(off);
  }

  void abranch(int addr)
  {
    curr_token = abs_inst(addr);
  }

  bool branchTo(std::function<bool(const token&)> pred) {
    while (!atEnd() && !pred(*curr_token)) { next(); }
    return !atEnd() && pred(*curr_token);
  }

  template<class... T>
  bool branchTo(const char *id, T&&... args)
  {
    std::vector<const char*> ids { id, std::forward<T&&>(args)... };
    return branchTo([ids = std::move(ids)](const token& t) {
        for (auto id : ids) {
          if (isTokenWithId(id, t)) {
            return true;
          }
        }
        return false;
      }
    );
  }

  bool atEnd() const {
    return curr_token == token_stream.end();
  }

  void next() { rbranch(1); }

  int run()
  {
    while (!atEnd()) {
      curr_token->interpret(*this, *curr_token);
    }
    return dstack.empty() ? 0 : dstack.back();
  }

  bool intrinsic(const std::string& id);

  std::map<std::string, token_iterator> dictionary;
  std::deque<int> dstack;
  std::deque<int> rstack;
  std::vector<token> token_stream;
  token_iterator curr_token;
};

std::map<std::string, void(*)(machine_state&)> intrinsics {
  {
    "dup",
    [](machine_state& m) {
      m.push(m.top());
      m.next();
    },
  },
  {
    "swap",
    [](machine_state& m) {
      auto b = m.pop();
      auto a = m.pop();
      m.push(b);
      m.push(a);
      m.next();
    }
  },
  {
    "over",
    [](machine_state& m) {
      auto b = m.pop();
      auto a = m.pop();
      m.push(a);
      m.push(b);
      m.push(a);
      m.next();
    }
  },
  {
    "rot",
    [](machine_state& m) {
      auto c = m.pop();
      auto b = m.pop();
      auto a = m.pop();
      m.push(b);
      m.push(c);
      m.push(a);
      m.next();
    }
  },
  {
    "drop",
    [](machine_state& m) {
      (void)m.pop();
      m.next();
    }
  },
  {
    "clear",
    [](machine_state& m) {
      m.dstack.clear();
      m.next();
    }
  },
  {
    "if",
    [](machine_state& m) {
      if (!m.pop()) {
        if (!m.branchTo("else", "then")) {
          m.error() << "'if' with no corresponding 'then'";
        }
      }
      m.next();
    }
  },
  {
    "else",
    [](machine_state& m) {
      if (!m.branchTo("then")) {
        m.error() << "'else' with no corresponding 'then'";
      }
      m.next();
    }
  },
  {
    "then",
    [](machine_state& m) { m.next(); }
  },
};

bool machine_state::intrinsic(const std::string& id)
{
  auto it = intrinsics.find(toLower(id));
  if (it != intrinsics.end()) {
    it->second(*this);
    return true;
  }
  return false;
}

void noop(machine_state& m, const token& tok)
{
  m.next();
}

bool isOperation(const char *begin, const char *end)
{
  static std::regex opRegex { R"(([-+*/%&|!=]|<>|<(=)?|>(=)?))" };
  return std::regex_match(begin, end, opRegex);
}

void interpOperation(machine_state& m, const token& tok)
{
  if (*tok.start == '!') {
    m.push(!m.pop());
  } else {
    auto r = m.pop();
    auto l = m.pop();

    switch (*tok.start)
    {
    case '+': m.push(l + r); break;
    case '-': m.push(l - r); break;
    case '*': m.push(l * r); break;
    case '/': m.push(l / r); break;
    case '%': m.push(l % r); break;
    case '&': m.push(l && r); break;
    case '|': m.push(l || r); break;
    case '<': {
        if (tok.end == tok.start + 1) {
          m.push(l < r);
        } else {
          if (*(tok.start + 1) == '=') {
            m.push(l <= r);
          } else if (*(tok.start + 1) == '>') {
            m.push(l != r);
          } else {
            m.error() << "malformed binary operator beginning with '<'";
          }
        }
      }
      break;
    case '>': {
        if (tok.end == tok.start + 1) {
          m.push(l > r);
        } else {
          m.assert(*(tok.start + 1) == '=')
            << "malformed binary operator beginning with '>'";
          m.push(l >= r);
        }
      }
      break;
    case '=': m.push(l == r); break;
    }
  }
  m.next();
}

void interpString(machine_state& m, const char *start, const char *end)
{
  m.push(0);
  for (auto it = end - 2; it != start; --it)
  {
    if (*it == '\\') {
      int c;
      if (m.pop(c)) {
        switch(c) {
        case 'n': m.push('\n'); break;
        case 'r': m.push('\r'); break;
        case '"': m.push('"'); break;
        case '\\': m.push('\\'); break;
        case 't': m.push('\t'); break;
        default: break;
        }
      } else {
        m.push('\\');
      }
    } else {
      m.push((int)*it);
    }
  }
}

lex_fn token_table[] {
  lexChar(
    tokens::start_definition,
    ':',
    [](machine_state& m, const token& tok)
    {
      m.next();
      if (m.atEnd() || m.curr_token->kind != tokens::identifier) {
        m.error() << "expecting identifier";
      }
      std::string id { m.curr_token->start, m.curr_token->end };
      m.next();
      auto start = m.curr_token;

      while (!m.atEnd() && m.curr_token->kind != tokens::end_definition) {
        m.next();
      }

      if (m.curr_token->kind != tokens::end_definition) {
        m.error() << "expecting ':'";
      }

      m.next();
      m.dictionary[id] = start;
    }
  ),
  lexChar(
    tokens::end_definition,
    ';',
    [](machine_state& m, const token& tok)
    {
      int rip;
      if (!m.rpop(rip)) {
        m.error() << "encountered end-of-definition with no corresponding "
                  << "start-of-definition.";
      }
      m.abranch(rip);
    }
  ),
  lexRegex(
    tokens::print,
    R"(\.([sd]|"[^"]*")?)",
    [](machine_state& m, const token& tok)
    {
      if ((tok.end - tok.start) > 1) {
        if (*(tok.start + 1) == '"') {
          interpString(m, tok.start + 1, tok.end);
        } else if (*(tok.start + 1) == 'd') {
          m.debug(std::cout);
          m.next();
          return;
        }
        while (true) {
          int c;
          if (m.pop(c)) {
            if (c == 0) {
              break;
            } else {
              std::cout << (char)c;
            }
          } else {
            m.error() << "no null terminator found before end of stack reached";
          }
        }
      } else {
        std::cout << m.pop() << std::endl;
      }
      m.next();
    }
  ),
  lexRegex(
    tokens::number,
    R"((-)?(0[xX][0-9a-fA-F]+|0[0-7]*|[1-9][0-9]*))",
    [](machine_state& m, const token& tok)
    {
      m.push(strtol(tok.start, nullptr, 0));
      m.next();
    }
  ),
  lexRegex(
    tokens::string,
    R"("[^"]*")",
    [](machine_state& m, const token& tok)
    {
      interpString(m, tok.start, tok.end);
      m.next();
    }
  ),
  lexRegex(
    tokens::identifier,
    R"([^\s]+)",
    [](machine_state& m, const token& tok)
    {
      if (isOperation(tok.start, tok.end)) {
        interpOperation(m, tok);
        return;
      }
      auto it = m.dictionary.find(tok.to_string());
      if (it == m.dictionary.end()) {
        if (m.intrinsic(tok.to_string())) {
          return;
        }
        m.error() << "no word named " << tok.to_string() << " in dictionary.";
      }

      m.next();
      m.rpush();
      m.curr_token = it->second;
    }
  ),
};
static_assert(sizeof(token_table) / sizeof(lex_fn) == num_token_kinds);

const char *skipWs(const char *c, const char *end)
{
  while (c && c != end && std::isspace(*c)) { ++c; }
  return c;
}

token_opt lexToken(const char *input, const char *end)
{
  for (auto fn : token_table)
  {
    token_opt t;
    if ((t = fn(input, end))) {
      return t;
    }
  }
  return { };
}

std::vector<token> lexTokens(const char *input, const char *end)
{
  std::vector<token> tokens;
  for (const char *it = skipWs(input, end); it != end; it = skipWs(it, end))
  {
    auto t = lexToken(it, end);
    if (!t) {
      const char *tokEnd = it;
      while (tokEnd != end && !std::isspace(*tokEnd)) ++tokEnd;
      std::cerr << "error at position " << (std::ptrdiff_t)(it - input)
                << ": unrecognized token " << std::string { it, tokEnd }
                << std::endl;
      exit(1);
    }
    tokens.push_back(*t);
    it = t->end;
  }
  return tokens;
}

void readFile(std::istream& is, std::string& out)
{
  std::stringstream ss;
  ss << is.rdbuf();
  out += ss.str();
}

int main(int argc, char *const argv[])
{
  std::string text;
  if (argc > 1) {
    for (int n = 1; n < argc; ++n) {
      if (std::string { argv[n]} == "-") {
        readFile(std::cin, text);
      } else {
        auto is = std::ifstream { argv[n], std::ios::binary };
        if (!is) {
          std::cerr << "couldn't open file " << argv[n] << std::endl;
          exit(1);
        }
        readFile(is, text);
      }
    }
  } else {
    text = forthString;
  }
  machine_state m {
    lexTokens(text.data(), text.data() + text.size())
  };
  return m.run();
}
