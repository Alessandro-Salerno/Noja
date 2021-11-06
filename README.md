# The Noja language

## Introduction

  This language was written as a personal study of how interpreters
and compilers work. For this reason, the language is very basic.
  One of the main inspirations was the CPython's source code since
it's extremely readable and has a very simple and clean architecture.

  This file was intended for people who already program in other 
high level languages (such as Python, Javascript, Ruby) and don't 
need to be introduced to basic programming concepts (variables, 
expressions and branches). This way, there is more space for the 
comparison of the language's features with the mainstream languages.

## Implementation overview

  The interpreter works by compiling the provided source to a bytecode 
format and executing it. The bytecode is very high level since it
does things like:

  - explicitly referring to variables by name.

  - treating values as atomic things: from the perspective of the 
    bytecode, a list and an integer occupy the same space on the 
    stack, which is 1.

  - referring to instructions by their index.

For example, by compiling the following snippet

```py
define = true;

if define:
        a = 33;

print(a, '\n');
```

one would obtain the following bytecode:

```
	 0: PUSHTRU 
	 1: ASS     "define" 
	 2: POP     1 
	 3: PUSHVAR "define" 
	 4: JUMPIFNOTANDPOP 8 
	 5: PUSHINT 33 
	 6: ASS     "a" 
	 7: POP     1 
	 8: PUSHSTR "\n"
	 9: PUSHVAR "a" 
	10: PUSHVAR "print" 
	11: CALL    2 
	12: POP     1 
	13: RETURN

```

as you can see, there are instructions like `ASS` and `PUSHVAR` that
assign to and read from variables by specifying names, and jumps
that refer to other points of the "executable" by specifying indices
(like `JUMPIFNOTANDPOP`) instead of raw addresses.

  All values (objects) are allocated on a garbage-collected heap. 
For this reason all variables are simply references to these objects.
The garbage collection algorithm is a copy-and-compact one. It 
behaves as a bump-pointer allocator until there is space left, 
and when space runs out, it creates a new heap, copies all of the 
alive object into it, calls the destructors of the dead objects 
and frees the old one.