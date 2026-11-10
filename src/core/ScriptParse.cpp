// Mod scripts: lexer, parser and printer (Script.h, doc/scripts.md).

#include "Script.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace attrib::script
{

	namespace fs = std::filesystem;

	namespace
	{

		std::string Located(const std::string& source, Pos p, const std::string& message)
		{
			std::string s = source.empty() ? std::string("script") : source;
			if (p.line) s += ":" + std::to_string(p.line) + ":" + std::to_string(p.col);
			return s + ": " + message;
		}

	} // namespace

	ScriptError::ScriptError(Pos p, const std::string& m, const std::string& source)
		: std::runtime_error(Located(source, p, m)), pos(p), message(m)
	{
	}

	ScriptError::ScriptError(Pos p, const std::string& m, const std::string& f, Pos fp)
		: std::runtime_error(Located(f, fp, m)), pos(p), message(m), file(f), filePos(fp)
	{
	}

	Message Message::Error(const ScriptError& e)
	{
		if (e.file.empty()) return Message(Level::Error, e.pos, e.message);
		return Message(Level::Error, e.pos, e.message, e.file, e.filePos.line);
	}

	ValuePtr MakeNumber(std::string text)
	{
		auto v = std::make_shared<Value>();
		v->kind = Value::Kind::Number;
		v->text = std::move(text);
		return v;
	}
	ValuePtr MakeString(std::string text)
	{
		auto v = std::make_shared<Value>();
		v->kind = Value::Kind::String;
		v->text = std::move(text);
		return v;
	}
	ValuePtr MakeWord(std::string text)
	{
		auto v = std::make_shared<Value>();
		v->kind = Value::Kind::Word;
		v->text = std::move(text);
		return v;
	}
	ValuePtr MakeList(std::vector<ValuePtr> items, Value::Kind kind)
	{
		auto v = std::make_shared<Value>();
		v->kind = kind;
		v->items = std::move(items);
		return v;
	}

	const char* OptionTypeName(OptionDef::Type t)
	{
		switch (t)
		{
			case OptionDef::Type::Bool: return "bool";
			case OptionDef::Type::Number: return "number";
			case OptionDef::Type::Integer: return "integer";
			case OptionDef::Type::Choice: return "choice";
			case OptionDef::Type::Text: return "text";
		}
		return "?";
	}

	std::string Script::meta(const std::string& key) const
	{
		for (const Statement& s : statements)
			if (s.kind == Statement::Kind::Meta && s.word == key) return s.text;
		return {};
	}

	void Script::setMeta(const std::string& key, const std::string& value)
	{
		for (Statement& s : statements)
			if (s.kind == Statement::Kind::Meta && s.word == key)
			{
				s.text = value;
				return;
			}
		// After the header and the other meta lines.
		size_t at = 0;
		while (at < statements.size() &&
			   (statements[at].kind == Statement::Kind::Header || statements[at].kind == Statement::Kind::Meta))
			++at;
		Statement s;
		s.kind = Statement::Kind::Meta;
		s.word = key;
		s.text = value;
		statements.insert(statements.begin() + std::ptrdiff_t(at), std::move(s));
	}

	std::vector<const OptionDef*> Script::options() const
	{
		std::vector<const OptionDef*> r;
		// A script included twice is read once, so its options come once.
		auto add = [&](const OptionDef* o)
		{
			if (std::find(r.begin(), r.end(), o) == r.end()) r.push_back(o);
		};
		std::function<void(const std::vector<Statement>&, bool)> walk = [&](const std::vector<Statement>& list, bool top)
		{
			for (const Statement& s : list)
			{
				if (s.kind == Statement::Kind::Option && top)
				{
					add(&s.option);
				}
				else if (s.kind == Statement::Kind::If)
				{
					walk(s.body, false);
					walk(s.elseBody, false);
				}
				else if (s.kind == Statement::Kind::Include && s.included)
				{
					for (const OptionDef* o : s.included->options()) add(o);
				}
			}
		};
		walk(statements, true);
		return r;
	}

	std::string GameId(const Database& db)
	{
		return db.target().id();
	}

	// ------------------------------------------------------------------ lexer ---

	namespace
	{

		struct Token
		{
			enum class T : uint8_t
			{
				End, Newline, Word, String, Option, Punct, Comment
			} t = T::End;
			std::string text;
			Pos pos;
			bool spaceBefore = true; // whitespace or a line start before it
		};

		// Characters that end a word. '+', '-', '*', '.', ':' and the like are part
		// of words, so 911gt3, 1.5e-3, race_bin-1 and 0x1F are one word each and
		// operators need spaces around them.
		bool Breaks(char c)
		{
			return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '"' || c == '#' || c == '{' || c == '}' || c == '[' ||
				c == ']' || c == '(' || c == ')' || c == ',' || c == ';' || c == '=' || c == '/' || c == '$';
		}

		std::vector<Token> Lex(std::string_view s, const std::string& source)
		{
			std::vector<Token> out;
			int line = 1;
			size_t lineStart = 0;
			bool space = true;
			auto pos = [&](size_t i)
			{
				return Pos{ line, int(i - lineStart) + 1 };
			};
			for (size_t i = 0; i < s.size();)
			{
				const char c = s[i];
				if (c == ' ' || c == '\t' || c == '\r')
				{
					++i;
					space = true;
					continue;
				}
				Token t;
				t.pos = pos(i);
				t.spaceBefore = space;
				space = false;
				if (c == '\n')
				{
					t.t = Token::T::Newline;
					out.push_back(t);
					++i;
					++line;
					lineStart = i;
					space = true;
					continue;
				}
				if (c == '#')
				{
					size_t e = s.find('\n', i);
					if (e == std::string_view::npos) e = s.size();
					size_t end = e;
					while (end > i + 1 && (s[end - 1] == '\r' || s[end - 1] == ' ' || s[end - 1] == '\t')) --end;
					t.t = Token::T::Comment;
					t.text = std::string(s.substr(i + 1, end - i - 1));
					out.push_back(t);
					i = e;
					continue;
				}
				if (c == '"')
				{
					t.t = Token::T::String;
					if (s.substr(i, 3) == "\"\"\"")
					{
						// Raw, may span lines: """...""". A newline right after the
						// opening quotes isn't part of it.
						size_t b = i + 3;
						if (b < s.size() && s[b] == '\r') ++b;
						if (b < s.size() && s[b] == '\n') ++b;
						const size_t e = s.find("\"\"\"", b);
						if (e == std::string_view::npos) throw ScriptError(t.pos, "unterminated \"\"\" string", source);
						t.text = std::string(s.substr(b, e - b));
						// The closing quotes may be indented: a last line of only
						// spaces isn't part of it.
						const size_t lastNl = t.text.find_last_of('\n');
						if (lastNl != std::string::npos && t.text.find_first_not_of(" \t", lastNl + 1) == std::string::npos)
							t.text.resize(lastNl + 1);
						for (size_t k = i; k < e; ++k)
							if (s[k] == '\n')
							{
								++line;
								lineStart = k + 1;
							}
						i = e + 3;
						out.push_back(t);
						continue;
					}
					++i;
					for (;;)
					{
						if (i >= s.size() || s[i] == '\n') throw ScriptError(t.pos, "unterminated string", source);
						const char d = s[i++];
						if (d == '"') break;
						if (d != '\\')
						{
							t.text += d;
							continue;
						}
						if (i >= s.size()) throw ScriptError(t.pos, "unterminated string", source);
						const char e = s[i++];
						switch (e)
						{
							case 'n': t.text += '\n'; break;
							case 't': t.text += '\t'; break;
							case 'r': t.text += '\r'; break;
							case '0': t.text += '\0'; break;
							case '\\': t.text += '\\'; break;
							case '"': t.text += '"'; break;
							case 'x':
							{
								if (i + 2 > s.size()) throw ScriptError(pos(i), "\\x needs two hex digits", source);
								const std::string hex(s.substr(i, 2));
								char* end = nullptr;
								const long v = std::strtol(hex.c_str(), &end, 16);
								if (!end || *end) throw ScriptError(pos(i), "\\x needs two hex digits", source);
								t.text += char(v);
								i += 2;
								break;
							}
							default: throw ScriptError(pos(i - 1), std::string("unknown escape \\") + e, source);
						}
					}
					out.push_back(t);
					continue;
				}
				if (c == '$')
				{
					size_t e = i + 1;
					while (e < s.size() && (std::isalnum(static_cast<unsigned char>(s[e])) || s[e] == '_')) ++e;
					if (e == i + 1) throw ScriptError(t.pos, "$ needs an option name", source);
					t.t = Token::T::Option;
					t.text = std::string(s.substr(i + 1, e - i - 1));
					out.push_back(t);
					i = e;
					continue;
				}
				const char n = i + 1 < s.size() ? s[i + 1] : 0;
				if (std::strchr("{}[](),;", c))
				{
					t.t = Token::T::Punct;
					t.text = std::string(1, c);
					out.push_back(t);
					++i;
					continue;
				}
				if (c == '=' || c == '/' || ((c == '+' || c == '-' || c == '*' || c == '!' || c == '<' || c == '>') && n == '='))
				{
					t.t = Token::T::Punct;
					t.text = n == '=' ? std::string{ c, '=' } : std::string(1, c);
					out.push_back(t);
					i += t.text.size();
					continue;
				}
				size_t e = i;
				while (e < s.size() && !Breaks(s[e]))
				{
					if ((s[e] == '+' || s[e] == '-' || s[e] == '*' || s[e] == '!' || s[e] == '<' || s[e] == '>') && e + 1 < s.size() && s[e + 1] == '=') break;
					++e;
				}
				t.t = Token::T::Word;
				t.text = std::string(s.substr(i, e - i));
				out.push_back(t);
				i = e;
			}
			Token end;
			end.pos = Pos{ line, int(s.size() - lineStart) + 1 };
			out.push_back(end);
			return out;
		}

		bool IsHexKey(std::string_view s)
		{
			if (s.size() < 3 || s.size() > 10 || s[0] != '0' || (s[1] != 'x' && s[1] != 'X')) return false;
			for (size_t i = 2; i < s.size(); ++i)
				if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
			return true;
		}

		// A decimal number (1, -2, 1.5, .5, 1e-3, inf, nan) or 0x hex.
		bool LooksNumeric(std::string_view s)
		{
			if (IsHexKey(s)) return true;
			std::string_view t = s;
			if (!t.empty() && (t[0] == '-' || t[0] == '+')) t.remove_prefix(1);
			if (t == "inf" || t == "nan") return true;
			if (t.empty()) return false;
			size_t i = 0;
			bool digits = false;
			while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i]))) ++i, digits = true;
			if (i < t.size() && t[i] == '.')
			{
				++i;
				while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i]))) ++i, digits = true;
			}
			if (!digits) return false;
			if (i < t.size() && (t[i] == 'e' || t[i] == 'E'))
			{
				++i;
				if (i < t.size() && (t[i] == '-' || t[i] == '+')) ++i;
				bool exp = false;
				while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i]))) ++i, exp = true;
				if (!exp) return false;
			}
			return i == t.size();
		}

		const char* const kReserved[] = { "true", "false", "null", "bytes", "lua", "was", "root", "and", "or", "not", "under", "in", "to", "from", "exact", "with", "echo" };

		bool IsReserved(std::string_view s)
		{
			for (const char* r : kReserved)
				if (s == r) return true;
			return false;
		}

		// Can `s` be written without quotes and read back as the same word?
		bool IsBare(std::string_view s)
		{
			if (s.empty() || s[0] == '.' || s[0] == '-' || s[0] == '+' || s[0] == '*') return false;
			for (size_t i = 0; i < s.size(); ++i)
			{
				const char c = s[i];
				if (Breaks(c) || c == '*' || c == '?' || static_cast<unsigned char>(c) < 0x21 || static_cast<unsigned char>(c) >= 0x7F)
					return false;
				if ((c == '+' || c == '-' || c == '!') && i + 1 < s.size() && s[i + 1] == '=') return false;
			}
			return !IsReserved(s);
		}

		// ----------------------------------------------------------------- parser ---

		class Parser
		{
		public:
			Parser(std::vector<Token> toks, std::string source) : t_(std::move(toks)), source_(std::move(source))
			{
			}

			Script script()
			{
				Script sc;
				sc.source = source_;
				sc.statements = statements(Context::Top, false);
				return sc;
			}

		private:
			enum class Context
			{
				Top, Collection, Class
			};
			// Parsing an `if` inside a collection's block: has, own, count and
			// fields may be tested.
			bool condInBlock_ = false;

			const Token& peek(size_t k = 0) const
			{
				return t_[std::min(i_ + k, t_.size() - 1)];
			}
			const Token& next()
			{
				return t_[std::min(i_++, t_.size() - 1)];
			}
			bool atWord(const char* w, size_t k = 0) const
			{
				return peek(k).t == Token::T::Word && peek(k).text == w;
			}
			bool atPunct(const char* p, size_t k = 0) const
			{
				return peek(k).t == Token::T::Punct && peek(k).text == p;
			}
			[[noreturn]] void fail(Pos p, const std::string& m) const
			{
				throw ScriptError(p, m, source_);
			}
			[[noreturn]] void failHere(const std::string& m) const
			{
				const Token& k = peek();
				std::string got = k.t == Token::T::End ? "the end"
					: k.t == Token::T::Newline ? "the end of the line"
					: k.t == Token::T::String ? "a string"
					: "'" + k.text + "'";
				fail(k.pos, m + ", found " + got);
			}
			void expectPunct(const char* p)
			{
				if (!atPunct(p)) failHere(std::string("expected '") + p + "'");
				next();
			}
			void expectWord(const char* w)
			{
				if (!atWord(w)) failHere(std::string("expected '") + w + "'");
				next();
			}
			void skipNewlines()
			{
				while (peek().t == Token::T::Newline || peek().t == Token::T::Comment || atPunct(";")) next();
			}

			// Statements up to '}' (inBlock) or the end.
			std::vector<Statement> statements(Context ctx, bool inBlock)
			{
				std::vector<Statement> out;
				for (;;)
				{
					const Token& k = peek();
					if (k.t == Token::T::End)
					{
						if (inBlock) failHere("expected '}'");
						return out;
					}
					if (k.t == Token::T::Newline || atPunct(";"))
					{
						next();
						continue;
					}
					if (atPunct("}"))
					{
						if (!inBlock) fail(k.pos, "'}' without a block to close");
						next();
						return out;
					}
					if (k.t == Token::T::Comment)
					{
						Statement s;
						s.kind = Statement::Kind::Comment;
						s.pos = k.pos;
						s.comment = k.text;
						next();
						out.push_back(std::move(s));
						continue;
					}
					out.push_back(ctx == Context::Top ? topStatement() : ctx == Context::Class ? classField() : blockStatement());
				}
			}

			// A comment after it, then a line end, ';', '}' or the end.
			void endStatement(Statement& s)
			{
				if (peek().t == Token::T::Comment) s.comment = next().text;
				if (peek().t == Token::T::Newline || atPunct(";"))
				{
					next();
					return;
				}
				if (peek().t == Token::T::End || atPunct("}")) return;
				failHere("expected the end of the statement");
			}

			std::vector<Statement> block(Context ctx)
			{
				skipNewlinesOnly();
				expectPunct("{");
				return statements(ctx, true);
			}
			void skipNewlinesOnly()
			{
				while (peek().t == Token::T::Newline) next();
			}

			std::string name(const char* what, bool* quoted = nullptr)
			{
				const Token& k = peek();
				if (k.t == Token::T::Word || k.t == Token::T::String)
				{
					if (quoted) *quoted = k.t == Token::T::String;
					if (k.t == Token::T::Word && IsReserved(k.text)) failHere(std::string("expected ") + what);
					return next().text;
				}
				failHere(std::string("expected ") + what);
			}

			Target target(bool allowRoot = false)
			{
				Target t;
				t.pos = peek().pos;
				if (allowRoot && atWord("root"))
				{
					next();
					t.root = true;
					return t;
				}
				bool q = false;
				t.cls = quotedName(name("a class/collection"), q);
				if (!atPunct("/")) failHere("expected '/' between the class and the collection");
				next();
				t.name = quotedName(name("a collection name"), q);
				while (atPunct("/"))
				{
					next();
					t.name += "/" + name("a collection name");
				}
				return t;
			}
			// A quoted 0x... is a name, not a hash: kept in quotes.
			static std::string quotedName(std::string n, bool quoted)
			{
				return quoted && IsHexKey(n) ? "\"" + n + "\"" : n;
			}

			FieldPath path()
			{
				FieldPath p;
				p.pos = peek().pos;
				const Token& first = peek();
				if (first.t == Token::T::String)
				{
					p.field = quotedName(next().text, true);
				}
				else if (first.t == Token::T::Word)
				{
					const std::string w = next().text;
					splitMembers(p, w, first.pos, true);
				}
				else
				{
					failHere("expected a field");
				}
				for (;;)
				{
					const Token& k = peek();
					if (k.spaceBefore) break;
					if (k.t == Token::T::Punct && k.text == "[")
					{
						next();
						const Token& n = peek();
						char* end = nullptr;
						const long long idx = n.t == Token::T::Word ? std::strtoll(n.text.c_str(), &end, 10) : 0;
						// [-1] is the last element (format 2).
						if (n.t != Token::T::Word || !end || *end || n.text.empty() || n.text == "-0") failHere("expected an index");
						next();
						expectPunct("]");
						p.steps.push_back({ std::string(), idx });
					}
					else if (k.t == Token::T::Word && k.text[0] == '.')
					{
						const std::string w = next().text;
						splitMembers(p, w.substr(1), k.pos, false);
					}
					else
					{
						break;
					}
				}
				return p;
			}
			void splitMembers(FieldPath& p, const std::string& w, Pos pos, bool first)
			{
				size_t b = 0;
				for (;;)
				{
					const size_t e = w.find('.', b);
					const std::string seg = w.substr(b, e == std::string::npos ? std::string::npos : e - b);
					if (seg.empty()) fail(pos, "empty name in " + w);
					if (first)
					{
						p.field = seg;
						first = false;
					}
					else
					{
						p.steps.push_back({ seg, -1 });
					}
					if (e == std::string::npos) break;
					b = e + 1;
				}
			}

			Statement topStatement()
			{
				Statement s;
				const Token& k = peek();
				s.pos = k.pos;
				if (k.t != Token::T::Word) failHere("expected a statement");
				const std::string w = k.text;
				using K = Statement::Kind;
				if (w == "attribulina")
				{
					next();
					s.kind = K::Header;
					const Token& n = peek();
					if (n.t != Token::T::Word || !LooksNumeric(n.text)) failHere("expected the format version");
					s.number = std::atoi(next().text.c_str());
					if (s.number < 1) fail(n.pos, "bad format version");
					if (s.number > kVersion)
						fail(n.pos, "this script needs a newer Attribulina (format " + std::to_string(s.number) + ", this one reads " +
							 std::to_string(kVersion) + ")");
				}
				else if (w == "game" || w == "title" || w == "author" || w == "version" || w == "description" || w == "url")
				{
					next();
					s.kind = K::Meta;
					s.word = w;
					const Token& n = peek();
					if (n.t != Token::T::Word && n.t != Token::T::String) failHere("expected text");
					s.text = next().text;
				}
				else if (w == "echo")
				{
					echo(s);
				}
				else if (w == "include")
				{
					next();
					s.kind = K::Include;
					s.text = includePath();
				}
				else if (w == "option")
				{
					next();
					s.kind = K::Option;
					option(s.option);
					s.option.pos = s.pos;
				}
				else if (w == "if")
				{
					return ifStatement(Context::Top);
				}
				else if (w == "edit")
				{
					next();
					s.kind = K::Edit;
					s.target = target();
					s.body = block(Context::Collection);
					return s;
				}
				else if (w == "create")
				{
					next();
					if (atWord("vault"))
					{
						next();
						s.kind = K::CreateVault;
						s.vault = name("a vault name");
						expectWord("in");
						s.word = name("a pack file name");
					}
					else
					{
						s.kind = K::Create;
						s.target = target();
						collectionClauses(s, true);
						if (opensBlock())
						{
							s.body = block(Context::Collection);
							return s;
						}
					}
				}
				else if (w == "collection")
				{
					next();
					s.kind = K::Collection;
					s.target = target();
					collectionClauses(s, false);
					if (opensBlock())
					{
						s.body = block(Context::Collection);
						return s;
					}
				}
				else if (w == "delete")
				{
					next();
					if (atWord("vault"))
					{
						next();
						s.kind = K::DeleteVault;
						s.vault = name("a vault name");
					}
					else if (atWord("class") && !atPunct("/", 1))
					{
						next();
						s.kind = K::DeleteClass;
						s.word = className();
					}
					else
					{
						s.kind = K::Delete;
						s.target = target();
						if (atWord("with"))
						{
							next();
							expectWord("children");
							s.withChildren = true;
						}
					}
				}
				else if (w == "class" && !atPunct("/", 1))
				{
					next();
					s.kind = K::Class;
					s.word = className();
					for (;;)
					{
						if (atWord("in") && s.vault.empty())
						{
							next();
							expectWord("vault");
							s.vault = name("a vault name");
						}
						else if (peek().t == Token::T::Word && IsClassSetting(peek().text))
						{
							const std::string key = next().text;
							s.settings.emplace_back(key, integer(key.c_str()));
						}
						else
						{
							break;
						}
					}
					s.body = block(Context::Class);
					return s;
				}
				else if (w == "rename")
				{
					next();
					if (atWord("vault") && !atPunct("/", 1))
					{
						next();
						s.kind = K::RenameVault;
						s.vault = name("a vault name");
						expectWord("to");
						s.word = name("the new name");
					}
					else
					{
						s.kind = K::Rename;
						s.target = target();
						expectWord("to");
						s.word = name("the new name");
					}
				}
				else if (w == "move")
				{
					next();
					s.target = target();
					if (atWord("under"))
					{
						next();
						s.kind = K::Move;
						s.under = target(true);
						s.hasUnder = true;
					}
					else if (atWord("to"))
					{
						next();
						expectWord("vault");
						s.kind = K::MoveToVault;
						s.vault = name("a vault name");
					}
					else
					{
						failHere("expected 'under' or 'to vault'");
					}
				}
				else
				{
					fail(k.pos, "unknown statement '" + w + "' (values go in an edit block)");
				}
				endStatement(s);
				return s;
			}

			// A class's name; a quoted 0x... is a name, not a hash.
			std::string className()
			{
				bool q = false;
				return quotedName(name("a class name", &q), q);
			}

			static bool IsClassSetting(const std::string& w)
			{
				return w == "reserve" || w == "layoutsize" || w == "layoutcount" || w == "keyshift" || w == "staticsize";
			}
			static bool IsFieldSetting(const std::string& w)
			{
				return w == "at" || w == "align" || w == "flags" || w == "max" || w == "size";
			}

			// A whole number (0x... too) for `what`.
			int64_t integer(const char* what)
			{
				const Token& n = peek();
				char* end = nullptr;
				const long long v = n.t == Token::T::Word ? std::strtoll(n.text.c_str(), &end, 0) : -1;
				if (n.t != Token::T::Word || n.text.empty() || !end || *end || v < 0) failHere(std::string("expected a number after ") + what);
				next();
				return v;
			}

			// layout|entry|static NAME TYPE[[count]] [unsearchable] [at n] [align n]
			// [flags n] [max n] [size n] [= value]
			Statement classField()
			{
				Statement s;
				s.kind = Statement::Kind::ClassField;
				s.pos = peek().pos;
				if (atWord("bytes"))
				{
					// The static block as stored, for bytes no static field covers.
					s.word = "bytes";
					s.value = value();
					endStatement(s);
					return s;
				}
				if (!atWord("layout") && !atWord("entry") && !atWord("static"))
					failHere("expected a field: layout, entry or static, its name and its type");
				s.word = next().text;
				s.path.pos = peek().pos;
				bool q = false;
				s.path.field = quotedName(name("the field's name", &q), q);
				const Token& ty = peek();
				if (ty.t != Token::T::Word && ty.t != Token::T::String) failHere("expected the field's type");
				s.text = next().text;
				if (atPunct("[") && !peek().spaceBefore)
				{
					next();
					s.isArray = true;
					if (!atPunct("]")) s.count = uint32_t(integer("'['"));
					expectPunct("]");
				}
				for (;;)
				{
					if (atWord("unsearchable") && !s.unsearchable)
					{
						next();
						s.unsearchable = true;
					}
					else if (peek().t == Token::T::Word && IsFieldSetting(peek().text))
					{
						const std::string key = next().text;
						s.settings.emplace_back(key, integer(key.c_str()));
					}
					else
					{
						break;
					}
				}
				if (atPunct("="))
				{
					next();
					s.value = value();
				}
				endStatement(s);
				return s;
			}

			// A quoted path, or words joined by '/' (parts/kits.amod).
			std::string includePath()
			{
				std::string p;
				if (peek().t == Token::T::String)
				{
					p = next().text;
				}
				else
				{
					p = name("the path of a script");
					while (atPunct("/") && !peek().spaceBefore)
					{
						next();
						if (peek().t != Token::T::Word || peek().spaceBefore) failHere("expected the rest of the path");
						p += "/" + next().text;
					}
				}
				if (p.empty()) failHere("expected the path of a script");
				return p;
			}

			// `{` on this line or the next.
			bool opensBlock()
			{
				size_t k = 0;
				while (peek(k).t == Token::T::Newline) ++k;
				return atPunct("{", k);
			}

			void collectionClauses(Statement& s, bool allowFrom)
			{
				for (;;)
				{
					if (allowFrom && atWord("from") && !s.hasFrom)
					{
						next();
						s.from = target();
						s.hasFrom = true;
					}
					else if (atWord("under") && !s.hasUnder)
					{
						next();
						s.under = target(true);
						s.hasUnder = true;
					}
					else if (atWord("in") && s.vault.empty())
					{
						next();
						expectWord("vault");
						s.vault = name("a vault name");
					}
					else if (!allowFrom && atWord("exact") && !s.exact)
					{
						next();
						s.exact = true;
					}
					else
					{
						return;
					}
				}
			}

			void option(OptionDef& o)
			{
				const Token& id = peek();
				if (id.t != Token::T::Word || !IsBare(id.text) || std::isdigit(static_cast<unsigned char>(id.text[0])))
					failHere("expected an option name");
				for (char c : id.text)
					if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') fail(id.pos, "option names are letters, digits and _");
				o.id = next().text;
				const Token& ty = peek();
				if (ty.t != Token::T::Word) failHere("expected the option type (bool, number, integer, choice or text)");
				if (ty.text == "bool") o.type = OptionDef::Type::Bool;
				else if (ty.text == "number") o.type = OptionDef::Type::Number;
				else if (ty.text == "integer") o.type = OptionDef::Type::Integer;
				else if (ty.text == "choice") o.type = OptionDef::Type::Choice;
				else if (ty.text == "text") o.type = OptionDef::Type::Text;
				else failHere("expected the option type (bool, number, integer, choice or text)");
				next();
				if (peek().t == Token::T::String) o.label = next().text;
				for (;;)
				{
					if (atWord("default"))
					{
						next();
						o.defaultValue = value();
					}
					else if (atWord("min") || atWord("max"))
					{
						const bool isMin = peek().text == "min";
						next();
						const Token& n = peek();
						if (n.t != Token::T::Word || !LooksNumeric(n.text)) failHere("expected a number");
						(isMin ? o.min : o.max) = std::strtod(next().text.c_str(), nullptr);
					}
					else if (atWord("choices"))
					{
						next();
						const ValuePtr l = value();
						if (l->kind != Value::Kind::List) fail(l->pos, "choices are a list: [\"a\", \"b\"]");
						for (const ValuePtr& c : l->items)
						{
							if (c->kind != Value::Kind::String && c->kind != Value::Kind::Word && c->kind != Value::Kind::Number)
								fail(c->pos, "a choice is a word or a string");
							o.choices.push_back(c->text);
						}
					}
					else if (atWord("help"))
					{
						next();
						if (peek().t != Token::T::String) failHere("expected a string");
						o.help = next().text;
					}
					else
					{
						break;
					}
				}
				if (o.type == OptionDef::Type::Choice && o.choices.empty()) fail(id.pos, "a choice option needs choices [...]");
			}

			// echo [warning|error] values..., up to the end of the statement.
			void echo(Statement& s)
			{
				s.kind = Statement::Kind::Echo;
				next();
				auto ends = [&](size_t k)
				{
					const Token& t = peek(k);
					return t.t == Token::T::Newline || t.t == Token::T::End || t.t == Token::T::Comment ||
						(t.t == Token::T::Punct && (t.text == ";" || t.text == "}"));
				};
				if ((atWord("warning") || atWord("error")) && !ends(1)) s.word = next().text;
				std::vector<ValuePtr> items;
				while (!ends(0))
				{
					items.push_back(value());
					if (atPunct(",")) next();
				}
				s.value = MakeList(std::move(items));
			}

			Statement ifStatement(Context ctx)
			{
				Statement s;
				s.kind = Statement::Kind::If;
				s.pos = peek().pos;
				expectWord("if");
				const bool outer = condInBlock_;
				condInBlock_ = ctx == Context::Collection;
				s.cond = condition();
				condInBlock_ = outer;
				s.body = block(ctx);
				// `else` may follow the closing brace on the same line.
				if (atWord("else"))
				{
					next();
					if (atWord("if")) s.elseBody.push_back(ifStatement(ctx));
					else s.elseBody = block(ctx);
					return s;
				}
				Statement dummy;
				endStatement(dummy);
				s.comment = dummy.comment;
				return s;
			}

			// A condition goes on to the next line after and, or, not and '('.
			void conditionGoesOn()
			{
				while (peek().t == Token::T::Newline || peek().t == Token::T::Comment) next();
			}
			Condition condition()
			{
				Condition c = conjunction();
				while (atWord("or"))
				{
					next();
					conditionGoesOn();
					Condition o;
					o.kind = Condition::Kind::Or;
					o.items.push_back(std::move(c));
					o.items.push_back(conjunction());
					c = std::move(o);
				}
				return c;
			}
			Condition conjunction()
			{
				Condition c = negation();
				while (atWord("and"))
				{
					next();
					conditionGoesOn();
					Condition a;
					a.kind = Condition::Kind::And;
					a.items.push_back(std::move(c));
					a.items.push_back(negation());
					c = std::move(a);
				}
				return c;
			}
			Condition negation()
			{
				if (atWord("not"))
				{
					next();
					conditionGoesOn();
					Condition n;
					n.kind = Condition::Kind::Not;
					n.items.push_back(negation());
					return n;
				}
				if (atPunct("("))
				{
					next();
					conditionGoesOn();
					Condition c = condition();
					conditionGoesOn();
					expectPunct(")");
					return c;
				}
				Condition c;
				const auto named = [&](int at)
				{
					return peek(at).t == Token::T::Word || peek(at).t == Token::T::String;
				};
				if (atWord("exists") && named(1))
				{
					next();
					if (atWord("class") && named(1))
					{
						next();
						c.kind = Condition::Kind::ExistsClass;
						c.word = nameText(next());
					}
					else if (atWord("vault") && named(1))
					{
						next();
						c.kind = Condition::Kind::ExistsVault;
						c.word = nameText(next());
					}
					else if (atWord("field") && named(1) && named(2))
					{
						next();
						c.kind = Condition::Kind::ExistsField;
						c.word = nameText(next());
						c.name = nameText(next());
					}
					else
					{
						c.kind = Condition::Kind::ExistsCollection;
						c.target = target();
					}
					return c;
				}
				const bool keyword = named(1) && !comparisonAt(1);
				if (condInBlock_ && atWord("has") && keyword)
				{
					next();
					c.kind = Condition::Kind::Has;
					c.path = path();
					c.hasPath = true;
					return c;
				}
				if (condInBlock_ && atWord("own") && keyword)
				{
					next();
					c.kind = Condition::Kind::Own;
					c.path = path();
					c.hasPath = true;
					if (!c.path.steps.empty()) fail(c.path.pos, "own takes a field (a value is own as a whole)");
					return c;
				}
				if (peek().t == Token::T::Option)
				{
					c.option = next().text;
				}
				else if (condInBlock_ && (peek().t == Token::T::Word || peek().t == Token::T::String))
				{
					if (atWord("count") && keyword)
					{
						next();
						c.count = true;
					}
					c.path = path();
					c.hasPath = true;
					if (!comparisonAt(0)) failHere(c.count ? "expected ==, <, > ... after count " + c.path.text()
														   : "expected ==, !=, <, >, <=, >= or contains after " + c.path.text());
				}
				else
				{
					failHere(condInBlock_ ? "expected $option, exists, has, own, count or a field in the condition"
										  : "expected $option or exists in the condition");
				}
				if (comparisonAt(0))
				{
					const std::string op = next().text;
					c.kind = op == "==" ? Condition::Kind::Equal : op == "!=" ? Condition::Kind::NotEqual : op == "<" ? Condition::Kind::Less
						: op == "<=" ? Condition::Kind::LessEqual : op == ">" ? Condition::Kind::Greater : op == ">=" ? Condition::Kind::GreaterEqual
						: Condition::Kind::Contains;
					if (c.kind == Condition::Kind::Contains && (!c.hasPath || c.count)) fail(c.path.pos, "contains takes an array: Cars contains pvehicle/x");
					c.value = c.kind == Condition::Kind::Contains || c.hasPath ? value() : primary();
				}
				return c;
			}
			// ==, !=, <, <=, >, >= or the word contains at peek(at).
			// (< and > are words: the lexer keeps them in names.)
			bool comparisonAt(int at) const
			{
				const Token& k = peek(at);
				if (k.t == Token::T::Punct) return k.text == "==" || k.text == "!=" || k.text == "<=" || k.text == ">=";
				return k.t == Token::T::Word && (k.text == "<" || k.text == ">" || k.text == "contains");
			}
			std::string nameText(const Token& t)
			{
				return t.t == Token::T::String ? quotedName(t.text, true) : t.text;
			}

			Statement blockStatement()
			{
				Statement s;
				const Token& k = peek();
				s.pos = k.pos;
				using K = Statement::Kind;
				// `if` starts a condition; a field named if is followed by '=', an
				// operator or an index.
				const Token& after = peek(1);
				const bool assigns = (after.t == Token::T::Punct && (after.text == "=" || after.text == "+=" || after.text == "-=" ||
																	 after.text == "*=" || after.text == "/=" || after.text == "[")) ||
					(after.t == Token::T::Word && !after.spaceBefore && after.text[0] == '.');
				if (atWord("if") && !assigns && after.t != Token::T::End) return ifStatement(Context::Collection);
				if (k.t == Token::T::Word && (k.text == "revert" || k.text == "resize" || k.text == "insert" || k.text == "remove" || k.text == "append") &&
					(peek(1).t == Token::T::Word || peek(1).t == Token::T::String))
				{
					const std::string w = next().text;
					s.path = path();
					if (w == "append")
					{
						s.kind = K::Append;
						if (atPunct("="))
						{
							next();
							s.value = value();
						}
						if (atWord("if") && atWord("missing", 1))
						{
							next();
							next();
							s.ifMissing = true;
						}
						endStatement(s);
						return s;
					}
					if (w == "remove" && atPunct("="))
					{
						// By value: every element that is it.
						next();
						s.kind = K::Remove;
						s.value = value();
						endStatement(s);
						return s;
					}
					if (w == "revert")
					{
						s.kind = K::Revert;
						if (!s.path.steps.empty()) fail(s.path.pos, "revert takes a field (a whole value is inherited, not part of one)");
					}
					else if (w == "resize")
					{
						s.kind = K::Resize;
						if (atWord("by"))
						{
							// By a number of elements, either way.
							next();
							const Token& n = peek();
							char* end = nullptr;
							const long long c = n.t == Token::T::Word ? std::strtoll(n.text.c_str(), &end, 10) : 0;
							if (n.t != Token::T::Word || !end || *end || n.text.empty() || c < -0xFFFFFF || c > 0xFFFFFF)
								failHere("expected the number of elements to add (or, negative, take away)");
							next();
							s.by = true;
							s.number = int(c);
							endStatement(s);
							return s;
						}
						const Token& n = peek();
						char* end = nullptr;
						const long long c = n.t == Token::T::Word ? std::strtoll(n.text.c_str(), &end, 10) : -1;
						if (n.t != Token::T::Word || !end || *end || c < 0 || c > 0xFFFFFF) failHere("expected the new element count");
						next();
						s.count = uint32_t(c);
					}
					else
					{
						s.kind = w == "insert" ? K::Insert : K::Remove;
						if (s.path.steps.empty() || !s.path.steps.back().member.empty())
							fail(s.path.pos, w == "insert" ? "insert needs an index: insert " + s.path.text() + "[0] (or append " + s.path.text() + " = ...)"
								 : "remove needs an index or a value: remove " + s.path.text() + "[0], or remove " + s.path.text() + " = ...");
						if (s.kind == K::Insert && atPunct("="))
						{
							next();
							s.value = value();
						}
					}
					endStatement(s);
					return s;
				}
				if (atWord("echo") && peek(1).t != Token::T::Punct)
				{
					echo(s);
					endStatement(s);
					return s;
				}
				s.kind = K::Set;
				s.path = path();
				const Token& op = peek();
				if (op.t != Token::T::Punct || (op.text != "=" && op.text != "+=" && op.text != "-=" && op.text != "*=" && op.text != "/="))
					failHere("expected '=' after " + s.path.text());
				s.op = op.text.size() == 1 ? '=' : op.text[0];
				next();
				s.value = value();
				if (atWord("was"))
				{
					next();
					s.was = value();
				}
				endStatement(s);
				return s;
			}

			// ------------------------------------------------------------ values ---

			ValuePtr value()
			{
				return sum();
			}

			bool atOperator(const char* op) const
			{
				const Token& k = peek();
				if (std::strcmp(op, "/") == 0) return k.t == Token::T::Punct && k.text == "/";
				return k.t == Token::T::Word && k.text == op;
			}
			ValuePtr binary(char op, ValuePtr a, ValuePtr b)
			{
				auto v = std::make_shared<Value>();
				v->kind = Value::Kind::Binary;
				v->op = op;
				v->pos = a->pos;
				v->items = { std::move(a), std::move(b) };
				return v;
			}
			ValuePtr sum()
			{
				ValuePtr a = product();
				while (atOperator("+") || atOperator("-"))
				{
					const char op = next().text[0];
					a = binary(op, a, product());
				}
				return a;
			}
			ValuePtr product()
			{
				ValuePtr a = unary();
				while (atOperator("*") || atOperator("/"))
				{
					const char op = next().text[0];
					a = binary(op, a, unary());
				}
				return a;
			}
			ValuePtr unary()
			{
				if (atOperator("-"))
				{
					const Pos p = next().pos;
					auto v = std::make_shared<Value>();
					v->kind = Value::Kind::Negate;
					v->pos = p;
					v->items.push_back(unary());
					return v;
				}
				return primary();
			}
			ValuePtr primary()
			{
				const Token& k = peek();
				auto v = std::make_shared<Value>();
				v->pos = k.pos;
				switch (k.t)
				{
					case Token::T::String:
						v->kind = Value::Kind::String;
						v->text = next().text;
						return v;
					case Token::T::Option:
						v->kind = Value::Kind::Option;
						v->text = next().text;
						return v;
					case Token::T::Word:
						if ((k.text == "bytes" || k.text == "lua") && peek(1).t == Token::T::String)
						{
							v->kind = k.text == "bytes" ? Value::Kind::Bytes : Value::Kind::Lua;
							next();
							v->text = next().text;
							return v;
						}
						if (k.text == "+" || k.text == "*" || k.text == "-") failHere("expected a value");
						v->kind = LooksNumeric(k.text) ? Value::Kind::Number : Value::Kind::Word;
						v->text = next().text;
						return v;
					case Token::T::Punct:
						if (k.text == "(") return group();
						if (k.text == "[") return list();
						if (k.text == "{") return structValue();
						break;
					default: break;
				}
				failHere("expected a value");
			}
			// Skips line ends and comments inside a value; true if it passed a line end.
			bool skipInside()
			{
				bool nl = false;
				while (peek().t == Token::T::Newline || peek().t == Token::T::Comment) nl |= next().t == Token::T::Newline;
				return nl;
			}
			ValuePtr group()
			{
				const Pos p = next().pos;
				skipInside();
				std::vector<ValuePtr> items;
				bool comma = false;
				while (!atPunct(")"))
				{
					items.push_back(value());
					skipInside();
					if (atPunct(","))
					{
						next();
						comma = true;
						skipInside();
					}
					else if (!atPunct(")"))
					{
						failHere("expected ',' or ')'");
					}
				}
				next();
				if (items.size() == 1 && !comma) return items[0];
				ValuePtr v = MakeList(std::move(items), Value::Kind::Tuple);
				v->pos = p;
				return v;
			}
			ValuePtr list()
			{
				const Pos p = next().pos;
				skipInside();
				std::vector<ValuePtr> items;
				while (!atPunct("]"))
				{
					items.push_back(value());
					skipInside();
					if (atPunct(","))
					{
						next();
						skipInside();
					}
					else if (!atPunct("]"))
					{
						failHere("expected ',' or ']'");
					}
				}
				next();
				ValuePtr v = MakeList(std::move(items));
				v->pos = p;
				return v;
			}
			ValuePtr structValue()
			{
				auto v = std::make_shared<Value>();
				v->kind = Value::Kind::Struct;
				v->pos = next().pos;
				skipInside();
				// Members are separated by commas or line ends.
				while (!atPunct("}"))
				{
					const std::string m = name("a member name");
					expectPunct("=");
					v->members.emplace_back(m, value());
					const bool nl = skipInside();
					if (atPunct(","))
					{
						next();
						skipInside();
					}
					else if (!atPunct("}") && !nl)
					{
						failHere("expected ',' or '}'");
					}
				}
				next();
				return v;
			}

			std::vector<Token> t_;
			std::string source_;
			size_t i_ = 0;
		};

		// ---------------------------------------------------------------- printer ---

		std::string Name(const std::string& n)
		{
			// Names kept in quotes by the parser (a quoted 0x...).
			if (n.size() >= 2 && n.front() == '"' && n.back() == '"') return n;
			if (IsHexKey(n)) return n;
			return IsBare(n) ? n : Quote(n);
		}

		std::string CondText(const Condition& c, bool nested)
		{
			using CK = Condition::Kind;
			// The left side of a comparison.
			const std::string left = c.hasPath ? (c.count ? "count " : "") + c.path.text() : "$" + c.option;
			auto compare = [&](const char* op)
			{
				return left + " " + op + " " + PrintValue(*c.value);
			};
			switch (c.kind)
			{
				case CK::Option: return "$" + c.option;
				case CK::Equal: return compare("==");
				case CK::NotEqual: return compare("!=");
				case CK::Less: return compare("<");
				case CK::LessEqual: return compare("<=");
				case CK::Greater: return compare(">");
				case CK::GreaterEqual: return compare(">=");
				case CK::Contains: return compare("contains");
				case CK::ExistsClass: return "exists class " + Name(c.word);
				case CK::ExistsVault: return "exists vault " + Name(c.word);
				case CK::ExistsField: return "exists field " + Name(c.word) + " " + Name(c.name);
				case CK::ExistsCollection: return "exists " + c.target.text();
				case CK::Has: return "has " + c.path.text();
				case CK::Own: return "own " + c.path.text();
				case CK::Not: return "not " + CondText(c.items[0], true);
				case Condition::Kind::And:
				case Condition::Kind::Or:
				{
					// a and b and c reads as (a and b) and c: the left one needs no parentheses.
					std::string s = CondText(c.items[0], c.items[0].kind != c.kind) + (c.kind == Condition::Kind::And ? " and " : " or ") +
						CondText(c.items[1], true);
					return nested ? "(" + s + ")" : s;
				}
			}
			return {};
		}

		void PrintValueTo(std::string& out, const Value& v, int indent);

		bool IsLong(const Value& v)
		{
			if (v.kind != Value::Kind::List && v.kind != Value::Kind::Struct && v.kind != Value::Kind::Tuple) return false;
			if (v.kind == Value::Kind::Struct) return !v.members.empty();
			for (const ValuePtr& i : v.items)
				if (i->kind == Value::Kind::Struct || i->kind == Value::Kind::List) return true;
			std::string s;
			PrintValueTo(s, v, -1);
			return s.size() > 96;
		}

		void PrintValueTo(std::string& out, const Value& v, int indent)
		{
			const std::string pad = indent >= 0 ? std::string(size_t(indent + 1) * 4, ' ') : std::string();
			const std::string close = indent >= 0 ? std::string(size_t(indent) * 4, ' ') : std::string();
			switch (v.kind)
			{
				case Value::Kind::Number:
				case Value::Kind::Word: out += v.text; return;
				case Value::Kind::String: out += Quote(v.text); return;
				case Value::Kind::Option: out += "$" + v.text; return;
				case Value::Kind::Bytes: out += "bytes " + Quote(v.text); return;
				case Value::Kind::Lua:
				{
					// Lua source keeps its lines: """ unless it holds """ itself.
					if (v.text.find("\"\"\"") == std::string::npos)
					{
						out += "lua \"\"\"\n" + v.text;
						if (v.text.empty() || v.text.back() != '\n') out += '\n';
						out += close + "\"\"\"";
					}
					else
					{
						out += "lua " + Quote(v.text);
					}
					return;
				}
				case Value::Kind::Negate:
					out += "-";
					PrintValueTo(out, *v.items[0], indent);
					return;
				case Value::Kind::Binary:
				{
					// class/collection is written without spaces; arithmetic with.
					const bool tight = v.op == '/' && v.items[0]->kind != Value::Kind::Number && v.items[1]->kind != Value::Kind::Option;
					auto prec = [](char op)
					{
						return op == '+' || op == '-' ? 1 : 2;
					};
					auto side = [&](const Value& x, bool right)
					{
						const bool paren = x.kind == Value::Kind::Binary && !(tight && x.op == '/') &&
							(prec(x.op) < prec(v.op) || (right && prec(x.op) == prec(v.op) && (v.op == '-' || v.op == '/')));
						if (paren) out += "(";
						PrintValueTo(out, x, indent);
						if (paren) out += ")";
					};
					side(*v.items[0], false);
					out += tight ? "/" : std::string(" ") + v.op + " ";
					side(*v.items[1], true);
					return;
				}
				case Value::Kind::Tuple:
				case Value::Kind::List:
				{
					const char* open = v.kind == Value::Kind::List ? "[" : "(";
					const char* end = v.kind == Value::Kind::List ? "]" : ")";
					const bool multi = indent >= 0 && IsLong(v);
					out += open;
					for (size_t i = 0; i < v.items.size(); ++i)
					{
						if (multi) out += "\n" + pad;
						else if (i) out += ", ";
						PrintValueTo(out, *v.items[i], multi ? indent + 1 : indent);
						if (multi) out += ",";
					}
					if (v.kind == Value::Kind::Tuple && v.items.size() == 1 && !multi) out += ",";
					if (multi) out += "\n" + close;
					out += end;
					return;
				}
				case Value::Kind::Struct:
				{
					if (v.members.empty())
					{
						out += "{}";
						return;
					}
					const bool multi = indent >= 0;
					out += "{";
					for (size_t i = 0; i < v.members.size(); ++i)
					{
						if (multi) out += "\n" + pad;
						else out += i ? ", " : " ";
						out += Name(v.members[i].first) + " = ";
						PrintValueTo(out, *v.members[i].second, multi ? indent + 1 : indent);
					}
					out += multi ? "\n" + close + "}" : " }";
					return;
				}
			}
		}

		void PrintStatements(std::string& out, const std::vector<Statement>& list, int indent);

		void PrintStatement(std::string& out, const Statement& s, int indent)
		{
			const std::string pad(size_t(indent) * 4, ' ');
			using K = Statement::Kind;
			out += pad;
			auto clauses = [&]()
			{
				if (s.hasFrom) out += " from " + s.from.text();
				if (s.hasUnder) out += " under " + s.under.text();
				if (!s.vault.empty()) out += " in vault " + Name(s.vault);
				if (s.exact) out += " exact";
			};
			auto body = [&](const std::vector<Statement>& b)
			{
				out += " {\n";
				PrintStatements(out, b, indent + 1);
				out += pad + "}";
			};
			switch (s.kind)
			{
				case K::Comment: out += "#" + s.comment + "\n"; return;
				case K::Header: out += "attribulina " + std::to_string(s.number); break;
				case K::Meta: out += s.word + " " + (s.word == "game" && IsBare(s.text) ? s.text : Quote(s.text)); break;
				case K::Option:
				{
					const OptionDef& o = s.option;
					out += "option " + o.id + " " + OptionTypeName(o.type);
					if (!o.label.empty()) out += " " + Quote(o.label);
					if (!o.choices.empty())
					{
						out += " choices [";
						for (size_t i = 0; i < o.choices.size(); ++i) out += (i ? ", " : "") + Quote(o.choices[i]);
						out += "]";
					}
					if (o.defaultValue) out += " default " + PrintValue(*o.defaultValue);
					if (o.min) out += " min " + NumberText(*o.min);
					if (o.max) out += " max " + NumberText(*o.max);
					if (!o.help.empty()) out += " help " + Quote(o.help);
					break;
				}
				case K::If:
				{
					out += "if " + CondText(s.cond, false);
					body(s.body);
					const Statement* e = &s;
					while (!e->elseBody.empty())
					{
						if (e->elseBody.size() == 1 && e->elseBody[0].kind == K::If)
						{
							e = &e->elseBody[0];
							out += " else if " + CondText(e->cond, false);
							body(e->body);
						}
						else
						{
							out += " else";
							body(e->elseBody);
							break;
						}
					}
					break;
				}
				case K::Edit:
					out += "edit " + s.target.text();
					body(s.body);
					break;
				case K::Create:
				case K::Collection:
					out += (s.kind == K::Create ? "create " : "collection ") + s.target.text();
					clauses();
					if (!s.body.empty()) body(s.body);
					break;
				case K::Delete: out += "delete " + s.target.text() + (s.withChildren ? " with children" : ""); break;
				case K::Rename: out += "rename " + s.target.text() + " to " + Name(s.word); break;
				case K::Move: out += "move " + s.target.text() + " under " + s.under.text(); break;
				case K::MoveToVault: out += "move " + s.target.text() + " to vault " + Name(s.vault); break;
				case K::CreateVault: out += "create vault " + Name(s.vault) + " in " + Name(s.word); break;
				case K::DeleteVault: out += "delete vault " + Name(s.vault); break;
				case K::RenameVault: out += "rename vault " + Name(s.vault) + " to " + Name(s.word); break;
				case K::Set:
				{
					out += s.path.text() + (s.op == '=' ? " = " : std::string(" ") + s.op + "= ");
					PrintValueTo(out, *s.value, indent);
					if (s.was)
					{
						out += " was ";
						PrintValueTo(out, *s.was, indent);
					}
					break;
				}
				case K::Revert: out += "revert " + s.path.text(); break;
				case K::Resize: out += "resize " + s.path.text() + (s.by ? " by " + std::to_string(s.number) : " " + std::to_string(s.count)); break;
				case K::Insert:
					out += "insert " + s.path.text();
					if (s.value)
					{
						out += " = ";
						PrintValueTo(out, *s.value, indent);
					}
					break;
				case K::Remove:
					out += "remove " + s.path.text();
					if (s.value)
					{
						out += " = ";
						PrintValueTo(out, *s.value, indent);
					}
					break;
				case K::Append:
					out += "append " + s.path.text();
					if (s.value)
					{
						out += " = ";
						PrintValueTo(out, *s.value, indent);
					}
					if (s.ifMissing) out += " if missing";
					break;
				case K::Include: out += "include " + Quote(s.text); break;
				case K::Class:
					out += "class " + Name(s.word);
					if (!s.vault.empty()) out += " in vault " + Name(s.vault);
					for (const auto& [k, v] : s.settings) out += " " + k + " " + std::to_string(v);
					body(s.body);
					break;
				case K::ClassField:
				{
					if (s.word == "bytes")
					{
						PrintValueTo(out, *s.value, indent);
						break;
					}
					out += s.word + " " + s.path.text() + " " + (IsBare(s.text) ? s.text : Quote(s.text));
					if (s.isArray) out += "[" + (s.count ? std::to_string(s.count) : std::string()) + "]";
					if (s.unsearchable) out += " unsearchable";
					for (const auto& [k, v] : s.settings)
					{
						char hex[16];
						std::snprintf(hex, sizeof(hex), "0x%02llX", (unsigned long long)v);
						out += " " + k + " " + (k == "flags" ? std::string(hex) : std::to_string(v));
					}
					if (s.value)
					{
						out += " = ";
						PrintValueTo(out, *s.value, indent);
					}
					break;
				}
				case K::DeleteClass: out += "delete class " + Name(s.word); break;
				case K::Echo:
					out += "echo";
					if (!s.word.empty()) out += " " + s.word;
					for (const ValuePtr& v : s.value->items)
					{
						out += " ";
						PrintValueTo(out, *v, indent);
					}
					break;
			}
			if (!s.comment.empty()) out += "  #" + s.comment;
			out += "\n";
		}

		void PrintStatements(std::string& out, const std::vector<Statement>& list, int indent)
		{
			for (const Statement& s : list) PrintStatement(out, s, indent);
		}

	} // namespace

	std::string NumberText(double d, bool asFloat)
	{
		if (std::isinf(d)) return d > 0 ? "inf" : "-inf";
		char buf[40];
		const int most = asFloat ? 9 : 17;
		for (int prec = 1; prec <= most; ++prec)
		{
			std::snprintf(buf, sizeof(buf), "%.*g", prec, d);
			const double back = asFloat ? double(std::strtof(buf, nullptr)) : std::strtod(buf, nullptr);
			if (back == d && std::signbit(back) == std::signbit(d)) break;
		}
		// %g gives 10 as "1e+01" at one digit: spell out exponents in the usual range.
		const char* e = std::strchr(buf, 'e');
		if (!e) return buf;
		const int exp = std::atoi(e + 1);
		if (exp < -6 || exp > 12) return buf;
		const bool neg = buf[0] == '-';
		std::string digits;
		for (const char* p = buf + neg; p < e; ++p)
			if (*p != '.') digits += *p;
		const int point = exp + 1; // digits before the decimal point
		std::string out = neg ? "-" : "";
		if (point <= 0) out += "0." + std::string(size_t(-point), '0') + digits;
		else if (size_t(point) >= digits.size()) out += digits + std::string(size_t(point) - digits.size(), '0');
		else out += digits.substr(0, size_t(point)) + "." + digits.substr(size_t(point));
		return out;
	}

	bool IsBareWord(std::string_view s)
	{
		return IsBare(s);
	}

	std::string Quote(std::string_view text)
	{
		std::string out = "\"";
		for (const char c : text)
		{
			switch (c)
			{
				case '"': out += "\\\""; break;
				case '\\': out += "\\\\"; break;
				case '\n': out += "\\n"; break;
				case '\t': out += "\\t"; break;
				case '\r': out += "\\r"; break;
				default:
					if (static_cast<unsigned char>(c) < 0x20)
					{
						char b[8];
						std::snprintf(b, sizeof(b), "\\x%02x", static_cast<unsigned char>(c));
						out += b;
					}
					else
					{
						out += c;
					}
			}
		}
		return out + "\"";
	}

	std::string Target::text() const
	{
		if (root) return "root";
		// Gameplay names are paths: their segments go bare when they can.
		if (name.find('/') != std::string::npos && name.front() != '"')
		{
			std::string joined;
			size_t b = 0;
			for (;;)
			{
				const size_t e = name.find('/', b);
				const std::string seg = name.substr(b, e == std::string::npos ? std::string::npos : e - b);
				if (seg.empty() || !IsBare(seg) || IsHexKey(seg)) return Name(cls) + "/" + Quote(name);
				joined += (joined.empty() ? "" : "/") + seg;
				if (e == std::string::npos) break;
				b = e + 1;
			}
			return Name(cls) + "/" + joined;
		}
		return Name(cls) + "/" + Name(name);
	}

	std::string FieldPath::text() const
	{
		std::string s = Name(field);
		for (const Step& st : steps)
		{
			if (st.member.empty()) s += "[" + std::to_string(st.index) + "]";
			else s += "." + (IsBare(st.member) ? st.member : Quote(st.member));
		}
		return s;
	}

	Script Parse(std::string_view text, const std::string& source, const std::string& folder)
	{
		Parser p(Lex(text, source), source);
		Script s = p.script();
		s.folder = folder;
		return s;
	}

	namespace
	{

		fs::path FromUtf8(const std::string& s)
		{
			return fs::path(std::u8string(s.begin(), s.end()));
		}
		std::string ToUtf8(const fs::path& p)
		{
			const std::u8string u = p.u8string();
			return std::string(u.begin(), u.end());
		}

		class IncludeReader
		{
		public:
			void read(Script& sc)
			{
				// The script itself, so it can't include itself either.
				if (!sc.folder.empty()) chain_.push_back(Id(FromUtf8(sc.folder) / FromUtf8(sc.source).filename()));
				walk(sc, sc.statements);
			}

		private:
			static fs::path Id(const fs::path& p)
			{
				std::error_code ec;
				fs::path c = fs::weakly_canonical(p, ec);
				return ec ? p : c;
			}

			void walk(const Script& sc, std::vector<Statement>& list)
			{
				for (Statement& s : list)
				{
					if (s.kind == Statement::Kind::If)
					{
						walk(sc, s.body);
						walk(sc, s.elseBody);
					}
					else if (s.kind == Statement::Kind::Include && !s.included)
					{
						s.included = load(sc, s);
					}
				}
			}

			std::shared_ptr<const Script> load(const Script& sc, const Statement& s)
			{
				auto fail = [&](const std::string& why)
				{
					return ScriptError(s.pos, "include " + Quote(s.text) + ": " + why, sc.source);
				};
				// Scripts written on Windows may use backslashes; '/' works on both.
				std::string written = s.text;
				std::replace(written.begin(), written.end(), '\\', '/');
				fs::path p = FromUtf8(written);
				if (p.is_relative())
				{
					if (sc.folder.empty()) throw fail("a script that isn't saved has no folder for a relative path; save it first, or give the full path");
					p = FromUtf8(sc.folder) / p;
				}
				p = p.lexically_normal();
				const fs::path id = Id(p);
				if (std::find(chain_.begin(), chain_.end(), id) != chain_.end())
					throw fail(!chain_.empty() && chain_.back() == id ? "a script can't include itself" : "that script includes this one");
				if (auto it = done_.find(id); it != done_.end()) return it->second;
				std::ifstream in(p, std::ios::binary);
				if (!in) throw fail("can't read " + ToUtf8(p));
				std::ostringstream text;
				text << in.rdbuf();
				auto inc = std::make_shared<Script>();
				chain_.push_back(id);
				try
				{
					*inc = Parse(text.str(), ToUtf8(p), ToUtf8(p.parent_path()));
					walk(*inc, inc->statements);
				}
				catch (const ScriptError& e)
				{
					// At the include line here, naming the file it is in.
					if (!e.file.empty()) throw ScriptError(s.pos, e.message, e.file, e.filePos);
					throw ScriptError(s.pos, e.message, ToUtf8(p), e.pos);
				}
				chain_.pop_back();
				done_[id] = inc;
				return inc;
			}

			std::vector<fs::path> chain_;                           // the scripts being read, outermost first
			std::map<fs::path, std::shared_ptr<const Script>> done_; // read already: a script included twice is read once
		};

		bool Unread(const std::vector<Statement>& list)
		{
			for (const Statement& s : list)
				if ((s.kind == Statement::Kind::Include && !s.included) || (s.kind == Statement::Kind::If && (Unread(s.body) || Unread(s.elseBody))))
					return true;
			return false;
		}

	} // namespace

	void LoadIncludes(Script& script)
	{
		IncludeReader().read(script);
	}

	bool HasUnreadIncludes(const Script& script)
	{
		return Unread(script.statements);
	}

	namespace
	{
		bool PathNeeds2(const FieldPath& p)
		{
			for (const FieldPath::Step& st : p.steps)
				if (st.member.empty() && st.index < 0) return true;
			return false;
		}
		bool CondNeeds2(const Condition& c)
		{
			using CK = Condition::Kind;
			switch (c.kind)
			{
				case CK::Option: return false;
				case CK::Not: case CK::And: case CK::Or:
					for (const Condition& i : c.items)
						if (CondNeeds2(i)) return true;
					return false;
				case CK::Equal: case CK::NotEqual: return c.hasPath;
				default: return true;
			}
		}
	} // namespace

	int FormatNeeded(const std::vector<Statement>& statements)
	{
		using K = Statement::Kind;
		for (const Statement& s : statements)
		{
			if (s.kind == K::Append || s.by || (s.kind == K::Remove && s.value) || PathNeeds2(s.path)) return 2;
			if (s.kind == K::If && CondNeeds2(s.cond)) return 2;
			if (FormatNeeded(s.body) > 1 || FormatNeeded(s.elseBody) > 1) return 2;
		}
		return 1;
	}

	std::string Print(const Script& script)
	{
		std::string out;
		// The header says the lowest format that reads the script.
		const int need = FormatNeeded(script.statements);
		if (!script.statements.empty() && script.statements[0].kind == Statement::Kind::Header && script.statements[0].number < need)
		{
			std::vector<Statement> list = script.statements;
			list[0].number = need;
			PrintStatements(out, list, 0);
			return out;
		}
		PrintStatements(out, script.statements, 0);
		return out;
	}

	std::string PrintValue(const Value& v)
	{
		std::string out;
		PrintValueTo(out, v, -1);
		return out;
	}

	// ------------------------------------------------------- what it names ---

	namespace
	{

		// Everything a line names, at the column it starts.
		class PlaceFinder
		{
		public:
			explicit PlaceFinder(std::string_view text) : text_(text)
			{
			}

			struct Item
			{
				Pos pos;
				ScriptPlace place;
			};
			std::vector<Item> items;

			void statements(const std::vector<Statement>& list, const Target* block)
			{
				for (const Statement& s : list) statement(s, block);
			}

		private:
			using K = Statement::Kind;

			void add(Pos p, ScriptPlace place)
			{
				if (p.line > 0) items.push_back({ p, std::move(place) });
			}
			void addTarget(const Target& t)
			{
				if (t.root || t.cls.empty()) return;
				ScriptPlace pl;
				pl.target = t;
				add(t.pos, std::move(pl));
			}
			ScriptPlace valuePlace(const Target& block, const FieldPath& path) const
			{
				ScriptPlace pl;
				pl.target = block;
				pl.path = path;
				pl.hasPath = true;
				return pl;
			}

			// Where `name = value` starts on the value's line: the name before it.
			Pos memberPos(const std::string& name, Pos valuePos) const
			{
				size_t b = 0;
				for (int l = 1; l < valuePos.line && b != std::string_view::npos; ++l)
				{
					b = text_.find('\n', b);
					if (b != std::string_view::npos) ++b;
				}
				if (b == std::string_view::npos || valuePos.col < 1) return valuePos;
				const std::string_view before = text_.substr(b, std::min(text_.size() - b, size_t(valuePos.col - 1)));
				const size_t at = before.rfind(name);
				return at == std::string_view::npos ? valuePos : Pos{ valuePos.line, int(at) + 1 };
			}

			// `steps`: the value is the one at `path` (an assignment), so a list's
			// items are its elements and a struct's members its members.
			void value(const Value& v, const Target& block, const FieldPath& path, bool steps)
			{
				using V = Value::Kind;
				ScriptPlace pl = valuePlace(block, path);
				if (v.kind == V::Binary && v.op == '/')
				{
					// class/name: a reference, when the first part is a word.
					std::vector<const Value*> parts;
					std::function<void(const Value&)> flatten = [&](const Value& x)
					{
						if (x.kind == V::Binary && x.op == '/')
						{
							flatten(*x.items[0]);
							flatten(*x.items[1]);
						}
						else
						{
							parts.push_back(&x);
						}
					};
					flatten(v);
					bool words = parts.size() >= 2 && parts[0]->kind != V::Number;
					for (const Value* x : parts) words &= x->kind == V::Word || x->kind == V::String || x->kind == V::Number;
					if (words)
					{
						Target t;
						t.cls = parts[0]->text;
						for (size_t i = 1; i < parts.size(); ++i) t.name += (i > 1 ? "/" : "") + parts[i]->text;
						t.pos = v.pos;
						pl.named = t;
					}
					add(v.pos, std::move(pl));
					return;
				}
				if (v.kind == V::String && v.text.find('/') != std::string::npos)
				{
					// "class/name" in quotes: a reference, or a gameplay path.
					const size_t slash = v.text.find('/');
					Target t;
					t.cls = v.text.substr(0, slash);
					t.name = v.text.substr(slash + 1);
					t.pos = v.pos;
					if (!t.cls.empty() && !t.name.empty()) pl.named = t;
				}
				add(v.pos, std::move(pl));
				if (v.kind == V::List)
				{
					for (size_t i = 0; i < v.items.size(); ++i)
					{
						FieldPath p = path;
						if (steps) p.steps.push_back({ std::string(), int64_t(i) });
						value(*v.items[i], block, p, steps);
					}
				}
				else if (v.kind == V::Struct)
				{
					for (const auto& [name, mv] : v.members)
					{
						if (!mv) continue;
						FieldPath p = path;
						if (steps) p.steps.push_back({ name, -1 });
						add(memberPos(name, mv->pos), valuePlace(block, p));
						value(*mv, block, p, steps);
					}
				}
			}

			void condition(const Condition& c, const Target* block)
			{
				using C = Condition::Kind;
				if (c.kind == C::ExistsCollection) addTarget(c.target);
				if (block && c.hasPath && !c.path.field.empty()) add(c.path.pos, valuePlace(*block, c.path));
				for (const Condition& x : c.items) condition(x, block);
			}

			void statement(const Statement& s, const Target* block)
			{
				switch (s.kind)
				{
					case K::Edit: case K::Create: case K::Collection:
						addTarget(s.target);
						if (s.hasFrom) addTarget(s.from);
						if (s.hasUnder) addTarget(s.under);
						statements(s.body, &s.target);
						return;
					case K::Delete: case K::Rename: case K::MoveToVault:
						addTarget(s.target);
						return;
					case K::Move:
						addTarget(s.target);
						if (s.hasUnder) addTarget(s.under);
						return;
					case K::If:
						condition(s.cond, block);
						statements(s.body, block);
						statements(s.elseBody, block);
						return;
					case K::Set: case K::Revert: case K::Resize: case K::Insert: case K::Remove: case K::Append:
						if (!block || s.path.field.empty()) return;
						add(s.path.pos, valuePlace(*block, s.path));
						// What an append, insert or remove gives isn't at the path's
						// elements: its items stay the array's.
						if (s.value) value(*s.value, *block, s.path, s.kind == K::Set && s.op == '=');
						if (s.was) value(*s.was, *block, s.path, s.kind == K::Set);
						return;
					default:
						return;
				}
			}

			std::string_view text_;
		};

		// The text with only the top-level statement around `line` left (the
		// other lines emptied, so positions hold): brackets of every kind
		// counted outside strings and comments.
		std::string StatementAround(std::string_view text, int line)
		{
			std::vector<int> depthAt; // at each line's start
			std::vector<bool> blank;
			int depth = 0;
			bool inString = false, lineHasToken = false;
			depthAt.push_back(0);
			for (size_t i = 0; i < text.size(); ++i)
			{
				const char c = text[i];
				if (c == '\n')
				{
					blank.push_back(!lineHasToken);
					lineHasToken = false;
					inString = false;
					depthAt.push_back(depth);
					continue;
				}
				if (inString)
				{
					if (c == '\\') ++i;
					else if (c == '"') inString = false;
					continue;
				}
				if (c == '#')
				{
					while (i + 1 < text.size() && text[i + 1] != '\n') ++i;
					continue;
				}
				if (c != ' ' && c != '\t' && c != '\r') lineHasToken = true;
				if (c == '"') inString = true;
				else if (c == '{' || c == '[' || c == '(') ++depth;
				else if ((c == '}' || c == ']' || c == ')') && depth > 0) --depth;
			}
			blank.push_back(!lineHasToken);
			const int lines = int(depthAt.size());
			if (line < 1 || line > lines) return {};
			auto starts = [&](int l)
			{
				return depthAt[size_t(l - 1)] == 0 && !blank[size_t(l - 1)];
			};
			int first = line;
			while (first > 1 && !starts(first)) --first;
			int last = line;
			while (last < lines && !starts(last + 1)) ++last;
			std::string out;
			size_t b = 0;
			for (int l = 1; l <= lines; ++l)
			{
				size_t e = text.find('\n', b);
				if (e == std::string_view::npos) e = text.size();
				if (l >= first && l <= last) out.append(text.substr(b, e - b));
				if (l < lines) out += '\n';
				b = e + 1;
			}
			return out;
		}

	} // namespace

	ScriptPlace PlaceAt(std::string_view text, Pos caret)
	{
		Script s;
		try
		{
			s = Parse(text, "script");
		}
		catch (const ScriptError&)
		{
			try
			{
				s = Parse(StatementAround(text, caret.line), "script");
			}
			catch (const ScriptError&)
			{
				return {};
			}
		}
		PlaceFinder f(text);
		f.statements(s.statements, nullptr);
		// The item that starts last at or before the caret on its line, else the
		// line's first; a later item at the same place is the deeper one.
		const PlaceFinder::Item* best = nullptr;
		const PlaceFinder::Item* first = nullptr;
		for (const PlaceFinder::Item& it : f.items)
		{
			if (it.pos.line != caret.line) continue;
			if (!first || it.pos.col < first->pos.col) first = &it;
			if (it.pos.col <= caret.col && (!best || it.pos.col >= best->pos.col)) best = &it;
		}
		if (!best) best = first;
		return best ? best->place : ScriptPlace{};
	}

} // namespace attrib::script
