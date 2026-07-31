---
id: spec.appendix_a_ebnf
order: 020
---

<!-- xr-spec:cn -->
---

## 附录 A. EBNF 语法

> 真值源：`src/frontend/parser/xparse_*.c`。本附录给出整理后的紧凑 EBNF；具体冲突由 parser 实现决议。

### A.1 词法层

```ebnf
SourceFile ::= Statement*

Comment ::= '//' [^\n]*
         |  '/*' .* '*/'

Identifier ::= IdStart IdContinue*
IdStart    ::= 'a'..'z' | 'A'..'Z' | '_' | NonAsciiUtf8Byte
IdContinue ::= IdStart | '0'..'9'
// 源文件先整体校验为 UTF-8；lexer 将任意非 ASCII UTF-8 字节视为 identifier 字节，当前不做 XID/NFC 归一化。

IntLiteral   ::= DecimalInt | HexInt | BinInt | OctInt
DecimalInt   ::= DecimalDigit ('_'? DecimalDigit)*
HexInt       ::= '0x' HexDigit ('_'? HexDigit)*
BinInt       ::= '0b' ('0' | '1') ('_'? ('0' | '1'))*
OctInt       ::= '0o' ('0'..'7') ('_'? ('0'..'7'))*

FloatLiteral ::= DecimalInt '.' DecimalInt? Exponent?
              |  DecimalInt Exponent
Exponent     ::= ('e' | 'E') ('+' | '-')? DecimalDigit+

BigIntLiteral ::= (DecimalInt | HexInt | BinInt | OctInt) 'n'

QuotedLiteral ::= StringLiteral | FixedByteLiteral
StringLiteral ::= StringPrefix (InlineQuoted | BlockQuoted)
FixedByteLiteral ::= FixedBytePrefix (InlineQuoted | BlockQuoted)
StringPrefix ::= '' | 'r'
FixedBytePrefix ::= 'b' | 'br' | 'c' | 'cr'
InlineQuoted ::= '"' InlinePayload* '"' | '""'
BlockQuoted ::= QuoteRun ImmediateLineEnding BlockBody BlockClose
QuoteRun ::= '"'{Q}                         // Q >= 3
BlockClose ::= LineStart Indent SameQuoteRun (LineEnding | EOF)
CharLiteral ::= "'" CharBody "'"
CharBody ::= UnicodeScalar | EscapeSeq | '\u{' HexDigit{1,6} '}'
RegexLiteral ::= '/' RegexBody '/' RegexFlag*
RegexFlag ::= 'i' | 'm' | 's' | 'g' | 'u'

BoolLiteral ::= 'true' | 'false'
NullLiteral ::= 'null'
```

### A.2 类型

```ebnf
Type ::= UnionType
UnionType ::= IntersectionType ('|' IntersectionType)*
IntersectionType ::= NullableType
NullableType ::= PrimaryType '?'?
PrimaryType ::= ConstType | FFIPointerType | CFunctionType | NamedType | FunctionType | TupleType | ObjectType
ConstType ::= 'const' PrimaryType
FFIPointerType ::= ('Ptr' | 'MutPtr') '<' Type '>'
CFunctionType ::= 'CFn' '<' FunctionType '>'
NamedType   ::= QualifiedIdent TypeArgs?
FunctionType ::= '(' TypeList? ')' '->' Type
TupleType   ::= '(' Type (',' Type)+ ')'
ObjectType  ::= '{' FieldList? '}'
FieldList   ::= ObjectField (',' ObjectField)* ','?
ObjectField ::= Identifier ':' Type
QualifiedIdent ::= Identifier ('.' Identifier)*
TypeArgs    ::= '<' Type (',' Type)* '>'
TypeList    ::= Type (',' Type)*
```

### A.3 表达式

```ebnf
Expression ::= AssignExpr
AssignExpr ::= TernaryExpr (AssignOp Expression)?
AssignOp   ::= '=' | '+=' | '-=' | '*=' | '/=' | '%='
            |  '&=' | '|=' | '^=' | '<<=' | '>>='

TernaryExpr ::= NullCoalesceExpr ('?' Expression ':' Expression)?
NullCoalesceExpr ::= LogicOrExpr ('??' LogicOrExpr)*
LogicOrExpr ::= LogicAndExpr ('||' LogicAndExpr)*
LogicAndExpr ::= BitOrExpr ('&&' BitOrExpr)*
BitOrExpr   ::= BitXorExpr ('|' BitXorExpr)*
BitXorExpr  ::= BitAndExpr ('^' BitAndExpr)*
BitAndExpr  ::= EqualityExpr ('&' EqualityExpr)*
EqualityExpr ::= RelationalExpr (('==' | '!=') RelationalExpr)*
RelationalExpr ::= RangeExpr ((('<' | '<=' | '>' | '>=') RangeExpr) | (('as' | 'is') Type))*
RangeExpr   ::= ShiftExpr (('..' | '..=') ShiftExpr)?
ShiftExpr   ::= AdditiveExpr (('<<' | '>>') AdditiveExpr)*
AdditiveExpr ::= MultiplicativeExpr (('+' | '-') MultiplicativeExpr)*
MultiplicativeExpr ::= UnaryExpr (('*' | '/' | '%') UnaryExpr)*
// range 松于所有算术运算符、紧于比较，非结合（a..b..c 是语法错误）；安全转换写为 `x as T?`，T? 是可空类型。

UnaryExpr ::= ('-' | '+' | '!' | '~') UnaryExpr
           |  'move' UnaryExpr
           |  'await' ('all' | 'any' | 'anySuccess')? UnaryExpr
           |  'go' (Block | PostfixExpr)
           |  'unsafe' Block
           |  PostfixExpr

PostfixExpr ::= Primary PostfixOp*
PostfixOp   ::= '(' ArgList? ')'              // call
             |  '.' Identifier                 // member
             |  '?.' Identifier                 // optional chain property
             |  '?.' Identifier '(' ArgList? ')'  // optional chain method
             |  '?[' Expression ']'            // optional chain index
             |  '[' Expression ']'             // index
             |  '[' Expression? ':' Expression? ']'  // slice
             |  '!'                            // force unwrap

Primary ::= IntLiteral | FloatLiteral | BigIntLiteral
         |  QuotedLiteral | CharLiteral | RegexLiteral
         |  BoolLiteral | NullLiteral
         |  Identifier
         |  ArrayLit | MapLit | SetLit | ObjectLit
         |  BareLambda
         |  ArrowFunction
         |  ComptimeExpr
         |  MatchExpr
         |  '(' Expression ')'
         |  '(' Expression (',' Expression)+ ')'  // tuple

ArrayLit ::= '[' (ArrayElem (',' ArrayElem)* ','?)? ']'
ArrayElem ::= '...' Expression | Expression
MapLit   ::= '#{' (MapEntry (',' MapEntry)* ','?)? '}'
MapEntry ::= Expression ':' Expression
SetLit   ::= '#[' (Expression (',' Expression)* ','?)? ']'
ObjectLit ::= '{' (ObjectFieldExpr (',' ObjectFieldExpr)* ','?)? '}'
ObjectFieldExpr ::= Identifier ':' Expression | Identifier | '...' Expression

BareLambda ::= Identifier '->' (Expression | Block)
ArrowFunction ::= '(' ArrowParams? ')' '->' (Expression | Block)
ArrowParams ::= ArrowParam (',' ArrowParam)*
ArrowParam  ::= Identifier ':' Type
// Note: arrow closures cannot declare an explicit return type;
// use `fn(p: T) -> R { ... }` or annotate the binding (`var f: (T) -> R = ...`) instead.

ComptimeExpr ::= 'comptime' (Expression | Block)

MatchExpr ::= 'match' '(' Expression ')' '{' MatchArm (','? MatchArm)* ','? '}'
MatchArm  ::= Pattern ('if' '(' Expression ')')? '->' (Expression | Block)

ArgList ::= CallArg (',' CallArg)* ','?
CallArg ::= ('ref' | 'out') Expression
          | '...' Expression
          | '_'
          | Expression
```

### A.4 模式

```ebnf
Pattern ::= LiteralPattern
         |  RangePattern
         |  EnumPattern
         |  TypePattern
         |  WildcardPattern
         |  BindingPattern
         |  MultiPattern

LiteralPattern  ::= IntLiteral | FloatLiteral | StringLiteral | CharLiteral | BoolLiteral | NullLiteral
RangePattern    ::= Expression ('..' | '..=') Expression
EnumPattern     ::= QualifiedIdent VariantPayloadPattern?    // ADT enum payload 解构
VariantPayloadPattern ::= '(' Pattern (',' Pattern)* ')'
TypePattern     ::= 'is' Type Identifier?
WildcardPattern ::= '_'
BindingPattern  ::= Identifier
MultiPattern    ::= Pattern (',' Pattern)+
```

### A.5 语句

```ebnf
Statement ::= ExprStmt
           |  IncDecStmt
           |  VarDecl
           |  ConstDecl
           |  FnDecl
           |  ExternBlock
           |  ClassDecl
           |  StructDecl
           |  InterfaceDecl
           |  EnumDecl
           |  TypeAliasDecl
           |  ImportDecl
           |  ExportDecl
           |  IfStmt
           |  WhileStmt
           |  ForStmt
           |  ForInStmt
           |  ForInPairStmt
           |  MatchStmt
           |  ScopeStmt
           |  SelectStmt
           |  ReturnStmt
           |  BreakStmt
           |  ContinueStmt
           |  ThrowStmt
           |  TryStmt
           |  DeferStmt
           |  YieldStmt
           |  Block
           // \u6ce8\uff1aprint/dump \u4f5c\u4e3a\u51fd\u6570\u8c03\u7528\u5305\u542b\u5728 ExprStmt \u4e2d\uff1bgo \u662f\u8868\u8fbe\u5f0f\uff08GoExpr\uff09

// LineBreak 不是一个 token：它是"行结尾在此结束语句"这一判定的结果。
// 判定规则（上一 token 可结束表达式 + 新行首 token 可开始表达式 + 不在
// '(' / '[' 之内）是规范性的，完整定义见 §1.2.1。
ExprStmt ::= Expression (';' | LineBreak)
IncDecStmt ::= Identifier ('++' | '--') (';' | LineBreak)
Block    ::= '{' Statement* '}'

IfStmt    ::= 'if' '(' Expression ')' Block ('else' 'if' '(' Expression ')' Block)* ('else' Block)?
LoopLabel ::= Identifier ':'
WhileStmt ::= LoopLabel? 'while' '(' Expression ')' Block
ForStmt   ::= LoopLabel? 'for' '(' VarDecl? ';' Expression? ';' (Expression | Identifier ('++' | '--'))? ')' Block
ForInStmt ::= LoopLabel? 'for' '(' Identifier 'in' Expression ')' Block
ForInPairStmt ::= LoopLabel? 'for' '(' Identifier ',' Identifier 'in' Expression ')' Block
             |  LoopLabel? 'for' '(' '(' Identifier ',' Identifier ')' 'in' Expression ')' Block
MatchStmt ::= 'match' '(' Expression ')' '{' MatchArm (','? MatchArm)* ','? '}'

ReturnStmt   ::= 'return' (Expression | '(' Expression (',' Expression)+ ')')?
BreakStmt    ::= 'break' Identifier?
ContinueStmt ::= 'continue' Identifier?

ThrowStmt ::= 'throw' Expression
TryStmt   ::= 'try' Block CatchClause+
CatchClause ::= 'catch' 'panic'? ('(' Identifier (':' Type)? ')')? Block

DeferStmt ::= 'defer' (Expression | Block)

// print 是普通全局函数调用，语法上属于 ExprStmt。

// go 是表达式，返回 Task<T>。不作为独立语句类别出现（封装在 ExprStmt 中）。

ScopeStmt ::= ('linked' | 'supervisor')? 'scope' Block

SelectStmt ::= 'select' '{' SelectArm+ '}'
SelectArm  ::= Identifier 'from' Expression '->' Block      // 接收
            |  Expression 'to' Expression '->' Block        // 发送
            |  'after' Expression '->' Block                // 超时
            |  '_' '->' Block                                // 默认

YieldStmt ::= 'yield' Expression
```

### A.6 声明

```ebnf
Visibility ::= 'export'
VarDecl ::= 'var' Binding
ConstDecl ::= AttrList? Visibility? 'const' Binding
Binding ::= BindingPattern (':' Type)? ('=' Expression)?
BindingPattern ::= Identifier
                |  '[' BindingPattern (',' BindingPattern)* ','? ']'
                |  '(' BindingPattern (',' BindingPattern)+ ','? ')'
                |  '{' ObjectBinding (',' ObjectBinding)* ','? '}'
ObjectBinding ::= Identifier (':' Identifier)?

FnDecl ::= AttrList? Visibility? 'fn' Identifier TypeParams? '(' ParamList? ')' ReturnType? Block
ExternBlock ::= 'extern' '"C"' '{' ExternFnDecl+ '}'
ExternFnDecl ::= Visibility? 'fn' Identifier '(' ParamList? ')' ReturnType? ';'?
ParamList ::= Param (',' Param)* ','?
Param     ::= Identifier ':' ParamType ('=' Expression)?
           |  '...' Identifier ':' Type
ParamType ::= ParamMode? Type
ParamMode ::= 'ref' | 'move'
ReturnType ::= '->' Type | '->' '(' Type (',' Type)+ ')'
Modifier  ::= 'private' | 'protected' | 'static' | 'const'
              // 公开可见性是默认语义；final 只作为 class 前缀

TypeParams ::= '<' TypeParam (',' TypeParam)* ','? '>'
TypeParam  ::= Identifier (':' Type ('&' Type)*)?         // 约束用 ':' ，多约束用 '&'
AliasTypeParams ::= '<' Identifier (',' Identifier)* ','? '>'

ClassDecl ::= AttrList? Visibility? 'final'? 'class' Identifier TypeParams?
              ('extends' NamedType)?
              ('implements' NamedType (',' NamedType)*)?
              '{' ClassMember* '}'
ClassMember ::= FieldDecl | MethodDecl | ConstructorDecl
FieldDecl ::= Modifier* Identifier ':' Type ('=' Expression)?
MethodDecl ::= Modifier* Identifier '(' ParamList? ')' ReturnType? Block
            |  Modifier* 'operator' OperatorToken '(' ParamList? ')' ReturnType? Block
ConstructorDecl ::= 'constructor' '(' ParamList? ')' Block

StructDecl ::= AttrList? Visibility? 'packed'? 'struct' Identifier TypeParams?
               ('implements' NamedType (',' NamedType)*)?
               '{' ClassMember* '}'

InterfaceDecl ::= Visibility? 'interface' Identifier TypeParams?
                  ('extends' NamedType (',' NamedType)*)?
                  '{' InterfaceMember* '}'
InterfaceMember ::= Identifier '(' ParamList? ')' ReturnType?

EnumDecl       ::= AttrList? Visibility? 'enum' Identifier TypeParams?
                   ('implements' NamedType (',' NamedType)*)?
                   '{' EnumVariant (',' EnumVariant)* ','? EnumMethod* '}'
EnumVariant    ::= Identifier VariantPayload?
EnumMethod     ::= 'fn' Identifier TypeParams? '(' ParamList? ')' ReturnType? Block
VariantPayload ::= '(' VariantField (',' VariantField)* ')'
VariantField   ::= (Identifier ':')? Type
BackingValue   ::= IntLiteral | FloatLiteral | StringLiteral | BoolLiteral

TypeAliasDecl ::= Visibility? 'type' Identifier AliasTypeParams? '=' Type

ImportDecl ::= 'import' ImportMembers 'from' ImportModule
            |  'import' ImportModule ('as' Identifier)?
ExportDecl ::= 'export' '{' ExportSpec (',' ExportSpec)* ','? '}' 'from' StringLiteral
            |  'export' '*' 'from' StringLiteral
ExportSpec ::= Identifier ('as' Identifier)?
ImportMembers ::= '{' ImportMember (',' ImportMember)* ','? '}'
ImportMember  ::= Identifier ('as' Identifier)?
ImportModule  ::= StringLiteral | Identifier ('/' Identifier)?

AttrList ::= PublicAttribute*
PublicAttribute ::= '@test' ('(' ('skip' | 'timeout' ':' IntegerLiteral) ')')?
                  | '@before_each' | '@after_each' | '@before_all' | '@after_all'
                  | '@deprecated' '(' StringLiteral ')'
                  | '@derive' '(' Identifier (',' Identifier)* ')'

OperatorToken ::= '+' | '-' | '*' | '/' | '%'
               |  '&' | '|' | '^'
               |  '==' | '!=' | '<' | '<=' | '>' | '>='
               |  '[]' | '[]='
               |  '!' | '~'
```

> 注：以上 EBNF 为指导性整理。precedence、associativity、消歧由 parser 实现决议；遇到歧义请以 `src/frontend/parser/xparse_*.c` 为准。
<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## Appendix A. EBNF Grammar

> Source of truth: `src/frontend/parser/xparse_*.c`. This appendix is a compact, curated EBNF; the parser implementation is the authoritative resolver of any conflicts.

### A.1 Lexical Layer

```ebnf
SourceFile ::= Statement*

Comment ::= '//' [^\n]*
         |  '/*' .* '*/'

Identifier ::= IdStart IdContinue*
IdStart    ::= 'a'..'z' | 'A'..'Z' | '_' | NonAsciiUtf8Byte
IdContinue ::= IdStart | '0'..'9'
// The source is first validated as UTF-8. The lexer accepts every non-ASCII UTF-8 byte in identifiers; it does not currently apply XID/NFC normalization.

IntLiteral   ::= DecimalInt | HexInt | BinInt | OctInt
DecimalInt   ::= DecimalDigit ('_'? DecimalDigit)*
HexInt       ::= '0x' HexDigit ('_'? HexDigit)*
BinInt       ::= '0b' ('0' | '1') ('_'? ('0' | '1'))*
OctInt       ::= '0o' ('0'..'7') ('_'? ('0'..'7'))*

FloatLiteral ::= DecimalInt '.' DecimalInt? Exponent?
              |  DecimalInt Exponent
Exponent     ::= ('e' | 'E') ('+' | '-')? DecimalDigit+

BigIntLiteral ::= (DecimalInt | HexInt | BinInt | OctInt) 'n'

QuotedLiteral ::= StringLiteral | FixedByteLiteral
StringLiteral ::= StringPrefix (InlineQuoted | BlockQuoted)
FixedByteLiteral ::= FixedBytePrefix (InlineQuoted | BlockQuoted)
StringPrefix ::= '' | 'r'
FixedBytePrefix ::= 'b' | 'br' | 'c' | 'cr'
InlineQuoted ::= '"' InlinePayload* '"' | '""'
BlockQuoted ::= QuoteRun ImmediateLineEnding BlockBody BlockClose
QuoteRun ::= '"'{Q}                         // Q >= 3
BlockClose ::= LineStart Indent SameQuoteRun (LineEnding | EOF)
CharLiteral ::= "'" CharBody "'"
CharBody ::= UnicodeScalar | EscapeSeq | '\u{' HexDigit{1,6} '}'
RegexLiteral ::= '/' RegexBody '/' RegexFlag*
RegexFlag ::= 'i' | 'm' | 's' | 'g' | 'u'

BoolLiteral ::= 'true' | 'false'
NullLiteral ::= 'null'
```

### A.2 Types

```ebnf
Type ::= UnionType
UnionType ::= IntersectionType ('|' IntersectionType)*
IntersectionType ::= NullableType
NullableType ::= PrimaryType '?'?
PrimaryType ::= ConstType | FFIPointerType | CFunctionType | NamedType | FunctionType | TupleType | ObjectType
ConstType ::= 'const' PrimaryType
FFIPointerType ::= ('Ptr' | 'MutPtr') '<' Type '>'
CFunctionType ::= 'CFn' '<' FunctionType '>'
NamedType   ::= QualifiedIdent TypeArgs?
FunctionType ::= '(' TypeList? ')' '->' Type
TupleType   ::= '(' Type (',' Type)+ ')'
ObjectType  ::= '{' FieldList? '}'
FieldList   ::= ObjectField (',' ObjectField)* ','?
ObjectField ::= Identifier ':' Type
QualifiedIdent ::= Identifier ('.' Identifier)*
TypeArgs    ::= '<' Type (',' Type)* '>'
TypeList    ::= Type (',' Type)*
```

### A.3 Expressions

```ebnf
Expression ::= AssignExpr
AssignExpr ::= TernaryExpr (AssignOp Expression)?
AssignOp   ::= '=' | '+=' | '-=' | '*=' | '/=' | '%='
            |  '&=' | '|=' | '^=' | '<<=' | '>>='

TernaryExpr ::= NullCoalesceExpr ('?' Expression ':' Expression)?
NullCoalesceExpr ::= LogicOrExpr ('??' LogicOrExpr)*
LogicOrExpr ::= LogicAndExpr ('||' LogicAndExpr)*
LogicAndExpr ::= BitOrExpr ('&&' BitOrExpr)*
BitOrExpr   ::= BitXorExpr ('|' BitXorExpr)*
BitXorExpr  ::= BitAndExpr ('^' BitAndExpr)*
BitAndExpr  ::= EqualityExpr ('&' EqualityExpr)*
EqualityExpr ::= RelationalExpr (('==' | '!=') RelationalExpr)*
RelationalExpr ::= RangeExpr ((('<' | '<=' | '>' | '>=') RangeExpr) | (('as' | 'is') Type))*
RangeExpr   ::= ShiftExpr (('..' | '..=') ShiftExpr)?
ShiftExpr   ::= AdditiveExpr (('<<' | '>>') AdditiveExpr)*
AdditiveExpr ::= MultiplicativeExpr (('+' | '-') MultiplicativeExpr)*
MultiplicativeExpr ::= UnaryExpr (('*' | '/' | '%') UnaryExpr)*
// range binds looser than every arithmetic operator, tighter than comparison, and is non-associative (a..b..c is a syntax error). A safe cast is `x as T?`, where T? is nullable.

UnaryExpr ::= ('-' | '+' | '!' | '~') UnaryExpr
           |  'move' UnaryExpr
           |  'await' ('all' | 'any' | 'anySuccess')? UnaryExpr
           |  'go' (Block | PostfixExpr)
           |  'unsafe' Block
           |  PostfixExpr

PostfixExpr ::= Primary PostfixOp*
PostfixOp   ::= '(' ArgList? ')'              // call
             |  '.' Identifier                 // member
             |  '?.' Identifier                 // optional chain property
             |  '?.' Identifier '(' ArgList? ')'  // optional chain method
             |  '?[' Expression ']'            // optional chain index
             |  '[' Expression ']'             // index
             |  '[' Expression? ':' Expression? ']'  // slice
             |  '!'                            // force unwrap

Primary ::= IntLiteral | FloatLiteral | BigIntLiteral
         |  QuotedLiteral | CharLiteral | RegexLiteral
         |  BoolLiteral | NullLiteral
         |  Identifier
         |  ArrayLit | MapLit | SetLit | ObjectLit
         |  BareLambda
         |  ArrowFunction
         |  ComptimeExpr
         |  MatchExpr
         |  '(' Expression ')'
         |  '(' Expression (',' Expression)+ ')'  // tuple

ArrayLit ::= '[' (ArrayElem (',' ArrayElem)* ','?)? ']'
ArrayElem ::= '...' Expression | Expression
MapLit   ::= '#{' (MapEntry (',' MapEntry)* ','?)? '}'
MapEntry ::= Expression ':' Expression
SetLit   ::= '#[' (Expression (',' Expression)* ','?)? ']'
ObjectLit ::= '{' (ObjectFieldExpr (',' ObjectFieldExpr)* ','?)? '}'
ObjectFieldExpr ::= Identifier ':' Expression | Identifier | '...' Expression

BareLambda ::= Identifier '->' (Expression | Block)
ArrowFunction ::= '(' ArrowParams? ')' '->' (Expression | Block)
ArrowParams ::= ArrowParam (',' ArrowParam)*
ArrowParam  ::= Identifier ':' Type
// Note: arrow closures cannot declare an explicit return type;
// use `fn(p: T) -> R { ... }` or annotate the binding (`var f: (T) -> R = ...`) instead.

ComptimeExpr ::= 'comptime' (Expression | Block)

MatchExpr ::= 'match' '(' Expression ')' '{' MatchArm (','? MatchArm)* ','? '}'
MatchArm  ::= Pattern ('if' '(' Expression ')')? '->' (Expression | Block)

ArgList ::= CallArg (',' CallArg)* ','?
CallArg ::= ('ref' | 'out') Expression
          | '...' Expression
          | '_'
          | Expression
```

### A.4 Patterns

```ebnf
Pattern ::= LiteralPattern
         |  RangePattern
         |  EnumPattern
         |  TypePattern
         |  WildcardPattern
         |  BindingPattern
         |  MultiPattern

LiteralPattern  ::= IntLiteral | FloatLiteral | StringLiteral | CharLiteral | BoolLiteral | NullLiteral
RangePattern    ::= Expression ('..' | '..=') Expression
EnumPattern     ::= QualifiedIdent VariantPayloadPattern?    // ADT enum payload destructuring
VariantPayloadPattern ::= '(' Pattern (',' Pattern)* ')'
TypePattern     ::= 'is' Type Identifier?
WildcardPattern ::= '_'
BindingPattern  ::= Identifier
MultiPattern    ::= Pattern (',' Pattern)+
```

### A.5 Statements

```ebnf
Statement ::= ExprStmt
           |  IncDecStmt
           |  VarDecl
           |  ConstDecl
           |  FnDecl
           |  ExternBlock
           |  ClassDecl
           |  StructDecl
           |  InterfaceDecl
           |  EnumDecl
           |  TypeAliasDecl
           |  ImportDecl
           |  ExportDecl
           |  IfStmt
           |  WhileStmt
           |  ForStmt
           |  ForInStmt
           |  ForInPairStmt
           |  MatchStmt
           |  ScopeStmt
           |  SelectStmt
           |  ReturnStmt
           |  BreakStmt
           |  ContinueStmt
           |  ThrowStmt
           |  TryStmt
           |  DeferStmt
           |  YieldStmt
           |  Block
           // Note: print/dump are calls inside ExprStmt; go is an expression (GoExpr)

// LineBreak is not a token: it is the outcome of deciding that a line ending
// terminates the statement here. That decision (previous token can end an
// expression + the new line's first token can begin one + not inside '(' / '[')
// is normative; see §1.2.1 for the full definition.
ExprStmt ::= Expression (';' | LineBreak)
IncDecStmt ::= Identifier ('++' | '--') (';' | LineBreak)
Block    ::= '{' Statement* '}'

IfStmt    ::= 'if' '(' Expression ')' Block ('else' 'if' '(' Expression ')' Block)* ('else' Block)?
LoopLabel ::= Identifier ':'
WhileStmt ::= LoopLabel? 'while' '(' Expression ')' Block
ForStmt   ::= LoopLabel? 'for' '(' VarDecl? ';' Expression? ';' (Expression | Identifier ('++' | '--'))? ')' Block
ForInStmt ::= LoopLabel? 'for' '(' Identifier 'in' Expression ')' Block
ForInPairStmt ::= LoopLabel? 'for' '(' Identifier ',' Identifier 'in' Expression ')' Block
             |  LoopLabel? 'for' '(' '(' Identifier ',' Identifier ')' 'in' Expression ')' Block
MatchStmt ::= 'match' '(' Expression ')' '{' MatchArm (','? MatchArm)* ','? '}'

ReturnStmt   ::= 'return' (Expression | '(' Expression (',' Expression)+ ')')?
BreakStmt    ::= 'break' Identifier?
ContinueStmt ::= 'continue' Identifier?

ThrowStmt ::= 'throw' Expression
TryStmt   ::= 'try' Block CatchClause+
CatchClause ::= 'catch' 'panic'? ('(' Identifier (':' Type)? ')')? Block

DeferStmt ::= 'defer' (Expression | Block)

// print is a normal global function call, syntactically an ExprStmt.

// go is an expression returning Task<T>. It is not a separate statement category (it appears wrapped in ExprStmt).

ScopeStmt ::= ('linked' | 'supervisor')? 'scope' Block

SelectStmt ::= 'select' '{' SelectArm+ '}'
SelectArm  ::= Identifier 'from' Expression '->' Block      // receive
            |  Expression 'to' Expression '->' Block        // send
            |  'after' Expression '->' Block                // timeout
            |  '_' '->' Block                                // default

YieldStmt ::= 'yield' Expression
```

### A.6 Declarations

```ebnf
Visibility ::= 'export'
VarDecl ::= 'var' Binding
ConstDecl ::= AttrList? Visibility? 'const' Binding
Binding ::= BindingPattern (':' Type)? ('=' Expression)?
BindingPattern ::= Identifier
                |  '[' BindingPattern (',' BindingPattern)* ','? ']'
                |  '(' BindingPattern (',' BindingPattern)+ ','? ')'
                |  '{' ObjectBinding (',' ObjectBinding)* ','? '}'
ObjectBinding ::= Identifier (':' Identifier)?

FnDecl ::= AttrList? Visibility? 'fn' Identifier TypeParams? '(' ParamList? ')' ReturnType? Block
ExternBlock ::= 'extern' '"C"' '{' ExternFnDecl+ '}'
ExternFnDecl ::= Visibility? 'fn' Identifier '(' ParamList? ')' ReturnType? ';'?
ParamList ::= Param (',' Param)* ','?
Param     ::= Identifier ':' ParamType ('=' Expression)?
           |  '...' Identifier ':' Type
ParamType ::= ParamMode? Type
ParamMode ::= 'ref' | 'move'
ReturnType ::= '->' Type | '->' '(' Type (',' Type)+ ')'
Modifier  ::= 'private' | 'protected' | 'static' | 'const'
              // public visibility is the default; final is only a class prefix

TypeParams ::= '<' TypeParam (',' TypeParam)* ','? '>'
TypeParam  ::= Identifier (':' Type ('&' Type)*)?         // constraints use ':', multiple use '&'
AliasTypeParams ::= '<' Identifier (',' Identifier)* ','? '>'

ClassDecl ::= AttrList? Visibility? 'final'? 'class' Identifier TypeParams?
              ('extends' NamedType)?
              ('implements' NamedType (',' NamedType)*)?
              '{' ClassMember* '}'
ClassMember ::= FieldDecl | MethodDecl | ConstructorDecl
FieldDecl ::= Modifier* Identifier ':' Type ('=' Expression)?
MethodDecl ::= Modifier* Identifier '(' ParamList? ')' ReturnType? Block
            |  Modifier* 'operator' OperatorToken '(' ParamList? ')' ReturnType? Block
ConstructorDecl ::= 'constructor' '(' ParamList? ')' Block

StructDecl ::= AttrList? Visibility? 'packed'? 'struct' Identifier TypeParams?
               ('implements' NamedType (',' NamedType)*)?
               '{' ClassMember* '}'

InterfaceDecl ::= Visibility? 'interface' Identifier TypeParams?
                  ('extends' NamedType (',' NamedType)*)?
                  '{' InterfaceMember* '}'
InterfaceMember ::= Identifier '(' ParamList? ')' ReturnType?

EnumDecl       ::= AttrList? Visibility? 'enum' Identifier TypeParams?
                   ('implements' NamedType (',' NamedType)*)?
                   '{' EnumVariant (',' EnumVariant)* ','? EnumMethod* '}'
EnumVariant    ::= Identifier VariantPayload?
EnumMethod     ::= 'fn' Identifier TypeParams? '(' ParamList? ')' ReturnType? Block
VariantPayload ::= '(' VariantField (',' VariantField)* ')'
VariantField   ::= (Identifier ':')? Type
BackingValue   ::= IntLiteral | FloatLiteral | StringLiteral | BoolLiteral

TypeAliasDecl ::= Visibility? 'type' Identifier AliasTypeParams? '=' Type

ImportDecl ::= 'import' ImportMembers 'from' ImportModule
            |  'import' ImportModule ('as' Identifier)?
ExportDecl ::= 'export' '{' ExportSpec (',' ExportSpec)* ','? '}' 'from' StringLiteral
            |  'export' '*' 'from' StringLiteral
ExportSpec ::= Identifier ('as' Identifier)?
ImportMembers ::= '{' ImportMember (',' ImportMember)* ','? '}'
ImportMember  ::= Identifier ('as' Identifier)?
ImportModule  ::= StringLiteral | Identifier ('/' Identifier)?

AttrList ::= PublicAttribute*
PublicAttribute ::= '@test' ('(' ('skip' | 'timeout' ':' IntegerLiteral) ')')?
                  | '@before_each' | '@after_each' | '@before_all' | '@after_all'
                  | '@deprecated' '(' StringLiteral ')'
                  | '@derive' '(' Identifier (',' Identifier)* ')'

OperatorToken ::= '+' | '-' | '*' | '/' | '%'
               |  '&' | '|' | '^'
               |  '==' | '!=' | '<' | '<=' | '>' | '>='
               |  '[]' | '[]='
               |  '!' | '~'
```

> Note: this EBNF is curated for guidance. Precedence, associativity, and disambiguation are determined by the parser implementation; in case of ambiguity, treat `src/frontend/parser/xparse_*.c` as authoritative.
<!-- /xr-spec:en -->
