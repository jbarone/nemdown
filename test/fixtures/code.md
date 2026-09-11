---
title: Syntax
---

# Code

```c
#include <stdio.h>
/* entry point */
int main(int argc, char **argv) {
  const char *msg = "hello\n";
  size_t n = 42;
  if (argc > 1 && NULL != argv) { printf(msg, n); }
  return 0;
}
```

```python
import os

@decorator
def greet(name: str) -> bool:
    """Docstring."""
    items = [1, 2.5, None, True]
    return os.path.exists(name)  # trailing comment
```

```bash
# deploy
set -euo pipefail
for f in "$@"; do
  echo "${f}" | grep -q 'x' && exit 1
done
```

```json
{ "name": "nemdown", "version": 1.0, "tags": ["c", "wayland"], "ok": true }
```
