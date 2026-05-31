# Riven Language 🔥

> A beginner-friendly, system-level programming language with clean syntax, OOP, async support, and automatic memory management.

```
  ____  _                  _
 |  _ \(_)_   _____ _ __  | |    __ _ _ __  __ _
 | |_) | \ \ / / _ \ '_ \ | |   / _` | '_ \/ _` |
 |  _ <| |\ V /  __/ | | || |__| (_| | | | | (_| |
 |_| \_\_| \_/ \___|_| |_||_____\__,_|_| |_|\__, |
                                              |___/
```

**Version:** 1.1.0  
**Language:** C (interpreter written in pure C)  
**Author:** Ansh  

---

## File Structure

```
riven/
├── include/
│   └── riven.h          # All types, structs, enums, prototypes
├── src/
│   ├── lexer.c          # Tokenizer — converts source to tokens
│   ├── parser.c         # Recursive descent parser — builds AST
│   ├── value.c          # Runtime value system + environment + ref/ptr
│   ├── stdlib.c         # Built-in native functions
│   ├── interpreter.c    # Tree-walk executor — runs the AST
│   └── main.c           # Entry point, REPL, CLI
├── examples/
│   ├── hello.rv         # Hello World
│   ├── demo.rv          # Full language demo
│   └── system.rvh       # Standard system header library
├── Makefile
└── README.md
```

---

## Build

```bash
# Requirements: gcc, make, pthreads (standard on Linux/macOS)
make

# Run a file
./rvn run examples/hello.rv

# Start interactive REPL
./rvn

# Debug: dump tokens
./rvn tokens myfile.rv

# Debug: dump AST
./rvn ast myfile.rv
```

---

## Language Quick Reference

### Entry Point
```riven
riven core {
    stamp("Hello Riven")
}
```

### Variables & Constants
```riven
name = "Ansh"
age  = 18
firm PI = 3.14159
```

### Data Types
| Type | Meaning |
|------|---------|
| `int` | Integer |
| `dnum` | Decimal |
| `txt` | String |
| `correct` | True |
| `incorrect` | False |
| `coll` | Array/List |
| `rec` | Record/Object |
| `emp` | Null |

### Output & Input
```riven
stamp("Hello {}", name)
name = grab("Enter your name: ")
```

### Functions
```riven
craft add(a, b) {
    returns a + b
}
```

### Conditions
```riven
if age > 18 {
    stamp("Adult")
} altif age == 18 {
    stamp("Just 18")
} else {
    stamp("Minor")
}
```

### Loops
```riven
flow 10 { }              ~~ fixed count

during x < 100 {         ~~ while loop
    x+>
}
```

### Collections
```riven
nums = [1, 2, 3]
push(nums, 4)
stamp(nums[0])
sort(nums)
```

### OOP — Frames
```riven
frame User {
    hidden password = "secret"
    open  name      = "Guest"

    boot(n) {
        self.name = n
    }

    open craft greet() {
        stamp("Hello {}", self.name)
    }

    hidden craft audit() {
        returns self.password
    }
}

user = User("Ansh")       ~~ or: spawn User("Ansh")
user.greet()
```

### References & Pointers
```riven
x = 10
ref r = x       ~~ true alias — writing r writes x
r = 99
stamp(x)        ~~ prints 99

raw {
    ptr p = 0   ~~ raw address pointer (unsafe mode only)
}
```

### Async (Spark)
```riven
spark craft worker(id) {
    stamp("Task {} running", id)
}

handle = worker(1)    ~~ launches background thread
stamp("Main continues")
sync                  ~~ wait for all spark tasks
```

### Error Handling
```riven
resc {
    attack("Something went wrong")
}
```

### Import
```riven
consistof "system.rvh"
```

### Comments
```riven
~~ Single line comment

<<
  Multi-line
  comment
>>
```

---

## Implemented Features

| Feature | Status |
|---------|--------|
| Variables, constants (`firm`) | ✅ Full |
| All data types | ✅ Full |
| `if` / `altif` / `else` | ✅ Full |
| `flow` / `during` loops | ✅ Full |
| `craft` functions + closures | ✅ Full |
| `returns` | ✅ Full |
| `frame` OOP | ✅ Full |
| `boot` with parameters | ✅ Full |
| `spawn` / `User()` | ✅ Full |
| `open` / `hidden` access control | ✅ Full (enforced at runtime) |
| `ref` true aliasing | ✅ Full (shared heap box) |
| `ptr` raw pointers | ✅ Full (safe + unsafe mode) |
| `raw` unsafe block | ✅ Full (mode switch) |
| `spark` async threads | ✅ Full (pthreads) |
| `sync` barrier | ✅ Full (joins all threads) |
| `attack` / `resc` error handling | ✅ Full |
| `consistof` imports | ✅ Full |
| Collections | ✅ Full |
| Records | ✅ Full |
| Type casting | ✅ Full |
| `fetch` HTTP | ✅ Full (via curl) |
| `grab` input | ✅ Full |
| `stamp` formatted output | ✅ Full |
| REPL | ✅ Full (multi-line aware) |
| Token/AST debug dump | ✅ Full |
