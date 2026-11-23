---
id: spec.appendix_a_ebnf
order: 020
---

<!-- xr-spec:cn -->
---

## 附录 A. EBNF 语法

> 规范源：`spec/source/grammar/language.ebnf`。正文与本附录使用同一组产生式。解析器实现须接受一致性测试；目标规则的存在不表示当前实现已经支持。

### A.1 词法层

<!-- xr-source:grammar 020-appendix-a-ebnf-grammar-01 -->
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
              |  '.' DecimalInt Exponent?
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
RegexLiteral ::= 're' (InlineQuoted | BlockQuoted)
BoolLiteral ::= 'true' | 'false'
NullLiteral ::= 'null'
```
<!-- /xr-source -->

### A.2 类型

<!-- xr-source:grammar 020-appendix-a-ebnf-grammar-02 -->
```ebnf
Type ::= UnionType
UnionType ::= IntersectionType ('|' IntersectionType)*
IntersectionType ::= NullableType
NullableType ::= PrimaryType '?'?
PrimaryType ::= FFIPointerType | CFunctionType | NamedType | FunctionType | TupleType
FFIPointerType ::= ('Ptr' | 'MutPtr') '<' Type '>'
CFunctionType ::= 'CFn' '<' FunctionType '>'
NamedType   ::= QualifiedIdent TypeArgs?
FunctionType ::= 'nosuspend'? 'fn' '(' FunctionTypeParams? ')' ThrowsClause? ReturnType?
FunctionTypeParams ::= ParamType (',' ParamType)* ','?
                     | (ParamType ',')* '...' Type ','?
TupleType   ::= '(' Type (',' Type)+ ')'
QualifiedIdent ::= Identifier ('.' Identifier)*
TypeArgs    ::= '<' Type (',' Type)* '>'
TypeList    ::= Type (',' Type)*
```
<!-- /xr-source -->

### A.3 表达式

<!-- xr-source:grammar 020-appendix-a-ebnf-grammar-03 -->
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
           |  TryExpr
           |  'copy' UnaryExpr
           |  'move' UnaryExpr
           |  'await' UnaryExpr
           |  GoExpr
           |  'unsafe' Block
           |  PostfixExpr
TryExpr   ::= 'try' ('?' | '!')? TernaryExpr
GoExpr    ::= 'go' GoOptions? CallExpr
GoOptions ::= '(' GoOption (',' GoOption)* ')'
GoOption ::= 'name' ':' StringLiteral | 'main' | 'background' | 'pinned'
CallExpr  ::= Primary PostfixOp* '(' ArgList? ')'
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
         |  ArrayLit | MapLit | StructLit
         |  ArrowLambda
         |  FnExpression
         |  ComptimeExpr
         |  MatchExpr
         |  '(' Expression ')'
         |  '(' Expression (',' Expression)+ ')'  // tuple
ArrayLit ::= '[' (ArrayElem (',' ArrayElem)* ','?)? ']'
ArrayElem ::= '...' Expression | Expression
MapLit   ::= '{' (MapEntry (',' MapEntry)* ','?)? '}'
MapEntry ::= '...' Expression | Expression ':' Expression
StructLit ::= NamedType '{' (StructEntry (',' StructEntry)* ','?)? '}'
StructEntry ::= '...' Expression | Identifier ':' Expression
ArrowLambda ::= Identifier '=>' (Expression | Block)
              | '(' ArrowParams? ')' '=>' (Expression | Block)
ArrowParams ::= ArrowParam (',' ArrowParam)* ','?
ArrowParam  ::= Identifier (':' ParamType)?
FnExpression ::= 'fn' TypeParams? '(' ArrowParams? ')' ThrowsClause? ReturnType? Block
// Note: arrow lambdas cannot declare an explicit return type;
// use `fn(p: T) -> R { ... }` or annotate the binding (`var f: fn(T) -> R = ...`) instead.
ComptimeExpr ::= 'comptime' (Expression | Block)
MatchExpr ::= 'match' '(' Expression ')' '{' MatchArm (',' MatchArm)* ','? '}'
MatchArm  ::= Pattern ('if' '(' Expression ')')? '=>' (Expression | Block)
ArgList ::= CallArg (',' CallArg)* ','?
CallArg ::= NamedCallArg | PositionalCallArg | '...' Expression | '_'
NamedCallArg ::= Identifier ':' ArgumentValue
PositionalCallArg ::= ArgumentValue
ArgumentValue ::= ('ref' | 'move')? Expression
```
<!-- /xr-source -->

`try` 后的下一个 token 为 `{` 时只解析为 `TryStmt`，后接 `scope` 时解析为 `TryScopeStmt`（§10.7），其余情况解析为 `TryExpr`；`try?` / `try!` 不能修饰代码块。`TryExpr` 的操作数从当前位置覆盖到当前表达式边界，且不包含未加括号的赋值运算符。括号、参数分隔符和语句结束仍限定表达式范围。`try?` 与同一未加括号的 `??` 表达式连用时报错，应写 `(try? expr) ?? fallback`。等待的标记顺序是 `try await task`，不是 `await try task`。

函数类型返回 Unit 时省略 `ReturnType`，如 `fn()`、`fn() throws(E)`；类型位置不接受显式 `-> ()`（§2.8）。`extern "C"` 声明与 `CFn` 回调必须不抛错，不能携带抛错签名（§5.2.9）。

### A.4 模式

<!-- xr-source:grammar 020-appendix-a-ebnf-grammar-04 -->
```ebnf
Pattern ::= LiteralPattern
         |  RangePattern
         |  EnumPattern
         |  TypePattern
         |  WildcardPattern
         |  BindingPattern
         |  MultiPattern
         |  TuplePattern
         |  ArrayPattern
LiteralPattern  ::= IntLiteral | FloatLiteral | StringLiteral | CharLiteral | BoolLiteral | NullLiteral
RangePattern    ::= Expression ('..' | '..=') Expression
EnumPattern     ::= EnumVariantPath
                  | EnumVariantPath '{' EnumFieldPatternList? '}'
EnumVariantPath ::= QualifiedIdent
                  | QualifiedIdent TypeArgs '.' Identifier
EnumFieldPatternList ::= EnumFieldPattern (',' EnumFieldPattern)* ','?
EnumFieldPattern ::= Identifier | Identifier ':' Pattern
TypePattern     ::= 'is' Type Identifier?
WildcardPattern ::= '_'
BindingPattern  ::= Identifier
MultiPattern    ::= Pattern (',' Pattern)+
TuplePattern ::= '(' ')' | '(' Pattern ',' (Pattern (',' Pattern)* ','?)? ')'
ArrayPattern ::= '[' (ArrayPatternItem (',' ArrayPatternItem)* ','?)? ']'
ArrayPatternItem ::= Identifier | '_' | '..' Identifier?
```
<!-- /xr-source -->

### A.5 语句

<!-- xr-source:grammar 020-appendix-a-ebnf-grammar-05 -->
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
           |  ForRefStmt
           |  ForInTupleStmt
           |  ForInPairStmt
           |  MatchStmt
           |  ScopeStmt
           |  TryScopeStmt
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
ForStmt ::= LoopLabel? 'for' '(' ForInit? ';' Expression? ';' ForStep? ')' Block
ForInStmt ::= LoopLabel? 'for' '(' Identifier 'in' Expression ')' Block
ForInPairStmt ::= LoopLabel? 'for' '(' Identifier ',' Identifier 'in' Expression ')' Block
ForRefStmt ::= LoopLabel? 'for' '(' 'ref' Identifier 'in' Expression ')' Block
             | LoopLabel? 'for' '(' Identifier ',' 'ref' Identifier 'in' Expression ')' Block
MatchStmt ::= 'match' '(' Expression ')' '{' MatchArm (',' MatchArm)* ','? '}'
ReturnStmt   ::= 'return' (Expression | '(' Expression (',' Expression)+ ')')?
BreakStmt    ::= 'break' Identifier?
ContinueStmt ::= 'continue' Identifier?
ThrowStmt ::= 'throw' Expression
TryStmt   ::= 'try' Block CatchClause+
CatchClause ::= 'catch' '(' CatchHeader ')' Block
             |  'catch' 'panic' ('(' Identifier (':' Type)? ')')? Block
CatchHeader ::= Identifier (':' Type)?
             |  EnumPattern (',' EnumPattern)*
DeferStmt ::= 'defer' Block

// print is a normal global function call, syntactically an ExprStmt.

// go is an expression returning Task<T, E>. It is not a separate statement category (it appears wrapped in ExprStmt).
ScopeStmt ::= 'scope' ('(' 'isolated' ')')? Block
TryScopeStmt ::= 'try' ScopeStmt
SelectStmt ::= 'select' '{' SelectArm+ '}'
SelectArm  ::= Identifier 'from' Expression '=>' Block      // receive
            |  Expression 'to' Expression '=>' Block        // send
            |  'after' Expression '=>' Block                // timeout
            |  '_' '=>' Block                                // default
YieldStmt ::= 'yield' Expression
```
<!-- /xr-source -->

### A.6 声明

<!-- xr-source:grammar 020-appendix-a-ebnf-grammar-06 -->
```ebnf
Visibility ::= 'export'
VarDecl ::= 'var' Binding
ConstDecl ::= AttrList? Visibility? 'const' Binding
Binding ::= DeclarationPattern (':' Type)? ('=' Expression)?
DeclarationPattern ::= Identifier
                |  '[' DeclarationPattern (',' DeclarationPattern)* ','? ']'
                |  '(' DeclarationPattern (',' DeclarationPattern)+ ','? ')'
FnDecl ::= AttrList? Visibility? 'fn' Identifier TypeParams? '(' ParamList? ')' ThrowsClause? ReturnType? Block
ExternBlock ::= 'extern' '"C"' '{' ExternFnDecl+ '}'
ExternFnDecl ::= Visibility? 'fn' Identifier '(' ParamList? ')' ReturnType? ';'?
ParamList ::= Param (',' Param)* ','?
Param     ::= Identifier ':' ParamType ('=' Expression)?
           |  '...' Identifier ':' Type
ParamType ::= 'escaping'? ParamMode? Type
// escaping is valid only for function-typed parameters; see the parameter rules.
ParamMode ::= 'ref' | 'move'
ThrowsClause ::= 'throws' ('(' Type ')')?
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
ClassMember ::= FieldDecl | MethodDecl | InitDecl | StaticBlock
StaticBlock ::= 'static' Block
FieldDecl ::= Modifier* Identifier ':' Type ('=' Expression)?
MethodDecl ::= Modifier* Identifier '(' ParamList? ')' ThrowsClause? ReturnType? Block
InitDecl ::= 'init' '(' ParamList? ')' ThrowsClause? Block
StructDecl ::= AttrList? Visibility? 'packed'? 'struct' Identifier TypeParams?
               ('align' '(' IntegerLiteral ')')?
               ('implements' NamedType (',' NamedType)*)?
               '{' ClassMember* '}'
InterfaceDecl ::= Visibility? 'interface' Identifier TypeParams?
                  ('extends' NamedType (',' NamedType)*)?
                  '{' InterfaceMember* '}'
InterfaceMember ::= Identifier TypeParams? '(' ParamList? ')' ThrowsClause? ReturnType? WhereClause?
EnumDecl       ::= AttrList? Visibility? 'enum' Identifier TypeParams?
                   ('implements' NamedType (',' NamedType)*)?
                   '{' EnumVariant (',' EnumVariant)* ','? EnumMethod* '}'
EnumVariant    ::= Identifier
                 | Identifier '{' EnumFieldDecl (',' EnumFieldDecl)* ','? '}'
EnumFieldDecl  ::= Identifier ':' Type
EnumMethod     ::= ('ref' | 'move')? Identifier TypeParams? '(' ParamList? ')' ThrowsClause? ReturnType? Block
                 | 'static' Identifier TypeParams? '(' ParamList? ')' ThrowsClause? ReturnType? Block
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
```
<!-- /xr-source -->

> 本文法与正文中的优先级、结合性和消歧约束共同定义目标语法。冲突须修正规范或实现，不能以当前解析器自动裁决；有限测试也不构成全语法等价证明。
<!-- /xr-spec:cn -->

<!-- xr-spec:en -->
---

## Appendix A. EBNF Grammar

> Normative source: `spec/source/grammar/language.ebnf`. Chapters and this appendix share productions. Parser conformance requires tests; a target rule does not establish current implementation support.

### A.1 Lexical Layer

<!-- xr-source:grammar 020-appendix-a-ebnf-grammar-01 -->
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
              |  '.' DecimalInt Exponent?
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
RegexLiteral ::= 're' (InlineQuoted | BlockQuoted)
BoolLiteral ::= 'true' | 'false'
NullLiteral ::= 'null'
```
<!-- /xr-source -->

### A.2 Types

<!-- xr-source:grammar 020-appendix-a-ebnf-grammar-02 -->
```ebnf
Type ::= UnionType
UnionType ::= IntersectionType ('|' IntersectionType)*
IntersectionType ::= NullableType
NullableType ::= PrimaryType '?'?
PrimaryType ::= FFIPointerType | CFunctionType | NamedType | FunctionType | TupleType
FFIPointerType ::= ('Ptr' | 'MutPtr') '<' Type '>'
CFunctionType ::= 'CFn' '<' FunctionType '>'
NamedType   ::= QualifiedIdent TypeArgs?
FunctionType ::= 'nosuspend'? 'fn' '(' FunctionTypeParams? ')' ThrowsClause? ReturnType?
FunctionTypeParams ::= ParamType (',' ParamType)* ','?
                     | (ParamType ',')* '...' Type ','?
TupleType   ::= '(' Type (',' Type)+ ')'
QualifiedIdent ::= Identifier ('.' Identifier)*
TypeArgs    ::= '<' Type (',' Type)* '>'
TypeList    ::= Type (',' Type)*
```
<!-- /xr-source -->

### A.3 Expressions

<!-- xr-source:grammar 020-appendix-a-ebnf-grammar-03 -->
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
           |  TryExpr
           |  'copy' UnaryExpr
           |  'move' UnaryExpr
           |  'await' UnaryExpr
           |  GoExpr
           |  'unsafe' Block
           |  PostfixExpr
TryExpr   ::= 'try' ('?' | '!')? TernaryExpr
GoExpr    ::= 'go' GoOptions? CallExpr
GoOptions ::= '(' GoOption (',' GoOption)* ')'
GoOption ::= 'name' ':' StringLiteral | 'main' | 'background' | 'pinned'
CallExpr  ::= Primary PostfixOp* '(' ArgList? ')'
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
         |  ArrayLit | MapLit | StructLit
         |  ArrowLambda
         |  FnExpression
         |  ComptimeExpr
         |  MatchExpr
         |  '(' Expression ')'
         |  '(' Expression (',' Expression)+ ')'  // tuple
ArrayLit ::= '[' (ArrayElem (',' ArrayElem)* ','?)? ']'
ArrayElem ::= '...' Expression | Expression
MapLit   ::= '{' (MapEntry (',' MapEntry)* ','?)? '}'
MapEntry ::= '...' Expression | Expression ':' Expression
StructLit ::= NamedType '{' (StructEntry (',' StructEntry)* ','?)? '}'
StructEntry ::= '...' Expression | Identifier ':' Expression
ArrowLambda ::= Identifier '=>' (Expression | Block)
              | '(' ArrowParams? ')' '=>' (Expression | Block)
ArrowParams ::= ArrowParam (',' ArrowParam)* ','?
ArrowParam  ::= Identifier (':' ParamType)?
FnExpression ::= 'fn' TypeParams? '(' ArrowParams? ')' ThrowsClause? ReturnType? Block
// Note: arrow lambdas cannot declare an explicit return type;
// use `fn(p: T) -> R { ... }` or annotate the binding (`var f: fn(T) -> R = ...`) instead.
ComptimeExpr ::= 'comptime' (Expression | Block)
MatchExpr ::= 'match' '(' Expression ')' '{' MatchArm (',' MatchArm)* ','? '}'
MatchArm  ::= Pattern ('if' '(' Expression ')')? '=>' (Expression | Block)
ArgList ::= CallArg (',' CallArg)* ','?
CallArg ::= NamedCallArg | PositionalCallArg | '...' Expression | '_'
NamedCallArg ::= Identifier ':' ArgumentValue
PositionalCallArg ::= ArgumentValue
ArgumentValue ::= ('ref' | 'move')? Expression
```
<!-- /xr-source -->

When the next token after `try` is `{`, parse only `TryStmt`; before `scope`, parse `TryScopeStmt` (§10.7); otherwise parse `TryExpr`. `try?` / `try!` cannot modify blocks. A `TryExpr` operand extends to the current expression boundary without consuming an unparenthesized assignment operator. Parentheses, argument separators, and statement endings still bound expressions. An unparenthesized combination of `try?` and `??` is an error; write `(try? expr) ?? fallback`. The ordering for marked waits is `try await task`, not `await try task`.

A Unit-returning function type omits `ReturnType`, as in `fn()` or `fn() throws(E)`; explicit `-> ()` is not allowed in type position (§2.8). `extern "C"` declarations and `CFn` callbacks must be non-throwing and cannot carry throwing signatures (§5.2.9).

### A.4 Patterns

<!-- xr-source:grammar 020-appendix-a-ebnf-grammar-04 -->
```ebnf
Pattern ::= LiteralPattern
         |  RangePattern
         |  EnumPattern
         |  TypePattern
         |  WildcardPattern
         |  BindingPattern
         |  MultiPattern
         |  TuplePattern
         |  ArrayPattern
LiteralPattern  ::= IntLiteral | FloatLiteral | StringLiteral | CharLiteral | BoolLiteral | NullLiteral
RangePattern    ::= Expression ('..' | '..=') Expression
EnumPattern     ::= EnumVariantPath
                  | EnumVariantPath '{' EnumFieldPatternList? '}'
EnumVariantPath ::= QualifiedIdent
                  | QualifiedIdent TypeArgs '.' Identifier
EnumFieldPatternList ::= EnumFieldPattern (',' EnumFieldPattern)* ','?
EnumFieldPattern ::= Identifier | Identifier ':' Pattern
TypePattern     ::= 'is' Type Identifier?
WildcardPattern ::= '_'
BindingPattern  ::= Identifier
MultiPattern    ::= Pattern (',' Pattern)+
TuplePattern ::= '(' ')' | '(' Pattern ',' (Pattern (',' Pattern)* ','?)? ')'
ArrayPattern ::= '[' (ArrayPatternItem (',' ArrayPatternItem)* ','?)? ']'
ArrayPatternItem ::= Identifier | '_' | '..' Identifier?
```
<!-- /xr-source -->

### A.5 Statements

<!-- xr-source:grammar 020-appendix-a-ebnf-grammar-05 -->
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
           |  ForRefStmt
           |  ForInTupleStmt
           |  ForInPairStmt
           |  MatchStmt
           |  ScopeStmt
           |  TryScopeStmt
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
ForStmt ::= LoopLabel? 'for' '(' ForInit? ';' Expression? ';' ForStep? ')' Block
ForInStmt ::= LoopLabel? 'for' '(' Identifier 'in' Expression ')' Block
ForInPairStmt ::= LoopLabel? 'for' '(' Identifier ',' Identifier 'in' Expression ')' Block
ForRefStmt ::= LoopLabel? 'for' '(' 'ref' Identifier 'in' Expression ')' Block
             | LoopLabel? 'for' '(' Identifier ',' 'ref' Identifier 'in' Expression ')' Block
MatchStmt ::= 'match' '(' Expression ')' '{' MatchArm (',' MatchArm)* ','? '}'
ReturnStmt   ::= 'return' (Expression | '(' Expression (',' Expression)+ ')')?
BreakStmt    ::= 'break' Identifier?
ContinueStmt ::= 'continue' Identifier?
ThrowStmt ::= 'throw' Expression
TryStmt   ::= 'try' Block CatchClause+
CatchClause ::= 'catch' '(' CatchHeader ')' Block
             |  'catch' 'panic' ('(' Identifier (':' Type)? ')')? Block
CatchHeader ::= Identifier (':' Type)?
             |  EnumPattern (',' EnumPattern)*
DeferStmt ::= 'defer' Block

// print is a normal global function call, syntactically an ExprStmt.

// go is an expression returning Task<T, E>. It is not a separate statement category (it appears wrapped in ExprStmt).
ScopeStmt ::= 'scope' ('(' 'isolated' ')')? Block
TryScopeStmt ::= 'try' ScopeStmt
SelectStmt ::= 'select' '{' SelectArm+ '}'
SelectArm  ::= Identifier 'from' Expression '=>' Block      // receive
            |  Expression 'to' Expression '=>' Block        // send
            |  'after' Expression '=>' Block                // timeout
            |  '_' '=>' Block                                // default
YieldStmt ::= 'yield' Expression
```
<!-- /xr-source -->

### A.6 Declarations

<!-- xr-source:grammar 020-appendix-a-ebnf-grammar-06 -->
```ebnf
Visibility ::= 'export'
VarDecl ::= 'var' Binding
ConstDecl ::= AttrList? Visibility? 'const' Binding
Binding ::= DeclarationPattern (':' Type)? ('=' Expression)?
DeclarationPattern ::= Identifier
                |  '[' DeclarationPattern (',' DeclarationPattern)* ','? ']'
                |  '(' DeclarationPattern (',' DeclarationPattern)+ ','? ')'
FnDecl ::= AttrList? Visibility? 'fn' Identifier TypeParams? '(' ParamList? ')' ThrowsClause? ReturnType? Block
ExternBlock ::= 'extern' '"C"' '{' ExternFnDecl+ '}'
ExternFnDecl ::= Visibility? 'fn' Identifier '(' ParamList? ')' ReturnType? ';'?
ParamList ::= Param (',' Param)* ','?
Param     ::= Identifier ':' ParamType ('=' Expression)?
           |  '...' Identifier ':' Type
ParamType ::= 'escaping'? ParamMode? Type
// escaping is valid only for function-typed parameters; see the parameter rules.
ParamMode ::= 'ref' | 'move'
ThrowsClause ::= 'throws' ('(' Type ')')?
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
ClassMember ::= FieldDecl | MethodDecl | InitDecl | StaticBlock
StaticBlock ::= 'static' Block
FieldDecl ::= Modifier* Identifier ':' Type ('=' Expression)?
MethodDecl ::= Modifier* Identifier '(' ParamList? ')' ThrowsClause? ReturnType? Block
InitDecl ::= 'init' '(' ParamList? ')' ThrowsClause? Block
StructDecl ::= AttrList? Visibility? 'packed'? 'struct' Identifier TypeParams?
               ('align' '(' IntegerLiteral ')')?
               ('implements' NamedType (',' NamedType)*)?
               '{' ClassMember* '}'
InterfaceDecl ::= Visibility? 'interface' Identifier TypeParams?
                  ('extends' NamedType (',' NamedType)*)?
                  '{' InterfaceMember* '}'
InterfaceMember ::= Identifier TypeParams? '(' ParamList? ')' ThrowsClause? ReturnType? WhereClause?
EnumDecl       ::= AttrList? Visibility? 'enum' Identifier TypeParams?
                   ('implements' NamedType (',' NamedType)*)?
                   '{' EnumVariant (',' EnumVariant)* ','? EnumMethod* '}'
EnumVariant    ::= Identifier
                 | Identifier '{' EnumFieldDecl (',' EnumFieldDecl)* ','? '}'
EnumFieldDecl  ::= Identifier ':' Type
EnumMethod     ::= ('ref' | 'move')? Identifier TypeParams? '(' ParamList? ')' ThrowsClause? ReturnType? Block
                 | 'static' Identifier TypeParams? '(' ParamList? ')' ThrowsClause? ReturnType? Block
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
```
<!-- /xr-source -->

> These productions and the prose precedence, associativity and disambiguation constraints define the target syntax. Disagreements require correction, not automatic preference for the parser. Finite tests do not prove whole-grammar equivalence.
<!-- /xr-spec:en -->
