# C++ Style Guide

## Goals of the Style Guide

**Optimize for the reader, not the writer**
:   Our codebase (and most individual components submitted to it) is expected to
    continue for quite some time. As a result, more time will be spent reading
    most of our code than writing it. We explicitly choose to optimize for the
    experience of our average software engineer reading, maintaining, and
    debugging code in our codebase rather than ease when writing said code.
    "Leave a trace for the reader" is a particularly common sub-point of this
    principle: When something surprising or unusual is happening in a snippet of
    code (for example, transfer of pointer ownership), leaving textual hints for
    the reader at the point of use is valuable (`std::unique_ptr` demonstrates
    the ownership transfer unambiguously at the call site).

**Avoid surprising or dangerous constructs**
:   C++ has features that are more surprising or dangerous than one might think
    at a glance. Some style guide restrictions are in place to prevent falling
    into these pitfalls. There is a high bar for style guide waivers on such
    restrictions, because waiving such rules often directly risks compromising
    program correctness.

**Avoid constructs that our average C++ programmer would find tricky or hard to maintain**
:   C++ has features that may not be generally appropriate because of the
    complexity they introduce to the code. In widely used
    code, it may be
    more acceptable to use trickier language constructs, because any benefits of
    more complex implementation are multiplied widely by usage, and the cost in
    understanding the complexity does not need to be paid again when working
    with new portions of the codebase. When in doubt, waivers to rules of this
    type can be sought by asking your project leads. This is
    specifically important for our codebase because code ownership and team
    membership changes over time: even if everyone that works with some piece of
    code currently understands it, such understanding is not guaranteed to hold
    a few years from now.

**Concede to optimization when necessary**
:   Performance optimizations can sometimes be necessary and appropriate, even
    when they conflict with the other principles of this document.

The intent of this document is to provide maximal guidance with reasonable
restriction. As always, common sense and good taste should prevail. Be
skeptical about and reluctant to use clever or unusual constructs: the absence
of a prohibition is not the same as a license to proceed. Use your judgment, and
if you are unsure, please don't hesitate to ask your
project leads to get additional input.

## C++ Version

Currently, code should target C++20, i.e., should not use C++23 features. The
C++ version targeted by this guide will advance (aggressively) over time.

Do not use [non-standard extensions](#nonstandard-extensions).

Consider portability to other environments before using features from C++17 and
C++20 in your project.

## Header Files

In general, every `.cc` file should have an associated `.h` file. There are some
common exceptions, such as unit tests and small `.cc` files containing just a
`main()` function.

Correct use of header files can make a huge difference to the readability, size
and performance of your code.

### Self-contained Headers

Header files should be self-contained (compile on their own) and end in `.h`.
Non-header files that are meant for inclusion should end in `.inc` and be used
sparingly.

All header files should be self-contained. Users and refactoring tools should
not have to adhere to special conditions to include the header. Specifically, a
header should have [header guards](#the-define-guard) and include all other
headers it needs.

When a header declares inline functions or templates that clients of the header
will instantiate, the inline functions and templates must also have definitions
in the header, either directly or in files it includes. Do not move these
definitions to separately included header (`-inl.h`) files; this practice was
common in the past, but is no longer allowed. When all instantiations of a
template occur in one `.cc` file, either because they're
[explicit](https://en.cppreference.com/w/cpp/language/class_template#Explicit_instantiation)
or because the definition is accessible to only the `.cc` file, the template
definition can be kept in that file.

There are rare cases where a file designed to be included is not self-contained.
These are typically intended to be included at unusual locations, such as the
middle of another file. They might not use [header guards](#the-define-guard),
and might not include their prerequisites. Name such files with the `.inc`
extension. Use sparingly, and prefer self-contained headers when possible.

### The #define Guard

All header files should have `#define` guards to prevent multiple inclusion. The
format of the symbol name should be
`<PROJECT>_<PATH>_<FILE>_H_`.

To guarantee uniqueness, they should be based on the full path in a project's
source tree. For example, the file `foo/src/bar/baz.h` in project `foo` should
have the following guard:

**Good example:**

```cpp
#ifndef FOO_BAR_BAZ_H_
#define FOO_BAR_BAZ_H_

...

#endif  // FOO_BAR_BAZ_H_
```

### Include What You Use

If a source or header file refers to a symbol defined elsewhere, the file should
directly include a header file which properly intends to provide a declaration
or definition of that symbol. It should not include header files for any other
reason.

Do not rely on transitive inclusions. This allows people to remove
no-longer-needed `#include` statements from their headers without breaking
clients. This also applies to related headers - `foo.cc` should include `bar.h`
if it uses a symbol from it even if `foo.h` includes `bar.h`.

### Forward Declarations

Avoid using forward declarations where possible. Instead,
[include the headers you need](#include-what-you-use).

Do not use forward declarations of entities that your project does not own.

For entities that you do own:

- Try to make the header for a declaration cheap enough to easily include it
  directly.
- If the full header is too expensive to include, try to speed up the header's
  compilation.
- If your best engineering judgment is that a forward declaration is a better
  way to fix a slowdown, exercise that judgment and use a forward declaration.

### Defining Functions in Header Files

Include the definition of a function at its point of declaration in a header
file only when the definition is short. If the definition otherwise has a reason
be in the header, put it in an internal part of the file. If necessary to make
the definition ODR-safe, mark it with an `inline` specifier.

Only define a function at its public declaration if it is short, say, 10 lines
or fewer. Put longer function bodies in the `.cc` file unless they must be in
the header for performance or technical reasons.

Even if a definition must be in the header, this is not a sufficient reason to
put it within the public part. Instead, the definition can be in an internal
part of the header, such as the [private](#access-control) section of a class,
within a namespace that includes the word `internal`, or below a comment like `//
Implementation details only below here`.

Once a definition is in a header file, it must be ODR-safe by having the
`inline` specifier or being implicitly specified inline by being a function
template or defined in a class body when first declared.

**Good example:**

```cpp
template <typename T>
class Foo {
 public:
  int bar() { return bar_; }

  void MethodWithHugeBody();

 private:
  int bar_;
};

// Implementation details only below here

template <typename T>
void Foo<T>::MethodWithHugeBody() {
  ...
}
```

### Names and Order of Includes

Include headers in the following order: Related header, C system headers, C++
standard library headers,
other libraries' headers, your project's headers.

All of a
project's header files should be listed as descendants of the project's source
directory without use of UNIX directory aliases `.` (the current
directory) or `..` (the parent directory). For example,
`google-awesome-project/src/base/logging.h` should
be included as:

**Good example:**

```cpp
#include "base/logging.h"
```

Headers should only be included using an angle-bracketed path if the library
requires you to do so. In particular, the following headers require angle
brackets:

- C and C++ standard library headers (e.g., `<stdlib.h>` and `<string>`).
- POSIX, Linux, and Windows system headers (e.g., `<unistd.h>` and
  `<windows.h>`).
- In rare cases, third\_party libraries (e.g., `<Python.h>`).

In `dir/foo.cc` or
`dir/foo_test.cc`, whose main purpose is to implement or
test the stuff in `dir2/foo2.h`, order your includes as
follows:

1. `dir2/foo2.h`.
2. A blank line
3. C system headers, and any other headers in angle brackets with the `.h`
   extension, e.g., `<unistd.h>`, `<stdlib.h>`, `<Python.h>`.
4. A blank line
5. C++ standard library headers (without file extension), e.g., `<algorithm>`,
   `<cstddef>`.
6. A blank line
7. Other libraries' `.h` files.
8. A blank line.
9. Your
   project's `.h` files.

Separate each non-empty group with one blank line.

With the preferred ordering, if the related header
`dir2/foo2.h` omits any necessary includes, the build of
`dir/foo.cc` or `dir/foo_test.cc`
will break. Thus, this rule ensures that build breaks show up first for the
people working on these files, not for innocent people in other packages.

`dir/foo.cc` and `dir2/foo2.h`
are usually in the same directory (e.g., `base/basictypes_test.cc` and
`base/basictypes.h`), but may sometimes be in different directories too
.

Note that the C headers such as `stddef.h` are essentially interchangeable with
their C++ counterparts (`cstddef`). Either style is acceptable, but prefer
consistency with existing code.

Within each section the includes should be ordered alphabetically. Note that
older code might not conform to this rule and should be fixed when convenient.

For example, the includes in
`google-awesome-project/src/foo/internal/fooserver.cc` might look like
this:

**Good example:**

```cpp
#include "foo/server/fooserver.h"

#include <sys/types.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "base/basictypes.h"
#include "foo/server/bar.h"
#include "third_party/absl/flags/flag.h"
```

**Exception:**

Sometimes, system-specific code needs conditional includes. Such code can put
conditional includes after other includes. Of course, keep your system-specific
code small and localized. Example:

**Good example:**

```cpp
#include "foo/public/fooserver.h"

#ifdef _WIN32
#include <windows.h>
#endif  // _WIN32
```

## Scoping

### Namespaces

With few exceptions, place code in a namespace. Namespaces should have unique
names based on the project name, and possibly its path. Do not use
*using-directives* (e.g., `using namespace foo`). Do not use inline namespaces.
For unnamed namespaces, see [Internal Linkage](#internal-linkage).

Namespaces should be used as follows:

- Follow the rules on [Namespace Names](#namespace-names).
- Terminate multi-line namespaces with comments as shown in the given
  examples.
- Namespaces wrap the entire source file after includes,
  [gflags](https://gflags.github.io/gflags/)
  definitions/declarations and forward declarations of classes from other
  namespaces.

  **Good example:**

  ```cpp
  // In the .h file
  namespace mynamespace {

  // All declarations are within the namespace scope.
  // Notice the lack of indentation.
  class MyClass {
   public:
    ...
    void Foo();
  };

  }  // namespace mynamespace
  ```

  **Good example:**

  ```cpp
  // In the .cc file
  namespace mynamespace {

  // Definition of functions is within scope of the namespace.
  void MyClass::Foo() {
    ...
  }

  }  // namespace mynamespace
  ```

  More complex `.cc` files might have additional details, like flags or
  using-declarations.

  **Good example:**

  ```cpp
  #include "a.h"

  ABSL_FLAG(bool, someflag, false, "a flag");

  namespace mynamespace {

  using ::foo::Bar;

  ...code for mynamespace...    // Code goes against the left margin.

  }  // namespace mynamespace
  ```
- To place generated protocol message code in a namespace, use the `package`
  specifier in the `.proto` file. See
  [Protocol Buffer Packages](https://protobuf.dev/reference/cpp/cpp-generated/#package)
  for details.
- Do not declare anything in namespace `std`, including forward declarations
  of standard library classes. Declaring entities in namespace `std` is
  undefined behavior, i.e., not portable. To declare entities from the
  standard library, include the appropriate header file.
- You may not use a *using-directive* to make all names from a namespace
  available.

  **Bad example:**

  ```cpp
  // Forbidden -- This pollutes the namespace.
  using namespace foo;
  ```
- Do not use *Namespace aliases* at namespace scope in header files except in
  explicitly marked internal-only namespaces, because anything imported into a
  namespace in a header file becomes part of the public API exported by that
  file. Namespace aliases can be used when those conditions don't apply, but
  they must have [appropriate names](#aliases-1).

  **Good example:**

  ```cpp
  // In a .h file, an alias must not be a separate API, or must be hidden in
  // an implementation detail.
  namespace librarian {

  namespace internal {  // Internal, not part of the API.
  namespace sidetable = ::pipeline_diagnostics::sidetable;
  }

  inline void my_inline_function() {
    // Local to a function.
    namespace baz = ::foo::bar::baz;
    ...
  }

  }  // namespace librarian
  ```

  **Good example:**

  ```cpp
  // Remove uninteresting parts of some commonly used names in .cc files.
  namespace sidetable = ::pipeline_diagnostics::sidetable;
  ```
- Do not use inline namespaces.
- Use namespaces with "internal" in the name to document parts of an API that
  should not be mentioned by users of the API.

  **Bad example:**

  ```cpp
  // We shouldn't use this internal name in non-absl code.
  using ::absl::container_internal::ImplementationDetail;
  ```

  Note that there is still a risk of collision between libraries within a
  nested `internal` namespace, so give each library within a namespace a
  unique internal namespace by adding the library's filename. For example,
  `gshoe/widget.h` would use `gshoe::internal_widget` as opposed to just
  `gshoe::internal`.
- Single-line nested namespace declarations
  are preferred in new code, but are not required.

  **Good example:**

  ```cpp
  namespace my_project::my_component {

    ...

  }  // namespace my_project::my_component
  ```

### Internal Linkage

When definitions in a `.cc` file do not need to be referenced outside that file,
give them internal linkage by placing them in an unnamed namespace or declaring
them `static`. Do not use either of these constructs in `.h` files.

#### Definition

All declarations can be given internal linkage by placing them in unnamed
namespaces. Functions and variables can also be given internal linkage by
declaring them `static`. This means that anything you're declaring can't be
accessed from another file. If a different file declares something with the same
name, then the two entities are completely independent.

Use of internal linkage in `.cc` files is encouraged for all code that does not
need to be referenced elsewhere. Do not use internal linkage in `.h` files.

Format unnamed namespaces like named namespaces. In the terminating comment,
leave the namespace name empty:

**Good example:**

```cpp
namespace {
...
}  // namespace
```

### Nonmember, Static Member, and Global Functions

Prefer placing nonmember functions in a namespace; use completely global
functions rarely.

Do not create a class consisting of static members unless there's a good reason.
Free functions in a namespace are the idiomatic way to define non-instance
operations. Wrapping them in a class is often an unnecessary Java-ism. Examples
of good reasons include a policy class or a trait, giving type semantics to a
separate representation, or closing an interface to extension.

Sometimes it is useful to define a function not bound to a class instance. Such
a function can be either a static member or a nonmember function. Nonmember
functions should not depend on external variables, and should nearly always
exist in a namespace. Do not create classes only to group static members; this
is no different than just giving the names a common prefix, and such grouping is
usually unnecessary anyway.

If you define a nonmember function and it is only needed in its `.cc` file, use
[internal linkage](#internal-linkage) to limit its scope.

### Local Variables

Place a function's variables in the narrowest scope possible, and initialize
variables in the declaration.

C++ allows you to declare variables anywhere in a function. We encourage you to
declare them in a scope as local as possible, and as close to the first use as
possible. This makes it easier for the reader to find the declaration and see
what type the variable is and what it was initialized to. In particular,
initialization should be used instead of declaration and assignment, e.g.,:

**Bad example:**

```cpp
int i;
i = f();      // Bad -- initialization separate from declaration.
```

**Good example:**

```cpp
int i = f();  // Good -- declaration has initialization.
```

**Bad example:**

```cpp
int jobs = NumJobs();
// More code...
f(jobs);      // Bad -- declaration separate from use.
```

**Good example:**

```cpp
int jobs = NumJobs();
f(jobs);      // Good -- declaration immediately (or closely) followed by use.
```

**Bad example:**

```cpp
std::vector<int> v;
v.push_back(1);  // Prefer initializing using brace initialization.
v.push_back(2);
```

**Good example:**

```cpp
std::vector<int> v = {1, 2};  // Good -- v starts initialized.
```

Variables needed for `if`, `while` and `for` statements should normally be
declared within those statements, so that such variables are confined to those
scopes. For example:

**Good example:**

```cpp
while (const char* p = strchr(str, '/')) str = p + 1;
```

There is one caveat: if the variable is an object, its constructor is invoked
every time it enters scope and is created, and its destructor is invoked every
time it goes out of scope.

**Bad example:**

```cpp
// Inefficient implementation:
for (int i = 0; i < 1000000; ++i) {
  Foo f;  // My ctor and dtor get called 1000000 times each.
  f.DoSomething(i);
}
```

It may be more efficient to declare such a variable used in a loop outside that
loop:

**Good example:**

```cpp
Foo f;  // My ctor and dtor get called once each.
for (int i = 0; i < 1000000; ++i) {
  f.DoSomething(i);
}
```

### Static and Global Variables

Objects with
[static storage duration](http://en.cppreference.com/w/cpp/language/storage_duration#Storage_duration)
are forbidden unless they are
[trivially destructible](http://en.cppreference.com/w/cpp/types/is_destructible).
Informally this means that the destructor does not do anything, even taking
member and base destructors into account. More formally it means that the type
has no user-defined or virtual destructor and that all bases and non-static
members are trivially destructible. Static function-local variables may use
dynamic initialization. Use of dynamic initialization for static class member
variables or variables at namespace scope is discouraged, but allowed in limited
circumstances; see below for details.

As a rule of thumb: a global variable satisfies these requirements if its
declaration, considered in isolation, could be `constexpr`.

#### Decision on destruction

When destructors are trivial, their execution is not subject to ordering at all
(they are effectively not "run"); otherwise we are exposed to the risk of
accessing objects after the end of their lifetime. Therefore, we only allow
objects with static storage duration if they are trivially destructible.
Fundamental types (like pointers and `int`) are trivially destructible, as are
arrays of trivially destructible types. Note that variables marked with
`constexpr` are trivially destructible.

**Good example:**

```cpp
const int kNum = 10;  // Allowed

struct X { int n; };
const X kX[] = {{1}, {2}, {3}};  // Allowed

void foo() {
  static const char* const kMessages[] = {"hello", "world"};  // Allowed
}

// Allowed: constexpr guarantees trivial destructor.
constexpr std::array<int, 3> kArray = {1, 2, 3};
```

**Bad example:**

```cpp
// bad: non-trivial destructor
const std::string kFoo = "foo";

// Bad for the same reason, even though kBar is a reference (the
// rule also applies to lifetime-extended temporary objects).
const std::string& kBar = StrCat("a", "b", "c");

void bar() {
  // Bad: non-trivial destructor.
  static std::map<int, int> kData = {{1, 0}, {2, 0}, {3, 0}};
}
```

Note that references are not objects, and thus they are not subject to the
constraints on destructibility. The constraint on dynamic initialization still
applies, though. In particular, a function-local static reference of the form
`static T& t = *new T;` is allowed.

#### Decision on initialization

Initialization is a more complex topic. This is because we must not only
consider whether class constructors execute, but we must also consider the
evaluation of the initializer:

```cpp
int n = 5;    // Fine
int m = f();  // ? (Depends on f)
Foo x;        // ? (Depends on Foo::Foo)
Bar y = g();  // ? (Depends on g and on Bar::Bar)
```

All but the first statement expose us to indeterminate initialization ordering.

The concept we are looking for is called *constant initialization* in the formal
language of the C++ standard. It means that the initializing expression is a
constant expression, and if the object is initialized by a constructor call,
then the constructor must be specified as `constexpr`, too:

**Good example:**

```cpp
struct Foo { constexpr Foo(int) {} };

int n = 5;  // Fine, 5 is a constant expression.
Foo x(2);   // Fine, 2 is a constant expression and the chosen constructor is constexpr.
Foo a[] = { Foo(1), Foo(2), Foo(3) };  // Fine
```

Constant initialization is always allowed. Constant initialization of static
storage duration variables should be marked with `constexpr` or `constinit`. Any
non-local static storage duration variable that is not so marked should be
presumed to have dynamic initialization, and reviewed very carefully.

By contrast, the following initializations are problematic:

**Bad example:**

```cpp
// Some declarations used below.
time_t time(time_t*);      // Not constexpr!
int f();                   // Not constexpr!
struct Bar { Bar() {} };

// Problematic initializations.
time_t m = time(nullptr);  // Initializing expression not a constant expression.
Foo y(f());                // Ditto
Bar b;                     // Chosen constructor Bar::Bar() not constexpr.
```

Dynamic initialization of nonlocal variables is discouraged, and in general it
is forbidden. However, we do permit it if no aspect of the program depends on
the sequencing of this initialization with respect to all other initializations.
Under those restrictions, the ordering of the initialization does not make an
observable difference. For example:

**Good example:**

```cpp
int p = getpid();  // Allowed, as long as no other static variable
                   // uses p in its own initialization.
```

Dynamic initialization of static local variables is allowed (and common).

#### Common patterns

- Global strings: if you require a named global or static string constant,
  consider using a `constexpr` variable of `string_view`, character array, or
  character pointer, pointing to a string literal. String literals have static
  storage duration already and are usually sufficient. See
  [TotW #140](https://abseil.io/tips/140).
- Maps, sets, and other dynamic containers: if you require a static, fixed
  collection, such as a set to search against or a lookup table, you cannot
  use the dynamic containers from the standard library as a static variable,
  since they have non-trivial destructors. Instead, consider
  a simple array of trivial types, e.g., an array of
  arrays of ints (for a "map from int to int"), or an array of pairs (e.g.,
  pairs of `int` and `const char*`). For small collections, linear search is
  entirely sufficient (and efficient, due to memory locality); consider using
  the facilities from
  [absl/algorithm/container.h](https://github.com/abseil/abseil-cpp/blob/master/absl/algorithm/container.h)
  for the standard operations. If necessary, keep the collection in sorted
  order and use a binary search algorithm. If you do really prefer a
  dynamic container from the standard library, consider using
  a function-local static pointer, as described
  below
  .
- Smart pointers (`std::unique_ptr`, `std::shared_ptr`): smart pointers
  execute cleanup during destruction and are therefore forbidden. Consider
  whether your use case fits into one of the other patterns described in this
  section. One simple solution is to use a plain pointer to a dynamically
  allocated object and never delete it (see last item).
- Static variables of custom types: if you require static, constant data of a
  type that you need to define yourself, give the type a trivial destructor
  and a `constexpr` constructor.
- If all else fails, you can create an object dynamically and never delete it
  by using a function-local static pointer or reference
  (e.g., `static const auto& impl = *new
  T(args...);`).

### thread\_local Variables

`thread_local` variables that aren't declared inside a function must be
initialized with a true compile-time constant, and this must be enforced by
using the [`constinit`](https://en.cppreference.com/w/cpp/language/constinit)
attribute. Prefer `thread_local` over other ways of defining thread-local data.

`thread_local` variables at class or namespace scope must be initialized with a
true compile-time constant (i.e., they must have no dynamic initialization). To
enforce this, `thread_local` variables at class or namespace scope must be
annotated with
[`constinit`](https://en.cppreference.com/w/cpp/language/constinit) (or
`constexpr`, but that should be rare):

**Good example:**

```cpp
constinit thread_local Foo foo = ...;
```

`thread_local` variables inside a function have no initialization concerns, but
still risk use-after-free during thread exit. Note that you can use a
function-scope `thread_local` to simulate a class- or namespace-scope
`thread_local` by defining a function or static method that exposes it:

**Good example:**

```cpp
Foo& MyThreadLocalFoo() {
  thread_local Foo result = ComplicatedInitialization();
  return result;
}
```

Note that `thread_local` variables will be destroyed whenever a thread exits. If
the destructor of any such variable refers to any other (potentially-destroyed)
`thread_local` we will suffer from hard to diagnose use-after-free bugs. Prefer
trivial types, or types that provably run no user-provided code at destruction
to minimize the potential of accessing any other `thread_local`.

`thread_local` should be preferred over other mechanisms for defining
thread-local data.

## Classes

Classes are the fundamental unit of code in C++. Naturally, we use them
extensively. This section lists the main dos and don'ts you should follow when
writing a class.

### Doing Work in Constructors

Avoid virtual method calls in constructors, and avoid initialization that can
fail if you can't signal an error.

Constructors should never call virtual functions. If appropriate for your code
, terminating the
program may be an appropriate error handling response. Otherwise, consider a
factory function or `Init()` method as described in
[TotW #42](https://abseil.io/tips/42) . Avoid `Init()` methods on objects
with no other states that affect which public methods may be called
(semi-constructed objects of this form are particularly hard to work with
correctly).

### Implicit Conversions

Do not define implicit conversions. Use the `explicit` keyword for conversion
operators and single-argument constructors.

Type conversion operators, and constructors that are callable with a single
argument, must be marked `explicit` in the class definition. As an exception,
copy and move constructors should not be `explicit`, since they do not perform
type conversion.

Implicit conversions can sometimes be necessary and appropriate for types that
are designed to be interchangeable, for example when objects of two types are
just different representations of the same underlying value. In that case,
contact
your project leads to request a waiver of this
rule.

Constructors that cannot be called with a single argument may omit `explicit`.
Constructors that take a single `std::initializer_list` parameter should also
omit `explicit`, in order to support copy-initialization (e.g., `MyType m = {1,
2};`).

### Copyable and Movable Types

A class's public API must make clear whether the class is copyable, move-only,
or neither copyable nor movable. Support copying and/or moving if these
operations are clear and meaningful for your type.

Every class's public interface must make clear which copy and move operations
the class supports. This should usually take the form of explicitly declaring
and/or deleting the appropriate operations in the `public` section of the
declaration.

Specifically, a copyable class should explicitly declare the copy operations, a
move-only class should explicitly declare the move operations, and a
non-copyable/movable class should explicitly delete the copy operations. A
copyable class may also declare move operations in order to support efficient
moves. Explicitly declaring or deleting all four copy/move operations is
permitted, but not required. If you provide a copy or move assignment operator,
you must also provide the corresponding constructor.

**Good example:**

```cpp
class Copyable {
 public:
  Copyable(const Copyable& other) = default;
  Copyable& operator=(const Copyable& other) = default;

  // The implicit move operations are suppressed by the declarations above.
  // You may explicitly declare move operations to support efficient moves.
};

class MoveOnly {
 public:
  MoveOnly(MoveOnly&& other) = default;
  MoveOnly& operator=(MoveOnly&& other) = default;

  // The copy operations are implicitly deleted, but you can
  // spell that out explicitly if you want:
  MoveOnly(const MoveOnly&) = delete;
  MoveOnly& operator=(const MoveOnly&) = delete;
};

class NotCopyableOrMovable {
 public:
  // Not copyable or movable
  NotCopyableOrMovable(const NotCopyableOrMovable&) = delete;
  NotCopyableOrMovable& operator=(const NotCopyableOrMovable&)
      = delete;

  // The move operations are implicitly disabled, but you can
  // spell that out explicitly if you want:
  NotCopyableOrMovable(NotCopyableOrMovable&&) = delete;
  NotCopyableOrMovable& operator=(NotCopyableOrMovable&&)
      = delete;
};
```

These declarations/deletions can be omitted only if they are obvious:

- If the class has no `private` section, like a [struct](#structs-vs-classes)
  or an interface-only base class, then the copyability/movability can be
  determined by the copyability/movability of any public data members.
- If a base class clearly isn't copyable or movable, derived classes naturally
  won't be either. An interface-only base class that leaves these operations
  implicit is not sufficient to make concrete subclasses clear.
- Note that if you explicitly declare or delete either the constructor or
  assignment operation for copy, the other copy operation is not obvious and
  must be declared or deleted. Likewise for move operations.

A type should not be copyable/movable if the meaning of copying/moving is
unclear to a casual user, or if it incurs unexpected costs. Move operations for
copyable types are strictly a performance optimization and are a potential
source of bugs and complexity, so avoid defining them unless they are
significantly more efficient than the corresponding copy operations.
If your type provides copy operations, it is
recommended that you design your class so that the default implementation of
those operations is correct. Remember to review the correctness of any defaulted
operations as you would any other code.

To eliminate the risk of slicing, prefer to make base classes abstract, by
making their constructors protected, by declaring their destructors protected,
or by giving them one or more pure virtual member functions. Prefer to avoid
deriving from concrete classes.

### Structs vs. Classes

Use a `struct` only for passive objects that carry data; everything else is a
`class`.

The `struct` and `class` keywords behave almost identically in C++. We add our
own semantic meanings to each keyword, so you should use the appropriate keyword
for the data-type you're defining.

`structs` should be used for passive objects that carry data, and may have
associated constants. All fields must be public. The struct type itself must not
have invariants that imply relationships between different fields, since direct
user access to those fields may break those invariants, but users of a struct
may have requirements and guarantees on particular uses of it. Constructors,
destructors, and helper methods may be present; however, these methods must not
require or enforce any invariants.

If more functionality or invariants are required, or struct has wide visibility
and expected to evolve, then a `class` is more appropriate. If in doubt, make it
a `class`.

For consistency with STL, you can use `struct` instead of `class` for stateless
types, such as traits, [template metafunctions](#template-metaprogramming), and
some functors.

Note that member variables in structs and classes have
[different naming rules](#variable-names).

### Structs vs. Pairs and Tuples

Prefer to use a `struct` instead of a pair or a tuple whenever the elements can
have meaningful names.

While using pairs and tuples can avoid the need to define a custom type,
potentially saving work when *writing* code, a meaningful field name will almost
always be much clearer when *reading* code than `.first`, `.second`, or
`std::get<X>`. While C++14's introduction of `std::get<Type>` to access a tuple
element by type rather than index (when the type is unique) can sometimes
partially mitigate this, a field name is usually substantially clearer and more
informative than a type.

Pairs and tuples may be appropriate in generic code where there are not specific
meanings for the elements of the pair or tuple. Their use may also be required
in order to interoperate with existing code or APIs.

### Inheritance

Composition is often more appropriate than inheritance. When using inheritance,
make it `public`.

All inheritance should be `public`. If you want to do private inheritance, you
should be including an instance of the base class as a member instead. You may
use `final` on classes when you don't intend to support using them as base
classes.

Do not overuse implementation inheritance. Composition is often more
appropriate. Try to restrict use of inheritance to the "is-a" case: `Bar`
subclasses `Foo` if it can reasonably be said that `Bar` "is a kind of" `Foo`.

Limit the use of `protected` to those member functions that might need to be
accessed from subclasses. Note that
[data members should be `private`](#access-control).

Explicitly annotate overrides of virtual functions or virtual destructors with
exactly one of an `override` or (less frequently) `final` specifier. Do not use
`virtual` when declaring an override. Rationale: A function or destructor marked
`override` or `final` that is not an override of a base class virtual function
will not compile, and this helps catch common errors. The specifiers serve as
documentation; if no specifier is present, the reader has to check all ancestors
of the class in question to determine if the function or destructor is virtual
or not.

Multiple inheritance is permitted, but multiple *implementation* inheritance is
strongly discouraged.

### Operator Overloading

Overload operators judiciously. Do not use user-defined literals.

Define overloaded operators only if their meaning is obvious, unsurprising, and
consistent with the corresponding built-in operators. For example, use `|` as a
bitwise- or logical-or, not as a shell-style pipe.

Define operators only on your own types. More precisely, define them in the same
headers, `.cc` files, and namespaces as the types they operate on. That way, the
operators are available wherever the type is, minimizing the risk of multiple
definitions. If possible, avoid defining operators as templates, because they
must satisfy this rule for any possible template arguments. If you define an
operator, also define any related operators that make sense, and make sure they
are defined consistently.

Prefer to define non-modifying binary operators as non-member functions. If a
binary operator is defined as a class member, implicit conversions will apply to
the right-hand argument, but not the left-hand one. It will confuse your users
if `a + b` compiles but `b + a` doesn't.

For a type `T` whose values can be compared for equality, define a non-member
`operator==` and document when two values of type `T` are considered equal. If
there is a single obvious notion of when a value `t1` of type `T` is less than
another such value `t2` then you may also define `operator<=>`, which should be
consistent with `operator==`. Prefer not to overload the other comparison and
ordering operators.

Don't go out of your way to avoid defining operator overloads. For example,
prefer to define `==`, `=`, and `<<`, rather than `Equals()`, `CopyFrom()`, and
`PrintTo()`. Conversely, don't define operator overloads just because other
libraries expect them. For example, if your type doesn't have a natural
ordering, but you want to store it in a `std::set`, use a custom comparator
rather than overloading `<`.

Do not overload `&&`, `||`, `,` (comma), or unary `&`. Do not overload
`operator""`, i.e., do not introduce user-defined literals. Do not use any such
literals provided by others (including the standard library).

Type conversion operators are covered in the section on
[implicit conversions](#implicit-conversions). The `=` operator is covered in
the section on [copy constructors](#copyable-and-movable-types). Overloading `<<` for use
with streams is covered in the section on [streams](#streams). See also the
rules on [function overloading](#function-overloading), which apply to operator
overloading as well.

### Access Control

Make classes' data members `private`, unless they are
[constants](#constant-names). This simplifies reasoning about invariants, at the
cost of some easy boilerplate in the form of accessors (usually `const`) if
necessary.

For technical reasons, we allow data members of a test fixture class defined in
a `.cc` file to be `protected` when using
[Google Test](https://github.com/google/googletest).
If a test fixture class is defined outside of the `.cc` file it is used in, for
example in a `.h` file, make data members `private`.

### Declaration Order

Group similar declarations together, placing `public` parts earlier.

A class definition should usually start with a `public:` section, followed by
`protected:`, then `private:`. Omit sections that would be empty.

Within each section, prefer grouping similar kinds of declarations together, and
prefer the following order:

1. Types and type aliases (`typedef`, `using`, `enum`, nested structs and
   classes, and `friend` types)
2. (Optionally, for structs only) non-`static` data members
3. Static constants
4. Factory functions
5. Constructors and assignment operators
6. Destructor
7. All other functions (`static` and non-`static` member functions, and
   `friend` functions)
8. All other data members (static and non-static)

When implementing an interface, group the declarations of its overridden member
functions together within each access section. Start each group with a comment
naming the interface, such as `// ITradingAdapter implementation.`. Use a separate
group for each interface, and keep class-specific methods outside these groups.

Do not put large method definitions inline in the class definition. Usually,
only trivial or performance-critical, and very short, methods may be defined
inline. See
[Defining Functions in Header Files](#defining-functions-in-header-files) for
more details.

## Functions

### Inputs and Outputs

The output of a C++ function is naturally provided via a return value and
sometimes via output parameters (or in/out parameters).

Prefer using return values over output parameters: they improve readability, and
often provide the same or better performance. See
[TotW #176](https://abseil.io/tips/176).

Prefer to return by value or, failing that, return by reference. Avoid returning
a raw pointer unless it can be null.

Parameters are either inputs to the function, outputs from the function, or
both. Non-optional input parameters should usually be values or `const`
references, while non-optional output and input/output parameters should usually
be references (which cannot be null). Generally, use `std::optional` to
represent optional by-value inputs, and use a `const` pointer when the
non-optional form would have used a reference. Use non-`const` pointers to
represent optional outputs and optional input/output parameters.

Avoid defining functions that require a reference parameter to outlive the call.
In some cases reference parameters can bind to temporaries, leading to lifetime
bugs. Instead, find a way to eliminate the lifetime requirement (for example, by
copying the parameter), or pass retained parameters by pointer and document the
lifetime and non-null requirements. See [TotW 116](https://abseil.io/tips/116)
for more.

When ordering function parameters, put all input-only parameters before any
output parameters. In particular, do not add new parameters to the end of the
function just because they are new; place new input-only parameters before the
output parameters. This is not a hard-and-fast rule. Parameters that are both
input and output muddy the waters, and, as always, consistency with related
functions may require you to bend the rule. Variadic functions may also require
unusual parameter ordering.

### Write Short Functions

Prefer small and focused functions.

We recognize that long functions are sometimes appropriate, so no hard limit is
placed on functions length. If a function exceeds about 40 lines, think about
whether it can be broken up without harming the structure of the program.

Even if your long function works perfectly now, someone modifying it in a few
months may add new behavior. This could result in bugs that are hard to find.
Keeping your functions short and simple makes it easier for other people to read
and modify your code. Small functions are also easier to test.

You could find long and complicated functions when working with
some code. Do
not be intimidated by modifying existing code: if working with such a function
proves to be difficult, you find that errors are hard to debug, or you want to
use a piece of it in several different contexts, consider breaking up the
function into smaller and more manageable pieces.

### Function Overloading

Use overloaded functions (including constructors) only if a reader looking at a
call site can get a good idea of what is happening without having to first
figure out exactly which overload is being called.

You may overload a function when there are no semantic differences between
variants. These overloads may vary in types, qualifiers, or argument count.
However, a reader of such a call must not need to know which member of the
overload set is chosen, only that **something** from the set is being called.

To reflect this unified design, prefer a single, comprehensive "umbrella"
comment that documents the entire overload set and is placed before the first
declaration.

Where a reader might have difficulty connecting the umbrella comment to a
specific overload, it's okay to have a comment for specific overloads.

### Default Arguments

Default arguments are allowed on non-virtual functions when the default is
guaranteed to always have the same value. Follow the same restrictions as for
[function overloading](#function-overloading), and prefer overloaded functions
if the readability gained with default arguments doesn't outweigh the downsides
below.

Default arguments are banned on virtual functions, where they don't work
properly, and in cases where the specified default might not evaluate to the
same value depending on when it was evaluated. (For example, don't write `void
f(int n = counter++);`.)

In some other cases, default arguments can improve the readability of their
function declarations enough to overcome the downsides above, so they are
allowed. When in doubt, use overloads.

### Trailing Return Type Syntax

Use trailing return types only where using the ordinary syntax (leading return
types) is impractical or much less readable.

In most cases, continue to use the older style of function declaration where the
return type goes before the function name. Use the new trailing-return-type form
only in cases where it's required (such as lambdas) or where, by putting the
type after the function's parameter list, it allows you to write the type in a
much more readable way. The latter case should be rare; it's mostly an issue in
fairly complicated template code, which is
[discouraged in most cases](#template-metaprogramming).

## Google-Specific Magic

There are various tricks and utilities that we use to make C++ code more robust,
and various ways we use C++ that may differ from what you see elsewhere.

### cpplint

Use `cpplint.py` to detect
style errors.

`cpplint.py` is a tool that
reads a source file and identifies many style errors. It is not perfect, and has
both false positives and false negatives, but it is still a valuable tool.

Some projects have instructions on how to run `cpplint.py` from their project
tools. If the project you are contributing to does not, you can download
[`cpplint.py`](https://raw.githubusercontent.com/cpplint/cpplint/HEAD/cpplint.py)
separately.

## Other C++ Features

### Ownership and Smart Pointers

Prefer to have single, fixed owners for dynamically allocated objects. Prefer to
transfer ownership with smart pointers.

If dynamic allocation is necessary, prefer to keep ownership with the code that
allocated it. If other code needs access to the object, consider passing it a
copy, or passing a pointer or reference without transferring ownership. Prefer
to use `std::unique_ptr` to make ownership transfer explicit. For example:

**Good example:**

```cpp
std::unique_ptr<Foo> FooFactory();
void FooConsumer(std::unique_ptr<Foo> ptr);
```

Do not design your code to use shared ownership without a very good reason. One
such reason is to avoid expensive copy operations, but you should only do this
if the performance benefits are significant, and the underlying object is
immutable (i.e., `std::shared_ptr<const Foo>`). If you do use shared ownership, prefer to use `std::shared_ptr`.

Never use `std::auto_ptr`.
Instead, use `std::unique_ptr`.

### Rvalue References

Use rvalue references only in certain special cases listed below.

Do not use rvalue references (or apply the `&&` qualifier to methods), except as
follows:

- You may use them to define move constructors and move assignment operators
  (as described in [Copyable and Movable Types](#copyable-and-movable-types)).
- You may use them to define `&&`-qualified methods that logically "consume"
  `*this`, leaving it in an unusable or empty state. Note that this applies
  only to method qualifiers (which come after the closing parenthesis of the
  function signature); if you want to "consume" an ordinary function
  parameter, prefer to pass it by value.
- You may use forwarding references in conjunction with
  [`std::forward`](http://en.cppreference.com/w/cpp/utility/forward), to
  support perfect forwarding.
- You may use them to define pairs of overloads, such as one taking `Foo&&`
  and the other taking `const Foo&`. Usually the preferred solution is just to
  pass by value, but an overloaded pair of functions sometimes yields better
  performance, for example if the functions sometimes don't consume the input.
  As always: if you're writing more complicated code for the sake of
  performance, make sure you have evidence that it actually helps.

### Friends

We allow use of `friend` classes and functions, within reason.

Friends should usually be defined in the same file so that the reader does not
have to look in another file to find uses of the private members of a class. A
common use of `friend` is to have a `FooBuilder` class be a friend of `Foo` so
that it can construct the inner state of `Foo` correctly, without exposing this
state to the world. In some cases it may be useful to make a unit test class a
friend of the class it tests.

Friends extend, but do not break, the encapsulation boundary of a class. In some
cases this is better than making a member `public` when you want to give only
one other class access to it. However, most classes should interact with other
classes solely through their public members.

### Exceptions

We do not use C++ exceptions.

This prohibition also applies to exception handling related features such as
`std::exception_ptr` and `std::nested_exception`.

There is an [exception](#windows-code) to this rule (no pun intended) for
Windows code.

### `noexcept`

Specify `noexcept` when it is useful and correct.

You may use `noexcept` when it is useful for performance if it accurately
reflects the intended semantics of your function, i.e., that if an exception is
somehow thrown from within the function body then it represents a fatal error.
You can assume that `noexcept` on move constructors has a meaningful performance
benefit. If you think there is significant performance benefit from specifying
`noexcept` on some other function, please discuss it with
your project
leads.

Prefer unconditional `noexcept` if exceptions are completely disabled (i.e.,
most Google C++ environments). Otherwise, use conditional `noexcept` specifiers
with simple conditions, in ways that evaluate false only in the few cases where
the function could potentially throw. The tests might include type traits check
on whether the involved operation might throw (e.g.,
`std::is_nothrow_move_constructible` for move-constructing objects), or on
whether allocation can throw (e.g., `absl::default_allocator_is_nothrow` for
standard default allocation). Note in many cases the only possible cause for an
exception is allocation failure (we believe move constructors should not throw
except due to allocation failure), and there are many applications where it's
appropriate to treat memory exhaustion as a fatal error rather than an
exceptional condition that your program should attempt to recover from.
Even for other potential failures you should prioritize
interface simplicity over supporting all possible exception throwing scenarios:
instead of writing a complicated `noexcept` clause that depends on whether a
hash function can throw, for example, simply document that your component
doesn't support hash functions throwing and make it unconditionally `noexcept`.

### Run-Time Type Information (RTTI)

Avoid using run-time type information (RTTI).

RTTI has legitimate uses but is prone to abuse, so you must be careful when
using it. You may use it freely in unit tests, but avoid it when possible in
other code. In particular, think twice before using RTTI in new code. If you
find yourself needing to write code that behaves differently based on the class
of an object, consider one of the following alternatives to querying the type:

- Virtual methods are the preferred way of executing different code paths
  depending on a specific subclass type. This puts the work within the object
  itself.
- If the work belongs outside the object and instead in some processing code,
  consider a double-dispatch solution, such as the Visitor design pattern.
  This allows a facility outside the object itself to determine the type of
  class using the built-in type system.

When the logic of a program guarantees that a given instance of a base class is
in fact an instance of a particular derived class, then a `dynamic_cast` may be
used freely on the object. Usually
[`absl::down_cast`](https://github.com/abseil/abseil-cpp/blob/master/absl/base/casts.h)
provides an even better alternative.

Decision trees based on type are a strong indication that your code is on the
wrong track.

**Bad example:**

```cpp
if (typeid(*data) == typeid(D1)) {
  ...
} else if (typeid(*data) == typeid(D2)) {
  ...
} else if (typeid(*data) == typeid(D3)) {
...
```

Code such as this usually breaks when additional subclasses are added to the
class hierarchy. Moreover, when properties of a subclass change, it is difficult
to find and modify all the affected code segments.

Do not hand-implement an RTTI-like workaround. The arguments against RTTI apply
just as much to workarounds like class hierarchies with type tags. Moreover,
workarounds disguise your true intent.

### Casting

Use C++-style casts like `static_cast<float>(double_value)`, or brace
initialization for conversion of arithmetic types like `int64_t y = int64_t{1}
<< 42`. Do not use cast formats like `(int)x` unless the cast is to `void`. You
may use cast formats like `T(x)` only when `T` is a class type.

In general, do not use C-style casts. Instead, use these C++-style casts when
explicit type conversion is necessary. Some are part of the standard C++
language, and some are defined in
[`absl/base/casts.h`](https://github.com/abseil/abseil-cpp/blob/master/absl/base/casts.h).
Use the first that **correctly applies**:

- Use brace initialization to convert arithmetic types (e.g., `int64_t{x}`).
  This is the safest approach because code will not compile if conversion can
  result in information loss. The syntax is also concise.
- When explicitly converting to a class type, use a function-style cast; e.g.,
  prefer `std::string(some_cord)` to `static_cast<std::string>(some_cord)`.
- Use `absl::implicit_cast` to safely cast up a type hierarchy, e.g., casting
  a `Foo*` to a `SuperclassOfFoo*` or casting a `Foo*` to a `const Foo*`. C++
  usually does this automatically but some situations need an explicit
  up-cast, such as use of the `?:` operator.
- Use `static_cast` as the equivalent of a C-style cast that does value
  conversion, when you need to explicitly up-cast a
  pointer from a class to its superclass, or when you need to explicitly cast
  a pointer from a superclass to a subclass. In this last case, you must be
  sure your object is actually an instance of the subclass.
- Use `absl::down_cast` to cast a pointer from a superclass to a subclass. You
  must be sure your object is actually an instance of the subclass.
- Use `const_cast` to remove the `const` qualifier (see
  [const](#use-of-const)).
- Use `reinterpret_cast` to do unsafe conversions of pointer types to and from
  integer and other pointer types, including `void*`. Use this only if you know what you
  are doing and you understand the aliasing issues. Also, consider
  dereferencing the pointer (without a cast) and using `std::bit_cast` to cast
  the resulting value.
- Use `std::bit_cast` to interpret the raw bits of a value using a different
  type of the same size (a type pun), such as interpreting the bits of a
  `double` as `int64_t`.

See the [RTTI section](#run-time-type-information-rtti) for guidance on the
use of `dynamic_cast`.

### Streams

Use streams where appropriate, and stick to "simple" usages. Overload `<<` for
streaming only for types representing values, and write only the user-visible
value, not any implementation details.

Use streams only when they are the best tool for the job. This is typically the
case when the I/O is ad-hoc, local, human-readable, and targeted at other
developers rather than end-users. Be consistent with the code around you, and
with the codebase as a whole; if there's an established tool for your problem,
use that tool instead. In particular, logging libraries are usually a
better choice than `std::cerr` or `std::clog` for diagnostic output, and the
libraries in
`absl/strings`
or the equivalent are usually a better choice than `std::stringstream`.

Avoid using streams for I/O that faces external users or handles untrusted data.
Instead, find and use the appropriate templating libraries to handle issues like
internationalization, localization, and security hardening.

If you do use streams, avoid the stateful parts of the streams API (other than
error state), such as `imbue()`, `xalloc()`, and `register_callback()`. Use
explicit formatting functions (such as `absl::StreamFormat()`) rather than
stream manipulators or formatting flags to control formatting details such as
number base, precision, or padding.

Overload `<<` as a streaming operator for your type only if your type represents
a value, and `<<` writes out a human-readable string representation of that
value. Avoid exposing implementation details in the output of `<<`; if you need
to print object internals for debugging, use named functions instead (a method
named `DebugString()` is the most common convention).

### Preincrement and Predecrement

Use the prefix form (`++i`) of the increment and decrement operators unless you
need postfix semantics.

Use prefix increment/decrement, unless the code explicitly needs the result of
the postfix increment/decrement expression.

### Use of const

In APIs, use `const` whenever it makes sense. `constexpr` is a better choice for
some uses of `const`.

We strongly recommend using `const` in APIs (i.e., on function parameters,
methods, and non-local variables) wherever it is meaningful and accurate. This
provides consistent, mostly compiler-verified documentation of what objects an
operation can mutate. Having a consistent and reliable way to distinguish reads
from writes is critical to writing thread-safe code, and is useful in many other
contexts as well. In particular:

- If a function guarantees that it will not modify an argument passed by
  reference or by pointer, the corresponding function parameter should be a
  reference-to-const (`const T&`) or pointer-to-const (`const T*`),
  respectively.
- For a function parameter passed by value, `const` has no effect on the
  caller, thus is not recommended in function declarations. See
  [TotW #109](https://abseil.io/tips/109).
- Declare methods to be `const` unless they alter the logical state of the
  object (or enable the user to modify that state, e.g., by returning a
  non-`const` reference, but that's rare), or they can't safely be invoked
  concurrently.

Using `const` on local variables is neither encouraged nor discouraged.

All of a class's `const` operations should be safe to invoke concurrently with
each other. If that's not feasible, the class must be clearly documented as
"thread-unsafe".

#### Where to put the const

Prefer putting `const` first, as in `const int* foo`, but this is not
required. Be consistent with the code around you.

### Use of constexpr, constinit, and consteval

Use `constexpr` to define true constants or to ensure constant initialization.
Use `constinit` to ensure constant initialization for non-constant variables.

`constexpr` definitions enable a more robust specification of the constant parts
of an interface. Use `constexpr` to specify true constants and the functions
that support their definitions. `consteval` may be used for code that must not
be invoked at runtime. Avoid complexifying function definitions to enable their
use with `constexpr`. Do not use `constexpr` or `consteval` to force inlining.

### Integer Types

Of the built-in C++ integer types, the only one used is `int`. If a program needs an integer type of a different size,
use an exact-width integer type from `<stdint.h>`, such as `int16_t`. If you
have a value that could ever be greater than or equal to 231, use a
64-bit type such as `int64_t`. Keep in mind that even if your value won't ever
be too large for an `int`, it may be used in intermediate calculations which may
require a larger type. When in doubt, choose a larger type.

The standard library header `<stdint.h>` defines types like `int16_t`,
`uint32_t`, `int64_t`, etc. You should always use those in preference to
`short`, `unsigned long long`, and the like, when you need a guarantee on the
size of an integer. Of the built-in integer types, only `int` should be used.
When appropriate, you are welcome to use standard type aliases like `size_t` and
`ptrdiff_t`.

We use `int` very often, for integers we know are not going to be too big, e.g.,
loop counters. Use plain old `int` for such things. You should assume that an
`int` is
at
least 32 bits, but don't assume that it has more than 32 bits. If you
need a 64-bit integer type, use `int64_t` or `uint64_t`.

For integers we know can be "big", use `int64_t`.

You should not use the unsigned integer types such as `uint32_t`, unless there
is a valid reason such as representing a bit pattern rather than a number, or
you need defined overflow modulo 2N. In particular, do not use
unsigned types to say a number will never be negative. Instead, use
assertions
for this.

Prefer to omit the `std::` prefix on standard library integer types, as the
extra 5 characters do not merit the added clutter.

If your code is a container that returns a size, be sure to use a type that will
accommodate any possible usage of your container. When in doubt, use a larger
type rather than a smaller type.

Use care when converting integer types. Integer conversions and promotions can
cause undefined behavior, leading to security bugs and other problems.

#### On Unsigned Integers

Unsigned integers are good for representing bitfields and modular arithmetic.
Because of historical accident, the C++ standard also uses unsigned integers to
represent the size of containers - many members of the standards body believe
this to be a mistake, but it is effectively impossible to fix at this point. The
fact that unsigned arithmetic doesn't model the behavior of a simple integer,
but is instead defined by the standard to model modular arithmetic (wrapping
around on overflow/underflow), means that a significant class of bugs cannot be
diagnosed by the compiler. In other cases, the defined behavior impedes
optimization.

That said, mixing signedness of integer types is responsible for an equally
large class of problems. The best advice we can provide: try to use iterators
and containers rather than pointers and sizes, try not to mix signedness, and
try to avoid unsigned types (except for representing bitfields or modular
arithmetic). Do not use an unsigned type merely to assert that a variable is
non-negative.

### Floating-Point Types

Of the built-in C++ floating-point types, the only ones used
are `float` and `double`. You may
assume that these types represent IEEE-754 binary32 and binary64, respectively.

Do not use `long double`, as it gives non-portable results.

### Architecture Portability

Write architecture-portable code. Do not rely on CPU features specific to a
single processor.

- When printing values, use type-safe numeric formatting libraries like
  [`absl::StrCat`](https://github.com/abseil/abseil-cpp/blob/master/absl/strings/str_cat.h),
  [`absl::Substitute`](https://github.com/abseil/abseil-cpp/blob/master/absl/strings/substitute.h),
  [`absl::StrFormat`](https://github.com/abseil/abseil-cpp/blob/master/absl/strings/str_format.h),
  or [`std::ostream`](#streams) instead of the `printf` family of functions.
- When moving structured data into or out of your process, encode it using a
  serialization library like
  [Protocol Buffers](https://protobuf.dev/)
  rather than copying the in-memory representation around.
- If you need to work with memory addresses as integers, store them in
  `uintptr_t`s rather than `uint32_t`s or `uint64_t`s.
- Use [braced-initialization](#casting) as needed to create 64-bit constants.
  For example:

  **Good example:**

  ```cpp
  int64_t my_value{0x123456789};
  uint64_t my_mask{uint64_t{3} << 48};
  ```
- Use portable [floating point types](#floating-point-types); avoid `long
  double`.
- Use portable [integer types](#integer-types); avoid `short`, `long`, and
  `long long`.

### Preprocessor Macros

Avoid defining macros, especially in headers; prefer inline functions, enums,
and `const` variables. Name macros with your project's namespace as a prefix. Do
not use macros to declare pieces of a C++ API.

Macros mean that the code you see is not the same as the code the compiler sees.
This can introduce unexpected behavior, especially since macros have global
scope.

The problems introduced by macros are especially severe when they are used to
declare pieces of a C++ API, and still more so for public APIs. Every error
message from the compiler when developers incorrectly use that interface now
must explain how the macros formed the interface. Refactoring and analysis tools
have a dramatically harder time updating the interface. As a consequence, we
specifically disallow using macros in this way. For example, avoid patterns
like:

**Bad example:**

```cpp
class WOMBAT_TYPE(Foo) {
  // ...

 public:
  EXPAND_PUBLIC_WOMBAT_API(Foo)

  EXPAND_WOMBAT_COMPARISONS(Foo, ==, <)
};
```

Luckily, macros are not nearly as necessary in C++ as they are in C. Instead of
using a macro to inline performance-critical code, use an inline function.
Instead of using a macro to store a constant, use a `const` variable. Instead of
using a macro to "abbreviate" a long variable name, use a reference. Instead of
using a macro to conditionally compile code ... well, don't do that at all
(except, of course, for the `#define` guards to prevent double inclusion of
header files). It makes testing much more difficult.

Macros can do things these other techniques cannot, and you do see them in the
codebase, especially in the lower-level libraries. And some of their special
features (like stringifying, concatenation, and so forth) are not available
through the language proper. But before using a macro, consider carefully
whether there's a non-macro way to achieve the same result. If you need to add a
macro which is used to declare an API, contact
your project leads to request a waiver of this
rule.

The following usage pattern will avoid many problems with macros; if you use
macros, follow it whenever possible:

- Don't define macros in a `.h` file.
- `#define` macros right before you use them, and `#undef` them right after.
- Do not just `#undef` an existing macro before replacing it with your own;
  instead, pick a name that's likely to be unique.
- Try not to use macros that expand to unbalanced C++ constructs, or at least
  document that behavior well.
- Prefer not using `##` to generate function/class/variable names.

Exporting macros from headers (i.e., defining them in a header without
`#undef`ing them before the end of the header) is extremely strongly
discouraged. If you do export a macro from a header, it must have a globally
unique name. To achieve this, it must be named with a prefix consisting of your
project's namespace name (but upper case).

### 0 and nullptr/NULL

Use `nullptr` for pointers, and `'\0'` for chars (and not the `0` literal).

For pointers (address values), use `nullptr`, as this provides type-safety.

Use `'\0'` for the null character. Using the correct type makes the code more
readable.

### sizeof

Prefer `sizeof(varname)` to `sizeof(type)`.

Use `sizeof(varname)` when you take the size of a particular variable.
`sizeof(varname)` will update appropriately if someone changes the variable type
either now or later. You may use `sizeof(type)` for code unrelated to any
particular variable, such as code that manages an external or internal data
format where a variable of an appropriate C++ type is not convenient.

**Good example:**

```cpp
MyStruct data;
memset(&data, 0, sizeof(data));
```

**Bad example:**

```cpp
memset(&data, 0, sizeof(MyStruct));
```

**Good example:**

```cpp
if (raw_size < sizeof(int)) {
  LOG(ERROR) << "compressed record not big enough for count: " << raw_size;
  return false;
}
```

### Type Deduction (including auto)

Use type deduction only if it makes the code clearer to readers who aren't
familiar with the project, or if it makes the code safer. Do not use it merely
to avoid the inconvenience of writing an explicit type.

The fundamental rule is: use type deduction only to make the code clearer or
safer, and do not use it merely to avoid the inconvenience of writing an
explicit type. When judging whether the code is clearer, keep in mind that your
readers are not necessarily on your team, or familiar with your project, so
types that you and your reviewer experience as unnecessary clutter will very
often provide useful information to others. For example, you can assume that the
return type of `make_unique<Foo>()` is obvious, but the return type of
`MyWidgetFactory()` probably isn't.

These principles apply to all forms of type deduction, but the details vary, as
described in the following sections.

#### Function template argument deduction

Function template argument deduction is almost always OK. Type deduction is the
expected default way of interacting with function templates, because it allows
function templates to act like infinite sets of ordinary function overloads.
Consequently, function templates are almost always designed so that template
argument deduction is clear and safe, or doesn't compile.

#### Local variable type deduction

For local variables, you can use type deduction to make the code clearer by
eliminating type information that is obvious or irrelevant, so that the reader
can focus on the meaningful parts of the code:

```cpp
std::unique_ptr<WidgetWithBellsAndWhistles> widget =
    std::make_unique<WidgetWithBellsAndWhistles>(arg1, arg2);
absl::flat_hash_map<std::string,
                    std::unique_ptr<WidgetWithBellsAndWhistles>>::const_iterator
    it = my_map_.find(key);
std::array<int, 6> numbers = {4, 8, 15, 16, 23, 42};
```

**Good example:**

```cpp
auto widget = std::make_unique<WidgetWithBellsAndWhistles>(arg1, arg2);
auto it = my_map_.find(key);
std::array numbers = {4, 8, 15, 16, 23, 42};
```

Types sometimes contain a mixture of useful information and boilerplate, such as
`it` in the example above: it's obvious that the type is an iterator, and in
many contexts the container type and even the key type aren't relevant, but the
type of the values is probably useful. In such situations, it's often possible
to define local variables with explicit types that convey the relevant
information:

**Good example:**

```cpp
if (auto it = my_map_.find(key); it != my_map_.end()) {
  WidgetWithBellsAndWhistles& widget = *it->second;
  // Do stuff with `widget`
}
```

If the type is a template instance, and the parameters are boilerplate but the
template itself is informative, you can use class template argument deduction to
suppress the boilerplate. However, cases where this actually provides a
meaningful benefit are quite rare. Note that class template argument deduction
is also subject to a [separate style rule](#class-template-argument-deduction).

Explicit types for elements of `map`-like containers are easy to get wrong. In
those cases [structured bindings](#structured-bindings) for the key and value
are almost always clearer than a single variable representing the pair.

Do not use `decltype(auto)` if a simpler option will work; because it's a fairly
obscure feature, it has a high cost in code clarity.

See
[TotW #232](https://abseil.io/tips/232)
for more details on when to use `auto` for variable declarations.

#### Return type deduction

Use return type deduction (for both functions and lambdas) only if the function
body has a very small number of `return` statements, and very little other code,
because otherwise the reader may not be able to tell at a glance what the return
type is. Furthermore, use it only if the function or lambda has a very narrow
scope, because functions with deduced return types don't define abstraction
boundaries: the implementation *is* the interface. In particular, public
functions in header files should almost never have deduced return types.

#### Function parameter type deduction

Avoid using parameter type deduction (using either `auto` or template
parameters) if you can give the parameter a specific type instead. If it will
only be called with one type, it will almost always be clearer to make that type
explicit, unless it is called very close to where it's defined (so that the
reader can easily see both), or it's a lambda being passed to an interface so
well-known that it's obvious what type it will eventually be called with (e.g.,
the `std::sort` example above)

Do not use `auto` parameters in non-lambda functions. Use named template
parameters instead, because they are more explicit about the fact that the
function is a template:

**Bad example:**

```cpp
void F(auto arg) { ... }
```

**Good example:**

```cpp
template <typename T>
void F(T arg) { ... }
```

In a lambda expression, if you need to refer to the type of a parameter, prefer
to make that type a template parameter:

**Bad example:**

```cpp
[](auto x) { decltype(x) y; ... }
```

**Good example:**

```cpp
[]<typename T>(T x) { T y; ... }
```

#### Lambda init captures

Init captures are covered by a [more specific style rule](#lambda-expressions),
which largely supersedes the general rules for type deduction.

#### Structured bindings

Unlike other forms of type deduction, structured bindings can actually give the
reader additional information, by giving meaningful names to the elements of a
larger object. This means that a structured binding declaration may provide a
net readability improvement over an explicit type, even in cases where `auto`
would not. Structured bindings are especially beneficial when the object is a
pair or tuple (as in the `insert` example above), because they don't have
meaningful field names to begin with, but note that you generally
[shouldn't use pairs or tuples](#structs-vs-pairs-and-tuples) unless a pre-existing API
like `insert` forces you to.

The elements of containers like `std::map<K, V>` benefit especially from
structured bindings; not only can the two parts of the pair be given more
specific names, but the use of type deduction dodges the common bug of failing
to `const`-qualify the key type of the element, which often surprisingly
succeeds in compiling by introducing unintended copying.

If the object being bound is a struct, it may sometimes be helpful to provide
names that are more specific to your usage, but keep in mind that this may also
mean the names are less recognizable to your reader than the field names. We
recommend using a comment to indicate the name of the underlying field, if it
doesn't match the name of the binding, using the same syntax as for function
parameter comments:

**Good example:**

```cpp
auto [/*field_name1=*/bound_name1, /*field_name2=*/bound_name2] = ...
```

As with function parameter comments, this can enable tools to detect if you get
the order of the fields wrong.

### Class Template Argument Deduction

Use class template argument deduction only with templates that have explicitly
opted into supporting it.

Do not use CTAD with a given template unless the template's maintainers have
opted into supporting use of CTAD by providing at least one explicit deduction
guide (all templates in the `std` namespace are also presumed to have opted in).
This should be enforced with a compiler warning if
available.

Uses of CTAD must also follow the general rules on
[Type deduction](#type-deduction-including-auto).

### Designated Initializers

Use designated initializers only in their C++20-compliant form.

Use designated initializers only in the form that is compatible with the C++20
standard: with initializers in the same order as the corresponding fields appear
in the struct definition.

### Lambda Expressions

Use lambda expressions where appropriate. Prefer explicit captures when the
lambda will escape the current scope.

- Use lambda expressions where appropriate, with formatting as described
  [below](#lambda-expressions-1).
- Prefer explicit captures if the lambda may escape the current scope. For
  example, instead of:

  **Bad example:**

  ```cpp
  {
    Foo foo;
    ...
    executor->Schedule([&] { Frobnicate(foo); })
    ...
  }
  // BAD! The fact that the lambda makes use of a reference to `foo` and
  // possibly `this` (if `Frobnicate` is a member function) may not be
  // apparent on a cursory inspection. If the lambda is invoked after
  // the function returns, that would be bad, because both `foo`
  // and the enclosing object could have been destroyed.
  ```

  prefer to write:

  **Good example:**

  ```cpp
  {
    Foo foo;
    ...
    executor->Schedule([&foo] { Frobnicate(foo); })
    ...
  }
  // BETTER - The compile will fail if `Frobnicate` is a member
  // function, and it's clearer that `foo` is dangerously captured by
  // reference.
  ```
- Use default capture by reference (`[&]`) only when the lifetime of the
  lambda is obviously shorter than any potential captures.
- Use default capture by value (`[=]`) only as a means of binding a few
  variables for a short lambda, where the set of captured variables is obvious
  at a glance, and which does not result in capturing `this` implicitly. (That
  means that a lambda that appears in a non-static class member function and
  refers to non-static class members in its body must capture `this`
  explicitly or via `[&]`.) Prefer not to write long or complex lambdas with
  default capture by value.
- Use captures only to actually capture variables from the enclosing scope. Do
  not use captures with initializers to introduce new names, or to
  substantially change the meaning of an existing name. Instead, declare a new
  variable in the conventional way and then capture it, or avoid the lambda
  shorthand and define a function object explicitly.
- See the section on [type deduction](#type-deduction-including-auto) for guidance on
  specifying the parameter and return types.

### Template Metaprogramming

Avoid complicated template programming.

Template metaprogramming sometimes allows cleaner and easier-to-use interfaces
than would be possible without it, but it's also often a temptation to be overly
clever. It's best used in a small number of low level components where the extra
maintenance burden is spread out over a large number of uses.

Think twice before using template metaprogramming or other complicated template
techniques; think about whether the average member of your team will be able to
understand your code well enough to maintain it after you switch to another
project, or whether a non-C++ programmer or someone casually browsing the
codebase will be able to understand the error messages or trace the flow of a
function they want to call. If you're using recursive template instantiations or
type lists or metafunctions or expression templates, or relying on SFINAE or on
the `sizeof` trick for detecting function overload resolution, then there's a
good chance you've gone too far.

If you use template metaprogramming, you should expect to put considerable
effort into minimizing and isolating the complexity. You should hide
metaprogramming as an implementation detail whenever possible, so that
user-facing headers are readable, and you should make sure that tricky code is
especially well commented. You should carefully document how the code is used,
and you should say something about what the "generated" code looks like. Pay
extra attention to the error messages that the compiler emits when users make
mistakes. The error messages are part of your user interface, and your code
should be tweaked as necessary so that the error messages are understandable and
actionable from a user point of view.

### Concepts and Constraints

Use concepts sparingly. In general, concepts and constraints should only be used
in cases where templates would have been used prior to C++20. Avoid introducing
new concepts in headers, unless the headers are marked as internal to the
library. Do not define concepts that are not enforced by the compiler. Prefer
constraints over template metaprogramming, and avoid the `template<Concept T>`
syntax; instead, use the `requires(Concept<T>)` syntax.

Predefined concepts in the standard library should be preferred to type traits,
when equivalent ones exist. (e.g., if `std::is_integral_v` would have been used
before C++20, then `std::integral` should be used in C++20 code.) Similarly,
prefer modern constraint syntax (via `requires(Condition)`). Avoid legacy
template metaprogramming constructs (such as `std::enable_if<Condition>`) as
well as the `template<Concept T>` syntax.

Do not manually re-implement any existing concepts or traits. For example, use
`requires(std::default_initializable<T>)` instead of `requires(requires { T v;
})` or the like.

New `concept` declarations should be rare, and only defined internally within a
library, such that they are not exposed at API boundaries. More generally, do
not use concepts or constraints in cases where you wouldn't use their legacy
template equivalents in C++17.

Do not define concepts that duplicate the function body or impose requirements
that would be insignificant or obvious from reading the body of the code or the
resulting error messages. For example, avoid the following:

**Bad example:**

```cpp
template <typename T>     // Bad - redundant with negligible benefit
concept Addable = std::copyable<T> && requires(T a, T b) { a + b; };
template <Addable T>
T Add(T x, T y, T z) { return x + y + z; }
```

Instead, prefer to leave code as an ordinary template unless you can demonstrate
that concepts or constraints result in significant improvement for that
particular case, such as in the resulting error messages for a deeply nested or
non-obvious requirement.

Concepts should be statically verifiable by the compiler. Do not use any concept
whose primary benefits would come from a semantic (or otherwise unenforced)
constraint. Requirements that are unenforced at compile time should instead be
imposed via other mechanisms such as comments, assertions, or tests.

### C++20 modules

Do not use C++20 Modules.

C++20 introduces "modules", a new language feature designed as an alternative to
textual inclusion of header files. It introduces three new keywords to support
this: `module`, `export`, and `import`.

Modules are a big shift in how C++ is written and compiled, and we are still
assessing how they may fit into Google's C++ ecosystem in the future.
Furthermore, they are not currently well-supported by our build systems,
compilers, and other tooling, and need further exploration as to the best
practices when writing and using them.

### Coroutines

Only use C++20 coroutines via libraries that have been approved by your project
leads.

Use only coroutine libraries that have been approved for project-wide use by
your project leads. Do not roll your own promise or awaitable types.

### Disallowed standard library features

Some modern C++ library functionality encourages coding practices that hamper
readability — for example by removing checked redundancy (such as type names)
that may be helpful to readers, or by encouraging template metaprogramming.
Other extensions duplicate functionality available through existing
mechanisms, which may lead to confusion
and conversion costs.

The following C++ standard library features may not be used:

- Compile-time rational numbers (`<ratio>`), because of concerns that it's
  tied to a more template-heavy interface style.
- The `<cfenv>` and `<fenv.h>` headers, because many compilers do not support
  those features reliably.
- The `<filesystem>` header, which does not have sufficient support for
  testing, and suffers from inherent security vulnerabilities.

### Third-party Libraries

When choosing to use third-party libraries, be consistent with the code around
you.

When choosing a library to solve a particular problem, first check if there's
already a library that your codebase typically uses for that kind of problem. If
so, use it. If not, prefer libraries from the following sources, in roughly
decreasing order of priority:

- [Abseil](https://abseil.io/) .
- The C++ standard library (except as discussed [here](#disallowed-standard-library-features))
- Existing first-party libraries in the codebase.
- Third-party libraries

If you choose to use a third-party library, make sure to follow
your codebase's policies for third-party code.

### Nonstandard Extensions

Nonstandard extensions to C++ may not be used unless otherwise specified.

Do not use nonstandard extensions. You may use portability wrappers that are
implemented using nonstandard extensions, so long as those wrappers
are provided by a designated project-wide portability
header.

### Aliases

Public aliases are for the benefit of an API's user, and should be clearly
documented.

Don't put an alias in your public API just to save typing in the implementation;
do so only if you intend it to be used by your clients.

When defining a public alias, document the intent of the new name, including
whether it is guaranteed to always be the same as the type it's currently
aliased to, or whether a more limited compatibility is intended. This lets the
user know whether they can treat the types as substitutable or whether more
specific rules must be followed, and can help the implementation retain some
degree of freedom to change the alias.

Don't put namespace aliases in your public API. (See also
[Namespaces](#namespaces).)

For example, these aliases document how they are intended to be used in client
code:

**Good example:**

```cpp
namespace mynamespace {
// Used to store field measurements. DataPoint may change from Bar* to some internal type.
// Client code should treat it as an opaque pointer.
using DataPoint = ::foo::Bar*;

// A set of measurements. Just an alias for user convenience.
using TimeSeries = std::unordered_set<DataPoint, std::hash<DataPoint>, DataPointComparator>;
}  // namespace mynamespace
```

These aliases don't document intended use, and half of them aren't meant for
client use:

**Bad example:**

```cpp
namespace mynamespace {
// Bad: none of these say how they should be used.
using DataPoint = ::foo::Bar*;
using ::std::unordered_set;  // Bad: just for local convenience
using ::std::hash;           // Bad: just for local convenience
typedef unordered_set<DataPoint, hash<DataPoint>, DataPointComparator> TimeSeries;
}  // namespace mynamespace
```

However, local convenience aliases are fine in function definitions, `private`
sections of classes, explicitly-marked internal namespaces, and in `.cc` files:

**Good example:**

```cpp
// In a .cc file
using ::foo::Bar;
```

### Switch Statements

If not conditional on an enumerated value, switch statements should always have
a `default` case (in the case of an enumerated value, the compiler will warn you
if any values are not handled). If the default case should never execute, treat
this as an error. For example:

**Good example:**

```cpp
switch (var) {
  case 0: {
    ...
    break;
  }
  case 1: {
    ...
    break;
  }
  default: {
    LOG(FATAL) << "Invalid value in switch statement: " << var;
  }
}
```

Fall-through from one case label to another must be annotated using the
`[[fallthrough]];` attribute. `[[fallthrough]];` should be placed at a point of
execution where a fall-through to the next case label occurs. A common exception
is consecutive case labels without intervening code, in which case no annotation
is needed.

**Good example:**

```cpp
switch (x) {
  case 41:  // No annotation needed here.
  case 43:
    if (dont_be_picky) {
      // Use this instead of or along with annotations in comments.
      [[fallthrough]];
    } else {
      CloseButNoCigar();
      break;
    }
  case 42:
    DoSomethingSpecial();
    [[fallthrough]];
  default:
    DoSomethingGeneric();
    break;
}
```

## Inclusive Language

In all code, including naming and comments, use inclusive language and avoid
terms that other programmers might find disrespectful or offensive (such as
"master" and "slave", "blacklist" and "whitelist", or "redline"), even if the
terms also have an ostensibly neutral meaning. Similarly, use gender-neutral
language unless you're referring to a specific person (and using their
pronouns). For example, use "they"/"them"/"their" for people of unspecified
gender
([even when singular](https://apastyle.apa.org/style-grammar-guidelines/grammar/singular-they)),
and "it"/"its" for software, computers, and other things that aren't people.

## Naming

The most important consistency rules are those that govern naming. The style of
a name immediately informs us what sort of thing the named entity is: a type, a
variable, a function, a constant, a macro, etc., without requiring us to search
for the declaration of that entity. The pattern-matching engine in our brains
relies a great deal on these naming rules.

Style rules about naming are pretty arbitrary, but we feel that consistency is more important than individual
preferences in this area, so regardless of whether you find them sensible or
not, the rules are the rules.

For the purposes of the naming rules below, a "word" is anything that you would
write in English without internal spaces. Either words are all lowercase, with
underscores between words
("[snake\_case](https://en.wikipedia.org/wiki/Snake_case)"), or words are mixed
case with the first letter of each word capitalized
("[camelCase](https://en.wikipedia.org/wiki/Camel_case)" or
"[PascalCase](https://en.wiktionary.org/wiki/Pascal_case)").

### Choosing Names

Give things names that make their purpose or intent understandable to a new
reader, even someone on a different team than the owners. Do not worry about
saving horizontal space as it is far more important to make your code
immediately understandable by a new reader.

Consider the context in which the name will be used. A name should be
descriptive even if it is used far from the code that makes it available for
use. However, a name should not distract the reader by repeating information
that's present in the immediate context. Generally, this means that
descriptiveness should be proportional to the name's scope of visibility. A free
function declared in a header should probably mention the header's library,
while a local variable probably shouldn't explain what function it's within.

Minimize the use of abbreviations that would likely be unknown to someone
outside your project (especially acronyms and initialisms). Do not abbreviate by
deleting letters within a word. When an abbreviation is used, prefer to
capitalize it as a single "word", e.g., `StartRpc()` rather than `StartRPC()`.
As a rule of thumb, an abbreviation is probably OK if it's listed in
Wikipedia.
Note that certain universally-known abbreviations are OK, such as `i` for a loop
index and `T` for a template parameter.

The names you see most frequently are not like most names; a small number of
"vocabulary" names are reused so widely that they are always in context. These
names tend to be short or even abbreviated and their full meaning comes from
explicit long-form documentation rather than from just comments on their
definition and the words within the names. For example, `absl::Status` has a
dedicated
[page in a devguide](https://abseil.io/docs/cpp/guides/status),
documenting its proper use. You probably won't define new vocabulary names very
often, but if you do, get
additional design review to make sure the chosen names work well when
used widely.

**Good example:**

```cpp
class MyClass {
 public:
  int CountFooErrors(const std::vector<Foo>& foos) {
    int n = 0;  // Clear meaning given limited scope and context
    for (const auto& foo : foos) {
      ...
      ++n;
    }
    return n;
  }
  // Function comment doesn't need to explain that this returns non-OK on
  // failure as that is implied by the `absl::Status` return type, but it
  // might document behavior for some specific codes.
  absl::Status DoSomethingImportant() {
    std::string fqdn = ...;  // Well-known abbreviation for Fully Qualified Domain Name
    return absl::OkStatus();
  }
 private:
  const int kMaxAllowedConnections = ...;  // Clear meaning within context
};
```

**Bad example:**

```cpp
class MyClass {
 public:
  int CountFooErrors(const std::vector<Foo>& foos) {
    int total_number_of_foo_errors = 0;  // Overly verbose given limited scope and context
    for (int foo_index = 0; foo_index < foos.size(); ++foo_index) {  // Use idiomatic `i`
      ...
      ++total_number_of_foo_errors;
    }
    return total_number_of_foo_errors;
  }
  // A return type with a generic name is unclear without widespread education.
  Result DoSomethingImportant() {
    int cstmr_id = ...;  // Deletes internal letters
  }
 private:
  const int kNum = ...;  // Unclear meaning within broad scope
};
```

### File Names

Filenames should be all lowercase and can include underscores (`_`) or dashes
(`-`). Follow the convention that your
project uses. If there is no consistent local
pattern to follow, prefer "`_`".

Examples of acceptable file names:

- `my_useful_class.cc`
- `my-useful-class.cc`
- `myusefulclass.cc`
- `myusefulclass_test.cc // _unittest and _regtest are deprecated.`

C++ files should have a `.cc` filename extension, and header files should have a
`.h` extension. Files that rely on being textually included at specific points
should end in `.inc` (see also the section on
[self-contained headers](#self-contained-headers)).

Do not use filenames that already exist in `/usr/include`, such as `db.h`.

In general, make your filenames very specific. For example, use
`http_server_logs.h` rather than `logs.h`. A very common case is to have a pair
of files called, e.g., `foo_bar.h` and `foo_bar.cc`, defining a class called
`FooBar`.

### Type Names

Type names start with a capital letter and have a capital letter for each new
word, with no underscores: `MyExcitingClass`, `MyExcitingEnum`.

The names of all types — classes, structs, type aliases, enums, and type
template parameters — have the same naming convention. Type names should start
with a capital letter and have a capital letter for each new word. No
underscores. For example:

**Good example:**

```cpp
// classes and structs
class UrlTable { ...
class UrlTableTester { ...
struct UrlTableProperties { ...

// typedefs
typedef hash_map<UrlTableProperties*, std::string> PropertiesMap;

// using aliases
using PropertiesMap = hash_map<UrlTableProperties*, std::string>;

// enums
enum class UrlTableError { ...
```

### Concept Names

Concept names follow the same rules as [type names](#type-names).

### Variable Names

The names of variables (including function parameters) and data members are
`snake_case` (all lowercase, with underscores between words). Data members of
classes (but not structs) additionally have trailing underscores. For instance:
`a_local_variable`, `a_struct_data_member`, `a_class_data_member_`.

#### Common Variable names

For example:

**Good example:**

```cpp
std::string table_name;  // OK - snake_case.
```

**Bad example:**

```cpp
std::string tableName;   // Bad - mixed case.
```

#### Class Data Members

Data members of classes, both static and non-static, are named like ordinary
nonmember variables, but with a trailing underscore. The exception to this is
static constant class members, which should follow the rules for
[naming constants](#constant-names).

**Good example:**

```cpp
class TableInfo {
 public:
  ...
  static const int kTableVersion = 3;  // OK - constant naming.
  ...

 private:
  std::string table_name_;             // OK - underscore at end.
  static Pool<TableInfo>* absl_nullable pool_;       // OK.
};
```

#### Struct Data Members

Data members of structs, both static and non-static, are named like ordinary
nonmember variables. They do not have the trailing underscores that data members
in classes have.

**Good example:**

```cpp
struct UrlTableProperties {
  std::string name;
  int num_entries;
  static Pool<UrlTableProperties>* absl_nullable pool;
};
```

See [Structs vs. Classes](#structs-vs-classes) for a discussion of when to use
a struct versus a class.

### Constant Names

Variables declared `constexpr` or `const`, and whose value is fixed for the
duration of the program, are named with a leading "k" followed by mixed case.
Underscores can be used as separators in the rare cases where capitalization
cannot be used for separation. For example:

**Good example:**

```cpp
const int kDaysInAWeek = 7;
const int kAndroid8_0_0 = 24;  // Android 8.0.0
```

All such variables with
[static storage duration](http://en.cppreference.com/w/cpp/language/storage_duration#Storage_duration)
should be named this way, including static constant class data members and
variables in templates where different instantiations of the template may have
different values. For example:

**Good example:**

```cpp
template <int kMaxAttempts>
class ExponentialBackoff final {
  static constexpr absl::string_view kLogTag = "exponential backoff";
  ...
};
```

This convention is optional for variables of other storage classes, e.g.,
automatic variables; otherwise the usual variable naming rules apply.

For example:

**Good example:**

```cpp
void ComputeFoo(absl::string_view suffix) {
  // Either of these is acceptable.
  const absl::string_view kPrefix = "prefix";
  const absl::string_view prefix = "prefix";
  ...
}
```

**Bad example:**

```cpp
void ComputeFoo(absl::string_view suffix) {
  // Bad - different invocations of ComputeFoo give kCombined different values.
  const std::string kCombined = absl::StrCat(kPrefix, suffix);
  ...
}
```

### Function Names

Ordinarily, functions follow
[PascalCase](https://en.wiktionary.org/wiki/Pascal_case): start with a capital
letter and have a capital letter for each new word.

**Good example:**

```cpp
AddTableEntry()
DeleteUrl()
OpenFileOrDie()
```

The same naming rule applies to class- and namespace-scope constants that are
exposed as part of an API and that are intended to look like functions, because
the fact that they're objects rather than functions is an unimportant
implementation detail.

Accessors and mutators (get and set functions) may be named like variables, in
`snake_case`. These often correspond to actual member variables, but this is not
required. For example, `int count()` and `void set_count(int count)`.

### Namespace Names

Namespace names are `snake_case` (all lowercase, with underscores between
words).

When [choosing names](#choosing-names) for namespaces, note that names
must be fully qualified when used in a header outside the namespace, because
[unqualified Aliases are generally banned](#aliases).

Top-level namespaces must be globally unique and recognizable, so each one should be owned by a single project or team, with a name based on the name of that project or team. Usually, all code in the namespace should be under one or more directories with the same name as the namespace.

Nested namespaces should avoid the names of well-known top-level namespaces,
especially `std` and `absl`, because in C++, nested namespaces do not protect
from collisions with names in other namespaces (see
[TotW #130](https://abseil.io/tips/130)).

### Enumerator Names

Enumerators (for both scoped and unscoped enums) should be named like
[constants](#constant-names), not like [macros](#macro-names). That is, use
`kEnumName` not `ENUM_NAME`.

**Good example:**

```cpp
enum class UrlTableError {
  kOk = 0,
  kOutOfMemory,
  kMalformedInput,
};
```

**Bad example:**

```cpp
enum class AlternateUrlTableError {
  OK = 0,
  OUT_OF_MEMORY = 1,
  MALFORMED_INPUT = 2,
};
```

Until January 2009, the style was to name enum values like
[macros](#macro-names). This caused problems with name collisions between enum
values and macros. Hence, the change to prefer constant-style naming was put in
place. New code should use constant-style naming.

### Template Parameter Names

Template parameters should follow the naming style for their category: type
template parameters should follow the rules for naming [types](#type-names), and
non-type template parameters should follow the rules for naming
[variables](#variable-names) or [constants](#constant-names).

### Macro Names

You're not really going to [define a macro](#preprocessor-macros), are you? If
you do, they're like this:
`MY_MACRO_THAT_SCARES_SMALL_CHILDREN_AND_ADULTS_ALIKE`.

Please see the [description of macros](#preprocessor-macros); in general macros
should *not* be used. However, if they are absolutely needed, then they should
be named with all capitals and underscores, and with a project-specific prefix.

```cpp
#define MYPROJECT_ROUND(x) ...
```

### Aliases

The name for an [alias](#aliases) follows the same principles as any other new
name, applied in the context where the alias is defined rather than where the
original name appears.

### Exceptions to Naming Rules

If you are naming something that is analogous to an existing C or C++ entity (or
a Rust entity via interop), then you can follow the existing naming convention
scheme.

`bigopen()`
:   function name, follows form of `open()`

`uint`
:   `typedef`

`bigpos`
:   `struct` or `class`, follows form of `pos`

`sparse_hash_map`
:   STL-like entity; follows STL naming conventions

`LONGLONG_MAX`
:   a constant, as in `INT_MAX`

## Comments

Comments are absolutely vital to keeping our code readable. The following rules
describe what you should comment and where. But remember: while comments are
very important, the best code is self-documenting. Giving sensible names to
types and variables is much better than using obscure names that you must then
explain through comments.

When writing your comments, write for your audience: the next
contributor
who will need to understand your code. Be generous — the next one may be you!

### Comment Style

Use either the `//` or `/* */` syntax, as long as you are consistent.

While either syntax is acceptable, `//` is *much* more common. Be consistent
with how you comment and what style you use where.

### File Comments

Start each file with license boilerplate.

If a source file (such as a `.h` file) declares multiple user-facing
abstractions (common functions, related classes, etc.), include a comment
describing the collection of those abstractions. Include enough detail for
future authors to know what does not fit there. However, the detailed
documentation about individual abstractions belongs with those abstractions, not
at the file level.

For instance, if you write a file comment for `frobber.h`, you do not need to
include a file comment in `frobber.cc` or `frobber_test.cc`. On the other hand,
if you write a collection of classes in `registered_objects.cc` that has no
associated header file, you must include a file comment in
`registered_objects.cc`.

#### Legal Notice and Author Line

Every file should contain license boilerplate. Choose the appropriate
boilerplate for the license used by the project (for example, Apache 2.0, BSD,
LGPL, GPL).

If you make significant changes to a file with an author line, consider deleting
the author line. New files should usually not contain copyright notice or author
line.

### Struct and Class Comments

Every non-obvious class or struct declaration should have an accompanying
comment that describes what it is for and how it should be used.

**Good example:**

```cpp
// Iterates over the contents of a GargantuanTable.
// Example:
//    std::unique_ptr<GargantuanTableIterator> iter = table->NewIterator();
//    for (iter->Seek("foo"); !iter->done(); iter->Next()) {
//      process(iter->key(), iter->value());
//    }
class GargantuanTableIterator {
  ...
};
```

#### Class Comments

The class comment should provide the reader with enough information to know how
and when to use the class, as well as any additional considerations necessary to
correctly use the class. Document the synchronization assumptions the class
makes, if any. If an instance of the class can be accessed by multiple threads,
take extra care to document the rules and invariants surrounding multithreaded
use.

The class comment is often a good place for a small example code snippet
demonstrating a simple and focused usage of the class.

When sufficiently separated (e.g., `.h` and `.cc` files), comments describing
the use of the class should go together with its interface definition; comments
about the class operation and implementation should accompany the implementation
of the class's methods.

### Function Comments

Declaration comments describe use of the function (when it is non-obvious);
comments at the definition of a function describe operation.

#### Function Declarations

Almost every function declaration should have comments immediately preceding it
that describe what the function does and how to use it. These comments may be
omitted only if the function is simple and obvious (e.g., simple accessors for
obvious properties of the class). Private methods and functions declared in
`.cc` files are not exempt. Function comments should be written with an implied
subject of *This function* and should start with the verb phrase; for example,
"Opens the file", rather than "Open the file". In general, these comments do not
describe how the function performs its task. Instead, that should be left to
comments in the function definition.

Types of things to mention in comments at the function declaration:

- What the inputs and outputs are. If function argument names are provided in
  `backticks`, then code-indexing tools may be able to present the
  documentation better.
- For class member functions: whether the object remembers reference or
  pointer arguments beyond the duration of the method call. This is quite
  common for pointer/reference arguments to constructors.
- For each pointer argument, whether the pointer may be null.
- For each nullable pointer argument, how a null input is handled.
- For each output or input/output argument, what happens to any state that
  argument is in (e.g., is the state appended to or overwritten?).
- If there are any performance implications of how a function is used.

Here is an example:

**Good example:**

```cpp
// Returns an iterator for this table, positioned at the first entry
// lexically greater than or equal to `start_word`. If there is no
// such entry, returns a null pointer. The client must not use the
// iterator after the underlying GargantuanTable has been destroyed.
//
// This method is equivalent to:
//    std::unique_ptr<Iterator> iter = table->NewIterator();
//    iter->Seek(start_word);
//    return iter;
std::unique_ptr<Iterator> GetIterator(absl::string_view start_word) const;
```

However, do not be unnecessarily verbose or state the completely obvious.

When documenting function overrides, focus on the specifics of the override
itself, rather than repeating the comment from the overridden function. In many
of these cases, the override needs no additional documentation and thus no
comment is required.

When commenting constructors and destructors, remember that the person reading
your code knows what constructors and destructors are for, so comments that just
say something like "destroys this object" are not useful. Document what
constructors do with their arguments (for example, if they take ownership of
pointers), and what cleanup the destructor does. If this is trivial, just skip
the comment. It is quite common for destructors not to have a header comment.

#### Function Definitions

If there is anything tricky about how a function does its job, the function
definition should have an explanatory comment. For example, in the definition
comment you might describe any coding tricks you use, give an overview of the
steps you go through, or explain why you chose to implement the function in the
way you did rather than using a viable alternative. For instance, you might
mention why it must acquire a lock for the first half of the function but why it
is not needed for the second half.

Note you should *not* just repeat the comments given with the function
declaration, in the `.h` file or wherever. It's okay to recapitulate briefly
what the function does, but the focus of the comments should be on how it does
it.

### Variable Comments

In general the actual name of the variable should be descriptive enough to give
a good idea of what the variable is used for. In certain cases, more comments
are required.

#### Class Data Members

The purpose of each class data member (also called an instance variable or
member variable) must be clear. If there are any invariants (special values,
relationships between members, lifetime requirements) not clearly expressed by
the type and name, they must be commented. However, if the type and name suffice
(`int num_events_;`), no comment is needed.

In particular, add comments to describe the existence and meaning of sentinel
values, such as nullptr or -1, when they are not obvious. For example:

**Good example:**

```cpp
private:
 // Used to bounds-check table accesses. -1 means
 // that we don't yet know how many entries the table has.
 int num_total_entries_;
```

#### Global Variables

All global variables should have a comment describing what they are, what they
are used for, and (if unclear) why they need to be global. For example:

**Good example:**

```cpp
// The total number of test cases that we run through in this regression test.
const int kNumTestCases = 6;
```

### Implementation Comments

In your implementation you should have comments in tricky, non-obvious,
interesting, or important parts of your code.

#### Explanatory Comments

Tricky or complicated code blocks should have comments before them.

#### Function Argument Comments

When the meaning of a function argument is nonobvious, consider one of the
following remedies:

- If the argument is a literal constant, and the same constant is used in
  multiple function calls in a way that tacitly assumes they're the same, you
  should use a named constant to make that constraint explicit, and to
  guarantee that it holds.
- Consider changing the function signature to replace a `bool` argument with
  an `enum` argument. This will make the argument values self-describing.
- For functions that have several configuration options, consider defining a
  single class or struct to hold all the options
  , and pass an
  instance of that. This approach has several advantages. Options are
  referenced by name at the call site, which clarifies their meaning. It also
  reduces function argument count, which makes function calls easier to read
  and write. As an added benefit, you don't have to change call sites when you
  add another option.
- Replace large or complex nested expressions with named variables.
- As a last resort, use comments to clarify argument meanings at the call
  site.

Consider the following example:

**Bad example:**

```cpp
// What are these arguments?
const DecimalNumber product = CalculateProduct(values, 7, false, nullptr);
```

versus:

**Good example:**

```cpp
ProductOptions options;
options.set_precision_decimals(7);
options.set_use_cache(ProductOptions::kDontUseCache);
const DecimalNumber product =
    CalculateProduct(values, options, /*completion_callback=*/nullptr);
```

#### Don'ts

Do not state the obvious. In particular, don't literally describe what code
does, unless the behavior is nonobvious to a reader who understands C++ well.
Instead, provide higher-level comments that describe *why* the code does what it
does, or make the code self-describing.

Compare this:

**Bad example:**

```cpp
// Find the element in the vector.  <-- Bad: obvious!
if (std::find(v.begin(), v.end(), element) != v.end()) {
  Process(element);
}
```

To this:

**Good example:**

```cpp
// Process "element" unless it was already processed.
if (std::find(v.begin(), v.end(), element) != v.end()) {
  Process(element);
}
```

Self-describing code doesn't need a comment. The comment from the example above
would be obvious:

**Good example:**

```cpp
if (!IsAlreadyProcessed(element)) {
  Process(element);
}
```

### Punctuation, Spelling, and Grammar

Pay attention to punctuation, spelling, and grammar; it is easier to read
well-written comments than badly written ones.

Comments should be as readable as narrative text, with proper capitalization and
punctuation. In many cases, complete sentences are more readable than sentence
fragments. Shorter comments, such as comments at the end of a line of code, can
sometimes be less formal, but you should be consistent with your style.

Although it can be frustrating to have a code reviewer point out that you are
using a comma when you should be using a semicolon, it is very important that
source code maintain a high level of clarity and readability. Proper
punctuation, spelling, and grammar help with that goal.

### TODO Comments

Use `TODO` comments for code that is temporary, a short-term solution, or
good-enough but not perfect.

`TODO`s should include the string `TODO` in all caps, followed by the bug ID,
name, e-mail address, or other identifier of the person or issue with the best
context about the problem referenced by the `TODO`. The main purpose is to have
a consistent `TODO` that can be searched to find out how to get more details
upon request. A `TODO` is not a commitment that the person referenced will fix
the problem. Thus when you create a `TODO` with a name, it is almost always your
name that is given.

**Good example:**

```cpp
// TODO(bug 12345): remove the "Last visitors" feature.
// TODO(example.com/my-design-doc): Manually fix up this code the next time it's touched.
// TODO(kl@gmail.com): Use a "*" here for concatenation operator.
// TODO(Zeke): change this to use relations.
```

If your `TODO` is of the form "At a future date do something" make sure that you
either include a very specific date ("Fix by November 2005") or a very specific
event ("Remove this code when all clients can handle XML responses.").

## Formatting

Coding style and formatting are pretty arbitrary, but a
project is much easier to follow if everyone uses
the same style. Individuals may not agree with every aspect of the formatting
rules, and some of the rules may take some getting used to, but it is important
that all
project contributors follow the style rules so
that they can all
read and understand everyone's code easily.

To help you format code correctly, we've created a
[settings file for emacs](https://raw.githubusercontent.com/google/styleguide/gh-pages/google-c-style.el).

### Line Length

Each line of text in your code should be at most 80 characters long.

We recognize that this rule is controversial, but so much existing code already
adheres to it, and we feel that consistency is important.

80 characters is the
maximum.

A line may exceed 80 characters if it is

- a comment line which is not feasible to split without harming readability,
  ease of cut and paste or auto-linking -- e.g., if a line contains an example
  command or a literal URL longer than 80 characters.
- a string literal that cannot easily be wrapped at 80 columns. This may be
  because it contains URIs or other semantically-critical pieces, or because
  the literal contains an embedded language, or because it is a multiline
  literal whose newlines are significant, such as help messages. In these
  cases, breaking up the literal would reduce readability, searchability,
  ability to click links, etc. Except for test code, such literals should
  appear at namespace scope near the top of a file. If a tool like
  Clang-Format doesn't recognize the unsplittable content,
  [disable the tool](https://clang.llvm.org/docs/ClangFormatStyleOptions.html#disabling-formatting-on-a-piece-of-code)
  around the content as necessary.

  (We must balance between usability/searchability of such literals and the
  readability of the code around them.)
- an include statement.
- a [header guard.](#the-define-guard)
- a using-declaration.

### Non-ASCII Characters

Non-ASCII characters should be rare, and must use UTF-8 formatting.

You shouldn't hard-code user-facing text in source, even English, so use of
non-ASCII characters should be rare. However, in certain cases it is appropriate
to include such words in your code. For example, if your code parses data files
from foreign sources, it may be appropriate to hard-code the non-ASCII string(s)
used in those data files as delimiters. More commonly, unit test code (which
does not need to be
localized) might contain non-ASCII strings. In such cases, you should use UTF-8,
since that is an encoding
understood by most tools able to handle more than just ASCII.

Hex encoding is also OK, and encouraged where it enhances readability — for
example, `"\xEF\xBB\xBF"`, or, even more simply, `"\uFEFF"`, is the Unicode
zero-width no-break space character, which would be invisible if included in the
source as straight UTF-8.

When possible, avoid the `u8` prefix. It has significantly different semantics
starting in C++20 than in C++17, producing arrays of `char8_t` rather than
`char`, and will change again in C++23.

You shouldn't use `char16_t` and `char32_t` character types, since they're for
non-UTF-8 text. For similar reasons you also shouldn't use `wchar_t` (unless
you're writing code that interacts with the Windows API, which uses `wchar_t`
extensively).

### Spaces vs. Tabs

Use only spaces, and indent 2 spaces at a time.

We use spaces for indentation. Do not use tabs in your code. You should set your
editor to emit spaces when you hit the tab key.

### Function Declarations and Definitions

Return type on the same line as function name, parameters on the same line if
they fit. Wrap parameter lists which do not fit on a single line as you would
wrap arguments in a [function call](#function-calls).

Functions look like this:

**Good example:**

```cpp
ReturnType ClassName::FunctionName(Type par_name1, Type par_name2) {
  DoSomething();
  ...
}
```

If you have too much text to fit on one line:

**Good example:**

```cpp
ReturnType ClassName::ReallyLongFunctionName(Type par_name1, Type par_name2,
                                             Type par_name3) {
  DoSomething();
  ...
}
```

or if you cannot fit even the first parameter:

**Good example:**

```cpp
ReturnType LongClassName::ReallyReallyReallyLongFunctionName(
    Type par_name1,  // 4 space indent
    Type par_name2,
    Type par_name3) {
  DoSomething();  // 2 space indent
  ...
}
```

Some points to note:

- Choose good parameter names.
- A parameter name may be omitted only if the parameter is not used in the
  function's definition.
- If you cannot fit the return type and the function name on a single line,
  break between them.
- If you break after the return type of a function declaration or definition,
  do not indent.
- The open parenthesis is always on the same line as the function name.
- There is never a space between the function name and the open parenthesis.
- There is never a space between the parentheses and the parameters.
- The open curly brace is always on the end of the last line of the function
  declaration, not the start of the next line.
- The close curly brace is either on the last line by itself or on the same
  line as the open curly brace.
- There should be a space between the close parenthesis and the open curly
  brace.
- All parameters should be aligned if possible.
- Default indentation is 2 spaces.
- Wrapped parameters have a 4 space indent.

Unused parameters that are obvious from context may omit the name:

**Good example:**

```cpp
class Foo {
 public:
  Foo(const Foo&) = delete;
  Foo& operator=(const Foo&) = delete;
};
```

Unused parameters that might not be obvious should comment out the variable name
in the function definition:

**Good example:**

```cpp
class Shape {
 public:
  virtual void Rotate(double radians) = 0;
};

class Circle : public Shape {
 public:
  void Rotate(double radians) override;
};

void Circle::Rotate(double /*radians*/) {}
```

**Bad example:**

```cpp
// Bad - if someone wants to implement later, it's not clear what the
// variable means.
void Circle::Rotate(double) {}
```

Attributes, and macros that expand to attributes, appear at the very beginning
of the function declaration or definition, before the return type:

**Good example:**

```cpp
  ABSL_ATTRIBUTE_NOINLINE void ExpensiveFunction();
  [[nodiscard]] bool IsOk();
```

### Lambda Expressions

Format parameters and bodies as for any other function, and capture lists like
other comma-separated lists.

For by-reference captures, do not leave a space between the ampersand (`&`) and
the variable name.

**Good example:**

```cpp
int x = 0;
auto x_plus_n = [&x](int n) -> int { return x + n; };
```

Short lambdas may be written inline as function arguments.

**Good example:**

```cpp
absl::flat_hash_set<int> to_remove = {7, 8, 9};
std::vector<int> digits = {3, 9, 1, 8, 4, 7, 1};
digits.erase(std::remove_if(digits.begin(), digits.end(), [&to_remove](int i) {
               return to_remove.contains(i);
             }),
             digits.end());
```

### Floating-point Literals

Floating-point literals should always have a radix point, with digits on both
sides, even if they use exponential notation. Readability is improved if all
floating-point literals take this familiar form, as this helps ensure that they
are not mistaken for integer literals, and that the `E`/`e` of the exponential
notation is not mistaken for a hexadecimal digit. It is fine to initialize a
floating-point variable with an integer literal (assuming the variable type can
exactly represent that integer), but note that a number in exponential notation
is never an integer literal.

**Bad example:**

```cpp
float f = 1.f;
long double ld = -.5L;
double d = 1248e6;
```

**Good example:**

```cpp
float f = 1.0f;
float f2 = 1.0;  // Also OK
float f3 = 1;    // Also OK
long double ld = -0.5L;
double d = 1248.0e6;
```

### Function Calls

Either write the call all on a single line, wrap the arguments at the
parenthesis, or start the arguments on a new line indented by four spaces and
continue at that 4 space indent. In the absence of other considerations, use the
minimum number of lines, including placing multiple arguments on each line where
appropriate.

Function calls have the following format:

**Good example:**

```cpp
bool result = DoSomething(argument1, argument2, argument3);
```

If the arguments do not all fit on one line, they should be broken up onto
multiple lines, with each subsequent line aligned with the first argument. Do
not add spaces after the open paren or before the close paren:

**Good example:**

```cpp
bool result = DoSomething(averyveryveryverylongargument1,
                          argument2, argument3);
```

Arguments may optionally all be placed on subsequent lines with a four space
indent:

**Good example:**

```cpp
if (...) {
  ...
  ...
  if (...) {
    bool result = DoSomething(
        argument1, argument2,  // 4 space indent
        argument3, argument4);
    ...
  }
```

Put multiple arguments on a single line to reduce the number of lines necessary
for calling a function unless there is a specific readability problem. Some find
that formatting with strictly one argument on each line is more readable and
simplifies editing of the arguments. However, we prioritize for the reader over
the ease of editing arguments, and most readability problems are better
addressed with the following techniques.

If having multiple arguments in a single line decreases readability due to the
complexity or confusing nature of the expressions that make up some arguments,
try creating variables that capture those arguments in a descriptive name:

**Good example:**

```cpp
int my_heuristic = scores[x] * y + bases[x];
bool result = DoSomething(my_heuristic, x, y, z);
```

Or put the confusing argument on its own line with an explanatory comment:

**Good example:**

```cpp
bool result = DoSomething(scores[x] * y + bases[x],  // Score heuristic.
                          x, y, z);
```

If there is still a case where one argument is significantly more readable on
its own line, then put it on its own line. The decision should be specific to
the argument which is made more readable rather than a general policy.

Sometimes arguments form a structure that is important for readability. In those
cases, feel free to format the arguments according to that structure:

**Good example:**

```cpp
// Transform the widget by a 3x3 matrix.
my_widget.Transform(x1, x2, x3,
                    y1, y2, y3,
                    z1, z2, z3);
```

### Braced Initializer List Format

Format a braced initializer list exactly like you would format a function call
in its place.

If the braced list follows a name (e.g., a type or variable name), format as if
the `{}` were the parentheses of a function call with that name. If there is no
name, assume a zero-length name.

**Good example:**

```cpp
// Examples of braced init list on a single line.
return {foo, bar};
functioncall({foo, bar});
std::pair<int, int> p{foo, bar};

// When you have to wrap.
SomeFunction(
    {"assume a zero-length name before {"},
    some_other_function_parameter);
SomeType variable{
    some, other, values,
    {"assume a zero-length name before {"},
    SomeOtherType{
        "Very long string requiring the surrounding breaks.",
        some, other, values},
    SomeOtherType{"Slightly shorter string",
                  some, other, values}};
SomeType variable{
    "This is too long to fit all in one line"};
MyType m = {  // Here, you could also break before {.
    superlongvariablename1,
    superlongvariablename2,
    {short, interior, list},
    {interiorwrappinglist,
     interiorwrappinglist2}};
```

### Looping and Branching Statements

At a high level, looping or branching statements consist of the following
**components**:

- One or more **statement keywords** (e.g., `if`, `else`, `switch`, `while`,
  `do`, or `for`).
- One **condition or iteration specifier**, inside parentheses.
- One or more **controlled statements**, or blocks of controlled statements.

For these statements:

- The components of the statement should be separated by single spaces (not
  line breaks).
- Inside the condition or iteration specifier, put one space (or a line break)
  between each semicolon and the next token, except if the token is a closing
  parenthesis or another semicolon.
- Inside the condition or iteration specifier, do not put a space after the
  opening parenthesis or before the closing parenthesis.
- Put any controlled statements inside blocks (i.e., use curly braces).
- Inside the controlled blocks, put one line break immediately after the
  opening brace, and one line break immediately before the closing brace.

**Good example:**

```cpp
if (condition) {                   // Good - no spaces inside parentheses, space before brace.
  DoOneThing();                    // Good - two-space indent.
  DoAnotherThing();
} else if (int a = f(); a != 3) {  // Good - closing brace on new line, else on same line.
  DoAThirdThing(a);
} else {
  DoNothing();
}

// Good - the same rules apply to loops.
while (condition) {
  RepeatAThing();
}

// Good - the same rules apply to loops.
do {
  RepeatAThing();
} while (condition);

// Good - the same rules apply to loops.
for (int i = 0; i < 10; ++i) {
  RepeatAThing();
}
```

**Bad example:**

```cpp
if(condition) {}                   // Bad - space missing after `if`.
else if ( condition ) {}           // Bad - space between the parentheses and the condition.
else if (condition){}              // Bad - space missing before `{`.
else if(condition){}               // Bad - multiple spaces missing.

for (int a = f();a == 10) {}       // Bad - space missing after the semicolon.

// Bad - `if ... else` statement does not have braces everywhere.
if (condition)
  foo;
else {
  bar;
}

// Bad - `if` statement too long to omit braces.
if (condition)
  // Comment
  DoSomething();

// Bad - `if` statement too long to omit braces.
if (condition1 &&
    condition2)
  DoSomething();
```

For historical reasons, we allow, but discourage, one exception to the above
rules: the curly braces for the controlled statement or the line breaks inside
the curly braces may be omitted if as a result the entire statement appears on
either a single line (in which case there is a space between the closing
parenthesis and the controlled statement) or on two lines (in which case there
is a line break after the closing parenthesis and there are no braces).

```cpp
// OK - each case fits on one line.
if (x == kFoo) { return new Foo(); }
while (TrySomething()) {}

// OK - braces are optional in this case.
if (x == kFoo) return new Foo();

// OK - condition fits on one line, body fits on another.
if (x == kBar)
  Bar(arg1, arg2, arg3);
```

This exception does not apply to multi-keyword statements like `if ... else` or
`do ... while`.

**Bad example:**

```cpp
// Bad - `if ... else` statement is missing braces.
if (x) DoThis();
else DoThat();

// Bad - `do ... while` statement is missing braces.
do DoThis();
while (x);
```

Use this style only when the statement is brief, and consider that loops and
branching statements with complex conditions or controlled statements may be
more readable with curly braces. Some
projects require curly braces always.

`case` blocks in `switch` statements can have curly braces or not, depending on
your preference. If you do include curly braces, they should be placed as shown
below.

**Good example:**

```cpp
switch (var) {
  case 0: {  // 2 space indent
    Foo();   // 4 space indent
    break;
  }
  default: {
    Bar();
  }
}
```

Empty loop bodies should use either an empty pair of braces or `continue` with
no braces, rather than a single semicolon. Empty braces are still subject to the
general line breaking rules, and also to the historic exception above.

**Good example:**

```cpp
while (condition) {  // Good - `{}` indicates no logic.
}                    // A linebreak is usually required.

while (condition) {}  // But OK if it fits on one line.

while (condition) {
  // Comments are okay, too
}

while (condition) continue;  // Good - `continue` indicates no logic.
```

**Bad example:**

```cpp
while (condition);  // Bad - looks like part of `do-while` loop.
```

### Pointer and Reference Expressions and Types

No spaces around period or arrow. Pointer operators do not have trailing spaces.

The following are examples of correctly-formatted pointer and reference
expressions:

**Good example:**

```cpp
x = *p;
p = &x;
x = r.y;
x = r->y;
```

Note that:

- There are no spaces around the period or arrow when accessing a member.
- Pointer operators have no space after the `*` or `&`.

When referring to a pointer or reference (variable declarations or definitions,
arguments, return types, template parameters, etc.), you must not place a space
before the asterisk/ampersand. Use a space to separate the type from the
declared name (if present).

**Good example:**

```cpp
// These are fine.
char* c;
const std::string& str;
int* GetPointer();
std::vector<char*>  // Note no space between '*' and '>'
```

It is allowed (if unusual) to declare multiple variables in the same
declaration, but it is disallowed if any of those have pointer or reference
decorations. Such declarations are easily misread.

```cpp
// Fine if helpful for readability.
int x, y;
```

**Bad example:**

```cpp
int x, *y;  // Disallowed - no & or * in multiple declaration
int *x, *y;  // Disallowed - no & or * in multiple declaration
int *x;  // Disallowed - & or * must be left of the space
char * c;  // Bad - spaces on both sides of *
const std::string & str;  // Bad - spaces on both sides of &
```

### Boolean Expressions

When you have a boolean expression that is longer than the
[standard line length](#line-length), be consistent in how you break up the
lines.

In this example, the logical AND operator is always at the end of the lines:

**Good example:**

```cpp
if (this_one_thing > this_other_thing &&
    a_third_thing == a_fourth_thing &&
    yet_another && last_one) {
  ...
}
```

Note that when the code wraps in this example, both of the `&&` logical AND
operators are at the end of the line. This is more common in Google code, though
wrapping all operators at the beginning of the line is also allowed. Feel free
to insert extra parentheses judiciously because they can be very helpful in
increasing readability when used appropriately, but be careful about overuse.
Also note that you should always use the punctuation operators, such as `&&` and
`~`, rather than the word operators, such as `and` and `compl`.

### Return Values

Do not needlessly surround the `return` expression with parentheses.

Use parentheses in `return expr;` only where you would use them in `x = expr;`.

**Good example:**

```cpp
return result;                  // No parentheses in the simple case.
// Parentheses OK to make a complex expression more readable.
return (some_long_condition &&
        another_condition);
```

**Bad example:**

```cpp
return (value);                // You wouldn't write var = (value);
return(result);                // return is not a function!
```

### Variable and Array Initialization

You may choose between `=`, `()`, and `{}`; the following are all correct:

**Good example:**

```cpp
int x = 3;
int x(3);
int x{3};
std::string name = "Some Name";
std::string name("Some Name");
std::string name{"Some Name"};
```

Be careful when using a braced initialization list `{...}` on a type with an
`std::initializer_list` constructor. A nonempty *braced-init-list* prefers the
`std::initializer_list` constructor whenever possible. Note that empty braces
`{}` are special, and will call a default constructor if available. To force the
non-`std::initializer_list` constructor, use parentheses instead of braces.

**Good example:**

```cpp
std::vector<int> v(100, 1);  // A vector containing 100 items: All 1s.
std::vector<int> v{100, 1};  // A vector containing 2 items: 100 and 1.
```

Also, the brace form prevents narrowing of integral types. This can prevent some
types of programming errors.

**Good example:**

```cpp
int pi(3.14);  // OK -- pi == 3.
int pi{3.14};  // Compile error: narrowing conversion.
```

### Preprocessor Directives

The hash mark that starts a preprocessor directive should always be at the
beginning of the line.

Even when preprocessor directives are within the body of indented code, the
directives should start at the beginning of the line.

**Good example:**

```cpp
// Good - directives at beginning of line
  if (lopsided_score) {
#if DISASTER_PENDING      // Correct -- Starts at beginning of line
    DropEverything();
# if NOTIFY               // OK but not required -- Spaces after #
    NotifyClient();
# endif
#endif
    BackToNormal();
  }
```

**Bad example:**

```cpp
// Bad - indented directives
  if (lopsided_score) {
    #if DISASTER_PENDING  // Wrong!  The "#if" should be at beginning of line
    DropEverything();
    #endif                // Wrong!  Do not indent "#endif"
    BackToNormal();
  }
```

### Class Format

Sections in `public`, `protected`, and `private` order, each indented one space.

The basic format for a class definition (lacking the comments, see
[Class Comments](#struct-and-class-comments) for a discussion of what comments are needed)
is:

**Good example:**

```cpp
class MyClass : public OtherClass {
 public:      // Note the 1 space indent!
  MyClass();  // Regular 2 space indent.
  explicit MyClass(int var);
  MyClass(const MyClass& other);
  MyClass& operator=(const MyClass& other);
  ~MyClass() {}

  void SomeFunction();
  void SomeFunctionThatDoesNothing() {}

  void set_some_var(int var) { some_var_ = var; }
  int some_var() const { return some_var_; }

 private:
  bool SomeInternalFunction();

  int some_var_;
  int some_other_var_;
};
```

Things to note:

- Any base class name should be on the same line as the subclass name, subject
  to the 80-column limit.
- The `public:`, `protected:`, and `private:` keywords should be indented one
  space.
- Except for the first instance, these keywords should be preceded by a blank
  line. This rule is optional in small classes.
- Do not leave a blank line after these keywords.
- The `public` section should be first, followed by the `protected` and
  finally the `private` section.
- See [Declaration Order](#declaration-order) for rules on ordering
  declarations within each of these sections.

### Constructor Initializer Lists

Constructor initializer lists can be all on one line or with subsequent lines
indented four spaces.

The acceptable formats for initializer lists are:

**Good example:**

```cpp
// When everything fits on one line:
MyClass::MyClass(int var) : some_var_(var) {
  DoSomething();
}

// If the signature and initializer list are not all on one line,
// you must wrap before the colon and indent 4 spaces:
MyClass::MyClass(int var)
    : some_var_(var), some_other_var_(var + 1) {
  DoSomething();
}

// When the list spans multiple lines, put each member on its own line
// and align them:
MyClass::MyClass(int var)
    : some_var_(var),             // 4 space indent
      some_other_var_(var + 1) {  // lined up
  DoSomething();
}

// As with any other code block, the close curly can be on the same
// line as the open curly, if it fits.
MyClass::MyClass(int var)
    : some_var_(var) {}
```

### Namespace Formatting

The contents of namespaces are not indented.

[Namespaces](#namespaces) do not add an extra level of indentation. For example,
use:

**Good example:**

```cpp
namespace {

void foo() {  // Correct.  No extra indentation within namespace.
  ...
}

}  // namespace
```

Do not indent within a namespace:

**Bad example:**

```cpp
namespace {

  // Wrong!  Indented when it should not be.
  void foo() {
    ...
  }

}  // namespace
```

### Horizontal Whitespace

Use of horizontal whitespace depends on location. Never put trailing whitespace
at the end of a line.

#### General

**Good example:**

```cpp
int i = 0;  // Two spaces before end-of-line comments.

void f(bool b) {  // Open braces should always have a space before them.
  ...
int i = 0;  // Semicolons usually have no space before them.
// Spaces inside braces for braced-init-list are optional.  If you use them,
// put them on both sides!
int x[] = { 0 };
int x[] = {0};

// Spaces around the colon in inheritance and initializer lists.
class Foo : public Bar {
 public:
  // For inline function implementations, put spaces between the braces
  // and the implementation itself.
  Foo(int b) : Bar(), baz_(b) {}  // No spaces inside empty braces.
  void Reset() { baz_ = 0; }  // Spaces separating braces from implementation.
  ...
```

Adding trailing whitespace can cause extra work for others editing the same file
when they merge, as can removing existing trailing whitespace. So, don't
introduce trailing whitespace. Remove it if you're already changing that line,
or do it in a separate clean-up
operation (preferably when no one else is working on the
file).

#### Loops and Conditionals

**Good example:**

```cpp
if (b) {          // Space after the keyword in conditions and loops.
} else {          // Spaces around else.
}
while (test) {}   // There is usually no space inside parentheses.
switch (i) {
for (int i = 0; i < 5; ++i) {
// Loops and conditions may have spaces inside parentheses, but this
// is rare.  Be consistent.
switch ( i ) {
if ( test ) {
for ( int i = 0; i < 5; ++i ) {
// For loops always have a space after the semicolon.  They may have a space
// before the semicolon, but this is rare.
for ( ; i < 5 ; ++i) {
  ...

// Range-based for loops always have a space before and after the colon.
for (auto x : counts) {
  ...
}
switch (i) {
  case 1:         // No space before colon in a switch case.
    ...
  case 2: break;  // Use a space after a colon if there's code after it.
```

#### Operators

**Good example:**

```cpp
// Assignment operators always have spaces around them.
x = 0;

// Other binary operators usually have spaces around them, but it's
// OK to remove spaces around factors.  Parentheses should have no
// internal padding.
v = w * x + y / z;
v = w*x + y/z;
v = w * (x + z);

// No spaces separating unary operators and their arguments.
x = -5;
++x;
if (x && !y)
  ...
```

#### Templates and Casts

**Good example:**

```cpp
// No spaces inside the angle brackets (< and >), before
// <, or between >( in a cast
std::vector<std::string> x;
y = static_cast<char*>(x);

// No spaces between type and pointer.
std::vector<char*> x;
```

### Vertical Whitespace

Use vertical whitespace sparingly; unnecessary blank lines make it harder to see
overall code structure. Use blank lines only where they aid the reader in
understanding the structure.

Do not add blank lines where indentation already provides clear delineation,
such as at the start or end of a code block. Do use blank lines to separate code
into closely related chunks, analogous to paragraph breaks in prose. Within a
statement or declaration, usually only insert line breaks to stay within the
[line length limit](#line-length), or to attach a comment to only part of the
contents.

## Exceptions to the Rules

The coding conventions described above are mandatory. However, like all good
rules, these sometimes have exceptions, which we discuss here.

### Existing Non-conformant Code

You may diverge from the rules when dealing with code that does not conform to
this style guide.

If you find yourself modifying code that was written to specifications other
than those presented by this guide, you may have to diverge from these rules in
order to stay consistent with the local conventions in that code. If you are in
doubt about how to do this, ask the original author or the person currently
responsible for the code. Remember that *consistency* includes local
consistency, too.

### Windows Code

Windows programmers have developed
their own set of coding conventions, mainly derived from the conventions in
Windows headers and other Microsoft code. We want to make it easy for anyone to
understand your code, so we have a single set of guidelines for everyone writing
C++ on any platform.

It is worth reiterating a few of the guidelines that you might forget if you are
used to the prevalent Windows style:

- Do not use Hungarian notation (for example, naming an integer `iNum`). Use
  the Google naming conventions, including the `.cc` extension for source
  files.
- Windows defines many of its own synonyms for primitive types, such as
  `DWORD`, `HANDLE`, etc. It is perfectly acceptable, and encouraged, that you
  use these types when calling Windows API functions. Even so, keep as close
  as you can to the underlying C++ types. For example, use `const TCHAR*`
  instead of `LPCTSTR`.
- When compiling with Microsoft Visual C++, set the compiler to warning level
  3 or higher, and treat all warnings as errors.
- Do not use `#pragma once`; instead use the standard Google include guards.
  The path in the include guards should be relative to the top of your project
  tree.
- In fact, do not use any nonstandard extensions, like `#pragma` and
  `__declspec`, unless you absolutely must. Using `__declspec(dllimport)` and
  `__declspec(dllexport)` is allowed; however, you must use them through
  macros such as `DLLIMPORT` and `DLLEXPORT`, so that someone can easily
  disable the extensions if they share the code.

However, there are just a few rules that we occasionally need to break on
Windows:

- Normally we
  [strongly discourage the use of multiple implementation inheritance](#inheritance);
  however, it is required when using COM and some ATL/WTL classes. You may use
  multiple implementation inheritance to implement COM or ATL/WTL classes and
  interfaces.
- Although you should not use exceptions in your own code, they are used
  extensively in the ATL and some STLs, including the one that comes with
  Visual C++. When using the ATL, you should define `_ATL_NO_EXCEPTIONS` to
  disable exceptions. You should investigate whether you can also disable
  exceptions in your STL, but if not, it is OK to turn on exceptions in the
  compiler. (Note that this is only to get the STL to compile. You should
  still not write exception handling code yourself.)
- The usual way of working with precompiled headers is to include a header
  file at the top of each source file, typically with a name like `StdAfx.h`
  or `precompile.h`. To make your code easier to share with other projects,
  avoid including this file explicitly (except in `precompile.cc`), and use
  the `/FI` compiler option to include the file automatically.
- Resource headers, which are usually named `resource.h` and contain only
  macros, do not need to conform to these style guidelines.
