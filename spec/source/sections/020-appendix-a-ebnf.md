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
IdStart    ::= 'a'..'z' | 'A'..'Z' | '_'
IdContinue ::= IdStart | '0'..'9'

IntLiteral   ::= DecimalInt | HexInt | BinInt | OctInt
DecimalInt   ::= DecimalDigit ('_'? DecimalDigit)*
HexInt       ::= '0x' HexDigit ('_'? HexDigit)*
BinInt       ::= '0b' ('0' | '1') ('_'? ('0' | '1'))*
OctInt       ::= '0o' ('0'..'7') ('_'? ('0'..'7'))*

FloatLiteral ::= DecimalInt '.' DecimalInt? Exponent?
              |  DecimalInt Exponent
Exponent     ::= ('e' | 'E') ('+' | '-')? DecimalDigit+

BigIntLiteral ::= DecimalInt 'n'

StringLiteral ::= '"' StringChar* '"'
                | "'" StringChar* "'"
RawStringLiteral ::= 'r' '"' [^"]* '"'
RegexLiteral ::= '/' RegexBody '/' RegexFlags?

BoolLiteral ::= 'true' | 'false'
NullLiteral ::= 'null'
```

### A.2 类型

```ebnf
Type ::= UnionType
UnionType ::= IntersectionType ('|' IntersectionType)*
IntersectionType ::= NullableType
NullableType ::= PrimaryType '?'?
PrimaryType ::= FFIPointerType | CFunctionType | NamedType | FunctionType | TupleType | ObjectType
FFIPointerType ::= ('RawPtr' | 'RawMut') '<' Type '>'
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

TernaryExpr ::= LogicOrExpr ('?' Expression ':' Expression)?
LogicOrExpr ::= LogicAndExpr ('||' LogicAndExpr)*
            |   NullCoalesce
LogicAndExpr ::= BitOrExpr ('&&' BitOrExpr)*
NullCoalesce ::= LogicAndExpr ('??' LogicAndExpr)*
BitOrExpr   ::= BitXorExpr ('|' BitXorExpr)*
BitXorExpr  ::= BitAndExpr ('^' BitAndExpr)*
BitAndExpr  ::= EqualityExpr ('&' EqualityExpr)*
EqualityExpr ::= RelationalExpr (('==' | '!=') RelationalExpr)*
RelationalExpr ::= ShiftExpr (('<' | '<=' | '>' | '>=') ShiftExpr)*
ShiftExpr   ::= AdditiveExpr (('<<' | '>>') AdditiveExpr)*
AdditiveExpr ::= MultiplicativeExpr (('+' | '-') MultiplicativeExpr)*
MultiplicativeExpr ::= TypeOpExpr (('*' | '/' | '%') TypeOpExpr)*
TypeOpExpr  ::= UnaryExpr (('as' | 'is') Type)*           // 安全转换写为 `x as T?`，T? 是可空类型
RangeExpr   ::= AdditiveExpr ('..' AdditiveExpr)?

UnaryExpr ::= ('-' | '+' | '!' | '~') UnaryExpr
           |  'new' QualifiedIdent TypeArgs? '(' ArgList? ')'
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
         |  StringLiteral | RawStringLiteral | RegexLiteral
         |  BoolLiteral | NullLiteral
         |  Identifier
         |  ArrayLit | MapLit | SetLit | ObjectLit
         |  ArrowFunction
         |  MatchExpr
         |  '(' Expression ')'
         |  '(' Expression (',' Expression)+ ')'  // tuple

ArrayLit ::= '[' (Expression (',' Expression)* ','?)? ']'
MapLit   ::= '#{' (MapEntry (',' MapEntry)* ','?)? '}'
MapEntry ::= Expression ':' Expression
SetLit   ::= '#[' (Expression (',' Expression)* ','?)? ']'
ObjectLit ::= '{' (ObjectFieldExpr (',' ObjectFieldExpr)* ','?)? '}'
ObjectFieldExpr ::= Identifier ':' Expression | Identifier

ArrowFunction ::= '(' ArrowParams? ')' '->' (Expression | Block)
ArrowParams ::= ArrowParam (',' ArrowParam)*
ArrowParam  ::= Identifier ':' Type
// Note: arrow closures cannot declare an explicit return type;
// use `fn(p: T) -> R { ... }` or annotate the binding (`let f: (T) -> R = ...`) instead.

MatchExpr ::= 'match' '(' Expression ')' '{' MatchArm (','? MatchArm)* ','? '}'
MatchArm  ::= Pattern ('if' '(' Expression ')')? '->' (Expression | Block)

ThrowExpr   ::= 'throw' Expression                // operand 静态类型必须是 enum 变体

ArgList ::= Expression (',' Expression)* ','?
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

LiteralPattern  ::= IntLiteral | FloatLiteral | StringLiteral | BoolLiteral | NullLiteral
RangePattern    ::= Expression '..' Expression
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
           |  FnDecl
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

ScopeStmt ::= 'scope' Block            // 词法作用域 + 结构化并发

SelectStmt ::= 'select' '{' SelectArm+ '}'
SelectArm  ::= Identifier 'from' Expression '->' Block      // 接收
            |  Expression 'to' Expression '->' Block        // 发送
            |  'after' Expression '->' Block                // 超时
            |  '_' '->' Block                                // 默认

YieldStmt ::= 'yield'
```

### A.6 声明

```ebnf
VarDecl ::= ('let' | 'const' | 'shared' ('const' | 'let')) Binding (',' Binding)*
Binding ::= BindingPattern (':' Type)? ('=' Expression)?
BindingPattern ::= Identifier
                |  '[' BindingPattern (',' BindingPattern)* ','? ']'
                |  '(' BindingPattern (',' BindingPattern)+ ','? ')'
                |  '{' Identifier (',' Identifier)* ','? '}'

FnDecl ::= AttrList? Modifier* 'fn' Identifier TypeParams? '(' ParamList? ')' ReturnType? FnBody
FnBody ::= Block | ';'?                         // 空函数体仅允许 @extern
ParamList ::= Param (',' Param)* ','?
Param     ::= Modifier* Identifier ':' Type ('=' Expression)?
           |  '...' Identifier ':' Type
ReturnType ::= '->' Type | '->' '(' Type (',' Type)+ ')'
Modifier  ::= 'in' | 'ref' | 'private' | 'static' | 'final' | 'abstract' | 'override'
              // 公开可见性是默认语义；override 可选

TypeParams ::= '<' TypeParam (',' TypeParam)* ','? '>'
TypeParam  ::= Identifier (':' Type ('&' Type)*)?         // 约束用 ':' ，多约束用 '&'
AliasTypeParams ::= '<' Identifier (',' Identifier)* ','? '>'

ClassDecl ::= Modifier* 'class' Identifier TypeParams?
              ('extends' NamedType)?
              ('implements' NamedType (',' NamedType)*)?
              '{' ClassMember* '}'
ClassMember ::= FieldDecl | MethodDecl | ConstructorDecl
FieldDecl ::= Modifier* Identifier ':' Type ('=' Expression)?
MethodDecl ::= Modifier* Identifier '(' ParamList? ')' ReturnType? Block
            |  Modifier* 'operator' OperatorToken '(' ParamList? ')' ReturnType? Block
ConstructorDecl ::= 'constructor' '(' ParamList? ')' Block

StructDecl ::= 'struct' Identifier TypeParams?
               ('implements' NamedType (',' NamedType)*)?
               '{' ClassMember* '}'

InterfaceDecl ::= 'interface' Identifier TypeParams?
                  ('extends' NamedType (',' NamedType)*)?
                  '{' InterfaceMember* '}'
InterfaceMember ::= Identifier '(' ParamList? ')' ReturnType?

EnumDecl       ::= 'enum' Identifier TypeParams?
                   ('implements' NamedType (',' NamedType)*)?
                   '{' EnumVariant (',' EnumVariant)* ','? EnumMethod* '}'
EnumVariant    ::= Identifier VariantPayload?
                |  Identifier '=' BackingValue                  // 简单枚举（无 payload）
EnumMethod     ::= 'fn' Identifier TypeParams? '(' ParamList? ')' ReturnType? Block
VariantPayload ::= '(' VariantField (',' VariantField)* ')'
VariantField   ::= (Identifier ':')? Type
BackingValue   ::= IntLiteral | FloatLiteral | StringLiteral | BoolLiteral

TypeAliasDecl ::= 'type' Identifier AliasTypeParams? '=' Type

ImportDecl ::= 'import' ImportMembers 'from' ImportModule
            |  'import' ImportModule ('as' Identifier)?
ExportDecl ::= 'export' Declaration                                         // 直接导出声明
            |  'export' Identifier                                          // 导出已声明标识符
            |  'export' '*' 'from' StringLiteral                            // 转发导出
ImportMembers ::= '{' ImportMember (',' ImportMember)* ','? '}'
ImportMember  ::= Identifier ('as' Identifier)?
ImportModule  ::= StringLiteral | Identifier ('/' Identifier)?

AttrList ::= ('@' Identifier ('(' ArgList? ')')?)*  // 例如 @extern("C")、@dylib("m")、@c_export("sym")、@repr(C)

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
IdStart    ::= 'a'..'z' | 'A'..'Z' | '_'
IdContinue ::= IdStart | '0'..'9'

IntLiteral   ::= DecimalInt | HexInt | BinInt | OctInt
DecimalInt   ::= DecimalDigit ('_'? DecimalDigit)*
HexInt       ::= '0x' HexDigit ('_'? HexDigit)*
BinInt       ::= '0b' ('0' | '1') ('_'? ('0' | '1'))*
OctInt       ::= '0o' ('0'..'7') ('_'? ('0'..'7'))*

FloatLiteral ::= DecimalInt '.' DecimalInt? Exponent?
              |  DecimalInt Exponent
Exponent     ::= ('e' | 'E') ('+' | '-')? DecimalDigit+

BigIntLiteral ::= DecimalInt 'n'

StringLiteral ::= '"' StringChar* '"'
                | "'" StringChar* "'"
RawStringLiteral ::= 'r' '"' [^"]* '"'
RegexLiteral ::= '/' RegexBody '/' RegexFlags?

BoolLiteral ::= 'true' | 'false'
NullLiteral ::= 'null'
```

### A.2 Types

```ebnf
Type ::= UnionType
UnionType ::= IntersectionType ('|' IntersectionType)*
IntersectionType ::= NullableType
NullableType ::= PrimaryType '?'?
PrimaryType ::= FFIPointerType | CFunctionType | NamedType | FunctionType | TupleType | ObjectType
FFIPointerType ::= ('RawPtr' | 'RawMut') '<' Type '>'
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

TernaryExpr ::= LogicOrExpr ('?' Expression ':' Expression)?
LogicOrExpr ::= LogicAndExpr ('||' LogicAndExpr)*
            |   NullCoalesce
LogicAndExpr ::= BitOrExpr ('&&' BitOrExpr)*
NullCoalesce ::= LogicAndExpr ('??' LogicAndExpr)*
BitOrExpr   ::= BitXorExpr ('|' BitXorExpr)*
BitXorExpr  ::= BitAndExpr ('^' BitAndExpr)*
BitAndExpr  ::= EqualityExpr ('&' EqualityExpr)*
EqualityExpr ::= RelationalExpr (('==' | '!=') RelationalExpr)*
RelationalExpr ::= ShiftExpr (('<' | '<=' | '>' | '>=') ShiftExpr)*
ShiftExpr   ::= AdditiveExpr (('<<' | '>>') AdditiveExpr)*
AdditiveExpr ::= MultiplicativeExpr (('+' | '-') MultiplicativeExpr)*
MultiplicativeExpr ::= TypeOpExpr (('*' | '/' | '%') TypeOpExpr)*
TypeOpExpr  ::= UnaryExpr (('as' | 'is') Type)*           // safe cast is `x as T?` where T? is a nullable type
RangeExpr   ::= AdditiveExpr ('..' AdditiveExpr)?

UnaryExpr ::= ('-' | '+' | '!' | '~') UnaryExpr
           |  'new' QualifiedIdent TypeArgs? '(' ArgList? ')'
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
         |  StringLiteral | RawStringLiteral | RegexLiteral
         |  BoolLiteral | NullLiteral
         |  Identifier
         |  ArrayLit | MapLit | SetLit | ObjectLit
         |  ArrowFunction
         |  MatchExpr
         |  '(' Expression ')'
         |  '(' Expression (',' Expression)+ ')'  // tuple

ArrayLit ::= '[' (Expression (',' Expression)* ','?)? ']'
MapLit   ::= '#{' (MapEntry (',' MapEntry)* ','?)? '}'
MapEntry ::= Expression ':' Expression
SetLit   ::= '#[' (Expression (',' Expression)* ','?)? ']'
ObjectLit ::= '{' (ObjectFieldExpr (',' ObjectFieldExpr)* ','?)? '}'
ObjectFieldExpr ::= Identifier ':' Expression | Identifier

ArrowFunction ::= '(' ArrowParams? ')' '->' (Expression | Block)
ArrowParams ::= ArrowParam (',' ArrowParam)*
ArrowParam  ::= Identifier ':' Type
// Note: arrow closures cannot declare an explicit return type;
// use `fn(p: T) -> R { ... }` or annotate the binding (`let f: (T) -> R = ...`) instead.

MatchExpr ::= 'match' '(' Expression ')' '{' MatchArm (','? MatchArm)* ','? '}'
MatchArm  ::= Pattern ('if' '(' Expression ')')? '->' (Expression | Block)

ThrowExpr   ::= 'throw' Expression                // operand's static type must be an enum variant

ArgList ::= Expression (',' Expression)* ','?
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

LiteralPattern  ::= IntLiteral | FloatLiteral | StringLiteral | BoolLiteral | NullLiteral
RangePattern    ::= Expression '..' Expression
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
           |  FnDecl
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

ScopeStmt ::= 'scope' Block            // lexical scope + structured concurrency

SelectStmt ::= 'select' '{' SelectArm+ '}'
SelectArm  ::= Identifier 'from' Expression '->' Block      // receive
            |  Expression 'to' Expression '->' Block        // send
            |  'after' Expression '->' Block                // timeout
            |  '_' '->' Block                                // default

YieldStmt ::= 'yield'
```

### A.6 Declarations

```ebnf
VarDecl ::= ('let' | 'const' | 'shared' ('const' | 'let')) Binding (',' Binding)*
Binding ::= BindingPattern (':' Type)? ('=' Expression)?
BindingPattern ::= Identifier
                |  '[' BindingPattern (',' BindingPattern)* ','? ']'
                |  '(' BindingPattern (',' BindingPattern)+ ','? ')'
                |  '{' Identifier (',' Identifier)* ','? '}'

FnDecl ::= AttrList? Modifier* 'fn' Identifier TypeParams? '(' ParamList? ')' ReturnType? FnBody
FnBody ::= Block | ';'?                         // empty body is only allowed for @extern
ParamList ::= Param (',' Param)* ','?
Param     ::= Modifier* Identifier ':' Type ('=' Expression)?
           |  '...' Identifier ':' Type
ReturnType ::= '->' Type | '->' '(' Type (',' Type)+ ')'
Modifier  ::= 'in' | 'ref' | 'private' | 'static' | 'final' | 'abstract' | 'override'
              // public visibility is the default; override is optional

TypeParams ::= '<' TypeParam (',' TypeParam)* ','? '>'
TypeParam  ::= Identifier (':' Type ('&' Type)*)?         // constraints use ':', multiple use '&'
AliasTypeParams ::= '<' Identifier (',' Identifier)* ','? '>'

ClassDecl ::= Modifier* 'class' Identifier TypeParams?
              ('extends' NamedType)?
              ('implements' NamedType (',' NamedType)*)?
              '{' ClassMember* '}'
ClassMember ::= FieldDecl | MethodDecl | ConstructorDecl
FieldDecl ::= Modifier* Identifier ':' Type ('=' Expression)?
MethodDecl ::= Modifier* Identifier '(' ParamList? ')' ReturnType? Block
            |  Modifier* 'operator' OperatorToken '(' ParamList? ')' ReturnType? Block
ConstructorDecl ::= 'constructor' '(' ParamList? ')' Block

StructDecl ::= 'struct' Identifier TypeParams?
               ('implements' NamedType (',' NamedType)*)?
               '{' ClassMember* '}'

InterfaceDecl ::= 'interface' Identifier TypeParams?
                  ('extends' NamedType (',' NamedType)*)?
                  '{' InterfaceMember* '}'
InterfaceMember ::= Identifier '(' ParamList? ')' ReturnType?

EnumDecl       ::= 'enum' Identifier TypeParams?
                   ('implements' NamedType (',' NamedType)*)?
                   '{' EnumVariant (',' EnumVariant)* ','? EnumMethod* '}'
EnumVariant    ::= Identifier VariantPayload?
                |  Identifier '=' BackingValue                  // simple enum (no payload)
EnumMethod     ::= 'fn' Identifier TypeParams? '(' ParamList? ')' ReturnType? Block
VariantPayload ::= '(' VariantField (',' VariantField)* ')'
VariantField   ::= (Identifier ':')? Type
BackingValue   ::= IntLiteral | FloatLiteral | StringLiteral | BoolLiteral

TypeAliasDecl ::= 'type' Identifier AliasTypeParams? '=' Type

ImportDecl ::= 'import' ImportMembers 'from' ImportModule
            |  'import' ImportModule ('as' Identifier)?
ExportDecl ::= 'export' Declaration                                         // export the declaration directly
            |  'export' Identifier                                          // export an already-declared identifier
            |  'export' '*' 'from' StringLiteral                            // forwarding export
ImportMembers ::= '{' ImportMember (',' ImportMember)* ','? '}'
ImportMember  ::= Identifier ('as' Identifier)?
ImportModule  ::= StringLiteral | Identifier ('/' Identifier)?

AttrList ::= ('@' Identifier ('(' ArgList? ')')?)*  // e.g. @extern("C"), @dylib("m"), @c_export("sym"), @repr(C)

OperatorToken ::= '+' | '-' | '*' | '/' | '%'
               |  '&' | '|' | '^'
               |  '==' | '!=' | '<' | '<=' | '>' | '>='
               |  '[]' | '[]='
               |  '!' | '~'
```

> Note: this EBNF is curated for guidance. Precedence, associativity, and disambiguation are determined by the parser implementation; in case of ambiguity, treat `src/frontend/parser/xparse_*.c` as authoritative.
<!-- /xr-spec:en -->
